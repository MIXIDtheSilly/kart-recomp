#!/usr/bin/env python3
"""NVN2 task / mesh shaders (translated as compute by tools/nvn_shaders.py) -> Vulkan TaskEXT / MeshEXT SPIR-V.

The translator's compute SPIR-V (stage buffer "ISBE" in shared memory, layout from the reference dmsh_meshstage.py) is turned into GLSL with spirv-cross, the
kernel body becomes a function and a new main() does what dmsh_meshstage.rewrite() does for Metal (fill the input region, run the body, read the regions back),
then glslangValidator compiles it as a task / mesh shader (vulkan1.3, GL_EXT_mesh_shader) -> work/nvn_shaders/<key>.mesh.spv.  See work/nvn_shaders/MESH.md.

usage: nvn_mesh.py [--keys k1,k2] [--flat] [--out DIR]
  --flat  also write <key>.meshflat.spv for meshes with per-primitive attributes: the primitives become three private vertices each (<= 256 vertices) and their
          per-primitive attributes flat per-vertex outputs, so the fragment shader needs no PerPrimitiveEXT decoration (only when 3 * maxp <= 256)
"""
import argparse, json, os, re, subprocess, sys, tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvn_shaders as ns

BIN = os.environ.get('VULKAN_BIN', r'C:\VulkanSDK\1.4.363.0\Bin')
EXE = lambda n: os.path.join(BIN, n + '.exe')
SKEW = 32
PER = {1: 1, 2: 2, 3: 3}


