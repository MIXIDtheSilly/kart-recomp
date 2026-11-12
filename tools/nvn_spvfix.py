#!/usr/bin/env python3
"""Translated shaders the NVIDIA Vulkan driver miscompiles -> work/nvn_shaders/<key>.fix.spv, which the renderer loads instead of <key>.spv.

Each is run through spirv-opt -O (bindings and interface kept, so the reflection table still fits), which restructures it enough that the driver's
code is correct. The translation itself (<key>.spv) is left as it is.

  87c52decaf1a8e68  vs: its first draw (a lit quad partway into the first race) faults the GPU with "instruction pointer invalid", then device loss
  c3e3a664e970c1bc  vs: the same, ~40 s after boot (title screen)

Both are huge unoptimized vertex shaders (183 and 235 KB of SPIR-V), so --min-bytes (default 150000) also fixes every vertex shader at least
that big, not only the listed ones. Device losses kept coming from other shaders, so --all (what build steps use) restructures every
vertex, pixel and compute shader (task and mesh shaders are loaded from nvn_mesh.py's files instead).

usage: nvn_spvfix.py [--dir DIR] [--min-bytes N] [--all]
"""
import argparse, concurrent.futures, json, os, subprocess, sys

BIN = os.environ.get('VULKAN_BIN', r'C:\VulkanSDK\1.4.363.0\Bin')
EXE = lambda n: os.path.join(BIN, n + '.exe')
FIXES = ['87c52decaf1a8e68', 'c3e3a664e970c1bc']


def main():
    ap = argparse.ArgumentParser()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import paths   # work/nvn_shaders-<KART_VER> for another game version, as nvn_shaders.py writes it
    ver = paths.get('KART_VER')
    ap.add_argument('--dir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work',
                                                  'nvn_shaders' + ('-' + ver if ver else '')))
    ap.add_argument('--min-bytes', type=int, default=150000)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    keys = set(FIXES)
    for f in os.listdir(a.dir):
        k, ext = os.path.splitext(f)
        if ext != '.spv' or '.' in k or (not a.all and os.path.getsize(os.path.join(a.dir, f)) < a.min_bytes):
            continue
        try:
            with open(os.path.join(a.dir, k + '.res.json')) as j:
                stage = json.load(j).get('stage')
            if stage == 'vs' or (a.all and stage in ('ps', 'cs', 'gs', 'tcs', 'tes')):
                keys.add(k)
        except OSError:
            pass

    def fix(k):
        src, out = os.path.join(a.dir, k + '.spv'), os.path.join(a.dir, k + '.fix.spv')
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            return None
        r = subprocess.run([EXE('spirv-opt'), '-O', '--preserve-bindings', '--preserve-interface', '--target-env=vulkan1.3', src, '-o', out],
                           capture_output=True, text=True)
        if r.returncode == 0:
            r = subprocess.run([EXE('spirv-val'), '--target-env', 'vulkan1.3', out], capture_output=True, text=True)
        if r.returncode:
            if os.path.exists(out): os.remove(out)
            return f'{k}: failed: {(r.stdout + r.stderr)[:400]}'
        return ''

    bad = done = 0
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 2)) as pool:
        for k, r in zip(sorted(keys), pool.map(fix, sorted(keys))):
            if r:
                print(r)
                bad += 1
            elif r == '':
                done += 1
    print(f'{len(keys)} shaders, {done} fixed now, {bad} failed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
