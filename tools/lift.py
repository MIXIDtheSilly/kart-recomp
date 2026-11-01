"""Function lifter: turns a module's functions into C++ source files.

Usage: python lift.py <out dir> <module> <gen dir> [--only addr,addr,...] [--chunk N] [--asm] [--stats-only]

Writes <gen dir>/<module>_NNN.cpp (functions), <gen dir>/<module>_table.cpp (address -> function table)
and <gen dir>/<module>_stats.json (coverage and the most common unsupported encodings).
"""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import a64  # noqa: E402

try:
    import capstone
    _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
except ImportError:
    _cs = None


def disasm(word, pc):
    if not _cs:
        return ""
    for _, _, m, o in _cs.disasm_lite(struct.pack("<I", word), pc):
        return f"{m} {o}".strip()
    return f".word {word:#010x}"


class Module:
    def __init__(self, out: Path, name: str):
        self.info = json.loads((out / f"{name}.json").read_text())
        self.img = (out / f"{name}.img").read_bytes()
        self.base = 0  # lifted module-relative; generated code adds the runtime base MB
        self.funcs = json.loads((out / f"{name}.funcs.json").read_text())
        self.starts = {self.base + f["addr"] for f in self.funcs}
        text = next(s for s in self.info["segments"] if s["name"] == "text")
        self.text = (self.base + text["addr"], self.base + text["addr"] + text["size"])

    def word(self, addr):
        return struct.unpack_from("<I", self.img, addr - self.base)[0]

    def read(self, addr, size, signed):
        fmt = {1: "b", 2: "h", 4: "i", 8: "q"}[size]
        return struct.unpack_from("<" + (fmt if signed else fmt.upper()), self.img, addr - self.base)[0]


# ---------------------------------------------------------------- jump tables

def find_def(insns, i, reg, window=24):
    """Index of the closest preceding instruction that defines `reg`, or None if unknown."""
    for j in range(i - 1, max(-1, i - 1 - window), -1):
        ins = insns[j][2]
        if ins is None:
            return None
        if ins.kind != "op":
            return None
        info = ins.info
        if info.get("rd") == reg or (info.get("op") in ("ldr", "ldr_reg") and info.get("rt") == reg and not info.get("v")):
            return j
        if reg in gp_writes(insns[j][1], ins)[0]:
            return None  # an instruction the analysis does not model writes this register
    return None


def const_value(mod, insns, i, reg, depth=0, window=24):
    """Resolve reg to a constant at instruction i through adr/adrp/add-imm/movz/movk chains."""
    if depth > 6:
        return None
    j = find_def(insns, i, reg, window)
    if j is None:
        return None
    info = insns[j][2].info
    op = info.get("op")
    if op in ("adr", "adrp", "movz"):
        return info["value"]
    if op == "add" and info.get("rn") != 31:
        b = const_value(mod, insns, j, info["rn"], depth + 1)
        return None if b is None else (b + info["imm"]) & ((1 << 64) - 1)
    if op == "movk":
        b = const_value(mod, insns, j, info["rd"], depth + 1)
        return None if b is None else (b & ~info["mask"]) | info["value"]
    if op == "mov":
        return const_value(mod, insns, j, info["rm"], depth + 1)
    return None


CALL_CLOBBERS = frozenset(range(19)) | {30}


