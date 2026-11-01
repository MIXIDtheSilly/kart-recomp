"""Linear-sweep census of a module's text segment.

Usage: python scan.py <out dir> <module>

Reports instruction mix, direct call targets (function candidates), syscalls and
indirect branches, and writes <module>.functions.json with candidate entry points.
"""
import collections
import json
import struct
import sys
from pathlib import Path

import capstone

SIMD_PREFIXES = ("ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out, name = Path(sys.argv[1]), sys.argv[2]
    info = json.loads((out / f"{name}.json").read_text())
    img = (out / f"{name}.img").read_bytes()
    text = next(s for s in info["segments"] if s["name"] == "text")
    start, end = text["addr"], text["addr"] + text["size"]

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    mnemonics = collections.Counter()
    call_targets = collections.Counter()
    tail_targets = set()
    svcs = collections.Counter()
    invalid = 0
    blr = br = 0
    simd = 0

    words = struct.unpack_from(f"<{(end - start) // 4}I", img, start)
    for i, w in enumerate(words):
        pc = start + i * 4
        top6 = w >> 26
        if top6 in (0b100101, 0b000101):  # BL / B imm26
            off = w & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            target = pc + off * 4
            if top6 == 0b100101:
                call_targets[target] += 1
                mnemonics["bl"] += 1
            else:
                mnemonics["b"] += 1
                if not (start <= target < end) or abs(target - pc) > 0x4000:
                    tail_targets.add(target)
            continue
        if w & 0xFFE0001F == 0xD4000001:
            svcs[(w >> 5) & 0xFFFF] += 1
        decoded = list(md.disasm_lite(img[pc:pc + 4], pc))
        if not decoded:
            invalid += 1
            continue
        _, _, mnem, ops = decoded[0]
        mnemonics[mnem] += 1
        if mnem == "blr":
            blr += 1
        elif mnem == "br":
            br += 1
        if any(r in ops for r in ("v0", "v1", "v2", "v3", "q0", "q1", ".4s", ".2d", ".16b", ".8h")) or mnem.startswith(SIMD_PREFIXES):
            simd += 1

    total = sum(mnemonics.values()) + invalid
    text_targets = {t for t in call_targets if start <= t < end}
    funcs = sorted(text_targets | {s["value"] for s in info["symbols"] if s["defined"] and s["type"] == "FUNC"})
    funcs = sorted(set(funcs) | set(info["init_array"]) | set(info["fini_array"]))

    report = {
        "module": name,
        "text_bytes": end - start,
        "instructions": total,
        "undecodable_words": invalid,
        "distinct_mnemonics": len(mnemonics),
        "simd_fp_instructions": simd,
        "direct_calls": sum(call_targets.values()),
        "indirect_calls_blr": blr,
        "indirect_jumps_br": br,
        "svc": dict(svcs),
        "function_candidates": len(funcs),
        "far_tail_call_targets": len(tail_targets),
        "top_mnemonics": mnemonics.most_common(80),
        "all_mnemonics": sorted(mnemonics),
    }
    (out / f"{name}.scan.json").write_text(json.dumps(report, indent=1))
    (out / f"{name}.functions.json").write_text(json.dumps([hex(f) for f in funcs]))

    print(f"{name}: {total:,} instructions, {len(funcs):,} function candidates, {len(mnemonics)} distinct mnemonics")
    print(f"  undecodable words {invalid:,}  SIMD/FP {simd:,} ({simd / total:.1%})")
    print(f"  calls: direct {report['direct_calls']:,}  indirect {blr:,}   indirect jumps (switch tables) {br:,}")
    print(f"  svc: {dict(svcs) or 'none'}")
    print("  top:", ", ".join(f"{m} {c:,}" for m, c in mnemonics.most_common(25)))


if __name__ == "__main__":
    main()
