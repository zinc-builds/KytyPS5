#!/usr/bin/env python3
"""Per-draw metrics for one garden.sh frame window, from the emulator's METRICS lines.

metrics.py <log> <start_line> <f0> <f1> <json_out> [identity key=value ...]
metrics.py --rel <log> <A-B> <json_out|->   (post hoc: frames A-B after StartLevel, as garden.sh)

The emulator prints a METRICS line (cumulative counters, benchMetrics.h) right after each PERF
line. This takes the METRICS lines that follow the PERF lines garden.sh uses as its window edges
(the first with frames >= f0 and the first with frames >= f1, after log line <start_line>),
diffs them and divides by the draws in between. Writes the full result as JSON to <json_out>
and prints the short fields garden.sh appends to its summary line (nothing for a binary without
METRICS lines).

Per-thread numbers (cmd = KytyGpuCmd, prep0/prep1 = KytyGpuPrep*, rec = KytyGpuRec):
  ipd / cpd   user-mode instructions / cycles per draw (self-monitoring perf counters)
  ns          CPU ns per draw (schedstat, exact); uns + sns: its user / sys split (stat ticks,
              10 ms resolution per window edge)
  runq        ns per draw spent runnable but not running (preempted, e.g. by a build)
  busy        CPU time / wall time of the window
  x, td       with KYTY_BENCH_PMU=1 (cmd and prep threads, P-core PMU): user-mode event counts per
              draw, and the top-down split of the thread's cycles per draw
Command-thread waits per draw (ns, wall time, counted only when a wait happens):
  wait_prep   prepare worker: drain + refill + steady + idle (wait_drain, wait_refill, ...)
  wait_rec    recorder
Queue: prep_ns (mean worker time per prepared draw), rej_f (prepared draws redone on the command
thread, per frame), drains_kd (queue drains per 1,000 draws).
"""
import json
import os
import re
import sys

CLK_TCK = os.sysconf("SC_CLK_TCK")
THREADS = ("cmd", "prep0", "prep1", "prep2", "rec")
KV = re.compile(r"(\S+?)=(\S+)")


def parse_kv(line):
    out = {}
    for k, v in KV.findall(line):
        try:
            out[k] = float(v)
        except ValueError:
            out[k] = v
    return out


def window_edges(path, start, f0, f1):
    """(perf_a, metrics_a, perf_b, metrics_b, build): METRICS dicts may be None."""
    build = None
    edges = []  # [perf, metrics]
    want = f0
    pending = None
    with open(path, errors="replace") as f:
        for n, line in enumerate(f, 1):
            if line.startswith("BUILD ") and build is None:
                build = parse_kv(line)
            if n <= start:
                continue
            if pending is not None:
                # The METRICS line follows its PERF line (other lines may come between).
                if line.startswith("METRICS ") and parse_kv(line).get("frames") == pending[0]["frames"]:
                    pending[1] = parse_kv(line)
                    edges.append(pending)
                    pending = None
                    if len(edges) == 2:
                        break
                    continue
                if not line.startswith("PERF "):
                    continue
                edges.append(pending)
                pending = None
                if len(edges) == 2:
                    break
            if line.startswith("PERF ") and want is not None:
                perf = parse_kv(line.split(" threads:")[0])
                if "frames" in perf and perf["frames"] >= want:
                    pending = [perf, None]
                    want = f1 if want == f0 else None
    if pending is not None and len(edges) < 2:
        edges.append(pending)
    if len(edges) < 2:
        return None
    return edges[0][0], edges[0][1], edges[1][0], edges[1][1], build


