"""Where the dependencies, the game dump and the compilers are: one place for the build scripts and tools.

Each setting comes from the environment, else from local.cfg at the repository root (KEY=VALUE lines, # starts
a comment; the file is not committed), else from the default below.

  EGL        Eglantine source with patches/eglantine.patch applied         (deps/Eglantine)
  EGL_BUILD  its CMake build folder: video_core.lib, audio_core.lib, ...    (deps/egl-build)
  NVN_REF    plumber-racer-2 source with patches/plumber-racer-2.patch      (deps/plumber-racer-2)
             applied: the shader splitter and the shader translator
  NVN_XLATE  folder of the built shader translator, dmsh_xlate.exe          (under NVN_REF)
  KART_GAME  the game dump: the folder holding exefs/ and romfs/            (none)
  VCVARS     Visual Studio's vcvars64.bat                                   (found with vswhere)
  CLANG      clang-cl.exe, for BUILD_CC=clang                               (C:\\Program Files\\LLVM\\bin)

tools/setup_deps.py fetches and builds the dependencies at the default places.

From batch files:
  python tools\\paths.py bat                    KEY=VALUE lines, for `for /f ... do set "%%L"`
  python tools\\paths.py rsp <template> <out>   the template with every $(KEY) replaced ($(ROOT): the repository)
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYS = ["EGL", "EGL_BUILD", "NVN_REF", "NVN_XLATE", "KART_GAME", "VCVARS", "CLANG"]


def _local():
    cfg = {}
    f = ROOT / "local.cfg"
    if f.exists():
        for line in f.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"')
    return cfg


def _find_vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        r = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            bat = Path(line.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if bat.exists():
                return str(bat)
    return r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"


def get(key):
    """The setting's value ('' when there is none)."""
    if os.environ.get(key):
        return os.environ[key]
    local = _local()
    if local.get(key):
        v = local[key]
        return str((ROOT / v).resolve()) if key != "VCVARS" and key != "CLANG" and not Path(v).is_absolute() else v
    if key == "EGL":
        return str(ROOT / "deps" / "Eglantine")
    if key == "EGL_BUILD":
        return str(ROOT / "deps" / "egl-build")
    if key == "NVN_REF":
        return str(ROOT / "deps" / "plumber-racer-2")
    if key == "NVN_XLATE":
        return str(Path(get("NVN_REF")) / "dm" / "gfx" / "tools" / "dmsh_xlate" / "bin" / "Release" / "net10.0")
    if key == "VCVARS":
        return _find_vcvars()
    if key == "CLANG":
        return r"C:\Program Files\LLVM\bin\clang-cl.exe"
    return ""


def game(sub=""):
    """The game dump folder (or a path inside it); exits with a message when KART_GAME is not set."""
    g = get("KART_GAME")
    if not g:
        sys.exit("KART_GAME is not set: put KART_GAME=<dump folder holding exefs\\ and romfs\\> in local.cfg")
    return str(Path(g) / sub) if sub else g


def vcvars_env():
    """The environment vcvars64.bat sets up (compilers, include and library paths, CMake and Ninja)."""
    out = subprocess.run(f'cmd /c ""{get("VCVARS")}" >nul && set"', capture_output=True, text=True, errors="replace").stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
    if "INCLUDE" not in env:
        sys.exit(f"vcvars64.bat failed ({get('VCVARS')}): install Visual Studio with the C++ workload, or set VCVARS in local.cfg")
    return env


def main():
    if sys.argv[1:2] == ["bat"]:
        for k in KEYS:
            print(f"{k}={get(k)}")
    elif sys.argv[1:2] == ["rsp"] and len(sys.argv) == 4:
        text = Path(sys.argv[2]).read_text(encoding="utf-8")
        text = text.replace("$(ROOT)", str(ROOT))
        for k in KEYS:
            text = text.replace(f"$({k})", get(k))
        Path(sys.argv[3]).parent.mkdir(parents=True, exist_ok=True)
        Path(sys.argv[3]).write_text(text, encoding="utf-8")
    else:
        for k in KEYS:
            print(f"{k:10} {get(k)}")


if __name__ == "__main__":
    main()
