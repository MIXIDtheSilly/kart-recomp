r"""Updates from the GitHub releases, in place, for the launcher (launcher/launcher.cpp).

  python tools/update.py check            the newest release, when it is newer than this one and has an archive for
                                           this system: "version <v>", then "notes" and the start of its notes
  python tools/update.py install <ver>    downloads that release's archive (kart-recomp-<ver>-windows.zip or
                                           -linux.tar.gz) and puts it over this folder: files that changed are
                                           replaced, files the old release had and the new one has not are deleted;
                                           the build, the tools Set up installed, saves and settings stay
  python tools/update.py stamp <ver>      when making a release: writes launcher/out/release.txt and
                                           launcher/out/files.txt (every file of the release) into the staged folder
                                           this script is in, so the release can update itself later

A release carries both files beside its prebuilt launcher; a checkout has neither, so it is never offered an update.
A file in use (Windows: the running launcher.exe) is renamed to <name>.old and replaced; the launcher deletes the .old
files it finds in launcher/out/update-leftovers.txt when it starts. Files whose contents did not change are not
touched, so the build's stage stamps (some go by file times) stay valid. Nothing outside the standard library: it runs
before Set up.
"""
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

import paths

REPO = "MIXIDtheSilly/kart-recomp"
# KART_UPDATE_API: another place answering as GitHub does (<api>/latest, <api>/tags/<tag>), to try an update out
API = os.environ.get("KART_UPDATE_API") or f"https://api.github.com/repos/{REPO}/releases"
ASSET_END = "-linux.tar.gz" if paths.LINUX else "-windows.zip"
HEADERS = {"User-Agent": "kart-recomp-launcher", "Accept": "application/vnd.github+json"}
OUT = paths.ROOT / "launcher" / "out"
LAST = ("launcher/out/files.txt", "launcher/out/release.txt")   # put in place last: until then it is the old release
# Never deleted, whatever a file list says: what is built, installed or written here (.gitignore)
KEEP = ("build/", "gen/", "out/", "work/", "ver/", "host/out/", "deps/", "tools/llvm-mingw/", "tools/llvm/",
        "tools/python/", "tools/dotnet/", "tools/venv/", "tools/spirv/", ".git/", "local.cfg", "local.linux.cfg")


def current():
    """This folder's release ('' in a checkout)."""
    p = OUT / "release.txt"
    return p.read_text(encoding="utf-8").strip() if p.exists() else ""


def key(v):
    """A version as numbers, to compare: 0.1.10 after 0.1.9."""
    return tuple(int(x) if x.isdigit() else 0 for x in re.split(r"[.\-]", v.lstrip("v")))


def release(tag=None):
    url = f"{API}/tags/{tag}" if tag else f"{API}/latest"   # latest: neither a draft nor a prerelease
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=15) as r:
        return json.load(r)


def asset(rel):
    return next((a for a in rel.get("assets", []) if a.get("name", "").endswith(ASSET_END)), None)


def check():
    have = current()
    if not have:
        return
    rel = release()
    v = rel.get("tag_name", "").lstrip("v")
    if not v or key(v) <= key(have) or not asset(rel):
        return
    print(f"version {v}")
    print("notes")
    lines = [l.rstrip() for l in (rel.get("body") or "").replace("\r", "").strip().split("\n")]
    notes = "\n".join(lines[:12])
    if len(notes) > 700 or len(lines) > 12:
        notes = notes[:700].rstrip() + "\n…"
    print(notes)


def inside(base, name):
    """Whether <name> lands inside <base> (no absolute paths, no ..)."""
    t = (base / name).resolve()
    return t == base or base in t.parents


def file_list(top):
    """Every file under <top>, as relative paths with / (the order files.txt keeps)."""
    return sorted(p.relative_to(top).as_posix() for p in top.rglob("*") if p.is_file())


def read_list(p):
    if not p.exists():
        return None
    return {l.strip() for l in p.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")}


def same(a, b):
    if not b.is_file() or a.stat().st_size != b.stat().st_size:
        return False
    ha, hb = hashlib.sha256(), hashlib.sha256()
    for p, h in ((a, ha), (b, hb)):
        with open(p, "rb") as f:
            while chunk := f.read(1 << 20):
                h.update(chunk)
    return ha.digest() == hb.digest()


def set_aside(dst, leftovers):
    """A file that cannot be replaced or deleted (in use on Windows) goes to <name>.old, deleted later."""
    old = dst.with_name(dst.name + ".old")
    try:
        old.unlink(missing_ok=True)
    except OSError:
        old = dst.with_name(f"{dst.name}.{int(time.time())}.old")
    os.replace(dst, old)
    leftovers.append(old.relative_to(paths.ROOT).as_posix())


def put(src, dst, leftovers):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.replace(src, dst)
    except PermissionError:
        set_aside(dst, leftovers)
        os.replace(src, dst)


def download(a, dest):
    """The archive, checked against the SHA-256 GitHub keeps for it."""
    print(f"downloading {a['browser_download_url']}", flush=True)
    h = hashlib.sha256()
    req = urllib.request.Request(a["browser_download_url"], headers={"User-Agent": HEADERS["User-Agent"]})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or a.get("size") or 0)
        done, last = 0, 0.0
        while chunk := r.read(1 << 18):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if time.time() - last > 0.3:
                last = time.time()
                paths.progress(done >> 10, max(total >> 10, 1), "Downloading")
    digest = a.get("digest") or ""
    if digest.startswith("sha256:") and digest[7:] != h.hexdigest():
        sys.exit(f"the download's SHA-256 is {h.hexdigest()}, not {digest[7:]}: try again (a proxy or a cut-off download?)")
    print(f"{done / 1e6:.1f} MB", flush=True)


