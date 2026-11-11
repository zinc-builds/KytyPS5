#!/usr/bin/env python3
"""kstats.py <label|log> [first-last]: average the KYTY_KERNEL_STATS lines of a garden.sh run over
its frame window (default 1000-1600 game frames after level start, as garden.sh measures) and
print the GPU command thread's kernel time per call site, per second and per draw."""
import re
import sys

arg = sys.argv[1]
path = arg if "/" in arg else f"/tmp/bench-{arg}.log"
f0, f1 = (int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1000-1600").split("-"))
lines = open(path, errors="replace").read().splitlines()
start = next(i for i, l in enumerate(lines) if "StartLevel aerial_garden" in l)
base = None
inside = False
ks, perf = [], []
for line in lines[start + 1:]:
    if line.startswith("PERF"):
        fr = int(re.search(r"frames=(\d+)", line).group(1))
        if base is None:
            base = fr
        # Both boundary rows count, as in garden.sh's window.
        if not inside and fr >= base + f0:
            inside = True
        if inside:
            perf.append(line)
        if inside and fr >= base + f1:
            break
    elif inside and line.startswith("KSTATS"):
        ks.append(line)
if not ks:
    sys.exit("no KSTATS lines in the window (run with KYTY_KERNEL_STATS=1)")

draws = sum(float(re.search(r"draws_s=(\d+)", p).group(1)) for p in perf) / max(1, len(perf))
total_dt = 0.0
head = {}
sites = {}
for line in ks:
    left, _, right = line.partition("|")
    fields = dict(kv.split("=") for kv in left.split()[1:])
    dt = float(fields.pop("dt"))
    total_dt += dt
    for k, v in fields.items():
        head[k] = head.get(k, 0.0) + float(v) * dt
    for chunk in right.split(";"):
        parts = chunk.split()
        if not parts:
            continue
        site = sites.setdefault(parts[0], {})
        for kv in parts[1:]:
            kind, val = kv.split("=")
            nums = [float(x) for x in val.split(":")] + [0.0]
            c, u, n = site.get(kind, (0.0, 0.0, 0.0))
            site[kind] = (c + nums[0] * dt, u + nums[1] * dt, n + nums[2] * dt)

avg = {k: v / total_dt for k, v in head.items()}
print(f"window frames {f0}-{f1}: {len(ks)} KSTATS s, {draws:.0f} draws/s (PERF)")
print(f"thread: run {avg['run_ms']:.0f} ms/s, utime {avg['utime_ms']:.0f}, stime {avg['stime_ms']:.0f} ms/s "
      f"(proc, {100 * avg['stime_ms'] / max(1e-9, avg['utime_ms'] + avg['stime_ms']):.1f}% of CPU), "
      f"measured syscalls+handlers {avg['measured_ms']:.1f} ms/s; minflt {avg['minflt']:.0f}/s, "
      f"vcsw {avg['vcsw']:.0f}/s, ivcsw {avg['ivcsw']:.0f}/s")
rows = []
for site, kinds in sites.items():
    for kind, (c, u, n) in kinds.items():
        rows.append((site, kind, c / total_dt, u / total_dt, n / total_dt))
rows.sort(key=lambda r: (r[1] == "region", -r[3]))
print(f"{'site':<15}{'kind':<10}{'calls/s':>10}{'us/s':>10}{'ns/call':>9}{'ns/draw':>9}{'units/call':>11}")
for site, kind, c, u, n in rows:
    units = f"{n / max(c, 1e-9):>11.1f}" if n else ""
    print(f"{site:<15}{kind:<10}{c:>10.0f}{u:>10.0f}{1000 * u / max(c, 1e-9):>9.0f}{1000 * u / max(draws, 1):>9.1f}{units}")
