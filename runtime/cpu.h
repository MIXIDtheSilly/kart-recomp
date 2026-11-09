// Guest CPU state and the helpers that recompiled code calls.
// Guest memory is identity-mapped: a guest address is a valid host pointer.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <type_traits>

union V128 {
    struct { uint64_t lo, hi; };
    uint8_t b[16];
    uint16_t h[8];
    uint32_t w[4];
    float f[4];
    double d[2];

    static V128 zx(uint64_t v) { V128 r; r.lo = v; r.hi = 0; return r; }
    static V128 f32(float v) { V128 r{}; r.f[0] = v; return r; }
    static V128 f64(double v) { V128 r{}; r.d[0] = v; return r; }
};
static_assert(sizeof(V128) == 16);

struct Ctx {
    uint64_t x[32];  // x[31] is never used: register 31 is sp or zr depending on the instruction
    uint64_t sp;
    alignas(16) V128 v[32];
    uint8_t n, z, cf, vf;
    uint64_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0;
    uint64_t excl_addr, excl_val, excl_val2;
    volatile uint32_t halt;  // set by the runtime to ask the thread to yield (preemption, interrupts)
    void* rt;                // runtime-owned per-thread data
};

using GuestFn = void (*)(Ctx*);

// Provided by the runtime: indirect control flow through the address -> function table.
void call_indirect(Ctx* c, uint64_t target);
[[noreturn]] void guest_trap(Ctx* c, uint64_t pc, uint32_t imm);
void guest_svc(Ctx* c, uint32_t imm);
void guest_yield(Ctx* c);  // called at loop back-edges when c->halt is set
[[noreturn]] void guest_unresolved_jump(Ctx* c, uint64_t from, uint64_t target);
uint64_t host_cntvct();
uint64_t host_cntfrq();

// ------------------------------------------------------------------ game patches (tools/patches.py)

// The frame loop's present-interval mode: -1 = the game's own, 0 = dynamic (from the whole frame's
// time), 1 = every vblank, 2 = every other vblank (30 fps). Set by the runtime before the game runs.
inline int32_t g_mkw_frame_mode = -1;
// The frame loop's object (x19 at that site, set every frame): [+0x214] is the current frame's interval.
inline uint64_t g_mkw_frame_obj = 0;

// ------------------------------------------------------------------ memory

// Host address of guest address 0 (the emulator's fastmem arena; 0 for identity mapping).
extern uintptr_t g_membase;
static inline void* hp(uint64_t a) { return reinterpret_cast<void*>(g_membase + a); }

template <class T> static inline T ld(uint64_t a) { T v; std::memcpy(&v, hp(a), sizeof v); return v; }
template <class T> static inline void st(uint64_t a, T v) { std::memcpy(hp(a), &v, sizeof v); }
// The same with the arena's base passed in: generated code reads g_membase once per function (a store through a
// guest pointer could otherwise be the global itself to the compiler, which then reloads it after every store).
template <class T> static inline T ldm(uintptr_t m, uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<void*>(m + a), sizeof v); return v; }
template <class T> static inline void stm(uintptr_t m, uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(m + a), &v, sizeof v); }

template <class T> static inline T ld_acq(uint64_t a) { return std::atomic_ref<T>(*static_cast<T*>(hp(a))).load(std::memory_order_acquire); }
template <class T> static inline void st_rel(uint64_t a, T v) { std::atomic_ref<T>(*static_cast<T*>(hp(a))).store(v, std::memory_order_release); }

static inline void dc_zva(uint64_t a) { std::memset(hp(a & ~63ull), 0, 64); }

// ------------------------------------------------------------------ atomics (LSE)