def unpack(archive, part):
    part.mkdir()
    base = part.resolve()
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for i in z.infolist():
                if not inside(base, i.filename):
                    sys.exit(f"the archive holds {i.filename}, outside its folder: not unpacked")
                p = Path(z.extract(i, part))
                if not i.is_dir():   # the archive's file times, as unzipping by hand keeps them
                    t = time.mktime(i.date_time + (0, 0, -1))
                    os.utime(p, (t, t))
    else:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                if not inside(base, m.name) or m.issym() or m.islnk() or m.isdev():
                    sys.exit(f"the archive holds {m.name}, which is not a plain file inside its folder: not unpacked")
            if hasattr(tarfile, "data_filter"):
                t.extractall(part, filter="data")   # keeps the executable bits (kart.sh, the launcher)
            else:
                t.extractall(part)
    tops = list(part.iterdir())
    if len(tops) != 1 or not (tops[0] / "tools" / "paths.py").exists():
        sys.exit("the archive is not a kart-recomp release (no single folder holding tools/paths.py)")
    top = tops[0]
    for need in ("launcher/out/launcher" + paths.EXE, "launcher/out/release.txt"):
        if not (top / need).exists():
            sys.exit(f"the archive has no {need}: not a release this can update to")
    return top


def install(v):
    rel = release("v" + v.lstrip("v"))
    a = asset(rel)
    if not a:
        sys.exit(f"release {v} has no archive for this system (*{ASSET_END})")
    root = paths.ROOT
    tmp = root / f".update-{v}{ASSET_END}"   # unpack goes by the ending: .zip or .tar.gz
    part = root / f".update-{v}"
    shutil.rmtree(part, ignore_errors=True)
    leftovers = []
    try:
        download(a, tmp)
        top = unpack(tmp, part)
        new = file_list(top)
        old = read_list(OUT / "files.txt")

        # Changed and new files over these; release.txt last, so an update cut short is offered again
        order = [f for f in new if f not in LAST] + [f for f in LAST if f in new]
        changed = added = 0
        for i, f in enumerate(order):
            src, dst = top / f, root / f
            if not dst.exists():
                added += 1
            elif same(src, dst):
                continue
            else:
                changed += 1
            put(src, dst, leftovers)
            if i % 20 == 0:
                paths.progress(i, len(order), "Updating files")

        # What the old release had and this one has not (never the build, tools or settings)
        removed = 0
        if old is None:
            print("no launcher/out/files.txt: files the new release dropped stay")
        for f in sorted(old - set(new)) if old else []:
            p = root / f
            if f.startswith(KEEP) or not inside(root.resolve(), f) or not p.is_file():
                continue
            try:
                p.unlink()
            except PermissionError:
                set_aside(p, leftovers)
            removed += 1
            for d in p.parents:   # folders it leaves empty
                if d == root:
                    break
                try:
                    d.rmdir()
                except OSError:
                    break
        print(f"updated to {v}: {changed} files changed, {added} added, {removed} removed", flush=True)
    finally:
        if leftovers:
            with open(OUT / "update-leftovers.txt", "a", encoding="utf-8") as f:
                f.writelines(l + "\n" for l in leftovers)
        tmp.unlink(missing_ok=True)
        shutil.rmtree(part, ignore_errors=True)


def stamp(v):
    """release.txt and files.txt for the release staged in this folder (run from the staged copy's tools/)."""
    if not any((OUT / n).exists() for n in ("launcher", "launcher.exe")):
        sys.exit(f"no prebuilt launcher in {OUT}: build it and copy it there first")
    shutil.rmtree(paths.ROOT / "tools" / "__pycache__", ignore_errors=True)   # this run's, not the release's
    (OUT / "release.txt").write_text(v.lstrip("v") + "\n", encoding="utf-8")
    (OUT / "files.txt").write_text("", encoding="utf-8")
    files = [f for f in file_list(paths.ROOT) if not f.startswith(KEEP) and "__pycache__/" not in f]
    (OUT / "files.txt").write_text("".join(f + "\n" for f in files), encoding="utf-8")
    print(f"{paths.ROOT}: release {v.lstrip('v')}, {len(files)} files")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "check":
        try:
            check()
        except (OSError, ValueError) as e:   # offline, GitHub's rate limit: no update offered
            sys.exit(f"no answer from GitHub ({e})")
    elif len(sys.argv) == 3 and sys.argv[1] == "install":
        install(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "stamp":
        stamp(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
