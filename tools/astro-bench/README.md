# Astro Bot benchmark harness

Drives Astro Bot (PPSA21567) into Sky Garden with a virtual gamepad and reports FPS — repeatable
A/B measurements of the GPU command thread under the heaviest draw load in the game.

Requirements: Linux, `python-evdev`, write access to `/dev/uinput` (uaccess), the game at
`~/Games/PS5/PPSA21567.zar` (or `KYTY_GAME`), and a save in slot 1 parked on the galaxy map before
the Gorilla Nebula (copy one with `setup-install.sh`).

```bash
python3 -I tools/astro-bench/vpad.py &          # virtual X360 pad on /tmp/vpad.fifo (leave running)
tools/astro-bench/setup-install.sh              # once per build dir: save data + warm shader cache
tools/astro-bench/garden.sh mylabel --amd-cpu   # one run → "mylabel: fps=… draws/frame=… …"
printf '%s\n' "a1 -- --amd-cpu" "b1 KYTY_NO_PIN=1 -- --amd-cpu" | tools/astro-bench/batch.sh
cat /tmp/bench-batch.txt                        # batch results (logs: /tmp/bench-<label>.log)
echo quit > /tmp/vpad.fifo                      # remove the virtual pad when done
```

- `run.sh` — launches the emulator windowed (1920×1080), muted, DualSense hidden, `KYTY_PERF_LOG=1`.
- `garden.sh <label> [flags]` — boot → title → save 1 → Gorilla Nebula → Sky Garden (Dive In is a
  *hold* of Cross) → averages `PERF` lines over `GARDEN_WINDOW` s (45) after `GARDEN_SETTLE` s (25).
  `KEEP_RUNNING=1` leaves the emulator in the level for profiling.
- `batch.sh` — runs `<label> [ENV=VAL…] -- [flags…]` lines back to back.
- `symbolize.py`, `chains.py` — map perf addresses of this stripped PIE through
  `_Build/linux/kyty_emulator_clang_lld.map` (perf report prints file offsets; vaddr = offset +
  0x1000; for `perf script` addresses subtract the load base from the r-xp mapping).
- `faultsum.py` — summarise `KYTY_FAULT_DIAG=1` output. `offcpu.py` — off-CPU time from
  `perf record -e sched:sched_switch` (needs root for tracepoints).
- `patches/` — emk530/issue #745 game patches (NoRT, GI probes) for `--game-patch`; no CPU gain.

Never run the harness while someone is playing on the same machine: it takes over the gamepad
slot and kills `kyty_emulator` processes it started.
