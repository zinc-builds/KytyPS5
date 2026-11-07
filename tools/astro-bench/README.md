# Astro Bot benchmark harness

Drives Astro Bot (PPSA21567) into Sky Garden with a virtual gamepad and reports FPS — repeatable
A/B measurements of the GPU command thread under the heaviest draw load in the game.

Requirements: the game at `~/Games/PS5/PPSA21567.zar` (or `KYTY_GAME`). The `menu` route also
needs `python-evdev`, write access to `/dev/uinput` (uaccess) and a save in slot 1 parked on the
galaxy map before the Gorilla Nebula (copy one with `setup-install.sh`).

```bash
[ -p /tmp/vpad.fifo ] || python3 -I tools/astro-bench/vpad.py &  # menu route only: virtual pad (shared)
tools/astro-bench/setup-install.sh              # once per build dir: save data + warm shader cache
tools/astro-bench/garden.sh mylabel --amd-cpu   # one run → "mylabel: fps=… draws/frame=… …"
printf '%s\n' "a1 -- --amd-cpu" "b1 KYTY_NO_PIN=1 -- --amd-cpu" | tools/astro-bench/batch.sh
cat /tmp/bench-batch.txt                        # batch results (logs: /tmp/bench-<label>.log)
```

The pad is shared by every agent on the machine: don't send it `quit`. Always pass `--amd-cpu`;
without it ~1.7M SIGILL/s drop Sky Garden to ~10 FPS. `run.sh` starts
`_Build/linux/install/kyty_emulator`, not the freshly linked one: after each build, install or
copy `_Build/linux/kyty_emulator` (and its `.map`) there, or the run measures the old binary.

- `run.sh` — launches the emulator windowed (1920×1080), muted, DualSense hidden, `KYTY_PERF_LOG=1`.
- `garden.sh <label> [flags]` — gets into Sky Garden, then averages `PERF` lines over
  `GARDEN_WINDOW` s (45) after `GARDEN_SETTLE` s (25). `KEEP_RUNNING=1` leaves the emulator in the
  level for profiling. Two routes (`GARDEN_ROUTE`):
  - `lvl` (default): boots straight into the level, ~14 s to `StartLevel` instead of ~55 s. The
    game is a developer build that reads its boot arguments from `/app0/args.txt`; `garden.sh`
    adds the game's own `-lvl aerial_garden` through `KYTY_APP0_OVERLAY` (a directory whose files
    replace `/app0` files). No vpad or save needed. A binary built before 45d400a8 (no overlay)
    boots to the title and the run fails: A/B against such a reference with `GARDEN_ROUTE=menu`.
  - `menu`: title → save 1 → Gorilla Nebula → Sky Garden (Dive In is a *hold* of Cross), driven
    by `vpad.py`. Needs the save. Its numbers are not comparable with `lvl` runs (FINDINGS.md).
- `benchlock.sh` — host-wide lock so agents sharing the machine never overlap. `garden.sh`,
  `run.sh` and `batch.sh` take it exclusively (the emulator inherits it, so `KEEP_RUNNING=1` holds
  it until the emulator exits); wrap other emulator launches in `benchlock.sh run …` and builds or
  unit tests in `benchlock.sh build …` (shared). `benchlock.sh status` shows the holder.
- `batch.sh` — runs `<label> [ENV=VAL…] -- [flags…]` lines back to back.
- `symbolize.py`, `chains.py` — map perf addresses of this stripped PIE through
  `_Build/linux/kyty_emulator_clang_lld.map` (perf report prints file offsets; vaddr = offset +
  0x1000; for `perf script` runtime addresses pass the load base = r-xp mapping start − (its file
  offset + 0x1000), e.g. from `/proc/<pid>/maps` or `perf script --show-mmap-events`).
- `faultsum.py` — summarise `KYTY_FAULT_DIAG=1` output. `offcpu.py` — off-CPU time from
  `perf record -e sched:sched_switch` (needs root for tracepoints).
- `patches/` — emk530/issue #745 game patches (NoRT, GI probes) for `--game-patch`; no CPU gain.

Never run the harness while someone is playing on the same machine: it takes over the gamepad
slot and kills `kyty_emulator` processes it started.
