"""Advanced SIMD (NEON) decoding for the recompiler.

Every vector operation is emitted as a small lane loop over V128 values: sources are copied
first (so Vd may alias Vn/Vm), lanes are computed with lane<T>() reads, and the result
replaces Vd. 64-bit (Q=0) and scalar forms zero the rest of the register, as the
architecture requires. Lane helpers live in runtime/neon.h.
"""
from a64 import Insn, Unsupported, bits, rx, rw, wr, with_addr, off_c

U = {0: "uint8_t", 1: "uint16_t", 2: "uint32_t", 3: "uint64_t"}
S = {0: "int8_t", 1: "int16_t", 2: "int32_t", 3: "int64_t"}
F = {0: "float", 1: "double"}
FM = {0: "uint32_t", 1: "uint64_t"}  # mask type matching F


def V(n):
    return f"c->v[{n}]"


def lanes(q, size):
    return (16 if q else 8) >> size


def loop(d, srcs, ltype, count, expr, keep=None, offset=0):
    """r[offset + i] = expr for i < count. keep: start from this register instead of zero."""
    decl = " ".join(f"const V128 {k} = {V(v)};" for k, v in srcs.items())
    init = f"V128 r = {keep};" if keep else "V128 r{};"
    idx = f"{offset} + i" if offset else "i"
    return (f"{{ {decl} {init} for (unsigned i = 0; i < {count}; i++) "
            f"setl<{ltype}>(r, {idx}, ({ltype})({expr})); {V(d)} = r; }}")


def L(t, reg, idx="i"):
    return f"lane<{t}>({reg}, {idx})"


def mask(cond, t):
    return f"({cond}) ? ({t})~({t})0 : ({t})0"


# ---------------------------------------------------------------- entry points

def decode_simd(w):
    """Decode an instruction from the 'data processing - SIMD and FP' group that a64.fp_simd did not handle."""
    if bits(w, 31, 31):
        raise Unsupported("simd-reserved")
    scalar = bits(w, 28, 28)
    if not scalar:
        if bits(w, 28, 24) == 0b01110:
            if bits(w, 21, 21):
                k = bits(w, 11, 10)
                if bits(w, 10, 10):
                    return three_same(w, False)
                if k == 0:
                    return three_diff(w)
                if bits(w, 20, 17) == 0b0000:
                    return two_misc(w, False)
                if bits(w, 20, 17) == 0b1000:
                    return across(w)
                raise Unsupported("neon-vector-misc")
            if bits(w, 29, 21) == 0b001110000 and not bits(w, 15, 15) and bits(w, 10, 10):
                return copy(w)
            if bits(w, 29, 21) == 0b101110000 and not bits(w, 15, 15) and bits(w, 10, 10):
                return copy(w)
            if not bits(w, 15, 15) and not bits(w, 10, 10):
                if bits(w, 29, 29) and bits(w, 23, 22) == 0:
                    return ext(w)
                if not bits(w, 29, 29) and bits(w, 11, 10) == 0b10:
                    return permute(w)
                if not bits(w, 29, 29) and bits(w, 11, 10) == 0b00:
                    return tbl(w)
            raise Unsupported("neon-vector-fp16/dot")
        if bits(w, 28, 23) == 0b011110 and bits(w, 10, 10):
            if bits(w, 22, 19) == 0:
                return modified_imm(w)
            return shift_imm(w, False)
        if bits(w, 28, 24) == 0b01111 and not bits(w, 10, 10):
            return by_element(w, False)
        raise Unsupported("neon-vector-other")

    if bits(w, 30, 30) != 1:
        raise Unsupported("simd-reserved")
    if bits(w, 28, 24) == 0b11110:
        if bits(w, 21, 21):
            if bits(w, 10, 10):
                return three_same(w, True)
            if bits(w, 20, 17) == 0b0000 and bits(w, 11, 10) == 0b10:
                return two_misc(w, True)
            if bits(w, 20, 17) == 0b1000 and bits(w, 11, 10) == 0b10:
                return pairwise_scalar(w)
            raise Unsupported("neon-scalar-misc")
        if bits(w, 29, 21) == 0b011110000 and not bits(w, 15, 15) and bits(w, 10, 10):
            return scalar_dup(w)
        raise Unsupported("neon-scalar-other")
    if bits(w, 28, 23) == 0b111110 and bits(w, 10, 10):
        return shift_imm(w, True)
    if bits(w, 28, 24) == 0b11111 and not bits(w, 10, 10):
        return by_element(w, True)
    raise Unsupported("neon-scalar-other")


# ---------------------------------------------------------------- three same

