// Standalone host: loads the game's modules into a native address space and runs the
// recompiled code on host threads, with a native kernel and native services.
#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <signal.h>
#include <sys/prctl.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <vector>

// The performance counter the timing below reads, on Linux the monotonic clock in nanoseconds.
union LARGE_INTEGER {
    long long QuadPart;
};
static void QueryPerformanceCounter(LARGE_INTEGER* t) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = static_cast<long long>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}
static void QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1'000'000'000; }
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <thread>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "aot_modules.h"
#include "os.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "memory.h"
#include "services/gamepads.h"
#include "video.h"
#include "watch.h"

namespace fs = std::filesystem;

// The game's tick length (s) and rate, as float bits (tools/patches.py mkw_tick_time): its own 1/60 and 1.0 unless HOST_TICK_HZ.
uint32_t g_mkw_tick_dt = 0x3c888889u, g_mkw_tick_rate = 0x3f800000u;
// The game's counters that count ticks (tools/patches.py mkw_tick_counters, mkw_frame_counters): each asks once, the
// first time it runs, where its step is. Named sites (HOST_TICK_PCS=<hex pc>,..., HOST_TICK_FUNCS=<hex function>,...;
// main offsets, so they belong to one game version; HOST_TICK_FLOATS=all for every float one) step by the tick rate
// instead of 1: floats by the rate itself, integers by 0 or 1 a tick (g_mkw_tick_whole), so they count 60ths of a second.
float g_mkw_tick_ratef = 1.f;  // the tick rate (60 / HOST_TICK_HZ)
float g_mkw_tick_secf = 1.f;   // what tools/patches.py mkw_tick_seconds scales the tick count by: the rate, or 1 (HOST_TICK_STEP)
bool g_mkw_tick_step_whole = false;  // HOST_TICK_STEP=whole: the tick time's count is the whole 60ths the tick completes
namespace {
const uint32_t k_tick_one = 1;
const float k_tick_onef = 1.f;
uint32_t g_mkw_tick_whole = 1;
double g_mkw_tick_acc = 0;
struct TickSites {
    std::vector<uint32_t> pcs, funcs, add_funcs;
    bool all_floats = false;
    TickSites() {
        auto list = [](const char* name, std::vector<uint32_t>& into) {
            const char* v = std::getenv(name);
            for (const char* p = v; p && *p;) {
                char* end = nullptr;
                const unsigned long x = std::strtoul(p, &end, 16);
                if (end == p) break;
                into.push_back(static_cast<uint32_t>(x));
                p = *end ? end + 1 : end;
            }
            std::sort(into.begin(), into.end());
        };
        list("HOST_TICK_PCS", pcs), list("HOST_TICK_FUNCS", funcs), list("HOST_TICK_ADD_FUNCS", add_funcs);
        const char* f = std::getenv("HOST_TICK_FLOATS");
        all_floats = f && !std::strcmp(f, "all");
    }
    bool named(uint32_t pc, uint32_t fn) const {
        return std::binary_search(pcs.begin(), pcs.end(), pc) || std::binary_search(funcs.begin(), funcs.end(), fn);
    }
};
const TickSites& tick_sites() {
    static const TickSites s;
    return s;
}
int g_job_log = 0;  // HOST_TICK_JOB_LOG: 1 = job lists and modules, 2 = also where each counter first runs
}  // namespace
// The job the thread is running (mkw_job_obj / mkw_job_done): the object's vtable and its module's index (-1: none).
thread_local u64 t_job_vt = 0;
thread_local int t_job_module = -2;  // -2: not in a job
thread_local u64 t_job_actor = 0;     // an actor job's actor
static void log_actor_strings(u64 a);
static void log_site(const char* kind, uint32_t pc, uint32_t fn) {
    if (g_job_log < 2) return;
    if (t_job_module != -2 && t_job_actor) log_actor_strings(t_job_actor);
    if (t_job_module == -2) LOG("site: %s main+%#x (function %#x) outside jobs", kind, pc, fn);
    else LOG("site: %s main+%#x (function %#x) in job vt main+%#llx%s module %d actor %#llx vt %#llx", kind, pc, fn,
             static_cast<unsigned long long>(t_job_vt & ~(1ull << 63)), t_job_vt >> 63 ? " (untagged)" : "", t_job_module,
             static_cast<unsigned long long>(t_job_actor), static_cast<unsigned long long>(t_job_actor ? ld<u64>(t_job_actor) : 0));
}

const uint32_t* mkw_tick_slot(uint32_t pc, uint32_t fn) {
    log_site("int", pc, fn);
    if (!tick_sites().named(pc, fn)) return &k_tick_one;
    LOG("tick: counter main+%#x (function %#x) counts 60ths of a second", pc, fn);
    return &g_mkw_tick_whole;
}
// A float accumulator x += y (tools/patches.py mkw_tick_adds): y is multiplied by this (HOST_TICK_PCS, HOST_TICK_ADD_FUNCS).
const float* mkw_tick_aslot(uint32_t pc, uint32_t fn) {
    log_site("add", pc, fn);
    const TickSites& t = tick_sites();
    if (!std::binary_search(t.pcs.begin(), t.pcs.end(), pc) && !std::binary_search(t.add_funcs.begin(), t.add_funcs.end(), fn))
        return &k_tick_onef;
    LOG("tick: accumulator main+%#x (function %#x) adds by the rate", pc, fn);
    return &g_mkw_tick_ratef;
}
const float* mkw_tick_fslot(uint32_t pc, uint32_t fn) {
    log_site("float", pc, fn);
    if (!tick_sites().all_floats && !tick_sites().named(pc, fn)) return &k_tick_onef;
    LOG("tick: float counter main+%#x (function %#x) steps by the rate", pc, fn);
    return &g_mkw_tick_ratef;
}
// The frame's first phase (tools/patches.py mkw_tick_time): one tick begins, so the integer step is how many whole 60ths
// of a second it completes (0 or 1 at 120 Hz).
// The tick time's count (tools/patches.py mkw_tick_time; the game's: 1 a tick): with HOST_TICK_STEP=whole, how many whole
// 60ths of a second this tick completes (0 or 1 at 120 Hz), so what counts by it counts 60ths while dt stays the tick's.
uint32_t mkw_tick_step(uint32_t count) { return g_mkw_tick_step_whole ? g_mkw_tick_whole : count; }
void half_tick();  // (below: whether the half rate is on this tick)
void mkw_tick_begin() {
    half_tick();
    g_mkw_tick_acc += g_mkw_tick_ratef;
    const double whole = std::floor(g_mkw_tick_acc);
    g_mkw_tick_acc -= whole;
    g_mkw_tick_whole = static_cast<uint32_t>(whole);
}

// ------------------------------------------------------------------ logging

static std::mutex g_log_lock;
static FILE* g_log_file = nullptr;
static LARGE_INTEGER g_qpc_start, g_qpf;

static void vlog(const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - g_qpc_start.QuadPart) / static_cast<double>(g_qpf.QuadPart);
    const u64 tid = kern::current_thread() ? kern::current_thread()->id : 0;
    std::scoped_lock lk{g_log_lock};
    fprintf(stderr, "[%9.4f t%llu] %s\n", t, tid, buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%9.4f t%llu] %s\n", t, tid, buf);
        fflush(g_log_file);
    }
}

void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// One line per event: microseconds since start, guest thread (0 = host thread), text.
static FILE* g_timeline_file = [] {
    const char* path = std::getenv("HOST_TIMELINE");
    FILE* f = path ? std::fopen(path, "w") : nullptr;
    if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    return f;
}();
bool g_timeline_on = g_timeline_file != nullptr;
static std::mutex g_timeline_lock;
static u64 g_timeline_lines = 0;

void timeline(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const u64 us = static_cast<u64>(static_cast<double>(now.QuadPart - g_qpc_start.QuadPart) * 1e6 /
                                    static_cast<double>(g_qpf.QuadPart));
    // HOST_TIMELINE_FROM_S=<s>: nothing is recorded before that time.
    static const u64 from_us = [] {
        const char* v = std::getenv("HOST_TIMELINE_FROM_S");
        return v ? std::strtoull(v, nullptr, 10) * 1000000 : 0ull;
    }();
    if (us < from_us) return;
    const u64 tid = kern::current_thread() ? kern::current_thread()->id : 0;
    std::scoped_lock lk{g_timeline_lock};
    std::fprintf(g_timeline_file, "%llu %llu %s\n", us, tid, buf);
    if (++g_timeline_lines % 4096 == 0) std::fflush(g_timeline_file);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
    if (auto* t = kern::current_thread()) dump_guest_stack(&t->ctx);
    gamepads::stop_vibration();
    // Started from Explorer the console closes with the process: say why (not in hidden test runs).
    if (!std::getenv("HOST_RENDER_HIDDEN")) {
        char text[1024];
        va_start(ap, fmt);
        vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
#ifdef _WIN32
        MessageBoxA(nullptr, text, "Kart: fatal error", MB_OK | MB_ICONERROR);
#else
        video::error_box("Kart: fatal error", text);
#endif
    }
#ifdef _WIN32
    ExitProcess(1);
#else
    if (g_log_file) fflush(g_log_file);
    _exit(1);
#endif
}

// ------------------------------------------------------------------ time

u64 host_ns() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<u64>(static_cast<double>(now.QuadPart) * 1e9 / static_cast<double>(g_qpf.QuadPart));
}

u64 guest_ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 hi;
    const u64 lo = _umul128(static_cast<u64>(now.QuadPart - g_qpc_start.QuadPart), GuestTickHz, &hi);
    u64 rem;
    return _udiv128(hi, lo, static_cast<u64>(g_qpf.QuadPart), &rem);
}

uint64_t host_cntvct() { return guest_ticks(); }
uint64_t host_cntfrq() { return GuestTickHz; }

// ------------------------------------------------------------------ modules

namespace {

struct LoadedModule {
    std::string name;
    u64 base, size;
    const FnEntry* table;
    size_t table_size;
};
std::vector<LoadedModule> g_loaded;
std::unordered_map<u64, GuestFn> g_overrides;

// LZ4 block decompression (NSO segments).
bool lz4_decompress(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    size_t s = 0, d = 0;
    while (s < src_size) {
        const u8 token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do { b = src[s++]; lit += b; } while (b == 255);
        }
        if (d + lit > dst_size || s + lit > src_size) return false;
        std::memcpy(dst + d, src + s, lit);
        s += lit;
        d += lit;
        if (s >= src_size) break;
        const size_t offset = src[s] | src[s + 1] << 8;
        s += 2;
        size_t len = token & 15;
        if (len == 15) {
            u8 b;
            do { b = src[s++]; len += b; } while (b == 255);
        }
        len += 4;
        if (offset == 0 || offset > d || d + len > dst_size) return false;
        for (size_t k = 0; k < len; k++, d++) dst[d] = dst[d - offset];
    }
    return d == dst_size;
}

