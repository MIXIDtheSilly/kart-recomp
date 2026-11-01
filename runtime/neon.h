// Lane helpers for recompiled Advanced SIMD (NEON) code. Included from cpu.h.
#pragma once
#include <immintrin.h>

template <class T> static inline T lane(const V128& v, unsigned i) { T t; std::memcpy(&t, v.b + i * sizeof(T), sizeof(T)); return t; }
template <class T> static inline void setl(V128& v, unsigned i, T x) { std::memcpy(v.b + i * sizeof(T), &x, sizeof(T)); }

template <class T> static inline V128 vdup(T x, unsigned n) {
    V128 r{};
    for (unsigned i = 0; i < n; i++) setl<T>(r, i, x);
    return r;
}

// Architectural Reduce(): pairwise tree over adjacent lanes.
template <class T, class F> static inline T vreduce(const V128& a, unsigned n, F f) {
    T t[16];
    for (unsigned i = 0; i < n; i++) t[i] = lane<T>(a, i);
    for (unsigned w = n; w > 1; w /= 2)
        for (unsigned i = 0; i < w / 2; i++) t[i] = f(t[2 * i], t[2 * i + 1]);
    return t[0];
}

// ------------------------------------------------------------------ permutes

template <class T> static inline T cat(const V128& a, const V128& b, unsigned n, unsigned i) { return i < n ? lane<T>(a, i) : lane<T>(b, i - n); }
template <class T> static inline V128 vuzp1(V128 a, V128 b, unsigned n) { V128 r{}; for (unsigned i = 0; i < n; i++) setl<T>(r, i, cat<T>(a, b, n, 2 * i)); return r; }
template <class T> static inline V128 vuzp2(V128 a, V128 b, unsigned n) { V128 r{}; for (unsigned i = 0; i < n; i++) setl<T>(r, i, cat<T>(a, b, n, 2 * i + 1)); return r; }
template <class T> static inline V128 vzip1(V128 a, V128 b, unsigned n) {
    V128 r{};
    for (unsigned i = 0; i < n / 2; i++) { setl<T>(r, 2 * i, lane<T>(a, i)); setl<T>(r, 2 * i + 1, lane<T>(b, i)); }
    return r;
}
template <class T> static inline V128 vzip2(V128 a, V128 b, unsigned n) {
    V128 r{};
    for (unsigned i = 0; i < n / 2; i++) { setl<T>(r, 2 * i, lane<T>(a, n / 2 + i)); setl<T>(r, 2 * i + 1, lane<T>(b, n / 2 + i)); }
    return r;
}
template <class T> static inline V128 vtrn1(V128 a, V128 b, unsigned n) {
    V128 r{};
    for (unsigned i = 0; i < n / 2; i++) { setl<T>(r, 2 * i, lane<T>(a, 2 * i)); setl<T>(r, 2 * i + 1, lane<T>(b, 2 * i)); }
    return r;
}
template <class T> static inline V128 vtrn2(V128 a, V128 b, unsigned n) {
    V128 r{};
    for (unsigned i = 0; i < n / 2; i++) { setl<T>(r, 2 * i, lane<T>(a, 2 * i + 1)); setl<T>(r, 2 * i + 1, lane<T>(b, 2 * i + 1)); }
    return r;
}
static inline V128 vext(V128 a, V128 b, unsigned pos, unsigned nbytes) {
    V128 r{};
    for (unsigned i = 0; i < nbytes; i++) r.b[i] = pos + i < nbytes ? a.b[pos + i] : b.b[pos + i - nbytes];
    return r;
}
static inline V128 vtbl(const V128* tab, unsigned regs, V128 idx, V128 old, unsigned nbytes, bool tbx) {
    V128 r{};
    for (unsigned i = 0; i < nbytes; i++) {
        unsigned k = idx.b[i];
        if (k < regs * 16) r.b[i] = tab[k / 16].b[k % 16];
        else r.b[i] = tbx ? old.b[i] : 0;
    }
    return r;
}