def three_same(w, scalar):
    q, u, size, rm, opcode, rn, rd = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16),
                                      bits(w, 15, 11), bits(w, 9, 5), bits(w, 4, 0))
    if opcode >= 0b11000 or (opcode & 0b11000) == 0b11000:
        return three_same_fp(w, scalar, q, u, size, rm, opcode, rn, rd)
    if opcode == 0b00011:
        if scalar:
            raise Unsupported("neon-scalar-logic")
        return logic(q, u, size, rm, rn, rd)
    n = 1 if scalar else lanes(q, size)
    if scalar and size != 3 and opcode not in (0b00001, 0b00101, 0b10110, 0b01001, 0b01011):
        raise Unsupported("neon-scalar-int-size")
    T, ST = U[size], S[size]
    a, b, dd = L(T, "a"), L(T, "b"), L(T, "dd")
    sa, sb = f"({ST}){a}", f"({ST}){b}"
    srcs = {"a": rn, "b": rm}
    acc = None
    pair = False
    expr = None
    if opcode == 0b10000:
        expr = f"{a} - {b}" if u else f"{a} + {b}"
    elif opcode == 0b10001:
        expr = mask(f"{a} == {b}" if u else f"({a} & {b}) != 0", T)
    elif opcode == 0b00110:
        expr = mask(f"{a} > {b}" if u else f"{sa} > {sb}", T)
    elif opcode == 0b00111:
        expr = mask(f"{a} >= {b}" if u else f"{sa} >= {sb}", T)
    elif opcode == 0b01000:
        expr = f"{'ushl' if u else 'sshl'}_l<{T}>({a}, {b})"
    elif opcode == 0b01010:
        expr = f"{'urshl' if u else 'srshl'}_l<{T}>({a}, {b})"
    elif opcode == 0b01100:
        expr = f"{a} > {b} ? {a} : {b}" if u else f"{sa} > {sb} ? {a} : {b}"
    elif opcode == 0b01101:
        expr = f"{a} < {b} ? {a} : {b}" if u else f"{sa} < {sb} ? {a} : {b}"
    elif opcode in (0b01110, 0b01111):
        cmp = f"{a} > {b}" if u else f"{sa} > {sb}"
        expr = f"({cmp}) ? ({T})({a} - {b}) : ({T})({b} - {a})"
        if opcode == 0b01111:
            expr = f"{dd} + ({T})({expr})"
            acc = rd
    elif opcode == 0b10010:
        expr = f"{dd} {'-' if u else '+'} ({T})({a} * {b})"
        acc = rd
    elif opcode == 0b10011:
        if u:
            raise Unsupported("pmul")
        expr = f"{a} * {b}"
    elif opcode == 0b00000:
        expr = f"{'uhadd' if u else 'shadd'}_l<{T}>({a}, {b})"
    elif opcode == 0b00010:
        expr = f"{'urhadd' if u else 'srhadd'}_l<{T}>({a}, {b})"
    elif opcode == 0b00100:
        expr = f"{'uhsub' if u else 'shsub'}_l<{T}>({a}, {b})"
    elif opcode == 0b00001:
        expr = f"{'uqadd' if u else 'sqadd'}_l<{T}>({a}, {b})"
    elif opcode == 0b00101:
        expr = f"{'uqsub' if u else 'sqsub'}_l<{T}>({a}, {b})"
    elif opcode == 0b10110:
        if size not in (1, 2):
            raise Unsupported("sqdmulh-size")
        expr = f"{'sqrdmulh' if u else 'sqdmulh'}_l<{T}>({a}, {b})"
    elif opcode in (0b10100, 0b10101, 0b10111):
        if scalar:
            raise Unsupported("neon-scalar-pairwise-int")
        pair = True
        if opcode == 0b10111:
            if u:
                raise Unsupported("addp-u")
            fn = "x + y"
        elif opcode == 0b10100:
            fn = "x > y ? x : y" if u else f"({ST})x > ({ST})y ? x : y"
        else:
            fn = "x < y ? x : y" if u else f"({ST})x < ({ST})y ? x : y"
        return Insn(code=[pairwise(rd, rn, rm, T, n, fn)])
    else:
        raise Unsupported(f"neon-3same-{opcode:05b}")
    if acc is not None:
        srcs["dd"] = acc
    return Insn(code=[loop(rd, srcs, T, n, expr)])


def pairwise(rd, rn, rm, T, n, fn):
    half = n // 2
    return (f"{{ const V128 a = {V(rn)}, b = {V(rm)}; V128 r{{}}; for (unsigned i = 0; i < {n}; i++) {{ "
            f"const V128& s = i < {half} ? a : b; unsigned j = (i % {half}) * 2; "
            f"{T} x = lane<{T}>(s, j), y = lane<{T}>(s, j + 1); setl<{T}>(r, i, ({T})({fn})); }} {V(rd)} = r; }}")


def logic(q, u, size, rm, rn, rd):
    n = 2 if q else 1
    a, b, d = "lane<uint64_t>(a, i)", "lane<uint64_t>(b, i)", "lane<uint64_t>(dd, i)"
    if not u:
        expr = {0: f"{a} & {b}", 1: f"{a} & ~{b}", 2: f"{a} | {b}", 3: f"{a} | ~{b}"}[size]
        return Insn(code=[loop(rd, {"a": rn, "b": rm}, "uint64_t", n, expr)])
    expr = {0: f"{a} ^ {b}", 1: f"{b} ^ (({b} ^ {a}) & {d})", 2: f"{d} ^ (({d} ^ {a}) & {b})",
            3: f"{d} ^ (({d} ^ {a}) & ~{b})"}[size]
    return Insn(code=[loop(rd, {"a": rn, "b": rm, "dd": rd}, "uint64_t", n, expr)])


def three_same_fp(w, scalar, q, u, size, rm, opcode, rn, rd):
    sz, o1 = size & 1, size >> 1
    if opcode < 0b11000:
        raise Unsupported("neon-fp16")
    if sz and not q and not scalar:
        raise Unsupported("fp-2d-reserved")
    T, M = F[sz], FM[sz]
    n = 1 if scalar else (4 if q else 2) >> sz
    a, b, dd = L(T, "a"), L(T, "b"), L(T, "dd")
    srcs = {"a": rn, "b": rm}
    key = (u, o1, opcode)
    out_t = T
    pair = None
    if key == (0, 0, 0b11010):
        expr = f"fadd_({a}, {b})"
    elif key == (0, 1, 0b11010):
        expr = f"fsub_({a}, {b})"
    elif key == (1, 0, 0b11011):
        expr = f"fmul_({a}, {b})"
    elif key == (1, 0, 0b11111):
        expr = f"fdiv_({a}, {b})"
    elif key == (0, 0, 0b11110):
        expr = f"fmax_a({a}, {b})"
    elif key == (0, 1, 0b11110):
        expr = f"fmin_a({a}, {b})"
    elif key == (0, 0, 0b11000):
        expr = f"fmaxnm_a({a}, {b})"
    elif key == (0, 1, 0b11000):
        expr = f"fminnm_a({a}, {b})"
    elif key == (0, 0, 0b11001):
        expr, srcs["dd"] = f"fma_({a}, {b}, {dd})", rd
    elif key == (0, 1, 0b11001):
        expr, srcs["dd"] = f"fma_(-{a}, {b}, {dd})", rd
    elif key == (1, 1, 0b11010):
        expr = f"std::fabs(fsub_({a}, {b}))"
    elif key == (0, 0, 0b11011):
        expr = f"fmulx_a({a}, {b})"
    elif key == (0, 0, 0b11111):
        expr = f"frecps_a({a}, {b})"
    elif key == (0, 1, 0b11111):
        expr = f"frsqrts_a({a}, {b})"
    elif key == (0, 0, 0b11100):
        expr, out_t = mask(f"{a} == {b}", M), M
    elif key == (1, 0, 0b11100):
        expr, out_t = mask(f"{a} >= {b}", M), M
    elif key == (1, 1, 0b11100):
        expr, out_t = mask(f"{a} > {b}", M), M
    elif key == (1, 0, 0b11101):
        expr, out_t = mask(f"std::fabs({a}) >= std::fabs({b})", M), M
    elif key == (1, 1, 0b11101):
        expr, out_t = mask(f"std::fabs({a}) > std::fabs({b})", M), M
    elif key == (1, 0, 0b11010):
        pair = "fadd_(x, y)"
    elif key == (1, 0, 0b11110):
        pair = "fmax_a(x, y)"
    elif key == (1, 1, 0b11110):
        pair = "fmin_a(x, y)"
    elif key == (1, 0, 0b11000):
        pair = "fmaxnm_a(x, y)"
    elif key == (1, 1, 0b11000):
        pair = "fminnm_a(x, y)"
    else:
        raise Unsupported(f"neon-3same-fp-{u}{o1}{opcode:05b}")
    if pair:
        if scalar:
            raise Unsupported("fp-pairwise-scalar-3same")
        return Insn(code=[pairwise(rd, rn, rm, T, n, pair)])
    if out_t == M:
        # Compare in the float type, store the mask in the integer type.
        return Insn(code=[loop(rd, srcs, M, n, expr)])
    return Insn(code=[loop(rd, srcs, T, n, expr)])


