// Guest write watch (watch.h).
#include "watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

#include "kernel.h"
#include "memory.h"

namespace nvn { extern std::atomic<uint64_t> g_frame; }

namespace watch {
namespace {

// The general-purpose register that addresses the memory operand of the x86 store at `code` and that
// can be moved without changing what the instruction does (it is not also a data operand), or -1.
// Recompiled code addresses guest memory as [membase + offset] through ordinary ModRM operands.
int store_address_register(const u8* code) {
    int i = 0;
    bool vex = false;
    u8 rex = 0;
    for (;; i++) {
        const u8 b = code[i];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x66 || b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 ||
            b == 0x36 || b == 0x64 || b == 0x65)
            continue;
        break;
    }
    if ((code[i] & 0xF0) == 0x40) rex = code[i++];
    u8 opcode{};
    bool two_byte = false;
    if (code[i] == 0xC5) {  // two-byte VEX: inverted R in bit 7, then the opcode
        rex = (code[i + 1] & 0x80) ? 0 : 4;
        opcode = code[i + 2];
        i += 3;
        vex = true;
    } else if (code[i] == 0xC4) {  // three-byte VEX: inverted R, X, B in bits 7-5
        const u8 b1 = code[i + 1];
        rex = ((b1 & 0x80) ? 0 : 4) | ((b1 & 0x40) ? 0 : 2) | ((b1 & 0x20) ? 0 : 1);
        opcode = code[i + 3];
        i += 4;
        vex = true;
    } else if (code[i] == 0x62) {
        return -1;  // EVEX: not emitted
    } else if (code[i] == 0x0F) {
        two_byte = true;
        i++;
        if (code[i] == 0x38 || code[i] == 0x3A) i++;
        opcode = code[i++];
    } else {
        opcode = code[i++];
        // String instructions and moffs moves address memory without a ModRM operand.
        if ((opcode >= 0xA0 && opcode <= 0xA7) || (opcode >= 0xAA && opcode <= 0xAF)) return -1;
    }
    const u8 modrm = code[i];
    const u32 mod = modrm >> 6;
    const int reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    if (mod == 3) return -1;
    int base = -1, index = -1;
    u32 scale = 0;
    if ((modrm & 7) == 4) {
        const u8 sib = code[i + 1];
        scale = sib >> 6;
        index = ((sib >> 3) & 7) | ((rex & 2) << 2);
        if (index == 4) index = -1;  // no index
        base = (sib & 7) | ((rex & 1) << 3);
        if ((sib & 7) == 5 && mod == 0) base = -1;  // disp32 only
    } else if ((modrm & 7) == 5 && mod == 0) {
        return -1;  // RIP-relative
    } else {
        base = (modrm & 7) | ((rex & 1) << 3);
    }
    // Registers the instruction reads or writes besides the address: the ModRM reg operand of
    // non-vector forms, RAX for CMPXCHG, RCX for shifts by CL, all of RAX-RDX for CMPXCHG16B.
    const auto conflicts = [&](int r) {
        if (r < 0 || r == 4) return true;  // RSP
        if (!vex && r == reg) return true;
        if (two_byte && (opcode == 0xB0 || opcode == 0xB1) && r == 0) return true;
        if (two_byte && opcode == 0xC7 && r <= 3) return true;
        if (!two_byte && !vex && (opcode == 0xD2 || opcode == 0xD3) && r == 1) return true;
        return false;
    };
    if (!conflicts(base)) return base;
    if (scale == 0 && !conflicts(index)) return index;
    return -1;
}

// The bytes the store at `code` writes to its memory operand, or 0 when the form is not known.
// Covers what MSVC emits for the recompiled code and the CRT: moves, SSE/AVX stores and extracts,
// read-modify-write ALU forms, atomics, SETcc.
u32 store_bytes(const u8* code) {
    int i = 0;
    bool opsize = false, rep_f3 = false, rep_f2 = false;
    for (;; i++) {
        const u8 b = code[i];
        if (b == 0x66) opsize = true;
        else if (b == 0xF3) rep_f3 = true;
        else if (b == 0xF2) rep_f2 = true;
        else if (b == 0xF0 || b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {
        } else break;
    }
    bool w = false;
    if ((code[i] & 0xF0) == 0x40) w = (code[i++] & 8) != 0;
    const u32 word = w ? 8 : (opsize ? 2 : 4);
    if (code[i] == 0xC5 || code[i] == 0xC4) {
        u32 map = 1, pp, l;
        u8 op;
        if (code[i] == 0xC5) {
            pp = code[i + 1] & 3;
            l = (code[i + 1] >> 2) & 1;
            op = code[i + 2];
        } else {
            map = code[i + 1] & 0x1F;
            w = (code[i + 2] & 0x80) != 0;
            pp = code[i + 2] & 3;
            l = (code[i + 2] >> 2) & 1;
            op = code[i + 3];
        }
        const u32 vec = l ? 32 : 16;
        if (map == 1) {
            if (op == 0x11) return pp == 2 ? 4 : pp == 3 ? 8 : vec;  // vmovups/vmovss/vmovsd
            if (op == 0x29 || op == 0x2B || op == 0xE7) return vec;  // vmovaps, vmovntps, vmovntdq
            if (op == 0x7F && (pp == 1 || pp == 2)) return vec;     // vmovdqa/vmovdqu
            if (op == 0x7E && pp == 1) return w ? 8 : 4;           // vmovd/vmovq to memory
            if (op == 0xD6 || op == 0x13 || op == 0x17) return 8;  // vmovq, vmovlps, vmovhps
        } else if (map == 2) {
            if (op == 0x2E || op == 0x2F || op == 0x8E) return vec;  // masked stores (upper bound)
        } else if (map == 3) {
            if (op == 0x14) return 1;
            if (op == 0x15) return 2;
            if (op == 0x16) return w ? 8 : 4;
            if (op == 0x17) return 4;
            if (op == 0x19 || op == 0x39) return 16;  // vextractf128/vextracti128
            if (op == 0x1D) return l ? 16 : 8;        // vcvtps2ph
        }
        return 0;
    }
    if (code[i] == 0x0F) {
        const u8 op = code[i + 1];
        if (op == 0x3A) {
            const u8 op3 = code[i + 2];
            if (op3 == 0x14) return 1;
            if (op3 == 0x15) return 2;
            if (op3 == 0x16) return w ? 8 : 4;
            if (op3 == 0x17) return 4;
            return 0;
        }
        if (op == 0x38) return code[i + 2] == 0xF1 ? word : 0;  // movbe
        if (op == 0x11) return rep_f3 ? 4 : rep_f2 ? 8 : 16;    // movups/movupd/movss/movsd
        if (op == 0x13 || op == 0x17 || op == 0xD6) return 8;   // movlps, movhps, movq
        if (op == 0x29 || op == 0x2B || op == 0xE7) return 16;  // movaps, movntps, movntdq
        if (op == 0x7F) return (opsize || rep_f3) ? 16 : 8;     // movdqa/movdqu, mmx movq
        if (op == 0x7E && opsize) return w ? 8 : 4;             // movd/movq to memory
        if (op == 0xC3) return w ? 8 : 4;                       // movnti
        if (op == 0xB0 || op == 0xC0) return 1;                 // cmpxchg/xadd byte
        if (op == 0xB1 || op == 0xC1 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) return word;
        if (op == 0xC7) return w ? 16 : 8;                      // cmpxchg8b/16b
        if (op >= 0x90 && op <= 0x9F) return 1;                 // setcc
        return 0;
    }
    switch (code[i]) {
    case 0x88: case 0xC6: case 0x86: case 0x80: case 0xFE: case 0xC0: case 0xD0: case 0xD2: case 0xF6:
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30:
        return 1;
    case 0x89: case 0xC7: case 0x87: case 0x81: case 0x83: case 0xFF: case 0xC1: case 0xD1: case 0xD3: case 0xF7:
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31:
        return word;
    default:
        return 0;
    }
}

struct Range {
    u64 lo, hi;
    // For bound(): the contents at the last bind, and the 16-byte units CPU stores wrote since (under g_mu).
    std::vector<u8> snap;
    std::vector<u8> stored;
    u64 binds = 0;
};
constexpr int MaxRanges = 256;
Range g_ranges[MaxRanges];
std::atomic<int> g_nranges{0};
std::mutex g_mu;                 // adding ranges, the per-frame record
std::set<u64> g_pages;           // protected guest pages (under g_mu)
std::atomic<u64> g_page_lo{~0ull}, g_page_hi{0};   // bounds of g_pages, for the fault path's quick test

// A store being single-stepped: either against the page's backing memory (the register addressing it was moved
// by `delta`), or with its pages unprotected for the one instruction (forms that cannot be redirected).
thread_local int t_reg = -1;
thread_local u64 t_delta = 0;
thread_local bool t_unprotected = false;
thread_local u64 t_addr = 0, t_page = 0;
thread_local u32 t_width = 0;
thread_local int t_depth = 0;
thread_local u64 t_stack[10];

struct Agg {
    u32 stores = 0, huge = 0, unknown = 0;
    u64 lo = ~0ull, hi = 0;        // offsets written in the range
    u64 first_huge_off = 0;
    float first_huge = 0;
    u32 tid = 0;
    u64 fpcr = 0;
};
u64 g_rec_frame = 0;
std::map<std::pair<u64, u64>, Agg> g_rec;   // (range lo, stack hash) -> this frame's stores
std::set<u64> g_stacks_logged;
std::atomic<u64> g_lines{0};

u64 max_lines() {
    static const u64 v = [] { const char* e = std::getenv("HOST_WATCH_LINES"); return e ? std::strtoull(e, nullptr, 0) : 200000ull; }();
    return v;
}

void line(const char* fmt, ...) {
    if (g_lines++ >= max_lines()) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("watch: %s", buf);
}

const Range* range_of(u64 a) {
    const int n = g_nranges.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return &g_ranges[i];
    return nullptr;
}

bool protected_page(u64 page) {
    if (page < g_page_lo.load(std::memory_order_relaxed) || page > g_page_hi.load(std::memory_order_relaxed)) return false;
    std::scoped_lock lk{g_mu};
    return g_pages.count(page) != 0;
}

// The host call stack of the faulting store (recompiled guest functions are ordinary host functions calling each other).
int unwind(const CONTEXT* ctx, u64* out, int max) {
    CONTEXT c = *ctx;
    int n = 0;
    while (n < max && c.Rip) {
        out[n++] = c.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fn) break;
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler_data, &frame, nullptr);
    }
    return n;
}

