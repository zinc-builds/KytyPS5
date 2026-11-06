# Plan: Sky Garden at 60 FPS

Status as of 2026-11-06: **~19.0 FPS** (`dev` 5864233b plus read-only depth in review). The target
is 60 FPS with stable pacing, which means 16.7 ms frames. This plan replaces "fix the top
profile entry" with three parallel tracks. Each track has an end-state architecture and a
per-frame budget. PRs stay small and measured, but each one moves toward its track's end state.

## Why small fixes alone won't reach 60

| Path | Now (per frame) | Budget for 60 | Gap |
|---|---|---|---|
| GPU command thread: draws (~5.7k × 7.7 µs) | ~44 ms | ≤ 11.5 ms (≤ 2.0 µs/draw) | 3.8× |
| GPU command thread: indirect-dispatch GPU syncs | ~5 ms | 0 | eliminate |
| GPU execution | ~23 ms | ≤ 13 ms | 1.8× |
| Guest CPU threads (game main thread, workers) | unmeasured | ≤ 14 ms | C0 measures it |

The draw cost is flat across stages: the SRT walk ~27%, texture resolve ~15–17%, stream copies
~12%, faults ~10%, the driver ~10%. A 5% fix to one stage can't close a 3.8× gap. The ambitious
part of this plan is **not redoing work that didn't change**:

- **Track B** reuses per-draw preparation across draws and frames.
- **Track C** stops copying guest memory the GPU could read in place.
- **Track A** issues barriers only where data actually flows.

Threading is the last step, for whatever per-draw work is left. On its own it caps out around
1.4× (`RESEARCH-threaded-draw.md` §3), and threading becomes much easier once preparation has
no side effects (B3).

## Track A: GPU execution layer

**Owner:** the existing GPU-thread agent.
**Owns:** `shaderResourceBarrier.*`, image layout/`Transit`, the barrier and render-pass logic in
`renderDraw.cpp`, the schedulers, the GPU diagnostics (`KYTY_GPU_TIME`, `KYTY_VK_COUNT`).
**End state:** a dependency-aware layer. It knows each access's semantics (atomic, plain read,
write), its ranges and its stages, and it emits barriers only at real consumption points.
Rendering stays open across compatible draws.