# ---------------------------------------------------------------- three different

def three_diff(w):
    q, u, size, rm, opcode, rn, rd = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16),
                                      bits(w, 15, 12), bits(w, 9, 5), bits(w, 4, 0))
    if size == 3:
        raise Unsupported("3diff-size")
    n = 8 >> size
    off = n if q else 0
    T, ST = U[size], S[size]
    W, SW = U[size + 1], S[size + 1]

    def ext(reg):
        return f"({W})({SW})({ST}){L(T, reg, f'{off} + i')}" if not u else f"({W}){L(T, reg, f'{off} + i')}"
    a, b = ext("a"), ext("b")
    srcs = {"a": rn, "b": rm}
    if opcode == 0b0000:
        expr = f"{a} + {b}"
    elif opcode == 0b0010:
        expr = f"{a} - {b}"
    elif opcode in (0b0001, 0b0011):
        wa = L(W, "a")
        expr = f"{wa} {'+' if opcode == 1 else '-'} {b}"
    elif opcode in (0b0111, 0b0101):
        cmp = f"{a} > {b}" if u else f"({SW}){a} > ({SW}){b}"
        expr = f"({cmp}) ? ({W})({a} - {b}) : ({W})({b} - {a})"
        if opcode == 0b0101:
            srcs["dd"] = rd
            expr = f"{L(W, 'dd')} + ({W})({expr})"
    elif opcode == 0b1100:
        expr = f"({W})({a} * {b})"
    elif opcode in (0b1000, 0b1010):
        srcs["dd"] = rd
        expr = f"{L(W, 'dd')} {'+' if opcode == 0b1000 else '-'} ({W})({a} * {b})"
    elif opcode in (0b0100, 0b0110):
        # addhn/subhn (raddhn/rsubhn when U): high half of the wide sum, narrowed into Vd.
        wa, wb = L(W, "a"), L(W, "b")
        op = "+" if opcode == 0b0100 else "-"
        rnd = f" + (({W})1 << {(8 << size) - 1})" if u else ""
        expr = f"({T})((({W})({wa} {op} {wb}){rnd}) >> {8 << size})"
        return Insn(code=[loop(rd, srcs, T, n, expr, keep=V(rd) if q else None, offset=off)])
    else:
        raise Unsupported(f"neon-3diff-{opcode:04b}")
    return Insn(code=[loop(rd, srcs, W, n, expr)])


# ---------------------------------------------------------------- two-register misc

