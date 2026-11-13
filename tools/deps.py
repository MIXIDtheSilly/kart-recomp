"""The Python side of the build's dependencies, for the launcher's Dependencies step.

  python tools\\deps.py check     prints "python <version>" and "missing <package> ..." (the required ones not installed)
  python tools\\deps.py install   installs what requirements.txt lists (pip first when this Python has none)

Required: capstone, and backports.zstd before Python 3.14. lz4 is optional (nso.py decodes without it, slower), so a
Python too new to have an lz4 wheel yet still builds.
"""
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def required():
    need = [("capstone", "capstone>=5,<6")]
    if sys.version_info < (3, 14):
        need.append(("backports.zstd", "backports.zstd"))
    return need


def installed(module):
    try:
        return importlib.util.find_spec(module) is not None
    except ModuleNotFoundError:   # a dotted name whose parent package is missing
        return False


def missing():
    return [name for name, _ in required() if not installed(name)]


def pip(*args):
    print("> pip " + " ".join(args), flush=True)
    return subprocess.run([sys.executable, "-m", "pip", "--disable-pip-version-check", *args]).returncode


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        print("python", sys.version.split()[0])
        print("missing", *missing())
        return
    if cmd != "install":
        sys.exit(__doc__)
    if not installed("pip"):
        print("this Python has no pip: adding it (ensurepip)", flush=True)
        if subprocess.run([sys.executable, "-m", "ensurepip", "--upgrade"]).returncode:
            sys.exit("ensurepip failed: reinstall Python from python.org with pip ticked")
    # Wheels only: building from source would need compilers set up for Python. A Python with no wheel yet for a
    # required package has to wait for one (or use an older Python: 3.13 works).
    need = [spec for name, spec in required() if not installed(name)]
    if need and pip("install", "--only-binary=:all:", *need):
        sys.exit("could not install " + ", ".join(need) + f" for Python {sys.version.split()[0]}: try Python 3.13 from python.org")
    if not installed("lz4") and pip("install", "--only-binary=:all:", "lz4"):
        print("lz4 has no wheel for this Python: unpacking uses the slower built-in decoder instead", flush=True)
    left = missing()
    if left:
        sys.exit("still missing: " + ", ".join(left))
    print("Python packages: all there", flush=True)


if __name__ == "__main__":
    main()