void flush_frame_locked() {
    for (const auto& [key, a] : g_rec) {
        const Range* r = range_of(key.first);
        line("frame %llu: %llx+%llx [%#llx, %#llx) %u stores by stack %016llx (thread %u, fpcr %#llx)%s%s", g_rec_frame,
             key.first, r ? r->hi - r->lo : 0, a.lo, a.hi, a.stores, key.second, a.tid, a.fpcr,
             a.unknown ? " (some of unknown width)" : "", "");
        if (a.huge) line("frame %llu:   %u stores of huge/NaN floats, first %g at +%#llx", g_rec_frame, a.huge, a.first_huge, a.first_huge_off);
    }
    g_rec.clear();
}

// One completed store: [addr, addr + width) now holds its bytes (read through the backing view).
void record(u64 addr, u32 width, bool unknown) {
    const Range* r = range_of(addr);
    if (!r) return;
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < t_depth; i++) h = (h ^ t_stack[i]) * 1099511628211ull;
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    std::scoped_lock lk{g_mu};
    if (frame != g_rec_frame) {
        flush_frame_locked();
        g_rec_frame = frame;
    }
    if (g_stacks_logged.insert(h).second) {
        std::string s;
        for (int i = 0; i < t_depth; i++) {
            if (i) s += " <- ";
            s += host_symbol(t_stack[i]);
        }
        line("stack %016llx: %s", h, s.c_str());
    }
    Agg& a = g_rec[{r->lo, h}];
    a.stores++;
    a.unknown += unknown;
    const u64 off = addr - r->lo;
    auto& stored = const_cast<Range*>(r)->stored;
    for (u64 u = off / 16; u <= (off + width - 1) / 16 && u < stored.size(); u++) stored[u] = 1;
    a.lo = std::min(a.lo, off);
    a.hi = std::max(a.hi, std::min<u64>(off + width, r->hi - r->lo));
    a.tid = GetCurrentThreadId();
    if (kern::KThread* t = kern::current_thread()) a.fpcr = t->ctx.fpcr;
    const u8* p = mem::backing_ptr(addr & ~(mem::PageSize - 1));
    if (!p) return;
    p += addr & (mem::PageSize - 1);
    for (u32 i = 0; i + 4 <= width && (addr & (mem::PageSize - 1)) + i + 4 <= mem::PageSize; i += 4) {
        float f;
        std::memcpy(&f, p + i, 4);
        if (!(std::fabs(f) < 1e8f)) {
            if (!a.huge++) a.first_huge = f, a.first_huge_off = off + i;
        }
    }
}

}  // namespace