// ------------------------------------------------------------------ integer lane ops (T is the unsigned lane type)

template <class T> using Sg = std::make_signed_t<T>;
template <class T> constexpr unsigned ebits = sizeof(T) * 8;

static inline uint8_t rbit8(uint8_t v) { return static_cast<uint8_t>(rbit32(v) >> 24); }

template <class T> static inline T clz_l(T v) {
    unsigned n = 0;
    for (int b = ebits<T> - 1; b >= 0 && !((v >> b) & 1); b--) n++;
    return static_cast<T>(n);
}
template <class T> static inline T cls_l(T v) {
    T x = static_cast<T>(v ^ static_cast<T>(v << 1));
    return static_cast<T>(clz_l<T>(static_cast<T>(x | 1)));
}

template <class T> static inline T ushl_l(T a, T b) {
    int s = static_cast<int8_t>(b & 0xFF);
    if (s >= 0) return s >= static_cast<int>(ebits<T>) ? 0 : static_cast<T>(a << s);
    s = -s;
    return s >= static_cast<int>(ebits<T>) ? 0 : static_cast<T>(a >> s);
}
template <class T> static inline T sshl_l(T a, T b) {
    int s = static_cast<int8_t>(b & 0xFF);
    if (s >= 0) return s >= static_cast<int>(ebits<T>) ? 0 : static_cast<T>(a << s);
    s = -s;
    Sg<T> sa = static_cast<Sg<T>>(a);
    return static_cast<T>(sa >> (s >= static_cast<int>(ebits<T>) ? ebits<T> - 1 : s));
}
template <class T> static inline T urshl_l(T a, T b) {
    int s = static_cast<int8_t>(b & 0xFF);
    if (s >= 0) return ushl_l<T>(a, b);
    s = -s;
    if (s > static_cast<int>(ebits<T>)) return 0;
    T base = s == static_cast<int>(ebits<T>) ? 0 : static_cast<T>(a >> s);
    return static_cast<T>(base + ((a >> (s - 1)) & 1));
}
template <class T> static inline T srshl_l(T a, T b) {
    int s = static_cast<int8_t>(b & 0xFF);
    if (s >= 0) return sshl_l<T>(a, b);
    s = -s;
    if (s > static_cast<int>(ebits<T>)) return 0;
    Sg<T> sa = static_cast<Sg<T>>(a);
    Sg<T> base = sa >> (s == static_cast<int>(ebits<T>) ? ebits<T> - 1 : s);
    return static_cast<T>(base + ((sa >> (s - 1)) & 1));
}

template <class T> static inline T uhadd_l(T a, T b) { return static_cast<T>((a >> 1) + (b >> 1) + (a & b & 1)); }
template <class T> static inline T shadd_l(T a, T b) { Sg<T> x = a, y = b; return static_cast<T>((x >> 1) + (y >> 1) + (x & y & 1)); }
template <class T> static inline T urhadd_l(T a, T b) { return static_cast<T>((a >> 1) + (b >> 1) + ((a | b) & 1)); }
template <class T> static inline T srhadd_l(T a, T b) { Sg<T> x = a, y = b; return static_cast<T>((x >> 1) + (y >> 1) + ((x | y) & 1)); }
template <class T> static inline T uhsub_l(T a, T b) { return static_cast<T>((a >> 1) - (b >> 1) - ((~a & b) & 1)); }
template <class T> static inline T shsub_l(T a, T b) { Sg<T> x = a, y = b; return static_cast<T>((x >> 1) - (y >> 1) - ((~x & y) & 1)); }

