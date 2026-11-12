# Frame skipping (`--frame-skip auto`)

An optional mode, **off by default**, for scenes or machines where the emulator cannot keep up
with the display. Games like Astro Bot advance their simulation by a fixed step per flip, so
every vblank the emulator misses makes the game run slower (at 50 FPS, Astro Bot runs at 0.83×).
With frame skipping on, the emulator now and then drops the *drawing* of a frame but still runs
everything else in it, so the game gets its flip on every vblank and keeps its speed; the screen
shows fewer distinct images instead.

The project's goal stays 60 FPS at full speed with the mode off. Frame skipping is a fallback.

```bash
kyty_emulator --game … --frame-skip auto      # or --frame-skip=auto; KYTY_FRAME_SKIP=auto
```

`KYTY_FRAME_SKIP` (`auto`, `off`, or `n` ≥ 2 to skip every n-th frame for testing) overrides the
flag. The performance overlay (`--perf-overlay`) shows frames shown and skipped per second and the
worst gap between shown images, in vblanks; its FPS keeps counting flips (game ticks).

## When it skips

Only when behind, never two frames in a row, never across a stall (a frame longer than four
vblank periods: loading, shader compiles). At every flip the GPU command thread:

1. measures its own CPU time for the frame just finished, separately for drawn and skipped frames;
2. computes the share of frames to skip so the average fits the vblank period with 5% margin:
   `(1 − f)·drawn + f·skipped ≤ 0.95·period`, capped at 50%; zero when a drawn frame fits;
3. adds feedback: each vblank that passed without a flip (a lost game tick) asks for a skip soon
   and raises the share for a while (it decays with a ~70-frame half-life);
4. spaces skips evenly by accumulating the share.

## What a skipped frame drops, and why that is safe

A skipped frame still runs: register and context state, compute dispatches, DMA and copies,
label writes, interrupts and end-of-pipe events, waits, the flip (on its vblank, presenting no new
image), occlusion-query dumps. The game sees exactly the same command stream completing.

It drops draws, PSSR passes, and the presentation of its image, **except** draws into:

| Kept | Why | How it is found |
|---|---|---|
| Render targets not redrawn every frame (shadow caches, probes, impostors) | Later frames reuse them without redrawing; a skipped refresh would stay stale until the next one | Each target's write rate over drawn frames; below 90% it is kept |
| Targets of draws that write buffers or storage images | Their writes outlive the frame (the CPU or a later pass may read them) | The draw commit reports shaders with buffer/image writes |
| Targets the CPU reads back (`--readback-tiled-images`, `--readback-linear-images`) | The texture cache writes them back to guest memory after each submit | The same size/tiling rule as the texture cache |
| Draws with no render target | They exist only for their side effects | No color or depth target bound |
| `KYTY_FRAME_SKIP_KEEP=<addr>,…` | Manual: a target base address or pixel-shader address | Found with the diagnostic below |

Video-out buffers are never kept: they are only scanned out, and a skipped frame does not present.
Occlusion queries: a dropped draw inside an open query counts one visible sample, so objects are
not culled because their draw was skipped (without `--occlusion-queries` every query already reads
as visible). New targets are kept until ~45 drawn frames have shown them to be every-frame ones.

What can still differ after a skipped frame: temporal effects (TAA or PSSR history) see one frame
less of history, and anything computed in a skipped frame from a render target (a depth pyramid
for GPU culling, a luminance average) uses the last drawn frame's content.

## Diagnostic: `KYTY_FRAME_SKIP_DIAG=1`

Works with the mode on or off. Every 600 flips it prints an `FSDIAG` summary (draws, indirect and
GPU-count indirect draws per frame, untargeted draws, draws with buffer or image writes, CPU read
faults on GPU-written memory) and one `FSDIAG target=` line per render target with its size,
format, class (`every-frame`, `periodic`, `side-effects`, `cpu-readback`, `keep-list`, `display`),
write rate and draws per written frame. Targets classed anything but `every-frame` and `display`
are kept in skipped frames. `KYTY_FRAME_SKIP_LOG=1` prints a `FRAMESKIP` line per second (flips,
shown and skipped frames, missed vblanks, dropped and kept draws, gaps between shown images).

## Checking game speed

`tools/re/gamespeed.py /tmp/bench-<label>.log` (any `garden.sh` log) prints Astro Bot's game speed
from an event the game logs itself; see `tools/astro-bench/README.md`.