| Step | What | Done when |
|---|---|---|
| A1 | Atomic write batches (#4, merged). Read-only depth, and no barrier for read after read (in review). | merged |
| A2 | Classify every remaining barrier and render-pass break per frame by cause (diag counters) | the table is in FINDINGS |
| A3 | Move transfers out of render passes: record uploads and copies for a draw batch into a pre-pass command buffer, so uploads stop ending rendering | render-pass breaks/frame cut in half or more |
| A4 | Remove the indirect-dispatch GPU sync: the SRT walk reads two GPU-written dwords. Bind a proven descriptor superset, or resolve the selection on the GPU, so the CPU never waits | `DI:GetComputeProgram` syncs are 0 |
| A5 | Whatever A2 shows is left: layout transitions, clears, depth→color copies | GPU ≤ 13 ms/frame |

## Track B: draw state cache (the main CPU bet)

**Owner:** new agent.
**Owns:** `pipelineCache.*`, `ResourceMaterialization.*`, the resolve half of `descriptors.cpp`
(`ResolveTexture`, `FindTextureDescription`, `FindImage` lookups), texture-cache lookup paths.
**End state:** most draws in a frame match a prepared draw from an earlier frame or an earlier
draw. On a hit, the draw thread validates a small read set and goes straight to commit and
record. It skips the SRT walk, texture resolve and buffer discovery. Misses build an immutable
prepared draw, which a worker thread can produce later.

| Step | What | Done when / kill criterion |
|---|---|---|
| B0 | **Measure repetition** (done, FINDINGS "Draw state cache"). A user-SGPR-keyed walk memo hits 7–8%: buffer descriptors carry per-frame addresses. The module, samplers and images match the program's previous lookup 99%, 99.5% and 91% of the time. | killed B1 as planned |
| ~~B1~~ | ~~Memoize the SRT walk on user SGPRs, validated against the read set~~. Dropped: keys rarely repeat, and validating a ~50-load read set costs most of a walk. | — |
| B2 | Memoize texture resolve per stage image slot: same T# words and no image registrations on its pages since → reuse (done, +6.2%) | `ResolveTexture` < 4% (now 8.7%: the misses dominate; next, keep a few descriptors per slot) |
| B1′ | Make the walk's remainder cheap instead of skipping the walk: reuse the specialization and module when the specialization-relevant descriptor fields match the program's previous lookup (99.7%); memoize `PrepareProgram`'s static vertex input decode (4%). Then the JIT walker itself: cut per-load overhead in its load stub. | `GetGraphicsPrograms` ≤ 12% of the thread |
| B3 | Whole prepared-draw cache. Preparation becomes side-effect free (no writes to the shared `Program::resources`) and returns an immutable `PreparedDraw`; per-stage samplers, shader data and texture bindings are reused by slot. | ≤ 3 µs/draw |
| B4 | Prepare misses on a worker thread (prepare/commit split), if misses still matter after B3 | ≤ 2 µs/draw |

## Track C: guest memory without copies, and the guest side

**Owner:** new agent.
**Owns:** `bufferCache.*`, the page/write trackers, `PrepareBda`, stream buffers, `memory/`
(uffd, fault handling).
**End state:** the GPU reads CPU-written guest memory in place, or from a persistently mapped
mirror that only gets dirty ranges. No per-draw stream copies, and no write-fault storm.

| Step | What | Done when / kill criterion |
|---|---|---|
| C0 | **Measure (≤ 1 day):** stream-copy bytes and calls per frame (`ObtainBuffer`/`CopySparseMemory`), `PrepareBda` bytes and reprotects, fault rate per thread, and whether the guest's own threads (game main thread, workers) are near 16.7 ms/frame, using Tracy and `perf` on the guest threads | the numbers are in FINDINGS. If the guest threads are over budget, open a Track D. |
| C1 | **Host-memory import prototype:** `VK_EXT_external_memory_host` to wrap the guest backing memory (or the hot ranges of it) as Vulkan buffers. CPU-written constant and vertex data is then read in place, with no copy. Check NVIDIA's alignment rules, the GPU-side cost of PCIe reads, and coherency with the write tracker. | stream copies < 2% of the draw thread, without a GPU-time regression. Kill it if GPU time rises more than CPU time falls. |
| C1a | (re-plan from C0) Reuse stream copies of the same range within a guest-sync epoch, command buffer and ring lap; `KYTY_STREAM_VERIFY=1` | 81–90% of copies reused, 0 verify mismatches |
| C2 | Write-fault storm: stop re-protecting pages the game rewrites every frame (`PrepareBda`, ~280 MB/s). Track them as always-dirty, or snapshot them per frame instead. | kernel fault time < 3% of the draw thread |
| C3 | Side task: how Astro Bot derives its frame delta (upstream #1931). Does any emulator-side timing make the slowdown worse than the FPS alone explains? Don't fake timing. | written up in FINDINGS |

## Shared rules (all tracks)

- **One machine.** Anything that launches `kyty_emulator` takes the exclusive bench lock:
  `garden.sh`, `run.sh` and `batch.sh` do it themselves; anything else (perf, Tracy, nsys,
  screenshots) goes through `tools/astro-bench/benchlock.sh run …`, ideally by launching via
  `run.sh`. Builds and unit tests take the shared lock: `benchlock.sh build cmake --build … -j 8`.
  With `KEEP_RUNNING=1` your emulator holds the lock until it exits, so kill it as soon as you are
  done. `benchlock.sh status` shows the holder.
- **Your own checkout and build.** Each track works in its own worktree with its own `_Build/linux`
  (run `setup-install.sh` once per build dir). Never build, install or benchmark in another track's
  checkout or in the user's `~/AI/emu/KytyPS5`. One `vpad.py` serves everyone: start it only if
  `/tmp/vpad.fifo` is missing.
- **A/B in one batch.** `dev` moves under you. Compare against a reference build of your own
  merge-base, run in the same `batch.sh` (which holds the lock across both runs).
- **Correctness first:** screenshots compared with a `dev` build, the touched unit tests (one suite
  at a time, under `MemoryMax=10G`), and a verify mode for any cache. Review with sol-loop, then a
  small PR to `dev` and a squash merge.
- **Stay in your files.** If you must touch another track's files, keep the change minimal and say
  so in the PR. `renderDraw.cpp` is shared: A changes its barrier and render-pass sections, B adds
  its cache lookup around preparation.
- **Record progress.** Add a row to the FINDINGS.md progress table for every merged gain (on a
  conflict, rebase and keep both rows). Update your track's status line below.

## Status

| Track | Current step | Notes |
|---|---|---|
| A | A1 done (#7, #8); A2 done; A3 measured: transfers end only ~10 passes/frame; A4 done (this PR); next: GPU time inside passes (A5) | GPU ~18–20 ms/frame; indirect-dispatch syncs 0 |
| B | B1′ (B0 done: B1 dropped; B2 merged, +6.2%) | texture resolve 16.7% → 8.7% of the GPU thread; next: the walk's remainder |
| C | C1a merged (stream-copy reuse, +5.7%); next C2 (async write tracking) | C0 in FINDINGS. Guest threads 13–14 ms/frame, ~40% fault handling: no Track D. C1 import deferred: labels are written before the host GPU reads |
