#!/usr/bin/env python3
"""hitches.py <label|log> [--frames A-B] [--load-mb N] [--load-before F] [--list]: long frames in a garden.sh run
made with KYTY_FRAME_DIAG=1.

Reads the GFRAME lines (one per guest flip, its wall time in ms=) of the level: level frame 0 is
the first PERF frame after the StartLevel line, as in garden.sh's window. Level-load frames, those
before level frame --load-before (default 150) that uploaded at least --load-mb (default 1024) of
textures and buffers (tex_up_b + buf_up_b), are listed apart and not counted; a streaming burst
later in the level counts however large. Prints one line:

  HITCH <label>: frames=<n> over50=<n> over100=<n> worst=<ms> (f=<level frame>) load=<ms@f,...>

and with --list, every counted frame over 50 ms (level frame, ms, and what it spent: command-thread
CPU, GPU waits, no guest work, texture GC, shader and pipeline compiles, texture uploads).
"""
import re
import sys


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith('-'):
        sys.exit(__doc__)
    src = args[0]
    frames = (0, 1 << 62)
    load_mb = 1024.0
    load_before = 150
    show = False
    i = 1
    while i < len(args):
        if args[i] == '--frames':
            a, b = args[i + 1].split('-')
            frames = (int(a), int(b))
            i += 2
        elif args[i] == '--load-mb':
            load_mb = float(args[i + 1])
            i += 2
        elif args[i] == '--load-before':
            load_before = int(args[i + 1])
            i += 2
        elif args[i] == '--list':
            show = True
            i += 1
        else:
            sys.exit(f'hitches.py: unknown option {args[i]}')
    label = src
    log = src if '/' in src or src.endswith('.log') else f'/tmp/bench-{src}.log'
    if log != src:
        label = src
    started = False
    base = None
    rows = []
    with open(log, errors='replace') as f:
        for line in f:
            if not started:
                started = 'StartLevel aerial_garden' in line
                continue
            if base is None and line.startswith('PERF'):
                m = re.search(r'\bframes=(\d+)', line)
                if m:
                    base = int(m.group(1))
            if line.startswith('GFRAME '):
                rows.append(dict(re.findall(r'(\w+)=([^\s]+)', line)))
    if not started or base is None:
        sys.exit(f'hitches.py: {log}: no level start or no PERF frames after it')
    if not rows:
        sys.exit(f'hitches.py: {log}: no GFRAME lines (run with KYTY_FRAME_DIAG=1)')
    counted = []
    loads = []
    for d in rows:
        lf = int(d['f']) - base
        if lf < frames[0] or lf > frames[1]:
            continue
        ms = float(d['ms'])
        up = (float(d.get('tex_up_b', 0)) + float(d.get('buf_up_b', 0))) / 1048576.0
        if up >= load_mb and lf < load_before:
            loads.append((lf, ms))
        else:
            counted.append((lf, ms, d))
    if not counted:
        sys.exit(f'hitches.py: {log}: no frames in level frames {frames[0]}-{frames[1]}')
    over50 = [c for c in counted if c[1] > 50.0]
    over100 = [c for c in counted if c[1] > 100.0]
    worst = max(counted, key=lambda c: c[1])
    load = ','.join(f'{ms:.0f}@{lf}' for lf, ms in loads) or 'none'
    print(f'HITCH {label}: frames={len(counted)} over50={len(over50)} over100={len(over100)} '
          f'worst={worst[1]:.1f} (f={worst[0]}) load={load}')
    if show:
        ms = lambda d, k: float(d.get(k, 0)) / 1e6
        for lf, wall, d in over50:
            print(f'  f={lf} ms={wall:.1f} cpu={ms(d, "cmd_cpu_ns"):.1f} '
                  f'gpu_wait={ms(d, "cmd_gpu_wait_ns"):.1f} '
                  f'no_guest_work={ms(d, "cmd_idle_ns") + ms(d, "cmd_blocked_ns"):.1f} '
                  f'gc={ms(d, "gc_ns"):.1f} compile={ms(d, "shader_ns") + ms(d, "pipeline_ns"):.1f} '
                  f'tex_up={float(d.get("tex_up_b", 0)) / 1048576:.0f}MB '
                  f'tex_dn={float(d.get("tex_dn_b", 0)) / 1048576:.0f}MB')


if __name__ == '__main__':
    main()
