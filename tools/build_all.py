"""Lift and build every module of the game into static libraries.

Usage: python build_all.py [module ...]   (default: every module nso.py unpacked, out/layout.json)
"""
import json
import subprocess
import sys
from pathlib import Path

import paths

ROOT = paths.art()  # holds out/, gen/ and build/ (ver/<KART_VER>/ for another game version)
TOOLS = Path(__file__).resolve().parent


def modules():
    """The dump's modules in load order, as nso.py found them (1.0.0: rtld main subsdk0 sdk; 1.8.0 adds subsdk1)."""
    return [m["name"] for m in json.loads((ROOT / "out" / "layout.json").read_text())]


def hot_list(m):
    """tools/hot_<module>_<first 8 hex digits of its build id>.txt: the list belongs to one game version."""
    mid = json.loads((ROOT / "out" / f"{m}.json").read_text())["module_id"]
    return TOOLS / f"hot_{m}_{mid[:8]}.txt"


def run(*args):
    r = subprocess.run([sys.executable, *map(str, args)])
    if r.returncode:
        sys.exit(r.returncode)


def main():
    mods = sys.argv[1:] or modules()
    for i, m in enumerate(mods, 1):
        print(f"== {m} (module {i} of {len(mods)})", flush=True)
        # tools/hot_<module>_<id>.txt: the functions MSVC finds too big to optimize (C4883; list them with
        # BUILD_WARN_HUGE=1, build/<module>_cl.log), compiled apart with its huge-function optimization. A module
        # without a list (the small ones, or a game version nobody listed yet) gets it for every file.
        hot = hot_list(m)
        run(TOOLS / "lift.py", ROOT / "out", m, ROOT / "gen", "--chunk", "600", *(["--hot", hot] if hot.exists() else []))
        run(TOOLS / "build_lib.py", ROOT / "gen", m, ROOT / "build", *([] if hot.exists() else ["--huge-all"]))


if __name__ == "__main__":
    main()
