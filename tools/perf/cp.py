# cp.py <timeline> [max frames]: the critical path of each frame, walked back from its present: which thread ran,
# who woke it and how long the wake took, and for GPU waits the chain queue -> renderer -> driver -> GPU -> completion.
# Needs HOST_TIMELINE_MIN_US=0 (wake events). Prints the average microseconds per frame by category.
import sys, collections, bisect

waits = collections.defaultdict(list)    # tid -> [(start, end, kind, arg)]
wakes = collections.defaultdict(list)    # wakee -> [(t, waker)]
presents = []
gpudone, vksubmit, rcommit, subq = [], [], [], {}   # (fence, t) sorted by fence; subq: fence -> (t, tid)
rsteps = []                              # (end, start)
for line in open(sys.argv[1], errors='replace'):
    p = line.split()
    if len(p) < 3: continue
    try: t = int(p[0]); tid = int(p[1])
    except ValueError: continue
    k = p[2]
    if k == 'blocked': d = int(p[4]); waits[tid].append((t - d, t, 'blocked', p[3]))
    elif k == 'sleep': d = int(p[3]); waits[tid].append((t, t + d, 'sleep', None))
    elif k == 'wake': wakes[int(p[3][1:])].append((t, tid))
    elif k == 'present': presents.append((t, tid))
    elif k == 'acquire': d = int(p[5]) + int(p[8]); waits[tid].append((t - d, t, 'acquire', None))
    elif k == 'gpuwait': d = int(p[3]); waits[tid].append((t - d, t, 'gpuwait', int(p[5].split('=')[1])))
    elif k == 'rcall': d = int(p[3]); waits[tid].append((t - d, t, 'rcall', None))
    elif k == 'room': d = int(p[3]); waits[tid].append((t - d, t, 'room', None))
    elif k == 'gpudone': gpudone.append((int(p[3].split('=')[1]), t))
    elif k == 'vksubmit': vksubmit.append((int(p[3].split('=')[1]), t))
    elif k == 'rcommit': rcommit.append((int(p[3].split('=')[1]), t))
    elif k == 'subq': subq[int(p[3].split('=')[1])] = (t, tid)
    elif k == 'rstep': d = int(p[3]); rsteps.append((t, t - d))
for l in waits.values(): l.sort(key=lambda w: w[1])
ends = {tid: [w[1] for w in l] for tid, l in waits.items()}
for l in wakes.values(): l.sort()
gpudone.sort(); vksubmit.sort(); rcommit.sort(); rsteps.sort()
rstep_ends = [r[0] for r in rsteps]

def first_ge(lst, fence):
    i = bisect.bisect_left(lst, (fence, -1))
    return lst[i] if i < len(lst) else None

def last_wait(tid, time):
    e = ends.get(tid)
    if not e: return None
    i = bisect.bisect_right(e, time) - 1
    return waits[tid][i] if i >= 0 else None

def last_wake(tid, lo, hi):
    l = wakes.get(tid)
    if not l: return None
    i = bisect.bisect_right(l, (hi, 1 << 60)) - 1
    return l[i] if i >= 0 and l[i][0] >= lo else None

tot = collections.Counter(); frames = 0; aborted = collections.Counter()
maxf = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
verbose = len(sys.argv) > 3
for fi in range(max(1, len(presents) - maxf - 5), len(presents) - 5):
    P, T = presents[fi]; P0 = presents[fi - 1][0]
    tid, time = T, P
    acc = collections.Counter(); steps = 0; trace = []
    def add(k, d):
        acc[k] += d
        if verbose: trace.append('%s %d' % (k, d))
    while time > P0 and steps < 2000:
        steps += 1
        w = last_wait(tid, time)
        if not w or w[1] <= P0:
            add('run t%d' % tid, time - P0); time = P0; break
        s, e, kind, arg = w
        if time > e: add('run t%d' % tid, time - e)
        if kind == 'sleep':
            add('sleep t%d' % tid, e - max(s, P0)); time = s
        elif kind in ('acquire', 'rcall', 'room'):
            add(kind + ' t%d' % tid, e - max(s, P0)); time = s
        elif kind == 'gpuwait':
            g = first_ge(gpudone, arg)
            if not g or g[1] <= s or g[1] > e + 200:
                add('gpuwait short', e - max(s, P0)); time = s; continue
            V, gt = g
            add('fence: done -> game thread runs', e - gt)
            v = first_ge(vksubmit, V); c = first_ge(rcommit, V); q = subq.get(V)
            if not v or not c or not q or v[0] != V or c[0] != V:
                add('fence: unknown chain', gt - max(s, P0)); time = s; continue
            vt, ct, (qt, qtid) = v[1], c[1], q
            add('fence: GPU (vkQueueSubmit -> done)', gt - vt)
            add('fence: driver thread (commit -> vkQueueSubmit)', vt - ct)
            i = bisect.bisect_left(rstep_ends, ct)
            rs = rsteps[i][1] if i < len(rsteps) else ct
            rs = max(rs, qt)
            add('fence: renderer records the submission', ct - rs)
            add('fence: submission waits in the renderer queue', rs - qt)
            tid, time = qtid, qt
        else:  # blocked in the guest kernel
            wk = last_wake(tid, s, e)
            if not wk:
                add('blocked, no wake seen (%s)' % arg, e - max(s, P0)); time = s; continue
            wt, waker = wk
            add('wake latency', e - wt)
            if waker == 0:
                add('woken by a host thread (%s)' % arg, wt - max(s, P0)); time = s; continue
            tid, time = waker, wt
    if steps >= 2000: aborted['steps'] += 1; continue
    frames += 1
    for k, v in acc.items(): tot[k] += v
    if verbose and frames <= int(sys.argv[3]): print('frame', fi, 'len', P - P0, '|', ' / '.join(reversed(trace)))
span = presents[-6][0] - presents[max(1, len(presents) - maxf - 5) - 1][0]
print('%d frames, mean frame %.0f us (%.1f fps); critical path per frame:' % (frames, span / max(frames, 1), 1e6 * frames / max(span, 1)))
s = sum(tot.values())
grp = collections.Counter()
for k, v in tot.items(): grp['run (game threads)' if k.startswith('run t') else k] += v
for k, v in grp.most_common(): print('  %7.0f us  %5.1f%%  %s' % (v / frames, 100.0 * v / s, k))
print('run time on the path by thread:')
for k, v in sorted(((k, v) for k, v in tot.items() if k.startswith('run t')), key=lambda x: -x[1])[:14]:
    print('  %7.0f us  %s' % (v / frames, k))
if aborted: print('aborted', dict(aborted))
