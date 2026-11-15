r"""Where the game dump and the compilers are: one place for the build scripts and tools.

Each setting comes from the environment, else from local.cfg at the repository root (KEY=VALUE lines, # starts
a comment; the file is not committed), else from the default below. On Linux, local.linux.cfg (same format) comes
before local.cfg, and local.cfg values that are Windows paths (C:\...) are skipped: one checkout can serve both systems.

  KART_GAME  the game dump: the folder holding exefs/ and romfs/            (none)
             (with KART_VER set, KART_GAME_<KART_VER> comes first)
  NVN_XLATE  folder the shader translator (dmsh_xlate.exe, from tools/xlate) is built into   (work/xlate)
  BUILD_CC   the C++ compiler: mingw (clang from llvm-mingw, which the launcher's Set up downloads: no Visual Studio
             needed), cl (Visual Studio) or clang (clang-cl with Visual Studio's libraries)
                                                             (mingw; on Linux always clang from LLVM, see below)
  LLVM       Linux: the folder holding bin/clang++, bin/ld.lld and bin/llvm-ar (python tools/toolchain.py install puts
             LLVM's release in tools/llvm; LLVM=/usr takes the distribution's clang)
                                                             (tools/llvm, else /usr when it has clang++)
  MINGW      the llvm-mingw toolchain (python tools\toolchain.py install puts it there)    (tools\llvm-mingw)
  VCVARS     Visual Studio's vcvars64.bat, for BUILD_CC=cl or clang         (found with vswhere)
  CLANG      clang-cl.exe, for BUILD_CC=clang                               (C:\Program Files\LLVM\bin)
  VULKAN_BIN folder with spirv-val, spirv-opt, spirv-dis, spirv-cross and glslangValidator
             (third_party\spirv\bin, which the repository carries; else the Vulkan SDK's Bin. Linux: tools/spirv,
             which ./kart.sh setup fills from the Vulkan SDK, else the Vulkan SDK's, else where spirv-val is on PATH)
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
KEYS = ["KART_GAME", "NVN_XLATE", "BUILD_CC", "MINGW", "LLVM", "VCVARS", "CLANG", "VULKAN_BIN", "KART_VER"]
LINUX = sys.platform.startswith("linux")
EXE = "" if LINUX else ".exe"   # what programs end in on this system
NOT_PATHS = ("BUILD_CC", "VCVARS", "CLANG", "KART_VER")


def _excepthook(kind, exc, tb):
    """A program a tool runs that is not there: Windows says only "[WinError 2] The system cannot find the file
    specified" (issue #22). Names the program, from the subprocess.Popen call in the traceback."""
    if issubclass(kind, FileNotFoundError) and getattr(exc, "winerror", None) == 2:
        t, prog = tb, None
        while t:
            f = t.tb_frame
            if f.f_code.co_name == "__init__" and isinstance(f.f_locals.get("self"), subprocess.Popen):
                args, exe = f.f_locals.get("args"), f.f_locals.get("executable")
                prog = exe or (args if isinstance(args, (str, bytes, os.PathLike)) else (list(args)[:1] or [None])[0])
                break
            t = t.tb_next
        if prog is not None:
            import traceback
            traceback.print_exception(kind, exc, tb)
            print(f"\ncould not start {os.fsdecode(prog)}: no such program. If it is a tool the build makes or "
                  "Set up installs, run Set up (Dependencies) again.", file=sys.stderr, flush=True)
            return
    sys.__excepthook__(kind, exc, tb)


sys.excepthook = _excepthook


def _windows_path(v):
    return len(v) >= 3 and v[0].isalpha() and v[1] == ":" and v[2] in "\\/"


def _local():
    cfg = {}
    for name in (["local.cfg", "local.linux.cfg"] if LINUX else ["local.cfg"]):
        f = ROOT / name
        if not f.exists():
            continue
        for line in f.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                k, v = k.strip(), v.strip().strip('"')
                if LINUX and name == "local.cfg" and _windows_path(v):
                    continue   # a Windows setting of a checkout both systems use
                cfg[k] = v
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
    if LINUX:   # the Vulkan SDK's tools Set up put in tools/spirv, the Vulkan SDK's, else the distribution's
        if (ROOT / "tools" / "spirv" / "spirv-val").exists():
            return str(ROOT / "tools" / "spirv")
        if os.environ.get("VULKAN_SDK"):
            return str(Path(os.environ["VULKAN_SDK"]) / "bin")
        import shutil
        found = shutil.which("spirv-val")
        return str(Path(found).parent) if found else "/usr/bin"
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
        return str((ROOT / v).resolve()) if key not in NOT_PATHS and not Path(v).is_absolute() else v
    if key == "NVN_XLATE":
        return str(ROOT / "work" / "xlate")
    if key == "MINGW":
        return str(ROOT / "tools" / "llvm-mingw")
    if key == "LLVM":
        own = ROOT / "tools" / "llvm"
        return str(own) if (own / "bin" / "clang++").exists() or not Path("/usr/bin/clang++").exists() else "/usr"
    if key == "BUILD_CC":
        return "mingw"   # ~50% faster game than cl (race ~150 fps against ~100); mingw_env says how to get it
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


def build_cc():
    """mingw, cl or clang (BUILD_CC; clang-cl counts as clang); linux on Linux."""
    if LINUX:
        return "linux"
    cc = get("BUILD_CC").lower()
    return "clang" if cc == "clang-cl" else cc


def mingw_env():
    """The environment with llvm-mingw's bin folder first on PATH; exits with a message when it is not there."""
    b = Path(get("MINGW")) / "bin"
    if not (b / "clang++.exe").exists():
        sys.exit(f"llvm-mingw is not in {b.parent}: run python tools\\toolchain.py install (the launcher's Set up does), "
                 "or set BUILD_CC=cl to build with Visual Studio")
    env = dict(os.environ)
    env["PATH"] = str(b) + os.pathsep + env.get("PATH", "")
    return env


def llvm_env():
    """Linux: the environment with LLVM's bin folder first on PATH; exits with a message when clang is not there."""
    b = Path(get("LLVM")) / "bin"
    if not (b / "clang++").exists():
        sys.exit(f"clang is not in {b}: run python3 tools/toolchain.py install, or install clang and lld from your "
                 "distribution and set LLVM=/usr in local.linux.cfg")
    env = dict(os.environ)
    env["PATH"] = str(b) + os.pathsep + env.get("PATH", "")
    return env


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
