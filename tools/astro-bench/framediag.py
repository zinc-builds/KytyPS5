#!/usr/bin/env python3
"""Summarize KYTY_FRAME_DIAG=1 lines (frameDiag.h) of a garden.sh log.

framediag.py <bench log> [A-B ...]   windows in level frames (as garden.sh's); default 1000-1600 and
                                      the whole level

GFRAME lines (guest frames: between two guest flips decoded on the command thread) are the unit:
work counted there belongs to that frame. FRAME lines (presented flips) give the refreshes the
viewer saw; counts between presented flips scale with the interval, so they explain nothing.
Per window:
  - presented frames in refreshes (60 Hz), and guest frames by command-thread wall time and by
    busy time (ms bins): histograms. A guest frame over 16.7 ms is not a dropped frame by itself:
    the display queue absorbs single ones (framemodel.py replays it);
  - per-frame means of every field for the fastest half, the slowest 10% and the slowest 1% of
    frames (by interval), and their ratio to the fastest half;
  - the worst 20 frames, each with the fields that stand out most against the median frame and
    the previous frame's interval (a slow frame after a slow frame: spill-over).
Headroom (how much faster frames must get for p99 = 1 refresh) is framemodel.py's replay.
"""
import math
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from metrics import level_base  # noqa: E402

REFRESH_MS = 1000.0 / 60.0
KV = re.compile(r"(\S+?)=(\S+)")
# Fields that can explain a slow frame, with a short label.
CAUSES = [
    ("draws", "draws"), ("disp", "dispatches"), ("cmd_cpu_ns", "cmd CPU"),
    ("cmd_gpu_wait_ns", "cmd GPU wait"), ("cmd_idle_ns", "cmd no guest work"), ("cmd_blocked_ns", "cmd only blocked work"),
    ("other_ns", "cmd other sleeps"), ("prep_wait_ns", "prepare wait"), ("rec_wait_ns", "recorder wait"),
    ("mutex_wait_ns", "mutex wait"), ("cmd_runq_ns", "run-queue delay"), ("cmd_ics", "preemptions"),
    ("cmd_vcs", "voluntary switches"), ("gc_ns", "GC"), ("collect_ns", "write collection"),
    ("submit_scan_ns", "submit scans"), ("shader_ns", "shader compile"), ("pipeline_ns", "pipeline compile"),
    ("tex_up_b", "texture upload"), ("buf_up_b", "buffer upload"), ("tex_dn_b", "texture download"),
    ("buf_dn_b", "buffer download"), ("tex_new", "textures created"), ("buf_new", "buffers created"),
    ("tex_evict", "texture evictions"), ("buf_evict", "buffer evictions"), ("meta_rb_ns", "metadata readback"),
    ("wt_faults", "write-tracking faults"), ("cmd_majflt", "cmd major faults"), ("cmd_minflt", "cmd minor faults"),
    ("proc_majflt", "process major faults"), ("rejected", "rejected prepares"), ("drains", "queue drains"),
    ("file_rd_b", "file reads"), ("late_vbl", "late vblanks"), ("submits", "submits"),
]


def parse(log, prefix):
    frames = []
    with open(log, errors="replace") as f:
        for line in f:
            if line.startswith(prefix + " "):
                r = {}
                for k, v in KV.findall(line):
                    try:
                        r[k] = float(v)
                    except ValueError:
                        pass
                frames.append(r)
    return frames


def demand(r):
    """The command thread's busy time in the frame, ms: CPU, run-queue delay, GPU and mutex waits
    (disjoint; the prepare/recorder waits are partly spins inside the CPU time, so not added)."""
    return (r.get("cmd_cpu_ns", 0) + r.get("cmd_gpu_wait_ns", 0) + r.get("mutex_wait_ns", 0) +
            r.get("cmd_runq_ns", 0)) / 1e6


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def histogram(values, label):
    n = len(values)
    hist = {}
    for v in values:
        hist[min(v, 5)] = hist.get(min(v, 5), 0) + 1
    print(label + "  ".join(f"{k if k < 5 else '5+'}: {hist.get(k, 0)} ({100 * hist.get(k, 0) / n:.1f}%)"
                            for k in range(1, 6)))


BINS = (16.7, 18.0, 19.0, 22.0, 33.3)


