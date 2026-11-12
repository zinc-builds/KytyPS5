#!/usr/bin/env python3
"""Frame-pacing replay model over KYTY_FRAME_DIAG=1 GFRAME lines (frameDiag.h).

framemodel.py <bench log> [A-B]     level frames (as garden.sh's), default 1000-1600

Each guest frame's command-thread wall time (between two guest flips decoded on the command
thread) is replayed against 60 Hz vblanks: frame k is ready when its work ends, shown at the first
vblank after that and after frame k-1's, and the guest runs at most DEPTH frames ahead of the
display. The share of presented frames that take two or more refreshes is what the viewer sees;
the model reproduced the measured share within ~0.5 points on Sky Garden (FINDINGS, "Frame
pacing"). It then answers what-ifs by editing the per-frame times:
  - removing a cause (the slow frames of one class at the median, or a per-frame component set
    to zero): guest-late idle (wall beyond work and the prepare/recorder waits), the waits;
  - a uniform cut of the command thread's work per frame.
A frame's work = command-thread CPU + its blocking GPU and mutex waits + run-queue delay. The
prepare and recorder waits are partly spins inside that CPU time, so wall = work + waits + idle
is an approximate split; the replay itself uses the measured wall time.
"""
import math
import os
import re
import statistics as st
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from metrics import level_base  # noqa: E402

V = 1000.0 / 60.0
DEPTH = 2
KV = re.compile(r"(\S+?)=(\S+)")
NUM = re.compile(r"[-0-9.e]+")


def load(log, a, b):
    _, base = level_base(log)
    base = base or 0
    frames = []
    with open(log, errors="replace") as f:
        for line in f:
            if line.startswith("GFRAME "):
                r = {k: float(v) for k, v in KV.findall(line) if NUM.fullmatch(v)}
                r["f_rel"] = r["f"] - base
                if a <= r["f_rel"] < b:
                    frames.append(r)
    return frames


def work(r):
    return (r["cmd_cpu_ns"] + r.get("cmd_gpu_wait_ns", 0) + r.get("mutex_wait_ns", 0) +
            r.get("cmd_runq_ns", 0)) / 1e6


def waits(r):
    return (r["prep_wait_ns"] + r["rec_wait_ns"]) / 1e6


def idle(r):
    return max(0.0, r["ms"] - work(r) - waits(r))


def cls(r):
    """The likeliest cause of a slow guest frame (wall > 19 ms)."""
    if r.get("shader_ns", 0) + r.get("pipeline_ns", 0) > 1e6:
        return "shader/pipeline compile"
    if r.get("cmd_gpu_wait_ns", 0) > 1e6:
        return "command thread waited on the GPU >1 ms"
    if idle(r) > 2.0:
        return "command thread idle >2 ms (guest late)"
    if r["prep_wait_ns"] > 3e6:
        return "prepare wait >3 ms"
    if r["rec_wait_ns"] > 2e6:
        return "recorder wait >2 ms"
    if r.get("cmd_runq_ns", 0) > 1e6 or r.get("cmd_majflt", 0) > 0:
        return "preempted / major fault"
    if r.get("cmd_minflt", 0) > 100:
        return "command-thread page faults"
    return "more command-thread CPU (no single cause)"


def replay(walls, depth=DEPTH):
    """Share of presented frames shown for two or more refreshes."""
    t = 0.0
    shown = []
    for k, x in enumerate(walls):
        start = max(t, shown[k - depth]) if k >= depth else t
        t = start + x
        v = math.ceil(t / V - 1e-9) * V
        if shown:
            v = max(v, shown[-1] + V)
        shown.append(v)
    gaps = [round((b - a) / V) for a, b in zip(shown, shown[1:])]
    return sum(1 for g in gaps if g >= 2) / max(1, len(gaps))


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    a, b = (int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1000-1600").split("-"))
    frames = load(sys.argv[1], a, b)
    if len(frames) < 50:
        print(f"{len(frames)} guest frames in {a}-{b}: too few", file=sys.stderr)
        return 1
    wall = [r["ms"] for r in frames]
    w = sorted(work(r) for r in frames)
    n = len(frames)
    print(f"level frames {a}-{b}: {n} guest frames; wall mean {st.mean(wall):.2f} ms = work "
          f"{st.mean(work(r) for r in frames):.2f} + prepare/recorder waits {st.mean(waits(r) for r in frames):.2f}"
          f" + guest-late idle {st.mean(idle(r) for r in frames):.2f}")
    print(f"work p50 {w[n // 2]:.2f}, p90 {w[int(n * .9)]:.2f}, p99 {w[int(n * .99)]:.2f}, max {w[-1]:.2f} ms")

    slow = [r for r in frames if r["ms"] > 19.0]
    print(f"\nguest frames over 19 ms: {len(slow)} ({100 * len(slow) / n:.1f}%)")
    for name, count in Counter(cls(r) for r in slow).most_common():
        sel = [r for r in slow if cls(r) == name]
        print(f"  {100 * count / n:5.1f}%  {name}: wall {st.mean(r['ms'] for r in sel):.1f} ms, work "
              f"{st.mean(work(r) for r in sel):.1f}, prepare wait {st.mean(r['prep_wait_ns'] for r in sel) / 1e6:.2f}")

    med = st.median(wall)
    rows = [
        ("as measured", wall),
        ("no periodic prepare-wait spike", [med if cls(r) == "prepare wait >3 ms" and r["ms"] > 19 else r["ms"] for r in frames]),
        ("no shader/pipeline compiles", [med if cls(r) == "shader/pipeline compile" and r["ms"] > 19 else r["ms"] for r in frames]),
        ("no guest-late idle", [r["ms"] - idle(r) for r in frames]),
        ("no prepare/recorder waits", [r["ms"] - waits(r) for r in frames]),
        ("work only (no idle, no waits)", [work(r) for r in frames]),
    ]
    rows += [(f"work -{c} ms per frame", [r["ms"] - c for r in frames]) for c in (0.5, 1.0, 1.5, 2.0, 2.5)]
    rows += [(f"work -{c} ms, no guest-late idle", [r["ms"] - idle(r) - c for r in frames]) for c in (0.5, 1.0, 1.5, 2.0)]
    print(f"\nreplay (depth {DEPTH}): presented frames taking 2+ refreshes")
    for name, walls in rows:
        print(f"  {name:36s} mean {st.mean(walls):5.2f} ms -> {100 * replay(walls):5.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