std::string hex(const u8* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

// Loads one NSO at `base`; returns its image size.
u64 load_nso(const fs::path& path, const AotModuleDesc& desc, u64 base) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fatal("cannot open %s", path.string().c_str());
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 0x100 || std::memcmp(file.data(), "NSO0", 4) != 0) fatal("%s is not an NSO", path.string().c_str());
    auto u32_at = [&](size_t o) { u32 v; std::memcpy(&v, file.data() + o, 4); return v; };
    const u32 flags = u32_at(0x0C);
    struct Seg { u32 file_off, mem_off, size, comp_size; };
    Seg segs[3];
    for (int i = 0; i < 3; i++) {
        segs[i] = {u32_at(0x10 + i * 0x10), u32_at(0x14 + i * 0x10), u32_at(0x18 + i * 0x10), u32_at(0x60 + i * 4)};
    }
    const u32 bss = u32_at(0x3C);
    const std::string id = hex(file.data() + 0x40, 0x20);
    if (id != desc.module_id) fatal("%s: build id %s does not match the recompiled %s", desc.name, id.c_str(), desc.module_id);
    const u64 image = (segs[2].mem_off + segs[2].size + bss + 0xFFF) & ~0xFFFull;
    if (image != desc.image_size) fatal("%s: image size %#llx, recompiled for %#llx", desc.name, image, desc.image_size);

    mem::map(base, image, mem::CodeData, mem::RW);
    for (int i = 0; i < 3; i++) {
        u8* dst = gptr(base + segs[i].mem_off);
        if (flags & (1u << i)) {
            if (!lz4_decompress(file.data() + segs[i].file_off, segs[i].comp_size, dst, segs[i].size))
                fatal("%s: segment %d does not decompress", desc.name, i);
        } else {
            std::memcpy(dst, file.data() + segs[i].file_off, segs[i].size);
        }
    }
    // How the kernel reports a module: text r-x and rodata r-- as Code, data and bss rw- as CodeData.
    mem::set_state(base, segs[1].mem_off, mem::Code, mem::RX);
    mem::set_state(base + segs[1].mem_off, segs[2].mem_off - segs[1].mem_off, mem::Code, mem::R);
    return image;
}

GuestFn find_guest_fn(u64 target) {
    auto ov = g_overrides.find(target);
    if (ov != g_overrides.end()) return ov->second;
    for (const auto& m : g_loaded) {
        if (target - m.base < m.size) return lookup_fn(m.table, m.table_size, target - m.base);
    }
    return nullptr;
}

int walk_frames(u64 fp, u64* out, int max) {
    int n = 0;
#ifndef _WIN32
    // (no fault handling around the reads here: each frame record is checked to be mapped first)
    while (fp && !(fp & 7) && n < max && mem::backing_ptr(fp) && mem::backing_ptr(fp + 8)) {
        const u64* frame = static_cast<const u64*>(hp(fp));
        out[n++] = frame[1];
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    return n;
#else
    __try {
        while (fp && !(fp & 7) && n < max) {
            const u64* frame = static_cast<const u64*>(hp(fp));
            out[n++] = frame[1];
            if (frame[0] <= fp) break;
            fp = frame[0];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
#endif
}

}  // namespace

#ifndef _WIN32
// ------------------------------------------------------------------ Linux: symbols, stacks, crashes

namespace {

// kart's own functions, from the symbol table of its file (linked without PIE: the addresses are where they run).
struct ElfSymbols {
    std::vector<std::pair<u64, u64>> funcs;   // address, offset of the name in `names`; sorted
    std::vector<u64> sizes;
    std::string names;
    u64 text_lo = ~0ull, text_hi = 0;

    ElfSymbols() {
        std::ifstream f("/proc/self/exe", std::ios::binary);
        std::string file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (file.size() < sizeof(Elf64_Ehdr) || std::memcmp(file.data(), ELFMAG, SELFMAG) != 0) return;
        Elf64_Ehdr eh;
        std::memcpy(&eh, file.data(), sizeof eh);
        if (eh.e_shoff + u64(eh.e_shnum) * sizeof(Elf64_Shdr) > file.size()) return;
        std::vector<Elf64_Shdr> sh(eh.e_shnum);
        std::memcpy(sh.data(), file.data() + eh.e_shoff, sh.size() * sizeof(Elf64_Shdr));
        for (const Elf64_Shdr& s : sh) {
            if (s.sh_type != SHT_SYMTAB || s.sh_link >= sh.size()) continue;
            const Elf64_Shdr& str = sh[s.sh_link];
            if (s.sh_offset + s.sh_size > file.size() || str.sh_offset + str.sh_size > file.size()) return;
            names.assign(file.data() + str.sh_offset, str.sh_size);
            std::vector<std::pair<u64, std::pair<u64, u64>>> all;   // address, (name, size)
            for (u64 o = 0; o + sizeof(Elf64_Sym) <= s.sh_size; o += sizeof(Elf64_Sym)) {
                Elf64_Sym sym;
                std::memcpy(&sym, file.data() + s.sh_offset + o, sizeof sym);
                if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC || !sym.st_value || sym.st_name >= names.size()) continue;
                all.push_back({sym.st_value, {sym.st_name, sym.st_size}});
                text_lo = std::min<u64>(text_lo, sym.st_value);
                text_hi = std::max<u64>(text_hi, sym.st_value + sym.st_size);
            }
            std::sort(all.begin(), all.end());
            funcs.reserve(all.size());
            sizes.reserve(all.size());
            for (const auto& [a, ns] : all) funcs.push_back({a, ns.first}), sizes.push_back(ns.second);
        }
    }

    // "name+0x12" of the function holding `a`, or "" outside them.
    std::string find(u64 a) const {
        // (outside kart's code: a size-0 symbol, as the last one, would otherwise take every library address above it)
        if (a < text_lo || a >= text_hi) return {};
        auto it = std::upper_bound(funcs.begin(), funcs.end(), std::pair<u64, u64>{a, ~0ull});
        if (it == funcs.begin()) return {};
        --it;
        const u64 size = sizes[it - funcs.begin()];
        if (size && a >= it->first + size) return {};
        const char* mangled = names.c_str() + it->second;
        int status = 0;
        char* plain = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
        char off[24];
        std::snprintf(off, sizeof off, "+%#llx", static_cast<unsigned long long>(a - it->first));
        std::string r = std::string(status == 0 && plain ? plain : mangled) + off;
        std::free(plain);
        return r;
    }
};

const ElfSymbols& elf_symbols() {
    static const ElfSymbols s;
    return s;
}

}  // namespace

std::string host_symbol(u64 rip) {
    static std::mutex m;
    std::scoped_lock lk{m};
    std::string own = elf_symbols().find(rip);
    if (!own.empty()) return own;
    // Outside kart (the Vulkan driver, system libraries): the library and offset, and its nearest exported symbol
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(rip), &info) || !info.dli_fname) return "?";
    const char* name = std::strrchr(info.dli_fname, '/');
    char b[400];
    std::snprintf(b, sizeof b, "%s+%#llx", name ? name + 1 : info.dli_fname,
                  static_cast<unsigned long long>(rip - reinterpret_cast<u64>(info.dli_fbase)));
    std::string r = b;
    if (info.dli_sname) {
        std::snprintf(b, sizeof b, " (%s+%#llx)", info.dli_sname, static_cast<unsigned long long>(rip - reinterpret_cast<u64>(info.dli_saddr)));
        r += b;
    }
    return r;
}

// Every thread of the process with its host call stack, for a stall: where each one runs or waits. A recompiled
// function carries its guest address in its name, so a guest thread's stack is also its guest call chain. Each thread
// reads its own stack when signalled (os::sample_thread); names are looked up afterwards.
static void dump_host_stacks() {
    const int self = static_cast<int>(os::thread_id());
    for (const os::ThreadInfo& t : os::list_threads()) {
        if (t.tid == self) continue;
        u64 pcs[28];
        const int n = os::sample_thread(t.tid, pcs, 28);
        std::string line;
        for (int i = 0; i < n; i++) {
            std::string sym = host_symbol(pcs[i]);
            if (sym.size() > 90) sym.resize(90);
            line += i ? " < " : " ";
            line += sym;
        }
        if (!n) line = " (no answer: signals blocked)";
        LOG("stack of host thread %d (%s):%s", t.tid, t.name.c_str(), line.c_str());
    }
}

namespace stepwatch {
void start() {
    if (std::getenv("HOST_WATCH_STEP")) LOG("stepwatch: HOST_WATCH_STEP needs the Windows debug registers; not on Linux");
}
}  // namespace stepwatch

