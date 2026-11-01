"""AArch64 instruction decoder that emits C++ for the recompiler.

PCs passed to decode() are module-relative; every address the code materialises is emitted
as MB + offset, where MB is the module's load base at runtime (see guest_addr).

decode(word, pc) returns an Insn whose `kind` tells the function lifter how control flows:
  op     straight-line code in `code`
  b      unconditional branch to `target`
  bcond  `code` then branch to `target` if `cond` (C++ expression) holds
  call   direct call to `target` (x30 already set in `code`)
  icall  indirect call through C++ expression `reg`
  ijump  indirect jump through `reg` (switch tables, tail calls through registers)
  ret    return
  trap   brk/udf/unallocated: never returns

Unsupported encodings raise Unsupported with a short class name, which the lifter
counts so missing instructions can be prioritised by how many functions they block.
The generated code relies on the helpers in runtime/cpu.h.
"""
from dataclasses import dataclass, field


class Unsupported(Exception):
    pass


@dataclass
class Insn:
    kind: str = "op"
    code: list = field(default_factory=list)
    target: int = 0
    cond: str = ""
    reg: str = ""
    info: dict = field(default_factory=dict)


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def hexc(v, sf=1):
    v &= (1 << (64 if sf else 32)) - 1
    return f"{v:#x}ull" if sf else f"{v:#x}u"


# ---------------------------------------------------------------- registers

def rx(n, sp=False):
    if n == 31:
        return "c->sp" if sp else "0ull"
    return f"c->x[{n}]"


def rw(n, sp=False):
    if n == 31:
        return "(uint32_t)c->sp" if sp else "0u"
    return f"(uint32_t)c->x[{n}]"


def rd_(n, sf, sp=False):
    return rx(n, sp) if sf else rw(n, sp)


def wr(n, sf, expr, sp=False):
    if n == 31 and not sp:
        return f"(void)({expr});"
    dst = "c->sp" if n == 31 else f"c->x[{n}]"
    return f"{dst} = {expr};" if sf else f"{dst} = (uint32_t)({expr});"


def ut(sf):
    return "uint64_t" if sf else "uint32_t"


def st_(sf):
    return "int64_t" if sf else "int32_t"


CONDS = {
    0: "c->z", 1: "!c->z", 2: "c->cf", 3: "!c->cf", 4: "c->n", 5: "!c->n", 6: "c->vf", 7: "!c->vf",
    8: "(c->cf && !c->z)", 9: "!(c->cf && !c->z)", 10: "(c->n == c->vf)", 11: "(c->n != c->vf)",
    12: "(!c->z && c->n == c->vf)", 13: "!(!c->z && c->n == c->vf)", 14: "true", 15: "true",
}


def shifted(expr, typ, amt, sf):
    if amt == 0:
        return expr
    if typ == 0:
        return f"({ut(sf)})({expr} << {amt})"
    if typ == 1:
        return f"({ut(sf)})({expr} >> {amt})"
    if typ == 2:
        return f"({ut(sf)})(({st_(sf)}){expr} >> {amt})"
    return f"rotr{64 if sf else 32}({expr}, {amt})"


EXTEND = ["(uint8_t)", "(uint16_t)", "(uint32_t)", "(uint64_t)",
          "(int64_t)(int8_t)", "(int64_t)(int16_t)", "(int64_t)(int32_t)", "(uint64_t)"]


def extended(n, option, sh, sf):
    e = f"(uint64_t){EXTEND[option]}{rx(n)}"
    if sh:
        e = f"({e} << {sh})"
    return e if sf else f"(uint32_t)({e})"


