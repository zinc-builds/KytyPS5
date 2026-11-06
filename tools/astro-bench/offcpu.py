#!/usr/bin/env python3
"""Off-CPU time per blocking stack for one thread, from `perf script` of sched_switch events.
Usage: perf script -i X -F comm,tid,time,event,trace,ip,sym,dso | offcpu.py <thread-comm> <lld.map>"""
import bisect, collections, re, sys
comm, mapf = sys.argv[1], sys.argv[2]
syms = []
for line in open(mapf, encoding='utf-8', errors='replace'):
    p = line.split(None, 4)
    if len(p) == 5 and p[3] == '1' and re.fullmatch(r'[0-9a-f]{5,}', p[0]):
        syms.append((int(p[0], 16), int(p[2], 16), p[4].strip()))
syms.sort(); addrs = [s[0] for s in syms]
def name(ip, sym, dso):
    if 'kyty_emulator' in dso and (sym in ('[unknown]', '') or sym.startswith('0x')):
        # perf reports file offsets for this PIE build when symbols are missing.
        a = int(ip, 16) + 0x1000
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and a < syms[i][0] + syms[i][1]:
            return re.sub(r'\(.*', '', syms[i][2])[:70]
    return f"{sym}[{dso.split('/')[-1]}]"[:70]
events, cur = [], None
for line in sys.stdin:
    if not line.startswith((' ', '\t')) and line.strip():
        m = re.match(r'\s*(\S.*?)\s+(\d+)\s+(\d+\.\d+):\s+\S*?(sched:\w+):\s*(.*)', line)
        cur = None
        if m:
            cur = {'comm': m.group(1), 'tid': int(m.group(2)), 't': float(m.group(3)),
                   'ev': m.group(4), 'trace': m.group(5), 'stack': []}
            events.append(cur)
    elif cur is not None and line.strip():
        parts = line.split()
        if len(parts) >= 2:
            cur['stack'].append(name(parts[0], parts[1], parts[-1].strip('()')))
off = collections.Counter(); total = 0.0; tids = set(); out = {}
for e in events:
    if e['ev'] != 'sched:sched_switch':
        continue
    prev = re.search(r'prev_comm=(.*?) prev_pid=(\d+).*?prev_state=(\S+).*next_comm=(.*?) next_pid=(\d+)', e['trace'])
    if not prev:
        continue
    pc, ppid, state, nc, npid = prev.groups()
    if pc == comm:
        tids.add(int(ppid))
        frames = [f for f in e['stack'] if not f.endswith('[kernel.kallsyms]]') and 'kallsyms' not in f]
        key = (state, ' <- '.join(frames[:5]))
        out[int(ppid)] = (e['t'], key)
    if nc == comm and int(npid) in out:
        t0, key = out.pop(int(npid))
        off[key] += e['t'] - t0; total += e['t'] - t0
span = events[-1]['t'] - events[0]['t'] if events else 0
print(f"{comm}: tids={sorted(tids)} off-CPU {total:.2f}s of {span:.2f}s window")
for (state, stack), s in off.most_common(14):
    print(f"{s:7.3f}s {100*s/max(span,1e-9):5.1f}%  [{state}] {stack}")
