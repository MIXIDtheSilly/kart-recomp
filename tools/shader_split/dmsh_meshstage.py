"""NVN2 (Switch 2) task and mesh shaders as Metal object and mesh functions.

The translator reads them as compute shaders (one dimension, the control section's thread count) whose stage buffer, written and read by ISBEWR / ISBERD
(InstEmitAmpereMem.cs AIsbe), lives in shared memory in three regions this module lays out (isbe_layout, the job's "isbe"): the output attributes, the map
and the input attributes. The layout is the hardware's, as Mesa's NAK writes it (nak_nir_lower_mesh_intrinsics.c):

  mesh output attributes, per vertex v and attribute word k (the k-th word the SPH's output map marks, in address order):
      (v / 32) * words * 128 + k * 128 + (v % 32) * 4                       ("skew": 32 vertices a group, a 128-byte row per word)
  mesh per-primitive attributes, after every vertex group, per primitive p and word k of the second SPH's output map (as NAK's skew_prim_attr_used):
      vertex total + (p / 32) * prim words * 128 + k * 128 + (p % 32) * 4
  mesh map: the primitive count as a word at byte 3, then the primitives' vertex indices as bytes from byte 4 (3 per triangle); the translator gives each map byte a
      word of its own (map byte b at word b), so the count is word 3 and index i word 4 + i
  mesh input: the workgroup index at 0, the task's payload from 0x10 (as the task wrote it)
  task output: the count of mesh workgroups at 4, the payload from 0x10
  task input: the workgroup index at 0

rewrite() turns the translated kernel into the Metal function: the input region is filled first (the payload, then the workgroup index), the shader's body
runs, and after a barrier the threads read the regions: a mesh sets its vertices, indices and primitive count, a task its payload and the mesh grid.

Control section (NVN2, version 0xc9), task and mesh: +0x56 stage (6 task, 7 mesh), +0x58 u16 threads, +0x5a u16 payload bytes (a task's output, its mesh's input;
0x80 for a mesh without a task), +0x5e has a task shader, +0x5f topology (3 triangles), +0x60 u16 maximum vertices, +0x62 u16 maximum primitives, +0x64 u32 the code-area offset of a second SPH
(a passthrough geometry header, as NAK's encode_gs_mesh_header) whose output map lists the per-primitive attributes, 0 when there are none. Measured on Mario Kart World's 25
(7 task, 18 mesh): their thread counts, sizes and output maps agree with the code's ISBEWR offsets; 11 of its mesh shaders have per-primitive generics (up to 4), which their
pixel shaders read flat.
"""
import re
import struct

STAGE = {6: 'ts', 7: 'ms'}
TOPOLOGY = {1: ('point', 1), 2: ('line', 2), 3: ('triangle', 3)}
IN_HEADER = 0x10        # the input region's header (the workgroup index at 0) before a task's payload
SKEW = 32               # vertices of a skew group
MAX_TG = 32768          # Metal's threadgroup memory limit, which the three regions share


def _omap(sph):
    """The attribute addresses an SPH's output map marks (NAK sph.rs): system values A/B bits 400..432 (bit i = address 4 i), generics bits 432..560 (bit i = address 0x80 + 4 i)"""
    ab = struct.unpack_from('<I', sph, 0x32)[0]
    g = int.from_bytes(sph[0x36:0x46], 'little')
    return [4 * i for i in range(32) if ab >> i & 1] + [0x80 + 4 * i for i in range(128) if g >> i & 1]


def info(ctl, code):
    """The task / mesh fields of a control section and the attributes its SPH (the first 0x80 bytes of the code area) marks written, with a mesh's per-primitive
    attributes (pmap) from its second SPH: a dict, or None for another stage"""
    if len(ctl) < 0x68 or ctl[0x56] not in STAGE: return None
    threads, payload = struct.unpack_from('<HH', ctl, 0x58)
    maxv, maxp = struct.unpack_from('<HH', ctl, 0x60)
    d = {'stage': STAGE[ctl[0x56]], 'threads': threads, 'payload': payload, 'has_task': bool(ctl[0x5e]), 'topology': ctl[0x5f], 'maxv': maxv, 'maxp': maxp}
    d['omap'] = _omap(code)
    gs = struct.unpack_from('<I', ctl, 0x64)[0] if d['stage'] == 'ms' and len(ctl) >= 0x68 else 0
    if gs and (gs + 0x80 > len(code) or struct.unpack_from('<I', code, gs)[0] & 0x3c1f != 0x1001): raise ValueError(f'no geometry SPH at the per-primitive header offset {gs:#x}')
    d['pmap'] = _omap(code[gs:gs + 0x80]) if gs else []
    return d