def decode_bit_masks(n, imms, immr, immediate, datasize):
    length = (n << 6 | (~imms & 0x3F)).bit_length() - 1
    if length < 1:
        raise Unsupported("bitmask-reserved")
    levels = (1 << length) - 1
    if immediate and (imms & levels) == levels:
        raise Unsupported("bitmask-reserved")
    s, r = imms & levels, immr & levels
    diff = (s - r) & levels
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    telem = (1 << (diff + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1) if r else welem
    wmask = tmask = 0
    for i in range(0, datasize, esize):
        wmask |= welem << i
        tmask |= telem << i
    return wmask, tmask


def mask(width):
    return (1 << width) - 1


# ---------------------------------------------------------------- top level

def decode(w, pc):
    op0 = bits(w, 28, 25)
    if op0 in (0b1000, 0b1001):
        return dp_imm(w, pc)
    if op0 in (0b1010, 0b1011):
        return branch_sys(w, pc)
    if op0 & 0b0101 == 0b0100:
        return load_store(w, pc)
    if op0 & 0b0111 == 0b0101:
        return dp_reg(w)
    if op0 & 0b0111 == 0b0111:
        return fp_simd(w)
    if w >> 16 == 0:
        return Insn("trap", [f"guest_trap(c, {guest_addr(pc)}, 0);"])
    # Unallocated or SVE (absent on this CPU): the hardware would raise an undefined-instruction fault.
    return Insn("trap", [f"guest_trap(c, {guest_addr(pc)}, 0xFFFD);"])


# ---------------------------------------------------------------- data processing (immediate)

def dp_imm(w, pc):
    op1 = bits(w, 25, 23)
    sf = bits(w, 31, 31)
    rd, rn = bits(w, 4, 0), bits(w, 9, 5)
    if op1 in (0, 1):
        imm = sext(bits(w, 23, 5) << 2 | bits(w, 30, 29), 21)
        if sf:
            val = (pc & ~0xFFF) + (imm << 12)
            name = "adrp"
        else:
            val = pc + imm
            name = "adr"
        val &= (1 << 64) - 1
        return Insn(code=[wr(rd, 1, guest_addr(val))], info={"op": name, "rd": rd, "value": val})
    if op1 == 2:
        op, s, sh, imm = bits(w, 30, 30), bits(w, 29, 29), bits(w, 22, 22), bits(w, 21, 10)
        imm <<= 12 if sh else 0
        a = rd_(rn, sf, sp=True)
        info = {"op": "subs" if op and s else ("sub" if op else ("adds" if s else "add")),
                "rd": rd, "rn": rn, "imm": imm, "sf": sf}
        if s:
            b = hexc(~imm if op else imm, sf)
            return Insn(code=[wr(rd, sf, f"adds{64 if sf else 32}(c, {a}, {b}, {op})")], info=info)
        expr = f"{a} {'-' if op else '+'} {hexc(imm, sf)}" if imm else a
        return Insn(code=[wr(rd, sf, expr, sp=True)], info=info)
    if op1 == 3:
        raise Unsupported("addg/subg")
    if op1 == 4:
        opc, n = bits(w, 30, 29), bits(w, 22, 22)
        if not sf and n:
            raise Unsupported("logical-imm-reserved")
        imm, _ = decode_bit_masks(n, bits(w, 15, 10), bits(w, 21, 16), True, 64 if sf else 32)
        a = rd_(rn, sf)
        k = hexc(imm, sf)
        if opc == 3:
            return Insn(code=[wr(rd, sf, f"logic_flags{64 if sf else 32}(c, {a} & {k})")])
        expr = {0: f"{a} & {k}", 1: f"{a} | {k}", 2: f"{a} ^ {k}"}[opc]
        if opc == 1 and rn == 31:
            expr = k
        info = {"op": "orr" if opc == 1 else "logic", "rd": rd}
        if opc == 0:
            info.update(op="and_imm", rn=rn, imm=imm)
        return Insn(code=[wr(rd, sf, expr, sp=True)], info=info)
    if op1 == 5:
        opc, hw, imm16 = bits(w, 30, 29), bits(w, 22, 21), bits(w, 20, 5)
        if opc == 1 or (not sf and hw > 1):
            raise Unsupported("movw-reserved")
        sh = hw * 16
        if opc == 0:
            return Insn(code=[wr(rd, sf, hexc(~(imm16 << sh), sf))])
        if opc == 2:
            return Insn(code=[wr(rd, sf, hexc(imm16 << sh, sf))], info={"op": "movz", "rd": rd, "value": imm16 << sh})
        keep = hexc(~(0xFFFF << sh), sf)
        return Insn(code=[wr(rd, sf, f"({rd_(rd, sf)} & {keep}) | {hexc(imm16 << sh, sf)}")],
                    info={"op": "movk", "rd": rd, "value": imm16 << sh, "mask": 0xFFFF << sh})
    if op1 == 6:
        return bitfield(w, sf, rd, rn)
    # op1 == 7: extr
    rm, imms = bits(w, 20, 16), bits(w, 15, 10)
    size = 64 if sf else 32
    if imms == 0:
        return Insn(code=[wr(rd, sf, rd_(rm, sf))])
    return Insn(code=[wr(rd, sf, f"({ut(sf)})(({rd_(rm, sf)} >> {imms}) | ({rd_(rn, sf)} << {size - imms}))")])


def bitfield(w, sf, rd, rn):
    opc, n, immr, imms = bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10)
    size = 64 if sf else 32
    if opc == 3 or n != sf:
        raise Unsupported("bitfield-reserved")
    U, S = ut(sf), st_(sf)
    src = rd_(rn, sf)
    if opc == 2:  # UBFM
        if imms == size - 1:
            expr = f"{src} >> {immr}" if immr else src
        elif imms + 1 == immr:
            expr = f"({U})({src} << {size - 1 - imms})"
        elif imms < immr:
            expr = f"({U})(({src} & {hexc(mask(imms + 1), sf)}) << {size - immr})"
        else:
            expr = f"({src} >> {immr}) & {hexc(mask(imms - immr + 1), sf)}"
        return Insn(code=[wr(rd, sf, expr)])
    if opc == 0:  # SBFM
        if imms == size - 1:
            expr = f"({U})(({S}){src} >> {immr})"
        elif imms >= immr:
            expr = f"({U})((({S})({U})({src} << {size - 1 - imms})) >> {size - 1 - imms + immr})"
        else:
            width, lsb = imms + 1, size - immr
            expr = f"({U})(((({S})({U})({src} << {size - width})) >> {size - width}) << {lsb})"
        return Insn(code=[wr(rd, sf, expr)])
    # BFM
    dst = rd_(rd, sf)
    if imms >= immr:
        width = imms - immr + 1
        m = mask(width)
        expr = f"({dst} & {hexc(~m, sf)}) | (({src} >> {immr}) & {hexc(m, sf)})"
    else:
        width, lsb = imms + 1, size - immr
        m = mask(width)
        expr = f"({dst} & {hexc(~(m << lsb), sf)}) | ({U})(({src} & {hexc(m, sf)}) << {lsb})"
    return Insn(code=[wr(rd, sf, expr)])


# ---------------------------------------------------------------- data processing (register)

