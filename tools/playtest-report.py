#!/usr/bin/env python3
"""playtest-report.py <session dir> [--json] [--all]: one page on a playtest session
(tools/playtest.sh, KYTY_PLAYTEST=1; docs/playtest.md).

Prints: session length, how it ended, the FPS distribution (per second of play; load and boot
seconds apart), the share of presented frames that took two or more refreshes, incidents grouped
by their primary cause (count, worst case, time lost), markers with their screenshots, new
unresolved imports, memory growth, what else ran on the emulator's cores, and the top three
things to fix. Also writes <session>/report.json (the same, as data); --json prints that instead.
--all lists every incident, not only the worst per cause.
"""
import csv
import glob
import json
import math
import os
import re
import sys
from collections import defaultdict

REFRESH_MS = 1000.0 / 60.0

# The targeted run that explains each primary cause further (environment for levels.sh -e or
# garden.sh): what the recorder can't afford to collect all the time.
NEXT_RUN = {
    'GPU wait': 'KYTY_GPU_WAIT_DIAG=1 (waiting call sites), KYTY_GPU_TIME=draws (GPU time per draw)',
    'game late (no GPU work queued)': 'KYTY_FRAME_DIAG=1 (GIDLE: what the game\'s submitting thread waited on)',
    'command thread off-CPU': 'KYTY_OFFCPU_SAMPLE=1 KYTY_GPU_WAIT_DIAG=1 (where it slept); check system.jsonl intruders',
    'command thread CPU (draw work)': 'KYTY_BENCH_PMU=1 and a perf profile of the command thread at that spot',
    'shader compiles': 'shader-store warm-up (a cold store after a rebuild compiles in play); KYTY_PIPELINE_STATS=1',
    'pipeline creation': 'KYTY_PIPELINE_STATS=1 (driver statistics per pipeline)',
    'texture uploads': 'KYTY_FRAME_DIAG=1 and KYTY_ASYNC_UPLOAD=stats (upload sizes and queues)',
    'large buffer upload copies': 'KYTY_BDA_STATS=1, KYTY_ASYNC_UPLOAD=stats',
    'texture/buffer GC': 'KYTY_FRAME_DIAG=1 (gc_ns, evictions per frame)',
    'write collection': 'KYTY_KERNEL_STATS=1 (scan and write-protect system calls)',
    'prepare-worker wait': 'KYTY_STALL_DIAG=1 KYTY_PREPARE_STATS=1',
    'recorder wait': 'KYTY_STALL_DIAG=1 KYTY_DRIVER_STATS=1 KYTY_GPU_WAIT_DIAG=1',
    'render-context mutex': 'KYTY_STALL_DIAG=1 (other threads\' synchronous commands)',
}


def next_run(cause, level):
    for key, env in NEXT_RUN.items():
        if key in cause:
            where = f"tools/astro-bench/levels.sh -m perf {level} -e '...'" if level else 'garden.sh'
            return f'{env}; e.g. {where}'
    return None


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def load_jsonl(path):
    out = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def pct(values, p):
    if not values:
        return None
    v = sorted(values)
    k = (len(v) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] if lo == hi else v[lo] + (v[hi] - v[lo]) * (k - lo)


