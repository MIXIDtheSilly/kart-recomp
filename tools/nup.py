"""Read NUP containers (loose ExeFS/RomFS trees for a base game, updates and DLC in one file).

Format as read by Eden/Eglantine forks (file_sys/nup_container.cpp): 0x80-byte superblock
"NUP\\x01", a TOC (packages, dir/file records, children array, string pool) and a blob region of
raw or zstd extents.

  python tools/nup.py list FILE.nup              packages and their top-level contents
  python tools/nup.py info FILE.nup              the game's version, size and packages (for the launcher)
  python tools/nup.py extract FILE.nup OUT [-p N] write the game as OUT/exefs + OUT/romfs: as Eglantine boots
                                                   it, the newest update over the base (Nup.game); with -p only
                                                   package N. Files already present with the right size are
                                                   skipped (re-runnable); exefs files of another version are
                                                   removed
"""
import argparse
import hashlib
import os
import struct
import sys

KINDS = {0: "base", 1: "update", 2: "addon"}


def _zstd_decompress(data, size):
    """Python 3.14's compression.zstd, else backports.zstd (what the other tools and the launcher check for), else
    the zstandard package."""
    try:
        from compression import zstd
    except ImportError:
        try:
            from backports import zstd
        except ImportError:
            zstd = None
    if zstd:
        return zstd.decompress(data)
    import zstandard
    return zstandard.ZstdDecompressor().decompress(data, max_output_size=size)
NO_DIR = 0xFFFFFFFF


class Nup:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = self.f.read(0x80)
        if sb[:4] != b"NUP\x01":
            raise ValueError("not a NUP file")
        (_, hsize, hver, self.toc_off, self.toc_size, toc_sha, self.blob_off, fsize,
         npkg, flags) = struct.unpack_from("<4sHHQQ32sQQII", sb)
        toc = self._read(self.toc_off, self.toc_size)
        if hashlib.sha256(toc).digest() != toc_sha:
            raise ValueError("TOC SHA-256 mismatch")
        magic, pc, dc, fc, pool_off, pool_size, _ = struct.unpack_from("<IIQQQQQ", toc)
        if magic != 0x01434F54:
            raise ValueError("bad TOC magic")
        pool = toc[pool_off:pool_off + pool_size]
        name = lambda o, n: pool[o:o + n].decode("utf-8")
        pos = 0x30
        self.packages = []
        for _ in range(pc):
            tid, root, exefs, romfs, ver, kind, pflags, _, _, moff, msize = struct.unpack_from(
                "<QIIIIBBHIQI", toc, pos)
            pos += 0x40
            self.packages.append(dict(title_id=tid, root=root, exefs=exefs, romfs=romfs, version=ver,
                                      kind=KINDS.get(kind, kind), standalone=bool(pflags & 1),
                                      meta=self._meta(moff, msize)))
        self.dirs = []
        for _ in range(dc):
            no, nl, first, count, _ = struct.unpack_from("<IIIIQ", toc, pos)
            pos += 0x18
            self.dirs.append((name(no, nl), first, count))
        self.files = []
        for _ in range(fc):
            no, nl, size, eoff, esize, sha, codec = struct.unpack_from("<IIQQQ32sB", toc, pos)
            pos += 0x50
            self.files.append((name(no, nl), size, eoff, esize, sha, codec))
        n = (pool_off - pos) // 4
        self.children = struct.unpack_from("<%dI" % n, toc, pos)

    def _read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def _meta(self, off, size):
        out = {}
        if not off or not size:
            return out
        blk = self._read(off, size)
        p = 0
        while p + 8 <= len(blk):
            t, _, n = struct.unpack_from("<HHI", blk, p)
            p += 8
            v = blk[p:p + n]
            p += n
            if t == 4:
                out["display_version"] = v.decode("utf-8", "replace")
            elif t == 5:
                out["title"] = v.decode("utf-8", "replace")
            elif t == 1:
                out["npdm"] = v
        return out

    def walk(self, d, prefix=""):
        """Yield (relative path, file index) under dir index d."""
        _, first, count = self.dirs[d]
        for c in self.children[first:first + count]:
            if c >= len(self.dirs):
                fi = c - len(self.dirs)
                yield prefix + self.files[fi][0], fi
            else:
                yield from self.walk(c, prefix + self.dirs[c][0] + "/")

    def read_file(self, fi):
        _, size, eoff, esize, sha, codec = self.files[fi]
        if size == 0:
            return b""
        data = self._read(eoff, esize)
        if codec == 1:
            data = _zstd_decompress(data, size)
        if len(data) != size:
            raise IOError("short read: " + self.files[fi][0])
        return data

    def default_package(self):
        ups = [i for i, p in enumerate(self.packages) if p["kind"] == "update"]
        if ups:
            return max(ups, key=lambda i: self.packages[i]["version"])
        return next(i for i, p in enumerate(self.packages) if p["kind"] == "base")

    def game(self):
        """The game as Eglantine's NUP loader boots it: ExeFS whole from the newest update when it has one, else
        from the base; RomFS the base's with the update's files over it (a standalone update carries the complete
        tree, so the overlay changes nothing). Returns (display version, {"exefs/<path>" or "romfs/<path>": file
        index}, the packages used)."""
        base = next((i for i, p in enumerate(self.packages) if p["kind"] == "base"), None)
        ups = [i for i, p in enumerate(self.packages) if p["kind"] == "update"]
        upd = max(ups, key=lambda i: self.packages[i]["version"]) if ups else None
        used = [i for i in (base, upd) if i is not None]
        if not used:
            raise ValueError("no base or update package")
        files = {}
        exefs_from = upd if upd is not None and self.packages[upd]["exefs"] != NO_DIR else base
        if exefs_from is not None and self.packages[exefs_from]["exefs"] != NO_DIR:
            for path, fi in self.walk(self.packages[exefs_from]["exefs"]):
                files["exefs/" + path] = fi
        for i in used:   # base first, the update over it
            if self.packages[i]["romfs"] != NO_DIR:
                for path, fi in self.walk(self.packages[i]["romfs"]):
                    files["romfs/" + path] = fi
        top = self.packages[used[-1]]
        return top["meta"].get("display_version", "v%d" % top["version"]), files, used