def dp_reg(w):
    sf = bits(w, 31, 31)
    op0, op1, op2, op3 = bits(w, 30, 30), bits(w, 28, 28), bits(w, 24, 21), bits(w, 15, 10)
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    B = 64 if sf else 32
    if not op1:
        if not op2 & 0b1000:  # logical shifted register
            opc, typ, n, amt = bits(w, 30, 29), bits(w, 23, 22), bits(w, 21, 21), bits(w, 15, 10)
            if not sf and amt >= 32:
                raise Unsupported("logical-reserved")
            b = shifted(rd_(rm, sf), typ, amt, sf)
            if n:
                b = f"({ut(sf)})~{b}"
            a = rd_(rn, sf)
            if opc == 1 and rn == 31 and not n:
                expr = b  # mov
            else:
                expr = {0: f"{a} & {b}", 1: f"{a} | {b}", 2: f"{a} ^ {b}", 3: f"{a} & {b}"}[opc]
            if opc == 3:
                expr = f"logic_flags{B}(c, {expr})"
            info = {"op": "mov", "rd": rd, "rm": rm} if opc == 1 and rn == 31 and not n and amt == 0 else {}
            return Insn(code=[wr(rd, sf, expr)], info=info)
        op, s = bits(w, 30, 30), bits(w, 29, 29)
        if not op2 & 1:  # add/sub shifted register
            typ, amt = bits(w, 23, 22), bits(w, 15, 10)
            if typ == 3 or (not sf and amt >= 32):
                raise Unsupported("addsub-reserved")
            b = shifted(rd_(rm, sf), typ, amt, sf)
            a = rd_(rn, sf)
            info = {"op": "addsub_reg", "rd": rd, "rn": rn, "rm": rm, "shift": typ, "amt": amt, "sub": op, "s": s, "sf": sf}
        else:  # add/sub extended register
            if bits(w, 23, 22):
                raise Unsupported("addsub-ext-reserved")
            option, sh = bits(w, 15, 13), bits(w, 12, 10)
            if sh > 4:
                raise Unsupported("addsub-ext-reserved")
            b = extended(rm, option, sh, sf)
            a = rd_(rn, sf, sp=True)
            info = {"op": "addsub_ext", "rd": rd, "rn": rn, "rm": rm, "option": option, "amt": sh, "sub": op, "s": s, "sf": sf}
        if s:
            b2 = f"({ut(sf)})~({b})" if op else b
            return Insn(code=[wr(rd, sf, f"adds{B}(c, {a}, {b2}, {op})")], info=info)
        return Insn(code=[wr(rd, sf, f"{a} {'-' if op else '+'} {b}", sp=bool(op2 & 1))], info=info)

    if op2 == 0b0000:
        if op3 == 0:  # adc/sbc
            op, s = bits(w, 30, 30), bits(w, 29, 29)
            a, b = rd_(rn, sf), rd_(rm, sf)
            if op:
                b = f"({ut(sf)})~{b}"
            if s:
                return Insn(code=[wr(rd, sf, f"adds{B}(c, {a}, {b}, c->cf)")])
            return Insn(code=[wr(rd, sf, f"{a} + {b} + c->cf")])
        raise Unsupported("rmif/setf")
    if op2 == 0b0010:  # conditional compare
        if bits(w, 29, 29) != 1 or bits(w, 10, 10) or bits(w, 4, 4):
            raise Unsupported("ccmp-reserved")
        op, cond, nzcv = bits(w, 30, 30), bits(w, 15, 12), bits(w, 3, 0)
        b = hexc(rm, sf) if bits(w, 11, 11) else rd_(rm, sf)
        if op:
            b = f"({ut(sf)})~{b}"
        return Insn(code=[f"if ({CONDS[cond]}) (void)adds{B}(c, {rd_(rn, sf)}, {b}, {op}); else set_nzcv(c, {nzcv});"])
    if op2 == 0b0100:  # conditional select
        if bits(w, 29, 29) or bits(w, 11, 11):
            raise Unsupported("csel-reserved")
        op, cond, o2 = bits(w, 30, 30), bits(w, 15, 12), bits(w, 10, 10)
        a, b = rd_(rn, sf), rd_(rm, sf)
        alt = {(0, 0): b, (0, 1): f"{b} + 1", (1, 0): f"~{b}", (1, 1): f"0 - {b}"}[(op, o2)]
        return Insn(code=[wr(rd, sf, f"{CONDS[cond]} ? ({ut(sf)})({a}) : ({ut(sf)})({alt})")])
    if op2 == 0b0110:
        if op0 == 0:
            return dp2(w, sf, rd, rn, rm)
        return dp1(w, sf, rd, rn)
    if op2 & 0b1000:
        return dp3(w, sf, rd, rn, rm)
    raise Unsupported("dp-reg-other")


def dp2(w, sf, rd, rn, rm):
    opcode = bits(w, 15, 10)
    B = 64 if sf else 32
    a, b = rd_(rn, sf), rd_(rm, sf)
    if bits(w, 29, 29):
        raise Unsupported("dp2-s")
    if opcode == 2:
        return Insn(code=[wr(rd, sf, f"udiv{B}({a}, {b})")])
    if opcode == 3:
        return Insn(code=[wr(rd, sf, f"sdiv{B}({a}, {b})")])
    if 8 <= opcode <= 11:
        amt = f"({b} & {B - 1})"
        expr = {8: f"{a} << {amt}", 9: f"{a} >> {amt}", 10: f"({ut(sf)})(({st_(sf)}){a} >> {amt})",
                11: f"rotr{B}({a}, (unsigned){amt})"}[opcode]
        return Insn(code=[wr(rd, sf, expr)])
    if opcode == 12:  # pacga
        return Insn(code=[wr(rd, 1, "0ull")])
    if 16 <= opcode <= 23:
        sz = opcode & 3
        castagnoli = opcode & 4
        if (sz == 3) != bool(sf):
            raise Unsupported("crc32-reserved")
        fn = f"crc32{'c' if castagnoli else ''}_{8 << sz}"
        return Insn(code=[wr(rd, 0, f"{fn}({rw(rn)}, {rx(rm)})")])
    raise Unsupported(f"dp2-{opcode}")


