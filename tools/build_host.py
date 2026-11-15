r"""Builds host\out\kart.exe (kart-<KART_VER>.exe) with llvm-mingw's clang: what host\build.bat does with Visual Studio.
host\build.bat runs it when BUILD_CC=mingw (the default; tools\toolchain.py downloads llvm-mingw; see paths.py).
On Linux (host/build.sh runs it) it builds host/out/kart (kart-<KART_VER>) with clang from LLVM, objects in
host/out/obj-linux, against the system's C and C++ libraries and SDL 3 (libSDL3.so.0: window, input, sound; the
headers are in third_party/SDL3).

Objects go to host\out\obj-mingw. A file is compiled again only when it, a header it includes (clang's .d files) or the
flags changed: <obj>.k records each such file's time, size and hash, so a file unzipped from a release with an older
time than the object still counts as changed. The exe links the module libraries build_lib.py made (<art>\build\mingw\<module>.a) and everything
statically (libc++, libunwind, winpthreads): it needs no DLL beyond Windows' own. BUILD_MP=<n>: at most n compilers
at once (default: one per core).
"""
import hashlib
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path, PureWindowsPath

import paths

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "host"
TP = ROOT / "third_party"
OUT = HOST / "out"
LINUX = paths.LINUX
OBJ = OUT / ("obj-linux" if LINUX else "obj-mingw")

DEFS = ["-DNDEBUG", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-DUNICODE", "-D_UNICODE", "-D_CRT_SECURE_NO_WARNINGS",
        "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00"]
# -fms-extensions: __try/__except (main.cpp) and the MSVC intrinsics; -mcx16: the 128-bit compare-and-swap.
# -g -gcodeview: a PDB beside the exe (--pdb below), which the stall and crash reports read function names from.
CXX = ["-std=c++20", "-O2", "-g", "-gcodeview", "-fms-extensions", "-mcx16", "-ffp-contract=off", "-w", *DEFS]
if LINUX:
    # -g1: line tables only (the crash and stall reports name functions from the symbol table); -fno-pie: kart links at
    # a fixed address, like the game's modules (build_lib.py LINUX_FLAGS)
    CXX = ["-std=c++20", "-O2", "-g1", "-mcx16", "-ffp-contract=off", "-fno-pie", "-pthread", "-w", "-DNDEBUG"]
HOST_FLAGS = [*CXX, "-fno-rtti", f"-I{HOST}", f"-I{ROOT / 'runtime'}", *(f"-I{TP / d}" for d in
              ("vulkan/include", "vma", "lz4", "zstd", "opus/include", "stb", *(["SDL3/include"] if LINUX else [])))]
AUDIO_FLAGS = [*CXX, f"-I{HOST}", *([f"-I{TP / 'SDL3/include'}"] if LINUX else [])]   # audio_core (adapted from Eden) uses RTTI
C_FLAGS = ["-O2", "-w", "-DNDEBUG", *(["-fno-pie"] if LINUX else ["-D_CRT_SECURE_NO_WARNINGS"])]
OPUS = ["-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINT", "-DHAVE_LRINTF", "-DDISABLE_DEBUG_FLOAT",
        *(f"-I{TP / 'opus' / d}" for d in ("", "include", "celt", "silk", "silk/float"))]
LIBS = ["user32", "gdi32", "shell32", "ole32", "oleaut32", "advapi32", "winmm", "bcrypt", "ntdll", "ws2_32", "uuid",
        "version", "setupapi", "dbghelp", "imm32", "cfgmgr32", "avrt", "ksuser", "hid", "xinput1_4", "synchronization",
        "mfplat", "mfuuid", "iphlpapi"]   # xinput1_4: mingw's libxinput.a is XINPUT1_3.dll, which Windows does not have
# Linux: SDL 3 by its soname (distributions ship libSDL3.so.0 with the system; the unversioned libSDL3.so link only comes
# with the -devel package), dlopen for the Vulkan loader and libvpx, pthread.
LINUX_LIBS = ["-l:libSDL3.so.0", "-ldl", "-lpthread"]