template <class T> static inline T uqadd_l(T a, T b) { T r = static_cast<T>(a + b); return r < a ? static_cast<T>(~T(0)) : r; }
template <class T> static inline T uqsub_l(T a, T b) { return a < b ? T(0) : static_cast<T>(a - b); }
template <class T> static inline T sqadd_l(T a, T b) {
    T r = static_cast<T>(a + b);
    if (static_cast<Sg<T>>((a ^ r) & (b ^ r)) < 0) return static_cast<Sg<T>>(a) < 0 ? static_cast<T>(T(1) << (ebits<T> - 1)) : static_cast<T>(~T(0) >> 1);
    return r;
}
template <class T> static inline T sqsub_l(T a, T b) {
    T r = static_cast<T>(a - b);
    if (static_cast<Sg<T>>((a ^ b) & (a ^ r)) < 0) return static_cast<Sg<T>>(a) < 0 ? static_cast<T>(T(1) << (ebits<T> - 1)) : static_cast<T>(~T(0) >> 1);
    return r;
}
template <class T> static inline T sqdmulh_l(T a, T b) {
    int64_t p = static_cast<int64_t>(static_cast<Sg<T>>(a)) * static_cast<Sg<T>>(b) * 2;
    if (static_cast<Sg<T>>(a) == (std::numeric_limits<Sg<T>>::min)() && a == b) return static_cast<T>(~T(0) >> 1);
    return static_cast<T>(p >> ebits<T>);
}
template <class T> static inline T sqrdmulh_l(T a, T b) {
    if (static_cast<Sg<T>>(a) == (std::numeric_limits<Sg<T>>::min)() && a == b) return static_cast<T>(~T(0) >> 1);
    int64_t p = static_cast<int64_t>(static_cast<Sg<T>>(a)) * static_cast<Sg<T>>(b) * 2 + (int64_t(1) << (ebits<T> - 1));
    return static_cast<T>(p >> ebits<T>);
}

template <class T, class W> static inline T uqnarrow(W v) { return v > static_cast<W>((std::numeric_limits<T>::max)()) ? (std::numeric_limits<T>::max)() : static_cast<T>(v); }
template <class S, class W> static inline S sqnarrow(W v) {
    if (v > static_cast<W>((std::numeric_limits<S>::max)())) return (std::numeric_limits<S>::max)();
    if (v < static_cast<W>((std::numeric_limits<S>::min)())) return (std::numeric_limits<S>::min)();
    return static_cast<S>(v);
}
template <class T, class W> static inline T squnarrow(W v) {
    if (v < 0) return 0;
    using UW = std::make_unsigned_t<W>;
    return static_cast<UW>(v) > static_cast<UW>((std::numeric_limits<T>::max)()) ? (std::numeric_limits<T>::max)() : static_cast<T>(v);
}

template <class T> static inline T uqshl_imm(T a, unsigned s) {
    if (s == 0) return a;
    return (a >> (ebits<T> - s)) ? static_cast<T>(~T(0)) : static_cast<T>(a << s);
}
template <class T> static inline T sqshl_imm(T a, unsigned s) {
    Sg<T> sa = static_cast<Sg<T>>(a);
    T r = static_cast<T>(a << s);
    if (static_cast<Sg<T>>(static_cast<T>(r)) >> s != sa) return sa < 0 ? static_cast<T>(T(1) << (ebits<T> - 1)) : static_cast<T>(~T(0) >> 1);
    return r;
}
template <class T> static inline T sqshlu_imm(T a, unsigned s) {
    if (static_cast<Sg<T>>(a) < 0) return 0;
    return uqshl_imm<T>(a, s);
}

// ------------------------------------------------------------------ floating point

template <class T> static inline T fmulx_a(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return nan2(a, b);
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return (std::signbit(a) != std::signbit(b)) ? T(-2) : T(2);
    return a * b;
}
// The step instructions negate the first operand before NaN processing (FPRecipStepFused).
template <class T> static inline T frecps_a(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return nan2(-a, b);
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return T(2);
    return std::fma(-a, b, T(2));
}
template <class T> static inline T frsqrts_a(T a, T b) {
    if (std::isnan(a) || std::isnan(b)) return nan2(-a, b);
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return T(1.5);
    return std::fma(-a, b, T(3)) / T(2);
}

