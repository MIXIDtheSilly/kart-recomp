"""Compile a module's generated C++ into a static library with MSVC.

Usage: python build_lib.py <gen dir> <module> <build dir> [--opt /O2]

Produces <build dir>/<module>.lib from <gen dir>/<module>_*.cpp. Objects are only rebuilt
when their source is newer, so re-running after lifting a subset is cheap.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VCVARS = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gen")
    ap.add_argument("module")
    ap.add_argument("build")
    ap.add_argument("--opt", default="/O2")
    a = ap.parse_args()

    gen, build = Path(a.gen).resolve(), Path(a.build).resolve()
    build.mkdir(parents=True, exist_ok=True)
    sources = sorted(gen.glob(f"{a.module}_*.cpp"))
    stale = [s for s in sources if not (build / (s.stem + ".obj")).exists()
             or (build / (s.stem + ".obj")).stat().st_mtime < s.stat().st_mtime]
    print(f"{len(sources)} sources, {len(stale)} to compile")

    rsp = build / f"{a.module}.rsp"
    rsp.write_text("\n".join(f'"{s}"' for s in stale))
    objs = build / f"{a.module}_objs.rsp"
    objs.write_text("\n".join(f'"{build / (s.stem + ".obj")}"' for s in sources))
    lines = [f'@call "{VCVARS}" >nul']
    if stale:
        lines.append(f'cl /nologo /c {a.opt} /MD /std:c++20 /EHsc /bigobj /w /MP /I"{ROOT / "runtime"}" /Fo"{build}\\\\" @"{rsp}" || exit /b 1')
    lines.append(f'lib /nologo /out:"{build / (a.module + ".lib")}" @"{objs}" || exit /b 1')
    bat = build / f"build_{a.module}.bat"
    bat.write_text("\r\n".join(lines) + "\r\n")

    t = time.time()
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, errors="replace")
    errors = [l for l in r.stdout.splitlines() if " error " in l or "fatal error" in l]
    if r.returncode:
        print("\n".join(errors[:30]) or r.stdout[-4000:])
        sys.exit(f"build failed ({len(errors)} errors)")
    size = (build / (a.module + ".lib")).stat().st_size
    print(f"built {a.module}.lib ({size / 2**20:.0f} MiB) in {time.time() - t:.0f} s")


if __name__ == "__main__":
    main()
