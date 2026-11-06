#!/usr/bin/env python3
"""Aggregate `perf script -F ip,dso` call chains for kyty_emulator. Usage: chains.py <map> <load_base_hex>"""
import bisect, collections, re, sys
syms = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    p = line.split(None, 4)
    if len(p) == 5 and p[3] == '1' and re.fullmatch(r'[0-9a-f]{5,}', p[0]):
        syms.append((int(p[0], 16), int(p[2], 16), p[4].strip()))
syms.sort(); A = [s[0] for s in syms]; base = int(sys.argv[2], 16)
def nm(ip, dso):
    if 'kyty_emulator' in dso:
        a = int(ip, 16) - base; i = bisect.bisect_right(A, a) - 1
        if i >= 0 and a < syms[i][0] + syms[i][1]:
            return re.sub(r'\(.*', '', syms[i][2]).replace('Libs::Graphics::', '')[:60]
    return dso.split('/')[-1][:24]
chains = collections.Counter(); cur = []
for l in sys.stdin:
    s = l.strip()
    if not s:
        if cur: chains[' <- '.join(cur[:int(sys.argv[3]) if len(sys.argv) > 3 else 4])] += 1
        cur = []; continue
    p = s.split()
    if len(p) >= 2 and re.fullmatch(r'[0-9a-f]+', p[0]):
        f = nm(p[0], p[-1].strip('()'))
        if not cur or cur[-1] != f: cur.append(f)
if cur: chains[' <- '.join(cur[:4])] += 1
tot = sum(chains.values())
for k, v in chains.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 14):
    print(f'{v/tot*100:5.1f}%  {k}')
