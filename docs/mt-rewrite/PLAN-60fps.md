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
| B2 | Memoize texture resolve per stage image slot: same T# words and no image registrations on its pages since → reuse (done, +6.2%; B2b: color and depth targets the same way, +3.8%) | `ResolveTexture` < 4% (now ~10% with its hits' lock and copies; a second way per slot did not help, see FINDINGS) |
| B1′ | Make the walk's remainder cheap instead of skipping the walk. The JIT walker (12% of the thread) is instruction-fetch bound (35% of the thread's L1i misses): inlining single-use values did not help (step 2, FINDINGS), it needs less code per walk; the rest of `MaterializeResources` (9%) and `PrepareProgram` (5%) need their per-draw allocations and copies cut. Separately, stop zeroing and copying ~1 KB `ImageDesc`s per draw (~7% of the thread in memset/memcpy; done in step 1, +6.0%: the draw-state memset fell from 2.2% to 0.1% of the thread, one ~1 KB copy per texture binding remains). | `GetGraphicsPrograms` ≤ 12% of the thread |
| B3 | **Side-effect-free preparation**, so a worker can prepare draw N+1 while the command thread records draw N. Writes made while a draw's programs are looked up and materialized today, each to remove or confine: (a) the shared `Program::resources`/`specialization` and `m_lookup_key` (done in B3a: lookups write caller-owned `StageResources`; modules have stable addresses); (b) image-heap acquisition inside materialization (host texture state); (c) `m_program_cache`/`m_tess_layouts` inserts and module compiles on a miss (shared with compute lookups on the command thread); (d) per-plan walker scratch (`srt_scratch`, `material_keys`, `selector_keys`) (done in B3b: the prepare thread walks in its own per-plan scratch); (e) guest reads that need the command thread (`ShaderReadGuest` checks `IsGpuThread`; GPU-dirty walker loads sync through it); (f) call-source programs, which rewrite their plan on lookup. Then an immutable `PreparedDraw` (programs, stage resources, render-state inputs) is the hand-off. | preparation touches only its own output and read-only caches |
| B4 | **Threaded prepare** (done, the snapshot design instead of the scout): each queued draw carries a copy of the registers; one worker on a performance core of its own runs the program lookups (walk, materialize, module select) up to 4 draws ahead while the command thread commits the oldest from its copy. The command thread adopts a lookup unless the worker failed (it only finds), GPU-written bytes appeared or the emulator stored into guest memory meanwhile, or a backing-view read is GPU-written; then it looks up itself. `KYTY_PREPARE_VERIFY=1` compares. +18.8% (6.53 → 5.47 µs/draw) | ≤ 2 µs/draw with the scout keeping up |

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
| C3 | (lead, done) Side task: how Astro Bot derives its frame delta (upstream #1931). Does any emulator-side timing make the slowdown worse than the FPS alone explains? Don't fake timing. | written up in FINDINGS |

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
| A | A1 done (#7, #8); A2 done; A3 measured: transfers end only ~10 passes/frame; A4 done (#10); A5: redundant binds, queries and same-target pass breaks ruled out on the GPU; recording cost measured (~10% of the thread): bind/push dedupe (#15, +1.2%) and a recording thread (+~2%) landed; in-pass GPU time is vertex shading, 53% of it in draws that switch shaders (not state) | GPU ~17 ms/frame (9.8 in passes: ~1.6 fragment, 0.7 per-draw state, the rest vertex work; 3.6 dispatches); indirect-dispatch syncs 0 |
| B | Async guest readbacks: the command thread records a guest read fault's download and moves on (its GPU wait in readbacks 9% → 0%), +7.4%; it is now ~98% busy, so FPS is its per-draw cost. B4 merged (threaded prepare, +18.8%, 5.47 µs/draw; worker 53% busy, so one worker keeps up). Core layout: game threads on the efficiency cores, +3.1%, worst frame 33.5 ms. Next for B4's owner: the commit side, which now bounds the pipeline (texture resolve and its validation ~13%, memcpy ~12%, buffer cache ~7%). B1′ (single-thread cuts; B4 went to its own agent): step 1, draw struct copies, +6.0% (6.71 µs/draw); step 2, walker inlining, no gain; step 3a, vertex-decode lookups, +0.5%; walker references skip known values (+2.8% before B4; on B4 the prepare worker 2.98 → 2.80 µs/draw, FPS unchanged since the commit side limits). Walker work now only buys worker headroom. Done: B0, B2 +6.2%, B2b +3.8%, B3a, B3b; B1 dropped, B2c ruled out | texture resolve 16.7% → 8.7%, render-target resolve 10.8% → 5.6% of the GPU thread; B3 inventory in the B3 row |
| C | C1a merged (#6, +5.7%). C2 merged (#19): hot chunks under asynchronous write protection, waits on the command processor's own labels proven and skipped; verifier clean (0 lost writes in 67.5M checks); +2.2% FPS, process CPU 688% → 565%. VRSQRTPS traps measured: ≤ +1.7% ceiling, ~0.8% reachable only through a tail-call heuristic, parked (FINDINGS). Remaining Track C cost is on the GPU thread (ObtainBuffer ~7%, PrepareBda ~3%), overlapping Track B. C3 moved to the lead | C0 in FINDINGS: guest threads fit with room once faults are gone, no Track D. C1 import deferred: labels are written before the host GPU reads. Next: the 1,088 trapped VRSQRTPS sites (the other guest-thread signal storm), then the rest of the GPU thread's buffer-cache path |
| Lead | C3 done: the game times itself with the process time counter; real speed = every flip 1 vblank apart (`KYTY_TIME_DIAG`, `flip_vbl`) | `dev` 458607ad: 24.2–24.4 FPS, frames at 2 or 3 vblanks |
| G | GPU execution time (shader codegen, tiler). G0: compute suite unblocked (#24). G1: `KYTY_GPU_TIME=draws`, `KYTY_SPV_DUMP` (#28). G2: vertex EXEC fold (#29), GPU 16.97 → 16.13 ms/frame, +2.8% FPS. G3: time outside passes split (transfers 2.8, barrier gaps 0.85 ms/frame) and dispatches by shader. G4: render targets tiled and detiled straight from and into the image (#34), GPU 16.26 → 14.89 ms/frame (transfers 2.71 → 1.34). G5: masked EXEC selects in vertex shaders, GPU −0.17 ms/frame (in passes 8.94 → 8.78), top VS 1,848 → 826 selects (#36). G6: `KYTY_PIPELINE_STATS`; indexed VGPR reads as a switch on M0, in passes −0.40 ms/frame (skinned VS family 219 registers, 22 × 116-way select chains) (#39). G7: masked selects for compute and pixel shaders (#42), GPU −0.5 ms/frame (dispatches −0.3). Measured, not landed: masked selects for mesh shaders (within noise). Buffer copies (~1.0 ms/frame) are `PrepareBda` uploads (Track C). Ruled out: per-lane EXEC branches; fused legacy MAD parked (−0.4 ms for a rounding change). Ceilings measured: compute lane mapping −0.4 ms, in-region EXEC selects −0.47 ms. Next: the indexed VGPR range's register pressure (local array, ~0.5 ms ceiling), the 0.6 ms compute dispatch `0ec2f135`; fewer upload copies and barriers is Track C's (~1.0 ms) | GPU ~13.9 ms/frame (8.2 in passes, 3.4 dispatches, ~2.2 between) |
| G3 | Compute dispatches and their barriers. Done: dispatches the guest lets overlap share their barriers (#45): GPU 14.14 → 13.29 ms/frame, dispatches 3.68 → 3.04; with #42 (masked selects for compute, Track G) dispatches are 2.79 ms/frame. The dispatches left unchained wait on the guest's own `WAIT_REG_MEM`/CS_PARTIAL_FLUSH. Ruled out: run-by-run half emission, image accesses in masked selects, same-half DPP shuffles. Left: `0ec2f135` (0.53 ms, 168 registers, spills), whose remaining lever is the parked one-lane-per-invocation mapping | GPU ~12.9 ms/frame (8.2 in passes, 2.8 dispatches) |
| G4 | GPU time outside shader math. Uploads ≥ 256 KiB cross PCIe on the copy engine into a device-local ring, the universal queue copies from there (#48): GPU 13.04 → 12.42 ms/frame (`KYTY_GPU_TIME=1`), transfers 1.35 → 0.58. `PrepareBda`'s uploads share one barrier pair: GPU 12.30 → 12.19, barrier gaps 0.53 → 0.45. Barrier gaps attributed by site (`GPUGAPSITE`): the rest is mostly guest-ordered dispatches (G3) and draw/DMA hazards. Ruled out: ReBAR staging (−1 FPS), attachment accesses on their own stages (no change). Skinned vertex shaders: an unsafe cap of the indexed-read candidates at 16 measured GPU −0.57 ms/frame (213 → 168 registers); `M0` is bounded only by per-vertex data, so the cap has no correctness proof, and exact versions (multiple-of-4 pruning, a local array) are unmeasured; parked as a project decision (FINDINGS). Done | GPU ~12.2 ms/frame |