def dp1(w, sf, rd, rn):
    opcode2, opcode = bits(w, 20, 16), bits(w, 15, 10)
    a = rd_(rn, sf)
    B = 64 if sf else 32
    if opcode2 == 1:
        # Pointer authentication sign/auth/strip: identity on an emulated system without PAC keys.
        return Insn()
    if opcode2 != 0:
        raise Unsupported("dp1-other")
    if opcode == 0:
        return Insn(code=[wr(rd, sf, f"rbit{B}({a})")])
    if opcode == 1:
        return Insn(code=[wr(rd, sf, f"rev16_{B}({a})")])
    if opcode == 2:
        return Insn(code=[wr(rd, sf, f"rev32_64({a})" if sf else f"bswap32({a})")])
    if opcode == 3 and sf:
        return Insn(code=[wr(rd, sf, f"bswap64({a})")])
    if opcode == 4:
        return Insn(code=[wr(rd, sf, f"clz{B}({a})")])
    if opcode == 5:
        return Insn(code=[wr(rd, sf, f"cls{B}({a})")])
    raise Unsupported(f"dp1-{opcode}")


def dp3(w, sf, rd, rn, rm):
    op54, op31, o0, ra = bits(w, 30, 29), bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10)
    if op54:
        raise Unsupported("dp3-reserved")
    a, b = rd_(rn, sf), rd_(rm, sf)
    acc = rd_(ra, sf)
    sign = "-" if o0 else "+"
    if op31 == 0:
        expr = f"({ut(sf)})({a} * {b})" if ra == 31 and not o0 else f"{acc} {sign} ({ut(sf)})({a} * {b})"
        return Insn(code=[wr(rd, sf, expr)])
    if not sf:
        raise Unsupported("dp3-reserved")
    if op31 == 1:
        prod = f"(uint64_t)((int64_t)(int32_t){rw(rn)} * (int64_t)(int32_t){rw(rm)})"
    elif op31 == 5:
        prod = f"(uint64_t){rw(rn)} * (uint64_t){rw(rm)}"
    elif op31 == 2 and not o0:
        return Insn(code=[wr(rd, 1, f"(uint64_t)__mulh((int64_t){rx(rn)}, (int64_t){rx(rm)})")])
    elif op31 == 6 and not o0:
        return Insn(code=[wr(rd, 1, f"__umulh({rx(rn)}, {rx(rm)})")])
    else:
        raise Unsupported("dp3-other")
    expr = prod if ra == 31 and not o0 else f"{rx(ra)} {sign} {prod}"
    return Insn(code=[wr(rd, 1, expr)])


# ---------------------------------------------------------------- branches, exceptions, system

def branch_sys(w, pc):
    top = bits(w, 31, 29)
    if bits(w, 30, 26) == 0b00101:
        target = (pc + sext(bits(w, 25, 0), 26) * 4) & ((1 << 64) - 1)
        if top == 0b100:
            return Insn("call", [f"c->x[30] = {guest_addr(pc + 4)};"], target=target)
        return Insn("b", target=target)
    if top == 0b010 and bits(w, 25, 24) == 0:
        if bits(w, 4, 4):
            raise Unsupported("bc.cond")
        target = pc + sext(bits(w, 23, 5), 19) * 4
        return Insn("bcond", target=target, cond=CONDS[bits(w, 3, 0)], info={"op": "b.cond", "cc": bits(w, 3, 0)})
    if bits(w, 30, 25) == 0b011010:  # cbz/cbnz
        sf, op, rt = bits(w, 31, 31), bits(w, 24, 24), bits(w, 4, 0)
        target = pc + sext(bits(w, 23, 5), 19) * 4
        return Insn("bcond", target=target, cond=f"{rd_(rt, sf)} {'!=' if op else '=='} 0")
    if bits(w, 30, 25) == 0b011011:  # tbz/tbnz
        bit = bits(w, 31, 31) << 5 | bits(w, 23, 19)
        op, rt = bits(w, 24, 24), bits(w, 4, 0)
        target = pc + sext(bits(w, 18, 5), 14) * 4
        return Insn("bcond", target=target, cond=f"(({rx(rt)} >> {bit}) & 1) {'!=' if op else '=='} 0")
    if bits(w, 31, 24) == 0b11010100:
        opc, ll = bits(w, 23, 21), bits(w, 1, 0)
        imm = bits(w, 20, 5)
        if opc == 0 and ll == 1:
            return Insn(code=[f"guest_svc(c, {imm});"])
        return Insn("trap", [f"guest_trap(c, {guest_addr(pc)}, {imm});"])
    if bits(w, 31, 22) == 0b1101010100:
        return system(w, pc)
    if bits(w, 31, 25) == 0b1101011:
        opc, op2, op3, rn, op4 = bits(w, 24, 21), bits(w, 20, 16), bits(w, 15, 10), bits(w, 9, 5), bits(w, 4, 0)
        if op2 != 0x1F:
            raise Unsupported("br-reserved")
        if opc == 2 and (op3 == 0 or op3 in (2, 3)):
            return Insn("ret")
        if opc in (0, 8) and op3 in (0, 2, 3):
            return Insn("ijump", reg=rx(rn), info={"rn": rn})
        if opc in (1, 9) and op3 in (0, 2, 3):
            return Insn("icall", [f"uint64_t t = {rx(rn)};", f"c->x[30] = {guest_addr(pc + 4)};"], reg="t")
        raise Unsupported(f"br-{opc}-{op3}")
    raise Unsupported("branch-other")