template <class T> static inline std::atomic_ref<T> aref(uint64_t a) { return std::atomic_ref<T>(*static_cast<T*>(hp(a))); }
template <class T> static inline T amo_add(uint64_t a, T v) { return aref<T>(a).fetch_add(v); }
template <class T> static inline T amo_clr(uint64_t a, T v) { return aref<T>(a).fetch_and(static_cast<T>(~v)); }
template <class T> static inline T amo_eor(uint64_t a, T v) { return aref<T>(a).fetch_xor(v); }
template <class T> static inline T amo_set(uint64_t a, T v) { return aref<T>(a).fetch_or(v); }
template <class T> static inline T amo_swp(uint64_t a, T v) { return aref<T>(a).exchange(v); }
template <class T, class F> static inline T amo_loop(uint64_t a, F f) {
    auto r = aref<T>(a);
    T old = r.load();
    while (!r.compare_exchange_weak(old, f(old))) {}
    return old;
}
template <class T> static inline T amo_smax(uint64_t a, T v) {
    using S = std::make_signed_t<T>;
    return amo_loop<T>(a, [v](T o) { return static_cast<S>(o) > static_cast<S>(v) ? o : v; });
}
template <class T> static inline T amo_smin(uint64_t a, T v) {
    using S = std::make_signed_t<T>;
    return amo_loop<T>(a, [v](T o) { return static_cast<S>(o) < static_cast<S>(v) ? o : v; });
}
template <class T> static inline T amo_umax(uint64_t a, T v) { return amo_loop<T>(a, [v](T o) { return o > v ? o : v; }); }
template <class T> static inline T amo_umin(uint64_t a, T v) { return amo_loop<T>(a, [v](T o) { return o < v ? o : v; }); }
template <class T> static inline T amo_cas(uint64_t a, T expected, T desired) {
    aref<T>(a).compare_exchange_strong(expected, desired);
    return expected;  // holds the old value either way
}

// 128-bit compare-and-swap; lo/hi receive the old memory contents.
static inline void casp128(uint64_t a, uint64_t& lo, uint64_t& hi, uint64_t new_lo, uint64_t new_hi) {
    alignas(16) int64_t cmp[2] = {static_cast<int64_t>(lo), static_cast<int64_t>(hi)};
    _InterlockedCompareExchange128(static_cast<volatile int64_t*>(hp(a)), static_cast<int64_t>(new_hi), static_cast<int64_t>(new_lo), cmp);
    lo = static_cast<uint64_t>(cmp[0]);
    hi = static_cast<uint64_t>(cmp[1]);
}

// Exclusive monitor emulated with compare-and-swap against the value seen by the load.
template <class T> static inline uint32_t excl_store(Ctx* c, uint64_t a, T v) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull;
    T expected = static_cast<T>(c->excl_val);
    return aref<T>(a).compare_exchange_strong(expected, v) ? 0 : 1;
}
template <class T> static inline void excl_load_pair(Ctx* c, uint64_t a) {
    c->excl_addr = a;
    if constexpr (sizeof(T) == 8) {
        alignas(16) int64_t cmp[2] = {0, 0};
        _InterlockedCompareExchange128(static_cast<volatile int64_t*>(hp(a)), 0, 0, cmp);
        c->excl_val = static_cast<uint64_t>(cmp[0]);
        c->excl_val2 = static_cast<uint64_t>(cmp[1]);
    } else {
        uint64_t both = aref<uint64_t>(a).load(std::memory_order_acquire);
        c->excl_val = static_cast<uint32_t>(both);
        c->excl_val2 = both >> 32;
    }
}
template <class T> static inline uint32_t excl_store_pair(Ctx* c, uint64_t a, uint64_t v0, uint64_t v1) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull;
    if constexpr (sizeof(T) == 8) {
        alignas(16) int64_t cmp[2] = {static_cast<int64_t>(c->excl_val), static_cast<int64_t>(c->excl_val2)};
        return _InterlockedCompareExchange128(static_cast<volatile int64_t*>(hp(a)), static_cast<int64_t>(v1),
                                              static_cast<int64_t>(v0), cmp) ? 0 : 1;
    } else {
        uint64_t expected = static_cast<uint32_t>(c->excl_val) | c->excl_val2 << 32;
        return aref<uint64_t>(a).compare_exchange_strong(expected, static_cast<uint32_t>(v0) | v1 << 32) ? 0 : 1;
    }
}

// ------------------------------------------------------------------ flags

// The flag helpers take a Ctx or, in generated code, a function's local copy of the flags (lift.py --regcache).
struct Flags {
    uint8_t n, z, cf, vf;
};

