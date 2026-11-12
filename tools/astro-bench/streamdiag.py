#!/usr/bin/env python3
"""streamdiag.py <bench log> [A-B]: summarise KYTY_STREAM_DIAG=1 lines (streamDiag.h).

A-B: level frames (after the first StartLevel line), default 200-800. Frames are split into
normal and burst frames (draws >= 1.4 x the window's median); every share is draw-weighted over
the frames of its kind.
"""
import collections
import re
import statistics
import sys


def fields(line):
    out = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


def num(value):
    try:
        return float(value)
    except ValueError:
        return 0.0


def pairs(text):
    out = collections.Counter()
    if text and text != "-":
        for item in text.split(","):
            name, _, count = item.rpartition(":")
            out[name] += int(count)
    return out


def main():
    log = sys.argv[1]
    first, last = (int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "200-800").split("-"))
    strm, stra, strk, strb, period = {}, {}, {}, collections.defaultdict(list), collections.defaultdict(list)
    start = None
    with open(log, errors="replace") as handle:
        for line in handle:
            if start is None and "StartLevel" in line:
                start = -1
                continue
            tag = line[:5]
            if not tag.startswith("STR"):
                continue
            f = fields(line)
            frame = int(f.get("f", 0))
            if start == -1:
                start = frame
            if start is None:
                continue
            level = frame - start
            if tag == "STRM ":
                strm[level] = f
            elif tag == "STRA ":
                stra[level] = f
            elif tag == "STRK ":
                strk[level] = line.split()[3:]
            elif tag == "STRB ":
                strb[level].append(f)
            else:
                period[tag.strip()].append((level, line.split(None, 2)[2].strip() if len(line.split(None, 2)) > 2 else ""))
    frames = [n for n in sorted(strm) if first <= n <= last and n in stra]
    if not frames:
        print("no STRM/STRA lines in the window")
        return
    draws = {n: num(strm[n]["draws"]) for n in frames}
    median = statistics.median(draws.values())
    kinds = {"normal": [n for n in frames if draws[n] < 1.4 * median],
             "burst": [n for n in frames if draws[n] >= 1.4 * median]}
    print(f"{log}: level frames {frames[0]}-{frames[-1]}, {len(frames)} frames, median draws {median:.0f}")
    for kind, group in kinds.items():
        if not group:
            continue
        total = lambda key, src=strm: sum(num(src[n].get(key, 0)) for n in group)
        mean = lambda key, src=strm: total(key, src) / len(group)
        units = total("units", stra)
        print(f"\n== {kind} frames: {len(group)} (draws/frame {mean('draws'):.0f}) ==")
        print("B. guest command buffers per frame")
        print(f"  graphics submissions {mean('subs'):.1f} (with draws {mean('dsubs'):.1f}), compute "
              f"{mean('csubs'):.1f}; spans {mean('spans'):.0f}, INDIRECT_BUFFER {mean('calls'):.1f}")
        print(f"  draws in the largest submission {100 * total('max_sub') / total('draws'):.1f}%, "
              f"largest three {100 * total('top3') / total('draws'):.1f}%")
        print(f"  submissions with draws whose first draw inherits registers: "
              f"{100 * total('inh_subs') / max(total('dsubs'), 1):.1f}%; inherited per such submission: "
              f"cx {total('inh_cx') / max(total('dsubs'), 1):.1f} sh {total('inh_sh') / max(total('dsubs'), 1):.1f} "
              f"uc {total('inh_uc') / max(total('dsubs'), 1):.1f} misc {total('inh_misc') / max(total('dsubs'), 1):.1f}; "
              f"start with a context-state op: {100 * total('ctx_first') / max(total('dsubs'), 1):.1f}%")
        print(f"  per frame: labels {mean('labels'):.0f} waits {mean('waits'):.0f} EVENT_WRITE {mean('events'):.0f} "
              f"(occlusion dumps {mean('e39'):.0f}) dma/copy {mean('dma'):.0f}; targets {mean('targets'):.1f}, "
              f"drawn to from more than one submission {mean('shared_targets'):.1f}")
        for key in ("submitters", "builders"):
            counts = collections.Counter()
            for n in group:
                counts.update(pairs(strm[n].get(key, "-")))
            text = ", ".join(f"{name} {count / len(group):.1f}" for name, count in counts.most_common(12))
            print(f"  {key} per frame: {text}")
        print("A. draw segments recurring from earlier frames (share of units)")
        share = lambda key: 100 * total(key, stra) / max(units, 1)
        for back in ("1", "2", "3", "33", "any"):
            keys = ("E", "P", "S", "DE", "DP") if back != "any" else ("E", "P", "DP")
            print(f"  back {back:>3}: " + "  ".join(f"{k} {share(k + back):5.1f}%" for k in keys))
        print(f"  in runs following the previous frame's order: P >=8 {share('runP8'):.1f}% >=64 {share('runP64'):.1f}%; "
              f"E >=8 {share('runE8'):.1f}% >=64 {share('runE64'):.1f}%")
        print(f"  whole spans: same address and bytes {share('span_same'):.1f}%, same bytes anywhere {share('span_content'):.1f}%")
        paired = total("paired", stra)
        weighted = lambda key: sum(num(stra[n][key]) * num(stra[n]["units"]) for n in group) / max(units, 1)
        pair_weighted = lambda key: sum(num(stra[n][key]) * num(stra[n]["paired"]) for n in group) / max(paired, 1)
        print(f"  segment: {weighted('pk_seg'):.2f} packets, {weighted('dw_seg'):.1f} dwords, "
              f"{weighted('wr_seg'):.1f} registers written; live registers it does not write {weighted('deps'):.0f}")
        print(f"  same-shape pairs with the previous frame: {100 * paired / max(units, 1):.1f}% of units, "
              f"identical {100 * total('pair_same', stra) / max(paired, 1):.1f}%, "
              f"{pair_weighted('pair_diff'):.2f} of {pair_weighted('pair_vals'):.1f} values differ")
        markers = collections.defaultdict(lambda: [0, 0, 0, 0])
        for n in group:
            for item in strk.get(n, []):
                name, _, counts = item.rpartition("=")
                if not name:
                    continue
                for i, value in enumerate(counts.split("/")[:4]):
                    markers[name][i] += int(value)
        top = sorted(markers.items(), key=lambda kv: -kv[1][0])[:16]
        print("  draws per frame by marker path (draws/dispatches/EVENT_WRITE/occlusion dumps):")
        for name, c in top:
            print(f"    {name}: {c[0] / len(group):.0f}/{c[1] / len(group):.1f}/{c[2] / len(group):.0f}/{c[3] / len(group):.0f}")
    for tag in ("STRH", "STRF", "STRO", "STRI", "STRD", "STRL"):
        lines = [text for level, text in period[tag] if first <= level <= last + 100]
        if lines:
            print(f"\n{tag} (last period in the window):")
            last_frame = max(level for level, _ in period[tag] if first <= level <= last + 100)
            for level, text in period[tag]:
                if level == last_frame:
                    print("  " + text[:1500])


if __name__ == "__main__":
    main()
