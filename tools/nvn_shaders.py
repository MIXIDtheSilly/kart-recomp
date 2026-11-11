#!/usr/bin/env python3
"""The game's (Switch 2, NVN2 / Ampere sm86) shaders -> Vulkan SPIR-V, offline.

Pipeline (after nxnative / plumber-racer-2 `dm/gfx/tools`, MIT):
  1. split   : ref shader_split.py scans RomFS + exefs/main and writes BTCD record packages (<stage>_<group>.bin), shader_map.tsv,
               shader_facts.tsv, shader_pairs.tsv  -> work/nvn_shaders/split/     (run once; needs `pip install backports.zstd` on Python < 3.14)
  2. jobs    : per shader, build the dmsh_xlate job exactly like dmsh_build.py job_for() does for NVN2 records
  3. xlate   : dmsh_xlate (the Ryujinx translator, TargetApi.Vulkan; NVN_XLATE, built by tools/setup_deps.py xlate) -> <key>.spv + <key>.res.json in work/nvn_shaders/
Paths (plumber-racer-2, the game dump, the translator) come from tools/paths.py: the environment or local.cfg.
Key = sha256(control[:0x30] + 00000000 + control[0x34:0x48] + code area)[:16] (NVN2: shader_split.py skey/key_ctl, 0x48 control bytes with +0x30 zeroed;
the 0x880 rule of the first NVN generation does not apply to version-0xc9 sections).

usage: nvn_shaders.py [--split] [--jobs N] [--chunk N] [--timeout S] [--stage vs,ps,...] [--keys k1,k2] [--limit N] [--validate] [--out DIR]
"""
import argparse, collections, concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys, tempfile, time, shutil

sys.dont_write_bytecode = True
RECOMP = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
WORK = os.path.join(RECOMP, 'work')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
REF = paths.get('NVN_REF')
GAME_ID = '0400C3F00006E000'
_GAME = paths.get('KART_GAME')
ROMFS = os.environ.get('KART_ROMFS') or (os.path.join(_GAME, 'romfs') if _GAME else '')
MAIN = os.environ.get('KART_MAIN') or (os.path.join(_GAME, 'exefs', 'main') if _GAME else '')
OUT = os.path.join(WORK, 'nvn_shaders')
SPLIT = os.path.join(OUT, 'split')
XLATE_DIR = paths.get('NVN_XLATE')
XLATE_EXE = os.path.join(XLATE_DIR, 'dmsh_xlate.exe')
SPIRV_VAL = os.environ.get('SPIRV_VAL', r'C:\VulkanSDK\1.4.363.0\Bin\spirv-val.exe')
REF_TOOLS = os.path.join(REF, 'dm', 'gfx', 'tools')

CTL_MAGIC = bytes.fromhex('34127698')
STAGE_OF_NAME = {'vertex': 'vs', 'pixel': 'ps', 'compute': 'cs', 'geometry': 'gs', 'hull': 'hs', 'domain': 'ds', 'task': 'ts', 'mesh': 'ms'}


def ref_module(name):
    """dmsh_ampere / dmsh_meshstage are read from the reference checkout (read only, no .pyc written)."""
    sys.path.insert(0, REF_TOOLS)
    try:
        return __import__(name)
    finally:
        sys.path.pop(0)


def ctl_key_bytes(ctl):
    """shader_split.py key_ctl() for an NVN2 control section (version 0xc9): first 0x48 bytes, +0x30 zeroed."""
    assert ctl[:4] == CTL_MAGIC and ctl[4:8] == b'\xc9\0\0\0', 'not an NVN2 control section'
    c = ctl[:0x48]
    return c[:0x30] + bytes(4) + c[0x34:]


def shader_key(ctl, code):
    return hashlib.sha256(ctl_key_bytes(ctl) + code).hexdigest()[:16]


def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]


