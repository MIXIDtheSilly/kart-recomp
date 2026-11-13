r"""The C++ toolchain the build uses without Visual Studio: llvm-mingw (clang, lld and the MinGW-w64 Windows headers and
libraries, https://github.com/mstorsjo/llvm-mingw), downloaded into MINGW (tools\llvm-mingw, see paths.py).

  python tools\toolchain.py check     prints "mingw <version>" when it is there, else "missing"
  python tools\toolchain.py install   downloads the pinned release (190 MB), checks its SHA-256 and unpacks the parts an
                                      x86-64 build uses (about 450 MB)

The release is pinned: a newer clang could change the generated code. To move to another one, change RELEASE and
SHA256 together.
"""
import hashlib
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

import paths

RELEASE = "20261020"   # LLVM 23.1.4
NAME = f"llvm-mingw-{RELEASE}-ucrt-x86_64"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{RELEASE}/{NAME}.zip"
SHA256 = "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"
# Not needed to build for x86-64: the other targets' libraries, the bundled Python, the debugger and the editor tools.
SKIP_DIRS = ("aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/", "python/")
SKIP_BIN = ("lldb", "liblldb", "libpython", "clangd", "clang-tidy")


def root():
    return Path(paths.get("MINGW"))


def version():
    """The installed release ('' when there is none)."""
    clang = root() / "bin" / "clang++.exe"
    if not clang.exists():
        return ""
    stamp = root() / "release.txt"
    if stamp.exists():
        return stamp.read_text().strip()
    r = subprocess.run([str(clang), "--version"], capture_output=True, text=True, errors="replace")
    return r.stdout.split("\n", 1)[0].strip() or "unknown"


def wanted(name):
    rel = name.split("/", 1)[1] if "/" in name else ""
    if not rel or rel.startswith(SKIP_DIRS):
        return False
    return not (rel.startswith("bin/") and rel[4:].startswith(SKIP_BIN))


def download(dest):
    print(f"downloading {URL}", flush=True)
    h = hashlib.sha256()
    with urllib.request.urlopen(URL, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done, last = 0, 0.0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if time.time() - last > 0.5:
                last = time.time()
                paths.progress(done >> 20, max(total >> 20, 1), "downloading llvm-mingw (MB)")
    if h.hexdigest() != SHA256:
        dest.unlink(missing_ok=True)
        sys.exit(f"the download's SHA-256 is {h.hexdigest()}, not {SHA256}: try again (a proxy or a cut-off download?)")


def install():
    dst = root()
    if version():
        print(f"llvm-mingw {version()} is already in {dst}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    zpath = dst.parent / f"{NAME}.zip"
    if not zpath.exists() or hashlib.sha256(zpath.read_bytes()).hexdigest() != SHA256:
        download(zpath)
    tmp = dst.parent / (dst.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    with zipfile.ZipFile(zpath) as z:
        members = [m for m in z.infolist() if wanted(m.filename)]
        print(f"unpacking {len(members)} files into {dst}", flush=True)
        prefix = NAME + "/"
        for i, m in enumerate(members):
            if not m.filename.startswith(prefix) or ".." in m.filename:
                sys.exit(f"unexpected file in the archive: {m.filename}")
            out = tmp / m.filename[len(prefix):]
            if m.is_dir():
                out.mkdir(parents=True, exist_ok=True)
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(m) as src, open(out, "wb") as f:
                shutil.copyfileobj(src, f, 1 << 20)
            if i % 200 == 0:
                paths.progress(i, len(members), "unpacking llvm-mingw")
    (tmp / "release.txt").write_text(f"{RELEASE} (LLVM 23.1.4)\n")
    if dst.exists():
        shutil.rmtree(dst)
    tmp.rename(dst)
    zpath.unlink()
    print(f"llvm-mingw {RELEASE} is in {dst}", flush=True)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        v = version()
        print(f"mingw {v}" if v else "missing")
    elif cmd == "install":
        install()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