def attr(a):
    """('pos', comp) / ('loc', N, comp) / None  (same mapping as the reference _attr_name)"""
    if 0x70 <= a < 0x80: return ('pos', (a - 0x70) // 4)
    if 0x80 <= a < 0x280: return ('loc', (a - 0x80) // 16, (a - 0x80) // 4 % 4)
    return None


def vec4_expr(arr, comps):
    """vec4 from {component: word-index expression}; unwritten components 0 (the reference's zero-initialised vertex)"""
    return 'vec4(' + ', '.join(f'uintBitsToFloat({arr}[{comps[c]}])' if c in comps else '0.0' for c in range(4)) + ')'


def build_glsl(glsl, m, lay, flat=False):
    """compute GLSL (spirv-cross, --vulkan-semantics) -> task / mesh GLSL"""
    sh = re.search(r'^shared uint (\w+)\[(\d+)\];\n', glsl, re.M)
    if not sh: raise ValueError('no shared array (the stage buffer)')
    arr, words = sh.group(1), int(sh.group(2))
    if words * 4 < lay[3]: raise ValueError(f'shared memory {words * 4} bytes < stage buffer {lay[3]}')
    loc = re.search(r'^layout\(local_size_x = \d+, local_size_y = 1, local_size_z = 1\) in;\n', glsl, re.M)
    if not loc or not re.search(r'^void main\(\)\n', glsl, re.M): raise ValueError('unexpected GLSL form')
    n = m['threads']
    ob, mb, ib = lay[0] // 4, lay[1] // 4, lay[2] // 4
    pw = max(m['payload'], 0x10) // 4
    head = glsl.replace(loc.group(0), '')
    head = re.sub(r'^void main\(\)\n', 'void dm_stage_body()\n', head, count=1, flags=re.M)
    head = head.replace('#version 460\n', '#version 460\n#extension GL_EXT_mesh_shader : require\n', 1)
    # blocks come after the translator's own declarations (they are unchanged), before the shared array
    pre = f'layout(local_size_x = {n}, local_size_y = 1, local_size_z = 1) in;\n'
    pre += 'layout(push_constant) uniform PC { uint first_group; } dm_pc;\n'
    pre += f'struct DmPayload {{ uint w[{pw}]; }};\ntaskPayloadSharedEXT DmPayload dm_pl;\n'
    head = head.replace(sh.group(0), pre + sh.group(0), 1)
    sync = '    memoryBarrierShared();\n    barrier();\n'
    if m['stage'] == 'ts':
        main = ('void main()\n{\n    uint dm_t = gl_LocalInvocationIndex;\n'
                f'    if (dm_t == 0u) {{ {arr}[{ib}] = dm_pc.first_group + gl_WorkGroupID.x; {arr}[{ob + 1}] = 0u; }}\n' + sync +
                '    dm_stage_body();\n' + sync +
                f'    for (uint i = dm_t; i < {pw}u; i += {n}u) dm_pl.w[i] = {arr}[{ob} + i];\n'
                f'    EmitMeshTasksEXT(min({arr}[{ob + 1}], 4096u), 1u, 1u);\n}}\n')
        return head + main
    topo = {1: 'points', 2: 'lines', 3: 'triangles'}[m['topology']]
    per = PER[m['topology']]
    omap, pmap = m['omap'], m['pmap']
    if any(attr(a) is None for a in pmap): raise ValueError(f'per-primitive system values {[hex(a) for a in pmap if attr(a) is None]}')
    unhandled = [a for a in omap if attr(a) is None]
    wv = len(omap)
    vtot = wv * SKEW * -(-m['maxv'] // SKEW)
    vlocs = sorted({attr(a)[1] for a in omap if attr(a) and attr(a)[0] == 'loc'})
    plocs = sorted({attr(a)[1] for a in pmap})
    if flat:
        if m['topology'] != 3 or not pmap or 3 * m['maxp'] > 256: raise ValueError('flat variant not applicable')
        mv = 3 * m['maxp']
    else:
        mv = m['maxv']
    decl = f'layout({topo}, max_vertices = {mv}, max_primitives = {m["maxp"]}) out;\n'
    for l in vlocs: decl += f'layout(location = {l}) out vec4 dm_v{l}[{mv}];\n'
    for l in plocs:
        decl += (f'layout(location = {l}) flat out vec4 dm_p{l}[{mv}];\n' if flat else
                 f'layout(location = {l}) perprimitiveEXT out vec4 dm_p{l}[{m["maxp"]}];\n')
    head = head.replace(pre, pre + decl, 1) if False else head.replace(sh.group(0), decl + sh.group(0), 1)

    def vsets(vexpr):
        """statements writing vertex outputs for the vertex whose source (stage buffer) index is vexpr"""
        s = ''
        byloc = {}
        for k, a in enumerate(omap):
            t = attr(a)
            if t is None: continue
            idx = f'{ob} + ({vexpr} / {SKEW}u) * {wv * SKEW}u + {k * SKEW}u + {vexpr} % {SKEW}u'
            if t[0] == 'pos': byloc.setdefault('pos', {})[t[1]] = idx
            else: byloc.setdefault(t[1], {})[t[2]] = idx
        return byloc

    def pexpr(pexp):
        byloc = {}
        for k, a in enumerate(pmap):
            t = attr(a)
            idx = f'{ob} + {vtot} + ({pexp} / {SKEW}u) * {len(pmap) * SKEW}u + {k * SKEW}u + {pexp} % {SKEW}u'
            if t[0] == 'loc': byloc.setdefault(t[1], {})[t[2]] = idx
            else: byloc.setdefault('pos', {})[t[1]] = idx   # (not reachable: rejected above)
        return byloc

    body = ('void main()\n{\n    uint dm_t = gl_LocalInvocationIndex;\n'
            + (f'    for (uint i = dm_t; i < {pw}u; i += {n}u) {arr}[{ib} + i] = dm_pl.w[i];\n' if m['has_task'] else '') + sync +
            f'    if (dm_t == 0u) {{ {arr}[{ib}] = ' + ('gl_WorkGroupID.x' if m['has_task'] else 'dm_pc.first_group + gl_WorkGroupID.x') + f'; {arr}[{mb} + 3] = 0u; }}\n' + sync +
            '    dm_stage_body();\n' + sync +
            f'    uint np = min({arr}[{mb} + 3], {m["maxp"]}u);\n')
    if not flat:
        body += f'    SetMeshOutputsEXT({m["maxv"]}u, np);\n'
        body += f'    for (uint v = dm_t; v < {m["maxv"]}u; v += {n}u)\n    {{\n'
        for key, comps in vsets('v').items():
            tgt = 'gl_MeshVerticesEXT[v].gl_Position' if key == 'pos' else f'dm_v{key}[v]'
            body += f'        {tgt} = {vec4_expr(arr, comps)};\n'
        body += '    }\n'
        idx = {1: f'gl_PrimitivePointIndicesEXT[i] = min({arr}[{mb} + 4 + i] & 255u, {m["maxv"] - 1}u);',
               2: f'gl_PrimitiveLineIndicesEXT[i / 2u][i % 2u] = min({arr}[{mb} + 4 + i] & 255u, {m["maxv"] - 1}u);',
               3: f'gl_PrimitiveTriangleIndicesEXT[i / 3u][i % 3u] = min({arr}[{mb} + 4 + i] & 255u, {m["maxv"] - 1}u);'}[m['topology']]
        body += f'    for (uint i = dm_t; i < np * {per}u; i += {n}u) {idx}\n'
        if plocs:
            body += f'    for (uint p = dm_t; p < np; p += {n}u)\n    {{\n'
            for key, comps in pexpr('p').items():
                body += f'        dm_p{key}[p] = {vec4_expr(arr, comps)};\n'
            body += '    }\n'
    else:
        body += '    SetMeshOutputsEXT(3u * np, np);\n'
        body += f'    for (uint i = dm_t; i < np * 3u; i += {n}u)\n    {{\n        uint v = min({arr}[{mb} + 4 + i] & 255u, {m["maxv"] - 1}u);\n        uint p = i / 3u;\n'
        for key, comps in vsets('v').items():
            tgt = 'gl_MeshVerticesEXT[i].gl_Position' if key == 'pos' else f'dm_v{key}[i]'
            body += f'        {tgt} = {vec4_expr(arr, comps)};\n'
        for key, comps in pexpr('p').items():
            body += f'        dm_p{key}[i] = {vec4_expr(arr, comps)};\n'
        body += '    }\n'
        body += f'    for (uint p = dm_t; p < np; p += {n}u) gl_PrimitiveTriangleIndicesEXT[p] = uvec3(3u * p, 3u * p + 1u, 3u * p + 2u);\n'
    body += '}\n'
    return head + body, unhandled


def run(args, **k):
    r = subprocess.run(args, capture_output=True, text=True, **k)
    return r.returncode, (r.stdout + r.stderr).strip()


def descriptors(spv):
    """{(set, binding)} of a SPIR-V's decorated variables, with their type text, for the unchanged-bindings check"""
    rc, dis = run([EXE('spirv-dis'), spv])
    if rc: raise ValueError('spirv-dis: ' + dis[:200])
    ds, bd = {}, {}
    for mm in re.finditer(r'OpDecorate (%\w+) DescriptorSet (\d+)', dis): ds[mm.group(1)] = int(mm.group(2))
    for mm in re.finditer(r'OpDecorate (%\w+) Binding (\d+)', dis): bd[mm.group(1)] = int(mm.group(2))
    bb = set(re.findall(r'OpDecorate (%\w+) BufferBlock', dis))
    ptr = {mm.group(1): mm.group(2) for mm in re.finditer(r'(%\w+) = OpTypePointer \w+ (%\w+)', dis)}
    ty = {}
    for mm in re.finditer(r'(%\w+) = OpVariable (%\w+) (Uniform|UniformConstant|StorageBuffer)', dis):
        ssbo = mm.group(3) == 'StorageBuffer' or ptr.get(mm.group(2)) in bb      # SPIR-V 1.3 BufferBlock == 1.6 StorageBuffer
        ty[mm.group(1)] = 'ssbo' if ssbo else mm.group(3)
    return {(ds[v], bd[v]): ty.get(v) for v in bd if v in ds}


def limits_of(m, lay, flat=False):
    """rough resource figures for the report: output memory bytes (NV style: 16 B a vec4 a vertex / primitive, 12 B indices a primitive), shared, payload"""
    mv = 3 * m['maxp'] if flat else m['maxv']
    nv = len({attr(a)[1] for a in m['omap'] if attr(a) and attr(a)[0] == 'loc'})
    npl = len({attr(a)[1] for a in m['pmap']})
    if m['stage'] == 'ts':
        return {'shared': lay[3], 'payload': max(m['payload'], 16)}
    return {'verts': mv, 'prims': m['maxp'], 'threads': m['threads'], 'shared': lay[3], 'payload': max(m['payload'], 16) if m['has_task'] else 0,
            'outmem': mv * 16 * (1 + nv) + m['maxp'] * (16 * npl + 3 * 4) + (mv * 16 * npl if flat else 0)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys')
    ap.add_argument('--flat', action='store_true')
    ap.add_argument('--out', default=ns.OUT)
    a = ap.parse_args()
    mesh = ns.ref_module('dmsh_meshstage')
    rows, _ = ns.load_tables()
    want = set(a.keys.split(',')) if a.keys else None
    packs, report, ok = {}, {}, 0
    tmp = tempfile.mkdtemp(prefix='nvn_mesh_')
    for name, idx, stname, key, src in rows:
        if ns.STAGE_OF_NAME.get(stname, name[:2]) not in ('ts', 'ms') or want and key not in want: continue
        if key in report: continue
        variants = [False] + ([True] if a.flat else [])
        for flat in variants:
            suffix = '.meshflat.spv' if flat else '.mesh.spv'
            try:
                if name not in packs: packs[name] = ns.records(open(os.path.join(ns.SPLIT, 'pkg', name), 'rb').read())
                rec = packs[name][idx]
                ci = rec.find(ns.CTL_MAGIC)
                m = mesh.info(rec[ci:], rec[0x38:ci])
                lay = mesh.isbe_layout(m)
                if flat and (m['stage'] != 'ms' or not m['pmap'] or m['topology'] != 3 or 3 * m['maxp'] > 256): break
                src_spv = os.path.join(a.out, key + '.spv')
                rc, g = run([EXE('spirv-cross'), src_spv, '--vulkan-semantics', '--version', '460'])
                if rc: raise ValueError('spirv-cross: ' + g[:200])
                res = build_glsl(g, m, lay, flat)
                glsl, unhandled = res if isinstance(res, tuple) else (res, [])
                gp = os.path.join(tmp, key + ('f' if flat else '') + ('.task' if m['stage'] == 'ts' else '.mesh'))
                open(gp, 'w').write(glsl)
                outp = os.path.join(a.out, key + suffix)
                rc, g = run([EXE('glslangValidator'), '-V', '--target-env', 'vulkan1.3', '-o', outp, gp])
                if rc: raise ValueError('glslang: ' + g[:600])
                rc, g = run([EXE('spirv-val'), '--target-env', 'vulkan1.3', outp])
                if rc: raise ValueError('spirv-val: ' + g[:400])
                d0, d1 = descriptors(src_spv), descriptors(outp)
                if d0 != d1: raise ValueError(f'descriptor bindings changed: {sorted(set(d0.items()) ^ set(d1.items()))}')
                loc = lambda ad: sorted({attr(x)[1] for x in ad if attr(x) and attr(x)[0] == 'loc'})
                report[key + (':flat' if flat else '')] = {'stage': m['stage'], 'threads': m['threads'], 'payload': m['payload'], 'has_task': m['has_task'],
                    'topology': m['topology'], 'maxv': m['maxv'], 'maxp': m['maxp'], 'vertex_locs': loc(m['omap']), 'prim_locs': loc(m['pmap']),
                    'prim_words': [hex(x) for x in m['pmap']], 'unhandled_omap': [hex(x) for x in unhandled], 'status': 'ok', **limits_of(m, lay, flat)}
                ok += not flat
            except Exception as e:
                report[key + (':flat' if flat else '')] = {'status': 'failed', 'error': f'{type(e).__name__}: {e}'}
                print(f'{key}{" flat" if flat else ""}: FAILED {e}', file=sys.stderr)
    json.dump(report, open(os.path.join(a.out, 'mesh_summary.json'), 'w'), indent=1, sort_keys=True)
    n = len([k for k in report if not k.endswith(':flat')])
    print(f'{ok}/{n} task/mesh shaders written (*.mesh.spv); summary in mesh_summary.json')
    import shutil; shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok == n else 1


if __name__ == '__main__':
    sys.exit(main())