namespace {

struct sigaction g_prev[NSIG];   // the handlers installed before ours (a fault outside kart's code goes on to them)

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

// A fault: the debug write watch's (watch.cpp), else a crash. Faults in other code (the Vulkan driver, a library) that
// another handler was installed for go on to it. Logs what Windows' crash_handler logs, then ends the process.
void crash_signal(int sig, siginfo_t* si, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
    const u64 rip = static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]);
    if (sig == SIGTRAP && watch::on_single_step(uc)) return;
    const bool memory = sig == SIGSEGV || sig == SIGBUS;
    const u64 fault = memory ? reinterpret_cast<u64>(si->si_addr) : 0;
    const u64 guest = fault - g_membase;
    const bool write = memory && (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;   // page fault error code: a write
    if (sig == SIGSEGV && write && guest < mem::AddressSpaceSize && watch::on_write_fault(guest, uc)) return;
    const ElfSymbols& own = elf_symbols();
    const bool ours = rip >= own.text_lo && rip < own.text_hi;
    if (!ours && guest >= mem::AddressSpaceSize) {
        const struct sigaction& prev = g_prev[sig];
        if (prev.sa_flags & SA_SIGINFO ? prev.sa_sigaction != nullptr : prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
            if (prev.sa_flags & SA_SIGINFO) prev.sa_sigaction(sig, si, context);
            else prev.sa_handler(sig);
            return;
        }
    }
    static std::atomic<bool> crashing{false};
    if (crashing.exchange(true)) {   // a second thread, or a fault in the report: the first one finishes it
        for (;;) pause();
    }
    log_line("[RED] crash: %s (code %d) at %s (rip %#llx), %s %#llx%s, thread %llu", signal_name(sig), si->si_code,
             host_symbol(rip).c_str(), rip, memory ? (write ? "writing" : "reading") : "addr",
             guest < mem::AddressSpaceSize ? guest : fault, guest < mem::AddressSpaceSize ? " (guest)" : "",
             static_cast<unsigned long long>(os::thread_id()));
    if (guest < mem::AddressSpaceSize) {
        // The guest memory blocks around the address.
        const mem::Info at = mem::query(guest);
        const mem::Info before = at.addr ? mem::query(at.addr - 1) : at;
        for (const mem::Info& b : {before, at})
            log_line("  block %#llx+%#llx state %#x perm %#x attr %#x", b.addr, b.size, b.state, b.perm, b.attr);
        log_line("  backing %p", static_cast<void*>(mem::backing_ptr(guest)));
    }
    u64 pcs[32];
    const int n = os::backtrace_from(rip, pcs, 32);
    for (int i = 1; i < n; i++) log_line("  #%-2d %s", i, host_symbol(pcs[i]).c_str());
    if (auto* t = kern::current_thread()) dump_guest_stack(&t->ctx);
    kern::dump_threads();
    gamepads::stop_vibration();
    if (g_log_file) fflush(g_log_file);
    _exit(3);   // (not exit(): the Vulkan driver's threads would run destructors under it)
}

}  // namespace

// The signal stack of the calling thread (a stack overflow is reported too): main, and each guest thread (kernel.cpp).
void host_signal_stack() {
    constexpr size_t Size = 256 << 10;
    stack_t ss{};
    ss.ss_sp = os::alloc_pages(Size);
    ss.ss_size = Size;
    if (ss.ss_sp) sigaltstack(&ss, nullptr);
}

static void install_crash_handler() {
    host_signal_stack();
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) {
        struct sigaction sa{};
        sa.sa_sigaction = crash_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_prev[sig]);
    }
}

#else  // Windows

std::string host_symbol(u64 rip) {
    static std::mutex m;
    std::scoped_lock lk{m};
    static bool init = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    // Outside kart.exe (the Vulkan driver, system DLLs): the module and offset too, as a driver's symbols are only its
    // exports and the nearest one can be far away.
    std::string mod;
    HMODULE hm = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &hm) && hm != GetModuleHandleW(nullptr)) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(hm, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < n; i++)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        char m8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, m8, sizeof m8, nullptr, nullptr);
        char b[MAX_PATH * 3 + 24];
        snprintf(b, sizeof b, "%s+%#llx", m8, rip - reinterpret_cast<u64>(hm));
        mod = b;
    }
    if (!init) return mod.empty() ? "?" : mod;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) return mod.empty() ? "?" : mod;
    char out[400];
    snprintf(out, sizeof out, "%s+%#llx", sym->Name, disp);
    return mod.empty() ? std::string(out) : mod + " (" + out + ")";
}

// The code addresses of a suspended thread's host stack, innermost first.
static int walk_host_stack(CONTEXT ctx, u64* out, int max) {
    int n = 0;
    __try {
        while (n < max && ctx.Rip) {
            out[n++] = ctx.Rip;
            DWORD64 base = 0;
            if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr)) {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &data, &frame, nullptr);
            } else {  // a leaf function: its return address is on top of the stack
                ctx.Rip = *reinterpret_cast<const u64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// Every thread of the process with its host call stack, for a stall: where each one runs or waits. A recompiled
// function carries its guest address in its name, so a guest thread's stack is also its guest call chain. A thread is
// suspended only while its stack is read; names are looked up after it runs again (the lookup takes locks it may hold).
static void dump_host_stacks() {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    int idle = 0;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        u64 pcs[28];
        int n = 0;
        bool locked = false;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            // The walk looks up unwind data under the loader's and the function tables' locks: a thread suspended while
            // holding one (a driver thread loading a DLL as it compiles a pipeline) would block it, and this thread with
            // it, leaving the other suspended for good (issue #23: the dump stopped after the guest threads and the game
            // never went on). So it walks on a helper and gives up after half a second.
            struct Walk {
                CONTEXT ctx{};
                u64 pcs[28]{};
                int n = 0;
                HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                ~Walk() { CloseHandle(done); }
            };
            auto w = std::make_shared<Walk>();
            w->ctx.ContextFlags = CONTEXT_FULL;
            if (w->done && GetThreadContext(h, &w->ctx)) {
                std::thread([w] {
                    w->n = walk_host_stack(w->ctx, w->pcs, 28);
                    SetEvent(w->done);
                }).detach();
                if (WaitForSingleObject(w->done, 500) == WAIT_OBJECT_0) n = w->n, std::memcpy(pcs, w->pcs, sizeof pcs);
                else locked = true;
            }
            ResumeThread(h);
        }
        PWSTR desc = nullptr;
        GetThreadDescription(h, &desc);
        CloseHandle(h);
        std::string line;
        bool pool_idle = false;
        for (int i = 0; i < n; i++) {
            std::string s = host_symbol(pcs[i]);
            if (i == 0 && s.find("WaitForWorkViaWorkerFactory") != std::string::npos) pool_idle = true;
            if (s.size() > 90) s.resize(90);
            line += i ? " < " : " ";
            line += s;
        }
        if (locked) line = " (not read: it holds a lock the stack walk needs)";
        if (pool_idle) idle++;   // (a thread-pool thread with nothing to do)
        else LOG("stack of host thread %lu (%ls):%s", te.th32ThreadID, desc && *desc ? desc : L"-", line.c_str());
        if (desc) LocalFree(desc);
    }
    CloseHandle(snap);
    if (idle) LOG("and %d idle thread-pool threads", idle);
}