SYSREG = {
    (3, 3, 13, 0, 2): "c->tpidr_el0",
    (3, 3, 13, 0, 3): "c->tpidrro_el0",
    (3, 3, 4, 4, 0): "c->fpcr",
    (3, 3, 4, 4, 1): "c->fpsr",
}


def system(w, pc):
    l, op0, op1, crn, crm, op2, rt = (bits(w, 21, 21), bits(w, 20, 19), bits(w, 18, 16), bits(w, 15, 12),
                                      bits(w, 11, 8), bits(w, 7, 5), bits(w, 4, 0))
    if op0 == 0 and not l:
        if crn == 2:
            return Insn()  # hints: nop, yield, wfe, sev, pac*sp, aut*sp, bti, ...
        if crn == 3:
            if op2 == 2:
                return Insn(code=["c->excl_addr = ~0ull;"])  # clrex
            if op2 in (4, 5):
                return Insn(code=["std::atomic_thread_fence(std::memory_order_seq_cst);"])
            if op2 == 6:
                return Insn()  # isb
        raise Unsupported("msr-imm")
    if op0 == 1 and not l:
        if (op1, crn, crm, op2) == (3, 7, 4, 1):
            return Insn(code=[f"dc_zva({rx(rt)});"])
        return Insn()  # cache maintenance
    key = (op0, op1, crn, crm, op2)
    if key in SYSREG:
        reg = SYSREG[key]
        if l:
            return Insn(code=[wr(rt, 1, reg)])
        if key == (3, 3, 13, 0, 3):
            raise Unsupported("msr-tpidrro")
        return Insn(code=[f"{reg} = {rx(rt)};"])
    if key == (3, 3, 4, 2, 0):
        return Insn(code=[wr(rt, 1, "get_nzcv(c)")] if l else [f"set_nzcv(c, (unsigned)({rx(rt)} >> 28));"])
    if l and key in ((3, 3, 14, 0, 2), (3, 3, 14, 0, 1)):  # cntvct_el0 / cntpct_el0
        return Insn(code=[wr(rt, 1, "host_cntvct()")])
    if l and key == (3, 3, 14, 0, 0):
        return Insn(code=[wr(rt, 1, "host_cntfrq()")])
    if l and key == (3, 3, 0, 0, 7):
        return Insn(code=[wr(rt, 1, "4ull")])  # dczid: 64-byte dc zva
    if l and key == (3, 3, 0, 0, 1):
        return Insn(code=[wr(rt, 1, "0x8444c004ull")])  # ctr_el0
    raise Unsupported(f"sysreg-{'mrs' if l else 'msr'}-{op0}.{op1}.{crn}.{crm}.{op2}")


# ---------------------------------------------------------------- loads and stores

INT_T = {0: "uint8_t", 1: "uint16_t", 2: "uint32_t", 3: "uint64_t"}
SINT_T = {0: "int8_t", 1: "int16_t", 2: "int32_t", 3: "int64_t"}


def vreg_load(t, size_log2, addr):
    if size_log2 == 4:
        return f"c->v[{t}] = ld<V128>({addr});"
    return f"c->v[{t}] = V128::zx(ld<{INT_T[size_log2]}>({addr}));"


def vreg_store(t, size_log2, addr):
    if size_log2 == 4:
        return f"st<V128>({addr}, c->v[{t}]);"
    return f"st<{INT_T[size_log2]}>({addr}, ({INT_T[size_log2]})c->v[{t}].lo);"


def int_load(t, size, opc, addr):
    """opc: 1 zero-extend, 2 sign-extend to 64, 3 sign-extend to 32."""
    if opc == 1:
        return wr(t, 1, f"(uint64_t)ld<{INT_T[size]}>({addr})")
    if opc == 2:
        return wr(t, 1, f"(uint64_t)(int64_t)ld<{SINT_T[size]}>({addr})")
    return wr(t, 0, f"(uint32_t)(int32_t)ld<{SINT_T[size]}>({addr})")


def int_store(t, size, addr):
    return f"st<{INT_T[size]}>({addr}, ({INT_T[size]}){rx(t)});"


def with_addr(base_reg, offset, mode, access):
    """mode: 'off' plain offset, 'pre' pre-index writeback, 'post' post-index writeback."""
    base = rx(base_reg, sp=True)
    if mode == "off":
        addr = f"{base} + {offset}" if offset else base
        return [f"{{ uint64_t a = {addr}; {access('a')} }}"]
    if mode == "pre":
        return [f"{{ uint64_t a = {base} + {offset}; {access('a')} {base} = a; }}"]
    return [f"{{ uint64_t a = {base}; {access('a')} {base} = a + {offset}; }}"]


def guest_addr(off):
    """A code/data address inside the module: pcs are module-relative, MB is the runtime load base."""
    return f"(MB + {off & ((1 << 64) - 1):#x}ull)"


def off_c(v):
    return f"{v & ((1 << 64) - 1):#x}ull"


def load_store(w, pc):
    op0 = bits(w, 31, 28)
    v = bits(w, 26, 26)
    if bits(w, 29, 24) == 0b001000 and not v:
        return exclusive(w)
    if bits(w, 29, 27) == 0b011 and not bits(w, 24, 24):
        opc, t = bits(w, 31, 30), bits(w, 4, 0)
        addr = guest_addr(pc + sext(bits(w, 23, 5), 19) * 4)
        if v:
            if opc == 3:
                raise Unsupported("ldr-lit-reserved")
            return Insn(code=[vreg_load(t, 2 + opc, addr)])
        if opc == 3:
            return Insn()  # prfm
        if opc == 2:
            return Insn(code=[int_load(t, 2, 2, addr)])
        return Insn(code=[int_load(t, 2 + opc, 1, addr)])
    if bits(w, 29, 27) == 0b101:
        return load_pair(w)
    if bits(w, 29, 27) == 0b111:
        return load_reg(w)
    if bits(w, 31, 31) == 0 and bits(w, 29, 24) == 0b001100 and v:
        return neon.ldst_multiple(w)
    if bits(w, 31, 31) == 0 and bits(w, 29, 24) == 0b001101 and v:
        return neon.ldst_single(w)
    if bits(w, 29, 24) == 0b011001 and not v:
        raise Unsupported("ldapr-unscaled")
    raise Unsupported(f"ldst-other-{op0:x}")