def isbe_layout(m):
    """[output base, map base, input base, total bytes] of the stage buffer in shared memory; ValueError when it does not fit Metal's threadgroup memory"""
    if m['stage'] == 'ms':
        if m['topology'] not in TOPOLOGY: raise ValueError(f'mesh topology {m["topology"]}')
        out = len(m['omap']) * 128 * -(-m['maxv'] // SKEW) + len(m.get('pmap', [])) * 128 * -(-m['maxp'] // SKEW)
        mp = 4 * (4 + m['maxp'] * TOPOLOGY[m['topology']][1])   # a word per map byte (InstEmitAmpereMem.cs AIsbe)
        inp = max(m['payload'], IN_HEADER) if m['has_task'] else IN_HEADER
    else:
        out, mp, inp = max(m['payload'], IN_HEADER), 0, IN_HEADER
    out = (out + 15) & ~15; inp = (inp + 15) & ~15
    total = out + mp + inp
    if total > MAX_TG: raise ValueError(f'stage buffer of {total} bytes exceeds the threadgroup memory limit')
    return [0, out, out + mp, total]


def _attr_name(a):
    """The vertex output member for attribute address a: the position, a generic location's component, or None (not passed on)"""
    if 0x70 <= a < 0x80: return 'gl_Position', (a - 0x70) // 4
    if 0x80 <= a < 0x280: return f'm{(a - 0x80) // 16}', (a - 0x80) // 4 % 4
    return None, None


def rewrite(msl, m, layout):
    """The translated kernel (spirv-cross MSL of a compute shader) as a Metal object (task) or mesh function; ValueError when it is not in the expected form"""
    kern = re.search(r'^kernel void main0\((.*)\)\n\{\n', msl, re.M)
    if not kern: raise ValueError('no kernel entry')
    sh = re.search(r'^([ \t]*)threadgroup spvUnsafeArray<uint, (\d+)> (\w+);\n', msl[kern.end():], re.M)
    if not sh: raise ValueError('no shared memory array (the stage buffer)')
    arr, words = sh.group(3), int(sh.group(2))
    if words * 4 < layout[3]: raise ValueError(f'shared memory of {words * 4} bytes, the stage buffer needs {layout[3]}')
    params = [p.strip() for p in kern.group(1).split(', ') if p.strip()]
    keep = [p for p in params if not re.search(r'\[\[(threadgroup_position_in_grid|thread_position_in_threadgroup|thread_index_in_threadgroup|thread_position_in_grid|threadgroups_per_grid)\]\]', p)]
    names = {re.search(r'\[\[(\w+)', p).group(1): re.search(r'(\w+) \[\[', p).group(1) for p in params if '[[' in p}
    # The body becomes a function of its own (its returns end the shader's part; the stage buffer is read after), called with what it used
    tid = names.get('thread_position_in_threadgroup', 'gl_LocalInvocationID')
    tgid = names.get('threadgroup_position_in_grid', 'gl_WorkGroupID')
    gid = names.get('thread_position_in_grid', 'gl_GlobalInvocationID')
    ngr = names.get('threadgroups_per_grid', 'gl_NumWorkGroups')
    body_start = kern.end() + sh.end()
    depth, i = 1, body_start
    while depth:
        depth += {'{': 1, '}': -1}.get(msl[i], 0); i += 1
    body = msl[body_start:i - 1]
    n = m['threads']
    decl = f'threadgroup uint* {arr}'
    fargs = ', '.join([re.sub(r'\s*\[\[[^]]*\]\]$', '', p) for p in keep] + [f'uint3 {tid}', f'uint3 {tgid}', f'uint3 {gid}', f'uint3 {ngr}', decl])   # the entry's own parameters, without their attributes
    head = msl[:kern.start()]
    func = f'static inline __attribute__((always_inline)) void dm_stage_body({fargs})\n{{\n{body}}}\n\n'
    call = ', '.join([re.search(r'(\w+)(?: \[\[[^]]*\]\])?$', p).group(1) for p in keep] + [tid, tgid, gid, ngr, arr])
    ob, mb, ib = layout[0] // 4, layout[1] // 4, layout[2] // 4
    pw = max(m['payload'], IN_HEADER) // 4   # payload words, header included (a task's output region, a mesh's input region)
    if m['stage'] == 'ts':
        pre = (f'struct dm_payload {{ uint w[{pw}]; }};\n')
        entry = (f'[[object]] void main0({", ".join(keep)}, object_data dm_payload& dm_pl [[payload]], mesh_grid_properties dm_grid, uint dm_t [[thread_index_in_threadgroup]], '
                 f'uint3 dm_tg [[threadgroup_position_in_grid]], uint3 dm_ng [[threadgroups_per_grid]])\n{{\n'
                 f'    threadgroup uint {arr}[{words}];\n'
                 f'    if (dm_t == 0u) {arr}[{ib}] = dm_tg.x;\n'
                 f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                 f'    dm_stage_body({call.replace(tid, "uint3(dm_t, 0u, 0u)", 1).replace(tgid, "dm_tg", 1).replace(gid, f"uint3(dm_tg.x * {n}u + dm_t, 0u, 0u)", 1).replace(ngr, "dm_ng", 1)});\n'
                 f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
                 f'    for (uint i = dm_t; i < {pw}u; i += {n}u) dm_pl.w[i] = {arr}[{ob} + i];\n'
                 f'    if (dm_t == 0u) dm_grid.set_threadgroups_per_grid(uint3({arr}[{ob + 1}], 1u, 1u));\n'
                 f'}}\n')
        return head + pre + func + entry
    # mesh: the vertex type the pixel stage reads (float4 per generic location it outputs), then the stage buffer read back
    kind, per = TOPOLOGY[m['topology']]
    locs = sorted({a for a in (_attr_name(a)[0] for a in m['omap']) if a and a != 'gl_Position'}, key=lambda s: int(s[1:]))
    vt = 'struct dm_vertex\n{\n    float4 gl_Position [[position]];\n' + ''.join(f'    float4 {l} [[user(locn{l[1:]})]];\n' for l in locs) + '};\n'
    pmap = m.get('pmap', [])
    plocs = sorted({a for a in (_attr_name(a)[0] for a in pmap) if a and a != 'gl_Position'}, key=lambda s: int(s[1:]))
    if any(_attr_name(a)[0] is None for a in pmap): raise ValueError(f'per-primitive system values {[hex(a) for a in pmap if _attr_name(a)[0] is None]}')
    if plocs: vt += 'struct dm_prim\n{\n' + ''.join(f'    float4 {l} [[user(locn{l[1:]})]];\n' for l in plocs) + '};\n'
    mesh_t = f'mesh<dm_vertex, {"dm_prim" if plocs else "void"}, {m["maxv"]}, {m["maxp"]}, topology::{kind}>'
    words_v = len(m['omap'])
    vtot = words_v * 128 * -(-m['maxv'] // SKEW) // 4   # words of the vertex groups, before the primitive groups
    psets = ''.join(f'        q.{_attr_name(a)[0]}[{_attr_name(a)[1]}] = as_type<float>({arr}[{ob + vtot} + (p / {SKEW}u) * {len(pmap) * SKEW}u + {k * SKEW}u + p % {SKEW}u]);\n' for k, a in enumerate(pmap))
    prims = (f'    for (uint p = dm_t; p < np; p += {n}u)\n    {{\n        dm_prim q = {{}};\n' + psets + '        dm_out.set_primitive(p, q);\n    }\n') if plocs else ''
    sets = []
    for k, a in enumerate(m['omap']):
        nm, c = _attr_name(a)
        if nm: sets.append(f'        o.{nm}[{c}] = as_type<float>({arr}[{ob} + (v / {SKEW}u) * {words_v * SKEW}u + {k * SKEW}u + v % {SKEW}u]);\n')
    pre = vt + (f'struct dm_payload {{ uint w[{pw}]; }};\n' if m['has_task'] else '')
    pl = f', const object_data dm_payload& dm_pl [[payload]]' if m['has_task'] else ''
    fill = (f'    for (uint i = dm_t; i < {pw}u; i += {n}u) {arr}[{ib} + i] = dm_pl.w[i];\n' if m['has_task'] else '')
    entry = (f'[[mesh]] void main0({", ".join(keep)}{pl}, {mesh_t} dm_out, uint dm_t [[thread_index_in_threadgroup]], '
             f'uint3 dm_tg [[threadgroup_position_in_grid]], uint3 dm_ng [[threadgroups_per_grid]])\n{{\n'
             f'    threadgroup uint {arr}[{words}];\n'
             + fill +
             f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
             f'    if (dm_t == 0u) {{ {arr}[{ib}] = dm_tg.x; {arr}[{mb} + 3] = 0u; }}\n'
             f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
             f'    dm_stage_body({call.replace(tid, "uint3(dm_t, 0u, 0u)", 1).replace(tgid, "dm_tg", 1).replace(gid, f"uint3(dm_tg.x * {n}u + dm_t, 0u, 0u)", 1).replace(ngr, "dm_ng", 1)});\n'
             f'    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
             f'    uint np = min({arr}[{mb} + 3], {m["maxp"]}u);\n'
             f'    for (uint v = dm_t; v < {m["maxv"]}u; v += {n}u)\n    {{\n        dm_vertex o = {{}};\n' + ''.join(sets) +
             '@ZFIX@'
             f'        dm_out.set_vertex(v, o);\n    }}\n'
             f'    for (uint i = dm_t; i < np * {per}u; i += {n}u) dm_out.set_index(i, uchar({arr}[{mb} + 4 + i]));\n'
             + prims +
             f'    if (dm_t == 0u) dm_out.set_primitive_count(np);\n'
             f'}}\n')
    # the depth conventions as a vertex shader's (dmsh_build.py vs_depth): main0 maps z for GL's (near=-w), main0_z leaves it; the host picks by the game's depth mode
    gl = entry.replace('@ZFIX@', '        o.gl_Position.z = fma(o.gl_Position.z, 0.5, o.gl_Position.w * 0.5);\n')
    raw = entry.replace('@ZFIX@', '').replace('[[mesh]] void main0(', '[[mesh]] void main0_z(', 1)
    return head + pre + func + gl + '\n' + raw