def two_misc(w, scalar):
    q, u, size, opcode, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    fp_ops = opcode >= 0b11000 or (size >> 1 and opcode in (0b01100, 0b01101, 0b01110, 0b01111)) or \
        (opcode in (0b10110, 0b10111) and not scalar)
    if fp_ops or (u and size >> 1 and opcode == 0b11111):
        return two_misc_fp(w, scalar, q, u, size, opcode, rn, rd)
    n = 1 if scalar else lanes(q, size)
    T, ST = U[size], S[size]
    a = L(T, "a")
    sa = f"({ST}){a}"
    src = {"a": rn}
    key = (u, opcode)
    if key in ((0, 0b01000), (0, 0b01001), (0, 0b01010), (1, 0b01000), (1, 0b01001)):
        cond = {(0, 0b01000): f"{sa} > 0", (0, 0b01001): f"{a} == 0", (0, 0b01010): f"{sa} < 0",
                (1, 0b01000): f"{sa} >= 0", (1, 0b01001): f"{sa} <= 0"}[key]
        return Insn(code=[loop(rd, src, T, n, mask(cond, T))])
    if key == (0, 0b01011):
        return Insn(code=[loop(rd, src, T, n, f"{sa} < 0 ? ({T})(0 - {a}) : {a}")])
    if key == (1, 0b01011):
        return Insn(code=[loop(rd, src, T, n, f"({T})(0 - {a})")])
    if scalar:
        if opcode in (0b10100, 0b10010) and not (u == 0 and opcode == 0b10010):
            return narrow_sat(w, True, q, u, size, opcode, rn, rd)
        raise Unsupported(f"neon-scalar-2misc-{u}{opcode:05b}")
    if key == (0, 0b00101) and size == 0:
        return Insn(code=[loop(rd, src, T, n, f"__popcnt16({a})")])
    if key == (1, 0b00101):
        if size == 0:
            return Insn(code=[loop(rd, src, T, n, f"~{a}")])
        if size == 1:
            return Insn(code=[loop(rd, src, "uint8_t", lanes(q, 0), f"rbit8({L('uint8_t', 'a')})")])
        raise Unsupported("2misc-reserved")
    if opcode in (0b00000, 0b00001) and not (u and opcode == 1):
        container = {(0, 0): 8, (1, 0): 4, (0, 1): 2}[(u, opcode)]
        if (1 << size) >= container:
            raise Unsupported("rev-reserved")
        group = container >> size  # lanes per container
        return Insn(code=[loop(rd, src, T, n, L(T, "a", f"(i / {group}) * {group} + ({group} - 1 - i % {group})"))])
    if opcode == 0b00100:
        return Insn(code=[loop(rd, src, T, n, f"{'clz' if u else 'cls'}_l<{T}>({a})")])
    if opcode == 0b10010 and not u:  # xtn / xtn2
        if size == 3:
            raise Unsupported("xtn-reserved")
        nn = 8 >> size
        return Insn(code=[loop(rd, src, T, nn, f"({T}){L(U[size + 1], 'a')}", keep=V(rd) if q else None,
                               offset=nn if q else 0)])
    if opcode in (0b10100, 0b10010):
        return narrow_sat(w, False, q, u, size, opcode, rn, rd)
    if opcode in (0b00010, 0b00110):  # [su]addlp / [su]adalp
        if size == 3:
            raise Unsupported("addlp-reserved")
        W = U[size + 1]
        ext = (lambda e: f"({W}){e}") if u else (lambda e: f"({W})({S[size + 1]})({ST}){e}")
        expr = f"{ext(L(T, 'a', '2 * i'))} + {ext(L(T, 'a', '2 * i + 1'))}"
        if opcode == 0b00110:
            src["dd"] = rd
            expr = f"{L(W, 'dd')} + {expr}"
        return Insn(code=[loop(rd, src, W, n // 2, expr)])
    if key == (1, 0b10011):  # shll
        if size == 3:
            raise Unsupported("shll-reserved")
        nn = 8 >> size
        return Insn(code=[loop(rd, src, U[size + 1], nn, f"({U[size + 1]}){L(T, 'a', f'{nn if q else 0} + i')} << {8 << size}")])
    raise Unsupported(f"neon-2misc-{u}{opcode:05b}")


def narrow_sat(w, scalar, q, u, size, opcode, rn, rd):
    """sqxtn / uqxtn / sqxtun (scalar and vector)."""
    if size == 3:
        raise Unsupported("narrow-reserved")
    T, W, SW = U[size], U[size + 1], S[size + 1]
    if opcode == 0b10100:
        fn = f"uqnarrow<{T}>({L(W, 'a')})" if u else f"sqnarrow<{S[size]}>(({SW}){L(W, 'a')})"
    else:  # sqxtun
        fn = f"squnarrow<{T}>(({SW}){L(W, 'a')})"
    if scalar:
        return Insn(code=[loop(rd, {"a": rn}, T, 1, fn)])
    nn = 8 >> size
    return Insn(code=[loop(rd, {"a": rn}, T, nn, fn, keep=V(rd) if q else None, offset=nn if q else 0)])


ROUND = {"n": "R_NEAR", "p": "R_PINF", "m": "R_MINF", "z": "R_ZERO", "a": "R_AWAY"}


def two_misc_fp(w, scalar, q, u, size, opcode, rn, rd):
    sz, hi = size & 1, size >> 1
    if opcode in (0b10110, 0b10111):  # fcvtn / fcvtl (vector only), fcvtxn
        if opcode == 0b10111 and not u:  # fcvtl / fcvtl2
            if sz:
                return Insn(code=[loop(rd, {"a": rn}, "double", 2, f"(double){L('float', 'a', f'{2 if q else 0} + i')}")])
            return Insn(code=[loop(rd, {"a": rn}, "float", 4, f"f16_to_f32({L('uint16_t', 'a', f'{4 if q else 0} + i')})")])
        if opcode == 0b10110 and not u:  # fcvtn / fcvtn2
            if sz:
                return Insn(code=[loop(rd, {"a": rn}, "float", 2, f"(float){L('double', 'a')}", keep=V(rd) if q else None, offset=2 if q else 0)])
            return Insn(code=[loop(rd, {"a": rn}, "uint16_t", 4, f"f32_to_f16({L('float', 'a')})", keep=V(rd) if q else None, offset=4 if q else 0)])
        raise Unsupported("fcvtxn")
    if sz and not q and not scalar:
        raise Unsupported("fp-2d-reserved")
    T, M = F[sz], FM[sz]
    I, SI = U[2 + sz], S[2 + sz]
    n = 1 if scalar else (4 if q else 2) >> sz
    a = L(T, "a")
    src = {"a": rn}
    key = (u, hi, opcode)
    cmps = {(0, 1, 0b01100): ">", (0, 1, 0b01101): "==", (0, 1, 0b01110): "<", (1, 1, 0b01100): ">=", (1, 1, 0b01101): "<="}
    if key in cmps:
        return Insn(code=[loop(rd, src, M, n, mask(f"{a} {cmps[key]} ({T})0", M))])
    simple = {
        (0, 1, 0b01111): f"std::fabs({a})", (1, 1, 0b01111): f"-{a}", (1, 1, 0b11111): f"fsqrt_({a})",
        (0, 0, 0b11000): f"frint_n({a})", (0, 0, 0b11001): f"frint_m({a})", (0, 1, 0b11000): f"frint_p({a})",
        (0, 1, 0b11001): f"frint_z({a})", (1, 0, 0b11000): f"frint_a({a})", (1, 0, 0b11001): f"frint_x({a})",
        (1, 1, 0b11001): f"frint_x({a})",
        (0, 1, 0b11101): f"frecpe_a({a})", (1, 1, 0b11101): f"frsqrte_a({a})",
    }
    if key in simple:
        return Insn(code=[loop(rd, src, T, n, simple[key])])
    if opcode == 0b11101 and not hi:  # scvtf / ucvtf (integer lanes of the same width)
        e = f"({T})({SI}){L(I, 'a')}" if not u else f"({T}){L(I, 'a')}"
        return Insn(code=[loop(rd, src, T, n, e)])
    cvt = {(0, 0b11010): "n", (0, 0b11011): "m", (0, 0b11100): "a", (1, 0b11010): "p", (1, 0b11011): "z"}
    if (hi, opcode) in cvt:
        it = I if u else SI
        return Insn(code=[loop(rd, src, I, n, f"({I})fcvt_int<{it}>({a}, {ROUND[cvt[(hi, opcode)]]})")])
    raise Unsupported(f"neon-2misc-fp-{u}{hi}{opcode:05b}")


# ---------------------------------------------------------------- across lanes, pairwise scalar

def across(w):
    q, u, size, opcode, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    if opcode in (0b01100, 0b01111):
        if not u or size & 1 or not q:
            raise Unsupported("across-fp16/reserved")
        o1 = size >> 1
        fn = {(0b01100, 0): "fmaxnm_a", (0b01100, 1): "fminnm_a", (0b01111, 0): "fmax_a", (0b01111, 1): "fmin_a"}[(opcode, o1)]
        return Insn(code=[f"c->v[{rd}] = V128::f32(vreduce<float>(c->v[{rn}], 4, [](float x, float y) {{ return {fn}(x, y); }}));"])
    if size == 3 or (size == 2 and not q):
        raise Unsupported("across-reserved")
    n = lanes(q, size)
    T, ST = U[size], S[size]
    if opcode == 0b00011:  # saddlv / uaddlv
        W = U[size + 1]
        ext = f"({W})lane<{T}>(a, i)" if u else f"({W})({S[size + 1]})({ST})lane<{T}>(a, i)"
        return Insn(code=[f"{{ const V128 a = c->v[{rn}]; {W} s = 0; for (unsigned i = 0; i < {n}; i++) s += {ext}; c->v[{rd}] = V128::zx(s); }}"])
    fn = {(0, 0b11011): "x + y", (0, 0b01010): f"({ST})x > ({ST})y ? x : y", (1, 0b01010): "x > y ? x : y",
          (0, 0b11010): f"({ST})x < ({ST})y ? x : y", (1, 0b11010): "x < y ? x : y"}.get((u, opcode))
    if fn is None:
        raise Unsupported(f"across-{u}{opcode:05b}")
    return Insn(code=[f"c->v[{rd}] = V128::zx(vreduce<{T}>(c->v[{rn}], {n}, []({T} x, {T} y) {{ return ({T})({fn}); }}));"])


def pairwise_scalar(w):
    u, size, opcode, rn, rd = bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    if not u:
        if opcode == 0b11011 and size == 3:
            return Insn(code=[f"c->v[{rd}] = V128::zx(c->v[{rn}].lo + c->v[{rn}].hi);"])
        raise Unsupported("pairwise-scalar-fp16")
    sz, o1 = size & 1, size >> 1
    T = F[sz]
    fn = {(0b01101, 0): "fadd_(x, y)", (0b01111, 0): "fmax_a(x, y)", (0b01111, 1): "fmin_a(x, y)",
          (0b01100, 0): "fmaxnm_a(x, y)", (0b01100, 1): "fminnm_a(x, y)"}.get((opcode, o1))
    if fn is None:
        raise Unsupported(f"pairwise-scalar-{opcode:05b}")
    ctor = "f64" if sz else "f32"
    return Insn(code=[f"{{ {T} x = lane<{T}>(c->v[{rn}], 0), y = lane<{T}>(c->v[{rn}], 1); c->v[{rd}] = V128::{ctor}({fn}); }}"])


# ---------------------------------------------------------------- copy, permute, ext, tbl

def elem(imm5):
    size = (imm5 & -imm5).bit_length() - 1
    if size > 3:
        raise Unsupported("copy-reserved")
    return size, imm5 >> (size + 1)


def copy(w):
    q, op, imm5, imm4, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 20, 16), bits(w, 14, 11), bits(w, 9, 5), bits(w, 4, 0)
    size, idx = elem(imm5)
    T = U[size]
    if op:  # ins (element)
        src_idx = imm4 >> size
        return Insn(code=[f"{{ {T} t = lane<{T}>(c->v[{rn}], {src_idx}); setl<{T}>(c->v[{rd}], {idx}, t); }}"])
    if imm4 == 0:  # dup (element)
        if size == 3 and not q:
            raise Unsupported("dup-reserved")
        return Insn(code=[f"c->v[{rd}] = vdup<{T}>(lane<{T}>(c->v[{rn}], {idx}), {lanes(q, size)});"])
    if imm4 == 1:  # dup (general)
        if size == 3 and not q:
            raise Unsupported("dup-reserved")
        return Insn(code=[f"c->v[{rd}] = vdup<{T}>(({T}){rx(rn)}, {lanes(q, size)});"])
    if imm4 == 3:  # ins (general)
        return Insn(code=[f"setl<{T}>(c->v[{rd}], {idx}, ({T}){rx(rn)});"])
    if imm4 == 7:  # umov
        return Insn(code=[wr(rd, 1, f"(uint64_t)lane<{T}>(c->v[{rn}], {idx})")])
    if imm4 == 5:  # smov
        e = f"(int64_t)lane<{S[size]}>(c->v[{rn}], {idx})"
        return Insn(code=[wr(rd, q, f"(uint64_t){e}" if q else f"(uint32_t){e}")])
    raise Unsupported("copy-other")


def scalar_dup(w):
    imm5, rn, rd = bits(w, 20, 16), bits(w, 9, 5), bits(w, 4, 0)
    if bits(w, 29, 29) or bits(w, 14, 11):
        raise Unsupported("scalar-copy-reserved")
    size, idx = elem(imm5)
    return Insn(code=[f"c->v[{rd}] = V128::zx(lane<{U[size]}>(c->v[{rn}], {idx}));"])


def permute(w):
    q, size, rm, opcode, rn, rd = bits(w, 30, 30), bits(w, 23, 22), bits(w, 20, 16), bits(w, 14, 12), bits(w, 9, 5), bits(w, 4, 0)
    if size == 3 and not q:
        raise Unsupported("permute-reserved")
    kind = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}.get(opcode)
    if not kind:
        raise Unsupported("permute-reserved")
    return Insn(code=[f"c->v[{rd}] = v{kind}<{U[size]}>(c->v[{rn}], c->v[{rm}], {lanes(q, size)});"])


def ext(w):
    q, rm, imm4, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 11), bits(w, 9, 5), bits(w, 4, 0)
    if not q and imm4 >= 8:
        raise Unsupported("ext-reserved")
    return Insn(code=[f"c->v[{rd}] = vext(c->v[{rn}], c->v[{rm}], {imm4}, {16 if q else 8});"])


