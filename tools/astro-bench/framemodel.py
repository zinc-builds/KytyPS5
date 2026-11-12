#!/usr/bin/env python3
"""Frame-pacing replay model over KYTY_FRAME_DIAG=1 GFRAME lines (frameDiag.h).

framemodel.py <bench log> [A-B]     level frames (as garden.sh's), default 1000-1600

Each guest frame's command-thread wall time (between two guest flips decoded on the command
thread) is replayed against 60 Hz vblanks: frame k is ready when its work ends, shown at the first
vblank after that and after frame k-1's, and the guest runs at most DEPTH frames ahead of the
display. The share of presented frames that take two or more refreshes is what the viewer sees;
on Sky Garden the replay of the measured walls matched the measured share within ~0.5 points
(FINDINGS, "Frame pacing").

A frame's wall time is compared with its measured parts:
  - busy: command-thread CPU time, run-queue delay, and its blocking waits on the GPU timeline and
    the render-context mutex;
  - no guest work: waiting with nothing queued (cmd_idle_ns), and with only blocked submissions
    queued (cmd_blocked_ns);
  - the remainder, other sleeps (prepare-worker, recorder and scheduler waits that slept, and any
    wait not instrumented). The waits are elapsed times, so they can include wake-up delay that
    the run-queue figure also counts: the parts can overlap, and the remainder is printed signed
    (a negative mean is overlap, not time).
The prepare/recorder waits (prep_wait_ns, rec_wait_ns) are shown separately: they are partly spins
inside the CPU time and partly sleeps inside the remainder.

What-ifs edit the per-frame walls and replay them; they assume the rest of the frame stays as it
was, so they are estimates, not measurements:
  - a cause removed: the slow frames of one class set to the window's median wall, or the measured
    no-guest-work time removed from every frame;
  - a uniform cut of the command thread's CPU per frame, bounded by each frame's CPU time;
every edited frame stays between zero and its measured wall time.
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


def busy(r):
    """Command-thread CPU, run-queue delay and its GPU and mutex waits, ms (the waits are elapsed times and can overlap the run-queue delay)."""
    return (r["cmd_cpu_ns"] + r.get("cmd_runq_ns", 0) + r.get("cmd_gpu_wait_ns", 0) +
            r.get("mutex_wait_ns", 0)) / 1e6


def idle(r):
    """Measured waits for guest work, ms: nothing queued, or only blocked submissions."""
    return (r["cmd_idle_ns"] + r.get("cmd_blocked_ns", 0)) / 1e6


def other(r):
    """The wall time not measured as busy or idle, ms: other sleeps, or overlap if negative."""
    return r["ms"] - busy(r) - idle(r)


def waits(r):
    """Prepare and recorder waits, ms: partly spins inside busy(), partly sleeps in other()."""
    return (r["prep_wait_ns"] + r["rec_wait_ns"]) / 1e6


def cls(r):
    """The likeliest cause of a slow guest frame (wall > 19 ms), first match."""
    if r.get("shader_ns", 0) + r.get("pipeline_ns", 0) > 1e6:
        return "shader/pipeline compile"
    if r.get("cmd_gpu_wait_ns", 0) > 1e6:
        return "command thread waited on the GPU >1 ms"
    if idle(r) > 2.0:
        return "command thread had no guest work >2 ms"
    if r["prep_wait_ns"] > 3e6:
        return "prepare wait >3 ms"
    if r["rec_wait_ns"] > 2e6:
        return "recorder wait >2 ms"
    if r.get("cmd_runq_ns", 0) > 1e6 or r.get("cmd_majflt", 0) > 0:
        return "preempted / major fault"
    if other(r) > 2.0:
        return "other sleeps >2 ms (unattributed)"
    if r.get("cmd_minflt", 0) > 100:
        return "command-thread page faults"
    return "more command-thread CPU (no single cause)"


def replay(walls, depth=DEPTH):
    """Share of presented frames shown for two or more refreshes."""
    if any(x < 0 for x in walls):
        raise ValueError("negative frame time")
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
    if any("cmd_idle_ns" not in r for r in frames):
        print("the log has no cmd_idle_ns (an older KYTY_FRAME_DIAG build)", file=sys.stderr)
        return 1
    wall = [r["ms"] for r in frames]
    n = len(frames)
    m = lambda f: st.mean(f(r) for r in frames)  # noqa: E731
    print(f"level frames {a}-{b}: {n} guest frames; wall mean {m(lambda r: r['ms']):.2f} ms = command thread busy "
          f"{m(busy):.2f} + no guest work {m(idle):.2f} + remainder {m(other):+.2f} (other sleeps; negative = overlap)")
    print(f"  busy = CPU {m(lambda r: r['cmd_cpu_ns']) / 1e6:.2f} + run queue {m(lambda r: r.get('cmd_runq_ns', 0)) / 1e6:.2f}"
          f" + GPU waits {m(lambda r: r.get('cmd_gpu_wait_ns', 0)) / 1e6:.2f} + mutex waits {m(lambda r: r.get('mutex_wait_ns', 0)) / 1e6:.2f};"
          f" waiting for the prepare workers/recorder (spin or sleep, overlaps) {m(waits):.2f}")
    print(f"  frames with no guest work >2 ms: {100 * sum(1 for r in frames if idle(r) > 2) / n:.1f}%")

    slow = [r for r in frames if r["ms"] > 19.0]
    print(f"\nguest frames over 19 ms: {len(slow)} ({100 * len(slow) / n:.1f}%), by first matching cause")
    for name, count in Counter(cls(r) for r in slow).most_common():
        sel = [r for r in slow if cls(r) == name]
        print(f"  {100 * count / n:5.1f}%  {name}: wall {st.mean(r['ms'] for r in sel):.1f} ms, busy "
              f"{st.mean(busy(r) for r in sel):.1f}, no guest work {st.mean(idle(r) for r in sel):.1f}, "
              f"remainder {st.mean(other(r) for r in sel):+.1f}, prepare wait {st.mean(r['prep_wait_ns'] for r in sel) / 1e6:.2f}")

    med = st.median(wall)
    def cut(r, c, no_idle=False):
        """The frame without c ms of its CPU (and its waits for guest work), never below 0."""
        less = min(c, r["cmd_cpu_ns"] / 1e6)
        if no_idle:
            less += min(idle(r), r["ms"] - less)
        return r["ms"] - less
    rows = [
        ("as measured", wall),
        ("no periodic prepare-wait spike", [med if cls(r) == "prepare wait >3 ms" and r["ms"] > 19 else r["ms"] for r in frames]),
        ("no shader/pipeline compiles", [med if cls(r) == "shader/pipeline compile" and r["ms"] > 19 else r["ms"] for r in frames]),
        ("no waiting for guest work", [cut(r, 0.0, True) for r in frames]),
    ]
    rows += [(f"CPU -{c} ms per frame", [cut(r, c) for r in frames]) for c in (0.5, 1.0, 1.5, 2.0)]
    rows += [(f"CPU -{c} ms, no waiting for guest work", [cut(r, c, True) for r in frames]) for c in (0.5, 1.0)]
    print(f"\nreplay (depth {DEPTH}): presented frames taking 2+ refreshes; what-ifs are estimates")
    for name, walls in rows:
        print(f"  {name:40s} mean {st.mean(walls):5.2f} ms -> {100 * replay(walls):5.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
