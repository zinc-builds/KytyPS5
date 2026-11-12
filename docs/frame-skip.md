# Frame skipping (`--frame-skip auto`)

An optional mode, **off by default**, for scenes or machines where the emulator cannot keep up
with the display. Games like Astro Bot advance their simulation by a fixed step per flip, so
every vblank the emulator misses makes the game run slower (at 49 FPS, Astro Bot runs at 0.82×).
With frame skipping on, the emulator now and then drops the *drawing* of a frame but still runs
everything else in it, so the game gets its flip on every vblank and keeps its speed; the screen
shows fewer distinct images instead.

The project's goal stays 60 FPS at full speed with the mode off. Frame skipping is a fallback.

```bash
kyty_emulator --game … --frame-skip auto      # or --frame-skip=auto; KYTY_FRAME_SKIP=auto
```

`KYTY_FRAME_SKIP` (`auto`, `off`, or `n` ≥ 2 to skip every n-th frame for testing) overrides the
flag. `KYTY_FRAME_SKIP_MAX=<1-4>` (default 1) is the most frames skipped in a row: on a slower
machine, 2 trades smoothness for full speed. The performance overlay (`--perf-overlay`) shows frames shown and skipped per second and the
worst gap between shown images, in vblanks; its FPS keeps counting flips (game ticks).

## When it skips

Only when behind, at most `KYTY_FRAME_SKIP_MAX` frames in a row (default 1), never across a stall (a frame longer than four
vblank periods: loading, shader compiles). At every flip the GPU command thread:

1. measures its own CPU time for the frame just finished, separately for drawn and skipped frames;
2. computes the share of frames to skip so the average fits the vblank period with 5% margin:
   `(1 − f)·drawn + f·skipped ≤ 0.95·period`, capped at max/(max+1) (50% by default); zero when
   a drawn frame fits;
3. adds feedback: each vblank that passed without a flip (a lost game tick) asks for a skip soon
   and raises the share for a while (it decays with a ~70-frame half-life);
4. spaces skips evenly by accumulating the share.

## What a skipped frame drops, and why that is safe

A skipped frame still runs: register and context state, compute dispatches, DMA and copies,
label writes, interrupts and end-of-pipe events, waits, the flip (on its vblank, presenting no new
image), occlusion-query dumps. The game sees exactly the same command stream completing.

It drops draws, PSSR passes, and the presentation of its image, **except** these draws (a draw
is kept if any of its color or depth attachments, or its shaders, call for it):

| Kept | Why | How it is found |
|---|---|---|
| Render targets refreshed every few frames (shadow caches, probes, impostors) | Later frames reuse them without redrawing; a skipped refresh would stay stale until the next one | Each target's write runs over drawn frames: three runs of one or two frames in a row make it periodic, until a run of 8 |
| New render targets (first 8 drawn frames) | A one-off render (a baked cube map) must not land in a skipped frame | First sight |
| Draws whose shaders write buffers or storage images | Their writes outlive the frame: the CPU or a later pass may read them | The draw commit reports the shader combination (ES/GS/PS addresses); later draws of it are kept |
| Shader combinations not yet drawn in a drawn frame | Their side effects are not known yet | Dropped only once the commit has seen them |
| Indirect draws | They patch shader registers that later draws inherit | Never dropped (3 a frame in Sky Garden) |
| Targets the CPU reads back (`--readback-tiled-images`, `--readback-linear-images`) | The texture cache writes them back to guest memory after each submit | The same size/tiling rule as the texture cache |
| Draws with no render target | They exist only for their side effects | No color or depth target bound |
| `KYTY_FRAME_SKIP_KEEP=<addr>,…` | Manual: a target base address or pixel-shader address | Found with the diagnostic below |

Video-out buffers are never kept: they are only scanned out, and a skipped frame does not present.
Occlusion queries: a dropped draw inside an open query counts one visible sample, so objects are
not culled because their draw was skipped (without `--occlusion-queries` every query already reads
as visible). A dropped draw still resolves a pending indirect instance count, so later draws see
the same register state as with the mode off.

A target is not "periodic" just because it goes unused for a while: dynamic resolution moves the
scene between differently sized targets, and each is redrawn every frame while in use.

What can still differ after a skipped frame: temporal effects (TAA or PSSR history) see one frame
less of history, and anything computed in a skipped frame from a render target (a depth pyramid
for GPU culling, a luminance average) uses the last drawn frame's content. A game's own GPU timing
sees skipped frames as cheap: Astro Bot's dynamic resolution then raises the resolution a little.

## Astro Bot, Sky Garden (KYTY_FRAME_SKIP_DIAG=1, 600-frame windows)

- **Render targets:** every target is redrawn in every frame except the two video-out buffers
  (alternating). No periodic targets, so nothing is kept for that reason.
- **Draws that write memory:** ~590 draws a frame (main pass, three pixel shaders) do atomic-only
  writes into three 128-byte buffers at `0x50740de00` (one per frame in flight), and the CPU reads
  them back (~1.3 read faults a frame on those pages): feedback the game consumes. ~15 draws a
  frame write two storage images (12 and 8 MiB), also read back. All are kept: a skipped frame
  still draws these ~620, and costs the command thread ~8 ms instead of ~22.
- **GPU-driven culling:** no multi-draw has a GPU-written count (3 plain indirect draws a frame),
  so no draw count depends on a skipped frame's depth.
- **CPU readback of render targets:** off by default (`--readback-*-images`), so the CPU never
  reads render-target memory; with it on, small tiled and linear targets are kept.
- **Occlusion queries:** without `--occlusion-queries` every query reads as visible; with it, a
  dropped draw inside a query counts as visible.
- **Untargeted draws:** ~30-50 a frame, kept.

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

## Measured on Astro Bot, Sky Garden (`dev` 891ff146, frames 1000–1600)

| | Flips/s (game speed) | Shown/s | Worst gap between shown images |
|---|---|---|---|
| Off | 49.4 (0.82×) | 49 | 1–3 vblanks |
| Auto, two runs | 57.4 / 58.8 (0.96× / 0.98×) | ~28 / ~29 | 3 vblanks (6–7 only in level-start stalls) |

Auto does not reach 1.0× at this base speed. A drawn frame costs the command thread ~22 ms. A
skipped frame still costs ~8 ms, most of it the ~620 draws it keeps for their feedback writes. With
one skip at most between drawn frames, the share sits at its 50% cap. The shortfall shrinks as the
base frame rate rises; `KYTY_FRAME_SKIP_MAX=2` trades smoothness for speed.
