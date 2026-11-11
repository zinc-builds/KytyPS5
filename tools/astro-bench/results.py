#!/usr/bin/env python3
"""Shared bench result store and A/B verdicts. Commands (results.sh wraps them):

  results.py add <json>                       append one garden.sh result to the store
  results.py ref <sha> [--window W] [--env E] [--flags F] [--max-age H] [--exe HASH]
                                              stored reference runs of <sha>: median and spread
  results.py table <json|label-glob>...       median and spread of runs (files or stored labels)
  results.py verdict --ref SEL... --new SEL... [--reused]
                                              one A/B verdict line (SEL: json file or label glob)

The store is ~/.cache/kyty-builds/results.jsonl (KYTY_RESULTS overrides): one JSON object per
garden.sh run with its identity (rev, exe hash, build mode, install, window, flags, KYTY_* env,
time, label) and metrics (see metrics.py).
"""
import fcntl
import fnmatch
import json
import math
import os
import statistics
import sys
import time
from datetime import datetime

STORE = os.environ.get("KYTY_RESULTS", os.path.expanduser("~/.cache/kyty-builds/results.jsonl"))

# name, path in "metrics", format, per-run noise floor (sd %) for the (long, short) window, the
# drift (sd %) between batches, added when the reference comes from another batch (None: the
# metric drifts too much, with clock speed and thermal state, for a stored reference to count),
# and the layout term (sd %), added when the two sides are different binaries: two builds of the
# same hot-path code differ by up to ~1% in cycles, CPU time and FPS (code alignment), while
# instructions stay within 0.1%. No number of runs removes that term.
# Floors: 5 runs of one binary in one batch (PR #86); long = frames 1000-1600, short = 200-500.
# Drift and layout: see PLAN-60fps.md "Bench metrics".
METRICS = [
    ("ipd", ("threads", "cmd", "inst"), "{:.0f}", (0.15, 0.17), 0.1, 0.1),
    ("cpd", ("threads", "cmd", "cyc"), "{:.0f}", (0.47, 0.53), 0.2, 0.6),
    ("cmd_ns", ("threads", "cmd", "ns"), "{:.0f}", (0.37, 0.77), 0.6, 0.6),
    ("cmd_uns", ("threads", "cmd", "user_ns"), "{:.0f}", (0.25, 0.95), 0.6, 0.9),
    ("cmd_sns", ("threads", "cmd", "sys_ns"), "{:.0f}", (1.6, 2.6), 3.0, 4.0),
    ("wait_prep", ("wait", "prep_ns"), "{:.0f}", (9.9, 4.2), None, 5.0),
    ("wait_rec", ("wait", "rec_ns"), "{:.0f}", (2.2, 8.7), None, 5.0),
    ("prep_ns", ("prepare_ns",), "{:.0f}", (1.6, 0.75), None, 1.5),
    ("prep_ipd", ("threads", "prep0", "inst"), "{:.0f}", (0.30, 0.20), 0.2, 0.1),
    ("rec_ipd", ("threads", "rec", "inst"), "{:.0f}", (0.10, 0.19), 0.2, 0.2),
    ("fps", ("fps",), "{:.2f}", (0.39, 1.14), None, 0.8),
    ("draws_f", ("draws_per_frame",), "{:.0f}", (0.15, 0.47), 0.2, 0.0),
    ("rej_f", ("rejected_per_frame",), "{:.2f}", (1.2, 4.3), None, 2.0),
    ("drains_kd", ("drains_per_kdraw",), "{:.2f}", (0.25, 0.25), 0.3, 0.0),
]
# Shown in the verdict line, in this order.
VERDICT = ["ipd", "cpd", "cmd_uns", "cmd_sns", "wait_prep", "wait_rec", "fps"]
LONG_WINDOW = "1000-1600"


def get(result, path):
    v = result.get("metrics", {})
    for p in path:
        if not isinstance(v, dict) or p not in v:
            return None
        v = v[p]
    return v


def load(path=STORE):
    out = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    except FileNotFoundError:
        pass
    return out


