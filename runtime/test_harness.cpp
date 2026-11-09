// Differential test harness: runs recompiled functions on cases produced by tools/difftest.py
// and writes the resulting CPU state plus every changed 8-byte word of guest memory.
//
// Usage: test_harness <image file> <image base> <rw offset> <cases.bin> <results.bin>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cpu.h"
#include "table.h"

extern const FnEntry main_table[];
extern const size_t main_table_size;
uint64_t g_base_main;  // load base of the module; generated code addresses are MB + offset
uintptr_t g_membase = 0;  // guest memory is identity-mapped in the harness
void guest_yield(Ctx*) {}

constexpr uint64_t SCRATCH = 0x100000000ull, SCRATCH_SIZE = 0x10000;
constexpr uint64_t STACK = 0x110000000ull, STACK_SIZE = 0x10000;
constexpr DWORD TRAP_CODE = 0xE0000001, UNRESOLVED_CODE = 0xE0000002, INDIRECT_CODE = 0xE0000003;

#pragma pack(push, 1)
struct CaseIn {
    uint64_t fn, seed;
    uint64_t x[31];
    uint64_t sp, nzcv, tpidr;
    V128 v[32];
};
struct CaseOut {
    uint64_t status;
    uint64_t x[31];
    uint64_t sp, nzcv;
    V128 v[32];
    uint64_t ndiff;
};
#pragma pack(pop)

GuestFn lookup_fn(const FnEntry* table, size_t size, uint64_t addr) {
    auto it = std::lower_bound(table, table + size, addr, [](const FnEntry& e, uint64_t a) { return e.addr < a; });
    return it != table + size && it->addr == addr ? it->fn : nullptr;
}

void call_indirect(Ctx*, uint64_t) { RaiseException(INDIRECT_CODE, 0, 0, nullptr); }
void guest_trap(Ctx*, uint64_t, uint32_t) { RaiseException(TRAP_CODE, 0, 0, nullptr); for (;;) {} }
void guest_unresolved_jump(Ctx*, uint64_t, uint64_t) { RaiseException(UNRESOLVED_CODE, 0, 0, nullptr); for (;;) {} }
void guest_svc(Ctx*, uint32_t) { RaiseException(TRAP_CODE, 0, 0, nullptr); }
uint64_t host_cntvct() { return 0; }
uint64_t host_cntfrq() { return 19200000; }
uint32_t crc32_8(uint32_t a, uint64_t) { return a; }
uint32_t crc32_16(uint32_t a, uint64_t) { return a; }
uint32_t crc32_32(uint32_t a, uint64_t) { return a; }
uint32_t crc32_64(uint32_t a, uint64_t) { return a; }

static uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void fill(uint64_t addr, uint64_t size, uint64_t seed) {
    auto* p = reinterpret_cast<uint64_t*>(addr);
    for (uint64_t i = 0; i < size / 8; i++) p[i] = splitmix(seed);
}

static void* map_at(uint64_t addr, uint64_t size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != reinterpret_cast<void*>(addr)) {
        std::fprintf(stderr, "cannot map %llx (+%llx)\n", addr, size);
        std::exit(2);
    }
    return p;
}

