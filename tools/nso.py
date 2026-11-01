"""NSO loader: unpacks Switch NSO executables into flat memory images plus metadata.

Usage: python nso.py <exefs dir> <out dir>

For each module (rtld, main, subsdk*, sdk) this writes:
  <name>.img   flat memory image (text/ro/data at their memory offsets, bss zero-filled)
  <name>.elf   the same image wrapped as an ELF64 shared object, for Ghidra/IDA
  <name>.json  segments, MOD0 info, dynamic tags, symbols and relocations
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

import lz4.block

MODULE_ORDER = ["rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"]

DT = {
    0: "NULL", 1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB",
    7: "RELA", 8: "RELASZ", 9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI",
    14: "SONAME", 16: "SYMBOLIC", 20: "PLTREL", 23: "JMPREL", 24: "BIND_NOW", 25: "INIT_ARRAY",
    26: "FINI_ARRAY", 27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ", 30: "FLAGS",
    0x6FFFFEF5: "GNU_HASH", 0x6FFFFFF9: "RELACOUNT", 0x6FFFFFFB: "FLAGS_1",
}

R_AARCH64 = {1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE", 257: "ABS64", 1024: "COPY", 1030: "TLS_TPREL"}

SYM_TYPE = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 6: "TLS"}
SYM_BIND = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}


class Nso:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: not an NSO")
        self.name = path.name
        flags = struct.unpack_from("<I", raw, 0xC)[0]
        segs = [struct.unpack_from("<III", raw, o) for o in (0x10, 0x20, 0x30)]
        csizes = struct.unpack_from("<III", raw, 0x60)
        hashes = [raw[0xA0 + i * 0x20: 0xC0 + i * 0x20] for i in range(3)]
        self.bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
        self.module_id = raw[0x40:0x60].hex()
        name_off, name_size = struct.unpack_from("<I", raw, 0x1C)[0], struct.unpack_from("<I", raw, 0x2C)[0]
        self.module_name = raw[name_off:name_off + name_size].split(b"\0")[0].decode("ascii", "replace") if name_size else ""

        self.segments = []
        for i, ((foff, moff, size), csize, digest) in enumerate(zip(segs, csizes, hashes)):
            compressed = flags & (1 << i)
            blob = raw[foff:foff + csize]
            data = lz4.block.decompress(blob, uncompressed_size=size) if compressed else blob[:size]
            if len(data) != size:
                raise ValueError(f"{path}: segment {i} size mismatch")
            if flags & (1 << (i + 3)) and hashlib.sha256(data).digest() != digest:
                raise ValueError(f"{path}: segment {i} hash mismatch")
            self.segments.append({"name": ("text", "ro", "data")[i], "addr": moff, "size": size, "data": data})

        data_seg = self.segments[2]
        self.bss_addr = data_seg["addr"] + data_seg["size"]
        self.image_size = (self.bss_addr + self.bss_size + 0xFFF) & ~0xFFF
        img = bytearray(self.image_size)
        for s in self.segments:
            img[s["addr"]:s["addr"] + s["size"]] = s["data"]
        self.img = bytes(img)
        self._parse_mod0()
        self._parse_dynamic()

    def u32(self, a):
        return struct.unpack_from("<I", self.img, a)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.img, a)[0]

    def cstr(self, a):
        end = self.img.index(b"\0", a)
        return self.img[a:end].decode("utf-8", "replace")

    def _parse_mod0(self):
        mod0 = self.u32(4)
        if self.img[mod0:mod0 + 4] != b"MOD0":
            raise ValueError(f"{self.name}: MOD0 not found")
        rel = struct.unpack_from("<6i", self.img, mod0 + 4)
        self.mod0 = {
            "addr": mod0,
            "dynamic": mod0 + rel[0],
            "bss_start": mod0 + rel[1],
            "bss_end": mod0 + rel[2],
            "eh_frame_hdr_start": mod0 + rel[3],
            "eh_frame_hdr_end": mod0 + rel[4],
            "module_object": mod0 + rel[5],
        }

    def _parse_dynamic(self):
        self.dynamic = []
        tags = {}
        a = self.mod0["dynamic"]
        while True:
            tag, val = struct.unpack_from("<qQ", self.img, a)
            a += 16
            if tag == 0:
                break
            self.dynamic.append((tag, val))
            tags.setdefault(tag, val)

        strtab = tags.get(5)
        self.needed = [self.cstr(strtab + v) for t, v in self.dynamic if t == 1]
        soname = tags.get(14)
        self.soname = self.cstr(strtab + soname) if soname is not None else None

        # Symbol count: symtab runs up to strtab in Nintendo's linker layout.
        symtab, syment = tags.get(6), tags.get(11, 24)
        count = (strtab - symtab) // syment if symtab < strtab else 0
        self.symbols = []
        for i in range(count):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", self.img, symtab + i * syment)
            self.symbols.append({
                "index": i,
                "name": self.cstr(strtab + st_name) if st_name else "",
                "type": SYM_TYPE.get(st_info & 0xF, str(st_info & 0xF)),
                "bind": SYM_BIND.get(st_info >> 4, str(st_info >> 4)),
                "defined": st_shndx != 0,
                "value": st_value,
                "size": st_size,
            })

        self.relocs = []
        for off_tag, size_tag in ((7, 8), (23, 2)):
            if off_tag not in tags:
                continue
            base, size = tags[off_tag], tags[size_tag]
            for a in range(base, base + size, 24):
                offset, info, addend = struct.unpack_from("<QQq", self.img, a)
                self.relocs.append({
                    "offset": offset,
                    "type": R_AARCH64.get(info & 0xFFFFFFFF, str(info & 0xFFFFFFFF)),
                    "sym": info >> 32,
                    "addend": addend,
                    "plt": off_tag == 23,
                })

        # DT_RELR (36) / DT_RELRSZ (35): packed relative relocations. An even entry is the next
        # location to relocate; an odd entry is a bitmap of the 63 words that follow.
        self.relr = []
        if 36 in tags:
            where = 0
            for a in range(tags[36], tags[36] + tags[35], 8):
                entry = self.u64(a)
                if entry & 1 == 0:
                    self.relr.append(entry)
                    where = entry + 8
                else:
                    for bit in range(1, 64):
                        if entry >> bit & 1:
                            self.relr.append(where + (bit - 1) * 8)
                    where += 63 * 8

        self.init_array = self._ptr_array(tags, 25, 27)
        self.fini_array = self._ptr_array(tags, 26, 28)
        self.dyn_tags = {DT.get(t, hex(t)): v for t, v in self.dynamic if t != 1}

    def _ptr_array(self, tags, off_tag, size_tag):
        if off_tag not in tags:
            return []
        # Entries are fixed up by RELATIVE relocs, so read the addends instead of raw memory.
        addends = {r["offset"]: r["addend"] for r in self.relocs if r["type"] == "RELATIVE"}
        base = tags[off_tag]
        return [addends.get(base + i * 8, self.u64(base + i * 8)) for i in range(tags[size_tag] // 8)]

    def to_elf(self) -> bytes:
        """Wrap the flat image as an ELF64 ET_DYN so disassemblers can load it with symbols."""
        phdrs = []
        PF_X, PF_W, PF_R = 1, 2, 4
        perms = {"text": PF_R | PF_X, "ro": PF_R, "data": PF_R | PF_W}
        hdr_size = 0x1000  # keep segments page aligned in the file
        for s in self.segments:
            memsz = s["size"] + (self.bss_size if s["name"] == "data" else 0)
            phdrs.append((1, perms[s["name"]], hdr_size + s["addr"], s["addr"], s["addr"], s["size"], memsz, 0x1000))
        dyn_count = len(self.dynamic) + 1
        phdrs.append((2, PF_R | PF_W, hdr_size + self.mod0["dynamic"], self.mod0["dynamic"], self.mod0["dynamic"],
                      dyn_count * 16, dyn_count * 16, 8))
        if self.mod0["eh_frame_hdr_end"] > self.mod0["eh_frame_hdr_start"]:
            ehs = self.mod0["eh_frame_hdr_start"]
            ehsz = self.mod0["eh_frame_hdr_end"] - ehs
            phdrs.append((0x6474E550, PF_R, hdr_size + ehs, ehs, ehs, ehsz, ehsz, 4))

        ehdr = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, b"\0" * 7,
                           3, 183, 1, 0, 0x40, 0, 0, 0x40, 0x38, len(phdrs), 0x40, 0, 0)
        out = bytearray(ehdr)
        for p in phdrs:
            out += struct.pack("<IIQQQQQQ", *p)
        out += b"\0" * (hdr_size - len(out))
        out += self.img[:self.bss_addr]
        return bytes(out)

    def summary(self, base: int) -> dict:
        defined = [s for s in self.symbols if s["defined"] and s["name"]]
        imported = [s for s in self.symbols if not s["defined"] and s["name"]]
        return {
            "file": self.name,
            "module_name": self.module_name,
            "module_id": self.module_id,
            "soname": self.soname,
            "needed": self.needed,
            "load_base": base,
            "image_size": self.image_size,
            "segments": [{k: v for k, v in s.items() if k != "data"} for s in self.segments]
                        + [{"name": "bss", "addr": self.bss_addr, "size": self.bss_size}],
            "mod0": self.mod0,
            "dynamic": self.dyn_tags,
            "init_array": self.init_array,
            "fini_array": self.fini_array,
            "counts": {
                "symbols": len(self.symbols),
                "exports": len(defined),
                "exported_functions": sum(1 for s in defined if s["type"] == "FUNC"),
                "imports": len(imported),
                "relocations": len(self.relocs),
                "relr": len(self.relr),
            },
            "symbols": self.symbols,
            "relocations": self.relocs,
            "relr": self.relr,
        }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exefs, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    # Lay modules out back to back the way rtld does, so cross-module addresses are stable.
    base = 0x8000000
    layout = []
    for name in MODULE_ORDER:
        path = exefs / name
        if not path.exists():
            continue
        nso = Nso(path)
        (out / f"{name}.img").write_bytes(nso.img)
        (out / f"{name}.elf").write_bytes(nso.to_elf())
        info = nso.summary(base)
        (out / f"{name}.json").write_text(json.dumps(info, indent=1))
        c = info["counts"]
        text = nso.segments[0]["size"]
        print(f"{name:8} base={base:#011x} size={nso.image_size:#010x} text={text / 2**20:6.1f} MiB "
              f"soname={nso.soname or '-':24} exports={c['exports']:6} (func {c['exported_functions']:6}) "
              f"imports={c['imports']:6} relocs={c['relocations']:7} init={len(nso.init_array)}")
        layout.append({"name": name, "base": base, "size": nso.image_size})
        base += (nso.image_size + 0x1FFFFF) & ~0x1FFFFF
    (out / "layout.json").write_text(json.dumps(layout, indent=1))


if __name__ == "__main__":
    main()
