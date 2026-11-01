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
        run(TOOLS / "lift.py", ROOT / "out", m, ROOT / "gen", "--chunk", "600")
        run(TOOLS / "build_lib.py", ROOT / "gen", m, ROOT / "build")


if __name__ == "__main__":
    main()