def sources(art):
    """(source, compiler, flags) for everything kart.exe is made of."""
    cpp = [*HOST.glob("*.cpp"), *(HOST / "services").glob("*.cpp"), *(HOST / "nvn").glob("*.cpp"),
           *(HOST / "nvn" / "vk").glob("*.cpp"), art / "gen" / "aot_modules.cpp"]
    out = [(s, "clang++", HOST_FLAGS) for s in cpp]
    out += [(s, "clang++", AUDIO_FLAGS) for s in sorted((HOST / "audio_core").rglob("*.cpp"))]
    out += [(TP / "lz4" / "lz4.c", "clang", C_FLAGS)]
    out += [(s, "clang", [*C_FLAGS, "-DZSTD_DISABLE_ASM", f"-I{TP / 'zstd'}"])
            for d in ("common", "decompress") for s in sorted((TP / "zstd" / d).glob("*.c"))]
    out += [(TP / "stb" / "stb_dxt.cpp", "clang++", [*C_FLAGS, f"-I{TP / 'stb'}"])]
    out += [(s, "clang", [*C_FLAGS, *OPUS]) for d in ("celt", "silk", "silk/float", "src")
            for s in sorted((TP / "opus" / d).glob("*.c"))]
    return out


def obj_of(src):
    try:
        rel = src.relative_to(ROOT)
    except ValueError:
        rel = Path(src.name)
    if src.name == "aot_modules.cpp":   # one per game version
        v = paths.get("KART_VER")
        rel = Path("aot_modules" + (f"-{v}" if v else "") + ".cpp")
    return OBJ / (str(rel.with_suffix("")).replace("\\", "_").replace("/", "_") + ".o")


def deps(d):
    """The files a .d file lists (make syntax: backslash-newline continues a line, a backslash escapes a space)."""
    try:
        text = d.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
    except OSError:
        return None
    _, _, rhs = text.partition(": ")
    return [p.replace("\\ ", " ") for p in re.split(r"(?<!\\)\s+", rhs) if p]


def digest(f):
    with open(f, "rb") as fh:
        return hashlib.blake2b(fh.read(), digest_size=16).hexdigest()


def record(files):
    """What an object was compiled from (<obj>.k): each file's time, size and contents' hash. 'A file newer than the
    object' is not enough: files unzipped from a release keep the release's times, which can be older than an object
    an earlier version compiled."""
    lines = []
    for f in files:
        try:
            st = os.stat(f)
            lines.append(f"{f}\t{st.st_mtime_ns}\t{st.st_size}\t{digest(f)}")
        except OSError:
            lines.append(f"{f}\t-\t-\t-")
    return "\n".join(lines)


def changed(rec):
    """A file of the record that is not as it was: its time or size moved and its contents differ."""
    for line in rec.splitlines():
        f, t, size, h = line.split("\t")
        try:
            st = os.stat(f)
        except OSError:
            return True   # a header that is gone
        if (str(st.st_mtime_ns), str(st.st_size)) != (t, size) and digest(f) != h:
            return True
    return False


def stale(src, obj):
    if not obj.exists():
        return True
    files = deps(obj.with_suffix(".d"))
    if files is None:
        return True
    k = obj.with_suffix(".k")
    try:
        rec = k.read_text(encoding="utf-8")
    except OSError:
        rec = None
    if rec is not None:
        return [l.split("\t")[0] for l in rec.splitlines()] != files or changed(rec)
    # An object from before the records: by time, and recorded from now on
    t = obj.stat().st_mtime
    for f in files:
        try:
            if os.stat(f).st_mtime > t:
                return True
        except OSError:
            return True
    k.write_text(record(files), encoding="utf-8")
    return False


def fmt(sec):
    sec = int(sec)
    return f"{sec // 60}:{sec % 60:02}"


