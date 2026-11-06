#!/usr/bin/env python3
"""Summarise FAULTDIAG lines (from after level start) by writer. Usage: faultsum.py <log> <lld.map>"""
import bisect, collections, re, sys
log, mapf = sys.argv[1], sys.argv[2]
lines = open(log, errors='replace').read().splitlines()
start = next((i for i, l in enumerate(lines) if 'StartLevel aerial_garden' in l), 0)
rows = [dict(re.findall(r'(\w+)=(\S+)', l)) for l in lines[start:] if l.startswith('FAULTDIAG')]
syms = []
for line in open(mapf, encoding='utf-8', errors='replace'):
    p = line.split(None, 4)
    if len(p) == 5 and p[3] == '1' and re.fullmatch(r'[0-9a-f]{5,}', p[0]):
        syms.append((int(p[0], 16), int(p[2], 16), p[4].strip()))
syms.sort(); A = [s[0] for s in syms]
def sym(a):
    a = int(a, 16)
    if a == 0: return 'none'
    i = bisect.bisect_right(A, a) - 1
    return re.sub(r'\(.*', '', syms[i][2])[:60] if i >= 0 and a < syms[i][0] + syms[i][1] else hex(a)
by = collections.defaultdict(lambda: [0, 0.0, collections.Counter()])
for r in rows:
    k = (r['gpu_thread'], r['dl'], sym(r['caller']))
    by[k][0] += 1; by[k][1] += float(r['ms']); by[k][2][r['written']] += 1
print(f"{len(rows)} samples (n {rows[0]['n'] if rows else '-'}..{rows[-1]['n'] if rows else '-'})")
for k, v in sorted(by.items(), key=lambda kv: -kv[1][1])[:12]:
    top = ', '.join(f"{w}x{c}" for w, c in v[2].most_common(3))
    print(f"gpu_thr={k[0]} dl={k[1]} n={v[0]:4} total={v[1]:7.1f}ms avg={v[1]/v[0]:5.2f}ms  {k[2]}  [{top}]")