def cmd_list(nup):
    for i, p in enumerate(nup.packages):
        m = p["meta"]
        print("[%d] %s %016X v%d (%s)%s %s" % (
            i, p["kind"], p["title_id"], p["version"], m.get("display_version", "?"),
            " standalone" if p["standalone"] else "", m.get("title", "")))
        for part in ("exefs", "romfs"):
            d = p[part]
            if d == NO_DIR:
                print("    %s: none" % part)
                continue
            files = list(nup.walk(d))
            total = sum(nup.files[fi][1] for _, fi in files)
            print("    %s: %d files, %.2f GB" % (part, len(files), total / 1e9))
            if part == "exefs":
                for path, fi in files:
                    print("      %-12s %10d" % (path, nup.files[fi][1]))


def cmd_info(nup):
    """Machine-readable, for the launcher: the game's version and size as extract writes it."""
    ver, files, used = nup.game()
    print("version", ver)
    print("bytes", sum(nup.files[fi][1] for fi in files.values()))
    print("files", len(files))
    print("packages", " ".join("%d:%s" % (i, nup.packages[i]["kind"]) for i in used))


def cmd_extract(nup, out, pkg, verify):
    if pkg is None:
        ver, files, used = nup.game()
        print("extracting version %s (%s) -> %s" % (
            ver, " + ".join("[%d] %s" % (i, nup.packages[i]["kind"]) for i in used), out), flush=True)
    else:
        p = nup.packages[pkg]
        print("extracting [%d] %s v%d -> %s" % (pkg, p["kind"], p["version"], out), flush=True)
        files = {part + "/" + path: fi for part in ("exefs", "romfs") if p[part] != NO_DIR
                 for path, fi in nup.walk(p[part])}
    # Every file in exefs\ is loaded as a module: none may stay from another version
    exefs = os.path.join(out, "exefs")
    if os.path.isdir(exefs):
        keep = {k[len("exefs/"):].lower() for k in files if k.startswith("exefs/")}
        for name in os.listdir(exefs):
            if name.lower() not in keep and os.path.isfile(os.path.join(exefs, name)):
                os.remove(os.path.join(exefs, name))
                print("  removed exefs/%s (not in this version)" % name)
    total = sum(nup.files[fi][1] for fi in files.values())
    done = skipped = 0
    seen = 0
    shown = -1
    for path, fi in sorted(files.items()):
        dst = os.path.join(out, *path.split("/"))
        size = nup.files[fi][1]
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            skipped += 1
        else:
            data = nup.read_file(fi)
            if verify and hashlib.sha256(data).digest() != nup.files[fi][4]:
                raise IOError("SHA-256 mismatch: " + path)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst + ".part", "wb") as f:
                f.write(data)
            os.replace(dst + ".part", dst)
            done += 1
        seen += size
        pct = seen * 100 // max(total, 1)
        if pct != shown and pct % 2 == 0:
            shown = pct
            print("  %3d%%  %.1f of %.1f GB" % (pct, seen / 1e9, total / 1e9), flush=True)
    print("done: %d written, %d already present" % (done, skipped))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("list")
    a.add_argument("nup")
    a = sub.add_parser("info")
    a.add_argument("nup")
    a = sub.add_parser("extract")
    a.add_argument("nup")
    a.add_argument("out")
    a.add_argument("-p", "--package", type=int)
    a.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()
    nup = Nup(args.nup)
    if args.cmd == "list":
        cmd_list(nup)
    elif args.cmd == "info":
        cmd_info(nup)
    else:
        cmd_extract(nup, args.out, args.package, not args.no_verify)


if __name__ == "__main__":
    main()