def gp_writes(w, ins):
    """General-purpose registers an instruction may write (over-approximated) and whether the
    write is a load relative to sp (a spill reload or epilogue restore)."""
    regs, stack = set(), False
    if ins is None:
        return set(range(31)), False
    if ins.kind in ("call", "icall"):
        return set(CALL_CLOBBERS), False
    if ins.kind != "op":
        return regs, False
    a64b = a64.bits
    op0 = a64b(w, 28, 25)
    rt, rn = w & 0x1F, a64b(w, 9, 5)
    if op0 in (0b1000, 0b1001) or op0 & 0b0111 == 0b0101:
        regs.add(rt)
    elif op0 & 0b0101 == 0b0100:
        v = a64b(w, 26, 26)
        if a64b(w, 29, 24) == 0b001000:
            regs |= {rt, a64b(w, 14, 10), a64b(w, 20, 16)}
        elif a64b(w, 29, 27) == 0b011 and not a64b(w, 24, 24):
            if not v:
                regs.add(rt)
        elif a64b(w, 29, 27) == 0b101:
            if a64b(w, 22, 22) and not v:
                regs |= {rt, a64b(w, 14, 10)}
                stack = rn == 31
            if a64b(w, 25, 23) in (1, 3):
                regs.add(rn)
        elif a64b(w, 29, 27) == 0b111:
            load = a64b(w, 23, 22) != 0
            if load and not v:
                regs.add(rt)
                stack = rn == 31
            if not a64b(w, 24, 24) and not a64b(w, 21, 21) and a64b(w, 11, 10) in (1, 3):
                regs.add(rn)
            if not a64b(w, 24, 24) and a64b(w, 21, 21) and a64b(w, 11, 10) == 0:
                regs.add(rt)  # LSE atomics return the old value
        elif a64b(w, 23, 23):
            regs.add(rn)  # SIMD structure load/store with post-index
    elif op0 in (0b1010, 0b1011):
        if a64b(w, 31, 22) == 0b1101010100 and a64b(w, 21, 21):
            regs.add(rt)  # mrs
    else:  # FP / SIMD: only the forms that write a general register
        if a64b(w, 30, 29) == 0 and a64b(w, 28, 24) == 0b11110:
            if a64b(w, 21, 21) and a64b(w, 15, 10) == 0 and a64b(w, 18, 16) in (0, 1, 4, 5, 6):
                regs.add(rt)
            elif not a64b(w, 21, 21) and a64b(w, 20, 19) == 3:
                regs.add(rt)
        elif a64b(w, 31, 31) == 0 and a64b(w, 29, 21) == 0b001110000 and not a64b(w, 15, 15) \
                and a64b(w, 10, 10) and a64b(w, 14, 11) in (0b0101, 0b0111):
            regs.add(rt)
    regs.discard(31)
    return regs, stack