def add(json_path):
    with open(json_path) as f:
        result = json.load(f)
    os.makedirs(os.path.dirname(STORE), exist_ok=True)
    with open(STORE, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.write(json.dumps(result, sort_keys=True) + "\n")


def age_hours(result):
    try:
        return (time.time() - datetime.fromisoformat(result["time"]).timestamp()) / 3600
    except (KeyError, ValueError):
        return math.inf


def is_warmup(result):
    return result.get("label", "").endswith("-warmup")


def stats(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    med = statistics.median(values)
    return {"n": len(values), "median": med, "min": min(values), "max": max(values),
            "range": (max(values) - min(values)) / med * 100 if med else 0.0,
            "sd": statistics.stdev(values) / med * 100 if len(values) > 1 and med else 0.0}


def table(results):
    print(f"{'metric':10} {'n':>2} {'median':>9} {'min':>9} {'max':>9} {'range%':>7} {'sd%':>6}")
    for name, path, fmt, *_ in METRICS:
        s = stats([get(r, path) for r in results])
        if s is None:
            continue
        print(f"{name:10} {s['n']:>2} {fmt.format(s['median']):>9} {fmt.format(s['min']):>9} "
              f"{fmt.format(s['max']):>9} {s['range']:>7.2f} {s['sd']:>6.2f}")


def select(selectors):
    out, stored = [], None
    for a in selectors:
        if os.path.exists(a):
            with open(a) as f:
                out.append(json.load(f))
        else:
            stored = load() if stored is None else stored
            out.extend(r for r in stored if fnmatch.fnmatch(r.get("label", ""), a) and not is_warmup(r))
    return out


def references(sha, window=None, env=None, flags=None, max_age=None, exe=None, mode=None):
    """Stored clean runs of commit <sha>: no -dirty build, same window, env, flags, build mode."""
    out = []
    for r in load():
        if is_warmup(r) or not str(r.get("rev", "")).startswith(sha) or "dirty" in str(r.get("build_hash", "")):
            continue
        if window is not None and r.get("window") != window:
            continue
        if env is not None and r.get("env", "") != env:
            continue
        if flags is not None and r.get("flags", "") != flags:
            continue
        if exe is not None and r.get("exe") != exe:
            continue
        if mode is not None and r.get("build_mode", "plain") != mode:
            continue
        if max_age is not None and age_hours(r) > max_age:
            continue
        out.append(r)
    return out


def verdict(refs, news, reused=False, label=""):
    """One line: each metric's change against its noise. sigma combines the per-run noise floor
    (or the measured sd when larger) of both sides; |delta| >= 3 sigma is a signal (!), >= 2 is
    weak (?), else noise (~). With reused references, metrics that drift between batches (FPS,
    waits) are shown but marked: claims of record need a same-batch pair."""
    window = (news or refs)[0].get("window", "") if (news or refs) else ""
    # Different binaries (by hash, else by install dir): add the code-layout term.
    ident = lambda r: r.get("exe") or r.get("install")
    cross = bool({ident(r) for r in refs} != {ident(r) for r in news})
    short = window != LONG_WINDOW
    parts = []
    for name in VERDICT:
        _, path, fmt, floors, drift, layout = next(m for m in METRICS if m[0] == name)
        rs, ns = stats([get(r, path) for r in refs]), stats([get(r, path) for r in news])
        if rs is None or ns is None:
            continue
        floor = floors[1 if short else 0]
        sr = max(floor, rs["sd"] if rs["n"] >= 3 else 0.0)
        sn = max(floor, ns["sd"] if ns["n"] >= 3 else 0.0)
        sigma = math.sqrt(sr * sr / rs["n"] + sn * sn / ns["n"] + (drift or 0.0) ** 2 * reused
                          + layout ** 2 * cross)
        delta = (ns["median"] - rs["median"]) / rs["median"] * 100 if rs["median"] else 0.0
        z = abs(delta) / sigma if sigma else 0.0
        mark = "!" if z >= 3 else "?" if z >= 2 else "~"
        if reused and drift is None:
            mark = "x"
        if name in ("wait_prep", "wait_rec"):
            text = f"{name} {ns['median'] - rs['median']:+.0f}ns({delta:+.1f}%±{sigma:.1f}){mark}"
        else:
            text = f"{name} {delta:+.2f}%±{sigma:.2f}{mark}"
        parts.append(text)
    head = f"AB {label}: " if label else "AB: "
    tail = (f" [ref n={len(refs)} {'stored' if reused else 'same batch'}, new n={len(news)}, "
            f"window {window}{', two binaries: +layout noise' if cross else ''}; ! signal >=3sd, ? 2-3sd, ~ noise"
            + (", x not valid against a stored ref: FPS of record needs a same-batch pair" if reused else "")
            + "]")
    return head + " ".join(parts) + tail


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "add":
        add(args[0])
    elif cmd == "table":
        table(select(args))
    elif cmd == "ref":
        import argparse
        p = argparse.ArgumentParser(prog="results.sh ref")
        p.add_argument("sha")
        p.add_argument("--window", default=None)
        p.add_argument("--env", default=None)
        p.add_argument("--flags", default=None)
        p.add_argument("--exe", default=None)
        p.add_argument("--mode", default=None)
        p.add_argument("--max-age", type=float, default=None)
        p.add_argument("--count", action="store_true", help="print only the number of runs")
        a = p.parse_args(args)
        rs = references(a.sha, a.window, a.env, a.flags, a.max_age, a.exe, a.mode)
        if a.count:
            print(len(rs))
            return 0
        if not rs:
            print(f"no stored reference runs of {a.sha}", file=sys.stderr)
            return 1
        windows = sorted({r.get("window", "") for r in rs})
        for w in windows:
            group = [r for r in rs if r.get("window", "") == w]
            youngest = min(age_hours(r) for r in group)
            print(f"== {a.sha[:12]} window {w}: {len(group)} runs, newest {youngest:.1f} h old, "
                  f"env '{group[0].get('env', '')}' flags '{group[0].get('flags', '')}'")
            table(group)
    elif cmd == "verdict":
        import argparse
        p = argparse.ArgumentParser(prog="results.py verdict")
        p.add_argument("--ref", nargs="+", default=[])
        p.add_argument("--new", nargs="+", required=True)
        p.add_argument("--reused", action="store_true")
        p.add_argument("--label", default="")
        # Stored reference runs instead of --ref (implies --reused).
        p.add_argument("--ref-stored", default=None)
        p.add_argument("--window", default=None)
        p.add_argument("--env", default=None)
        p.add_argument("--flags", default=None)
        p.add_argument("--exe", default=None)
        p.add_argument("--max-age", type=float, default=None)
        a = p.parse_args(args)
        news = select(a.new)
        if a.ref_stored:
            refs = references(a.ref_stored, a.window, a.env, a.flags, a.max_age, a.exe)
            a.reused = True
        else:
            refs = select(a.ref)
        if not refs or not news:
            print("verdict: no runs with metrics on one side", file=sys.stderr)
            return 1
        print(verdict(refs, news, a.reused, a.label))
    else:
        print(f"unknown command {cmd}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