SPLIT_DRIVER = r'''# Driver for the reference shader_split.py on Windows: provides the `zstd -d -c -q` CLI it shells out to via backports.zstd (no zstd.exe needed).
import sys, os, subprocess
sys.dont_write_bytecode = True
_run = subprocess.run
def run(args, *a, **k):
    if isinstance(args, list) and args[:1] == ['zstd'] and args[1:4] == ['-d', '-c', '-q']:
        from backports import zstd
        r = subprocess.CompletedProcess(args, 0)
        try: r.stdout = zstd.decompress(k['input']); r.stderr = b''
        except Exception as e:
            if k.get('check'): raise subprocess.CalledProcessError(1, args, stderr=str(e).encode())
            r.returncode = 1; r.stdout = b''; r.stderr = str(e).encode()
        return r
    return _run(args, *a, **k)
subprocess.run = run
sys.path.insert(0, os.path.join(os.environ['NVN_REF'], 'dm', 'gfx', 'tools'))
if __name__ == '__main__':
    import shader_split
    shader_split.sys.argv = [shader_split.__file__] + sys.argv[1:]
    import nso
    shader_split.main()
'''


def run_split():
    os.makedirs(SPLIT, exist_ok=True)
    open(os.path.join(OUT, 'split_driver.py'), 'w').write(SPLIT_DRIVER)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', DM_GAME=GAME_ID, J=os.environ.get('J', '16'), PYTHONIOENCODING='utf-8', PYTHONUTF8='1', NVN_REF=REF)
    cmd = [sys.executable, os.path.join(OUT, 'split_driver.py'), ROMFS, MAIN, os.path.join(SPLIT, 'pkg'), os.path.join(SPLIT, 'shader_map.tsv'),
           os.path.join(SPLIT, 'shader_facts.tsv'), os.path.join(SPLIT, 'shader_pairs.tsv'), os.path.join(SPLIT, 'pso_pairs.tsv')]
    print('split:', ' '.join(cmd), flush=True)
    with open(os.path.join(SPLIT, 'split.log'), 'w') as log:
        r = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit(f'shader_split.py failed ({r.returncode}); see {SPLIT}/split.log')


def load_tables():
    rows = []
    for ln in open(os.path.join(SPLIT, 'shader_map.tsv'), encoding='utf-8'):
        if ln.startswith('#') or ln.startswith('package\t'):
            continue
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4:
            rows.append((c[0], int(c[1]), c[2], c[3], c[4]))
    facts = {}
    for ln in open(os.path.join(SPLIT, 'shader_facts.tsv')):
        if ln.strip() and not ln.startswith('#'):
            k, v = ln.rstrip('\n').split('\t', 1)
            facts[k] = json.loads(v)
    return rows, facts


def job_for(key, stage, rec, facts, w, amp, mesh):
    """dmsh_build.py job_for(), NVN2 branch (this game has no NVN1 shaders: every control section is version 0xc9)."""
    ci = rec.find(CTL_MAGIC)
    ctl = rec[ci:]
    if not amp.is_nvn2(ctl):
        raise ValueError('not an NVN2 control section')
    code = rec[0x38:ci]
    c1 = amp.c1_segment(ctl, code)            # between control +0x40 and +0x44 inside the code area
    code = code[:amp.inst_len(ctl)]           # SPH (0x80) + instructions, control +0x38 long
    extra = {'arch': 'sm86'}
    if stage == 'cs':                          # compute driver constants layout: push words, ubo / ssbo / texture counts, size
        extra['nvn2_cs'] = [ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<H', ctl, 0xba)[0], ctl[0xa9] * 4]
    if stage in ('ts', 'ms'):                  # translated as compute; stage buffer (ISBE) in shared memory
        mi = mesh.info(ctl, rec[0x38:ci])
        extra['meshstage'] = mi
        extra['isbe'] = mesh.isbe_layout(mi)
        code = code[0x80:]
    cp = os.path.join(w, key + '.bin')
    with open(cp, 'wb') as f:
        f.write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}
    if key not in facts:
        j['no_facts'] = True
        return j
    j.update(facts[key])
    if 'isbe' in j:
        j['shared'] = j['isbe'][3]
        j['local'] = [j['meshstage']['threads'], 1, 1]
    return j


def run_xlate(chunk_jobs, outdir, workdir, timeout):
    """-> (stderr text, timed_out)"""
    jp = os.path.join(workdir, f'job_{chunk_jobs[0]["key"]}_{len(chunk_jobs)}.json')
    with open(jp, 'w') as f:
        json.dump(chunk_jobs, f)
    try:
        r = subprocess.run([XLATE_EXE, jp, outdir], capture_output=True, text=True, timeout=timeout,
                           env=dict(os.environ, DOTNET_gcServer='0', DOTNET_TieredPGO='0'))
        return r.stderr, False
    except subprocess.TimeoutExpired:
        return '', True
    finally:
        try:
            os.unlink(jp)
        except OSError:
            pass


