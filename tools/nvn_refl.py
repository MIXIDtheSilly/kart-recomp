#!/usr/bin/env python3
"""Shader reflection for the native NVN Vulkan renderer (host/nvn/vk/shaders.cpp): work/nvn_shaders/*.res.json (dmsh_xlate) -> one table, refl.tsv.

Every resource of a translated shader is in descriptor set 0 with its own binding number (Program.cs Accessor). This names what each binding is in NVN terms,
decoded as dm/gfx/tools/dmsh_build.py reflect() / decode_tex() do (plumber-racer-2, MIT), so the host needs no JSON parser.

Row:  key <TAB> stage <TAB> c1 (hex, '-' none) <TAB> bindings, space separated  binding:kind:reg:off:aux
kinds:
  u   uniform buffer slot reg (c[3 + reg])           c1  the shader's own constant segment c[1]
  d0  driver segment c[0]                            d2  driver segment c[2] (handle tables)
  p   push constants c[24]
  s   storage buffer slot reg of the stage's c[0] table, aux = 1 if written
  su  storage buffer whose address is in uniform slot reg at byte off, aux = written
  up  compute: uniform slot reg read as a storage buffer (c[0] + 0x210 + 0x10 reg)
  gm  global memory pool table (aux = written)       gl  global memory write log
  t   texture slot reg (combined handle; 128 + r separate), aux = Ryujinx SamplerType
  i   image slot reg, aux = SamplerType
  x   not decodable (the host skips draws with it)
usage: nvn_refl.py [--out FILE]
"""
import glob, json, os, struct, sys

RECOMP = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
OUT = os.path.join(RECOMP, 'work', 'nvn_shaders' + ('-' + paths.get('KART_VER') if paths.get('KART_VER') else ''))
SPLIT = os.path.join(OUT, 'split')

TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SAMP_BASE, ROW = 0x20, 0x120, 8, 0x168, 0x568, 8
SSBO = {'vs': 0x110, 'cs': 0x310, 'gs': 0x410, 'ps': 0x510, 'ts': 0x670, 'ms': 0x770}
C0_CS_UBO, GMEM_SLOT = 0x210, 0xFF


def records(d):
    import re
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]


def c1_of_all():
    """key -> c1 bytes (the control section's +0x40 / +0x44 range of the code area, dmsh_ampere.c1_segment)"""
    out, packs = {}, {}
    for line in open(os.path.join(SPLIT, 'shader_map.tsv'), encoding='utf-8', errors='replace'):
        if line.startswith('#') or line.startswith('package'): continue
        f = line.rstrip('\n').split('\t')
        if len(f) < 4: continue
        name, idx, key = f[0], int(f[1]), f[3]
        if key in out: continue
        if name not in packs: packs[name] = records(open(os.path.join(SPLIT, 'pkg', name), 'rb').read())
        rec = packs[name][idx]
        ci = rec.find(bytes.fromhex('34127698'))
        ctl, code = rec[ci:], rec[0x38:ci]
        a, e = struct.unpack_from('<II', ctl, 0x40)
        out[key] = code[a:e] if a <= e <= len(code) else b''
    return out


def tex_slot(handle, cbuf_slot, image):
    if cbuf_slot >= 0 and (cbuf_slot & 0xffff) != 2: return None
    o0, o1, ty = handle & 0x3fff, (handle >> 14) & 0x3fff, handle >> 28
    words = [o0] if ty in (0, 3) else [o0, o1]
    for w in words:
        b = w * 4
        if b % ROW: continue
        if image and IMG_BASE <= b < IMG_BASE + ROW * IMG_SLOTS: return (b - IMG_BASE) // ROW
        if not image and TEX_BASE <= b < SEPTEX_BASE: return (b - TEX_BASE) // ROW
        if not image and SEPTEX_BASE <= b < SAMP_BASE: return 128 + (b - SEPTEX_BASE) // ROW
    return None


def row(r, c1):
    st = r['stage']
    ents = []
    for b in r['cbufs']:
        s = b['slot']
        if 3 <= s <= 16: ents.append((b['binding'], 'u', s - 3, 0, 0))
        elif s == 1: ents.append((b['binding'], 'c1', 0, 0, 0))
        elif s in (0, 2): ents.append((b['binding'], 'd%d' % s, 0, 0, 0))
        elif s == 24: ents.append((b['binding'], 'p', 0, 0, 0))
        else: ents.append((b['binding'], 'x', s, 0, 0))
    for b in r['sbufs']:
        rw = 1 if b['flags'] & 1 else 0
        cs, off = b['cb_slot'], b['cb_offset'] * 4
        if cs == GMEM_SLOT:
            if b['cb_offset'] == 1: ents.append((b['binding'], 'gl', 0, 0, 1))
            else: ents.append((b['binding'], 'gm', 0, 0, int(any('"Store"' in n or '"Atomic' in n for n in r['notes']))))
        elif cs == 0 and st == 'cs' and C0_CS_UBO <= off < C0_CS_UBO + 0x100 and (off - C0_CS_UBO) % 0x10 == 0:
            ents.append((b['binding'], 'up', (off - C0_CS_UBO) // 0x10, 0, rw))
        elif cs == 0 and st in SSBO and 0 <= off - SSBO[st] < 0x100 and (off - SSBO[st]) % 0x10 == 0:
            ents.append((b['binding'], 's', (off - SSBO[st]) // 0x10, 0, rw))
        elif cs >= 3: ents.append((b['binding'], 'su', cs - 3, off, rw))
        else: ents.append((b['binding'], 'x', cs, off, 0))
    for name, image in (('textures', False), ('images', True)):
        for t in r[name]:
            slot = tex_slot(t['handle'], t['cbuf_slot'], image) if t['array'] == 1 else None
            ents.append((t['binding'], ('i' if image else 't') if slot is not None else 'x', slot if slot is not None else 0, 0, t['type']))
    ents.sort()
    return '%s\t%s\t%s\t%s' % (r['key'], st, c1.hex() if c1 else '-', ' '.join('%d:%s:%d:%d:%d' % e for e in ents))


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(OUT, 'refl.tsv')
    c1s = c1_of_all()
    rows, unknown = [], 0
    for f in sorted(glob.glob(os.path.join(OUT, '*.res.json'))):
        r = json.load(open(f))
        line = row(r, c1s.get(r['key'], b''))
        unknown += ':x:' in line
        rows.append(line)
    with open(out, 'w', newline='\n') as fh:
        fh.write('# native NVN shader reflection (tools/nvn_refl.py): key stage c1 bindings(binding:kind:reg:off:aux)\n')
        fh.write('\n'.join(rows) + '\n')
    print(f'{len(rows)} shaders -> {out}; {unknown} with an undecodable binding')


if __name__ == '__main__':
    main()