def tbl(w):
    q, rm, length, op, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 13), bits(w, 12, 12), bits(w, 9, 5), bits(w, 4, 0)
    regs = ", ".join(V((rn + i) % 32) for i in range(length + 1))
    return Insn(code=[f"{{ const V128 tab[] = {{ {regs} }}; c->v[{rd}] = vtbl(tab, {length + 1}, c->v[{rm}], c->v[{rd}], {16 if q else 8}, {'true' if op else 'false'}); }}"])


# ---------------------------------------------------------------- modified immediate

def expand_imm(op, cmode, imm8):
    c = cmode >> 1
    if c in (0, 1, 2, 3):
        return ("u32", imm8 << (8 * c))
    if c in (4, 5):
        return ("u16", imm8 << (8 * (c & 1)))
    if c == 6:
        return ("u32", (imm8 << 8 | 0xFF) if not cmode & 1 else (imm8 << 16 | 0xFFFF))
    if cmode == 0b1110:
        if not op:
            return ("u8", imm8)
        v = 0
        for i in range(8):
            if imm8 >> i & 1:
                v |= 0xFF << (8 * i)
        return ("u64", v)
    raise ValueError


def fp_imm_bits(imm8, double):
    a, b, cdefgh = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
    if double:
        return a << 63 | (b ^ 1) << 62 | (0xFF if b else 0) << 54 | cdefgh << 48
    return a << 31 | (b ^ 1) << 30 | (0x1F if b else 0) << 25 | cdefgh << 19