def translate_chunk(chunk, outdir, workdir, timeout):
    """Run a chunk; a crash or timeout loses the chunk, so unfinished keys are retried one by one."""
    errs = {}
    err, to = run_xlate(chunk, outdir, workdir, timeout)
    for ln in err.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m:
            errs[m.group(1)] = m.group(2)[:200]
    for j in chunk:
        k = j['key']
        if k in errs or os.path.exists(os.path.join(outdir, k + '.res.json')):
            continue
        err1, to1 = run_xlate([j], outdir, workdir, timeout)
        got = False
        for ln in err1.splitlines():
            m = re.match(r'\[RED\] (\w+): (.*)', ln)
            if m:
                errs[m.group(1)] = m.group(2)[:200]; got = True
        if not got and not os.path.exists(os.path.join(outdir, k + '.res.json')):
            errs[k] = 'timeout' if to1 else ('crash: ' + (err1.strip().splitlines() or ['no output'])[0][:160])
    for j in chunk:   # record the job inputs the host needs next to the translator's reflection
        rp = os.path.join(outdir, j['key'] + '.res.json')
        if j['key'] in errs or not os.path.exists(rp):
            continue
        r = json.load(open(rp))
        r['job'] = {k: j[k] for k in ('arch', 'local', 'shared', 'nvn2_cs', 'isbe', 'meshstage', 'no_facts', 'debug', 'reduced') if k in j}
        r['job']['c1'] = j['c1']
        json.dump(r, open(rp, 'w'), indent=1)
    return errs


