#!/usr/bin/env python3
"""Game-speed check for Sky Garden bench logs (garden.sh writes /tmp/bench-<label>.log).

Astro Bot advances its simulation by a fixed step per flip, so game speed = flips per second / 60.
The check uses an event the game logs itself: `SubLevelLocator sublevel_locator_veg_chibi ...
state changed 6->7` fires ~850 flips after `StartLevel aerial_garden` in every run (15-60 FPS),
i.e. ~14.2 s on a PS5. Per log it prints:
  event:  flips and seconds from StartLevel to the event, and the game speed over that stretch
          (includes the level-start load hitches);
  window: flips/s in level frames 1000-1600 (the bench plateau) / 60;
  FRAMESKIP summary when the run had KYTY_FRAME_SKIP_LOG=1.
Event times are interpolated from the 1 s PERF lines (about +-0.5 s).
Usage: gamespeed.py [bench logs...]   (default: /tmp/bench-*.log)
"""
import glob
import re
import sys

PERF = re.compile(r'^PERF fps=([\d.]+).*?frames=(\d+) t=([\d.]+)')
SKIP = re.compile(r'^FRAMESKIP (.*)')


def check(path):
    lines = open(path, errors='replace').read().splitlines()
    perf, skip, start, event = [], [], None, None
    for i, l in enumerate(lines):
        m = PERF.search(l)
        if m:
            perf.append((i, int(m[2]), float(m[3])))
            continue
        m = SKIP.search(l)
        if m:
            skip.append((i, dict(kv.split('=') for kv in m[1].split())))
        elif start is None and 'StartLevel aerial_garden' in l:
            start = i
        elif event is None and 'veg_chibi' in l and '6->7' in l:
            event = i
    if start is None or not perf:
        return None

    def at(i):
        before = [p for p in perf if p[0] < i]
        after = [p for p in perf if p[0] > i]
        if not before or not after:
            return None
        return ((before[-1][1] + after[0][1]) / 2, (before[-1][2] + after[0][2]) / 2)

    out = [path]
    s = at(start)
    e = at(event) if event is not None else None
    if s and e and e[1] > s[1]:
        flips, secs = e[0] - s[0], e[1] - s[1]
        out.append('  event: %.0f flips in %.1f s -> game speed %.2fx' % (flips, secs, flips / secs / 60))
    level = [p for p in perf if p[0] > start]
    if level:
        base = level[0][1]
        win = [p for p in level if 1000 <= p[1] - base <= 1600]
        if len(win) >= 2 and win[-1][2] > win[0][2]:
            fps = (win[-1][1] - win[0][1]) / (win[-1][2] - win[0][2])
            out.append('  window 1000-1600: %.1f flips/s -> game speed %.2fx' % (fps, fps / 60))
    rows = [d for i, d in skip if i > start][10:]
    if rows:
        avg = lambda k: sum(float(d.get(k, 0)) for d in rows) / len(rows)
        out.append('  FRAMESKIP (%d s, from 10 s into the level): ' % len(rows) +
                   ' '.join('%s=%.2f' % (k, avg(k)) for k in rows[0]))
    return '\n'.join(out)


paths = sys.argv[1:] or sorted(glob.glob('/tmp/bench-*.log'))
for p in paths:
    r = check(p)
    if r:
        print(r)
