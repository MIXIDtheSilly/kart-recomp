// Native NVN: calling NVN implementations from recompiled guest code.
//
// An implementation is an ordinary C++ function with NVN's own signature (nvn.h types, host pointers),
// written with the NVN macro:
//
//     NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer* b) { ... }
//
// The macro checks the signature against nvn_protos.h (NVNboolean parameters are uint32_t there: a
// guest caller leaves the upper bits of a bool register undefined, so only the low 8 bits count) and
// registers an adapter that takes the arguments from the guest registers by the AArch64 procedure call
// standard: integers and pointers from x0-x7 in order, floats from v0-v7 in order; the result goes to
// x0 or v0. Guest pointers become host pointers (0 stays nullptr); function pointers (callbacks into the
// game) stay guest addresses, to be called with gfx::guest_call.
#pragma once
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "../common.h"
#include "nvn.h"
#include "nvn_protos.h"

namespace nvn {

// Guest address <-> host pointer for NVN objects in guest memory.
template <class T> inline T* gp(u64 a) { return a ? static_cast<T*>(hp(a)) : nullptr; }
inline u64 ga(const void* p) { return p ? reinterpret_cast<uintptr_t>(p) - g_membase : 0; }

using Adapter = void (*)(Ctx*);
struct Impl {
    const char* name;
    Adapter fn;
};
// Adds an implementation (static initialisers of the NVN macro).
int register_impl(const char* name, Adapter fn);

namespace detail {

template <class T> constexpr bool is_fp = std::is_floating_point_v<T>;
template <class T> constexpr bool is_fnptr = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

// Register index of parameter I among the integer (fp = false) or floating-point parameters.
template <bool Fp, class... A, size_t... J> constexpr int reg_index(size_t i, std::index_sequence<J...>) {
    int n = 0;
    ((J < i && is_fp<A> == Fp ? ++n : 0), ...);
    return n;
}

template <class T> T arg_int(Ctx* c, int r) {
    const u64 v = r < 8 ? c->x[r] : 0;  // NVN has no function with more than 8 integer arguments
    if constexpr (is_fnptr<T>) return reinterpret_cast<T>(static_cast<uintptr_t>(v));
    else if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(v ? g_membase + v : 0);
    else if constexpr (sizeof(T) == 1) return static_cast<T>(static_cast<u8>(v));
    else if constexpr (sizeof(T) == 2) return static_cast<T>(static_cast<u16>(v));
    else if constexpr (sizeof(T) == 4) {
        const u32 w = static_cast<u32>(v);
        T t;
        std::memcpy(&t, &w, 4);
        return t;
    } else {
        T t;
        std::memcpy(&t, &v, 8);
        return t;
    }
}
template <class T> T arg_fp(Ctx* c, int r) {
    if constexpr (sizeof(T) == 4) return c->v[r].f[0];
    else return c->v[r].d[0];
}

template <class R> void put_result(Ctx* c, R r) {
    if constexpr (is_fp<R>) {
        c->v[0] = sizeof(R) == 4 ? V128::f32(static_cast<float>(r)) : V128::f64(static_cast<double>(r));
    } else if constexpr (is_fnptr<R>) {
        c->x[0] = reinterpret_cast<uintptr_t>(r);
    } else if constexpr (std::is_pointer_v<R>) {
        c->x[0] = ga(r);
    } else if constexpr (std::is_enum_v<R> || std::is_integral_v<R>) {
        // Extend as the type does: callers read only the low bits of narrow results.
        c->x[0] = static_cast<u64>(static_cast<std::make_unsigned_t<typename
            std::conditional_t<std::is_enum_v<R>, std::underlying_type<R>, std::type_identity<R>>::type>>(r));
    } else {
        static_assert(sizeof(R) <= 8, "NVN results larger than 8 bytes are not passed in x0");
        u64 v = 0;
        std::memcpy(&v, &r, sizeof r);
        c->x[0] = v;
    }
}

template <auto F> struct Adapt;
template <class R, class... A, R (*F)(A...)> struct Adapt<F> {
    template <size_t... I> static void call(Ctx* c, std::index_sequence<I...> seq) {
        auto get = [c](auto tag, auto idx) {
            using T = typename decltype(tag)::type;
            constexpr size_t i = decltype(idx)::value;
            if constexpr (is_fp<T>) return arg_fp<T>(c, reg_index<true, A...>(i, std::index_sequence_for<A...>{}));
            else return arg_int<T>(c, reg_index<false, A...>(i, std::index_sequence_for<A...>{}));
        };
        if constexpr (std::is_void_v<R>) F(get(std::type_identity<A>{}, std::integral_constant<size_t, I>{})...);
        else put_result<R>(c, F(get(std::type_identity<A>{}, std::integral_constant<size_t, I>{})...));
        (void)seq;
    }
    static void run(Ctx* c) { call(c, std::index_sequence_for<A...>{}); }
};

}  // namespace detail

template <auto F> constexpr Adapter adapt = &detail::Adapt<F>::run;

}  // namespace nvn

// An implementation checked against nvn_protos.h.
#define NVN(name, ret, ...)                                                                        \
    static ret h_##name(__VA_ARGS__);                                                              \
    static_assert(std::is_same_v<decltype(&h_##name), NVNT_##name>, #name ": signature differs from nvn_protos.h"); \
    [[maybe_unused]] static const int r_##name = ::nvn::register_impl(#name, ::nvn::adapt<&h_##name>); \
    static ret h_##name(__VA_ARGS__)
// An NVN2 function without a prototype in nvn_protos.h.
#define NVN_NOPROTO(name, ret, ...)                                                                \
    static ret h_##name(__VA_ARGS__);                                                              \
    [[maybe_unused]] static const int r_##name = ::nvn::register_impl(#name, ::nvn::adapt<&h_##name>); \
    static ret h_##name(__VA_ARGS__)
