#!/usr/bin/env python3
"""Compute / task / mesh shaders whose barriers hang Intel GPUs -> work/nvn_shaders/<file>.bar.spv, which the renderer loads on Intel.

The game's compute shaders start with "if (id >= count) return;" and then wait at workgroup barriers (158 of the 205 that use barriers
in 1.8.0). NVIDIA lets the remaining threads through; an Intel GPU waits for the returned ones forever (the handheld's device loss). Turning
the barriers into memory barriers (Ryujinx's way, shaders.cpp drop_divergent_barriers) stops the hang but breaks every reduction they
do (black screen). Here the threads stay for the barriers instead:

  - every return inside a construct sets a "dead" variable and branches to the construct's merge block;
  - from then on, what a dead thread would change outside itself is skipped: stores outside Function / Private memory, image writes,
    atomics, calls of functions that do any of these (results come from an OpPhi with an undefined value on the skipped side);
  - a loop without a barrier in it exits after one pass for a dead thread, so its garbage indices cannot spin it forever.

Shared-memory slots of dead threads stay unwritten, as on NVIDIA. Every output is checked with spirv-val; a shader that cannot be
rewritten (a called function with a barrier that writes outside shared memory) or fails validation gets no .bar.spv and the renderer falls back to dropping its barriers. A shader with barriers but no
early return gets a plain copy, which tells the renderer its barriers are fine as they are.

usage: nvn_barfix.py [--dir DIR] [--force] [--only KEY]
"""
import argparse, concurrent.futures, json, os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
BIN = paths.get('VULKAN_BIN')   # third_party\spirv\bin (VULKAN_BIN overrides)
EXE = lambda n: os.path.join(BIN, n + '.exe')

OpTypeBool, OpTypePointer, OpConstantTrue, OpConstantFalse, OpUndef = 20, 32, 41, 42, 1
OpFunction, OpFunctionParameter, OpFunctionEnd, OpFunctionCall = 54, 55, 56, 57
OpVariable, OpImageTexelPointer, OpLoad, OpStore, OpCopyMemory, OpCopyMemorySized = 59, 60, 61, 62, 63, 64
OpAccessChain, OpInBoundsAccessChain, OpPtrAccessChain, OpInBoundsPtrAccessChain, OpCopyObject = 65, 66, 67, 70, 83
OpImageWrite, OpLogicalOr, OpLogicalAnd, OpLogicalNot = 99, 166, 167, 168
OpControlBarrier, OpPhi, OpLoopMerge, OpSelectionMerge, OpLabel = 224, 245, 246, 247, 248
OpBranch, OpBranchConditional, OpReturn, OpReturnValue = 249, 250, 253, 254
OpEntryPoint = 15
ATOMICS = set(range(228, 243)) | {318, 319, 5614, 5615, 6035}   # every atomic but OpAtomicLoad (227)
ATOMIC_NO_RESULT = {228, 319}                                     # OpAtomicStore, OpAtomicFlagClear
PTR_DEFS = {OpVariable, OpImageTexelPointer, OpAccessChain, OpInBoundsAccessChain, OpPtrAccessChain, OpInBoundsPtrAccessChain,
            OpCopyObject, OpFunctionParameter}
LOCAL = {6, 7}   # Private, Function


class Fail(Exception):
    pass


def instrs(w):
    out, i = [], 5
    while i < len(w):
        n = w[i] >> 16
        if n == 0 or i + n > len(w):
            raise Fail('malformed')
        out.append(list(w[i:i + n]))
        i += n
    return out