template <class C> static inline void set_nzcv(C* c, unsigned v) { c->n = (v >> 3) & 1; c->z = (v >> 2) & 1; c->cf = (v >> 1) & 1; c->vf = v & 1; }
template <class C> static inline uint64_t get_nzcv(C* c) { return static_cast<uint64_t>(c->n << 3 | c->z << 2 | c->cf << 1 | c->vf) << 28; }

template <class C> static inline uint64_t adds64(C* c, uint64_t a, uint64_t b, unsigned ci) {
    uint64_t r;
    c->cf = _addcarry_u64(static_cast<unsigned char>(ci), a, b, &r);
    c->n = r >> 63; c->z = r == 0;
    c->vf = ((~(a ^ b) & (a ^ r)) >> 63) & 1;
    return r;
}
template <class C> static inline uint32_t adds32(C* c, uint32_t a, uint32_t b, unsigned ci) {
    unsigned int r;
    c->cf = _addcarry_u32(static_cast<unsigned char>(ci), a, b, &r);
    c->n = r >> 31; c->z = r == 0;
    c->vf = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}
template <class C> static inline uint64_t logic_flags64(C* c, uint64_t r) { c->n = r >> 63; c->z = r == 0; c->cf = 0; c->vf = 0; return r; }
template <class C> static inline uint32_t logic_flags32(C* c, uint32_t r) { c->n = r >> 31; c->z = r == 0; c->cf = 0; c->vf = 0; return r; }

// ------------------------------------------------------------------ integer helpers