bool wanted(const char* shader, u32 slot) {
    static const std::string spec = [] { const char* e = std::getenv("HOST_WATCH_SSBO"); return std::string(e ? e : ""); }();
    if (spec.empty() || !shader) return false;
    char item[64];
    snprintf(item, sizeof item, "%s:%u", shader, slot);
    for (size_t at = spec.find(item); at != std::string::npos; at = spec.find(item, at + 1)) {
        const size_t end = at + std::strlen(item);
        if ((at == 0 || spec[at - 1] == ',') && (end == spec.size() || spec[end] == ',')) return true;
    }
    return false;
}

void add(u64 addr, u64 size) {
    if (!addr || !size || size > (1u << 20)) return;
    std::unique_lock lk{g_mu};
    const int n = g_nranges.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (g_ranges[i].lo == addr && g_ranges[i].hi == addr + size) return;
    if (n == MaxRanges) return;
    g_ranges[n].lo = addr, g_ranges[n].hi = addr + size;
    g_ranges[n].stored.assign((size + 15) / 16, 0);
    g_nranges.store(n + 1, std::memory_order_release);
    for (u64 page = addr / mem::PageSize; page <= (addr + size - 1) / mem::PageSize; page++) {
        if (!g_pages.insert(page).second) continue;
        if (page < g_page_lo) g_page_lo = page;
        if (page > g_page_hi) g_page_hi = page;
        mem::protect_view(page * mem::PageSize, mem::PageSize, true, false);
    }
    lk.unlock();
    std::vector<u64> others;
    const u32 aliases = mem::alias_count(addr & ~(mem::PageSize - 1), &others);
    std::string al;
    for (u64 o : others) {
        char b[24];
        snprintf(b, sizeof b, " %llx", o);
        al += b;
    }
    char albuf[64];
    snprintf(albuf, sizeof albuf, "%u mappings", aliases);
    line("watching %llx+%llx (frame %llu, %d ranges), first page: %s%s", addr, size, nvn::g_frame.load(), n + 1, albuf, al.c_str());
}