def rewrite(w):
    """-> (new words, returns converted, side effects guarded, loops shortened); Fail when it cannot."""
    ins = instrs(w)
    bound = w[3]

    def new_id():
        nonlocal bound
        bound += 1
        return bound - 1

    op = lambda x: x[0] & 0xffff
    head = lambda o, n: o | n << 16
    entry = next((x[2] for x in ins if op(x) == OpEntryPoint), None)
    ptr_sc, id_type = {}, {}
    bool_ty = c_true = c_false = 0
    voids = {x[1] for x in ins if op(x) == 19}   # OpTypeVoid
    for x in ins:
        o = op(x)
        if o == OpTypePointer: ptr_sc[x[1]] = (x[2], x[3])
        elif o == OpTypeBool: bool_ty = x[1]
        elif o in PTR_DEFS: id_type[x[2]] = x[1]
    for x in ins:
        if op(x) == OpConstantTrue and x[1] == bool_ty: c_true = x[2]
        if op(x) == OpConstantFalse and x[1] == bool_ty: c_false = x[2]

    impure = set()   # functions with side effects (their calls are skipped like stores)

    def side_effect(x):
        o = op(x)
        if o == OpImageWrite or o in ATOMICS: return True
        if o in (OpStore, OpCopyMemory, OpCopyMemorySized):
            t = id_type.get(x[1])
            return t is None or ptr_sc.get(t, (None,))[0] not in LOCAL
        return o == OpFunctionCall and x[3] in impure

    # functions: the entry one is rewritten; a called one with a barrier cannot be, one with side effects is skipped as a whole
    funcs, cur = {}, None
    for idx, x in enumerate(ins):
        if op(x) == OpFunction: cur = x[2]; funcs[cur] = [idx, None]
        elif op(x) == OpFunctionEnd: funcs[cur][1] = idx
    if entry not in funcs: raise Fail('no entry function')
    # one with a barrier (dmsh_hmma16816_f16: NVIDIA's warp matrix multiply done through shared memory) is run by dead threads too,
    # all its lanes take part; that only works when it writes nothing but shared memory
    collective = {f for f, (a, b) in funcs.items() if f != entry and any(op(x) == OpControlBarrier for x in ins[a:b])}
    for f in collective:
        a, b = funcs[f]
        for x in ins[a:b]:
            if side_effect(x) and not (op(x) == OpStore and ptr_sc.get(id_type.get(x[1]), (None,))[0] == 4):
                raise Fail('a called function with a barrier and writes outside shared memory')
    while True:
        more = {f for f, (a, b) in funcs.items()
                if f != entry and f not in impure and f not in collective and any(side_effect(x) for x in ins[a:b])}
        if not more: break
        impure |= more
    a, b = funcs[entry]
    body = ins[a:b + 1]

    # loops in the entry function without a barrier in them: header label -> merge label
    label_at = {x[1]: k for k, x in enumerate(body) if op(x) == OpLabel}
    short, cur = {}, None
    for k, x in enumerate(body):
        if op(x) == OpLabel: cur = x[1]
        elif op(x) == OpLoopMerge:
            m = label_at.get(x[1])
            if m is None or label_at[cur] > m: raise Fail('loop merge before its header')
            if not any(op(y) == OpControlBarrier or (op(y) == OpFunctionCall and y[3] in collective) for y in body[label_at[cur]:m]):
                short[cur] = x[1]

    # does anything need doing? a return inside a construct (the early return) with a barrier somewhere
    merges, early = [], 0
    for x in body:
        o = op(x)
        if o in (OpSelectionMerge, OpLoopMerge): merges.append(x[1])
        elif o == OpLabel: merges = [m for m in merges if m != x[1]]
        elif o in (OpReturn, OpReturnValue) and merges: early += 1
    if not early:
        return None, 0, 0, 0

    decls = []
    if not bool_ty: bool_ty = new_id(); decls.append([head(OpTypeBool, 2), bool_ty])
    if not c_true: c_true = new_id(); decls.append([head(OpConstantTrue, 3), bool_ty, c_true])
    if not c_false: c_false = new_id(); decls.append([head(OpConstantFalse, 3), bool_ty, c_false])
    ptr_fb = next((p for p, (sc, t) in ptr_sc.items() if sc == 7 and t == bool_ty), 0)
    if not ptr_fb: ptr_fb = new_id(); decls.append([head(OpTypePointer, 4), ptr_fb, 7, bool_ty])
    undef = {}

    def undef_of(t):
        if t not in undef: undef[t] = new_id(); decls.append([head(OpUndef, 3), t, undef[t]])
        return undef[t]

    dead = new_id()
    out, mine = [], set()   # mine: ids of the OpPhis added here (their parents are right already)
    merges, cur, orig, first_label, renamed = [], None, None, True, {}
    returns = guarded = shortened = 0
    k = 0
    while k < len(body):
        x = body[k]
        o = op(x)
        if o == OpLabel:
            if cur is not None and orig != cur: renamed[orig] = cur
            merges = [m for m in merges if m != x[1]]
            cur = orig = x[1]
            out.append(x)
            if first_label:
                first_label = False
                out.append([head(OpVariable, 5), ptr_fb, dead, 7, c_false])
            k += 1
            continue
        if o in (OpSelectionMerge, OpLoopMerge):
            if o == OpLoopMerge and cur != orig: raise Fail('a side effect in a loop header block')
            merges.append(x[1])
            out.append(x)
            k += 1
            continue
        if o == OpReturnValue and merges: raise Fail('early return with a value in the entry function')
        if o == OpReturn and merges:
            out.append([head(OpStore, 3), dead, c_true])
            out.append([head(OpBranch, 2), merges[-1]])
            returns += 1
            k += 1
            continue
        if side_effect(x):
            run = []
            while k < len(body) and side_effect(body[k]):
                run.append(body[k]); k += 1
            d, do, after = new_id(), new_id(), new_id()
            out.append([head(OpLoad, 4), bool_ty, d, dead])
            out.append([head(OpSelectionMerge, 3), after, 0])
            out.append([head(OpBranchConditional, 4), d, after, do])
            out.append([head(OpLabel, 2), do])
            phis = []
            for y in run:
                y = list(y)
                if (op(y) in ATOMICS and op(y) not in ATOMIC_NO_RESULT) or (op(y) == OpFunctionCall and y[1] not in voids):
                    res, inner = y[2], new_id()
                    y[2] = inner
                    phis.append([head(OpPhi, 7), y[1], res, undef_of(y[1]), cur, inner, do])
                    mine.add(res)
                out.append(y)
            out.append([head(OpBranch, 2), after])
            out.append([head(OpLabel, 2), after])
            out.extend(phis)
            cur = after
            guarded += len(run)
            continue
        if o == OpBranch and x[1] in short and label_at.get(x[1], 1 << 30) < k:   # back edge of a barrier-free loop
            d = new_id()
            out.append([head(OpLoad, 4), bool_ty, d, dead])
            out.append([head(OpBranchConditional, 4), d, short[x[1]], x[1]])
            shortened += 1
            k += 1
            continue
        if o == OpBranchConditional and len(x) >= 4:
            t, f = x[2], x[3]
            hdr = t if t in short and short[t] == f else f if f in short and short[f] == t else None
            if hdr is not None and label_at.get(hdr, 1 << 30) < k:
                d, c2 = new_id(), new_id()
                out.append([head(OpLoad, 4), bool_ty, d, dead])
                if hdr == t:   # stay while c and not dead
                    nd = new_id()
                    out.append([head(OpLogicalNot, 4), bool_ty, nd, d])
                    out.append([head(OpLogicalAnd, 5), bool_ty, c2, x[1], nd])
                else:          # leave when c or dead
                    out.append([head(OpLogicalOr, 5), bool_ty, c2, x[1], d])
                y = list(x); y[1] = c2
                out.append(y)
                shortened += 1
                k += 1
                continue
        out.append(x)
        k += 1
    if renamed:
        for y in out:
            if op(y) == OpPhi and y[2] not in mine:
                for j in range(4, len(y), 2):
                    y[j] = renamed.get(y[j], y[j])

    first_fn = min(v[0] for v in funcs.values())
    new = ins[:first_fn] + decls + ins[first_fn:a] + out + ins[b + 1:]
    words = list(w[:5])
    words[3] = bound
    for y in new: words.extend(y)
    return words, returns, guarded, shortened


