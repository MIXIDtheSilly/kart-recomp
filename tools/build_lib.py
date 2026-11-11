"""Compile a module's generated C++ into a static library with MSVC.

Usage: python build_lib.py <gen dir> <module> <build dir> [--opt /O2]

Produces <build dir>/<module>.lib from <gen dir>/<module>_*.cpp. Objects are only rebuilt
when their source is newer, so re-running after lifting a subset is cheap.
"""
import argparse
import os
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
    warn = "/W1 /w14883" if os.environ.get("BUILD_WARN_HUGE") else "/w"
    objs = build / f"{a.module}_objs.rsp"
    objs.write_text("\n".join(f'"{build / (s.stem + ".obj")}"' for s in sources))
    lines = [f'@call "{VCVARS}" >nul']
    for kind, group in (("", [s for s in stale if "_hot_" not in s.stem]), ("_hot", [s for s in stale if "_hot_" in s.stem])):
        if not group:
            continue
        rsp = build / f"{a.module}{kind}.rsp"
        rsp.write_text("\n".join(f'"{s}"' for s in group))
        # BUILD_FORCE_INLINE=1: the hot files also get the runtime helpers forced inline (RT_FORCE_INLINE in
        # runtime/cpu.h). Off by default: no measured fps gain, ~3 min a file more, and cl needs a few GB a file.
        huge = " /d2OptimizeHugeFunctions" if kind or a.huge_all else ""
        if kind and os.environ.get("BUILD_FORCE_INLINE"):
            huge += " /DRT_FORCE_INLINE"
        lines.append(f'cl /nologo /c {a.opt}{huge} /MD /std:c++20 /EHsc /bigobj {warn} /MP{os.environ.get("BUILD_MP", "")} /I"{ROOT / "runtime"}" /Fo"{build}\\\\" @"{rsp}" || exit /b 1')
    lines.append(f'lib /nologo /out:"{lib}" @"{objs}" || exit /b 1')
    bat = build / f"build_{a.module}.bat"
    bat.write_text("\r\n".join(lines) + "\r\n")

    t = time.time()
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, errors="replace")
    (build / f"{a.module}_cl.log").write_text(r.stdout, errors="replace")
    errors = [l for l in r.stdout.splitlines() if " error " in l or "fatal error" in l]
    if r.returncode:
        print("\n".join(errors[:30]) or r.stdout[-4000:])
        sys.exit(f"build failed ({len(errors)} errors)")
    size = lib.stat().st_size
    print(f"built {a.module}.lib ({size / 2**20:.0f} MiB) in {time.time() - t:.0f} s")


CLANG = paths.get("CLANG")


def vcvars_env():
    """The environment vcvars64.bat sets up (include and library paths for clang-cl and lib)."""
    out = subprocess.run(f'cmd /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, errors="replace").stdout
    return dict(l.split("=", 1) for l in out.splitlines() if "=" in l)


def build_clang(a, sources, stale, build, lib, arch, stamp, key):
    """clang-cl takes one file at a time here (it has no /MP): BUILD_MP files compile in parallel.

    BUILD_ARCH (default -march=native) sets the target CPU: -march=native was 82 -> 89.6 fps in the scripted race
    (clang without it: 82, the same as cl), but the build then only runs on CPUs with the same features.
    -ffp-contract=off: never fuse a separate guest multiply and add into an FMA (the guest rounds twice)."""
    from concurrent.futures import ThreadPoolExecutor
    env = vcvars_env()
    flags = [CLANG, "/nologo", "/c", a.opt, "/MD", "/std:c++20", "/EHsc", "/bigobj", "/w", "-ffp-contract=off",
             *arch.split(), f"/I{ROOT / 'runtime'}"]
    t = time.time()
    if stale and stamp.exists():
        stamp.unlink()  # a failed or partial build must not look finished for the new flags

    def one(s):
        extra = ["/DRT_FORCE_INLINE"] if "_hot_" in s.stem and os.environ.get("BUILD_FORCE_INLINE") else []
        r = subprocess.run([*flags, *extra, f"/Fo{build / (s.stem + '.obj')}", str(s)], env=env,
                           capture_output=True, text=True, errors="replace")
        return s, r

    failed = []
    with ThreadPoolExecutor(int(os.environ.get("BUILD_MP") or os.cpu_count())) as ex:
        # The biggest files first, so one of them does not start last and hold up the end of the build.
        for s, r in ex.map(one, sorted(stale, key=lambda s: -s.stat().st_size)):
            if r.returncode:
                failed.append(s)
                print(f"{s.name}:\n{(r.stdout + r.stderr)[-3000:]}")
    if failed:
        sys.exit(f"build failed ({len(failed)} files)")
    objs = build / f"{a.module}_objs.rsp"
    objs.write_text("\n".join(f'"{build / (s.stem + ".obj")}"' for s in sources))
    r = subprocess.run(["lib", "/nologo", f"/out:{lib}", f"@{objs}"], env=env, capture_output=True, text=True,
                       errors="replace", shell=True)
    if r.returncode:
        sys.exit(f"lib failed:\n{r.stdout[-3000:]}")
    stamp.write_text(key)
    print(f"built {a.module}.lib with clang-cl {arch} ({lib.stat().st_size / 2**20:.0f} MiB) in {time.time() - t:.0f} s")


if __name__ == "__main__":
    main()
