// Standalone host: loads the game's modules into a native address space and runs the
// recompiled code on host threads, with a native kernel and native services.
#include <windows.h>

#include <chrono>
#include <thread>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "aot_modules.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "memory.h"
#include "video.h"
#include "watch.h"
#include "common/logging.h"
#include "common/windows/timer_resolution.h"

namespace fs = std::filesystem;

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
    // Started from Explorer the console closes with the process: say why (not in hidden test runs).
    if (!std::getenv("HOST_RENDER_HIDDEN")) {
        char text[1024];
        va_start(ap, fmt);
        vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
        MessageBoxA(nullptr, text, "Kart: fatal error", MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
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
}

}  // namespace

std::string host_symbol(u64 rip) {
    static std::mutex m;
    std::scoped_lock lk{m};
    static bool init = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    if (!init) return "?";
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) return "?";
    char out[400];
    snprintf(out, sizeof out, "%s+%#llx", sym->Name, disp);
    return out;
}

namespace {

LONG CALLBACK crash_handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // The store redirected to backing memory (video::on_cpu_write_fault) has run.
    if (code == EXCEPTION_SINGLE_STEP && video::on_single_step(ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
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
    // A guest access to GPU-written memory the CPU has not read yet: download it, then retry.
    if (code == EXCEPTION_ACCESS_VIOLATION && guest < mem::AddressSpaceSize &&
        video::on_read_trap(guest, ep->ExceptionRecord->ExceptionInformation[0] == 1)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // A guest write to a page the GPU caches: drop the GPU's copy, then retry the store.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[0] == 1 &&
        guest < mem::AddressSpaceSize && video::on_cpu_write_fault(guest, ep->ContextRecord)) {
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
    // Not ExitProcess: the Vulkan driver's DLL detach can wait on threads that never return.
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

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
extern "C" int __llvm_profile_write_file(void);
extern "C" int kart_pgo_nop(void) { return 0; }
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=kart_pgo_nop")

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc_start);
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);

    fs::path game = argc > 1 ? fs::path(argv[1]) : fs::path("F:/switch2/Kart/base");
    g_config.game_dir = game;
    g_config.user_dir = fs::current_path() / "user";
    fs::create_directories(g_config.user_dir);
    g_log_file = _wfopen((g_config.user_dir / "host.log").c_str(), L"w");
    LOG("game %s", game.string().c_str());

    if (argc > 2 && std::strcmp(argv[2], "--bake-textures") == 0) return nvk::bake_textures(game, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0);

    AddVectoredExceptionHandler(0, crash_handler);
    // video_core logs through Eglantine's logger (user/log/eden_log.txt under the exe's folder).
    Common::Log::Initialize();
    Common::Log::Start();
    // As Eden's frontend: the finest host timer resolution (0.5 ms), for the GPU thread's acquire
    // polling and the game's short timed waits.
    const auto timer_resolution = Common::Windows::SetCurrentTimerResolutionToMaximum();
    LOG("host timer resolution %lld us", static_cast<long long>(timer_resolution.count() / 1000));
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
    video::start();
    kern::start_main_thread(g_loaded[0].base, stack + stack_size, stack_size);

    // HOST_DUMP_AFTER=<s>: log every guest thread's state after that many seconds (stalls).
    if (const char* v = std::getenv("HOST_DUMP_AFTER")) {
        const int secs = std::atoi(v);
        std::thread([secs] {
            std::this_thread::sleep_for(std::chrono::seconds{secs});
            kern::dump_threads();
        }).detach();
    }
    // KART_PGO_DUMP=<s>: write the PGO profile (LLVM_PROFILE_FILE, default default.profraw) after that many
    // seconds. The runtime only writes it at a clean exit, and benchmark runs end by killing the process.
    if (const char* v = std::getenv("KART_PGO_DUMP")) {
        const int secs = std::atoi(v);
        std::thread([secs] {
            std::this_thread::sleep_for(std::chrono::seconds{secs});
            LOG("PGO profile written (%d)", __llvm_profile_write_file());
        }).detach();
    }
    video::run_window_loop();
    return 0;
}