def compute(a, b, pa, pb):
    d = lambda k: b.get(k, 0) - a.get(k, 0)
    frames = pb["frames"] - pa["frames"]
    wall_ns = (pb["t"] - pa["t"]) * 1e9
    draws = d("draws")
    if draws <= 0 or frames <= 0:
        return None
    r = {
        "fps": frames / (pb["t"] - pa["t"]),
        "frames": frames,
        "seconds": pb["t"] - pa["t"],
        "draws": draws,
        "draws_per_frame": draws / frames,
        "draws_per_s": draws / (wall_ns / 1e9),
        "proc_ns_per_draw": d("proc.cpu_ns") / draws,
        "proc_sys_ns_per_draw": d("proc.st") * 1e9 / CLK_TCK / draws,
        "threads": {},
    }
    for t in THREADS:
        if f"{t}.cpu_ns" not in b or f"{t}.cpu_ns" not in a:
            continue
        cpu = d(f"{t}.cpu_ns")
        sys_ns = min(d(f"{t}.st") * 1e9 / CLK_TCK, cpu)
        th = {
            "ns": cpu / draws,
            "sys_ns": sys_ns / draws,
            "user_ns": (cpu - sys_ns) / draws,
            "runq_ns": d(f"{t}.runq_ns") / draws,
            "busy": cpu / wall_ns,
            "vcs_per_kdraw": d(f"{t}.vcs") * 1000 / draws,
            "ics_per_kdraw": d(f"{t}.ics") * 1000 / draws,
        }
        if f"{t}.inst" in b and f"{t}.inst" in a:
            th["inst"] = d(f"{t}.inst") / draws
            th["cyc"] = d(f"{t}.cyc") / draws
            th["ipc"] = th["inst"] / th["cyc"] if th["cyc"] else 0.0
            th["pmu_run"] = b.get(f"{t}.pmu_run", 0)
        # KYTY_BENCH_PMU=1: event counts per draw ("x"), and the top-down split of the thread's
        # cycles per draw ("td": level 1 retiring / bad_spec / fe_bound / be_bound, level 2
        # heavy_ops, br_mispredict, fetch_lat, mem_bound, and core_bound = be_bound - mem_bound).
        xs = sorted({k.split(".", 2)[2] for k in b if k.startswith(f"{t}.x.")})
        if xs:
            th["x"] = {n: d(f"{t}.x.{n}") / draws for n in xs}
        slots = d(f"{t}.td.slots")
        if slots > 0 and "cyc" in th:
            td = {n: d(f"{t}.td.{n}") / slots * th["cyc"]
                  for n in ("retiring", "bad_spec", "fe_bound", "be_bound", "heavy_ops",
                            "br_mispredict", "fetch_lat", "mem_bound") if f"{t}.td.{n}" in b}
            if "be_bound" in td and "mem_bound" in td:
                td["core_bound"] = td["be_bound"] - td["mem_bound"]
            th["td"] = td
        r["threads"][t] = th
    waits = {k: d(f"{k}_wait_ns") / draws for k in ("drain", "refill", "steady", "idle")}
    r["wait"] = {
        "prep_ns": sum(waits.values()),
        **{f"{k}_ns": v for k, v in waits.items()},
        "prep_waits_per_kdraw": d("waits") * 1000 / draws,
        "rec_ns": d("rec_wait_ns") / draws,
        "rec_waits_per_kdraw": d("rec_waits") * 1000 / draws,
    }
    jobs = d("prep_jobs")
    r["prepare_ns"] = d("prep_busy_ns") / jobs if jobs > 0 else None
    r["prepared_share"] = jobs / draws
    r["rejected_per_frame"] = d("rejected") / frames
    r["drains_per_kdraw"] = d("drains") * 1000 / draws
    return r


def short(r):
    c = r["threads"].get("cmd", {})
    w = r["wait"]
    parts = []
    if "inst" in c:
        parts.append(f"cmd_ipd={c['inst']:.0f} cmd_cpd={c['cyc']:.0f}")
    if c:
        parts.append(f"cmd_ns={c['ns']:.0f} cmd_uns={c['user_ns']:.0f} cmd_sns={c['sys_ns']:.0f}")
    parts.append(f"wait_prep={w['prep_ns']:.0f} wait_rec={w['rec_ns']:.0f}")
    if r["prepare_ns"] is not None:
        parts.append(f"prep_ns={r['prepare_ns']:.0f}")
    parts.append(f"rej_f={r['rejected_per_frame']:.2f} drains_kd={r['drains_per_kdraw']:.2f}")
    return " " + " ".join(parts)


def level_base(log):
    """garden.sh's start line and base frame: the StartLevel line, then the first PERF frames=.
    The same rule as garden.sh: on the -lvl route (the log has the /app0/args.txt overlay) the
    first StartLevel, whatever the level; on the menu route (no overlay) StartLevel aerial_garden,
    because the world map may start first. Before, only aerial_garden counted, so on any other level
    the windows of framediag.py and framemodel.py were absolute flip counts (~480 frames early)."""
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    lvl = any("/app0/args.txt from overlay" in line for line in lines[:500])
    key = "StartLevel " if lvl else "StartLevel aerial_garden"
    start = next((n for n, line in enumerate(lines, 1) if key in line), None)
    if start is None:
        return None, None
    for line in lines[start:]:
        if line.startswith("PERF "):
            m = re.search(r"frames=(\d+)", line)
            if m:
                return start, int(m.group(1))
    return None, None


def main():
    if len(sys.argv) >= 5 and sys.argv[1] == "--rel":
        log, window, out = sys.argv[2], sys.argv[3], sys.argv[4]
        start, base = level_base(log)
        if start is None:
            return 1
        a, b = (int(x) for x in window.split("-"))
        sys.argv = [sys.argv[0], log, str(start), str(base + a), str(base + b), out, f"window={window}",
                    f"label={os.path.basename(log)[6:-4]}"]
    if len(sys.argv) < 6:
        print(__doc__, file=sys.stderr)
        return 2
    log, start, f0, f1, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    identity = {}
    for kv in sys.argv[6:]:
        k, _, v = kv.partition("=")
        identity[k] = v
    edges = window_edges(log, start, f0, f1)
    if edges is None:
        return 0
    pa, ma, pb, mb, build = edges
    if ma is None or mb is None:
        return 0  # a binary without METRICS lines
    r = compute(ma, mb, pa, pb)
    if r is None:
        return 0
    result = {**identity, "metrics": r}
    if build:
        result["rev"] = build.get("rev")
        result["build_hash"] = build.get("hash")
    if out == "-":
        json.dump(result, sys.stdout, sort_keys=True)
        print()
        return 0
    with open(out, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    print(short(r), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
