# sub.py <timeline>: per submission the renderer's step time against the stage binds and descriptor sets made in it.
import sys, bisect, collections
commits = []; rsteps = []
for line in open(sys.argv[1], errors='replace'):
    p = line.split()
    if len(p) < 4: continue
    if p[2] == 'rcommit' and len(p) >= 7:
        kv = dict(x.split('=') for x in p[3:7])
        commits.append((int(p[0]), int(kv['made']), int(kv['binds']), int(kv['gens'])))
    elif p[2] == 'rstep' and p[5] == 'sub=1':
        rsteps.append((int(p[0]), int(p[3])))
ends = [r[0] for r in rsteps]
rows = []
for i in range(1, len(commits)):
    t, made, binds, gens = commits[i]
    j = bisect.bisect_left(ends, t)
    if j >= len(rsteps): break
    rows.append((rsteps[j][1], made - commits[i - 1][1], binds - commits[i - 1][2], gens - commits[i - 1][3]))
big = [r for r in rows if r[2] >= 1500]
print('%d submissions, %d with 1500+ stage binds' % (len(rows), len(big)))
big.sort()
def pct(q): return big[min(len(big) - 1, int(q * len(big)))]
print('big submissions, step us (made / binds): p10 %s  p50 %s  p90 %s  p99 %s  max %s' % (pct(.1)[:3], pct(.5)[:3], pct(.9)[:3], pct(.99)[:3], big[-1][:3]))
# cost model: least squares us = a*binds + b*made
import math
sx = sy = sxx = sxy = syy = sxz = syz = 0.0
for us, made, binds, g in big:
    x, y, z = binds - made, made, us
    sxx += x * x; sxy += x * y; syy += y * y; sxz += x * z; syz += y * z
det = sxx * syy - sxy * sxy
if det:
    a = (sxz * syy - syz * sxy) / det; b = (sxx * syz - sxy * sxz) / det
    print('fit: %.2f us per stage bind reused, %.2f us per set made' % (a, b))
bk = collections.defaultdict(list)
for us, made, binds, g in big:
    bk[min(made // 500, 12)].append((us, made, binds, g))
print('sets made   subs   mean step us   mean binds   new generations')
for k in sorted(bk):
    L = bk[k]
    print('%5d+     %5d   %8.0f       %7.0f      %d' % (k * 500, len(L), sum(r[0] for r in L) / len(L), sum(r[2] for r in L) / len(L), sum(r[3] for r in L)))
tot = sum(r[0] for r in rows)
print('total renderer submission time %.0f ms; in big submissions %.0f ms; made per big submission mean %.0f' % (tot / 1000, sum(r[0] for r in big) / 1000, sum(r[1] for r in big) / max(1, len(big))))