def has_barrier(w):
    try:
        return any((x[0] & 0xffff) == OpControlBarrier for x in instrs(w))
    except Fail:
        return False


def main():
    ap = argparse.ArgumentParser()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import paths
    ver = paths.get('KART_VER')
    ap.add_argument('--dir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work',
                                                  'nvn_shaders' + ('-' + ver if ver else '')))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--only')
    a = ap.parse_args()
    srcs = []
    for f in sorted(os.listdir(a.dir)):
        if a.only and not f.startswith(a.only): continue
        if f.endswith('.mesh.spv') or f.endswith('.meshflat.spv'):
            srcs.append(f)
        elif f.endswith('.spv') and f.count('.') == 1:
            try:
                with open(os.path.join(a.dir, f[:-4] + '.res.json')) as j:
                    if json.load(j).get('stage') == 'cs': srcs.append(f)
            except OSError:
                pass

    def one(f):
        src, out = os.path.join(a.dir, f), os.path.join(a.dir, f[:-4] + '.bar.spv')
        if not a.force and os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
            return 'kept', ''
        b = open(src, 'rb').read()
        w = list(struct.unpack('<%dI' % (len(b) // 4), b))
        if not has_barrier(w):
            if os.path.exists(out): os.remove(out)
            return 'none', ''
        try:
            nw, r, g, s = rewrite(w)
        except Fail as e:
            if os.path.exists(out): os.remove(out)
            return 'failed', f'{f}: {e}'
        if nw is None:
            open(out, 'wb').write(b)
            return 'copied', ''
        open(out, 'wb').write(struct.pack('<%dI' % len(nw), *nw))
        v = subprocess.run([EXE('spirv-val'), '--target-env', 'vulkan1.3', out], capture_output=True, text=True)
        if v.returncode:
            os.remove(out)
            return 'failed', f'{f}: spirv-val: {(v.stdout + v.stderr).strip()[:500]}'
        return 'rewritten', f'{f}: {r} returns, {g} side effects guarded, {s} loops shortened' if a.only else ''

    counts = {}
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 2)) as pool:
        for i, (kind, msg) in enumerate(pool.map(one, srcs)):
            counts[kind] = counts.get(kind, 0) + 1
            if msg: print(msg)
            if i % 25 == 0 or i + 1 == len(srcs):
                paths.progress(i + 1, len(srcs), 'compute shaders for Intel GPUs (barriers)')
    print(', '.join(f'{v} {k}' for k, v in sorted(counts.items())))
    # A shader that could not be rewritten is not fatal: on Intel the renderer drops its barriers instead
    return 1 if counts.get('failed') and a.only else 0


if __name__ == '__main__':
    sys.exit(main())