def load_pair(w):
    opc, v, mode, l = bits(w, 31, 30), bits(w, 26, 26), bits(w, 25, 23), bits(w, 22, 22)
    imm7, t2, n, t = sext(bits(w, 21, 15), 7), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0)
    if mode > 3:
        raise Unsupported("ldst-pair-other")
    m = {0: "off", 1: "post", 2: "off", 3: "pre"}[mode]
    if v:
        if opc == 3:
            raise Unsupported("ldp-v-reserved")
        size = 2 + opc
        scale = 1 << size
        off = imm7 * scale
        if l:
            acc = lambda a: f"{{ V128 r0, r1; {vtmp_load('r0', size, a)} {vtmp_load('r1', size, f'{a} + {scale}')} c->v[{t}] = r0; c->v[{t2}] = r1; }}"
        else:
            acc = lambda a: f"{vreg_store(t, size, a)} {vreg_store(t2, size, f'{a} + {scale}')}"
        return Insn(code=with_addr(n, off_c(off), m, acc))
    if opc == 3 or (opc == 1 and not l):
        raise Unsupported("ldp-reserved/stgp")
    sf = opc >> 1
    size = 3 if sf else 2
    scale = 1 << size
    off = imm7 * scale
    if l:
        if opc == 1:
            ld = lambda a: f"(uint64_t)(int64_t)ld<int32_t>({a})"
        else:
            ld = lambda a: f"(uint64_t)ld<{INT_T[size]}>({a})"
        def acc(a):
            s = f"uint64_t r0 = {ld(a)}, r1 = {ld(f'{a} + {scale}')};"
            return f"{s} {wr(t, 1, 'r0')} {wr(t2, 1, 'r1')}"
    else:
        acc = lambda a: f"{int_store(t, size, a)} {int_store(t2, size, f'{a} + {scale}')}"
    return Insn(code=with_addr(n, off_c(off), m, acc), info={"op": "ldp" if l else "stp"})


def vtmp_load(tmp, size_log2, addr):
    if size_log2 == 4:
        return f"{tmp} = ld<V128>({addr});"
    return f"{tmp} = V128::zx(ld<{INT_T[size_log2]}>({addr}));"


def load_reg(w):
    size, v, opc = bits(w, 31, 30), bits(w, 26, 26), bits(w, 23, 22)
    n, t = bits(w, 9, 5), bits(w, 4, 0)
    if bits(w, 24, 24):
        mode, off = "off", bits(w, 21, 10)
        scaled = True
    elif not bits(w, 21, 21):
        kind = bits(w, 11, 10)
        off = sext(bits(w, 20, 12), 9)
        scaled = False
        mode = {0: "off", 1: "post", 2: "off", 3: "pre"}[kind]
    else:
        kind = bits(w, 11, 10)
        if kind == 2:
            return load_reg_regoff(w, size, v, opc, n, t)
        if kind == 0 and not v:
            return atomic_op(w, size, n, t)
        raise Unsupported("ldraa/ldrab")

    if v:
        lsz = size | (opc & 2) << 1
        if lsz > 4:
            raise Unsupported("ldst-v-reserved")
        o = off << lsz if scaled else off
        acc = (lambda a: vreg_load(t, lsz, a)) if opc & 1 else (lambda a: vreg_store(t, lsz, a))
        return Insn(code=with_addr(n, off_c(o), mode, acc))
    o = off << size if scaled else off
    if opc == 0:
        acc = lambda a: int_store(t, size, a)
    elif opc == 1:
        acc = lambda a: int_load(t, size, 1, a)
    else:
        if size == 3:
            return Insn()  # prfm / prfum
        if size == 2 and opc == 3:
            raise Unsupported("ldst-reserved")
        acc = lambda a: int_load(t, size, opc, a)
    info = {"op": "ldr" if opc else "str", "size": size, "rt": t, "rn": n, "imm": o, "mode": mode,
            "signed": opc >= 2}
    return Insn(code=with_addr(n, off_c(o), mode, acc), info=info)


def load_reg_regoff(w, size, v, opc, n, t):
    rm, option, s = bits(w, 20, 16), bits(w, 15, 13), bits(w, 12, 12)
    if not option & 2:
        raise Unsupported("ldst-regoff-reserved")
    if v:
        lsz = size | (opc & 2) << 1
        if lsz > 4:
            raise Unsupported("ldst-v-reserved")
        sh = lsz if s else 0
    else:
        sh = size if s else 0
    idx = f"(uint64_t){EXTEND[option]}{rx(rm)}"
    if sh:
        idx = f"({idx} << {sh})"
    addr_expr = f"{rx(n, sp=True)} + {idx}"
    info = {"op": "ldr_reg", "size": size, "rt": t, "rn": n, "rm": rm, "shift": sh, "option": option,
            "signed": opc >= 2, "v": v}
    if v:
        access = vreg_load(t, lsz, "a") if opc & 1 else vreg_store(t, lsz, "a")
    elif opc == 0:
        access = int_store(t, size, "a")
    elif opc == 1:
        access = int_load(t, size, 1, "a")
    else:
        if size == 3:
            return Insn()
        if size == 2 and opc == 3:
            raise Unsupported("ldst-reserved")
        access = int_load(t, size, opc, "a")
    return Insn(code=[f"{{ uint64_t a = {addr_expr}; {access} }}"], info=info)