static DWORD run_guarded(GuestFn fn, Ctx* c) {
    __try {
        fn(c);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

static void diff_region(uint64_t addr, const uint8_t* before, uint64_t size, std::vector<uint64_t>& out) {
    auto* now = reinterpret_cast<const uint64_t*>(addr);
    auto* old = reinterpret_cast<const uint64_t*>(before);
    for (uint64_t i = 0; i < size / 8; i++) {
        if (now[i] != old[i]) {
            out.push_back(addr + i * 8);
            out.push_back(now[i]);
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: test_harness <image> <base> <rw offset> <cases> <results>\n");
        return 1;
    }
    uint64_t base = std::strtoull(argv[2], nullptr, 0), rw_off = std::strtoull(argv[3], nullptr, 0);
    g_base_main = base;

    FILE* f = std::fopen(argv[1], "rb");
    std::fseek(f, 0, SEEK_END);
    uint64_t image_size = static_cast<uint64_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> pristine(image_size);
    std::fread(pristine.data(), 1, image_size, f);
    std::fclose(f);

    map_at(base, image_size);
    map_at(SCRATCH, SCRATCH_SIZE);
    map_at(STACK, STACK_SIZE);
    std::memcpy(reinterpret_cast<void*>(base), pristine.data(), image_size);

    FILE* in = std::fopen(argv[4], "rb");
    FILE* out = std::fopen(argv[5], "wb");
    std::vector<uint8_t> scratch0(SCRATCH_SIZE), stack0(STACK_SIZE);
    std::vector<uint64_t> diffs;
    CaseIn ci;
    const char* be = std::getenv("HARNESS_BENCH");
    const int bench_n = be ? std::atoi(be) : 0;
    long long bench_ticks = 0, bench_calls = 0;
    while (std::fread(&ci, sizeof ci, 1, in) == 1) {
        std::memcpy(reinterpret_cast<void*>(base + rw_off), pristine.data() + rw_off, image_size - rw_off);
        fill(SCRATCH, SCRATCH_SIZE, ci.seed);
        fill(STACK, STACK_SIZE, ci.seed ^ 0x5555);
        std::memcpy(scratch0.data(), reinterpret_cast<void*>(SCRATCH), SCRATCH_SIZE);
        std::memcpy(stack0.data(), reinterpret_cast<void*>(STACK), STACK_SIZE);

        Ctx c{};
        std::memcpy(c.x, ci.x, sizeof ci.x);
        c.sp = ci.sp;
        std::memcpy(c.v, ci.v, sizeof ci.v);
        set_nzcv(&c, static_cast<unsigned>(ci.nzcv >> 28));
        c.tpidr_el0 = ci.tpidr;
        c.excl_addr = ~0ull;

        CaseOut co{};
        GuestFn fn = lookup_fn(main_table, main_table_size, ci.fn - base);
        const Ctx init = c;
        co.status = fn ? run_guarded(fn, &c) : 1;
        // HARNESS_BENCH=<n>: each case that ran cleanly again n times from the same registers (memory as left)
        if (bench_n && fn && co.status == 0) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            for (int k = 0; k < bench_n; k++) {
                Ctx cb = init;
                run_guarded(fn, &cb);
            }
            QueryPerformanceCounter(&t1);
            bench_ticks += t1.QuadPart - t0.QuadPart, bench_calls += bench_n;
            std::memcpy(reinterpret_cast<void*>(SCRATCH), scratch0.data(), SCRATCH_SIZE);
            std::memcpy(reinterpret_cast<void*>(STACK), stack0.data(), STACK_SIZE);
            Ctx again = init;
            std::memcpy(reinterpret_cast<void*>(base + rw_off), pristine.data() + rw_off, image_size - rw_off);
            co.status = run_guarded(fn, &again);
            c = again;
        }
        std::memcpy(co.x, c.x, sizeof co.x);
        co.sp = c.sp;
        co.nzcv = get_nzcv(&c);
        std::memcpy(co.v, c.v, sizeof co.v);

        diffs.clear();
        diff_region(SCRATCH, scratch0.data(), SCRATCH_SIZE, diffs);
        diff_region(STACK, stack0.data(), STACK_SIZE, diffs);
        diff_region(base + rw_off, pristine.data() + rw_off, image_size - rw_off, diffs);
        co.ndiff = diffs.size() / 2;
        std::fwrite(&co, sizeof co, 1, out);
        std::fwrite(diffs.data(), 8, diffs.size(), out);
    }
    std::fclose(out);
    if (bench_n) {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        std::fprintf(stderr, "bench: %lld calls, %.1f ns per call\n", bench_calls,
                     bench_calls ? 1e9 * bench_ticks / fq.QuadPart / bench_calls : 0.0);
    }
    return 0;
}