def bins(label, values):
    n = len(values)
    edges = (0.0,) + BINS + (math.inf,)
    parts = []
    for lo, hi in zip(edges, edges[1:]):
        count = sum(1 for v in values if lo <= v < hi)
        name = f"<{hi}" if lo == 0 else (f">={lo}" if hi == math.inf else f"{lo}-{hi}")
        parts.append(f"{name}: {count} ({100 * count / n:.1f}%)")
    print(label + "  ".join(parts))


def report(frames, presented, label):
    n = len(frames)
    if n < 20:
        print(f"{label}: {n} frames, too few")
        return
    for r in frames:
        # The wall time not measured as busy or waiting for guest work: other sleeps.
        r["other_ns"] = max(0.0, r["ms"] * 1e6 - demand(r) * 1e6 - r.get("cmd_idle_ns", 0) -
                            r.get("cmd_blocked_ns", 0))
    ms = [r["ms"] for r in frames]
    print(f"\n=== {label}: {n} guest frames, mean {st.mean(ms):.2f} ms ({1000 / st.mean(ms):.1f} FPS), "
          f"p50 {pct(ms, 50):.1f}, p90 {pct(ms, 90):.1f}, p99 {pct(ms, 99):.1f}, max {max(ms):.1f} ms")
    if presented:
        histogram([int(r["vbl"]) for r in presented], f"presented ({len(presented)}), refreshes: ")
    bins("guest frames, wall ms: ", ms)
    bins("guest frames, cmd busy ms: ", [demand(r) for r in frames])

    order = sorted(range(n), key=lambda i: ms[i])
    fast = [frames[i] for i in order[: n // 2]]
    slow10 = [frames[i] for i in order[-max(1, n // 10):]]
    slow1 = [frames[i] for i in order[-max(1, n // 100):]]
    print(f"\n{'field (per frame)':24s} {'fast half':>12s} {'slow 10%':>12s} {'x':>6s} {'slow 1%':>12s} {'x':>6s}")
    rows = [("ms", "interval ms"), ("demand", "cmd busy ms")] + CAUSES
    for key, name in rows:
        get = (lambda r: demand(r)) if key == "demand" else (lambda r, k=key: r.get(k, 0))
        a, b, c = (st.mean(get(r) for r in g) for g in (fast, slow10, slow1))
        if a == 0 and b == 0 and c == 0:
            continue
        ra = f"{b / a:6.2f}" if a else "   new"
        rc = f"{c / a:6.2f}" if a else "   new"
        print(f"{name:24s} {a:12.1f} {b:12.1f} {ra} {c:12.1f} {rc}")

    med = {key: st.median(r.get(key, 0) for r in frames) for key, _ in CAUSES}
    spread = {key: max(1.0, st.median(abs(r.get(key, 0) - med[key]) for r in frames)) for key, _ in CAUSES}
    print("\nworst 20 guest frames (level frame, wall ms, cmd busy ms, previous frame's ms; top fields vs the median frame):")
    for i in reversed(order[-20:]):
        r = frames[i]
        prev = frames[i - 1]["ms"] if i > 0 else 0
        scores = []
        for key, name in CAUSES:
            excess = r.get(key, 0) - med[key]
            if excess > 0:
                scores.append((excess / spread[key], name, r.get(key, 0), med[key]))
        scores.sort(reverse=True)
        top = "; ".join(f"{name} {v:.0f} (med {m:.0f})" for _, name, v, m in scores[:4])
        print(f"  f={int(r['f_rel']):5d} {r['ms']:6.1f} ms, busy {demand(r):5.1f}, prev {prev:5.1f} | {top}")


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    log = sys.argv[1]
    windows = sys.argv[2:] or ["1000-1600", "0-999999"]
    _, base = level_base(log)
    if base is None:
        base = 0
    frames = parse(log, "GFRAME")
    presented = parse(log, "FRAME")
    for r in frames + presented:
        r["f_rel"] = r.get("f", 0) - base
    for w in windows:
        a, b = (int(x) for x in w.split("-"))
        sel = [r for r in frames if a <= r["f_rel"] < b]
        pres = [r for r in presented if a <= r["f_rel"] < b]
        report(sel, pres, f"level frames {w}" if b < 999999 else "whole level")
    return 0


if __name__ == "__main__":
    sys.exit(main())
