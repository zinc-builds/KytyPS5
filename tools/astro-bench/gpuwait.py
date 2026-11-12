#!/usr/bin/env python3
"""Frame timeline from a KYTY_GPU_WAIT_DIAG=trace file (gpuWaitDiag.h).

Usage: gpuwait.py <trace> [--from-flip N] [--flips K] [--show F]
Per guest flip interval (decode of flip i to decode of flip i+1): wall ms, GPU pending/idle ms
(pending = some submitted tick not yet complete), command-thread GPU-wait ms by site, submits,
draws, the flip's own completion latency (flip decode -> its tick complete), and the backlog
the GPU still had at the flip. --show F prints the event timeline of F frames.
"""
import argparse
import collections
import statistics
import sys


def load(path):
    sites, ev = {}, []
    for line in open(path, encoding='utf-8', errors='replace'):
        p = line.split()
        if not p:
            continue
        k = p[0]
        if k == 'L':
            sites[int(p[1])] = p[2] + ' ' + (p[3] if len(p) > 3 else '')
        elif k == 'S':
            ev.append((int(p[2]), 'S', int(p[1]), int(p[4]), int(p[5]), int(p[3])))
        elif k == 'C':
            ev.append((int(p[2]), 'C', int(p[1])))
        elif k == 'W':
            ev.append((int(p[2]), 'W', int(p[1]), int(p[3]), int(p[4]), int(p[5]), int(p[6])))
        elif k == 'F':
            ev.append((int(p[2]), 'F', int(p[1])))
        elif k == 'D':
            ev.append((int(p[2]), 'D', int(p[1])))
        elif k == 'G':
            ev.append((int(p[2]), 'G', int(p[1])))
    ev.sort(key=lambda e: e[0])
    return sites, ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--from-flip', type=int, default=-1, help='first flip (default: skip 40%%)')
    ap.add_argument('--flips', type=int, default=300)
    ap.add_argument('--show', type=int, default=0)
    a = ap.parse_args()
    sites, ev = load(a.trace)
    flips = [e for e in ev if e[1] == 'F']
    if len(flips) < 3:
        sys.exit('too few flips')
    first = a.from_flip if a.from_flip >= 0 else int(len(flips) * 0.4)
    last = min(len(flips) - 1, first + a.flips)
    t0, t1 = flips[first][0], flips[last][0]

    submit_t, done_t = {}, {}
    for e in ev:
        if e[1] == 'S':
            submit_t[e[2]] = e[0]
        elif e[1] == 'C':
            done_t[e[2]] = e[0]
    # completion time for every tick (C events cover ranges)
    done_sorted = sorted(done_t.items())
    tick_done = {}
    prev = 0
    for tick, t in done_sorted:
        for x in range(prev + 1, tick + 1):
            tick_done[x] = t
        prev = max(prev, tick)

    # GPU pending intervals: walk events; pending = last_submitted > last_done
    frames = []
    fi = first
    cur = None
    last_sub, last_done = 0, 0
    pend_since = None
    for e in ev:
        t = e[0]
        if t < t0:
            if e[1] == 'S':
                last_sub = max(last_sub, e[2])
            elif e[1] == 'C':
                last_done = max(last_done, e[2])
            continue
        if t > t1:
            break
        if e[1] == 'F':
            if cur is not None:
                if pend_since is not None:
                    cur['pending'] += t - pend_since
                    pend_since = t
                cur['end'] = t
                frames.append(cur)
            cur = collections.defaultdict(float)
            cur['start'] = t
            cur['flip_tick'] = e[2]
            cur['backlog_at_flip'] = last_sub - last_done
            ft = e[2]
            cur['flip_latency'] = (tick_done.get(ft, t) - t) if ft in tick_done else float('nan')
            cur['waits'] = collections.defaultdict(float)
            cur['nwaits'] = collections.defaultdict(int)
            if last_sub > last_done:
                pend_since = t
            continue
        if cur is None:
            continue
        if e[1] == 'S':
            cur['submits'] += 1
            if last_sub <= last_done:  # GPU was idle; starts pending now
                pend_since = t
            last_sub = max(last_sub, e[2])
        elif e[1] == 'C':
            last_done = max(last_done, e[2])
            if last_done >= last_sub and pend_since is not None:
                cur['pending'] += t - pend_since
                pend_since = None
        elif e[1] == 'W':
            site = e[4]
            cur['waits'][site] += e[3] - e[0]
            cur['nwaits'][site] += 1
    n = len(frames)
    if n == 0:
        sys.exit('no frames')
    wall = [(f['end'] - f['start']) / 1e6 for f in frames]
    pend = [f['pending'] / 1e6 for f in frames]
    print(f'flips {first}..{last} ({n} frames), wall {statistics.mean(wall):.2f} ms/frame '
          f'(p50 {statistics.median(wall):.2f}), {1000 / statistics.mean(wall):.1f} FPS')
    print(f'GPU pending {statistics.mean(pend):.2f} ms/frame, idle {statistics.mean(wall) - statistics.mean(pend):.2f}')
    subs = [f['submits'] for f in frames]
    print(f'submits/frame {statistics.mean(subs):.1f}')
    lat = [f['flip_latency'] / 1e6 for f in frames if f['flip_latency'] == f['flip_latency']]
    if lat:
        print(f'flip tick complete after flip decode: mean {statistics.mean(lat):.2f} ms, '
              f'p50 {statistics.median(lat):.2f}, max {max(lat):.2f}')
    bl = [f['backlog_at_flip'] for f in frames]
    print(f'GPU backlog (unfinished submits) at flip decode: mean {statistics.mean(bl):.1f}')
    tot = collections.defaultdict(float)
    cnt = collections.defaultdict(int)
    for f in frames:
        for s, v in f['waits'].items():
            tot[s] += v
            cnt[s] += f['nwaits'][s]
    print('command-thread GPU waits per frame:')
    for s, v in sorted(tot.items(), key=lambda x: -x[1]):
        print(f'  {v / n / 1e6:7.3f} ms  {cnt[s] / n:5.2f} waits  avg {v / cnt[s] / 1e3:7.1f} us  {sites.get(s, s)}')
    if a.show:
        print('\ntimeline (ms from first shown flip):')
        s0 = frames[0]['start']
        s1 = frames[min(a.show, n) - 1]['end']
        for e in ev:
            if e[0] < s0 or e[0] > s1:
                continue
            t = (e[0] - s0) / 1e6
            if e[1] == 'S':
                print(f'{t:9.3f} SUBMIT  tick {e[2]} draws {e[3]} idle_gpu={e[4]} call_us={e[5] / 1e3:.0f}')
            elif e[1] == 'C':
                print(f'{t:9.3f}   done  tick {e[2]}')
            elif e[1] == 'W':
                print(f'{t:9.3f} WAIT    tick {e[2]} for {(e[3] - e[0]) / 1e3:.0f} us  known {e[5]} cur {e[6]}  {sites.get(e[4], e[4])}')
            elif e[1] == 'F':
                print(f'{t:9.3f} FLIP    tick {e[2]}')
            elif e[1] == 'D':
                print(f'{t:9.3f} flipdone tick {e[2]}')
            elif e[1] == 'G':
                print(f'{t:9.3f} guest submission q{e[2]}')


if __name__ == '__main__':
    main()