def modified_imm(w):
    q, op, cmode, o2, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12), bits(w, 11, 11), bits(w, 4, 0)
    imm8 = bits(w, 18, 16) << 5 | bits(w, 9, 5)
    if o2:
        raise Unsupported("fmov-fp16-imm")
    if cmode == 0b1111:
        if op:
            if not q:
                raise Unsupported("fmov-reserved")
            return Insn(code=[f"c->v[{rd}] = vdup<uint64_t>({fp_imm_bits(imm8, True):#x}ull, 2);"])
        return Insn(code=[f"c->v[{rd}] = vdup<uint32_t>({fp_imm_bits(imm8, False):#x}u, {4 if q else 2});"])
    kind, val = expand_imm(op, cmode, imm8)
    t = {"u8": ("uint8_t", 0), "u16": ("uint16_t", 1), "u32": ("uint32_t", 2), "u64": ("uint64_t", 3)}[kind]
    T, size = t
    n = lanes(q, size)
    is_orrbic = cmode < 0b1100 and cmode & 1
    width = 8 << size
    if is_orrbic:
        if op:  # bic
            return Insn(code=[loop(rd, {"a": rd}, T, n, f"{L(T, 'a')} & ({T}){~val & ((1 << width) - 1):#x}")])
        return Insn(code=[loop(rd, {"a": rd}, T, n, f"{L(T, 'a')} | ({T}){val:#x}")])
    if op and cmode != 0b1110:  # mvni
        val = ~val & ((1 << width) - 1)
    return Insn(code=[f"c->v[{rd}] = vdup<{T}>(({T}){val:#x}ull, {n});"])


# ---------------------------------------------------------------- shift by immediate

