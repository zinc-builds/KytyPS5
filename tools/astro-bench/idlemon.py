#!/usr/bin/env python3
"""idlemon.py <cpu list> <out.json>: busy % of the CPUs the core layout leaves idle, until SIGTERM.

garden.sh runs it over a run's measured window (rule 7 of the bench procedure, Track R's signal):
foreign load shows up on the CPUs our layout keeps free (the SMT siblings of the command thread's
and the prepare workers' cores), ~5.5% busy on a quiet Sky Aztec run against 60-63% under a
4.8-CPU outside load; PSI cannot tell the game's own oversubscribed E-cores from outside load.
Samples /proc/stat every 100 ms. Writes {"cpus", "samples", "mean", "max5s"} (percent busy:
1 - (idle + iowait) / total, averaged over the CPUs; max5s is the worst 5 s stretch).
"""
import json
import signal
import sys
import time


def read(cpus):
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu") or line.startswith("cpu "):
                continue
            parts = line.split()
            n = int(parts[0][3:])
            if n in cpus:
                v = [int(x) for x in parts[1:]]
                out[n] = (v[3] + v[4], sum(v[:8]))  # idle + iowait, total (no guest columns)
    return out


def main():
    cpus = {int(c) for c in sys.argv[1].split(",") if c}
    path = sys.argv[2]
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    busy = []  # one value per 100 ms sample: mean busy over the CPUs
    prev = read(cpus)
    while not stop:
        time.sleep(0.1)
        cur = read(cpus)
        vals = []
        for c in cpus:
            if c in cur and c in prev:
                idle, total = cur[c][0] - prev[c][0], cur[c][1] - prev[c][1]
                if total > 0:
                    vals.append(100.0 * (1.0 - idle / total))
        if vals:
            busy.append(sum(vals) / len(vals))
        prev = cur
    win = 50  # 5 s of 100 ms samples
    max5 = max((sum(busy[i:i + win]) / win for i in range(0, max(len(busy) - win + 1, 0))), default=None)
    if max5 is None and busy:
        max5 = sum(busy) / len(busy)
    with open(path, "w") as f:
        json.dump({"cpus": sorted(cpus), "samples": len(busy),
                   "mean": round(sum(busy) / len(busy), 2) if busy else None,
                   "max5s": round(max5, 2) if max5 is not None else None}, f)


if __name__ == "__main__":
    main()