static inline uint64_t rotr64(uint64_t v, unsigned s) { return _rotr64(v, static_cast<int>(s)); }
static inline uint32_t rotr32(uint32_t v, unsigned s) { return _rotr(v, static_cast<int>(s)); }
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if (static_cast<int64_t>(b) == -1) return 0 - a;
    return static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>(b));
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if (static_cast<int32_t>(b) == -1) return 0 - a;
    return static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
}
static inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
static inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v); }
static inline uint64_t rev32_64(uint64_t v) { return static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) | static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v >> 32))) << 32; }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
static inline uint64_t rev16_64(uint64_t v) { return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull); }
static inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return bswap64(v);
}
static inline uint32_t rbit32(uint32_t v) { return static_cast<uint32_t>(rbit64(v) >> 32); }
static inline uint64_t clz64(uint64_t v) { return v ? __lzcnt64(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __lzcnt(v) : 32; }
// CLS counts bits after the sign bit that equal it: clz of (x ^ x<<1) over the top N-1 bits.
static inline uint64_t cls64(uint64_t v) { return clz64((v ^ (v << 1)) | 1); }
static inline uint32_t cls32(uint32_t v) { return clz32((v ^ (v << 1)) | 1); }

uint32_t crc32_8(uint32_t acc, uint64_t v);
uint32_t crc32_16(uint32_t acc, uint64_t v);
uint32_t crc32_32(uint32_t acc, uint64_t v);
uint32_t crc32_64(uint32_t acc, uint64_t v);
static inline uint32_t crc32c_8(uint32_t acc, uint64_t v) { return _mm_crc32_u8(acc, static_cast<uint8_t>(v)); }
static inline uint32_t crc32c_16(uint32_t acc, uint64_t v) { return _mm_crc32_u16(acc, static_cast<uint16_t>(v)); }
static inline uint32_t crc32c_32(uint32_t acc, uint64_t v) { return _mm_crc32_u32(acc, static_cast<uint32_t>(v)); }
static inline uint32_t crc32c_64(uint32_t acc, uint64_t v) { return static_cast<uint32_t>(_mm_crc32_u64(acc, v)); }

// ------------------------------------------------------------------ floating point

template <class C> static inline void fcmp_flags(C* c, double a, double b) {
    if (std::isnan(a) || std::isnan(b)) set_nzcv(c, 0b0011);
    else if (a == b) set_nzcv(c, 0b0110);
    else if (a < b) set_nzcv(c, 0b1000);
    else set_nzcv(c, 0b0010);
}

// NaN rules differ between the architectures: x86 produces a negative default NaN and returns the
// first operand's NaN; Arm produces +qNaN and prefers signalling NaNs (FPProcessNaNs). Arithmetic
// therefore runs natively and only takes the slow path when the result is NaN.
template <class T> using FBits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
template <class T> static inline T default_nan() {
    FBits<T> b = sizeof(T) == 4 ? FBits<T>(0x7FC00000u) : FBits<T>(0x7FF8000000000000ull);
    T r; std::memcpy(&r, &b, sizeof r); return r;
}
template <class T> static inline bool is_snan(T v) {
    FBits<T> b; std::memcpy(&b, &v, sizeof v);
    constexpr FBits<T> quiet = FBits<T>(1) << (sizeof(T) == 4 ? 22 : 51);
    return std::isnan(v) && !(b & quiet);
}
template <class T> static inline bool is_qnan(T v) { return std::isnan(v) && !is_snan(v); }
template <class T> static inline T fp_qnan(T a) { return a + a; }  // quiets a signalling NaN, keeps sign and payload
template <class T> static inline T nan2(T a, T b) {
    if (is_snan(a)) return fp_qnan(a);
    if (is_snan(b)) return fp_qnan(b);
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return default_nan<T>();
}
template <class T> static inline T nan3(T acc, T a, T b) {  // FPMulAdd operand order: addend first
    if (is_snan(acc)) return fp_qnan(acc);
    if (is_snan(a)) return fp_qnan(a);
    if (is_snan(b)) return fp_qnan(b);
    if (std::isnan(acc)) {
        bool inf_zero = (std::isinf(a) && b == 0) || (a == 0 && std::isinf(b));
        return inf_zero ? default_nan<T>() : acc;
    }
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return default_nan<T>();
}
template <class T> static inline T fadd_(T a, T b) { T r = a + b; return r == r ? r : nan2(a, b); }
template <class T> static inline T fsub_(T a, T b) { T r = a - b; return r == r ? r : nan2(a, b); }
template <class T> static inline T fmul_(T a, T b) { T r = a * b; return r == r ? r : nan2(a, b); }
template <class T> static inline T fdiv_(T a, T b) { T r = a / b; return r == r ? r : nan2(a, b); }
template <class T> static inline T fma_(T a, T b, T acc) { T r = std::fma(a, b, acc); return r == r ? r : nan3(acc, a, b); }
template <class T> static inline T fsqrt_(T a) {
    if (std::isnan(a)) return fp_qnan(a);
    return a < 0 ? default_nan<T>() : std::sqrt(a);
}

template <class T> static inline T fmax_a(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return nan2(a, b);
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <class T> static inline T fmin_a(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return nan2(a, b);
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
// maxNum/minNum: a quiet NaN loses to a number; signalling NaNs still propagate.
template <class T> static inline T fmaxnm_a(T a, T b) {
    if (is_qnan(a) && !std::isnan(b)) return b;
    if (is_qnan(b) && !std::isnan(a)) return a;
    return fmax_a(a, b);
}
template <class T> static inline T fminnm_a(T a, T b) {
    if (is_qnan(a) && !std::isnan(b)) return b;
    if (is_qnan(b) && !std::isnan(a)) return a;
    return fmin_a(a, b);
}

template <class T> static inline T frint_n(T v) { return std::nearbyint(v); }  // host default rounding is nearest-even
template <class T> static inline T frint_p(T v) { return std::ceil(v); }
template <class T> static inline T frint_m(T v) { return std::floor(v); }
template <class T> static inline T frint_z(T v) { return std::trunc(v); }
template <class T> static inline T frint_a(T v) { return std::round(v); }
template <class T> static inline T frint_x(T v) { return std::nearbyint(v); }

enum Rounding { R_NEAR, R_PINF, R_MINF, R_ZERO, R_AWAY };

template <class I, class F> static inline I fcvt_int(F v, Rounding r) {
    if (std::isnan(v)) return 0;
    double d = static_cast<double>(v);
    switch (r) {
    case R_NEAR: d = std::nearbyint(d); break;
    case R_PINF: d = std::ceil(d); break;
    case R_MINF: d = std::floor(d); break;
    case R_ZERO: d = std::trunc(d); break;
    case R_AWAY: d = std::round(d); break;
    }
    constexpr I imin = (std::numeric_limits<I>::min)(), imax = (std::numeric_limits<I>::max)();
    if (d <= static_cast<double>(imin)) return imin;
    if (d >= static_cast<double>(imax)) return imax;
    return static_cast<I>(d);
}

#include "neon.h"