def build_preds(insns, start, tables=None):
    """Predecessor lists over instruction indices: direct branches, fallthrough, and the edges of
    jump tables already resolved exactly (`tables`: br pc -> target pcs)."""
    preds = [[] for _ in insns]
    end = start + 4 * len(insns)
    for i, (pc, w, ins) in enumerate(insns):
        kind = ins.kind if ins else "trap"
        if kind in ("b", "bcond") and start <= ins.target < end:
            preds[(ins.target - start) // 4].append(i)
        if kind not in ("b", "ret", "ijump", "trap") and i + 1 < len(insns):
            preds[i + 1].append(i)
        if tables and pc in tables:
            for t in set(tables[pc]):
                preds[(t - start) // 4].append(i)
    return preds


def reaching_const(mod, insns, preds, i, reg):
    """Value of `reg` just before instruction i if every definition reaching it is the same constant.

    Walks the control-flow graph backwards from i. Reaching the function entry, a call that
    clobbers reg, a load or any non-constant definition makes the value unknown.
    """
    values = set()
    seen = set()
    work = list(preds[i])
    while work:
        j = work.pop()
        if j in seen:
            continue
        seen.add(j)
        pc, w, ins = insns[j]
        regs, _ = gp_writes(w, ins)
        if reg in regs:
            v = const_value(mod, insns, j + 1, reg)
            if v is None:
                return None
            values.add(v)
            if len(values) > 1:
                return None
            continue
        if j == 0:
            return None  # reached the entry without a definition: reg is an argument
        # Instructions without known predecessors are only reachable through jump tables
        # (often the one being resolved). Ignoring them is safe: the emitted switch compares the
        # runtime target against its labels, so a wrong guess traps instead of misdirecting.
        work.extend(preds[j])
    return values.pop() if len(values) == 1 else None


def table_bound(insns, i, idx_reg):
    """Find `cmp idx, #N` guarding the jump (following register copies); returns entry count N+1."""
    aliases = {idx_reg}
    for j in range(i - 1, max(-1, i - 40), -1):
        ins = insns[j][2]
        if ins is None:
            continue
        info = ins.info
        if info.get("op") == "subs" and info.get("rd") == 31 and info.get("rn") in aliases:
            return info["imm"] + 1
        written = gp_writes(insns[j][1], ins)[0] & aliases
        if not written:
            continue
        # Earlier values of an overwritten register are unrelated to the index, unless the
        # write is a copy whose source carries the same value.
        aliases -= written
        if info.get("op") == "mov":
            aliases.add(info["rm"])
        elif info.get("op") == "and_imm" and info["imm"] == 0xFFFFFFFF:
            aliases.add(info["rn"])  # zero-extension: the 32-bit compare bounds it the same way
        if not aliases:
            return None
    return None


def resolve_jump_table(mod, insns, i, reg, start, end, tables=None):
    """Return (targets, exact) for a switch-table `br`, or None if it does not look like one.

    When the table cannot be read exactly, falls back to a conservative superset: every
    instruction the table could reach given the entry width and the base address.
    """
    # Compilers can hoist many unrelated constant loads between the add and the br.
    j = find_def(insns, i, reg, window=64)
    if j is None:
        return None
    add = insns[j][2].info
    if add.get("op") not in ("addsub_ext", "addsub_reg") or add.get("sub") or add.get("s"):
        return None
    k = find_def(insns, j, add["rm"])
    if k is None:
        return None
    ld = insns[k][2].info
    if ld.get("op") != "ldr_reg" or ld.get("v"):
        return None
    shift = add["amt"]
    size = 1 << ld["size"]
    signed = ld["signed"] or add.get("option") in (4, 5, 6)
    preds = None
    base = const_value(mod, insns, j, add["rn"])
    if base is None:
        preds = build_preds(insns, start, tables)
        base = reaching_const(mod, insns, preds, j, add["rn"])
    table = const_value(mod, insns, k, ld["rn"], window=200)
    if table is None:
        preds = preds or build_preds(insns, start, tables)
        table = reaching_const(mod, insns, preds, k, ld["rn"])
    count = table_bound(insns, k, ld["rm"])

    if base is not None and table is not None:
        # With a known bound the table is read exactly. Without one, read until an entry leaves
        # the function: every real entry is valid, so this only ever adds extra (harmless) labels.
        exact = bool(count) and count <= 4096
        targets = []
        for e in range(count if exact else 1024):
            try:
                t = (base + (mod.read(table + e * size, size, signed) << shift)) & ((1 << 64) - 1)
            except struct.error:
                break
            if not (start <= t < end) or t & 3:
                break
            targets.append(t)
        else:
            if exact:
                return targets, True
        if not exact and targets:
            return targets, True

    lo, hi = start, end
    if base is not None and size == 1:
        lo_e, hi_e = (-128, 127) if signed else (0, 255)
        lo, hi = max(start, base + (lo_e << shift)), min(end, base + (hi_e << shift) + 4)
    return list(range(lo, hi, 4)), False


# ---------------------------------------------------------------- emitting

def write_if_changed(path, text):
    """Keep unchanged files untouched so the build only recompiles chunks that changed."""
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)


# Loop back-edges check the preemption flag so a guest thread that spins can be descheduled.
POLL = "if (c->halt) guest_yield(c); "


# Generated function names carry the module so several modules link into one binary.
PREFIX = "f"


def fname(addr):
    return f"{PREFIX}_{addr:x}"


def lift_function(mod, start, size, with_asm, stats, entries=None):
    """Lift [start, start+size) as one C++ function. With `entries` (a multi-entry region), the
    body takes the entry address and dispatches to it, and each entry gets a wrapper."""
    end = start + size
    insns = []
    unsupported = []
    for pc in range(start, end, 4):
        w = mod.word(pc)
        try:
            ins = a64.decode(w, pc)
        except a64.Unsupported as e:
            ins = None
            unsupported.append(str(e))
        insns.append((pc, w, ins))

    labels = set(entries or ())
    callees = set()
    ijumps = []
    for i, (pc, w, ins) in enumerate(insns):
        if ins is None:
            continue
        if ins.kind in ("b", "bcond") and start <= ins.target < end:
            labels.add(ins.target)
        elif ins.kind in ("b", "bcond", "call"):
            callees.add(ins.target)
        elif ins.kind == "ijump" and ins.info.get("rn") is not None:
            ijumps.append(i)

    # Resolve switch tables to a fixpoint: an exactly resolved table adds CFG edges that can
    # make the table register of a nested switch (reached only through those edges) resolvable.
    exact_tables, jt = {}, {}
    pending = ijumps
    while pending:
        progress, still = False, []
        for i in pending:
            pc = insns[i][0]
            res = resolve_jump_table(mod, insns, i, insns[i][2].info["rn"], start, end, exact_tables)
            if res and res[1]:
                exact_tables[pc] = jt[pc] = res[0]
                progress = True
            else:
                still.append(i)
                if res:
                    jt[pc] = res[0]
                else:
                    jt.pop(pc, None)
        pending = still if progress else []
    for i in ijumps:
        pc = insns[i][0]
        if pc in exact_tables:
            stats["jump_tables"] += 1
        elif pc in jt:
            stats["jump_tables_superset"] += 1
        else:
            stats["indirect_tail_calls"] += 1
    for targets in jt.values():
        labels.update(targets)

    body = []
    for i, (pc, w, ins) in enumerate(insns):
        if pc in labels:
            body.append(f"L_{pc:x}:")
        comment = f"  // {pc:x}: {disasm(w, pc)}" if with_asm else ""
        if ins is None:
            body.append(f"    guest_trap(c, {a64.guest_addr(pc)}, 0xFFFF);  // unsupported{comment}")
            continue
        lines = list(ins.code)
        k = ins.kind
        if k == "b":
            if start <= ins.target < end:
                lines.append(f"{POLL if ins.target <= pc else ''}goto L_{ins.target:x};")
            else:
                lines.append(tail_call(mod, ins.target, pc, callees))
        elif k == "bcond":
            if start <= ins.target < end and ins.target <= pc:
                lines.append(f"if ({ins.cond}) {{ {POLL}goto L_{ins.target:x}; }}")
            elif start <= ins.target < end:
                lines.append(f"if ({ins.cond}) goto L_{ins.target:x};")
            else:
                lines.append(f"if ({ins.cond}) {{ {tail_call(mod, ins.target, pc, callees)} }}")
        elif k == "call":
            if ins.target in mod.starts:
                lines.append(f"{fname(ins.target)}(c);")
            else:
                lines.append(f"call_indirect(c, {a64.guest_addr(ins.target)});")
        elif k == "icall":
            # Braced so the temporary does not sit between a goto and its label.
            lines = ["{"] + lines + [f"call_indirect(c, {ins.reg});", "}"]
        elif k == "ijump":
            if pc in jt:
                cases = " ".join(f"case {t:#x}ull: goto L_{t:x};" for t in sorted(set(jt[pc])))
                lines.append(f"{POLL}switch ({ins.reg} - MB) {{ {cases} default: guest_unresolved_jump(c, {a64.guest_addr(pc)}, {ins.reg}); }}")
            else:
                lines.append(f"call_indirect(c, {ins.reg}); return;")
        elif k == "ret":
            lines.append("return;")
        body.append("    " + " ".join(lines) + comment if lines else f"   {comment}" if comment else None)
    body = [b for b in body if b is not None]

    last = insns[-1][2] if insns else None
    if last is None or last.kind not in ("b", "ret", "trap", "ijump"):
        if end in mod.starts:
            callees.add(end)
            body.append(f"    {fname(end)}(c); return;  // falls through into the next function")
        else:
            body.append(f"    guest_trap(c, {a64.guest_addr(end)}, 0xFFFE);  // fell off the end")

    if unsupported:
        stats["functions_with_unsupported"] += 1
        for u in set(unsupported):
            stats["blocked_by"][u] += 1
        for u in unsupported:
            stats["unsupported_insns"][u] += 1
    stats["functions"] += 1
    stats["instructions"] += len(insns)
    if entries:
        rname = f"{PREFIX}_r_{start:x}"
        cases = " ".join(f"case {e:#x}ull: goto L_{e:x};" for e in entries)
        code = (f"void {rname}(Ctx* c, uint64_t entry) {{\n    switch (entry) {{ {cases} }}\n" + "\n".join(body) + "\n}\n"
                + "".join(f"void {fname(e)}(Ctx* c) {{ {rname}(c, {e:#x}ull); }}\n" for e in entries))
    else:
        code = f"void {fname(start)}(Ctx* c) {{\n" + "\n".join(body) + "\n}\n"
    if any(len(t) > 256 for pc, t in jt.items() if pc not in exact_tables):
        # Thousands of possible goto targets make MSVC's optimizer run for hours; these few
        # functions are compiled unoptimised instead.
        code = '#pragma optimize("", off)\n' + code + '#pragma optimize("", on)\n'
    return code, callees, not unsupported


def tail_call(mod, target, pc, callees):
    if target in mod.starts:
        callees.add(target)
        return f"{fname(target)}(c); return;"
    return f"call_indirect(c, {a64.guest_addr(target)}); return;"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("module")
    ap.add_argument("gen")
    ap.add_argument("--only", help="comma-separated module-relative function offsets")
    ap.add_argument("--chunk", type=int, default=600)
    ap.add_argument("--asm", action="store_true", help="annotate each line with its disassembly")
    ap.add_argument("--stats-only", action="store_true")
    a = ap.parse_args()

    mod = Module(Path(a.out), a.module)
    global PREFIX
    PREFIX = f"{a.module}"
    gen = Path(a.gen)
    gen.mkdir(parents=True, exist_ok=True)
    funcs = mod.funcs
    if a.only:
        wanted = {int(x, 16) for x in a.only.split(",")}
        regions = {f["region"] for f in funcs if f["addr"] in wanted and "region" in f}
        funcs = [f for f in funcs if f["addr"] in wanted or f.get("region") in regions]

    stats = {"functions": 0, "instructions": 0, "functions_with_unsupported": 0, "jump_tables": 0,
             "jump_tables_superset": 0, "indirect_tail_calls": 0, "blocked_by": collections.Counter(), "unsupported_insns": collections.Counter()}
    clean = []
    chunk, chunk_callees, chunk_defs, n = [], set(), set(), 0

    def flush():
        nonlocal chunk, chunk_callees, chunk_defs, n
        if not chunk or a.stats_only:
            chunk, chunk_callees, chunk_defs = [], set(), set()
            return
        decls = "".join(f"void {fname(t)}(Ctx* c);\n" for t in sorted(chunk_callees | chunk_defs))
        write_if_changed(gen / f"{a.module}_{n:03}.cpp",
                         f'#include "cpu.h"\n\nextern uint64_t g_base_{a.module};\n#define MB g_base_{a.module}\n\n{decls}\n' + "\n".join(chunk))
        n += 1
        chunk, chunk_callees, chunk_defs = [], set(), set()

    region_members = {}
    for f in funcs:
        if "region" in f:
            region_members.setdefault(f["region"], []).append(f)
    for idx, f in enumerate(funcs):
        start = mod.base + f["addr"]
        if "region" in f:
            if f["region"] != f["addr"]:
                continue  # lifted together with the region's first entry
            members = region_members[f["region"]]
            entries = [mod.base + m["addr"] for m in members]
            code, callees, ok = lift_function(mod, start, sum(m["size"] for m in members), a.asm, stats, entries)
            if ok:
                clean.extend(m["addr"] for m in members)
            chunk_defs.update(entries)
        else:
            code, callees, ok = lift_function(mod, start, f["size"], a.asm, stats)
            if ok:
                clean.append(f["addr"])
            chunk_defs.add(start)
        chunk.append(code)
        chunk_callees |= {t for t in callees if t in mod.starts}
        if len(chunk) >= a.chunk:
            flush()
        if idx % 20000 == 0 and idx:
            print(f"  {idx:,}/{len(funcs):,}", flush=True)
    flush()
    if not a.stats_only and not a.only:
        for old in gen.glob(f"{a.module}_[0-9][0-9][0-9].cpp"):
            if int(old.stem.rsplit("_", 1)[1]) >= n:
                old.unlink()

    if not a.stats_only:
        lifted = [mod.base + f["addr"] for f in funcs]
        decls = "".join(f"void {fname(t)}(Ctx* c);\n" for t in lifted)
        rows = ",\n".join(f"    {{{t:#x}ull, {fname(t)}}}" for t in lifted)
        write_if_changed(gen / f"{a.module}_table.cpp", 
            f'#include "cpu.h"\n#include "table.h"\n\n{decls}\nextern const FnEntry {a.module}_table[] = {{\n{rows}\n}};\n'
            f"extern const size_t {a.module}_table_size = {len(lifted)};\n")

    report = {k: (v.most_common() if isinstance(v, collections.Counter) else v) for k, v in stats.items()}
    report["clean_functions"] = len(clean)
    (gen / f"{a.module}_stats.json").write_text(json.dumps(report, indent=1))
    (gen / f"{a.module}_clean.json").write_text(json.dumps(clean))
    s = stats
    print(f"{a.module}: {s['functions']:,} functions, {s['instructions']:,} instructions")
    print(f"  fully supported: {len(clean):,} of {len(funcs):,} entry points ({len(clean) / max(1, len(funcs)):.1%})")
    print(f"  jump tables exact {s['jump_tables']:,}, conservative {s['jump_tables_superset']:,}, "
          f"indirect tail calls {s['indirect_tail_calls']:,}")
    print("  most blocking encodings (functions blocked):")
    for name, cnt in s["blocked_by"].most_common(15):
        print(f"    {cnt:7,}  {name}")


if __name__ == "__main__":
    main()
