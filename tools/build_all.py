"""Lift and build every module of the game into static libraries.

Usage: python build_all.py [module ...]   (default: rtld main subsdk0 sdk)
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
MODULES = ["rtld", "main", "subsdk0", "sdk"]


def run(*args):
    r = subprocess.run([sys.executable, *map(str, args)])
    if r.returncode:
        sys.exit(r.returncode)


def main():
    for m in sys.argv[1:] or MODULES:
        # work/hot_<module>.txt: the functions MSVC finds too big to optimize (C4883; list them with
        # BUILD_WARN_HUGE=1, build/<module>_cl.log), compiled apart with its huge-function optimization. A module
        # without a list (the small ones) gets it for every file.
        hot = ROOT / "work" / f"hot_{m}.txt"
        run(TOOLS / "lift.py", ROOT / "out", m, ROOT / "gen", "--chunk", "600", *(["--hot", hot] if hot.exists() else []))
        run(TOOLS / "build_lib.py", ROOT / "gen", m, ROOT / "build", *([] if hot.exists() else ["--huge-all"]))


if __name__ == "__main__":
    main()
