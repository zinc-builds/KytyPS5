#!/usr/bin/env python3
"""passdiag.py <bench log> [--heavy N] [--frame F]: summarise a KYTY_PASS_DIAG=1 run.

Frames after the level start are split into heavy (more than N draws, default 1.3x the median)
and the rest; per kind: draws, queue drains, drain reasons and EVENT_WRITE types per frame, then
the debug-marker paths with the most drains. --frame F prints that frame's target groups (PASST)."""
import argparse
import collections
import re

KV = re.compile(r"(\w+)=(\S+)")


def counts(text):
    out = collections.Counter()
    if text and text != "-":
        for item in text.split(","):
            k, _, n = item.rpartition(":")
            out[k] += int(n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--heavy", type=int)
    ap.add_argument("--frame", type=int)
    a = ap.parse_args()
    lines = open(a.log, encoding="utf-8", errors="replace").read().splitlines()
    start = next((i for i, l in enumerate(lines) if "StartLevel " in l), 0)
    frames, markers = {}, collections.defaultdict(list)
    for l in lines[start:]:
        if l.startswith("PASS "):
            kv = dict(KV.findall(l))
            frames[int(kv["f"])] = kv
        elif l.startswith("PASSM "):
            kv = dict(KV.findall(l))
            markers[int(kv["f"])].append((kv["marker"], int(kv["draws"]), int(kv["drains"])))
        elif a.frame is not None and l.startswith(f"PASST f={a.frame} "):
            print(l)
    if not frames:
        print("passdiag: no PASS lines (run with KYTY_PASS_DIAG=1)")
        return
    draws = sorted(int(kv["draws"]) for kv in frames.values())
    heavy = a.heavy if a.heavy is not None else int(1.3 * draws[len(draws) // 2])
    for name, pick in (("heavy", lambda d: d > heavy), ("rest", lambda d: d <= heavy)):
        sel = [f for f, kv in frames.items() if pick(int(kv["draws"]))]
        if not sel:
            continue
        n = len(sel)
        reasons, events, by_marker = collections.Counter(), collections.Counter(), collections.Counter()
        for f in sel:
            reasons += counts(frames[f]["reasons"])
            events += counts(frames[f]["events"])
            for m, d, dr in markers.get(f, []):
                by_marker[m] += dr
        d = sum(int(frames[f]["draws"]) for f in sel) / n
        dr = sum(int(frames[f]["drains"]) for f in sel) / n
        print(f"== {name} frames (draws {'>' if name == 'heavy' else '<='} {heavy}): {n}, "
              f"draws/frame {d:.0f}, drains/frame {dr:.0f}")
        print("   drain reasons/frame:", " ".join(f"{k}={v / n:.0f}" for k, v in reasons.most_common(6)))
        print("   EVENT_WRITE types/frame:", " ".join(f"0x{int(k):02x}={v / n:.0f}" for k, v in events.most_common(6)))
        print("   markers by drains/frame:")
        for m, v in by_marker.most_common(6):
            print(f"     {v / n:7.0f}  {m}")


if __name__ == "__main__":
    main()