void bound(u64 addr) {
    std::scoped_lock lk{g_mu};
    const int n = g_nranges.load(std::memory_order_relaxed);
    Range* r = nullptr;
    for (int i = 0; i < n; i++)
        if (g_ranges[i].lo == addr) r = &g_ranges[i];
    if (!r) return;
    const u64 size = r->hi - r->lo;
    std::vector<u8> now(size);
    for (u64 at = 0; at < size;) {
        const u64 page_left = mem::PageSize - ((addr + at) & (mem::PageSize - 1)), k = std::min(page_left, size - at);
        const u8* p = mem::backing_ptr((addr + at) & ~(mem::PageSize - 1));
        if (!p) return;
        std::memcpy(now.data() + at, p + ((addr + at) & (mem::PageSize - 1)), k);
        at += k;
    }
    r->binds++;
    if (r->snap.size() == size) {
        u32 changed = 0, unexplained = 0;
        std::string where;
        for (u64 u = 0; u * 16 < size; u++) {
            const u64 k = std::min<u64>(16, size - u * 16);
            if (!std::memcmp(now.data() + u * 16, r->snap.data() + u * 16, k)) continue;
            changed++;
            if (r->stored[u]) continue;
            if (unexplained++ < 12) {
                char b[96];
                u32 w[4] = {};
                std::memcpy(w, now.data() + u * 16, k);
                snprintf(b, sizeof b, " +%#llx=%08x %08x %08x %08x", u * 16, w[0], w[1], w[2], w[3]);
                where += b;
            }
        }
        if (unexplained)
            line("bind %llu of %llx (frame %llu): %u units changed, %u without a CPU store:%s", r->binds, addr, nvn::g_frame.load(),
                 changed, unexplained, where.c_str());
    } else {
        u32 w[8];
        std::memcpy(w, now.data(), std::min<u64>(32, size));
        line("first bind of %llx (frame %llu): %08x %08x %08x %08x %08x %08x %08x %08x", addr, nvn::g_frame.load(), w[0], w[1], w[2],
             w[3], w[4], w[5], w[6], w[7]);
    }
    r->snap = std::move(now);
    std::fill(r->stored.begin(), r->stored.end(), 0);
}

