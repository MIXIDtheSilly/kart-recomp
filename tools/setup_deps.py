"""Fetch, patch and build what the recompilation needs besides this repository.

  python tools/setup_deps.py            fetch, egl and xlate below, in that order
  python tools/setup_deps.py fetch      clone Eglantine and plumber-racer-2 at the pinned commits, apply patches/
  python tools/setup_deps.py egl        configure and build Eglantine's libraries (CMake + Ninja: an hour or more)
  python tools/setup_deps.py xlate      build the shader translator dmsh_xlate (.NET 10 SDK)
  python tools/setup_deps.py patches    write patches/ again from the changes in the dependency folders
                                        (after changing Eglantine or plumber-racer-2 there)

The folders are deps/Eglantine, deps/egl-build and deps/plumber-racer-2 unless local.cfg or the environment
says otherwise (EGL, EGL_BUILD, NVN_REF: see tools/paths.py). BUILD_MP=<n> caps the build's parallel jobs.

Each dependency is the upstream repository at one commit plus this repository's patch: nothing of them is
copied in here.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import paths

ROOT = paths.ROOT
PATCHES = ROOT / "patches"

DEPS = [
    # name, setting, upstream, pinned commit
    ("eglantine", "EGL", "https://github.com/Djihads80/Eglantine", "adae7635708e155d967e84d7ad84bf63c5203409"),
    ("plumber-racer-2", "NVN_REF", "https://github.com/cheemu21/plumber-racer-2", "843170ac3e7068d0a096acc9694aea478cc8873b"),
]

# Eglantine as kart.exe links it: RelWithDebInfo with full inlining (CMake's default /Ob1 inlines only functions
# marked inline: races ran ~5% slower), no Qt frontend.
EGL_CONFIG = [
    "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT",
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob2 /DNDEBUG", "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /Ob2 /DNDEBUG",
    "-DENABLE_QT=OFF", "-DYUZU_CMD=ON", "-DENABLE_WEB_SERVICE=OFF", "-DYUZU_ROOM=OFF", "-DYUZU_TESTS=OFF",
    "-DENABLE_WERROR=OFF", "-DENABLE_UPDATE_CHECKER=OFF",
]
# eden-cli: builds every library kart.exe links (video_core, audio_core, common, shader_recompiler and their
# dependencies), and the emulator itself for comparing.
EGL_TARGET = "yuzu-cmd"


def git(repo, *args, check=True, **kw):
    return subprocess.run(["git", "-C", str(repo), *args], check=check, **kw)


def run(cmd, **kw):
    print(">", " ".join(map(str, cmd)), flush=True)
    r = subprocess.run(list(map(str, cmd)), **kw)
    if r.returncode:
        sys.exit(f"failed ({r.returncode}): {cmd[0]}")


def fetch():
    for name, key, url, commit in DEPS:
        repo, patch = Path(paths.get(key)), PATCHES / f"{name}.patch"
        if not (repo / ".git").exists():
            repo.parent.mkdir(parents=True, exist_ok=True)
            run(["git", "clone", "--filter=blob:none", "--no-checkout", url, repo])
            run(["git", "-C", repo, "checkout", "--detach", commit])
        else:
            head = git(repo, "rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
            if head != commit:
                print(f"{name}: {repo} is at {head[:10]}, not the pinned {commit[:10]}: leaving it as it is")
        # Applied already (the reverse applies), else apply it
        if git(repo, "apply", "--reverse", "--check", str(patch), check=False, capture_output=True).returncode == 0:
            print(f"{name}: {patch.name} is applied")
            continue
        r = git(repo, "apply", "--whitespace=nowarn", str(patch), check=False, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{name}: {patch.name} does not apply to {repo}:\n{r.stderr}")
        print(f"{name}: {patch.name} applied")


def egl():
    src, build = paths.get("EGL"), paths.get("EGL_BUILD")
    if not Path(src, "src", "video_core").exists():
        sys.exit(f"Eglantine not found at {src}: python tools/setup_deps.py fetch")
    env = paths.vcvars_env()
    run(["cmake", "-S", src, "-B", build, *EGL_CONFIG], env=env)
    jobs = ["--parallel", os.environ["BUILD_MP"]] if os.environ.get("BUILD_MP") else []
    run(["cmake", "--build", build, "--target", EGL_TARGET, *jobs], env=env)


def xlate():
    proj = Path(paths.get("NVN_REF"), "dm", "gfx", "tools", "dmsh_xlate")
    if not proj.exists():
        sys.exit(f"plumber-racer-2 not found at {proj.parents[3]}: python tools/setup_deps.py fetch")
    run(["dotnet", "build", "-c", "Release", proj / "dmsh_xlate.csproj"])
    out = Path(paths.get("NVN_XLATE"), "dmsh_xlate.exe")
    if not out.exists():
        sys.exit(f"built, but {out} is missing: set NVN_XLATE in local.cfg to the folder holding dmsh_xlate.exe")


def write_patches():
    """Each dependency's changes against its pinned commit, new files included (those its .gitignore does not
    ignore), staged in a scratch index so the repository's own index stays as it is."""
    PATCHES.mkdir(exist_ok=True)
    for name, key, _, commit in DEPS:
        repo = Path(paths.get(key))
        if not (repo / ".git").exists():
            print(f"{name}: no repository at {repo}, patch left as it is")
            continue
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
            git(repo, "read-tree", commit, env=env)
            git(repo, "add", "-A", env=env)
            diff = git(repo, "diff", "--cached", "--binary", "--no-color", "--no-ext-diff", commit, env=env,
                       capture_output=True).stdout
        out = PATCHES / f"{name}.patch"
        out.write_bytes(diff)
        files = sum(1 for l in diff.splitlines() if l.startswith(b"diff --git"))
        print(f"{name}: {out.relative_to(ROOT)} ({files} files, {len(diff) // 1024} KB) against {commit[:10]}")


def main():
    steps = sys.argv[1:] or ["fetch", "egl", "xlate"]
    table = {"fetch": fetch, "egl": egl, "xlate": xlate, "patches": write_patches}
    for s in steps:
        if s not in table:
            sys.exit(__doc__)
    for s in steps:
        table[s]()


if __name__ == "__main__":
    main()
