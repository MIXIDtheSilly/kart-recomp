r"""The way from a game dump to playing on Linux, the steps the Windows launcher (launcher/launcher.cpp) takes, on the
command line. ./kart.sh runs it.

  ./kart.sh setup                     what the build needs: clang (LLVM's release into tools/llvm, tools/toolchain.py),
                                      the Python packages (a virtual environment in tools/venv, tools/deps.py), the .NET
                                      SDK for the shader translator (into tools/dotnet when the system has none) and the
                                      SPIR-V tools (from the Vulkan SDK into tools/spirv when some are missing); checks the
                                      system's libraries (SDL 3, the Vulkan loader, libvpx)
  ./kart.sh game <folder>             the dump to build and play (the folder holding exefs/ and romfs/), into
                                      local.linux.cfg (KART_GAME, or KART_GAME_<version> with --version)
  ./kart.sh build                     unpack, find the functions, lift and compile, translate the shaders, link: each
                                      step that is not done yet (--force: all of them; the tools redo only what changed)
  ./kart.sh play                      starts the game (host/out/kart, or kart-<version>)
  ./kart.sh bake                      decodes every texture into the texture cache ahead of time (fewer first-race hitches)

  --version <v>   another game version (1.8.0), built beside the default one in ver/<v>/ (tools/paths.py KART_VER); the
                  default is LAUNCHER_VERSION from local.linux.cfg / local.cfg, else the default build.

Settings: tools/paths.py (local.linux.cfg, then local.cfg; the game's HOST_* settings in docs/how-it-works.md).
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import paths

ROOT = paths.ROOT
TOOLS = ROOT / "tools"
VENV = TOOLS / "venv"
DOTNET = TOOLS / "dotnet"
SPIRV = TOOLS / "spirv"
# The SPIR-V tools of the Vulkan SDK the Windows build carries (third_party/spirv), from LunarG's Linux release of it
VKSDK = "1.4.363.0"
VKSDK_URL = f"https://sdk.lunarg.com/sdk/download/{VKSDK}/linux/vulkansdk-linux-x86_64-{VKSDK}.tar.xz"
VKSDK_SHA256 = "197962f5cbf80baf2775a03336a01cee7c8745686c65aaa70d3f751ade4d7e43"
SPIRV_TOOLS = ("spirv-val", "spirv-opt", "spirv-dis", "spirv-cross", "glslangValidator")


def say(msg):
    print(msg, flush=True)


def version_of(args):
    v = args.version if args.version is not None else paths._local().get("LAUNCHER_VERSION", "")
    return "" if v in ("", "base", "1.0.0", "default") else v


def tool_env(ver):
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    if ver:
        env["KART_VER"] = ver
    else:
        env.pop("KART_VER", None)
    if (DOTNET / "dotnet").exists():   # the SDK setup put here (nvn_shaders.py runs `dotnet build`)
        env["PATH"] = str(DOTNET) + os.pathsep + env.get("PATH", "")
        env["DOTNET_ROOT"] = str(DOTNET)
    env.setdefault("DOTNET_CLI_TELEMETRY_OPTOUT", "1")
    return env


def python():
    py = VENV / "bin" / "python"
    return str(py) if py.exists() else sys.executable


def run(cmd, env, cwd=ROOT):
    say("> " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd)
    if r.returncode:
        sys.exit(f"failed (exit code {r.returncode}): {' '.join(str(c) for c in cmd)}")


# ------------------------------------------------------------------ setup

def have_lib(name):
    try:
        ctypes.CDLL(name)
        return True
    except OSError:
        return False


def setup(args):
    problems = []
    # The libraries the game loads (no -devel packages: the headers are in third_party/)
    for lib, what, need in (("libSDL3.so.0", "SDL 3 (window, input, sound): the SDL3 package (Fedora: SDL3, Debian/Ubuntu: libsdl3-0)", True),
                            ("libvulkan.so.1", "the Vulkan loader (Fedora: vulkan-loader, Debian/Ubuntu: libvulkan1) and your GPU's Vulkan driver", True),
                            ("libvpx.so.9", "", False)):
        if lib == "libvpx.so.9":
            ok = any(have_lib(f"libvpx.so.{n}") for n in (12, 11, 10, 9, 8, 7, 6))
            say(f"  libvpx              {'found' if ok else 'missing: the movies show black (install libvpx)'}")
            continue
        ok = have_lib(lib)
        say(f"  {lib:20}{'found' if ok else 'MISSING: ' + what}")
        if not ok and need:
            problems.append(what)
    # The C++ library headers clang compiles against (libstdc++, glibc)
    if not list(Path("/usr/include/c++").glob("*/vector")):
        problems.append("the C++ standard library headers (Fedora: gcc-c++, Debian/Ubuntu: g++)")
        say("  C++ headers         MISSING: install g++ (gcc-c++)")
    # clang
    if (Path(paths.get("LLVM")) / "bin" / "clang++").exists():
        say(f"  clang               {paths.get('LLVM')}")
    else:
        say("  clang               installing LLVM's release into tools/llvm (1.2 GB download)")
        run([sys.executable, TOOLS / "toolchain.py", "install"], tool_env(""))
    # Python packages in a virtual environment (distributions keep pip away from the system's Python)
    if not (VENV / "bin" / "python").exists():
        say("  Python packages     making tools/venv")
        r = subprocess.run([sys.executable, "-m", "venv", str(VENV)])
        if r.returncode:
            sys.exit("python3 -m venv failed: install your distribution's python3-venv package")
    run([python(), TOOLS / "deps.py", "install"], tool_env(""))
    # .NET SDK (the shader translator, tools/xlate)
    if shutil.which("dotnet") or (DOTNET / "dotnet").exists():
        say("  .NET SDK            found")
    else:
        say("  .NET SDK            installing the .NET 10 SDK into tools/dotnet (dotnet-install.sh)")
        script = TOOLS / "dotnet-install.sh"
        with urllib.request.urlopen("https://dot.net/v1/dotnet-install.sh", timeout=60) as r:
            script.write_bytes(r.read())
        run(["bash", script, "--channel", "10.0", "--install-dir", DOTNET, "--no-path"], tool_env(""))
        script.unlink(missing_ok=True)
    # SPIR-V tools (the shader steps; distributions often lack spirv-cross)
    bin_dir = Path(paths.get("VULKAN_BIN"))
    lack = [t for t in SPIRV_TOOLS if not (bin_dir / t).exists()]
    if lack:
        say(f"  SPIR-V tools        {', '.join(lack)} missing in {bin_dir}: taking them from the Vulkan SDK {VKSDK} (370 MB download)")
        spirv_install()
        bin_dir = Path(paths.get("VULKAN_BIN"))
    say(f"  SPIR-V tools        {bin_dir}")
    if problems:
        say("\nStill missing (install with your package manager, then run ./kart.sh setup again):\n  - " + "\n  - ".join(problems))
        sys.exit(1)
    say("\nSet up. Next: ./kart.sh game <dump folder>, then ./kart.sh build")


def spirv_install():
    import hashlib
    import tarfile
    from toolchain import fetch
    tpath = TOOLS / f"vulkansdk-linux-x86_64-{VKSDK}.tar.xz"
    if not tpath.exists():
        fetch(VKSDK_URL, tpath, VKSDK_SHA256, "downloading the Vulkan SDK (MB)")
    else:
        h = hashlib.sha256()
        with open(tpath, "rb") as f:
            while chunk := f.read(1 << 22):
                h.update(chunk)
        if h.hexdigest() != VKSDK_SHA256:
            tpath.unlink()
            fetch(VKSDK_URL, tpath, VKSDK_SHA256, "downloading the Vulkan SDK (MB)")
    want = {f"{VKSDK}/x86_64/bin/{n}": n for n in ("spirv-val", "spirv-opt", "spirv-dis", "spirv-cross", "glslang")}
    want[f"{VKSDK}/LICENSE.txt"] = "LICENSE.txt"
    tmp = TOOLS / "spirv.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    with tarfile.open(tpath, "r:xz") as t:
        for m in t:
            if m.name in want and m.isfile():
                with t.extractfile(m) as src, open(tmp / want[m.name], "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                (tmp / want[m.name]).chmod(0o755)
    missing = [n for n in want.values() if not (tmp / n).exists()]
    if missing:
        sys.exit(f"the Vulkan SDK archive lacks {', '.join(missing)}")
    (tmp / "glslangValidator").symlink_to("glslang")
    shutil.rmtree(SPIRV, ignore_errors=True)
    tmp.rename(SPIRV)
    tpath.unlink()


# ------------------------------------------------------------------ game

def set_game(args):
    folder = Path(args.folder).expanduser().resolve()
    if not (folder / "exefs").is_dir() or not (folder / "romfs").is_dir():
        sys.exit(f"{folder} has no exefs/ and romfs/: that is not a dump (a .nup unpacks with python3 tools/nup.py extract <file> <folder>)")
    ver = version_of(args)
    key = f"KART_GAME_{ver}" if ver else "KART_GAME"
    cfg = ROOT / "local.linux.cfg"
    lines = cfg.read_text(encoding="utf-8").splitlines() if cfg.exists() else ["# Linux settings (tools/paths.py): they come before local.cfg"]
    lines = [l for l in lines if l.split("=", 1)[0].strip() != key] + [f"{key}={folder}"]
    if args.version is not None:
        lines = [l for l in lines if l.split("=", 1)[0].strip() != "LAUNCHER_VERSION"] + [f"LAUNCHER_VERSION={args.version}"]
    cfg.write_text("\n".join(lines) + "\n", encoding="utf-8")
    say(f"{key}={folder} (in local.linux.cfg)")


# ------------------------------------------------------------------ build

def build(args):
    ver = version_of(args)
    env = tool_env(ver)
    os.environ.update({k: v for k, v in env.items() if k == "KART_VER"})
    if not ver:
        os.environ.pop("KART_VER", None)
    game = paths.get("KART_GAME")
    if not game:
        sys.exit("no dump set: ./kart.sh game <dump folder>" + (f" --version {ver}" if ver else ""))
    art = paths.art()
    out = art / "out"
    py = python()
    t0 = time.time()

    def modules():
        import json
        return [m["name"] for m in json.loads((out / "layout.json").read_text())]

    def all_have(folder, suffix):
        return (out / "layout.json").exists() and all((folder / (m + suffix)).exists() for m in modules())

    def newer_than(outputs, inputs):
        """Whether an input changed after the oldest output was made."""
        oldest = min((o.stat().st_mtime for o in outputs), default=0)
        return any(f.stat().st_mtime > oldest for pattern in inputs for f in ROOT.glob(pattern))

    say(f"== game {game}" + (f" (version {ver}, files in {art})" if ver else ""))
    if args.force or not all_have(out, ".json"):
        say("== 1/6 unpack the modules")
        run([py, TOOLS / "nso.py", Path(game) / "exefs", out], env)
    if args.force or not all_have(out, ".funcs.json"):
        say("== 2/6 find the functions")
        for m in modules():
            run([py, TOOLS / "scan.py", out, m], env)
            run([py, TOOLS / "funcs.py", out, m], env)
    libs = [art / "build" / "linux" / f"{m}.a" for m in modules()]
    lift_inputs = ["tools/build_all.py", "tools/lift.py", "tools/a64.py", "tools/neon.py", "tools/patches.py", "tools/build_lib.py",
                   "runtime/*.h"]
    if args.force or not all(l.exists() for l in libs) or newer_than(libs, lift_inputs):
        say("== 3/6 lift and compile (the first time: about half an hour on 8 cores)")
        run([py, TOOLS / "build_all.py"], env)
    if args.force or not (art / "gen" / "aot_modules.cpp").exists():
        say("== 4/6 module glue")
        run([py, TOOLS / "gen_glue.py"], env)
    sh = ROOT / "work" / ("nvn_shaders" + (f"-{ver}" if ver else ""))
    if args.force or not ((sh / "refl.tsv").exists() and any(sh.glob("*.fix.spv")) and any(sh.glob("*.bar.spv"))):
        say("== 5/6 translate the shaders")
        for step in (["nvn_shaders.py"], ["nvn_refl.py"], ["nvn_mesh.py", "--flat"], ["nvn_spvfix.py", "--all"], ["nvn_barfix.py"]):
            run([py, TOOLS / step[0], *step[1:]], env)
    say("== 6/6 link")
    run([py, TOOLS / "build_host.py"], env)
    minutes = int(time.time() - t0) // 60
    say(f"\nBuilt in {minutes} min. ./kart.sh play" + (f" --version {args.version}" if args.version is not None else ""))


# ------------------------------------------------------------------ play

def exe_of(ver):
    return ROOT / "host" / "out" / ("kart" + (f"-{ver}" if ver else ""))


def play(args, extra=()):
    ver = version_of(args)
    exe = exe_of(ver)
    if not exe.exists():
        sys.exit(f"{exe.name} is not built yet: ./kart.sh build" + (f" --version {ver}" if args.version is not None else ""))
    env = dict(os.environ)
    env.pop("KART_VER", None)
    os.chdir(exe.parent)   # (its pipeline cache and user folder are relative to host/out)
    os.execve(str(exe), [str(exe), *extra], env)


def bake(args):
    ver = version_of(args)
    exe = exe_of(ver)
    if not exe.exists():
        sys.exit(f"{exe.name} is not built yet: ./kart.sh build")
    os.environ.update({k: v for k, v in tool_env(ver).items() if k == "KART_VER"})
    game = paths.get("KART_GAME")
    if not game:
        sys.exit("no dump set: ./kart.sh game <dump folder>")
    play(args, [game, "--bake-textures"])


def main():
    if not paths.LINUX:
        sys.exit("this is the Linux front end: on Windows the launcher does this (launcher\\build.bat, launcher\\out\\launcher.exe)")
    ap = argparse.ArgumentParser(prog="./kart.sh", description="Set up, build and play on Linux.", epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", help="the game version (1.8.0); default: LAUNCHER_VERSION, else the default build")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("setup", help="install and check what the build needs")
    g = sub.add_parser("game", help="the dump folder to build and play")
    g.add_argument("folder")
    b = sub.add_parser("build", help="build what is not built yet")
    b.add_argument("--force", action="store_true", help="run every step (each still redoes only what changed)")
    sub.add_parser("play", help="start the game")
    sub.add_parser("bake", help="decode every texture ahead of time")
    for p in sub.choices.values():
        p.add_argument("--version", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.cmd == "setup":
        setup(args)
    elif args.cmd == "game":
        set_game(args)
    elif args.cmd == "build":
        build(args)
    elif args.cmd == "play":
        play(args)
    elif args.cmd == "bake":
        bake(args)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