def slope(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def fmt_t(s):
    s = int(s)
    return f'{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


def shot_path(session, rel):
    if not rel:
        return None
    for cand in (rel[:-4] + '.png' if rel.endswith('.ppm') else rel, rel):
        if os.path.exists(os.path.join(session, cand)):
            return os.path.join(session, cand)
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    session = os.path.abspath(args[0])
    as_json = '--json' in sys.argv
    show_all = '--all' in sys.argv
    start = load_json(os.path.join(session, 'session.json')) or {}
    end = load_json(os.path.join(session, 'session_end.json'))
    crash = load_json(os.path.join(session, 'crash.json'))
    launch = {}
    try:
        with open(os.path.join(session, 'launch.txt')) as f:
            for line in f:
                k, _, v = line.partition(':')
                launch[k.strip()] = v.strip()
    except OSError:
        pass

    # Frames.
    frames = []
    try:
        with open(os.path.join(session, 'frames.csv')) as f:
            for row in csv.DictReader(f):
                try:
                    frames.append({k: float(v) for k, v in row.items() if v not in (None, '')})
                except ValueError:
                    pass
    except OSError:
        pass
    presents = []
    try:
        with open(os.path.join(session, 'present.csv')) as f:
            for row in csv.DictReader(f):
                try:
                    presents.append((float(row['t']), float(row['ms'])))
                except (ValueError, KeyError):
                    pass
    except OSError:
        pass
    events = load_jsonl(os.path.join(session, 'events.jsonl'))
    system = load_jsonl(os.path.join(session, 'system.jsonl'))
    incidents = []
    markers = []
    for path in sorted(glob.glob(os.path.join(session, 'incidents', '*.json'))):
        rec = load_json(path)
        if not rec:
            continue
        rec.pop('frames', None)
        rec.pop('presents_ms', None)
        rec['_events'] = rec.pop('events', [])
        rec['file'] = os.path.relpath(path, session)
        (markers if rec.get('type') == 'marker' else incidents).append(rec)

    duration = frames[-1]['t'] if frames else (end or {}).get('duration_s', 0)
    play = [f for f in frames if f.get('load', 0) == 0]
    # FPS per whole second of play: guest frames whose time falls in it (seconds with any load or
    # boot frame are left out).
    per_sec = defaultdict(int)
    load_secs = set()
    for f in frames:
        sec = int(f['t'])
        per_sec[sec] += 1
        if f.get('load', 0):
            load_secs.add(sec)
    # Every whole second between the first and the last frame, frozen ones (no frame) included;
    # a frameless second belongs to the phase of the frame that ends the gap.
    play_fps = []
    if frames:
        first, last = int(frames[0]['t']), int(frames[-1]['t'])
        next_load = {}
        for f in frames:  # an occupied second takes its first frame's phase
            next_load.setdefault(int(f['t']), f.get('load', 0))
        carry = 0
        for sec in range(last, first - 1, -1):
            carry = next_load.get(sec, carry)
            next_load[sec] = carry
        for sec in range(first + 1, last):
            if sec in load_secs or (per_sec.get(sec, 0) == 0 and next_load.get(sec)):
                continue
            play_fps.append(per_sec.get(sec, 0))
    buckets = {'60+': 0, '57-59': 0, '50-56': 0, '40-49': 0, '<40': 0}
    for v in play_fps:
        key = '60+' if v >= 60 else '57-59' if v >= 57 else '50-56' if v >= 50 else '40-49' if v >= 40 else '<40'
        buckets[key] += 1
    n_sec = max(1, len(play_fps))
    fps = {
        'seconds': len(play_fps),
        'mean': round(sum(play_fps) / n_sec, 2) if play_fps else None,
        'p50': pct(play_fps, 50), 'p5': pct(play_fps, 5), 'min': min(play_fps) if play_fps else None,
        'share': {k: round(100.0 * v / n_sec, 2) for k, v in buckets.items()},
    }
    # Presented frames by refreshes (whole session and play only).
    play_ranges = []
    for f in play:
        play_ranges.append(f['t'])
    load_t = sorted(f['t'] for f in frames if f.get('load', 0))
    def in_load(t):
        # A present is a load present when a load frame lies within 0.5 s of it.
        import bisect
        i = bisect.bisect_left(load_t, t - 0.5)
        return i < len(load_t) and load_t[i] <= t + 0.5
    vbl = [max(1, round(ms / REFRESH_MS)) for t, ms in presents if not in_load(t)]
    pres = {
        'presented': len(vbl),
        'two_refresh_pct': round(100.0 * sum(1 for v in vbl if v == 2) / max(1, len(vbl)), 2),
        'three_plus_pct': round(100.0 * sum(1 for v in vbl if v >= 3) / max(1, len(vbl)), 3),
    }
    guest_ms = [f['ms'] for f in play]
    long_frames = {
        'over25': sum(1 for v in guest_ms if v > 25), 'over33': sum(1 for v in guest_ms if v > 33.4),
        'over100': sum(1 for v in guest_ms if v > 100), 'worst_ms': round(max(guest_ms), 1) if guest_ms else None,
        'p99_ms': round(pct(guest_ms, 99), 2) if guest_ms else None,
    }
    # Memory.
    rss_pts = [(f['t'], f['rss_mb']) for f in frames if f.get('rss_mb', 0) > 0]
    # Growth while playing: play frames only (level loads allocate on purpose), over 3 min or more.
    rss_play = [(f['t'], f['rss_mb']) for f in play if f.get('rss_mb', 0) > 0]
    s = None
    if rss_play and rss_play[-1][0] - rss_play[0][0] >= 180:
        s = slope([t for t, _ in rss_play], [r for _, r in rss_play])
    vram = [x['gpu']['vram_used_mb'] for x in system if 'gpu' in x]
    memory = {
        'rss_start_mb': round(rss_pts[0][1]) if rss_pts else None,
        'rss_end_mb': round(rss_pts[-1][1]) if rss_pts else None,
        'rss_slope_mb_per_min': round(s * 60, 1) if s is not None else None,
        'vram_start_mb': vram[0] if vram else None, 'vram_end_mb': vram[-1] if vram else None,
        'vram_max_mb': max(vram) if vram else None,
    }
    # System: who else ran on the emulator's cores, temperatures, clocks.
    intruders = defaultdict(lambda: [0.0, 0])
    temps_max = defaultdict(float)
    for x in system:
        for i in x.get('intruders', []):
            intruders[i['comm']][0] += i['pct']
            intruders[i['comm']][1] += 1
        for k, v in x.get('temps', {}).items():
            temps_max[k] = max(temps_max[k], v)
    n_sys = max(1, len(system))
    # Thermal throttling: samples with the CPU package at its critical temperature, and the clock of
    # the emulator's cores then against the rest of the session. An outside cause (cooling).
    limits = next((x['temp_limits'] for x in system if x.get('temp_limits')), None)
    if limits is None:  # sessions from before the sampler read the limit: Intel's usual TjMax
        limits = {k: 100.0 for x in system[:1] for k in x.get('temps', {}) if k.startswith('coretemp:Package')}
    hot_t, mhz_hot, mhz_ok = [], [], []
    for x in system:
        cores = x.get('cores', {})
        mhz = [cores[str(c)]['mhz'] for c in x.get('hot_cores', []) if cores.get(str(c), {}).get('mhz')]
        at_limit = any(x.get('temps', {}).get(k, 0) >= lim - 1.0 for k, lim in limits.items())
        if at_limit and 'start_mono_s' in start:
            hot_t.append(x['t'] - start['start_mono_s'])
        if mhz:
            (mhz_hot if at_limit else mhz_ok).append(sum(mhz) / len(mhz))
    thermal = None
    if hot_t:
        thermal = {'samples_at_limit': len(hot_t), 'samples': len(system), 'first_t': round(hot_t[0], 1),
                   'limit_c': max(limits.values()),
                   'core_mhz_at_limit': round(sum(mhz_hot) / len(mhz_hot)) if mhz_hot else None,
                   'core_mhz_otherwise': round(sum(mhz_ok) / len(mhz_ok)) if mhz_ok else None}
    sys_info = {
        'samples': len(system),
        'intruders': sorted(({'comm': k, 'avg_pct': round(v[0] / n_sys, 2), 'seen': v[1]} for k, v in intruders.items()),
                            key=lambda d: -d['avg_pct'])[:5],
        'temps_max': {k: round(v, 1) for k, v in sorted(temps_max.items())},
        'thermal_limit': thermal,
    }
    # Levels.
    levels = []
    for e in events:
        if e.get('type') == 'level' and e.get('kind') == 'StartLevel':
            levels.append({'t': e['t'], 'name': e['name']})
    # Stubs.
    stubs = [{'t': e['t'], 'name': e['name']} for e in events if e.get('type') == 'stub_first_call']
    if end:
        calls = {s['name']: s['calls'] for s in end.get('stubs', [])}
        for st in stubs:
            st['calls'] = calls.get(st['name'])
    # Incidents by cause.
    def frame_excess(rec):
        # Every long frame of the incident (a merged stretch has many), else its worst one.
        if rec.get('lost_ms'):  # 0 (no frame over 33.4 ms): fall back to the worst frame
            return max(0.0, rec['lost_ms'])
        longs = rec.get('long_frames') or []
        if longs:
            return sum(max(0.0, f['ms'] - REFRESH_MS) for f in longs)
        return max(0.0, (rec.get('worst_ms') or 0) - REFRESH_MS)
    groups = defaultdict(list)
    for rec in incidents:
        cause = rec.get('primary_cause', 'none')
        trig = rec.get('triggers', {})
        if 'freeze>2s' in trig:
            cause = 'freeze (no flips for 2 s or more): ' + cause
        elif 'new-stub' in trig and len(trig) == 1:
            cause = 'new unresolved import'
        elif 'fatal' in trig:
            cause = 'fatal: ' + cause
        groups[(rec.get('phase', 'play'), cause)].append(rec)
    by_cause = []
    for (phase, cause), recs in groups.items():
        worst = max(recs, key=lambda r: r.get('worst_ms') or 0)
        by_cause.append({
            'phase': phase, 'cause': cause, 'count': len(recs),
            'lost_ms': round(sum(frame_excess(r) for r in recs), 1),
            'worst': {'id': worst.get('id'), 't': worst.get('t'), 'worst_ms': worst.get('worst_ms'),
                      'summary': worst.get('summary'), 'file': worst.get('file'),
                      'screenshot': shot_path(session, worst.get('screenshot')), 'level': worst.get('level')},
            'ids': [r.get('id') for r in recs],
            'next_run': next_run(cause, worst.get('level')),
        })
    by_cause.sort(key=lambda g: (g['phase'] != 'play', -g['lost_ms'], -g['count']))
    marker_list = []
    for m in markers:
        marker_list.append({
            'number': m.get('marker'), 't': m.get('t'), 'wall': m.get('wall'), 'level': m.get('level'),
            'sublevel': m.get('sublevel'), 'screenshot': shot_path(session, m.get('screenshot')),
            'screenshot_status': m.get('screenshot_status'), 'worst_ms_10s': m.get('worst_ms'),
            'summary_10s': m.get('summary'), 'file': m.get('file'),
        })

    # Top three things to fix.
    top = []
    if crash:
        top.append(f"CRASH: {crash.get('reason')} ({crash.get('detail')}) at {fmt_t(crash.get('t', 0))}, "
                   f"level {crash.get('level') or '?'}: crash.json")
    for g in by_cause:
        if g['phase'] != 'play' or len(top) >= 3:
            continue
        w = g['worst']
        top.append(f"{g['cause']}: {g['count']} incident(s), {g['lost_ms']:.0f} ms lost; worst {w['worst_ms']} ms "
                   f"at {fmt_t(w['t'] or 0)} ({w['file']}): {w['summary']}")
    if thermal and len(top) < 3 and (not thermal['core_mhz_at_limit'] or not thermal['core_mhz_otherwise'] or
                                     thermal['core_mhz_at_limit'] < 0.97 * thermal['core_mhz_otherwise']):
        top.append(f"not the emulator: the CPU package reached its thermal limit ({thermal['limit_c']:.0f} °C) in "
                   f"{thermal['samples_at_limit']} of {thermal['samples']} system samples (first at "
                   f"{fmt_t(thermal['first_t'])}), the emulator's cores at {thermal['core_mhz_at_limit']} MHz then vs "
                   f"{thermal['core_mhz_otherwise']}; check the cooling (fan curve, dust, power profile)")
    if len(top) < 3 and pres['two_refresh_pct'] > 1.0:
        top.append(f"pacing: {pres['two_refresh_pct']}% of presented frames took two refreshes outside loads "
                   f"(target < 1%)")
    if len(top) < 3 and stubs:
        top.append(f"{len(stubs)} unresolved import(s) called: " + ', '.join(s['name'] for s in stubs[:5]))
    if len(top) < 3 and marker_list:
        top.append(f"{len(marker_list)} marker(s) to look at by eye: " +
                   ', '.join(f"#{m['number']} ({m['screenshot'] or 'no screenshot'})" for m in marker_list[:3]))
    if len(top) < 3 and memory['rss_slope_mb_per_min'] and memory['rss_slope_mb_per_min'] > 20:
        top.append(f"memory grows {memory['rss_slope_mb_per_min']} MB/min")
    for g in by_cause:
        if len(top) >= 3:
            break
        if g['phase'] != 'play':
            w = g['worst']
            top.append(f"[{g['phase']}] {g['cause']}: {g['count']} incident(s), worst {w['worst_ms']} ms ({w['file']})")

    ended = 'crash' if crash else ('normal exit' if end else 'no end record (killed?)')
    report = {
        'session': session, 'commit': launch.get('commit') or start.get('git_hash'),
        'build_mode': launch.get('build_mode') or start.get('build_mode'), 'install': launch.get('install'),
        'started': start.get('start_wall'), 'duration_s': round(duration, 1),
        'guest_frames': int(frames[-1]['g']) if frames else 0, 'ended': ended,
        'exit_status': launch.get('exit_status'), 'crash': crash and {k: crash.get(k) for k in ('reason', 'detail', 't', 'g', 'level')},
        'levels': levels, 'fps': fps, 'presents': pres, 'long_frames': long_frames, 'memory': memory,
        'incidents': {'total': len(incidents), 'by_phase': {p: sum(1 for r in incidents if r.get('phase') == p)
                                                             for p in ('play', 'load', 'boot')},
                      'unwritten': (end or {}).get('incidents_unwritten', 0), 'by_cause': by_cause},
        'markers': marker_list, 'stubs': stubs, 'system': sys_info, 'top3': top[:3],
        'dropped': {'events': (end or {}).get('events_dropped'), 'frames': (end or {}).get('frames_dropped')},
    }
    with open(os.path.join(session, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)
    if as_json:
        print(json.dumps(report, indent=1))
        return

    out = []
    p = out.append
    p(f"PLAYTEST {session}")
    p(f"build {report['commit'] or '?'} ({report['build_mode'] or '?'}); started {report['started']}; "
      f"{fmt_t(duration)} long, {report['guest_frames']} guest frames; ended: {ended}"
      + (f" (exit status {report['exit_status']})" if report['exit_status'] not in (None, '') else ''))
    if crash:
        p(f"  CRASH: {crash.get('reason')}: {crash.get('detail')} at {fmt_t(crash.get('t', 0))} g={crash.get('g')} "
          f"level={crash.get('level')}")
    if levels:
        p('levels: ' + ', '.join(f"{l['name']} @{fmt_t(l['t'])}" for l in levels))
    if fps['seconds']:
        sh = fps['share']
        p(f"FPS over {fps['seconds']} s of play: mean {fps['mean']}, p50 {fps['p50']:.0f}, p5 {fps['p5']:.0f}, "
          f"min {fps['min']}; seconds at 60: {sh['60+']}%, 57-59: {sh['57-59']}%, 50-56: {sh['50-56']}%, "
          f"40-49: {sh['40-49']}%, <40: {sh['<40']}%")
    else:
        p('FPS: no seconds of play outside loads')
    p(f"presented frames outside loads: {pres['presented']}; two refreshes {pres['two_refresh_pct']}%, "
      f"three or more {pres['three_plus_pct']}%")
    p(f"guest frames in play: p99 {long_frames['p99_ms']} ms, worst {long_frames['worst_ms']} ms; >25 ms: "
      f"{long_frames['over25']}, >33 ms: {long_frames['over33']}, >100 ms: {long_frames['over100']}")
    m = memory
    growth = f"{m['rss_slope_mb_per_min']} MB/min" if m['rss_slope_mb_per_min'] is not None else 'n/a, under 3 min of play'
    p(f"memory: RSS {m['rss_start_mb']} -> {m['rss_end_mb']} MB (growth in play: {growth}); "
      f"VRAM {m['vram_start_mb']} -> {m['vram_end_mb']} MB (max {m['vram_max_mb']})")
    inc = report['incidents']
    p(f"incidents: {inc['total']} (play {inc['by_phase']['play']}, load {inc['by_phase']['load']}, "
      f"boot {inc['by_phase']['boot']}){', ' + str(inc['unwritten']) + ' past the cap' if inc['unwritten'] else ''}; "
      f"markers {len(marker_list)}")
    for g in by_cause:
        w = g['worst']
        p(f"  [{g['phase']}] {g['cause']}: {g['count']}x, {g['lost_ms']:.0f} ms lost; worst {w['worst_ms']} ms "
          f"@{fmt_t(w['t'] or 0)} {w['file']}")
        p(f"      {w['summary']}")
        if w['screenshot']:
            p(f"      screenshot {w['screenshot']}")
        if g.get('next_run'):
            p(f"      next run: {g['next_run']}")
        if show_all and len(g['ids']) > 1:
            p(f"      all: {g['ids']}")
    for mk in marker_list:
        p(f"  marker {mk['number']} @{fmt_t(mk['t'] or 0)} ({mk['wall']}) level {mk['level']}/{mk['sublevel']}: "
          f"{mk['screenshot'] or 'no screenshot (' + str(mk['screenshot_status']) + ')'}")
        p(f"      last 10 s: worst {mk['worst_ms_10s']} ms: {mk['summary_10s']}  ({mk['file']})")
    if stubs:
        p('new unresolved imports: ' + ', '.join(f"{s['name']} ({s.get('calls')} calls) @{fmt_t(s['t'])}" for s in stubs))
    if sys_info['samples']:
        intr = ', '.join(f"{i['comm']} {i['avg_pct']}%" for i in sys_info['intruders']) or 'none'
        tm = ', '.join(f'{k} {v}' for k, v in sys_info['temps_max'].items())
        p(f"system ({sys_info['samples']} samples): other processes on the emulator's cores: {intr}; max temps: {tm}")
        th = sys_info['thermal_limit']
        if th:
            clk = (f"; the emulator's cores ran at {th['core_mhz_at_limit']} MHz then vs {th['core_mhz_otherwise']} MHz "
                   f"otherwise" if th['core_mhz_at_limit'] and th['core_mhz_otherwise'] else '')
            p(f"THERMAL: CPU package at its limit ({th['limit_c']:.0f} °C) in {th['samples_at_limit']} of "
              f"{th['samples']} samples, first at {fmt_t(th['first_t'])}{clk}: the CPU throttles there, so slow "
              f"frames in those seconds are partly the cooling's doing (an outside cause)")
    if report['dropped']['events'] or report['dropped']['frames']:
        p(f"dropped: {report['dropped']['events']} events, {report['dropped']['frames']} frame records")
    p('top three things to fix:')
    for i, t in enumerate(top[:3], 1):
        p(f"  {i}. {t}")
    if not top:
        p('  nothing: no incidents, markers or crash')
    p(f"(data: {os.path.join(session, 'report.json')})")
    print('\n'.join(out))


if __name__ == '__main__':
    main()