AMO = {0: "add", 1: "clr", 2: "eor", 3: "set", 4: "smax", 5: "smin", 6: "umax", 7: "umin"}


def atomic_op(w, size, n, t):
    o3, opc, rs = bits(w, 15, 15), bits(w, 14, 12), bits(w, 20, 16)
    T = INT_T[size]
    addr = rx(n, sp=True)
    if o3 and opc == 0:
        fn = "amo_swp"
    elif o3 and opc == 4:
        return Insn(code=[int_load(t, size, 1, addr)])  # ldapr
    elif not o3:
        fn = f"amo_{AMO[opc]}"
    else:
        raise Unsupported("atomic-other")
    expr = f"(uint64_t){fn}<{T}>({addr}, ({T}){rx(rs)})"
    if t == 31:
        return Insn(code=[f"(void){expr};"])
    return Insn(code=[f"c->x[{t}] = {expr};"])


def exclusive(w):
    size, o2, l, o1, rs, o0, t2, n, t = (bits(w, 31, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21),
                                         bits(w, 20, 16), bits(w, 15, 15), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0))
    T = INT_T[size]
    addr = rx(n, sp=True)
    if not o2 and not o1:
        if l:
            return Insn(code=[f"{{ uint64_t a = {addr}; c->excl_addr = a; c->excl_val = ld_acq<{T}>(a); {wr(t, 1, 'c->excl_val')} }}"])
        return Insn(code=[f"{{ uint64_t a = {addr}; {wr(rs, 0, f'excl_store<{T}>(c, a, ({T}){rx(t)})')} }}"])
    if not o2 and o1:
        if size < 2:  # casp: compare-and-swap of the pair (Rs, Rs+1) with (Rt, Rt+1)
            if rs & 1 or t & 1:
                raise Unsupported("casp-reserved")
            if size == 0:
                old = f"amo_cas<uint64_t>(a, (uint64_t)(uint32_t){rx(rs)} | (uint64_t){rx(rs + 1)} << 32, (uint64_t)(uint32_t){rx(t)} | (uint64_t){rx(t + 1)} << 32)"
                return Insn(code=[f"{{ uint64_t a = {addr}; uint64_t o = {old}; {wr(rs, 0, 'o')} {wr(rs + 1, 0, 'o >> 32')} }}"])
            return Insn(code=[f"{{ uint64_t a = {addr}; uint64_t lo = {rx(rs)}, hi = {rx(rs + 1)}; casp128(a, lo, hi, {rx(t)}, {rx(t + 1)}); "
                              f"{wr(rs, 1, 'lo')} {wr(rs + 1, 1, 'hi')} }}"])
        half = INT_T[size]
        if l:
            return Insn(code=[f"{{ uint64_t a = {addr}; excl_load_pair<{half}>(c, a); {wr(t, 1, 'c->excl_val')} {wr(t2, 1, 'c->excl_val2')} }}"])
        return Insn(code=[f"{{ uint64_t a = {addr}; {wr(rs, 0, f'excl_store_pair<{half}>(c, a, {rx(t)}, {rx(t2)})')} }}"])
    if o2 and not o1:
        if l:
            return Insn(code=[f"{{ uint64_t a = {addr}; {wr(t, 1, f'(uint64_t)ld_acq<{T}>(a)')} }}"])
        return Insn(code=[f"{{ uint64_t a = {addr}; st_rel<{T}>(a, ({T}){rx(t)}); }}"])
    # cas
    return Insn(code=[f"{{ uint64_t a = {addr}; {wr(rs, 1, f'(uint64_t)amo_cas<{T}>(a, ({T}){rx(rs)}, ({T}){rx(t)})')} }}"])


# ---------------------------------------------------------------- floating point / SIMD

FT = {0: ("float", "f32", "f[0]"), 1: ("double", "f64", "d[0]")}


def fp_simd(w):
    if bits(w, 30, 29) == 0 and bits(w, 28, 24) == 0b11110 and bits(w, 21, 21) == 1:
        if bits(w, 31, 31) and bits(w, 15, 10):
            raise Unsupported("fp-reserved")
        return fp_scalar(w)
    if bits(w, 28, 24) == 0b11110 and bits(w, 21, 21) == 0 and bits(w, 30, 29) == 0:
        return neon.fp_fixed(w)
    if bits(w, 31, 24) == 0b00011111:
        return fp3(w)
    return neon.decode_simd(w)


def vread(n, ftype):
    return f"c->v[{n}].{FT[ftype][2]}"


def vwrite(d, ftype, expr):
    return f"c->v[{d}] = V128::{FT[ftype][1]}({expr});"


