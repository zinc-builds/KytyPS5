# Playtest flight recorder

When the user plays by hand, the emulator records every hitch and problem on its own, with
enough context to diagnose it afterwards without a written report.

## Starting a session

```
tools/playtest.sh                  # title screen, the install's save
tools/playtest.sh aerial_garden    # straight into a level (the game's "-lvl" name)
```

- It runs the newest PGO build of `dev` under `~/.cache/kyty-builds` that has the recorder
  (`--install DIR` picks another), with `run.sh`'s flags (`--amd-cpu`, the warm shader store,
  Mailbox at 60 Hz vblanks, 1920x1080) and the default thread placement, so what is played is
  what the bench measures. The performance overlay stays hidden (F2 shows it); the recorder counts
  without it.
- If an agent benchmark or build holds the bench lock, it waits for it: queued at priority 1
  (ahead of every queued agent job) for at most 30 minutes, printing the elapsed time and the
  holder every 30 s (Ctrl+C cancels). It then holds the lock for the session, so no agent
  benchmark runs while you play (`--no-lock` doesn't take it). It refuses only when your own
  emulator is running (one not started from an agent worktree or the build cache) or another
  playtest session is.
- The shader store is per install, so a new build starts cold: `tools/playtest-store.py` computes
  the build's store key from git (as `shader_store_key.cmake` does) and, when the install's store
  is missing, from another build or under half the size of the largest same-key store in the build
  cache, copies that one in; with none it says to expect a long first load.
- At exit it converts the screenshots to PNG, prints the one-page report and the session dir.
- Saves are the install's own `_SaveData` (seeded from the play checkout when the install was
  built).

**The marker**, for anything that looks or feels wrong: **F9**, or the **PS button** or the
**DualSense microphone button**. Games can't read either button and the default mapping leaves
them unbound. A green "Marker N saved" pill in the top-left corner confirms it for 2.5 s. The
marker saves a screenshot of the guest image (without the pill), the last 10 s of frames and
events, and a number.

## What is recorded

Per guest frame (the command thread, once per guest flip, into a ring only it writes): frame
time, the presented flip count, the command thread's CPU, user and kernel time, faults and
context switches, its waits (no guest work, blocked submissions, GPU, render-context mutex,
prepare workers, recorder), garbage collection and write-collection time, draws, dispatches,
submits, shader and pipeline compiles and their time, texture and buffer uploads, downloads,
creations, frees and evictions, rejected flips, late vblanks, the deepest flip queue, guest file
reads, audio underruns, unresolved-import calls, emulated instructions, write faults.

Per presented flip: the interval.

Pad input, at the pad HLE read (`PadRead`/`PadReadState`): each state the game received that
differs from the previous one (and one every 600 frames as a checkpoint), with the guest frame and
the frame since the last `StartLevel` (`lg`), in `input.jsonl`: buttons, sticks, triggers and the
whole `PadData` as hex (touch and motion included), for replay.

Events (per-thread rings): shader compiles (all are shader-store misses at run time) with the
hash, stage and time; pipeline creations with their time; texture uploads of 1 MiB or more with
size, format and mips; buffer uploads of 4 MiB or more with the staging-copy time; collections of
0.5 ms or more; the first call of each unresolved import; audio underruns; level starts (the
game's `StartLevel <name>` and `Level has started: <name>` lines); markers; injected faults.

Outside load, every 100 ms, in the emulator (`reserved_cpus.csv`): the busy % of the CPUs the
layout leaves idle on purpose (the SMT siblings of the command thread's and prepare workers' CPUs,
and `KYTY_RESERVE_CPUS`, that no pinned emulator thread runs on: CPUs 9 and 11 here). A slow frame during which one of them
was 25% busy or more, and whose emulator-side top cause is only what sibling contention looks like
from inside (slower command-thread CPU work, off-CPU time, waits on the workers or the recorder,
or nothing), is tagged `outside_load` and its cause is "outside load (host task on a
reserved core)"; a compile, upload, GC, GPU wait, collection or mutex wait still names itself: host work there (a fan daemon's 47 ms poll every 2 s, app bursts) slows the
command thread and workers without being the emulator's doing (FINDINGS, "outside load").

System, every 2 s, read-only (`tools/playtest-sample.py`, nice 19): per-core busy % and MHz,
temperatures, the emulator's RSS and named threads with their CPUs, other processes that ran on
those CPUs or their SMT siblings, and VRAM, GPU load, temperature and clock (`nvidia-smi`). The
report flags samples with the CPU package at its critical temperature (where it throttles) and the
clock of the emulator's cores then: an outside cause (cooling).

`tools/playtest-check.sh` runs the user's launch end to end from a clean environment (`env -i`, no
`KYTY_*`, no lock held), to the level start and 20 s in, stops the emulator and checks the session;
`--hold shared|exclusive` first holds the lock as an agent build or benchmark would, and
`--user-emulator` checks the refusal. Re-run it after any change to `playtest.sh` or `benchlock.sh`.

## Triggers and incidents