def shift_imm(w, scalar):
    q, u, immh, immb, opcode, rn, rd = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 22, 19), bits(w, 18, 16),
                                       bits(w, 15, 11), bits(w, 9, 5), bits(w, 4, 0))
    size = immh.bit_length() - 1
    esize = 8 << size
    imm = immh << 3 | immb
    rsh = 2 * esize - imm
    lsh = imm - esize
    if scalar:
        q = 1
        if size != 3 and opcode not in (0b11100, 0b11111, 0b10010, 0b10011, 0b10000, 0b10001, 0b01110, 0b01100):
            raise Unsupported("scalar-shift-size")
    elif size == 3 and not q:
        raise Unsupported("shift-reserved")
    T, ST = U[size], S[size]
    n = 1 if scalar else lanes(q, size)
    a, d = L(T, "a"), L(T, "dd")
    sa = f"({ST}){a}"
    srcs = {"a": rn}

    def shr(signed):
        if signed:
            return f"({T})({sa} >> {min(rsh, esize - 1)})"
        return f"({T})0" if rsh == esize else f"({T})({a} >> {rsh})"

    def rshr(signed):
        base = f"({T})({sa} >> {min(rsh, esize - 1)})" if signed else (f"({T})0" if rsh == esize else f"({T})({a} >> {rsh})")
        bit = f"(({T})({sa} >> {rsh - 1}) & 1)" if signed else f"(({T})({a} >> {rsh - 1}) & 1)"
        return f"({T})({base} + {bit})"

    if opcode in (0b00000, 0b00010, 0b00100, 0b00110):
        expr = (rshr if opcode & 0b00100 else shr)(not u)
        if opcode & 0b00010:
            srcs["dd"] = rd
            expr = f"{d} + {expr}"
        return Insn(code=[loop(rd, srcs, T, n, expr)])
    if opcode == 0b01010 and not u:
        return Insn(code=[loop(rd, srcs, T, n, f"({T})({a} << {lsh})")])
    if opcode == 0b01010 and u:  # sli
        keep = (1 << lsh) - 1
        srcs["dd"] = rd
        return Insn(code=[loop(rd, srcs, T, n, f"({T})({a} << {lsh}) | ({d} & ({T}){keep:#x})")])
    if opcode == 0b01000 and u:  # sri
        srcs["dd"] = rd
        if rsh == esize:
            return Insn(code=[loop(rd, srcs, T, n, d)])
        keep = ~(((1 << esize) - 1) >> rsh) & ((1 << esize) - 1)
        return Insn(code=[loop(rd, srcs, T, n, f"({T})({a} >> {rsh}) | ({d} & ({T}){keep:#x})")])
    if opcode == 0b10100:  # sshll / ushll
        if size == 3 or scalar:
            raise Unsupported("shll-reserved")
        W, SW = U[size + 1], S[size + 1]
        nn = 8 >> size
        src = L(T, "a", f"{nn if q else 0} + i")
        e = f"({W}){src}" if u else f"({W})({SW})({ST}){src}"
        return Insn(code=[loop(rd, srcs, W, nn, f"({W})({e} << {lsh})")])
    if opcode in (0b10000, 0b10001, 0b10010, 0b10011):  # shrn / rshrn / sqshrn / uqshrn / sqshrun...
        if size == 3:
            raise Unsupported("shrn-reserved")
        W, SW = U[size + 1], S[size + 1]
        rs = 2 * esize - imm
        wa = L(W, "a")
        rnd = opcode & 1
        if (opcode, u) in ((0b10000, 0), (0b10001, 0)):
            val = f"({W})(({wa} >> {rs}) + {'((' + wa + ' >> ' + str(rs - 1) + ') & 1)' if rnd else '0'})"
            fn = f"({T}){val}"
        elif opcode in (0b10010, 0b10011):
            if u:
                val = f"({W})(({wa} >> {rs}) + {'((' + wa + ' >> ' + str(rs - 1) + ') & 1)' if rnd else '0'})"
                fn = f"uqnarrow<{T}>({val})"
            else:
                val = f"(({SW}){wa} >> {rs}) + {'((' + wa + ' >> ' + str(rs - 1) + ') & 1)' if rnd else '0'}"
                fn = f"({T})sqnarrow<{ST}>(({SW})({val}))"
        else:  # sqshrun / sqrshrun
            val = f"(({SW}){wa} >> {rs}) + {'((' + wa + ' >> ' + str(rs - 1) + ') & 1)' if rnd else '0'}"
            fn = f"squnarrow<{T}>(({SW})({val}))"
        if scalar:
            return Insn(code=[loop(rd, srcs, T, 1, fn)])
        nn = 8 >> size
        return Insn(code=[loop(rd, srcs, T, nn, fn, keep=V(rd) if q else None, offset=nn if q else 0)])
    if opcode in (0b11100, 0b11111):  # fixed-point conversions
        if size not in (2, 3):
            raise Unsupported("fixed-fp16")
        Fl = F[size - 2]
        fbits = rsh
        if opcode == 0b11100:
            src = f"({ST}){a}" if not u else a
            return Insn(code=[loop(rd, srcs, Fl, n, f"({Fl})(({Fl}){src} / {float(2 ** fbits)!r})")])
        it = T if u else ST
        return Insn(code=[loop(rd, srcs, T, n, f"({T})fcvt_int<{it}>((double){L(Fl, 'a')} * {float(2 ** fbits)!r}, R_ZERO)")])
    if opcode in (0b01110, 0b01100):  # sqshl / uqshl / sqshlu (immediate)
        if opcode == 0b01100 and not u:
            raise Unsupported("shift-reserved")
        kind = "uqshl" if u and opcode == 0b01110 else ("sqshlu" if opcode == 0b01100 else "sqshl")
        return Insn(code=[loop(rd, srcs, T, n, f"{kind}_imm<{T}>({a}, {lsh})")])
    raise Unsupported(f"shift-imm-{u}{opcode:05b}")


# ---------------------------------------------------------------- by element

def by_element(w, scalar):
    q, u, size, l, m, rm, opcode, h, rn, rd = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 21, 21),
                                               bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 11),
                                               bits(w, 9, 5), bits(w, 4, 0))
    if opcode in (0b0001, 0b0101, 0b1001) and size >> 1:
        sz = size & 1
        if sz and l:
            raise Unsupported("byelem-reserved")
        idx = h if sz else (h << 1 | l)
        reg = m << 4 | rm
        T = F[sz]
        n = 1 if scalar else (4 if q else 2) >> sz
        a, b, d = L(T, "a"), L(T, "b", str(idx)), L(T, "dd")
        srcs = {"a": rn, "b": reg}
        if opcode == 0b1001:
            expr = f"fmulx_a({a}, {b})" if u else f"fmul_({a}, {b})"
        else:
            if u:
                raise Unsupported("byelem-reserved")
            srcs["dd"] = rd
            expr = f"fma_({'-' if opcode == 0b0101 else ''}{a}, {b}, {d})"
        return Insn(code=[loop(rd, srcs, T, n, expr)])
    if size == 1:
        idx, reg = h << 2 | l << 1 | m, rm
    elif size == 2:
        idx, reg = h << 1 | l, m << 4 | rm
    else:
        raise Unsupported("byelem-int-size")
    T, ST = U[size], S[size]
    a, b, d = L(T, "a"), L(T, "b", str(idx)), L(T, "dd")
    srcs = {"a": rn, "b": reg}
    if scalar:
        if opcode in (0b1100, 0b1101) and not u:
            return Insn(code=[loop(rd, srcs, T, 1, f"{'sqrdmulh' if opcode & 1 else 'sqdmulh'}_l<{T}>({a}, {b})")])
        raise Unsupported(f"byelem-scalar-int-{opcode:04b}")
    n = lanes(q, size)
    if opcode == 0b1000 and not u:
        return Insn(code=[loop(rd, srcs, T, n, f"{a} * {b}")])
    if opcode in (0b0000, 0b0100) and u:
        srcs["dd"] = rd
        return Insn(code=[loop(rd, srcs, T, n, f"{d} {'+' if opcode == 0 else '-'} ({T})({a} * {b})")])
    if opcode in (0b1100, 0b1101) and not u:
        return Insn(code=[loop(rd, srcs, T, n, f"{'sqrdmulh' if opcode & 1 else 'sqdmulh'}_l<{T}>({a}, {b})")])
    if opcode in (0b1010, 0b0010, 0b0110):  # [su]mull, [su]mlal, [su]mlsl by element
        W, SW = U[size + 1], S[size + 1]
        nn = 8 >> size
        src_a = L(T, "a", f"{nn if q else 0} + i")
        ea = f"({W}){src_a}" if u else f"({W})({SW})({ST}){src_a}"
        eb = f"({W}){b}" if u else f"({W})({SW})({ST}){b}"
        prod = f"({W})({ea} * {eb})"
        if opcode == 0b1010:
            return Insn(code=[loop(rd, srcs, W, nn, prod)])
        srcs["dd"] = rd
        return Insn(code=[loop(rd, srcs, W, nn, f"{L(W, 'dd')} {'+' if opcode == 0b0010 else '-'} {prod}")])
    raise Unsupported(f"byelem-int-{u}{opcode:04b}")