def fix_caps(path):
    """SPIR-V capability fix-ups (idempotent), so a plain Vulkan 1.3 device can load the modules:
      * the translator never declares StorageImageRead/WriteWithoutFormat although it emits OpImageRead/OpImageWrite on storage images of format Unknown
        (322 of 11,561 shaders fail spirv-val otherwise): add them (Vulkan 1.3 features shaderStorageImageRead/WriteWithoutFormat);
      * it declares Float64 in every module (none uses a double) and FragmentShaderPixelInterlockEXT + SPV_EXT_fragment_shader_interlock in every pixel shader
        (none begins an interlock): drop them when unused, so the device needs neither shaderFloat64 nor fragmentShaderPixelInterlock.
    Returns True when the file changed."""
    b = open(path, 'rb').read()
    w = list(struct.unpack('<%dI' % (len(b) // 4), b))
    i, unknown_storage, caps, ops, has64, ilock = 5, False, set(), set(), False, False
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        if op == 17:
            caps.add(w[i + 1])
        elif op == 25 and w[i + 7] == 2 and w[i + 8] == 0:   # OpTypeImage sampled=2 (storage), format Unknown
            unknown_storage = True
        elif op in (98, 99, 320, 321):
            ops.add(op)
        elif op == 22 and w[i + 2] == 64:
            has64 = True
        elif op in (5364, 5365):                              # OpBegin/EndInvocationInterlockEXT
            ilock = True
        i += n
    add = []
    if unknown_storage:
        if ops & {98, 320} and 55 not in caps: add.append(55)
        if ops & {99, 321} and 56 not in caps: add.append(56)
    drop_caps = set()
    if not has64 and 10 in caps: drop_caps.add(10)
    if not ilock:
        drop_caps |= caps & {5378, 5363}
    if not add and not drop_caps and not (not ilock and b'SPV_EXT_fragment_shader_interlock' in b):
        return False
    out, i, last_cap_end = w[:5], 5, None
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        if op == 17 and w[i + 1] in drop_caps:
            i += n; continue
        if op == 10 and not ilock and b'SPV_EXT_fragment_shader_interlock' in struct.pack('<%dI' % (n - 1), *w[i + 1:i + n]):
            i += n; continue
        out += w[i:i + n]
        if op == 17:
            last_cap_end = len(out)
        i += n
    if add:
        out[last_cap_end:last_cap_end] = [x for c in add for x in ((2 << 16) | 17, c)]
    open(path, 'wb').write(struct.pack('<%dI' % len(out), *out))
    return True


def validate_one(key, outdir):
    r = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.3', os.path.join(outdir, key + '.spv')], capture_output=True, text=True)
    return key, r.returncode, (r.stderr or r.stdout).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--split', action='store_true', help='(re)run shader_split.py even if its output exists')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 8) - 2))
    ap.add_argument('--chunk', type=int, default=40)
    ap.add_argument('--timeout', type=int, default=600, help='seconds per xlate process')
    ap.add_argument('--stage'); ap.add_argument('--keys'); ap.add_argument('--limit', type=int)
    ap.add_argument('--validate', action='store_true', help='run spirv-val (vulkan1.3) on the produced .spv files')
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    outdir = a.out; os.makedirs(outdir, exist_ok=True)
    if not os.path.exists(XLATE_EXE):
        sys.exit(f'{XLATE_EXE} missing: python tools/setup_deps.py xlate (or set NVN_XLATE in local.cfg)')
    if a.split or not os.path.exists(os.path.join(SPLIT, 'shader_map.tsv')):
        if not ROMFS or not MAIN:
            paths.game()   # (exits: KART_GAME not set)
        run_split()
    amp, mesh = ref_module('dmsh_ampere'), ref_module('dmsh_meshstage')
    rows, facts = load_tables()
    want_stage = set(a.stage.split(',')) if a.stage else None
    want_keys = set(a.keys.split(',')) if a.keys else None
    packs = {}
    t0 = time.time()
    w = tempfile.mkdtemp(prefix='nvn_jobs_', dir=outdir)
    jobs, bad, meta = [], {}, {}
    for name, idx, stname, key, src in rows:
        stage = STAGE_OF_NAME.get(stname, name[:2])
        if want_stage and stage not in want_stage or want_keys and key not in want_keys:
            continue
        if a.limit and len(jobs) >= a.limit:
            break
        if name not in packs:
            packs[name] = records(open(os.path.join(SPLIT, 'pkg', name), 'rb').read())
        rec = packs[name][idx]
        ci = rec.find(CTL_MAGIC)
        if shader_key(rec[ci:], rec[0x38:ci]) != key:
            bad[key] = 'key mismatch (record does not hash to its map key)'; continue
        try:
            jobs.append(job_for(key, stage, rec, facts, w, amp, mesh))
        except Exception as e:
            bad[key] = f'job: {type(e).__name__}: {str(e)[:150]}'
        meta[key] = {'stage': stage, 'package': name, 'source': src}
    print(f'{len(jobs)} jobs prepared in {time.time() - t0:.0f}s ({len(bad)} rejected)', flush=True)
    chunks = [jobs[i:i + a.chunk] for i in range(0, len(jobs), a.chunk)]
    errs, done = dict(bad), 0
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for e in ex.map(lambda c: translate_chunk(c, outdir, w, a.timeout), chunks):
            errs.update(e); done += 1
            if done % 20 == 0 or done == len(chunks):
                print(f'  chunks {done}/{len(chunks)}  failed so far {len(errs)}  {time.time() - t0:.0f}s', flush=True)
    shutil.rmtree(w, ignore_errors=True)
    ok = [j['key'] for j in jobs if os.path.exists(os.path.join(outdir, j['key'] + '.spv')) and j['key'] not in errs]
    summary = {k: {**meta.get(k, {}), 'status': 'ok' if k in ok else 'failed', **({'error': errs[k]} if k in errs else {})} for k in list(meta) + [k for k in errs if k not in meta]}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        fixed = sum(ex.map(lambda k: fix_caps(os.path.join(outdir, k + '.spv')), ok))
    print(f'capability fix-ups applied to {fixed} shaders')
    if a.validate:
        print('spirv-val ...', flush=True)
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            for k, rc, msg in ex.map(lambda k: validate_one(k, outdir), ok):
                summary[k]['spirv_val'] = 'ok' if rc == 0 else msg[:400]
    with open(os.path.join(outdir, 'build_summary.json'), 'w') as f:
        json.dump(summary, f, indent=0)
    st = collections.Counter((v['stage'], v['status']) for v in summary.values() if 'stage' in v)
    print(f'total {len(summary)}  translated {len(ok)}  failed {len(summary) - len(ok)}')
    for s in sorted({s for s, _ in st}):
        print(f'  {s}: ok {st[(s, "ok")]}  failed {st[(s, "failed")]}')
    reasons = collections.Counter(re.sub(r'\b[0-9a-f]{16}\b', 'KEY', v.get('error', ''))[:110] for v in summary.values() if v['status'] == 'failed')
    for r, n in reasons.most_common(20):
        print(f'  [{n:5d}] {r}')


if __name__ == '__main__':
    main()
