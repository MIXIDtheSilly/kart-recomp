r"""Where the game dump and the compilers are: one place for the build scripts and tools.

Each setting comes from the environment, else from local.cfg at the repository root (KEY=VALUE lines, # starts
a comment; the file is not committed), else from the default below.

  KART_GAME  the game dump: the folder holding exefs/ and romfs/            (none)
             (with KART_VER set, KART_GAME_<KART_VER> comes first)
  NVN_XLATE  folder the shader translator (dmsh_xlate.exe, from tools/xlate) is built into   (work/xlate)
  VCVARS     Visual Studio's vcvars64.bat                                   (found with vswhere)
  CLANG      clang-cl.exe, for BUILD_CC=clang                               (C:\Program Files\LLVM\bin)
  VULKAN_BIN folder with spirv-val, spirv-opt, spirv-dis, spirv-cross and glslangValidator
             (third_party\spirv\bin, which the repository carries; else the Vulkan SDK's Bin)
  KART_VER   another game version, built next to the default one: its out/, gen/ and build/ go to
             ver/<KART_VER>/ and host\build.bat links host\out\kart-<KART_VER>.exe   (none)
             Set it in the environment for that build only: in local.cfg it would switch every build.

From batch files:
  python tools\paths.py bat    KEY=VALUE lines, for `for /f ... do set "%%L"`
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYS = ["KART_GAME", "NVN_XLATE", "VCVARS", "CLANG", "VULKAN_BIN", "KART_VER"]


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


def _find_vulkan_bin():
    own = ROOT / "third_party" / "spirv" / "bin"
    if (own / "spirv-val.exe").exists():
        return str(own)
    if os.environ.get("VULKAN_SDK"):
        return str(Path(os.environ["VULKAN_SDK"]) / "Bin")
    sdks = sorted(Path(r"C:\VulkanSDK").glob("*/Bin"))
    return str(sdks[-1]) if sdks else str(own)


def get(key):
    """The setting's value ('' when there is none)."""
    if key == "KART_GAME" and get("KART_VER"):
        v = get("KART_VER")
        g = os.environ.get("KART_GAME_" + v) or _local().get("KART_GAME_" + v)
        if g:
            return g if Path(g).is_absolute() else str((ROOT / g).resolve())
    if os.environ.get(key):
        return os.environ[key]
    local = _local()
    if local.get(key):
        v = local[key]
        return str((ROOT / v).resolve()) if key not in ("VCVARS", "CLANG") and not Path(v).is_absolute() else v
    if key == "NVN_XLATE":
        return str(ROOT / "work" / "xlate")
    if key == "VCVARS":
        return _find_vcvars()
    if key == "CLANG":
        return r"C:\Program Files\LLVM\bin\clang-cl.exe"
    if key == "VULKAN_BIN":
        return _find_vulkan_bin()
    return ""


def game(sub=""):
    """The game dump folder (or a path inside it); exits with a message when KART_GAME is not set."""
    g = get("KART_GAME")
    if not g:
        sys.exit("KART_GAME is not set: put KART_GAME=<dump folder holding exefs\\ and romfs\\> in local.cfg")
    return str(Path(g) / sub) if sub else g


def art(sub=""):
    """Where the files made from the game go (out/, gen/, build/): the repository, or ver/<KART_VER>/."""
    v = get("KART_VER")
    base = ROOT / "ver" / v if v else ROOT
    return base / sub if sub else base


def progress(done, total, what=""):
    """A progress line for the launcher, which sets KART_PROGRESS=1 and draws "@@progress <done> <total> <what>" as a bar
    instead of logging it. Nothing without KART_PROGRESS."""
    if os.environ.get("KART_PROGRESS"):
        print(f"@@progress {done} {total} {what}", flush=True)


def vcvars_env():
    """The environment vcvars64.bat sets up (compilers, include and library paths)."""
    out = subprocess.run(f'cmd /c ""{get("VCVARS")}" >nul && set"', capture_output=True, text=True, errors="replace").stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
    if "INCLUDE" not in env:
        sys.exit(f"vcvars64.bat failed ({get('VCVARS')}): install Visual Studio with the C++ workload, or set VCVARS in local.cfg")
    env["VSLANG"] = "1033"   # compiler messages in English where that language is installed: easier to search for and report
    return env


def main():
    if sys.argv[1:2] == ["bat"]:
        for k in KEYS:
            print(f"{k}={get(k)}")
        # For host\build.bat: where the module glue is, and the exe to link.
        print(f"ART={art()}")
        print(f"KART_EXE=kart{'-' + get('KART_VER') if get('KART_VER') else ''}.exe")
    else:
        for k in KEYS:
            print(f"{k:10} {get(k)}")


if __name__ == "__main__":
    main()