# ---------------------------------------------------------------- loads and stores

MULTI = {0b0000: (4, 1), 0b0010: (1, 4), 0b0100: (3, 1), 0b0110: (1, 3), 0b0111: (1, 1), 0b1000: (2, 1), 0b1010: (1, 2)}


def ldst_multiple(w):
    q, l, post, rm, opcode, size, rn, rt = (bits(w, 30, 30), bits(w, 22, 22), bits(w, 23, 23), bits(w, 20, 16),
                                           bits(w, 15, 12), bits(w, 11, 10), bits(w, 9, 5), bits(w, 4, 0))
    if opcode not in MULTI:
        raise Unsupported("ldst-multi-reserved")
    selem, rpt = MULTI[opcode]
    if size == 3 and not q and selem > 1:
        raise Unsupported("ldst-multi-reserved")
    T = U[size]
    ebytes = 1 << size
    n = lanes(q, size)
    total = (16 if q else 8) * selem * rpt
    regs = [(rt + i) % 32 for i in range(selem * rpt)]
    parts = []
    if l:
        parts.append(" ".join(f"V128 t{i}{{}};" for i in range(len(regs))))
    for r in range(rpt):
        for e in range(n):
            for s in range(selem):
                reg_i = r * selem + s
                off = ((r * n + e) * selem + s) * ebytes if rpt == 1 else (r * n + e) * ebytes
                if rpt > 1:
                    off = (reg_i * n + e) * ebytes
                if l:
                    parts.append(f"setl<{T}>(t{reg_i}, {e}, ld<{T}>(a + {off}));")
                else:
                    parts.append(f"st<{T}>(a + {off}, lane<{T}>(c->v[{regs[reg_i]}], {e}));")
    if l:
        parts.append(" ".join(f"c->v[{regs[i]}] = t{i};" for i in range(len(regs))))
    body = " ".join(parts)
    return Insn(code=[ldst_addr(rn, post, rm, total, body)])


def ldst_addr(rn, post, rm, total, body):
    base = rx(rn, sp=True)
    if not post:
        return f"{{ uint64_t a = {base}; {body} }}"
    inc = f"{total:#x}ull" if rm == 31 else rx(rm)
    return f"{{ uint64_t a = {base}; {body} {base} = a + {inc}; }}"


def ldst_single(w):
    q, l, post, r, rm, opcode, s, size, rn, rt = (bits(w, 30, 30), bits(w, 22, 22), bits(w, 23, 23), bits(w, 21, 21),
                                                 bits(w, 20, 16), bits(w, 15, 13), bits(w, 12, 12), bits(w, 11, 10),
                                                 bits(w, 9, 5), bits(w, 4, 0))
    selem = ((opcode & 1) << 1 | r) + 1
    scale = opcode >> 1
    regs = [(rt + i) % 32 for i in range(selem)]
    if scale == 3:  # replicate
        if not l or s:
            raise Unsupported("ldr-replicate-reserved")
        T = U[size]
        n = lanes(q, size)
        body = " ".join(f"{V(regs[i])} = vdup<{T}>(ld<{T}>(a + {i << size}), {n});" for i in range(selem))
        return Insn(code=[ldst_addr(rn, post, rm, selem << size, body)])
    if scale == 0:
        idx = q << 3 | s << 2 | size
    elif scale == 1:
        if size & 1:
            raise Unsupported("ldst-single-reserved")
        idx = q << 2 | s << 1 | size >> 1
    elif scale == 2:
        if size & 2:
            raise Unsupported("ldst-single-reserved")
        if size & 1:
            if s:
                raise Unsupported("ldst-single-reserved")
            scale, idx = 3, q
        else:
            idx = q << 1 | s
    T = U[scale]
    eb = 1 << scale
    if l:
        body = " ".join(f"setl<{T}>({V(regs[i])}, {idx}, ld<{T}>(a + {i * eb}));" for i in range(selem))
    else:
        body = " ".join(f"st<{T}>(a + {i * eb}, lane<{T}>({V(regs[i])}, {idx}));" for i in range(selem))
    return Insn(code=[ldst_addr(rn, post, rm, selem * eb, body)])


# ---------------------------------------------------------------- scalar FP <-> fixed point

def fp_fixed(w):
    sf, ftype, rmode, opcode, scale, rn, rd = (bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16),
                                              bits(w, 15, 10), bits(w, 9, 5), bits(w, 4, 0))
    if ftype not in F or (not sf and scale < 32):
        raise Unsupported("fp-fixed-reserved")
    fbits = 64 - scale
    Fl = F[ftype]
    ctor = "f64" if ftype else "f32"
    ut, st = ("uint64_t", "int64_t") if sf else ("uint32_t", "int32_t")
    src = rx(rn) if sf else rw(rn)
    if rmode == 0 and opcode in (2, 3):
        val = f"({st}){src}" if opcode == 2 else src
        return Insn(code=[f"c->v[{rd}] = V128::{ctor}(({Fl})(({Fl}){val} / {float(2 ** fbits)!r}));"])
    if rmode == 3 and opcode in (0, 1):
        it = st if opcode == 0 else ut
        return Insn(code=[wr(rd, sf, f"({ut})fcvt_int<{it}>((double)c->v[{rn}].{'d' if ftype else 'f'}[0] * {float(2 ** fbits)!r}, R_ZERO)")])
    raise Unsupported("fp-fixed-other")
