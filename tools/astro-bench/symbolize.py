#!/usr/bin/env python3
"""Map 'kyty_emulator 0xOFFSET' lines from perf report to function names via the lld map.
perf prints file offsets for this PIE; the R E segment's vaddr is offset + 0x1000."""
import bisect, re, sys
mapf, delta = sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1000
syms = []
for line in open(mapf, encoding='utf-8', errors='replace'):
    p = line.split(None, 4)
    if len(p) == 5 and p[3] == '1' and re.fullmatch(r'[0-9a-f]{5,}', p[0]):
        syms.append((int(p[0], 16), int(p[2], 16), p[4].strip()))
syms.sort(); addrs = [s[0] for s in syms]
for line in sys.stdin:
    m = re.search(r'kyty_emulator\s+\[\.\]\s+0x([0-9a-f]+)', line)
    if m:
        a = int(m.group(1), 16) + delta
        i = bisect.bisect_right(addrs, a) - 1
        name = syms[i][2] if i >= 0 and a < syms[i][0] + syms[i][1] else '?'
        line = line[:m.start()] + 'kyty  ' + name[:110] + '\n'
    sys.stdout.write(line)
