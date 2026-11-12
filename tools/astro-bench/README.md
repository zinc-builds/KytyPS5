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
  It refuses to start when this checkout's `_Build/linux/kyty_emulator` is newer than the installed
  one (you built but didn't `cmake --install`); `KYTY_INSTALL=<dir>` runs another install.
- `garden.sh <label> [flags]` — gets into Sky Garden, then measures **game frames 1000–1600 after
  level start** (`GARDEN_FRAMES=<first>-<last>`): fps is frames over the seconds between them, and
  the other columns average the `PERF` lines in between. The game is frame-locked and the opening
  flight is scripted, so a frame range is the same scene in every run whatever the build's speed
  (a time window is not), and runs finish sooner as the emulator gets faster. Four identical runs
  spread 0.17 FPS (sd 0.07) this way, against 1.15 (sd 0.48) with the old window, so **one run per
  configuration is enough**. Binaries whose `PERF` lines lack `frames=`/`t=`, or `GARDEN_WINDOW`
  set, use the old time window: `GARDEN_WINDOW` s (45) after `GARDEN_SETTLE` s (25).
  `KEEP_RUNNING=1` leaves the emulator in the level for profiling. Two routes (`GARDEN_ROUTE`):
  - `lvl` (default): boots straight into the level, ~14 s to `StartLevel` instead of ~55 s. The
    game is a developer build that reads its boot arguments from `/app0/args.txt`; `garden.sh`
    adds the game's own `-lvl aerial_garden` through `KYTY_APP0_OVERLAY` (a directory whose files
    replace `/app0` files). No vpad or save needed. A binary built before 45d400a8 (no overlay)
    boots to the title and the run fails: A/B against such a reference with `GARDEN_ROUTE=menu`.
  - `menu`: title → save 1 → Gorilla Nebula → Sky Garden (Dive In is a *hold* of Cross), driven
    by `vpad.py`. Needs the save. Its numbers are not comparable with `lvl` runs (FINDINGS.md).
  `GARDEN_LEVEL=<name>` boots another level on the `lvl` route: a `<File>` name from the dump's
  `data/prein/product_levels.xml` (e.g. `timestop_casino`; `tools/re/zx.cpp` extracts it). The game
  logs `StartLevel` with a short name (`snowy_canyon`), so the first `StartLevel` counts. Runs of
  another level stay out of the result store (its references are Sky Garden's).
- `levels.sh [-m verify|perf] [-f A-B] [-s F] [-e ENV] [-t TAG] [-l FILE] <level>...` — one
  `garden.sh` run per level in one hold of the lock, then a table (loaded level, result or the
  failure with the log's last error line, FPS, draws/frame, `cmd_ns`, worst frame, screenshot).
  `-m verify` (default): every verifier that exits on a difference, frames 0–400, a screenshot at
  frame 300. `-m perf`: frames 200–800. `levels-spread.txt` is a spread of eight levels.
- `benchlock.sh` — host-wide lock so agents sharing the machine never overlap. `garden.sh`,
  `run.sh` and `batch.sh` take it exclusively (the emulator inherits it, so `KEEP_RUNNING=1` holds
  it until the emulator exits); wrap other emulator launches in `benchlock.sh run …` and builds or
  unit tests in `benchlock.sh build …` (shared). `benchlock.sh status` shows the holder.
- `batch.sh` — runs `<label> [ENV=VAL…] -- [flags…]` lines back to back, after a short unreported
  warm-up run of the first line (the first run of a batch reads slow; `BATCH_WARMUP=0` skips it).
- `refbuild.sh <commit>` — prints the install dir of a Release build of that commit, building it
  once into `~/.cache/kyty-builds/<sha>/` for every checkout to share. Use it for A/B reference
  builds instead of building your merge-base yourself, and call it before `batch.sh`, not inside:
  `ref=$(tools/astro-bench/refbuild.sh "$(git merge-base HEAD origin/dev)")`, then a batch line
  `ref1 KYTY_INSTALL=$ref -- --amd-cpu`.
- **Build modes** (`KYTY_BUILD_MODE`, `buildmode.sh`): `plain` (default), `lto` (ThinLTO),
  `pgo` (ThinLTO + PGO with `KYTY_PGO_PROFILE`, the fast build: ~+11% FPS) and `pgogen`
  (instrumented). `refbuild.sh` keys its cache on the mode and the profile's hash
  (`<sha>-pgo-<hash12>/`), so plain and PGO installs never mix. A/B protocol, retrain rule and
  measured times: "Build modes" in `docs/mt-rewrite/PLAN-60fps.md`.
  - `build.sh [target]` — builds this checkout in the mode into `_Build/linux` (plain) or
    `_Build/linux-<mode>`, installs it there (`…/install`, save and shader cache copied in) and
    prints the install dir: the other side of a `refbuild.sh` A/B.
  - `pgo-train.sh [commit]` — trains a profile for a commit (default `origin/dev`): instrumented
    refbuild, a warm-up run and a `KYTY_PGO_DUMP` garden run under one exclusive hold, then
    `llvm-profdata merge` into `~/.cache/kyty-pgo/<sha>.profdata` (printed). One trainer per
    commit; `current.profdata` follows `origin/dev`.
- Builds use **ccache** (`/usr/bin/ccache`, shared cache with `base_dir = /home/tomm` in
  `~/.config/ccache/ccache.conf`, so worktrees under `$HOME` share entries; a full build drops from
  ~107 s to ~28 s on hits). A build dir configured before ccache was installed cached
  `CCACHE_PROGRAM-NOTFOUND`: reconfigure once with `cmake -S . -B _Build/linux
  -DCCACHE_PROGRAM=/usr/bin/ccache`.
- `symbolize.py`, `chains.py` — map perf addresses of this stripped PIE through
  `_Build/linux/kyty_emulator_clang_lld.map` (perf report prints file offsets; vaddr = offset +
  0x1000; for `perf script` runtime addresses pass the load base = r-xp mapping start − (its file
  offset + 0x1000), e.g. from `/proc/<pid>/maps` or `perf script --show-mmap-events`).
- `kstats.py <label>` — the GPU command thread's kernel time by call site over the frame window,
  from a run with `KYTY_KERNEL_STATS=1`.
- `framediag.py <bench log> [A-B …]` — summarise `KYTY_FRAME_DIAG=1` lines: refresh and wall
  histograms, fast vs slow frames field by field, the worst 20 frames. `framemodel.py <bench log>
  [A-B]` — replay the guest frames against 60 Hz vblanks: causes of slow frames and what-ifs
  (FINDINGS, "Frame pacing").
- `faultsum.py` — summarise `KYTY_FAULT_DIAG=1` output. `offcpu.py` — off-CPU time from
  `perf record -e sched:sched_switch` (needs root for tracepoints).
- `../re/gamespeed.py [bench logs…]` — **game-speed check**. Astro Bot advances a fixed step per
  flip, so game speed = flips/s ÷ 60. The script finds the game's own `veg_chibi … 6->7` event
  (~850 flips after `StartLevel` in every run, ~14.2 s on a PS5) and prints the game speed over
  the opening, over frames 1000–1600, and the `FRAMESKIP` summary if present. Runs on any
  `/tmp/bench-<label>.log` (default: all of them). `tools/re/` also has the eboot helpers
  (`.zar` extractor, SELF → flat image, import and caller lookups); see its README.
- `patches/` — emk530/issue #745 game patches (NoRT, GI probes) for `--game-patch`; no CPU gain.

Never run the harness while someone is playing on the same machine: it takes over the gamepad
slot and kills `kyty_emulator` processes it started.
