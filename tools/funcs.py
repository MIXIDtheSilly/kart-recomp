"""Function discovery from the .eh_frame unwind tables plus direct call targets.

Usage: python funcs.py <out dir> <module>

Every function compiled with unwind info has an FDE giving its exact [start, end).
Call targets and exports that fall outside any FDE are added as size-unknown functions
(the lifter bounds them by the next known start). Writes <module>.funcs.json.
"""
import bisect
import json
import struct
import sys
from pathlib import Path



class Reader:
    def __init__(self, img):
        self.img = img

    def uleb(self, a):
        v = shift = 0
        while True:
            b = self.img[a]
            a += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return v, a

    def sleb(self, a):
        v = shift = 0
        while True:
            b = self.img[a]
            a += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    v -= 1 << shift
                return v, a

    def encoded(self, a, enc, datarel=0):
        """Read a DW_EH_PE encoded pointer at a. Returns (value, next)."""
        if enc == 0xFF:
            return None, a
        fmt = enc & 0x0F
        app = enc & 0x70
        if fmt == 0x00:
            v, n = struct.unpack_from("<Q", self.img, a)[0], a + 8
        elif fmt == 0x01:
            v, n = self.uleb(a)
        elif fmt == 0x02:
            v, n = struct.unpack_from("<H", self.img, a)[0], a + 2
        elif fmt == 0x03:
            v, n = struct.unpack_from("<I", self.img, a)[0], a + 4
        elif fmt == 0x04:
            v, n = struct.unpack_from("<Q", self.img, a)[0], a + 8
        elif fmt == 0x09:
            v, n = self.sleb(a)
        elif fmt == 0x0A:
            v, n = struct.unpack_from("<h", self.img, a)[0], a + 2
        elif fmt == 0x0B:
            v, n = struct.unpack_from("<i", self.img, a)[0], a + 4
        elif fmt == 0x0C:
            v, n = struct.unpack_from("<q", self.img, a)[0], a + 8
        else:
            raise ValueError(f"unsupported pointer encoding {enc:#x}")
        if app == 0x10:
            v += a
        elif app == 0x30:
            v += datarel
        elif app != 0:
            raise ValueError(f"unsupported pointer application {enc:#x}")
        return v & 0xFFFFFFFFFFFFFFFF, n


def parse_eh_frame(img, hdr):
    r = Reader(img)
    version, ptr_enc, count_enc, table_enc = img[hdr:hdr + 4]
    if version != 1:
        raise ValueError("unexpected eh_frame_hdr version")
    eh_frame, a = r.encoded(hdr + 4, ptr_enc, hdr)
    count, a = r.encoded(a, count_enc, hdr)

    cies = {}
    funcs = {}
    for _ in range(count):
        _, a = r.encoded(a, table_enc, hdr)
        fde, a = r.encoded(a, table_enc, hdr)
        length = struct.unpack_from("<I", img, fde)[0]
        cie_ptr_pos = fde + 4
        cie = cie_ptr_pos - struct.unpack_from("<I", img, cie_ptr_pos)[0]
        if cie not in cies:
            cies[cie] = parse_cie(r, img, cie)
        enc = cies[cie]
        start, p = r.encoded(fde + 8, enc)
        size, _ = r.encoded(p, enc & 0x0F)
        funcs[start] = size
    return eh_frame, funcs


def parse_cie(r, img, cie):
    """Return the FDE pointer encoding declared by the CIE's 'R' augmentation."""
    p = cie + 8 + 1  # length, id, version
    aug_end = img.index(b"\0", p)
    aug = img[p:aug_end].decode()
    p = aug_end + 1
    _, p = r.uleb(p)  # code alignment
    _, p = r.sleb(p)  # data alignment
    _, p = r.uleb(p)  # return address register
    if not aug.startswith("z"):
        return 0x00
    _, p = r.uleb(p)  # augmentation data length
    for ch in aug[1:]:
        if ch == "R":
            return img[p]
        if ch == "P":
            penc = img[p]
            _, p = r.encoded(p + 1, penc)
        elif ch in "LB":
            p += 1
    return 0x00


def pointer_targets(info, img, t0, t1):
    """Code addresses the module takes as values: function pointers in data (RELR and RELATIVE
    relocations, e.g. vtables and callback tables) and addresses formed by adr / adrp+add."""
    out = set()
    for where in info.get("relr", []):
        v = struct.unpack_from("<Q", img, where)[0]
        if t0 <= v < t1 and not v & 3:
            out.add(v)
    for r in info["relocations"]:
        if r["type"] == "RELATIVE" and t0 <= r["addend"] < t1 and not r["addend"] & 3:
            out.add(r["addend"])
    words = struct.unpack_from(f"<{(t1 - t0) // 4}I", img, t0)
    for i, w in enumerate(words):
        pc = t0 + i * 4
        if w & 0x9F000000 == 0x10000000:  # adr
            imm = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
            v = pc + (imm - (1 << 21) if imm & (1 << 20) else imm)
        elif w & 0x9F000000 == 0x90000000 and i + 1 < len(words):  # adrp + add rd, rd, #imm
            rd = w & 0x1F
            nxt = words[i + 1]
            if nxt & 0xFFC00000 != 0x91000000 or nxt & 0x1F != rd or (nxt >> 5) & 0x1F != rd:
                continue
            imm = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
            page = (pc & ~0xFFF) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
            v = page + ((nxt >> 10) & 0xFFF)
        else:
            continue
        if t0 <= v < t1 and not v & 3:
            out.add(v)
    return out