// HOST_WATCH_STEP=1 (debug aid): hardware watchpoints (debug registers, every thread) on the game's frame step
// ([g_mkw_frame_obj + 0x214], vblanks per frame) and its vblank counter; after 25 s logs each distinct host stack
// that touched them, which names the recompiled guest functions reading the step.
namespace nvn {
extern std::atomic<uint64_t> g_frame;  // the game's presents so far
}
namespace stepwatch {
std::atomic<bool> armed{false};
constexpr int Max = 1 << 16, Depth = 6;
u64 hits[Max][Depth + 1];  // [0] = which watchpoint (DR6 bits)
std::atomic<int> count{0};

bool on_trap(CONTEXT* ctx) {
    if (!armed.load(std::memory_order_relaxed) || !(ctx->Dr6 & 0xF)) return false;
    const int i = count.fetch_add(1, std::memory_order_relaxed);
    if (i < Max) {
        hits[i][0] = ctx->Dr6 & 0xF;
        u64 pcs[Depth]{};
        const int n = walk_host_stack(*ctx, pcs, Depth);
        for (int k = 0; k < Depth; k++) hits[i][k + 1] = k < n ? pcs[k] : 0;
    }
    ctx->Dr6 = 0;
    return true;
}

void arm_all(const u64 a[4], const bool write_only[4], bool on) {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    u64 dr7 = 0;  // Ln; R/W = 11 (read or write) or 01 (write), LEN = 11 (4 bytes)
    for (int i = 0; i < 4; i++)
        if (on && a[i]) dr7 |= (1ull << (2 * i)) | ((write_only[i] ? 0xDull : 0xFull) << (16 + 4 * i));
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &ctx)) {
                ctx.Dr0 = on ? a[0] : 0, ctx.Dr1 = on ? a[1] : 0, ctx.Dr2 = on ? a[2] : 0, ctx.Dr3 = on ? a[3] : 0;
                ctx.Dr7 = dr7;
                SetThreadContext(h, &ctx);
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

// Watches a[] for `secs` seconds, then logs each distinct host stack that touched them under its watchpoint's name.
void run(const u64 a[4], const bool write_only[4], const std::string names[4], int secs) {
    count = 0;
    armed = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    while (std::chrono::steady_clock::now() < end) {  // (threads made later get armed too)
        arm_all(a, write_only, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    arm_all(a, write_only, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    armed = false;
    const int n = std::min(count.load(), Max);
    std::map<std::vector<u64>, int> uniq;
    for (int i = 0; i < n; i++) uniq[std::vector<u64>(hits[i], hits[i] + Depth + 1)]++;
    LOG("stepwatch: %d accesses, %zu distinct stacks", count.load(), uniq.size());
    for (const auto& [st, c] : uniq) {
        std::string line;
        for (int k = 1; k <= Depth && st[k]; k++) {
            std::string s = host_symbol(st[k]);
            if (s.size() > 60) s.resize(60);
            line += k > 1 ? " < " : " ";
            line += s;
        }
        std::string which;
        for (int i = 0; i < 4; i++)
            if (st[0] & (1ull << i)) which += (which.empty() ? "" : "+") + names[i];
        LOG("stepwatch: %s x%d:%s", which.c_str(), c, line.c_str());
    }
}

// Whether the 8 bytes at guest address `a` can be read.
bool readable(u64 a) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!a || !VirtualQuery(hp(a), &mi, sizeof mi)) return false;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// HOST_WATCH_MEM=<spec>[;<spec>...] (debug aid, up to 4): watchpoints on 4 bytes of game memory each. A spec is hex
// numbers joined by '>': an offset in main, then for each further number the pointer there is read and the number
// added, e.g. 37b22f8>e8>58 = [main+0x37b22f8] + 0xe8 read again + 0x58. A leading 'w' watches writes only.
// HOST_WATCH_AT=<s> waits that long after start first, HOST_WATCH_SECS=<s> is how long it watches (default 10).
void start_mem() {
    const char* spec = std::getenv("HOST_WATCH_MEM");
    if (!spec || !*spec) return;
    std::thread([text = std::string(spec)] {
        const int at = std::getenv("HOST_WATCH_AT") ? std::atoi(std::getenv("HOST_WATCH_AT")) : 0;
        const int secs = std::getenv("HOST_WATCH_SECS") ? std::atoi(std::getenv("HOST_WATCH_SECS")) : 10;
        std::this_thread::sleep_for(std::chrono::seconds(at));
        std::string names[4];
        bool wr[4]{};
        int n = 0;
        for (size_t i = 0; i < text.size() && n < 4;) {
            const size_t e = std::min(text.find(';', i), text.size());
            names[n] = text.substr(i, e - i);
            if (!names[n].empty() && names[n][0] == 'w') wr[n] = true;
            if (!names[n].empty()) n++;
            i = e + 1;
        }
        u64 a[4]{};
        for (;;) {  // (until every chain resolves)
            bool all = module_base("main") != 0;
            for (int k = 0; k < n && all; k++) {
                const char* p = names[k].c_str() + (wr[k] ? 1 : 0);
                char* end = nullptr;
                u64 g = module_base("main") + std::strtoull(p, &end, 16);
                while (*end == '>' && g) {
                    g = readable(g) ? ld<u64>(g) : 0;
                    if (g) g += std::strtoull(end + 1, &end, 16);
                }
                if (!g || !readable(g)) all = false;
                a[k] = g;
            }
            if (all) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        u64 h[4]{};
        for (int k = 0; k < n; k++) {
            h[k] = reinterpret_cast<u64>(hp(a[k]));
            LOG("stepwatch: %s = %#llx, now %#x (%g)", names[k].c_str(), static_cast<unsigned long long>(a[k]), ld<u32>(a[k]),
                static_cast<double>(ld<float>(a[k])));
        }
        run(h, wr, names, secs);
        for (int k = 0; k < n; k++)
            LOG("stepwatch: %s after %d s: %#x (%g)", names[k].c_str(), secs, ld<u32>(a[k]), static_cast<double>(ld<float>(a[k])));
    }).detach();
}

// The guest's writable memory as host ranges [guest, guest + size) of at most 64 MiB, the host pages that are there.
struct Range {
    u64 guest, size, off;  // off: where its words start in a snapshot
};
std::vector<Range> writable_ranges(u64& total) {
    std::vector<Range> out;
    total = 0;
    for (u64 a = 0; a < mem::AddressSpaceSize;) {
        const mem::Info b = mem::query(a);
        if (b.size == 0) break;
        if (b.state != mem::Free && b.perm == mem::RW)
            for (u64 p = b.addr; p < b.addr + b.size;) {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(hp(p), &mi, sizeof mi)) break;
                const u64 end = std::min(b.addr + b.size, p + (reinterpret_cast<u64>(mi.BaseAddress) + mi.RegionSize - reinterpret_cast<u64>(hp(p))));
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (u64 q = p; q < end; q += 64ull << 20) {
                        const u64 n = std::min(end - q, 64ull << 20);
                        out.push_back({q, n, total / 4});
                        total += n;
                    }
                p = end;
            }
        a = b.addr + b.size;
    }
    return out;
}

// A copy of guest memory the game may free meanwhile: ReadProcessMemory fails on a page that is gone instead of
// faulting (llvm-mingw's __try catches nothing). False if any of it was gone.
bool copy_may_fault(void* dst, const void* src, size_t n) {
    const HANDLE self = GetCurrentProcess();
    auto* d = static_cast<u8*>(dst);
    const auto* sp = static_cast<const u8*>(src);
    bool all = true;
    for (size_t o = 0; o < n;) {  // 1 MiB at a time, a page at a time where that fails
        const size_t piece = std::min<size_t>(n - o, 1 << 20);
        SIZE_T got = 0;
        if (ReadProcessMemory(self, sp + o, d + o, piece, &got) && got == piece) {
            o += piece;
            continue;
        }
        for (size_t p = o; p < o + piece; p += 4096) {
            const size_t len = std::min<size_t>(o + piece - p, 4096);
            if (!ReadProcessMemory(self, sp + p, d + p, len, &got) || got != len) std::memset(d + p, 0, len), all = false;
        }
        o += piece;
    }
    return all || n > 4096;  // (a big copy with holes is still worth comparing: the holes read as 0 both times)
}

bool still_there(u64 guest) {
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery(hp(guest), &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// fn(i, thread) for every range index i, on `threads` threads.
template <class F>
void each_range(size_t n, int threads, F fn) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++)
        ts.emplace_back([&, t] {
            for (size_t i; (i = next.fetch_add(1)) < n;) fn(i, t);
        });
    for (auto& t : ts) t.join();
}

// HOST_TICK_SCAN=<s>[,<s>...] (debug aid for the 120 Hz work): at each time since start, three snapshots of the guest's
// writable memory HOST_TICK_SCAN_GAP s apart (default 2) find the 32-bit ints and floats that grow at a steady rate
// of 0.2-2.2 per game frame (nvn::g_frame, presents) through both gaps: the game's frame counters. Each is logged with
// its rate; HOST_TICK_SCAN_WATCH=<n> then watches writes to the first n that grow 1 a frame (HOST_TICK_SCAN_STEP, the
// step to look for) for 1 s per four and logs who wrote them.
// HOST_TICK_SCAN_THREADS=<n> copies with n threads (default 8).
void start_scan() {
    const char* spec = std::getenv("HOST_TICK_SCAN");
    if (!spec || !*spec) return;
    std::thread([text = std::string(spec)] {
        const double default_gap = std::getenv("HOST_TICK_SCAN_GAP") ? std::atof(std::getenv("HOST_TICK_SCAN_GAP")) : 2.0;
        const int nwatch = std::getenv("HOST_TICK_SCAN_WATCH") ? std::atoi(std::getenv("HOST_TICK_SCAN_WATCH")) : 0;
        const int threads = std::getenv("HOST_TICK_SCAN_THREADS") ? std::max(1, std::atoi(std::getenv("HOST_TICK_SCAN_THREADS"))) : 8;
        const auto t0 = std::chrono::steady_clock::now();
        auto secs = [t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        auto wait_until = [&](double s) {
            while (secs() < s) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        auto rate_of = [](u32 a, u32 b, bool fl, double dt) -> double {
            if (!fl) return static_cast<double>(static_cast<s32>(b - a)) / dt;
            float fa, fb;
            std::memcpy(&fa, &a, 4), std::memcpy(&fb, &b, 4);
            if (!std::isfinite(fa) || !std::isfinite(fb) || std::fabs(fa) > 1e8f) return 0;
            return (static_cast<double>(fb) - fa) / dt;
        };
        const double minrate = std::getenv("HOST_TICK_SCAN_MINRATE") ? std::atof(std::getenv("HOST_TICK_SCAN_MINRATE")) : 0.2;
        // (HOST_TICK_SCAN_DOWN=1: counters that fall instead, timers counting down)
        const double sgn = std::getenv("HOST_TICK_SCAN_DOWN") && std::atoi(std::getenv("HOST_TICK_SCAN_DOWN")) ? -1.0 : 1.0;
        auto plausible = [minrate, sgn](double r, bool fl) { r *= sgn; return r >= (fl ? minrate : 0.2) && r <= 2.2; };  // per frame (HOST_TICK_SCAN_MINRATE: a float's lowest)
        struct Cand {
            u64 guest;
            u32 last, range;
            bool is_float;
            double rate;
        };
        u32* snap = nullptr;
        u64 snap_words = 0;
        for (size_t i = 0; i < text.size();) {
            const size_t e = std::min(text.find(',', i), text.size());
            const std::string item = text.substr(i, e - i);
            i = e + 1;
            // "<s>" or "f<frame>" (the game's frame count), either with "/<gap s>"
            const size_t slash = item.find('/');
            const double gap = slash != std::string::npos ? std::atof(item.c_str() + slash + 1) : default_gap;
            if (item[0] == 'f') {
                const u64 f = std::strtoull(item.c_str() + 1, nullptr, 10);
                while (nvn::g_frame.load() < f) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                wait_until(std::atof(item.c_str()));
            }
            u64 total = 0;
            const std::vector<Range> rs = writable_ranges(total);
            LOG("tickscan: start at frame %llu", static_cast<unsigned long long>(nvn::g_frame.load()));
            if (std::getenv("HOST_TICK_SCAN_LIST")) {
                LOG("tickscan: %zu ranges, %llu MiB", rs.size(), static_cast<unsigned long long>(total >> 20));
                continue;
            }
            if (total / 4 > snap_words) {  // (kept for the next scan: its pages are already there)
                if (snap) VirtualFree(snap, 0, MEM_RELEASE);
                snap_words = total / 4 + (256ull << 20) / 4;
                snap = static_cast<u32*>(VirtualAlloc(nullptr, snap_words * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (!snap) {
                    LOG("tickscan: no room for a %llu MiB snapshot", static_cast<unsigned long long>(total >> 20));
                    return;
                }
            }
            // A: copy each range, noting when.
            std::vector<double> ta(rs.size()), tb(rs.size());
            std::vector<u64> fa(rs.size()), fb(rs.size());  // the game's frame count when each was read
            std::vector<char> ok(rs.size());
            const double a0 = secs();
            const u64 f0 = nvn::g_frame.load();
            each_range(rs.size(), threads, [&](size_t k, int) {
                ok[k] = still_there(rs[k].guest);
                ta[k] = secs(), fa[k] = nvn::g_frame.load();
                if (ok[k]) ok[k] = copy_may_fault(snap + rs[k].off, hp(rs[k].guest), rs[k].size);
            });
            const double a1 = secs();
            // B, a gap later: the words whose change since A is a plausible counter's; then C, a gap after B, for the same
            // range at once (a clock that restarts every few seconds, such as a camera cut's, still shows): those that kept
            // their rate.
            std::vector<std::vector<Cand>> per(threads);
            std::vector<size_t> ncands(threads);
            std::vector<std::vector<u32>> copy(threads, std::vector<u32>((64ull << 20) / 4));
            each_range(rs.size(), threads, [&](size_t k, int t) {
                if (!ok[k] || !still_there(rs[k].guest)) {
                    ok[k] = 0;
                    return;
                }
                wait_until(ta[k] + gap);
                tb[k] = secs(), fb[k] = nvn::g_frame.load();
                if (!copy_may_fault(copy[t].data(), hp(rs[k].guest), rs[k].size)) return;  // (read at one moment, compared after)
                const u32* now = copy[t].data();
                const u32* was = snap + rs[k].off;
                const double frames = static_cast<double>(fb[k] - fa[k]);
                if (frames < 10) return;
                std::vector<Cand> mine;
                for (u64 w = 0; w < rs[k].size / 4; w++) {
                    const u32 x = was[w], y = now[w];
                    if (x == y) continue;
                    for (const bool fl : {false, true})
                        if (const double rt = rate_of(x, y, fl, frames); plausible(rt, fl) && mine.size() < 100'000)
                            mine.push_back({rs[k].guest + 4 * w, y, static_cast<u32>(k), fl, rt});
                }
                ncands[t] += mine.size();
                if (mine.empty()) return;
                wait_until(tb[k] + gap);
                const u64 fc = nvn::g_frame.load();
                if (!copy_may_fault(copy[t].data(), hp(rs[k].guest), rs[k].size)) return;
                const double frames2 = static_cast<double>(fc - fb[k]);
                if (frames2 < 10) return;
                for (const Cand& x : mine) {
                    const u32 v = copy[t][(x.guest - rs[k].guest) / 4];
                    const double rt = rate_of(x.last, v, x.is_float, frames2);
                    if (plausible(rt, x.is_float) && std::fabs(rt - x.rate) <= 0.03 * std::fabs(x.rate)) per[t].push_back({x.guest, v, x.range, x.is_float, (rt + x.rate) / 2});
                }
            });
            const double b1 = secs();
            std::vector<Cand> found;
            size_t total_cands = 0;
            for (int t = 0; t < threads; t++) found.insert(found.end(), per[t].begin(), per[t].end()), total_cands += ncands[t];
            LOG("tickscan at %.1f s (frame %llu): %zu ranges, %llu MiB, copied in %.2f s, compared in %.2f s; %zu candidates, %zu steady counters", a0,
                static_cast<unsigned long long>(f0), rs.size(), static_cast<unsigned long long>(total >> 20), a1 - a0, b1 - a1, total_cands, found.size());
            std::sort(found.begin(), found.end(), [](const Cand& x, const Cand& y) { return x.guest < y.guest; });
            for (size_t k = 0; k < found.size() && k < 20000; k++) {
                const Cand& c = found[k];
                float f;
                std::memcpy(&f, &c.last, 4);
                LOG("tickscan: %s %.5f/frame %s = %s", describe_guest(c.guest).c_str(), c.rate, c.is_float ? "float" : "int",
                    c.is_float ? std::to_string(f).c_str() : std::to_string(static_cast<s32>(c.last)).c_str());
            }
            // The ones that grow HOST_TICK_SCAN_STEP a frame (within 6%), one per value (instances of one kind of object
            // usually count alike), are watched; with HOST_TICK_SCAN_MAXVAL=<v> only those still below v (recently started).
            const double hz = std::getenv("HOST_TICK_SCAN_STEP") ? std::atof(std::getenv("HOST_TICK_SCAN_STEP")) : 1.0;
            const double maxval = std::getenv("HOST_TICK_SCAN_MAXVAL") ? std::atof(std::getenv("HOST_TICK_SCAN_MAXVAL")) : 1e30;
            std::vector<Cand> watch;
            for (const Cand& c : found) {
                float fv;
                std::memcpy(&fv, &c.last, 4);
                const double v = c.is_float ? static_cast<double>(fv) : static_cast<double>(static_cast<s32>(c.last));
                if (std::fabs(v) >= maxval) continue;
                if (std::fabs(c.rate - sgn * hz) > 0.06 * hz || static_cast<int>(watch.size()) >= nwatch) continue;
                bool seen = false;
                for (const Cand& w : watch) seen |= w.is_float == c.is_float && w.last == c.last;
                if (!seen) watch.push_back(c);
            }
            for (size_t k = 0; k < watch.size(); k += 4) {
                u64 a[4]{};
                bool wr[4] = {true, true, true, true};
                std::string names[4];
                for (size_t j = 0; j < 4 && k + j < watch.size(); j++) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(watch[k + j].guest));
                    a[j] = reinterpret_cast<u64>(hp(watch[k + j].guest)), names[j] = b;
                }
                run(a, wr, names, 1);
            }
        }
    }).detach();
}

void start() {
    start_mem();
    start_scan();
    if (!std::getenv("HOST_WATCH_STEP")) return;
    std::thread([] {
        while (!g_mkw_frame_obj) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const u64 a[4] = {reinterpret_cast<u64>(hp(g_mkw_frame_obj + 0x214)), reinterpret_cast<u64>(hp(module_base("main") + 0x33e6ae8))};
        const bool wr[4]{};
        const std::string names[4] = {"step", "counter"};
        LOG("stepwatch: frame object %#llx, watching for 25 s", static_cast<unsigned long long>(g_mkw_frame_obj));
        run(a, wr, names, 25);
    }).detach();
}
}  // namespace stepwatch

namespace {

LONG CALLBACK crash_handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && stepwatch::on_trap(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_SINGLE_STEP && watch::on_single_step(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000409) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 rip = ep->ContextRecord->Rip;
    u64 fault = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION) fault = ep->ExceptionRecord->ExceptionInformation[1];
    const u64 guest = fault - g_membase;
    // A store into a watched guest buffer (HOST_WATCH_SSBO).
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[0] == 1 && guest < mem::AddressSpaceSize &&
        watch::on_write_fault(guest, ep->ContextRecord)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // A page another thread is remapping (Windows unmaps a whole view to unmap part of it, then maps
    // the rest back): once the remap is done the page is accessible again, so retry.
    if (code == EXCEPTION_ACCESS_VIOLATION && guest < mem::AddressSpaceSize && mem::wait_mapped(guest)) {
        const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(hp(guest), &mbi, sizeof mbi);
        const DWORD prot = mbi.Protect & 0xff;
        const bool ok = mbi.State == MEM_COMMIT &&
                        (write ? prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE : prot != PAGE_NOACCESS);
        thread_local u64 last_fault = 0, repeats = 0;
        repeats = guest == last_fault ? repeats + 1 : 0;
        last_fault = guest;
        if (ok && repeats < 100000) return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Faults outside the host's own code (the Vulkan driver handles some of its own) are not ours.
    static const auto [image_lo, image_hi] = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const u8*>(dos) + dos->e_lfanew);
        const u64 lo = reinterpret_cast<u64>(dos);
        return std::pair{lo, lo + nt->OptionalHeader.SizeOfImage};
    }();
    if (code == EXCEPTION_ACCESS_VIOLATION && (rip < image_lo || rip >= image_hi) && guest >= mem::AddressSpaceSize) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    log_line("host exception %#lx at %s (rip %#llx), %s %#llx%s", code, host_symbol(rip).c_str(), rip,
             code == EXCEPTION_ACCESS_VIOLATION ? (ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading") : "addr",
             guest < mem::AddressSpaceSize ? guest : fault, guest < mem::AddressSpaceSize ? " (guest)" : "");
    if (guest < mem::AddressSpaceSize) {
        // The guest memory blocks around the address.
        const mem::Info at = mem::query(guest);
        const mem::Info before = at.addr ? mem::query(at.addr - 1) : at;
        for (const mem::Info& b : {before, at})
            log_line("  block %#llx+%#llx state %#x perm %#x attr %#x", b.addr, b.size, b.state, b.perm, b.attr);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(hp(guest), &mbi, sizeof mbi);
        log_line("  host page: state %#lx protect %#lx type %#lx region %p+%#zx, backing %p", mbi.State, mbi.Protect,
                 mbi.Type, mbi.BaseAddress, mbi.RegionSize, static_cast<void*>(mem::backing_ptr(guest)));
    }
    if (auto* t = kern::current_thread()) dump_guest_stack(&t->ctx);
    static std::atomic<bool> dumped{false};
    if (!dumped.exchange(true)) kern::dump_threads();
    gamepads::stop_vibration();
    // Not ExitProcess: the Vulkan driver's DLL detach can wait on threads that never return.
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A fault nobody handled: crash_handler leaves those outside kart.exe to their module (the Vulkan driver handles some
// of its own), so a crash inside the driver used to end the process with 0xC0000005 and nothing in host.log (issues
// #23, #26, #29). Logs where it happened and the thread's stack, then lets Windows end the process as before.
LONG WINAPI last_chance(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const u64 rip = ep->ContextRecord->Rip;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        log_line("[RED] crash: exception %#lx at %s (rip %#llx), %s %#llx, thread %lu", er->ExceptionCode, host_symbol(rip).c_str(), rip,
                 er->ExceptionInformation[0] == 8 ? "executing" : er->ExceptionInformation[0] ? "writing" : "reading",
                 static_cast<unsigned long long>(er->ExceptionInformation[1]), GetCurrentThreadId());
    else
        log_line("[RED] crash: exception %#lx at %s (rip %#llx), thread %lu", er->ExceptionCode, host_symbol(rip).c_str(), rip,
                 GetCurrentThreadId());
    u64 pcs[32];
    const int n = walk_host_stack(*ep->ContextRecord, pcs, 32);
    for (int i = 1; i < n; i++) log_line("  #%-2d %s", i, host_symbol(pcs[i]).c_str());
    if (g_log_file) fflush(g_log_file);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
#endif  // _WIN32

std::string describe_guest(u64 addr) {
    char buf[96];
    for (const auto& m : g_loaded) {
        if (addr - m.base < m.size) {
            snprintf(buf, sizeof buf, "%#llx (%s+%#llx)", addr, m.name.c_str(), addr - m.base);
            return buf;
        }
    }
    snprintf(buf, sizeof buf, "%#llx", addr);
    return buf;
}

void dump_guest_stack(Ctx* c) {
    log_line("  sp=%#llx lr=%s fp=%#llx", c->sp, describe_guest(c->x[30]).c_str(), c->x[29]);
    u64 frames[32];
    const int n = walk_frames(c->x[29], frames, 32);
    for (int i = 0; i < n; i++) log_line("  #%-2d %s", i, describe_guest(frames[i]).c_str());
}

// ------------------------------------------------------------------ 120 Hz: jobs at half rate
// The game's frame runs its subsystems ("modules") as jobs of a few job lists (tools/patches.py mkw_jobs). At
// HOST_TICK_HZ=120 the ones that count frames (race sequence, menus) would run twice as fast; the ones named here run
// every other tick instead (when the tick completes no whole 60th, g_mkw_tick_whole == 0), and named modules get a tick
// time of their own (1/60 s, rate 1). HOST_TICK_HALF_JOBS=<hex key>,... names jobs by what they call (main offsets the
// job log shows, HOST_TICK_JOB_LOG=1); HOST_TICK_HALF_MODULES=<hex module vtable>,... names modules: their jobs and
// their tick time; HOST_TICK_HALF_ALL=1 halves every job not named in HOST_TICK_FULL_JOBS / _FULL_MODULES.
namespace {
struct HalfRate {
    std::vector<u64> jobs, modules, full_jobs, full_modules;
    std::vector<std::string> actors;  // HOST_TICK_HALF_ACTORS: names, or prefixes ending in '*'
    std::string while_actor;          // HOST_TICK_HALF_WHILE: only while this actor is about (its jobs run)
    double from = 0;                  // HOST_TICK_HALF_FROM: not before this many seconds
    bool all = false, log = false;
    HalfRate() {
        auto list = [](const char* name, std::vector<u64>& into) {
            for (const char* p = std::getenv(name); p && *p;) {
                char* end = nullptr;
                const unsigned long long x = std::strtoull(p, &end, 16);
                if (end == p) break;
                into.push_back(x);
                p = *end ? end + 1 : end;
            }
            std::sort(into.begin(), into.end());
        };
        list("HOST_TICK_HALF_JOBS", jobs), list("HOST_TICK_HALF_MODULES", modules);
        list("HOST_TICK_FULL_JOBS", full_jobs), list("HOST_TICK_FULL_MODULES", full_modules);
        if (const char* v = std::getenv("HOST_TICK_HALF_WHILE")) while_actor = v;
        if (const char* v = std::getenv("HOST_TICK_HALF_FROM")) from = std::atof(v);
        if (const char* v = std::getenv("HOST_TICK_HALF_ACTORS"))
            for (std::string t = v; !t.empty();) {
                const size_t e = std::min(t.find(','), t.size());
                if (e) actors.push_back(t.substr(0, e));
                t.erase(0, std::min(e + 1, t.size()));
            }
        all = std::getenv("HOST_TICK_HALF_ALL") && std::atoi(std::getenv("HOST_TICK_HALF_ALL"));
        g_job_log = std::getenv("HOST_TICK_JOB_LOG") ? std::atoi(std::getenv("HOST_TICK_JOB_LOG")) : 0;
        log = g_job_log > 0;
    }
    bool on() const { return all || !jobs.empty() || !modules.empty() || !actors.empty(); }
    bool actor_named(const std::string& n) const {
        for (const std::string& a : actors)
            if (a.back() == '*' ? n.compare(0, a.size() - 1, a, 0, a.size() - 1) == 0 : n == a) return true;
        return false;
    }
    static bool has(const std::vector<u64>& v, u64 x) { return x && std::binary_search(v.begin(), v.end(), x); }
};
const HalfRate& half_rate() {
    static const HalfRate h;
    return h;
}
u64 main_off(u64 a) {
    for (const auto& m : g_loaded)
        if (m.name == "main") return a - m.base < m.size ? a - m.base : 0;
    return 0;
}
bool guest_readable(u64 a, u64 n = 8) {
    if (!a || a >= mem::AddressSpaceSize) return false;
    const mem::Info b = mem::query(a);
    return b.state != mem::Free && (b.perm & mem::R) && a + n <= b.addr + b.size;
}
u64 peek(u64 a) { return guest_readable(a) ? ld<u64>(a) : 0; }

std::atomic<const std::vector<u64>*> g_half_objs{nullptr};  // job objects (tagged as in the list) to skip, sorted
struct JobWho {
    u64 obj, vt;
    int module;
    bool operator<(const JobWho& o) const { return obj < o.obj; }
};
std::atomic<const std::vector<JobWho>*> g_job_who{nullptr};  // every listed job: its vtable and module (HOST_TICK_JOB_LOG=2)
std::mutex g_half_lock;
std::vector<u64> g_all_modules;   // the module system's modules (guest addresses)
std::vector<u64> g_half_modules;  // module objects with a tick time of their own (guest addresses)
u64 g_half_page = 0;              // guest page: [0] the skip job's vtable pointer, [0x10..] its vtable, [0x80] the 60 Hz tick time
u64 g_mkw_sys = 0;

// A guest page for the skip job and the 60 Hz tick time, and a function that returns at once for the skip job to run.
u64 half_page() {
    if (g_half_page) return g_half_page;
    u64 ret_fn = 0;
    for (const auto& m : g_loaded)
        if (m.name == "main")
            for (u64 a = m.base; a < m.base + m.size && !ret_fn; a += 4)
                if (mem::query(a).perm == mem::RX && ld<u32>(a) == 0xd65f03c0u && recompiled_fn(a)) ret_fn = a;
    const u64 page = mem::find_free(mem::AslrBase, mem::AslrSize, mem::PageSize);
    if (!ret_fn || !page) {
        LOG("half rate: no room for the skip job");
        return 0;
    }
    mem::map(page, mem::PageSize, mem::Normal, mem::RW);
    st<u64>(page, page + 0x10);
    for (int i = 0; i < 8; i++) st<u64>(page + 0x10 + 8 * i, ret_fn);
    LOG("half rate: skip job at %#llx runs %s", static_cast<unsigned long long>(page), describe_guest(ret_fn).c_str());
    return g_half_page = page;
}

// What a job calls: the job object's run function, or (the default run: an inner callable at +0x10) the callable's,
// or (that one a member-function delegate: object +8, function +0x10, adjust +0x18) the member function. `self` gets
// the object it runs on when one is known.
u64 job_target(u64 obj, u64* self, std::string* how) {
    *self = 0;
    const bool tagged = obj & 1;
    obj &= ~1ull;
    const u64 vt = peek(obj), run = peek(vt + 0x10);
    char b[256];
    if (tagged) {
        if (how) snprintf(b, sizeof b, "tagged obj %#llx vt %s run %s", obj, describe_guest(vt).c_str(), describe_guest(run).c_str()), *how = b;
        *self = obj;
        return run;
    }
    const u64 inner = peek(obj + 0x10), ivt = peek(inner), irun = peek(ivt + 0x20);
    const u64 dobj = peek(inner + 8), dfn = peek(inner + 0x10), dadj = peek(inner + 0x18);
    u64 target = irun;
    if (dobj && main_off(dobj) == 0) {
        const u64 thiz = dobj + (static_cast<s64>(dadj) >> 1);
        u64 fn = dfn;
        if (dadj & 1) fn = peek(peek(thiz) + dfn);
        if (main_off(fn)) target = fn, *self = thiz;
    }
    if (how) {
        snprintf(b, sizeof b, "obj %#llx vt %s run %s; inner %#llx vt %s run %s; delegate %#llx %#llx %#llx",
                 obj, describe_guest(vt).c_str(), describe_guest(run).c_str(), inner, describe_guest(ivt).c_str(),
                 describe_guest(irun).c_str(), dobj, dfn, dadj);
        *how = b;
    }
    return target;
}
}  // namespace

// The strings an actor points to (directly or one pointer away, first 0x800 bytes): its name among them (HOST_TICK_JOB_LOG=2).
static void log_actor_strings(u64 a) {
    static std::mutex lock;
    static std::vector<u64> seen;
    std::lock_guard<std::mutex> lk(lock);
    if (std::find(seen.begin(), seen.end(), a) != seen.end()) return;
    seen.push_back(a);
    auto str = [](u64 p, std::string& out) {
        if (!guest_readable(p, 4)) return false;
        out.clear();
        for (u64 k = 0; k < 64; k++) {
            if (!guest_readable(p + k, 1)) return false;
            const char ch = static_cast<char>(ld<u8>(p + k));
            if (!ch) break;
            if (ch < 32 || ch > 126) return false;
            out += ch;
        }
        return out.size() >= 4;
    };
    std::string line, sv;
    for (u64 off = 0; off < 0x800 && line.size() < 900; off += 8) {
        const u64 p = peek(a + off);
        if (!p || main_off(p) && main_off(p) < 0x2c00000) continue;  // (code)
        if (str(p, sv)) {
            line += " +" + std::to_string(off) + "=\"" + sv + "\"";
            continue;
        }
        for (u64 o2 = 0; o2 < 0x40; o2 += 8)
            if (const u64 q = peek(p + o2); q && str(q, sv)) {
                line += " +" + std::to_string(off) + ">" + std::to_string(o2) + "=\"" + sv + "\"";
                break;
            }
    }
    LOG("actor %#llx:%s", static_cast<unsigned long long>(a), line.c_str());
}

// The scheduler starts a job list (tools/patches.py mkw_jobs): {..., +8 count, +0x10 descriptors (+8 the job object)}.
// A list is looked at again whenever its jobs change (scenes add and remove some).
void mkw_job_list(u64 list) {
    const HalfRate& h = half_rate();
    if (!h.on() && !h.log) return;
    const s32 n = static_cast<s32>(ld<u32>(list + 8));
    const u64 descs = ld<u64>(list + 0x10);
    if (n < 0 || n > 4096 || (n && !guest_readable(descs, 8ull * n))) return;
    u64 sig = static_cast<u64>(n) * 0x9E3779B97F4A7C15ull ^ descs;
    for (s32 i = 0; i < n; i++) sig = (sig ^ ld<u64>(ld<u64>(descs + 8 * i) + 8)) * 0x100000001B3ull;
    std::lock_guard<std::mutex> lk(g_half_lock);
    static std::map<u64, std::pair<u64, std::vector<u64>>> seen;  // list -> its signature, its jobs to skip
    static std::map<u64, std::vector<JobWho>> who;                // list -> its jobs
    auto& entry = seen[list];
    who[list].clear();
    if (entry.first == sig && sig) return;
    entry.first = sig;
    entry.second.clear();
    if (h.on()) half_page();
    const char* name = "?";
    static const char* const names[] = {"4d0", "4d8", "4e0", "4e8", "4f0", "4f8"};
    for (int i = 0; i < 6 && g_mkw_sys; i++)
        if (peek(g_mkw_sys + 0x4d0 + 8 * i) == list) name = names[i];
    if (h.log) LOG("jobs: list %#llx (sys+0x%s), %d jobs", static_cast<unsigned long long>(list), name, n);
    for (s32 i = 0; i < n; i++) {
        const u64 d = peek(descs + 8 * i), obj = peek(d + 8);
        u64 self = 0;
        std::string how;
        const u64 target = job_target(obj, &self, h.log ? &how : nullptr);
        // A module's own job runs on the module; its helper jobs (+0x10 the module) count as the module's too.
        const auto is_module = [](u64 m) { return m && std::find(g_all_modules.begin(), g_all_modules.end(), m) != g_all_modules.end(); };
        u64 module = is_module(self) ? self : 0;
        if (!module && (obj & 1) && is_module(peek((obj & ~1ull) + 0x10))) module = peek((obj & ~1ull) + 0x10);
        const u64 key = main_off(target), svt = main_off(peek(self)), mvt = main_off(peek(module));
        const bool full = HalfRate::has(h.full_jobs, key) || HalfRate::has(h.full_modules, svt) || HalfRate::has(h.full_modules, mvt);
        const bool half = !full && (h.all || HalfRate::has(h.jobs, key) || HalfRate::has(h.modules, svt) || HalfRate::has(h.modules, mvt));
        if (half) entry.second.push_back(obj);
        const auto mi = std::find(g_all_modules.begin(), g_all_modules.end(), module);
        who[list].push_back({obj, module ? mvt : svt, module && mi != g_all_modules.end() ? static_cast<int>(mi - g_all_modules.begin()) : -1});
        if (h.log)
            LOG("jobs:  %s[%d] key %#llx self %#llx (vt main+%#llx) module vt main+%#llx%s  %s", name, i,
                static_cast<unsigned long long>(key), static_cast<unsigned long long>(self), static_cast<unsigned long long>(svt),
                static_cast<unsigned long long>(mvt), half ? " HALF" : "", how.c_str());
    }
    std::vector<u64> skip;
    for (const auto& [l, e] : seen) skip.insert(skip.end(), e.second.begin(), e.second.end());
    std::sort(skip.begin(), skip.end());
    g_half_objs.store(new std::vector<u64>(std::move(skip)));  // (the old one stays: a job may be reading it)
    if (g_job_log >= 2) {
        std::vector<JobWho> all;
        for (const auto& [l, v] : who) all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        g_job_who.store(new std::vector<JobWho>(std::move(all)));
    }
}

// An actor's job (+0x18 its tick time, +0x20 the actor; the actor's name at actor +0x20) of an actor HOST_TICK_HALF_ACTORS
// names: -1 not one, 0 sit this tick out, 1 run, with a 60 Hz copy of its tick time. Each worker keeps what it found
// per job object (checked again when the actor or its name changes).
namespace {
std::mutex g_shadow_lock;
std::atomic<u64> g_shadow_of[32];  // tick times the shadows at half page + 0x200 + 0x40 * k copy
u64 shadow_for(u64 orig) {
    for (int k = 0; k < 32; k++)
        if (g_shadow_of[k].load(std::memory_order_relaxed) == orig) return g_half_page + 0x200 + 0x40 * k;
    std::lock_guard<std::mutex> lk(g_shadow_lock);
    for (int k = 0; k < 32; k++) {
        const u64 v = g_shadow_of[k].load();
        if (v == orig) return g_half_page + 0x200 + 0x40 * k;
        if (!v) {
            g_shadow_of[k].store(orig);
            LOG("half rate: 60 Hz copy %d of the tick time at %#llx", k, static_cast<unsigned long long>(orig));
            return g_half_page + 0x200 + 0x40 * k;
        }
    }
    return 0;
}
}  // namespace
std::atomic<s64> g_while_seen{-1};  // the tick (g_half_ticks) the HOST_TICK_HALF_WHILE actor's job last came by
std::atomic<s64> g_half_ticks{0};
std::atomic<bool> g_half_on{false};  // the half rate is on this tick (half_tick)
static s64 steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static int half_actor_job(u64 obj, bool active) {
    struct Seen {
        u64 actor, namep, orig;
        bool half, is_actor, is_while;
    };
    thread_local std::unordered_map<u64, Seen> seen;
    const u64 o = obj & ~1ull, actor = ld<u64>(o + 0x20);
    auto it = seen.find(o);
    if (it == seen.end() || it->second.actor != actor || (it->second.is_actor && ld<u64>(actor + 0x20) != it->second.namep)) {
        Seen e{actor, 0, 0, false, false, false};
        std::string name;
        if (guest_readable(actor + 0x20) && (e.namep = ld<u64>(actor + 0x20)) && guest_readable(e.namep, 1))
            for (u64 k = 0; k < 64 && guest_readable(e.namep + k, 1); k++) {
                const char ch = static_cast<char>(ld<u8>(e.namep + k));
                if (!ch) break;
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
                    name.clear();
                    break;
                }
                name += ch;
            }
        if (name.size() < 3) name.clear();  // (not an actor's name: some other job's +0x20)
        e.is_actor = guest_readable(actor + 0x20);
        e.half = !name.empty() && half_rate().actor_named(name);
        e.is_while = !name.empty() && name == half_rate().while_actor;
        if (e.half) LOG("half rate: actor %s (%#llx) job %#llx runs every other tick", name.c_str(), static_cast<unsigned long long>(actor),
                        static_cast<unsigned long long>(o));
        it = seen.insert_or_assign(o, e).first;
    }
    Seen& e = it->second;
    if (e.is_while) g_while_seen.store(g_half_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!e.half) return -1;
    if (!active) {  // (its own tick time back)
        if (e.orig && ld<u64>(o + 0x18) - g_half_page < mem::PageSize) st<u64>(o + 0x18, e.orig);
        return -1;
    }
    if (g_mkw_tick_whole == 0) return 0;
    const u64 cur = ld<u64>(o + 0x18);
    if (cur - g_half_page >= mem::PageSize) e.orig = cur;  // (the game set it: follow)
    if (const u64 sh = e.orig ? shadow_for(e.orig) : 0) {
        for (u64 k = 0; k < 0x40; k += 8) st<u64>(sh + k, ld<u64>(e.orig + k));
        st<u32>(sh, 0x3c888889u), st<u32>(sh + 4, 0x3f800000u);
        st<u64>(o + 0x18, sh);
    }
    return 1;
}

// A job is about to run (tools/patches.py mkw_jobs): the object it runs, or the skip job on the ticks it sits out.
// Each tick: whether the half rate is on (HOST_TICK_HALF_FROM: not before that many seconds after the first tick;
// HOST_TICK_HALF_WHILE: only while that actor's jobs come by, within the last 30 ticks).
void half_tick() {
    const HalfRate& h = half_rate();
    if (!h.on()) return;
    static const s64 t0 = steady_ms();
    const s64 tick = g_half_ticks.fetch_add(1) + 1, seen = g_while_seen.load(std::memory_order_relaxed);
    const bool on = steady_ms() - t0 >= static_cast<s64>(h.from * 1000) && (h.while_actor.empty() || (seen >= 0 && tick - seen <= 30));
    if (on != g_half_on.exchange(on)) LOG("half rate: %s", on ? "on" : "off");
}
static bool half_active() { return g_half_on.load(std::memory_order_relaxed); }
u64 mkw_job_obj(u64 obj) {
    if (const std::vector<JobWho>* w = g_job_who.load(std::memory_order_acquire)) {
        const auto it = std::lower_bound(w->begin(), w->end(), JobWho{obj, 0, 0});
        if (it != w->end() && it->obj == obj) t_job_vt = it->vt, t_job_module = it->module, t_job_actor = 0;
        else {  // a job of another list: its vtable (untagged: the job wraps a callable); an actor's job: +0x20 the actor
            t_job_vt = main_off(ld<u64>(obj & ~1ull)) | (obj & 1 ? 0 : 1ull << 63), t_job_module = -1;
            const u64 a = (obj & 1) ? peek((obj & ~1ull) + 0x20) : 0;
            t_job_actor = a && main_off(peek(a)) ? a : 0;
        }
    }
    if (!g_half_page) return obj;
    const bool active = half_active();
    if ((obj & 1) && (!half_rate().actors.empty() || !half_rate().while_actor.empty())) {
        if (const int r = half_actor_job(obj, active); r >= 0) return r ? obj : g_half_page | 1;
    }
    if (!active) return obj;
    if (g_mkw_tick_whole != 0) return obj;
    const std::vector<u64>* s = g_half_objs.load(std::memory_order_acquire);
    if (!s || !std::binary_search(s->begin(), s->end(), obj)) return obj;
    return g_half_page | 1;
}

// A job returned (tools/patches.py mkw_jobs).
void mkw_job_done() { t_job_module = -2; }

// The frame's module phase hands each module its tick time (tools/patches.py mkw_jobs; the module system: +0x280 count,
// +0x288 modules, +0x500 the tick time): the named modules get the 60 Hz one.
void mkw_tick_modules(u64 sys) {
    const HalfRate& h = half_rate();
    if (!h.on() && !h.log) return;
    const s32 n = static_cast<s32>(ld<u32>(sys + 0x280));
    const u64 mods = ld<u64>(sys + 0x288);
    if (sys != g_mkw_sys) {
        std::lock_guard<std::mutex> lk(g_half_lock);
        g_mkw_sys = sys;
        g_half_modules.clear(), g_all_modules.clear();
        if (h.on()) half_page();
        for (s32 i = 0; i < n && i < 1024; i++) {
            const u64 m = peek(mods + 8 * i), vt = main_off(peek(m));
            const bool full = HalfRate::has(h.full_modules, vt);
            const bool half = !full && (h.all || HalfRate::has(h.modules, vt));
            if (half) g_half_modules.push_back(m);
            g_all_modules.push_back(m);
            if (h.log)
                LOG("jobs: module %d %#llx vt main+%#llx phase obj %#llx%s", i, static_cast<unsigned long long>(m),
                    static_cast<unsigned long long>(vt), static_cast<unsigned long long>(peek(m + 0x60)), half ? " HALF" : "");
        }
        if (h.log)
            for (int i = 0; i < 6; i++) LOG("jobs: sys+%#x list %#llx", 0x4d0 + 8 * i, static_cast<unsigned long long>(ld<u64>(sys + 0x4d0 + 8 * i)));
    }
    if (!g_half_page || g_half_modules.empty() || !half_active()) return;
    // A copy of the shared tick time (+0x10: the set of paused modules, and more) at 1/60 s, rate 1.
    const u64 t = g_half_page + 0x80, shared = ld<u64>(sys + 0x500);
    for (u64 k = 0; k < 0x40; k += 8) st<u64>(t + k, ld<u64>(shared + k));
    st<u32>(t, 0x3c888889u), st<u32>(t + 4, 0x3f800000u);
    for (const u64 m : g_half_modules) st<u64>(m + 0x58, t);
}

void host_override(u64 addr, GuestFn fn) { g_overrides[addr] = fn; }

GuestFn recompiled_fn(u64 addr) {
    for (const auto& m : g_loaded)
        if (addr - m.base < m.size) return lookup_fn(m.table, m.table_size, addr - m.base);
    return nullptr;
}

u64 module_base(const char* name) {
    for (const auto& m : g_loaded)
        if (m.name == name) return m.base;
    return 0;
}

// ------------------------------------------------------------------ entry points for recompiled code

GuestFn lookup_fn(const FnEntry* table, size_t size, uint64_t addr) {
    size_t lo = 0, hi = size;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < size && table[lo].addr == addr ? table[lo].fn : nullptr;
}

void call_indirect(Ctx* c, uint64_t target) {
    struct Entry {
        u64 addr;
        GuestFn fn;
    };
    thread_local Entry cache[4096]{};
    Entry& e = cache[(target >> 2) & 4095];
    if (e.addr != target || !e.fn) {
        GuestFn fn = find_guest_fn(target);
        if (!fn) fatal("indirect branch to code that was not recompiled: %s (from %s)", describe_guest(target).c_str(),
                       describe_guest(c->x[30]).c_str());
        e = {target, fn};
    }
    e.fn(c);
}

void guest_svc(Ctx* c, uint32_t imm) { kern::svc(c, imm); }
void guest_yield(Ctx* c) { kern::preempt(c); }
void guest_trap(Ctx* c, uint64_t pc, uint32_t imm) { fatal("guest trap %#x at %s", imm, describe_guest(pc).c_str()); }
void guest_unresolved_jump(Ctx* c, uint64_t from, uint64_t target) {
    fatal("jump table at %s reached unknown target %s", describe_guest(from).c_str(), describe_guest(target).c_str());
}

static uint32_t crc32_bytes(uint32_t acc, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        acc ^= static_cast<uint8_t>(v >> (8 * i));
        for (int b = 0; b < 8; b++) acc = (acc >> 1) ^ (0xEDB88320u & (0u - (acc & 1)));
    }
    return acc;
}
uint32_t crc32_8(uint32_t a, uint64_t v) { return crc32_bytes(a, v, 1); }
uint32_t crc32_16(uint32_t a, uint64_t v) { return crc32_bytes(a, v, 2); }
uint32_t crc32_32(uint32_t a, uint64_t v) { return crc32_bytes(a, v, 4); }
uint32_t crc32_64(uint32_t a, uint64_t v) { return crc32_bytes(a, v, 8); }

// ------------------------------------------------------------------ main

HostConfig g_config;

namespace nvk { int bake_textures(const std::filesystem::path& game, size_t check); }

// Clang PGO (tools/build_lib.py BUILD_PGO=gen): instrumented modules link clang's profile runtime
// (clang_rt.profile); in every other build this resolves to the no-op below.
#ifdef _WIN32
extern "C" int __llvm_profile_write_file(void);
extern "C" int kart_pgo_nop(void) { return 0; }
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=kart_pgo_nop")
#else
extern "C" __attribute__((weak)) int __llvm_profile_write_file(void);
#endif

std::string host_exe_variant();  // localcfg.cpp

#ifdef _WIN32
extern "C" {
NTSYSAPI LONG NTAPI NtQueryTimerResolution(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);
NTSYSAPI LONG NTAPI NtSetTimerResolution(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
}
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

// Sets the system timer to its finest resolution, kept even while the window is in the background (no power
// throttling of timers); returns the resolution now in effect, in 100 ns units.
static ULONG finest_timer_resolution() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
    ULONG coarsest = 0, finest = 0, current = 0;
    NtQueryTimerResolution(&coarsest, &finest, &current);
    NtSetTimerResolution(finest, TRUE, &current);
    return current;
}
#endif

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc_start);
#ifdef _WIN32
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
#else
    // Timers fire within 1 ns of when they are due instead of the default 50 us (the game's job loops sleep 20 us
    // between polls); threads started from here on inherit it.
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
#endif

    // The game dump (the folder holding exefs\ and romfs\): the first argument, else KART_GAME from the environment
    // or local.cfg (localcfg.cpp)
    fs::path game;
    if (argc > 1) {
        game = argv[1];
    } else if (const char* e = getenv("KART_GAME"); e && *e) {
        game = e;
    }
    if (game.empty()) {
        fprintf(stderr, "usage: %s <game dump folder, holding exefs\\ and romfs\\> [--bake-textures [n]]\n"
                        "       (or set KART_GAME to that folder, in the environment or in local.cfg)\n",
                argv[0]);
        return 2;
    }
    g_config.game_dir = game;
    // kart-<version>.exe (another game version) keeps its saves, caches and logs apart: user-<version>.
    const std::string variant = host_exe_variant();
    g_config.user_dir = fs::current_path() / (variant.empty() ? std::string("user") : "user-" + variant);
    // HOST_USER_DIR: another folder for saves and logs (a second instance on this PC, for LAN play).
    if (const char* e = getenv("HOST_USER_DIR"); e && *e) g_config.user_dir = fs::path(e);
    fs::create_directories(g_config.user_dir);
    g_log_file = os::open_file(g_config.user_dir / "host.log", "w");
    LOG("game %s", game.string().c_str());

    if (argc > 2 && std::strcmp(argv[2], "--bake-textures") == 0) return nvk::bake_textures(game, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0);

#ifdef _WIN32
    AddVectoredExceptionHandler(0, crash_handler);
    SetUnhandledExceptionFilter(last_chance);
    // The finest host timer resolution (0.5 ms), for the renderer's acquire polling and the game's short timed waits.
    LOG("host timer resolution %lu us", finest_timer_resolution() / 10);
#else
    install_crash_handler();
#endif
    mem::init();
    video::init();

    // HOST_FPS_MODE=dynamic (default) | 60 | 30 | game: the game's present interval (cpu.h
    // g_mkw_frame_mode). Dynamic keeps the game clock at real time when frames run late.
    {
        const char* v = std::getenv("HOST_FPS_MODE");
        const std::string mode = v ? v : "dynamic";
        g_mkw_frame_mode = mode == "game" ? -1 : mode == "60" ? 1 : mode == "30" ? 2 : 0;
        LOG("frame mode %s (%d)", mode.c_str(), g_mkw_frame_mode);
    }
    // HOST_TICK_HZ=<n> (experiment): the game's ticks last 1/n s instead of 1/60 (tools/patches.py mkw_tick_time),
    // rate 60/n; HOST_TICK_DT=<s> and HOST_TICK_RATE=<x> set the two apart. With HOST_NVN_PRESENT_HZ=<n> and
    // HOST_FPS_MODE=60 that is one shorter tick per frame.
    {
        float dt = 1.f / 60.f, rate = 1.f;
        if (const char* v = std::getenv("HOST_TICK_HZ"); v && std::atof(v) > 0) dt = 1.f / static_cast<float>(std::atof(v)), rate = 60.f * dt;
        if (const char* v = std::getenv("HOST_TICK_DT"); v && std::atof(v) > 0) dt = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("HOST_TICK_RATE"); v && std::atof(v) > 0) rate = static_cast<float>(std::atof(v));
        std::memcpy(&g_mkw_tick_dt, &dt, 4), std::memcpy(&g_mkw_tick_rate, &rate, 4);
        if (g_mkw_tick_dt != 0x3c888889u || g_mkw_tick_rate != 0x3f800000u) LOG("tick time %g s, rate %g", dt, rate);
        g_mkw_tick_ratef = rate;  // (the counters' step: mkw_tick_slot)
        if (const char* v = std::getenv("HOST_TICK_STEP"); v && !std::strcmp(v, "whole")) g_mkw_tick_step_whole = true, LOG("tick count: whole 60ths");
        g_mkw_tick_secf = g_mkw_tick_step_whole ? 1.f : rate;
        half_rate();              // (reads HOST_TICK_JOB_LOG before the first counter runs)
    }
    stepwatch::start();

    u64 base = 0x803fd000;  // where the console's loader put rtld for this title
    for (size_t i = 0; i < g_aot_module_count; i++) {
        const AotModuleDesc& d = g_aot_modules[i];
        const u64 size = load_nso(game / "exefs" / d.name, d, base);
        *d.base = base;
        g_loaded.push_back({d.name, base, size, d.table, aot_table_size(i)});
        LOG("%s at %#llx (%zu functions)", d.name, base, aot_table_size(i));
        base += size;
    }

    ipc::register_all_services();
    host_install_overrides();

    const u64 stack_size = 0x100000;
    const u64 stack = mem::StackBase + 0x4000;
    mem::map(stack, stack_size, mem::Stack, mem::RW);
    kern::start_main_thread(g_loaded[0].base, stack + stack_size, stack_size);

    // HOST_DUMP_AFTER=<s>: log every guest thread's state after that many seconds (stalls).
    if (const char* v = std::getenv("HOST_DUMP_AFTER")) {
        const int secs = std::atoi(v);
        std::thread([secs] {
            std::this_thread::sleep_for(std::chrono::seconds{secs});
            kern::dump_threads();
            dump_host_stacks();
        }).detach();
    }
    // A stall: the game showed frames and then none for HOST_STALL_DUMP_S seconds (default 10, 0 = never; normal runs
    // never go 6 s without one). Logs what every guest thread waits on and every host thread's stack, once per stall
    // and at most 3 times a run: the rare mid-race hang leaves no other trace.
    {
        const char* v = std::getenv("HOST_STALL_DUMP_S");
        const int secs = v ? std::atoi(v) : 10;
        if (secs > 0) {
            std::thread([secs] {
                os::set_thread_name("stall watch");
                u64 dumped_for = 0;
                for (int dumps = 0; dumps < 3;) {
                    std::this_thread::sleep_for(std::chrono::seconds{1});
                    const u64 last = video::last_frame_ns();
                    if (!last || last == dumped_for || host_ns() - last < static_cast<u64>(secs) * 1'000'000'000ull) continue;
                    dumped_for = last, dumps++;
                    LOG("[RED] stall: no frame for %d s. Guest threads, then every host thread's stack:", secs);
                    kern::dump_threads();
                    dump_host_stacks();
                }
            }).detach();
        }
    }
    // KART_PGO_DUMP=<s>: write the PGO profile (LLVM_PROFILE_FILE, default default.profraw) after that many
    // seconds. The runtime only writes it at a clean exit, and benchmark runs end by killing the process.
    if (const char* v = std::getenv("KART_PGO_DUMP")) {
        const int secs = std::atoi(v);
        std::thread([secs] {
            std::this_thread::sleep_for(std::chrono::seconds{secs});
#ifdef _WIN32
            LOG("PGO profile written (%d)", __llvm_profile_write_file());
#else
            LOG("PGO profile written (%d)", __llvm_profile_write_file ? __llvm_profile_write_file() : -1);
#endif
        }).detach();
    }
    video::run_window_loop();
    return 0;
}