static inline unsigned recip_estimate(unsigned a) {
    a = a * 2 + 1;
    unsigned b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline unsigned rsqrt_estimate(unsigned a) {
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    uint64_t b = 512;
    while (uint64_t(a) * (b + 1) * (b + 1) < (uint64_t(1) << 28)) b++;
    return static_cast<unsigned>((b + 1) / 2);
}

// FPRecipEstimate / FPRSqrtEstimate from the Arm ARM, round-to-nearest, FZ off.
template <class T> static inline T frecpe_a(T v) {
    using I = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    constexpr int E = sizeof(T) == 4 ? 8 : 11, Fb = sizeof(T) == 4 ? 23 : 52;
    I bitsv; std::memcpy(&bitsv, &v, sizeof v);
    I sign = bitsv >> (E + Fb);
    if (std::isnan(v)) return v + v;
    if (std::isinf(v)) return sign ? T(-0.0) : T(0.0);
    if (v == 0) return sign ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
    if (std::fabs(v) < (sizeof(T) == 4 ? 0x1p-128 : 0x1p-1024)) return sign ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
    int64_t exp = static_cast<int64_t>((bitsv >> Fb) & ((I(1) << E) - 1));
    uint64_t frac = static_cast<uint64_t>(bitsv & ((I(1) << Fb) - 1)) << (52 - Fb);
    if (exp == 0) {
        if (!((frac >> 51) & 1)) { exp = -1; frac = (frac << 2) & ((uint64_t(1) << 52) - 1); }
        else frac = (frac << 1) & ((uint64_t(1) << 52) - 1);
    }
    unsigned scaled = 256 | static_cast<unsigned>(frac >> 44);
    int64_t rexp = (sizeof(T) == 4 ? 253 : 2045) - exp;
    uint64_t rfrac = static_cast<uint64_t>(recip_estimate(scaled) & 0xFF) << 44;
    if (rexp == 0) rfrac = (uint64_t(1) << 51) | (rfrac >> 1);
    else if (rexp == -1) { rfrac = (uint64_t(1) << 50) | (rfrac >> 2); rexp = 0; }
    I out = (sign << (E + Fb)) | (static_cast<I>(rexp & ((1 << E) - 1)) << Fb) | static_cast<I>(rfrac >> (52 - Fb));
    T r; std::memcpy(&r, &out, sizeof r);
    return r;
}
template <class T> static inline T frsqrte_a(T v) {
    using I = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    constexpr int E = sizeof(T) == 4 ? 8 : 11, Fb = sizeof(T) == 4 ? 23 : 52;
    I bitsv; std::memcpy(&bitsv, &v, sizeof v);
    if (std::isnan(v)) return v + v;
    if (v == 0) return std::signbit(v) ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
    if (std::signbit(v)) return std::numeric_limits<T>::quiet_NaN();
    if (std::isinf(v)) return T(0);
    int64_t exp = static_cast<int64_t>((bitsv >> Fb) & ((I(1) << E) - 1));
    uint64_t frac = static_cast<uint64_t>(bitsv & ((I(1) << Fb) - 1)) << (52 - Fb);
    const uint64_t m52 = (uint64_t(1) << 52) - 1;
    if (exp == 0) {
        while (!((frac >> 51) & 1)) { frac = (frac << 1) & m52; exp--; }
        frac = (frac << 1) & m52;
    }
    unsigned scaled = (exp & 1) ? (128 | static_cast<unsigned>(frac >> 45)) : (256 | static_cast<unsigned>(frac >> 44));
    int64_t rexp = ((sizeof(T) == 4 ? 380 : 3068) - exp) / 2;
    uint64_t est = rsqrt_estimate(scaled) & 0xFF;
    I out = (static_cast<I>(rexp & ((1 << E) - 1)) << Fb) | (static_cast<I>(est) << (Fb - 8));
    T r; std::memcpy(&r, &out, sizeof r);
    return r;
}

static inline float f16_to_f32(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
static inline uint16_t f32_to_f16(float f) {
    return static_cast<uint16_t>(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT), 0));
}
