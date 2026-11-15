r"""The C++ toolchain the build uses without Visual Studio: llvm-mingw (clang, lld and the MinGW-w64 Windows headers and
libraries, https://github.com/mstorsjo/llvm-mingw), downloaded into MINGW (tools\llvm-mingw, see paths.py).
On Linux: LLVM's own release of the same clang (https://github.com/llvm/llvm-project/releases), downloaded into LLVM
(tools/llvm); it uses the system's C++ library (libstdc++ and the glibc headers: the distribution's g++ package).

  python tools\toolchain.py check     prints "mingw <version>" (Linux: "llvm <version>") when it is there, else "missing"
  python tools\toolchain.py install   downloads the pinned release (190 MB; Linux 1.2 GB, 2 GB before Python 3.14), checks its SHA-256 and
                                      unpacks the parts an x86-64 build uses (about 450 MB; Linux 600 MB)

The release is pinned: a newer clang could change the generated code. To move to another one, change RELEASE and
SHA256 together (Linux: LLVM_RELEASE and LLVM_SHA256).
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


# ------------------------------------------------------------------ Linux: LLVM's release

LLVM_RELEASE = "23.1.4"   # the LLVM in llvm-mingw RELEASE: the game compiles to the same code on both systems
LLVM_NAME = f"LLVM-{LLVM_RELEASE}-Linux-X64"
LLVM_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_RELEASE}/{LLVM_NAME}.tar.zst"
LLVM_SHA256 = "efdd1dbd089b99bfcfcde7b8eb1236163f2e88df5a981cca4f783ec9cb5e0378"
# Before Python 3.14 (no zstd module) the same release packed with xz: 2 GB instead of 1.2
LLVM_XZ_URL = LLVM_URL[:-len(".tar.zst")] + ".tar.xz"
LLVM_XZ_SHA256 = "25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3"
# What a build runs (the release has 12 GB: MLIR, Flang, LLDB and the rest stay out). Links (clang++ -> clang -> clang-23)
# are kept as links: clang picks C or C++ by the name it is started as.
LLVM_BIN = ("clang", "clang++", "clang-23", "lld", "ld.lld", "llvm-ar", "llvm-ranlib", "llvm-symbolizer", "llvm-addr2line",
            "llvm-objdump", "llvm-nm")


def llvm_root():
    return Path(paths.get("LLVM"))


def llvm_version():
    clang = llvm_root() / "bin" / "clang++"
    if not clang.exists():
        return ""
    stamp = llvm_root() / "release.txt"
    return stamp.read_text().strip() if stamp.exists() else "unknown"


def llvm_wanted(name):
    parts = name.split("/")[1:]
    if not parts:
        return False
    if parts[0] == "bin":
        return len(parts) == 2 and parts[1] in LLVM_BIN
    return parts[:2] == ["lib", "clang"]   # clang's own headers (x86 intrinsics, stddef.h ...) and runtime libraries


def llvm_install():
    import tarfile
    dst = llvm_root()
    if llvm_version():
        print(f"LLVM {llvm_version()} is already in {dst}")
        return
    try:
        from compression.zstd import DecompressionParameter   # Python 3.14
        url, sha, ext = LLVM_URL, LLVM_SHA256, ".tar.zst"
        # The archive is one zstd frame with a 2 GiB window (zstd --long): the decoder must be allowed that much.
        open_args = {"mode": "r:zst", "options": {DecompressionParameter.window_log_max: 31}}
    except ImportError:
        url, sha, ext = LLVM_XZ_URL, LLVM_XZ_SHA256, ".tar.xz"
        open_args = {"mode": "r:xz"}
    dst.parent.mkdir(parents=True, exist_ok=True)
    tpath = dst.parent / f"{LLVM_NAME}{ext}"
    if not tpath.exists() or file_sha256(tpath) != sha:
        fetch(url, tpath, sha, "downloading LLVM (MB)")
    tmp = dst.parent / (dst.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    prefix = LLVM_NAME + "/"
    n = 0
    with tarfile.open(tpath, **open_args) as t:
        print(f"unpacking clang, lld and llvm-ar into {dst}", flush=True)
        for m in t:
            if not llvm_wanted(m.name):
                continue
            if not m.name.startswith(prefix) or ".." in m.name.split("/"):
                sys.exit(f"unexpected file in the archive: {m.name}")
            out = tmp / m.name[len(prefix):]
            if m.isdir():
                out.mkdir(parents=True, exist_ok=True)
            elif m.issym():
                if "/" in m.linkname or ".." in m.linkname:
                    sys.exit(f"unexpected link in the archive: {m.name} -> {m.linkname}")
                out.parent.mkdir(parents=True, exist_ok=True)
                out.symlink_to(m.linkname)
            elif m.isfile():
                out.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, open(out, "wb") as f:
                    shutil.copyfileobj(src, f, 1 << 20)
                out.chmod(0o755 if m.mode & 0o111 else 0o644)
            n += 1
            if n % 100 == 0:
                paths.progress(n, 1400, "unpacking LLVM")
    (tmp / "release.txt").write_text(f"{LLVM_RELEASE}\n")
    if dst.exists():
        shutil.rmtree(dst)
    tmp.rename(dst)
    tpath.unlink()
    print(f"LLVM {LLVM_RELEASE} is in {dst}", flush=True)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, dest, sha256, what):
    print(f"downloading {url}", flush=True)
    h = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done, last = 0, 0.0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if time.time() - last > 0.5:
                last = time.time()
                paths.progress(done >> 20, max(total >> 20, 1), what)
    if h.hexdigest() != sha256:
        dest.unlink(missing_ok=True)
        sys.exit(f"the download's SHA-256 is {h.hexdigest()}, not {sha256}: try again (a proxy or a cut-off download?)")


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
    linux = sys.platform.startswith("linux")
    if cmd == "check":
        v = llvm_version() if linux else version()
        print((f"llvm {v}" if linux else f"mingw {v}") if v else "missing")
    elif cmd == "install":
        llvm_install() if linux else install()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