void gpu_access(const char* what, const char* shader, u32 slot, u64 addr, u64 size) {
    const int n = g_nranges.load(std::memory_order_acquire);
    if (!n || !addr || !size) return;
    for (int i = 0; i < n; i++) {
        if (addr >= g_ranges[i].hi || addr + size <= g_ranges[i].lo) continue;
        static std::mutex mu;
        static std::map<std::string, u64> seen;
        char key[128];
        snprintf(key, sizeof key, "%s %s %u", what, shader ? shader : "-", slot);
        std::scoped_lock lk{mu};
        const u64 c = ++seen[key];
        if (c <= 3 || (c & (c - 1)) == 0)
            line("gpu %s %s slot %u [%llx, +%llx) overlaps watched %llx+%llx (frame %llu, %llu times)", what, shader ? shader : "-", slot, addr, size,
                 g_ranges[i].lo, g_ranges[i].hi - g_ranges[i].lo, nvn::g_frame.load(), c);
        return;
    }
}

bool on_write_fault(u64 guest, void* context) {
    const u64 page = guest / mem::PageSize;
    if (!protected_page(page) || t_reg >= 0 || t_unprotected) return false;
    auto* ctx = static_cast<CONTEXT*>(context);
    const u8* code = reinterpret_cast<const u8*>(ctx->Rip);
    const u32 width = store_bytes(code);
    const u64 off = guest & (mem::PageSize - 1);
    u8* const backing = mem::backing_ptr(page * mem::PageSize);
    // A store reaching into the next page needs the backing memory to continue there.
    const bool fits = off + std::max<u32>(width, 32) <= mem::PageSize ||
                      mem::backing_ptr((page + 1) * mem::PageSize) == backing + mem::PageSize;
    const int reg = backing && width && fits ? store_address_register(code) : -1;
    t_addr = guest;
    t_width = width ? width : 16;
    t_depth = range_of(guest) || range_of(guest + t_width - 1) ? unwind(ctx, t_stack, 10) : 0;
    if (reg >= 0) {
        t_reg = reg;
        t_delta = reinterpret_cast<u64>(backing + off) - reinterpret_cast<u64>(hp(guest));
        (&ctx->Rax)[reg] += t_delta;
    } else {
        // Unknown form or host code (memcpy): this one instruction runs with the page writable.
        static std::atomic<int> said{0};
        if (said++ < 16) line("store at %s not redirected, page unprotected for one instruction", host_symbol(ctx->Rip).c_str());
        t_unprotected = true;
        t_page = page;
        mem::protect_view(page * mem::PageSize, protected_page(page + 1) ? 2 * mem::PageSize : mem::PageSize, true, true);
    }
    ctx->EFlags |= 0x100u;
    return true;
}

bool on_single_step(void* context) {
    if (t_reg < 0 && !t_unprotected) return false;
    auto* ctx = static_cast<CONTEXT*>(context);
    ctx->EFlags &= ~0x100u;
    const bool unknown = t_unprotected;
    if (t_reg >= 0) {
        (&ctx->Rax)[t_reg] -= t_delta;
        t_reg = -1;
    } else {
        t_unprotected = false;
        std::scoped_lock lk{g_mu};
        for (u64 p : {t_page, t_page + 1})
            if (g_pages.count(p)) mem::protect_view(p * mem::PageSize, mem::PageSize, true, false);
    }
    if (t_depth) record(t_addr, t_width, unknown);
    return true;
}

}  // namespace watch
