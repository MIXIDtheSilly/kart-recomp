"""Compile a module's generated C++ into a static library with MSVC.

Usage: python build_lib.py <gen dir> <module> <build dir> [--opt /O2]

Produces <build dir>/<module>.lib from <gen dir>/<module>_*.cpp. Objects are only rebuilt
when their source is newer, so re-running after lifting a subset is cheap.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import paths

ROOT = Path(__file__).resolve().parent.parent
VCVARS = paths.get("VCVARS")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gen")
    ap.add_argument("module")
    ap.add_argument("build")
    ap.add_argument("--opt", default="/O2")
    ap.add_argument("--huge-all", action="store_true", help="every file with the huge-function optimization")
    a = ap.parse_args()

    gen, build = Path(a.gen).resolve(), Path(a.build).resolve()
    lib = build / (a.module + ".lib")
    # BUILD_CC=clang: clang-cl (LLVM) instead of cl, objects in <build>/clang so the cl ones stay for switching back.
    clang = os.environ.get("BUILD_CC", "cl").lower() in ("clang", "clang-cl")
    # BUILD_PGO=gen: instrumented objects (link clang_rt.profile, run with KART_PGO_DUMP); BUILD_PGO=<x.profdata>:
    # optimized with that profile. Each has its own object folder.
    pgo = os.environ.get("BUILD_PGO", "")
    if clang:
        build = build / ("clang" if not pgo else "clang_pgogen" if pgo == "gen" else "clang_pgo")
    build.mkdir(parents=True, exist_ok=True)
    sources = sorted(gen.glob(f"{a.module}_*.cpp"))
    stale = [s for s in sources if not (build / (s.stem + ".obj")).exists()
             or (build / (s.stem + ".obj")).stat().st_mtime < s.stat().st_mtime]
    if clang:
        # The target CPU flags change every object: a change of BUILD_ARCH recompiles everything.
        arch = os.environ.get("BUILD_ARCH", "-march=native")
        if pgo == "gen":
            # No value profiling: KART_PGO_DUMP writes mid-run, while the game's threads still add to the runtime's
            # indirect-call target lists. Link kart.exe with /INCREMENTAL:NO: incremental linking pads the profile
            # data sections, and the write then crashed (lprofGetVPDataReader) leaving a cut-off file.
            arch += " -fprofile-instr-generate -mllvm -disable-vp=true"
        elif pgo:
            prof = Path(pgo).resolve()
            arch += f" -fprofile-instr-use={prof} -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
        stamp = build / f"{a.module}_arch.txt"
        # The stamp also holds the profile's time: a new profile recompiles everything.
        key = arch + (f" @{prof.stat().st_mtime_ns}" if pgo and pgo != "gen" else "")
        if not stamp.exists() or stamp.read_text() != key:
            stale = sources
    print(f"{len(sources)} sources, {len(stale)} to compile")
    if clang:
        return build_clang(a, sources, stale, build, lib, arch, stamp, key)

    # MSVC skips optimizing functions it finds too big (C4883, silent under /w): the hot ones (lift.py --hot, in
    # <module>_hot_*.cpp) are compiled with /d2OptimizeHugeFunctions, which takes far longer, the rest as they are.
    # BUILD_WARN_HUGE=1 reports the functions it skips (C4883) in <build>/<module>_cl.log. BUILD_MP=<n>: parallel compiles.
    warn = ["/W1", "/w14883"] if os.environ.get("BUILD_WARN_HUGE") else ["/w"]
    env = paths.vcvars_env()   # exits with a message when Visual Studio's C++ tools are missing
    cl = shutil.which("cl", path=env.get("PATH", env.get("Path", "")))
    if not cl:
        sys.exit("cl.exe not found after vcvars64.bat: install Visual Studio's \"Desktop development with C++\"")
    base = [cl, "/nologo", "/c", a.opt, "/MD", "/std:c++20", "/EHsc", "/bigobj", *warn, f"/I{ROOT / 'runtime'}"]

    def flags(s):
        hot = "_hot_" in s.stem
        # BUILD_FORCE_INLINE=1: the hot files also get the runtime helpers forced inline (RT_FORCE_INLINE in
        # runtime/cpu.h). Off by default: no measured fps gain, ~3 min a file more, and cl needs a few GB a file.
        return (["/d2OptimizeHugeFunctions"] if hot or a.huge_all else []) + \
               (["/DRT_FORCE_INLINE"] if hot and os.environ.get("BUILD_FORCE_INLINE") else [])

    t = time.time()
    log = open(build / f"{a.module}_cl.log", "w", encoding="utf-8", errors="replace")
    failed = compile_all(a.module, stale, build, lambda s, out: [*base, *flags(s), f"/Fo{out}", str(s)], env, log, t)
    if failed:
        sys.exit(f"build failed ({len(failed)} of {len(stale)} files did not compile; the compiler's output is above)")
    lib_all(a.module, sources, build, lib, env, log,
            lambda objs: compile_all(a.module, objs, build, lambda s, out: [*base, *flags(s), f"/Fo{out}", str(s)], env, log, t))
    print(f"built {a.module}.lib ({lib.stat().st_size / 2**20:.0f} MiB) in {fmt(time.time() - t)}")


def fmt(sec):
    sec = int(sec)
    return f"{sec // 3600}:{sec // 60 % 60:02}:{sec % 60:02}" if sec >= 3600 else f"{sec // 60}:{sec % 60:02}"


FILE_GIB, HOT_GIB = 1.0, 3.0   # a compiler on one generated file / one hot file: 0.63 / 2.25 GB at most (measured), plus margin


def free_ram_gib():
    import ctypes

    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [(f, ctypes.c_ulonglong) for f in
                    ("total", "avail", "ptotal", "pavail", "vtotal", "vavail", "xavail")]
    ms = MS()
    ms.len = ctypes.sizeof(MS)
    return ms.avail / 2**30 if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)) else 64.0


def compile_all(module, stale, build, cmd, env, log, t0):
    """Compiles <stale> BUILD_MP at a time (default: every core), the hot files first (they take the longest), then the
    biggest. Each object is written as <stem>.obj.tmp and renamed once the compiler succeeds, so a build that is stopped or
    runs out of memory never leaves a cut-off .obj that looks finished (LNK1136 on the next build). Returns the failed files."""
    from concurrent.futures import ThreadPoolExecutor, as_completed
    if not stale:
        return []
    import threading
    order = sorted(stale, key=lambda s: ("_hot_" not in s.stem, -s.stat().st_size))
    n, done, failed, last = len(order), 0, [], time.time()
    hot = sum("_hot_" in s.stem for s in order)
    workers = int(os.environ.get("BUILD_MP") or os.cpu_count())
    # As many compilers at once as the free RAM holds (a 16 GB PC running 8 swapped for hours, issue #10)
    budget = max(HOT_GIB, free_ram_gib() - 1.5)
    need = lambda s: HOT_GIB if "_hot_" in s.stem else FILE_GIB
    fit = min(workers, int(budget // FILE_GIB))
    print(f"compiling {n} files, up to {workers} at a time ({budget:.0f} GB of RAM free: {fit} files"
          + (f", {min(workers, int(budget // HOT_GIB))} of the hot ones" if hot else "") + ")"
          + (f"; the {hot} hot files first, several minutes each" if hot else ""), flush=True)
    paths.progress(0, n, f"{module}: compiling")
    mem = threading.Condition()
    in_use = [0.0]

    def one(s):
        obj = build / (s.stem + ".obj")
        tmp = build / (s.stem + ".obj.tmp")
        with mem:
            mem.wait_for(lambda: in_use[0] == 0 or in_use[0] + need(s) <= budget)
            in_use[0] += need(s)
        try:
            r = subprocess.run(cmd(s, tmp), env=env, capture_output=True, encoding="oem", errors="replace")
        finally:
            with mem:
                in_use[0] -= need(s)
                mem.notify_all()
        if r.returncode == 0 and tmp.exists():
            os.replace(tmp, obj)
        else:
            tmp.unlink(missing_ok=True)
        return s, r

    with ThreadPoolExecutor(workers) as ex:
        for f in as_completed([ex.submit(one, s) for s in order]):
            s, r = f.result()
            out = (r.stdout + r.stderr).strip()
            # cl echoes the file name first: only what follows is worth showing
            lines = [l for l in out.splitlines() if l.strip() and l.strip() != s.name]
            log.write(f"{s.name}: exit {r.returncode}\n" + "\n".join(lines) + "\n")
            done += 1
            if r.returncode:
                failed.append(s)
                print(f"{s.name} failed (exit code {r.returncode}):\n" + ("\n".join(lines[-40:]) or "(no output)"), flush=True)
            paths.progress(done, n, f"{module}: compiling")
            now = time.time()
            if now - last >= 30 or done == n:
                last = now
                el = now - t0
                eta = f", about {fmt(el / done * (n - done))} left" if done < n else ""
                print(f"  {done}/{n} compiled, {fmt(el)} so far{eta}", flush=True)
    return failed


def lib_all(module, sources, build, lib, env, log, recompile):
    """Archives the objects; an object the librarian finds corrupt (LNK1136, LNK1106: left by an older build that was cut
    off) is compiled again once."""
    objs = build / f"{module}_objs.rsp"
    objs.write_text("\n".join(f'"{build / (s.stem + ".obj")}"' for s in sources))
    libexe = shutil.which("lib", path=env.get("PATH", env.get("Path", ""))) or "lib"
    for attempt in range(2):
        r = subprocess.run([libexe, "/nologo", f"/out:{lib}", f"@{objs}"], env=env, capture_output=True, encoding="oem", errors="replace")
        out = r.stdout + r.stderr
        log.write(out)
        if r.returncode == 0:
            return
        bad = [Path(m) for m in re.findall(r"^(.*?\.obj) : fatal error LNK1\d\d\d", out, re.M)]
        if attempt == 0 and bad:
            print(f"{len(bad)} corrupt object(s) from an earlier build, compiling again: " + ", ".join(b.name for b in bad), flush=True)
            for b in bad:
                b.unlink(missing_ok=True)
            by_stem = {s.stem: s for s in sources}
            if recompile([by_stem[b.stem] for b in bad if b.stem in by_stem]):
                sys.exit("build failed")
            continue
        sys.exit(f"lib failed:\n{out[-3000:]}")


CLANG = paths.get("CLANG")


def build_clang(a, sources, stale, build, lib, arch, stamp, key):
    """clang-cl takes one file at a time here (it has no /MP): BUILD_MP files compile in parallel.

    BUILD_ARCH (default -march=native) sets the target CPU: -march=native was 82 -> 89.6 fps in the scripted race
    (clang without it: 82, the same as cl), but the build then only runs on CPUs with the same features.
    -ffp-contract=off: never fuse a separate guest multiply and add into an FMA (the guest rounds twice)."""
    env = paths.vcvars_env()
    flags = [CLANG, "/nologo", "/c", a.opt, "/MD", "/std:c++20", "/EHsc", "/bigobj", "/w", "-ffp-contract=off",
             *arch.split(), f"/I{ROOT / 'runtime'}"]
    t = time.time()
    if stale and stamp.exists():
        stamp.unlink()  # a failed or partial build must not look finished for the new flags

    def cmd(s, out):
        extra = ["/DRT_FORCE_INLINE"] if "_hot_" in s.stem and os.environ.get("BUILD_FORCE_INLINE") else []
        return [*flags, *extra, f"/Fo{out}", str(s)]

    log = open(build / f"{a.module}_cl.log", "w", encoding="utf-8", errors="replace")
    failed = compile_all(a.module, stale, build, cmd, env, log, t)
    if failed:
        sys.exit(f"build failed ({len(failed)} of {len(stale)} files did not compile; the compiler's output is above)")
    lib_all(a.module, sources, build, lib, env, log, lambda objs: compile_all(a.module, objs, build, cmd, env, log, t))
    stamp.write_text(key)
    print(f"built {a.module}.lib with clang-cl {arch} ({lib.stat().st_size / 2**20:.0f} MiB) in {fmt(time.time() - t)}")


if __name__ == "__main__":
    main()
