#!/usr/bin/env python3
"""Summarize KYTY_FRAME_DIAG=1 FRAME lines (frameDiag.h) of a garden.sh log.

framediag.py <bench log> [A-B ...]   windows in level frames (as garden.sh's); default 1000-1600 and
                                      the whole level

Per window:
  - frame times in refreshes (60 Hz): histogram;
  - per-frame means of every field for the fastest half, the slowest 10% and the slowest 1% of
    frames (by interval), and their ratio to the fastest half;
  - the worst 20 frames, each with the fields that stand out most against the median frame and
    the previous frame's interval (a slow frame after a slow frame: spill-over);
  - a headroom model: each frame's command-thread demand (CPU + waits) relative to the window's
    mean; the mean demand at which 99% of frames would fit one refresh.
"""
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
    ("gpu_wait_ns", "GPU wait"), ("prep_wait_ns", "prepare wait"), ("rec_wait_ns", "recorder wait"),
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


def parse(log):
    frames = []
    with open(log, errors="replace") as f:
        for line in f:
            if line.startswith("FRAME "):
                r = {}
                for k, v in KV.findall(line):
                    try:
                        r[k] = float(v)
                    except ValueError:
                        pass
                frames.append(r)
    return frames


def demand(r):
    """The command thread's accounted time in the frame, ms: CPU plus its waits."""
    return (r.get("cmd_cpu_ns", 0) + r.get("gpu_wait_ns", 0) + r.get("prep_wait_ns", 0) +
            r.get("rec_wait_ns", 0) + r.get("mutex_wait_ns", 0) + r.get("cmd_runq_ns", 0)) / 1e6


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def report(frames, label):
    n = len(frames)
    if n < 20:
        print(f"{label}: {n} frames, too few")
        return
    ms = [r["ms"] for r in frames]
    vbl = [int(r["vbl"]) for r in frames]
    print(f"\n=== {label}: {n} frames, mean {st.mean(ms):.2f} ms ({1000 / st.mean(ms):.1f} FPS), "
          f"p50 {pct(ms, 50):.1f}, p90 {pct(ms, 90):.1f}, p99 {pct(ms, 99):.1f}, max {max(ms):.1f} ms")
    hist = {}
    for v in vbl:
        hist[min(v, 5)] = hist.get(min(v, 5), 0) + 1
    print("refreshes: " + "  ".join(f"{k if k < 5 else '5+'}: {hist.get(k, 0)} ({100 * hist.get(k, 0) / n:.1f}%)"
                                    for k in range(1, 6)))

    order = sorted(range(n), key=lambda i: ms[i])
    fast = [frames[i] for i in order[: n // 2]]
    slow10 = [frames[i] for i in order[-max(1, n // 10):]]
    slow1 = [frames[i] for i in order[-max(1, n // 100):]]
    print(f"\n{'field (per frame)':24s} {'fast half':>12s} {'slow 10%':>12s} {'x':>6s} {'slow 1%':>12s} {'x':>6s}")
    rows = [("ms", "interval ms"), ("demand", "cmd demand ms")] + CAUSES
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
    print("\nworst 20 frames (level frame, ms, refreshes, previous frame's ms; top fields vs the median frame):")
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
        print(f"  f={int(r['f_rel']):5d} {r['ms']:6.1f} ms {int(r['vbl'])} vbl, prev {prev:5.1f} | {top}")

    # Headroom: a frame's demand relative to the mean demand; at mean demand X ms, the frames with
    # ratio * X > one refresh miss it.
    d = [demand(r) for r in frames]
    mean_d = st.mean(d)
    ratios = sorted(x / mean_d for x in d)
    q99 = ratios[int(len(ratios) * 0.99)]
    q90 = ratios[int(len(ratios) * 0.90)]
    miss_at_60 = sum(1 for x in ratios if x * REFRESH_MS > REFRESH_MS) / len(ratios)
    print(f"\nheadroom: command-thread demand mean {mean_d:.2f} ms (CPU+waits); demand/mean p90 {q90:.2f}, "
          f"p99 {q99:.2f}, max {ratios[-1]:.2f}")
    print(f"  mean demand for p99 within one refresh: {REFRESH_MS / q99:.2f} ms "
          f"({1000 / (REFRESH_MS / q99):.0f} FPS-equivalent average); for p90: {REFRESH_MS / q90:.2f} ms")
    print(f"  at a mean of exactly one refresh, {100 * miss_at_60:.1f}% of frames would exceed it")


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    log = sys.argv[1]
    windows = sys.argv[2:] or ["1000-1600", "0-999999"]
    _, base = level_base(log)
    if base is None:
        base = 0
    frames = parse(log)
    for r in frames:
        r["f_rel"] = r.get("f", 0) - base
    for w in windows:
        a, b = (int(x) for x in w.split("-"))
        sel = [r for r in frames if a <= r["f_rel"] < b]
        report(sel, f"level frames {w}" if b < 999999 else "whole level")
    return 0


if __name__ == "__main__":
    sys.exit(main())