An incident opens on: a guest frame over 33.4 ms; two over 25 ms within a second; under 57 guest
frames in the last second; no guest flip for 2 s; the first call of an unresolved import; a fatal
error. Triggers within 3 s of the last one join the open incident (a bad stretch is one record,
closed 3 s after its last trigger or after 30 s). At most 300 incident files; past that, one line
each. A screenshot is taken at most once every 10 s.

Each incident file (`incidents/incident-NNNN.json`) has: wall time, session time, guest frame,
level and sub-level, phase (`boot`: before any level in the first minute, `load`: from `StartLevel` until 3 s
pass without a frame over 50 ms, at most 90 s, else `play`), its triggers, the worst frame and **its causes ranked by how much each
exceeds the window's usual frame** (median of the frames under 20 ms), a one-line summary such as

```
117 ms frame: +85 ms shader compiles (3 compile(s)), +20 ms GPU wait; uploads: 4096x4096 Bc7UnormBlock 21.3 MB
```

and the whole window (10 s before to 1.5 s after) of frames, events and present intervals.

Causes: shader compiles, pipeline creation, GPU wait, game late (no GPU work queued), prepare-
worker wait, recorder wait, render-context mutex, texture/buffer GC, write collection, large
upload copies, command thread off-CPU (blocked or preempted outside the known waits; voluntary
and involuntary switches and major faults in the detail), command thread CPU (draw work).

A fatal error (EXIT/assert, an unhandled SIGSEGV/SIGBUS/SIGILL, SIGABRT, SIGFPE,
`std::terminate`, a page-manager fatal) writes `crash.json` straight from the rings (no locks, no
allocation: the last ~10 s of frames and 15 s of events), then gives the helper thread up to 1.5 s
to close the open incident as `fatal` and flush its files.

## Session directory

`~/.cache/kyty-playtest/<timestamp>/` (`KYTY_PLAYTEST_DIR` overrides):

| File | What |
|---|---|
| `launch.txt` | install, build mode and commit, flags, env, start/end, exit status |
| `session.json` | the emulator's view: git hash, build label, BUILD_MODE, argv, `KYTY_*`/`SDL_*`/`VK_*` env |
| `emulator.log` | the emulator's stdout/stderr (the startup configuration report lands here) |
| `frames.csv`, `present.csv` | every guest frame and presented flip |
| `input.jsonl` | pad states the game read, when they change |
| `events.jsonl` | every event and one line per incident and marker |
| `incidents/` | `incident-NNNN.json`, `marker-NNN.json` |
| `shots/` | `incident-NNNN.png`, `marker-NNN.png` |
| `system.jsonl` | the 2 s system samples |
| `reserved_cpus.csv` | the reserved-idle CPUs' busy %, every 100 ms |
| `crash.json` | only after a fatal error |
| `session_end.json` | end time, counts, unresolved imports with their call counts |
| `report.json` | `playtest-report.py`'s data |

## Reading it

```
tools/playtest-report.py <session dir> [--json] [--all]
```

One page: length and how it ended, FPS per second of play (load seconds apart), presented frames
at two and three refreshes, long frames, memory growth (MB/min), incidents grouped by primary cause
with count, time lost and the worst one, markers with screenshots and the last 10 s, unresolved
imports, other processes on the emulator's cores, outside-load incidents apart from the emulator's
(with how many fall on a ~2 s period and the host processes seen near them), and the top three
things to fix. Each cause
group names the targeted diagnostic run that explains it further (e.g. `KYTY_GPU_WAIT_DIAG=1` for
GPU waits, `KYTY_FRAME_DIAG=1` for a late game, `KYTY_OFFCPU_SAMPLE=1` for off-CPU time), as a
`levels.sh` command for the incident's level, so a follow-up never needs the user.

## Cost

Off: one static flag test per hook. On: per guest frame the command thread reads its counters and
makes two system calls (its CPU clock and `getrusage`); events are rare (compiles, uploads of 1 MiB
or more, slow GCs). Nothing runs per draw. All files are written by one helper thread at nice 10
on the efficiency cores. Measured cost: FINDINGS, "Playtest flight recorder".

## Fault injection (for proving the attribution)

`KYTY_PLAYTEST_INJECT="sleep:<f>:<ms>,miss:<f>[:<n>],upload:<f>:<MiB>,exit:<f>,segv:<f>"`, `<f>`
counted in guest frames from the first `StartLevel`:

- `sleep`: the command thread sleeps `<ms>` after that guest flip;
- `miss`: the next `<n>` (default 1) shader lookups on the command thread miss and compile again
  (the modules are not stored);
- `upload`: the largest recently used sampled textures, `<MiB>` in all, are treated as written by
  the CPU, so they upload again from (unchanged) guest memory;
- `exit`: an `EXIT` (the fatal-error path of every assert) on the command thread;
- `segv`: a write to an inaccessible page on the command thread (an unhandled SIGSEGV).

Each injection is an `inject` event in the window, so a reader can tell; the attribution does not
read it.