def main():
    t0 = time.time()
    env = paths.llvm_env() if LINUX else paths.mingw_env()
    bindir = Path(paths.get("LLVM" if LINUX else "MINGW")) / "bin"
    art = paths.art()
    exe = OUT / f"kart{'-' + paths.get('KART_VER') if paths.get('KART_VER') else ''}{paths.EXE}"
    libdir = "linux" if LINUX else "mingw"
    rsp = art / "gen" / "aot_libs.rsp"
    if not rsp.exists():
        sys.exit(f"{rsp} is missing: run python tools\\gen_glue.py first")
    # (either separator: the list may have been written on the other system)
    mods = [PureWindowsPath(l.strip().strip('"')).stem for l in rsp.read_text().splitlines() if l.strip()]
    libs = [art / "build" / libdir / f"{m}.a" for m in mods]
    missing = [str(l) for l in libs if not l.exists()]
    if missing:
        sys.exit("module libraries missing (python tools/build_all.py makes them" + ("" if LINUX else " with BUILD_CC=mingw") + "): "
                 + ", ".join(missing))

    OBJ.mkdir(parents=True, exist_ok=True)
    # New flags or another llvm-mingw: compile everything again
    stamp = OBJ / "flags.txt"
    key = "\n".join([str(bindir), *HOST_FLAGS, *AUDIO_FLAGS, *C_FLAGS, *OPUS])
    if not stamp.exists() or stamp.read_text() != key:
        for o in OBJ.glob("*.o"):
            o.unlink()
        stamp.write_text(key)

    todo = [(s, cc, fl) for s, cc, fl in sources(art) if stale(s, obj_of(s))]
    allsrc = sources(art)
    workers = int(os.environ.get("BUILD_MP") or os.cpu_count())
    print(f"{len(allsrc)} host sources, {len(todo)} to compile ({workers} at a time) with {'clang' if LINUX else 'llvm-mingw clang'}", flush=True)

    def one(item):
        s, cc, fl = item
        o = obj_of(s)
        tmp = o.with_suffix(".o.tmp")
        r = subprocess.run([str(bindir / f"{cc}{paths.EXE}"), "-c", *fl, "-MMD", "-MF", str(o.with_suffix(".d")), "-o", str(tmp), str(s)],
                           env=env, capture_output=True, text=True, errors="replace")
        if r.returncode == 0:
            os.replace(tmp, o)
            files = deps(o.with_suffix(".d"))
            if files is not None:
                o.with_suffix(".k").write_text(record(files), encoding="utf-8")
        else:
            tmp.unlink(missing_ok=True)
        return s, r

    failed = []
    if todo:
        paths.progress(0, len(todo), "host: compiling")
        with ThreadPoolExecutor(workers) as ex:
            for i, f in enumerate(as_completed([ex.submit(one, it) for it in todo]), 1):
                s, r = f.result()
                if r.returncode:
                    failed.append(s)
                    print(f"{s.relative_to(ROOT) if s.is_relative_to(ROOT) else s} failed:\n{(r.stdout + r.stderr).strip()[-4000:]}", flush=True)
                paths.progress(i, len(todo), "host: compiling")
    if failed:
        sys.exit(f"build failed ({len(failed)} of {len(todo)} files did not compile; the compiler's output is above)")
    print(f"compiled in {fmt(time.time() - t0)}, linking {exe.name}", flush=True)

    # The objects and libraries through a response file (forward slashes: clang reads it the GNU way)
    link_rsp = OBJ / f"{exe.stem}_link.rsp"
    q = lambda p: '"' + Path(p).as_posix() + '"'
    link_rsp.write_text("\n".join([*(q(obj_of(s)) for s, _, _ in allsrc), "-Wl,--start-group", *map(q, libs),
                                   "-Wl,--end-group"]))
    if LINUX:
        # lld; no PIE (the module tables hold 300 k function addresses: no relocations to apply at start); the symbol table
        # stays in (main.cpp names functions from it in crash and stall reports)
        cmd = [str(bindir / "clang++"), "-fuse-ld=lld", "-no-pie", "-pthread", "-o", str(exe), f"@{link_rsp}", *LINUX_LIBS]
    else:
        pdb = exe.with_suffix(".pdb")
        cmd = [str(bindir / "clang++.exe"), "-static", "-g", f"-Wl,--pdb={pdb}", "-o", str(exe), f"@{link_rsp}",
               *(f"-l{l}" for l in LIBS)]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
    out = (r.stdout + r.stderr).strip()
    if r.returncode:
        hint = "\n(is the game running? close it and build again)" if "permission denied" in out.lower() else ""
        sys.exit(f"link failed:\n{out[-6000:]}{hint}")
    if out:
        print(out[-3000:])
    print(f"built {exe} ({exe.stat().st_size / 2**20:.0f} MiB) in {fmt(time.time() - t0)}", flush=True)


if __name__ == "__main__":
    main()