def fp_scalar(w):
    ftype = bits(w, 23, 22)
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    op1015 = bits(w, 15, 10)
    if bits(w, 11, 10) == 0 and bits(w, 15, 12) == 0:
        return fp_int(w)
    if ftype not in FT:
        raise Unsupported(f"fp-half/{ftype}")
    T = FT[ftype][0]
    a, b = vread(rn, ftype), vread(rm, ftype)
    if bits(w, 14, 10) == 0b10000:  # 1-source
        opcode = bits(w, 20, 15)
        if opcode == 0:
            return Insn(code=[vwrite(rd, ftype, a)])
        if opcode == 1:
            return Insn(code=[vwrite(rd, ftype, f"std::fabs({a})")])
        if opcode == 2:
            return Insn(code=[vwrite(rd, ftype, f"-{a}")])
        if opcode == 3:
            return Insn(code=[vwrite(rd, ftype, f"fsqrt_({a})")])
        if opcode in (4, 5):
            to = opcode & 1
            if to == ftype:
                raise Unsupported("fcvt-reserved")
            return Insn(code=[vwrite(rd, to, f"({FT[to][0]}){a}")])
        if opcode == 7:
            raise Unsupported("fcvt-half")
        if 8 <= opcode <= 15:
            mode = {8: "frint_n", 9: "frint_p", 10: "frint_m", 11: "frint_z", 12: "frint_a", 14: "frint_x", 15: "frint_x"}.get(opcode)
            if not mode:
                raise Unsupported("frint-reserved")
            return Insn(code=[vwrite(rd, ftype, f"{mode}({a})")])
        raise Unsupported("fp1-other")
    if bits(w, 13, 10) == 0b1000:  # compare
        opc = bits(w, 4, 0)
        rhs = f"({T})0" if opc & 8 else b
        return Insn(code=[f"fcmp_flags(c, {a}, {rhs});"])
    if bits(w, 12, 10) == 0b100:  # fmov immediate
        imm8 = bits(w, 20, 13)
        return Insn(code=[vwrite(rd, ftype, fp_imm(imm8, ftype))])
    k = bits(w, 11, 10)
    if k == 1:  # fccmp
        cond, nzcv = bits(w, 15, 12), bits(w, 3, 0)
        return Insn(code=[f"if ({CONDS[cond]}) fcmp_flags(c, {a}, {b}); else set_nzcv(c, {nzcv});"])
    if k == 2:  # 2-source
        opcode = bits(w, 15, 12)
        expr = {0: f"fmul_({a}, {b})", 1: f"fdiv_({a}, {b})", 2: f"fadd_({a}, {b})", 3: f"fsub_({a}, {b})",
                4: f"fmax_a({a}, {b})", 5: f"fmin_a({a}, {b})", 6: f"fmaxnm_a({a}, {b})", 7: f"fminnm_a({a}, {b})",
                8: f"-fmul_({a}, {b})"}.get(opcode)
        if expr is None:
            raise Unsupported("fp2-reserved")
        return Insn(code=[vwrite(rd, ftype, expr)])
    if k == 3:  # fcsel
        cond = bits(w, 15, 12)
        return Insn(code=[f"c->v[{rd}] = {CONDS[cond]} ? V128::{FT[ftype][1]}({a}) : V128::{FT[ftype][1]}({b});"])
    raise Unsupported("fp-scalar-other")


def fp_imm(imm8, ftype):
    """VFPExpandImm: (-1)^sign * (16 + frac) / 16 * 2^n with n in [-3, 4]."""
    b6, e, frac = (imm8 >> 6) & 1, (imm8 >> 4) & 3, imm8 & 0xF
    n = e - 3 if b6 else e + 1
    val = (16 + frac) / 16 * (2.0 ** n)
    if imm8 >> 7:
        val = -val
    return repr(val) + ("f" if ftype == 0 else "")

def fp_int(w):
    sf, ftype, rmode, opcode = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16)
    rd, rn = bits(w, 4, 0), bits(w, 9, 5)
    if opcode in (6, 7):  # fmov
        if rmode == 1 and ftype == 2 and sf:
            if opcode == 6:
                return Insn(code=[wr(rd, 1, f"c->v[{rn}].hi")])
            return Insn(code=[f"c->v[{rd}].hi = {rx(rn)};"])
        if rmode != 0:
            raise Unsupported("fmov-reserved")
        if ftype == 3:
            raise Unsupported("fmov-half")
        if opcode == 6:
            return Insn(code=[wr(rd, sf, f"c->v[{rn}].lo")])
        return Insn(code=[f"c->v[{rd}] = V128::zx({rd_(rn, sf)});"])
    if ftype not in FT:
        raise Unsupported("fp-int-half")
    T = FT[ftype][0]
    a = vread(rn, ftype)
    if opcode in (2, 3) and rmode == 0:  # scvtf / ucvtf
        src = f"({st_(sf)}){rd_(rn, sf)}" if opcode == 2 else rd_(rn, sf)
        return Insn(code=[vwrite(rd, ftype, f"({T}){src}")])
    rounding = {(0, 0): "near", (0, 4): "away", (1, 0): "pinf", (2, 0): "minf", (3, 0): "zero"}
    key = (rmode, opcode & 6)
    if key in rounding and opcode in (0, 1, 4, 5):
        signed = not (opcode & 1)
        it = (st_ if signed else ut)(sf)
        return Insn(code=[wr(rd, sf, f"({ut(sf)})fcvt_int<{it}>({a}, R_{rounding[key].upper()})")])
    raise Unsupported(f"fp-int-{rmode}-{opcode}")


def fp3(w):
    ftype, o1, rm, o0, ra, rn, rd = (bits(w, 23, 22), bits(w, 21, 21), bits(w, 20, 16), bits(w, 15, 15),
                                     bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0))
    if ftype not in FT:
        raise Unsupported("fp3-half")
    a, b, acc = vread(rn, ftype), vread(rm, ftype), vread(ra, ftype)
    expr = {(0, 0): f"fma_({a}, {b}, {acc})", (0, 1): f"fma_(-{a}, {b}, {acc})",
            (1, 0): f"fma_(-{a}, {b}, -{acc})", (1, 1): f"fma_({a}, {b}, -{acc})"}[(o1, o0)]
    return Insn(code=[vwrite(rd, ftype, expr)])


import neon  # noqa: E402  (imports names from this module, so it must load last)