def branch_target(w, pc):
    """Target of b / bl / b.cond / cbz / cbnz / tbz / tbnz, with a flag for bl."""
    if w >> 26 in (0b000101, 0b100101):
        off = w & 0x3FFFFFF
        return pc + ((off - (1 << 26)) if off & (1 << 25) else off) * 4, w >> 31
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:  # b.cond, cbz/cbnz
        off = (w >> 5) & 0x7FFFF
        return pc + ((off - (1 << 19)) if off & (1 << 18) else off) * 4, 0
    if w & 0x7E000000 == 0x36000000:  # tbz/tbnz
        off = (w >> 5) & 0x3FFF
        return pc + ((off - (1 << 14)) if off & (1 << 13) else off) * 4, 0
    return None, 0


def assign_regions(img, funcs):
    """Group code without unwind info into multi-entry regions.

    Such code (PLT stubs, outlined helpers, hand-written assembly) can branch between what look
    like separate functions. A run of contiguous entries is cut at every entry no internal branch
    crosses; each piece with several entries gets a "region" id (its first entry) and is lifted as
    one C++ function, so every internal branch stays a goto. Calls and tail calls to entries do
    not prevent cuts: they are emitted as calls either way.
    """
    i = 0
    while i < len(funcs):
        if funcs[i]["eh"]:
            i += 1
            continue
        j = i
        while j + 1 < len(funcs) and not funcs[j + 1]["eh"] and funcs[j]["addr"] + funcs[j]["size"] == funcs[j + 1]["addr"]:
            j += 1
        run = funcs[i:j + 1]
        lo, hi = run[0]["addr"], run[-1]["addr"] + run[-1]["size"]
        entries = {f["addr"] for f in run}
        crossing = [0] * ((hi - lo) // 4 + 1)  # +1 at span start, -1 at span end (difference array)
        for pc in range(lo, hi, 4):
            t, is_call = branch_target(struct.unpack_from("<I", img, pc)[0], pc)
            if t is None or is_call or not (lo <= t < hi):
                continue
            if t in entries and (struct.unpack_from("<I", img, pc)[0] >> 26) == 0b000101:
                continue  # tail call to an entry
            a, b = (pc, t) if pc < t else (t, pc)
            crossing[(a - lo) // 4 + 1] += 1  # cuts strictly after a ...
            crossing[(b - lo) // 4 + 1] -= 1  # ... up to and including b
        depth, open_at = 0, {}
        for k in range((hi - lo) // 4):
            depth += crossing[k]
            open_at[lo + k * 4] = depth
        region = None
        for f in run:
            if region is None or open_at[f["addr"]] == 0:
                region = f["addr"]
            f["region"] = region
        i = j + 1
    # Single-entry pieces are ordinary functions.
    count = {}
    for f in funcs:
        if "region" in f:
            count[f["region"]] = count.get(f["region"], 0) + 1
    for f in funcs:
        if "region" in f and count[f["region"]] == 1:
            del f["region"]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out, name = Path(sys.argv[1]), sys.argv[2]
    info = json.loads((out / f"{name}.json").read_text())
    img = (out / f"{name}.img").read_bytes()
    text = next(s for s in info["segments"] if s["name"] == "text")
    t0, t1 = text["addr"], text["addr"] + text["size"]

    _, fde_funcs = parse_eh_frame(img, info["mod0"]["eh_frame_hdr_start"])
    candidates = {int(x, 16) for x in json.loads((out / f"{name}.functions.json").read_text())}

    starts = sorted(fde_funcs)
    ends = [s + fde_funcs[s] for s in starts]
    pointers = pointer_targets(info, img, t0, t1)

    def inside_fde(c):
        i = bisect.bisect_right(starts, c) - 1
        return i >= 0 and c < ends[i]

    # Direct branch targets (bl and b, i.e. calls and tail calls) and code pointers outside the
    # functions that have unwind info are entry points. Code without unwind info is lifted as
    # multi-entry regions (see lift.py), so an extra entry there is always harmless.
    words = struct.unpack_from(f"<{(t1 - t0) // 4}I", img, t0)
    for i, w in enumerate(words):
        if w >> 26 not in (0b000101, 0b100101):
            continue
        pc = t0 + i * 4
        off = w & 0x3FFFFFF
        candidates.add(pc + ((off - (1 << 26)) if off & (1 << 25) else off) * 4)

    extra = []
    for c in sorted(candidates | pointers):
        if c in fde_funcs or not (t0 <= c < t1) or inside_fde(c) or c & 3:
            continue  # internal branch, or a pointer/call into the middle of a known function
        extra.append(c)
    print(f"  {len(pointers):,} code pointers (relocations, adr/adrp), "
          f"{len(set(extra) - candidates):,} of them new entry points")

    names = {s["value"]: s["name"] for s in info["symbols"] if s["defined"] and s["type"] == "FUNC" and s["name"]}
    all_starts = sorted(set(starts) | set(extra))
    funcs = []
    for i, s in enumerate(all_starts):
        size = fde_funcs.get(s)
        nxt = all_starts[i + 1] if i + 1 < len(all_starts) else t1
        entry = {"addr": s, "size": size if size is not None else nxt - s, "eh": size is not None}
        if s in names:
            entry["name"] = names[s]
        funcs.append(entry)
    assign_regions(img, funcs)

    covered = sum(f["size"] for f in funcs)
    (out / f"{name}.funcs.json").write_text(json.dumps(funcs))
    print(f"{name}: {len(fde_funcs):,} functions from unwind info, {len(extra):,} extra from call targets/exports")
    print(f"  total {len(funcs):,} functions covering {covered / (t1 - t0):.1%} of text")
    sizes = sorted(f["size"] for f in funcs)
    print(f"  size: median {sizes[len(sizes) // 2]} B, p99 {sizes[len(sizes) * 99 // 100]} B, max {sizes[-1]:,} B")


if __name__ == "__main__":
    main()
