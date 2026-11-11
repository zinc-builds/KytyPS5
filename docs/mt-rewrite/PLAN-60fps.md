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
| C1 | **Parked (2026-11-11; FINDINGS "C1 (in-place guest reads): parked").** Correct late labels cost 15% and the in-place prototype was FPS-flat. Was: **Host-memory import prototype:** `VK_EXT_external_memory_host` to wrap the guest backing memory (or the hot ranges of it) as Vulkan buffers. CPU-written constant and vertex data is then read in place, with no copy. Check NVIDIA's alignment rules, the GPU-side cost of PCIe reads, and coherency with the write tracker. | stream copies < 2% of the draw thread, without a GPU-time regression. Kill it if GPU time rises more than CPU time falls. |
| C1a | (re-plan from C0) Reuse stream copies of the same range within a guest-sync epoch, command buffer and ring lap; `KYTY_STREAM_VERIFY=1` | 81–90% of copies reused, 0 verify mismatches |
| C2 | Write-fault storm: stop re-protecting pages the game rewrites every frame (`PrepareBda`, ~280 MB/s). Track them as always-dirty, or snapshot them per frame instead. | kernel fault time < 3% of the draw thread |
| C3 | (lead, done) Side task: how Astro Bot derives its frame delta (upstream #1931). Does any emulator-side timing make the slowdown worse than the FPS alone explains? Don't fake timing. | written up in FINDINGS |

## Command-thread budget and the structural plan (2026-11-07, plan of record)

60 FPS needs every frame in one vblank: ~3.2 µs per draw on the GPU command thread against
~4.8 µs now, so about a third must come off. Small cuts do not add up to that; three structural
changes might.

Where the command thread's time goes on `dev` ab1429f1 (98% busy, 204k draws/s), from LBR call
stacks (`perf record --call-graph lbr --user-callchains`), each sample charged to the innermost
stage on its stack. Class: (a) inherent, stays on this thread in order; (b) movable to another
thread; (c) removable or cachable.

| Stage | ns/draw | Share | Class | Main parts |
|---|---|---|---|---|
| Texture and render-target resolve | 1,647 | 34% | b, part c | RefindExactImage 286, ResolveStageTexture 240, FindTextureDescription 124, GetSampler 85, MaterializeColorClear 85, FindView 79 |
| Draw flow, other | 715 | 15% | a, part b | ExecutePreparedDraw's own (inlined) code 422, PrepareBindings 117, DrawIndex 65, PostQueuedDraw 44 |
| Buffer acquisition | 668 | 14% | b | ObtainBuffer 282, SynchronizeBuffer 86, RebindBuffers 74, FindBuffers 65, HasGpuDirtyBytes 52, FindBuffer 46 |
| Pipeline and program lookups | 274 | 6% | b | GetGraphicsPipeline 156, compute programs |
| CommitBindings | 274 | 6% | a (push order), b (building) | |
| Prepare hand-off | 253 | 5% | c | Worker::Wait 202, 85% of it at packet drains |
| PrepareBda guest copy | 229 | 5% | a | snapshot, memory-bound |
| Scheduler | 198 | 4% | c | PopPendingOperations 156 (Track B fence unit) |
| PM4 decode and registers | 182 | 4% | a | |
| Recorder hand-off | 106 | 2% | a | |
| Image transitions | 86 | 2% | a | |
| Labels and events | 76 | 2% | a | |
| Compute other, PrepareBda rest | 72 | 1.5% | a | |

Submissions: the graphics queue takes 235 per second (~6 per frame, in order) and 94% of the
thread's time. The three compute queues used take 57 ms/s together, and two of them block on
graphics labels ~196 times/s each. A second command processor for compute could save at most
~6%, so it is ruled out.

Structural changes, in order:

1. **Buffer acquisition on the prepare worker (Track B4): phase A merged, +3.9%** (clean lookups and FindBuffers; ~1.84M hits/s, ~1 stale/s; buffer stage 668 → 519 ns/draw; worker 2.57 µs/draw, 51% busy, so item 2 is not needed yet). Phase B, stream copies on the worker (~90 ns/draw of copies plus the stream path), is next. The worker runs FindBuffers and
   ObtainBuffer for the draw. The commit adopts the result after one buffer-registration
   generation check and one CPU-dirty generation check per draw; anything else falls back. Stream
   copies move too: the worker runs after the draw's PM4 position and before any later
   guest-visible packet, so their snapshot holds. Requires a verify mode (worker result vs a fresh
   command-thread acquisition), a kill switch, and an A/B. Estimate −0.4 to −0.5 µs/draw. Risk:
   medium.
2. **A second prepare worker on a reserved performance core (Track B4): merged, +2.5%.** Draws
   alternate between two workers (CPUs 8 and 6), each with its own texture memo and walk
   scratch; the command thread's wait for the worker 7.7% → 2.5%, each worker 36% busy at
   3.10 µs/draw. Risk: medium.
3. **Texture and render-target resolve on the worker**, speculated and validated by the texture
   cache's registration generation and per-image settled state. Track B leads: it owns the lookups
   and the fence unit that provides lock-free lookups. Track B4 provides the prepared-draw format
   and the worker integration. Starts once B's fence unit and item 1 have landed. Estimate −0.9 to
   −1.2 µs/draw. Risk: high.

Status of the above (2026-11-08): item 1 phase B (stream copies on the worker) is paused, since
Track B's C1 (in-place guest reads) may make it obsolete. Item 2 is merged. Item 3 is merged in
parts: texture resolves (#62, #67) and render targets (#69 color, #71 depth).

### Budget refresh (2026-11-08, `dev` f465f33, after #72)

At 47.55 FPS and 249k draws/s the command thread costs **4.01 µs per draw** against ~3.17 for 60
FPS, so ~0.84 µs/draw must still come off. It is ~94% busy; `KYTY_STALL_DIAG` waits: prepare
workers 3.5%, synchronous commands 1.0%, recorder 0.6%, scheduler 0.6%. The two prepare workers
take 3.34 µs per draw each and are 40% busy, so they have room for more. Same method as above
(LBR call stacks, innermost stage); the 2026-11-07 figures for comparison:

| Stage | ns/draw | 2026-11-07 | Main parts |
|---|---|---|---|
| Draw flow, other | 957 | 715 | ExecutePreparedDraw's own code 331, PrepareBindings' own 262, DrawIndex 131, PostQueuedDraw 93 (35 the register copy), PrepareDrawRenderState 72 |
| Texture and render-target resolve | 771 | 1,647 | RefindExactImage 96, GetSampler 82, MaterializeColorClear 80, FindView 72, TouchImage 52, FindTexture 51 |
| Buffer acquisition | 564 | 668 | ObtainBuffer 273, RebindBuffers 72, HasGpuDirtyBytes 47, TouchBuffer 40 |
| CommitBindings | 302 | 274 | |
| Pipeline and program lookups | 280 | 274 | GetGraphicsPipeline 170, WalkSrt 52 (dispatches) |
| PrepareBda guest copy | 268 | 229 | 93% on the dispatch path |
| Prepare hand-off | 247 | 253 | Worker::Wait 178, almost all at packet drains (~3.5k/s) |
| PM4 decode and registers | 174 | 182 | |
| Image transitions, recorder, labels, scheduler | 88 / 87 / 85 / 83 | 86 / 106 / 76 / 198 | |
| Compute other, PrepareBda rest, other | 104 | 72 | |

Across stages, guest-memory copies (`TryReadFast`'s memcpy: `PrepareBda` and the stream
copies) are 374 ns/draw (9.3%).

A temporary counter showed that **76% of stage binding sets equal the previous draw's for the
same stage** (same module, worker texture records and sampler values): 69% of images, at 5.3
images per stage.

Next moves:

| Move | Owner | Estimate | Notes |
|---|---|---|---|
| A. **Merged, +3.4%** (~75% of stage sets reused). Stage binding-set reuse: a repeated set reuses the previous draw's images, samplers and views, checked by record identity and one global binding epoch (bumped by every image registration or unregistration, image recreate, sampler-cache eviction and view destroy); each image is still marked bound per draw and pending clears still apply | B4 | −350 to −420 ns/draw, up to −50 more if CommitBindings also skips building repeated sets | verify mode re-resolves everything and reports the first mismatch; kill switch |
| B. **Merged, +0.9%** (`GetGraphicsPipeline` ~162 → ~95 ns/draw; any added worker time costs the command thread, so the worker part had to stay small). Pipeline key built on the worker from the programs, the proposed targets and the registers; the command thread checks the final target formats | B5 | ~−120 ns/draw | adds a key/hash field to the prepared draw |
| Small cuts. **Merged, +0.9–1.3% draws/s**: serial-only reuse compare, lock-free `IsMetaCleared`, LRU touch skip within a GC tick, vertex-range merge on the worker. The `FindDepthTarget` depth+stencil fast path and a `MaterializeColorClear` memo followed from Track B: +1.8% (FINDINGS) | B4 | ~−40 to −50 ns/draw | kill switches `KYTY_META_LOCK=1`, `KYTY_LRU_MIRROR=0`; verify `KYTY_WORKER_BUFFERS=verify`, `KYTY_BINDING_REUSE_VERIFY` |
| Drain deferral, re-scoped by `KYTY_DRAIN_STATS` (2026-11-11): all drains ~2.1% of the command thread, not 6.7% (that came from `KYTY_STALL_DIAG` runs). Context-state NOP: **merged**, no longer drains (~0.24%). RELEASE_MEM (0.8%) becomes an ordered item after #70 (ranged label stores), with Track B. EVENT_WRITE/ACQUIRE_MEM/PFP_SYNC_ME (~0.8%) parked: their drain is what makes decode-time guest reads (SET_*_REG_INDIRECT) see queued draws' GPU writes | B4 | ~−10 ns/draw merged; ≤ −30 ns/draw left | kill switch `KYTY_CONTEXT_STATE_DRAIN=1` |
| Prepare latency (thin-commit roadmap step 1; gate: wait ≤ 80 ns/draw at queue depth ≥ 8, rejections < 300/s). Item 1, walker page log: **merged** (#87). 1b, ranged GPU-write hazard: **merged** (#88): rejections 265/s → ~1/s. Depth sweep: FPS flat at depths 4–16, default stays 4. Free lanes at depth 8: wait −80 ns/draw, cycles/draw −2.7% (same binary); being measured for a PR | B4 | wait ~100–140 → ≤ 80 ns/draw | kill switches `KYTY_WALKER_PAGE_LOG=0`, `KYTY_RANGED_GPU_HAZARD=0`; verify `KYTY_PREPARE_VERIFY` |
| Guest copies (C1, in-place guest reads): **parked** | Track B | – | it needs late labels: correctly ordered they cost 15% (binds read pending labels and CP writes as data), and the stage 2 prototype was flat on FPS (FINDINGS) |

A, B and the deferral would bring the thread to ~3.3 µs/draw; with C1 parked, the rest has to come from elsewhere.

## Thin serial commit roadmap (2026-11-11)

The goal is a design that does not depend on one fast pinned core: the command thread only
validates and hands off in order, the prepare workers do the per-draw work, and textures,
samplers and later buffers stay resident on the GPU. Estimated serial path ~4.0 → ~0.7–1.1 µs per
draw, so FPS then follows worker count up to about 3–4 workers. Design, ruled-out variants and
exactness risks: `RESEARCH-thin-commit.md` (its `file:line` references are for `dev` 85eb0cb6).

| Step | What | Owner | Gate |
|---|---|---|---|
| 0 | Stats: slow-path share, recorder ns/draw, stage 0 (time in the code stage 1 deletes) | Track B | numbers in FINDINGS |
| 1 | Latency: walker read logging, ranged hazards, depth sweep, free lanes, third worker | Track B4 | wait ≤ 80 ns/draw at depth ≥ 8; rejections < 300/s |
| 2 | Run-level targets, programs and pipeline | Track B | −270 and −150 ns/draw |
| 3 | Resident textures and samplers (stage 1; 1a is `KYTY_RESIDENT_TEXTURES=shadow`, then a 1b design review) | Track R | −150 ns/draw on PGO; verify clean |
| 4 | #78 revived, covering all per-draw uploads | Track B4 (later) | `ObtainBuffer` and uploads −150 ns/draw |
| 5 | Buffers by address (stage 2), the draw packet filled by the worker | Track R | no `pushDescriptorSet` on queued draws; `CommitBindings` ≤ 100 ns |
| 6 | Run-level bindings | unassigned | trivial after 5 |
| 7 | No direct recording on the draw path | unassigned | draw-path recorder drains at 0 |
| 8 | Parallel recording with secondary command buffers | unassigned | only if the recorder gates |
| 9 | Decode thread | unassigned | only if decode gates |

Worker time is not free (#77, #78, #82): under the latency co-limit later steps measure flat, so
step 1 comes first.

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
- **A/B with `ab.sh`, judged on instructions and cycles per draw.** `dev` moves under you: compare
  against your merge-base (`ab.sh` builds it with `refbuild.sh`). Decide on `ipd`/`cpd` (the
  command thread's instructions/cycles per draw), not FPS: see "Bench metrics and A/B" below.
  An FPS claim of record needs a same-batch pair in frames 1000–1600 (`gate.sh` runs it).
- **Correctness first:** `gate.sh` before merging (screenshots against the merge-base, the verify
  modes), the touched unit tests (one suite at a time, under `MemoryMax=10G`), and a verify mode
  for any cache. Review with sol-loop, then a small PR to `dev` and a squash merge; paste the gate
  block into the PR body.
- **Stay in your files.** If you must touch another track's files, keep the change minimal and say
  so in the PR. `renderDraw.cpp` is shared: A changes its barrier and render-pass sections, B adds
  its cache lookup around preparation.
- **Record progress.** Add a row to the FINDINGS.md progress table for every merged gain (on a
  conflict, rebase and keep both rows). Update your track's status line below.
- **Build modes.** Both sides of an A/B use the same `KYTY_BUILD_MODE` and, in `pgo` mode, the
  same profile file: pin it once with `export KYTY_PGO_PROFILE=$(readlink -e
  ~/.cache/kyty-pgo/current.profdata)` before `refbuild.sh` and `build.sh` (protocol below).

### Build modes: PGO + ThinLTO (Track P, 2026-11-11)

PGO + ThinLTO is worth +12% FPS on `dev` with no source change (plain 49.29 / 48.56, PGO+LTO
55.04 / 54.76, same batch). The tools build any commit or checkout in one of four modes
(`KYTY_BUILD_MODE`, `tools/astro-bench/buildmode.sh`): `plain` (default), `lto`, `pgo` (ThinLTO +
`-fprofile-use=$KYTY_PGO_PROFILE`) and `pgogen` (instrumented). CMake: `-DKYTY_LTO=thin`,
`-DKYTY_PGO=generate|use`, `-DKYTY_PGO_PROFILE=<file>`; all off by default. Relinks share a ThinLTO
cache in `~/.cache/kyty-thinlto`.

**A/B in pgo mode** (`ab.sh` and `gate.sh`, "Bench metrics and A/B" below, build the reference
with `refbuild.sh`, which follows the exported mode and profile):

```bash
export KYTY_BUILD_MODE=pgo KYTY_PGO_PROFILE=$(readlink -e ~/.cache/kyty-pgo/current.profdata)
new=$(tools/astro-bench/build.sh)                          # this checkout: _Build/linux-pgo/install
tools/astro-bench/ab.sh mylabel --new-install "$new"       # screening; the ref is <merge-base>-pgo-<hash>
tools/astro-bench/gate.sh mylabel --new-install "$new"     # once, before merge
```

- Same mode and same profile file on both sides, always. `refbuild.sh` keys its cache on the mode
  and the profile's hash, both scripts print `mode=pgo … sha256=<hash>`, each install has a
  `BUILD_MODE` file next to it, and `ab.sh` refuses two installs whose mode or profile differ.
- A shared profile is fine for A/Bs; there is no need to retrain per side. It was trained on the
  merge-base, so a branch's new code runs without counts, but measured on #83 (+1.8% on plain
  builds) that cost nothing: #83 with the `dev` profile 54.83 FPS, with a profile retrained on #83
  itself 54.70 (4 runs each, standard error ~0.3; FINDINGS "Build modes"). For a branch that adds
  a lot of new hot code, check once: train its own profile (`pgo-train.sh HEAD`) and compare the
  branch built with it against the branch built with the shared one.
- Retrain after hot-path merges to `dev` (anything on the command thread, the prepare workers or
  the guest-memory paths): whoever merges runs `tools/astro-bench/pgo-train.sh origin/dev` after
  `git fetch`; `current.profdata` then moves to the new `dev` commit. A retrain takes ~5 minutes
  of machine time: the instrumented build, 2.5–3.5 min (153 s cold in a build dir, 209 s as a
  `refbuild.sh` of a new commit while other builds ran), then a warm-up run and the training run,
  ~1¾ min under the exclusive lock (the instrumented build runs at ~31 FPS), and seconds to merge.
  Lock waits come on top.
- Prefer the pgo-mode A/B for merge decisions: that is the build the user plays, and gains sized
  on plain builds shrink on it (14–26% in the research measurements; #83's +1.8% on plain read
  +0.1% on PGO). A plain A/B is still fine for quick iteration.
- Build times (one changed `.cpp` is `renderDraw.cpp`; wall time without lock waits):

  | Build | Cold (no ccache) | Incremental | Fresh dir, ccache warm |
  |---|---|---|---|
  | plain | 146 s | 7 s | 111 s (`refbuild.sh`, worktree setup included) |
  | pgogen (instrumented) | 153 s | 6 s | 209 s (`refbuild.sh` of a new commit) |
  | pgo (ThinLTO + PGO) | 141 s (link 17 s) | 7 s (relink ~2 s from the cache) | 202 s with a new profile, 22 s for `build.sh` after `refbuild.sh` with the same profile |

  A new profile changes every compile command, so the first build with it recompiles everything;
  `refbuild.sh` and `build.sh` with the same profile then share ccache entries.
- The profile is trained on Sky Garden frames 0–1600 only. Other levels run code paths it saw
  rarely or never, so a play build may gain less there (see "Building the fast binary" in
  `docs/mt-rewrite/README.md`).

## Bench metrics and A/B (Track T, 2026-11-11)

Every `garden.sh` run reports per-draw costs over its frame window, at no measurable cost (#86).
They are appended to the summary line; `/tmp/garden-<label>.json` has the full set, and
`metrics.py` documents every field.
- `cmd_ipd` / `cmd_cpd`: the command thread's user-mode instructions and cycles per draw.
- `cmd_ns = cmd_uns + cmd_sns`: its CPU ns per draw, user plus sys.
- `wait_prep` / `wait_rec`: ns per draw it waits for the workers / the recorder.
- `prep_ns`: mean worker time per prepared draw.
- `rej_f`: rejected prepared draws per frame. `drains_kd`: queue drains per 1,000 draws.

Every run also lands in `~/.cache/kyty-builds/results.jsonl`.

**Noise** (sd per run; one binary, one batch / between batches / between two binaries of the
same hot code):

| metric | 1000–1600 | 200–500 | drift between batches | two binaries (layout) |
|---|---|---|---|---|
| `cmd_ipd` | 0.15% | 0.17% | ~0.1% | ~0.1% |
| `cmd_cpd` | 0.47% | 0.53% | ~0.2% | ~0.6% |
| `cmd_uns` | 0.25% | 0.95% | ~0.6% | ~0.9% |
| FPS | 0.39% | 1.1% | ~0.8% | ~0.8% |

Code layout alone moves cycles, CPU time and FPS by up to ~1% between two builds of the same
hot-path code, whatever the number of runs. Instructions per draw don't move. So a change worth
less than ~1% is only visible in `ipd`, plus `cpd` when it is a stall fix rather than less work.

**Commands.**
- `ab.sh <label>`: screening A/B, frames 200–500 (~35 s a run instead of ~59).
  - With ≥3 stored runs of the merge-base binary from the last 24 h, it runs only the new side
    (2 runs); otherwise 2 + 2 interleaved.
  - It prints one verdict line: each metric's Δ ± sigma, then `!` signal (≥3 sigma), `?` (2–3)
    or `~` noise. Sigma includes the drift term for a stored reference and the layout term for
    two binaries.
  - For a kill switch on one binary (no layout term):
    `ab.sh <label> --ref-install D --new-install D --new-env KYTY_X=0`.
- `results.sh ref <sha> [--window W]`: stored numbers of a commit (median, spread).
- `gate.sh <label>` (~7 min, one lock hold) prints a pass/fail block for the PR:
  - a same-batch pair in 1000–1600 (2 + 2 runs, the FPS of record);
  - one run with `KYTY_PREPARE_VERIFY`, `KYTY_BINDING_REUSE_VERIFY`,
    `KYTY_WORKER_TARGETS_VERIFY` and `KYTY_WORKER_PIPELINE_KEY_VERIFY` (`--verify-env` adds more);
  - screenshots of level frame 600 from both sides (`GARDEN_SHOT`, saved in-emulator through
    `KYTY_SHOT_FILE`: the guest image, no desktop capture).

**When a same-batch FPS pair is still required:** for any FPS number written into FINDINGS.md or
a PR title, and when the change targets waits or the GPU (cycles and instructions on the command
thread don't see those). A stored reference is never valid for FPS or waits: they drift with clock
speed (CPU 10 runs at 92–96 °C).

**Design note: in-process ABAB of kill switches** (assessed, not built). A switch registry that
the bench flips at frame boundaries would give A and B phases inside one run, free of drift and
layout noise.
- Flip point: queue drained and workers idle (`FinishQueuedDraws` + `WaitPrepareIdle`, no open
  dispatch chain).
- METRICS lines would gain `phase=` and `value=`; `metrics.py` would sum A phases against B
  phases, dropping ~30 frames after each flip.
- Safe to flip:
  - at any time: `KYTY_LRU_MIRROR`;
  - drained: `KYTY_WORKER_TARGETS`, `_TEXTURES`, `_PIPELINE_KEY`, `_BUFFERS`, `KYTY_META_LOCK`;
  - drained, with a reset hook that drops state recorded by earlier draws:
    `KYTY_BINDING_REUSE`, `KYTY_BIND_DEDUPE`;
  - at a submission boundary: `KYTY_DISPATCH_CHAIN`;
  - startup only: prepare depth/workers/threads, the shared texture memo, the record thread, CPU
    pinning, async upload/WP, fast guest reads, DTC, shader codegen switches, late labels.
- A flippable feature must:
  - read its switch through the registry, never a `static`;
  - declare its flip class;
  - keep derived state current while off, or provide a reset hook;
  - keep its verify mode valid across flips.
- Limits: switches only, not code changes. Caches carry over between phases. Phases must
  alternate often (~150 frames), because `ipd` differs by up to 1% between scenes.
- Cost: ~200 lines plus 2–5 per switch. Worth building when Track B's run-level commit cuts
  (each kill-switched, ~150–270 ns/draw) need FPS numbers of record.

## Status

| Track | Current step | Notes |
|---|---|---|
| A | A1 done (#7, #8); A2 done; A3 measured: transfers end only ~10 passes/frame; A4 done (#10); A5: redundant binds, queries and same-target pass breaks ruled out on the GPU; recording cost measured (~10% of the thread): bind/push dedupe (#15, +1.2%) and a recording thread (+~2%) landed; in-pass GPU time is vertex shading, 53% of it in draws that switch shaders (not state) | GPU ~17 ms/frame (9.8 in passes: ~1.6 fragment, 0.7 per-draw state, the rest vertex work; 3.6 dispatches); indirect-dispatch syncs 0 |
| B | PrepareBda uploads (`KYTY_BDA_STATS`): real bytes (89% of lines changed, 0.6% in-frame repeats); DMA shaders read almost none, and 56% are replaced before any overlapping bind, so C1 (BDA page table on guest memory) could remove the eager upload; how much moves to bind-time copies is measured on its prototype. C1 parked: correctly ordered late labels cost 15% (binds read pending labels and CP writes as data; the free-looking version uploaded stale bytes), and in-place reads were FPS-flat (FINDINGS). One texture memo for both prepare workers: the command thread's re-checks of adopted records 84k → 51k/s, +1.5%. Second prepare worker (item 2 of the structural plan, B4): command thread waiting for the worker 7.7% → 2.5%, +2.5%; next for B4: buffer phase B (stream copies on the worker). Command-thread copies: texture bindings no longer recopy a worker record's ~1 KB desc, +1.8%; color-target copies (~1.8%) wait for render-target resolve on the worker. Texture resolve on the prepare worker (item 1): command thread resolve 13.5% → 6.2%, +1.2%; the worker now co-limits (2.94 µs/draw, command thread waits 8%), so the rest needs the second worker; its memo as a flat table, +2.8%. Per-draw fences: texture cache lock, scheduler mutex, recorder fence and counter locks off the command thread, +1.3% on top of #55 (#58); the last ones (render context mutex, depth-target lookup, range clamp) +0.7%. Async guest readbacks: the command thread records a guest read fault's download and moves on (its GPU wait in readbacks 9% → 0%), +7.4%; it is now ~98% busy, so FPS is its per-draw cost. B4 merged (threaded prepare, +18.8%, 5.47 µs/draw; worker 53% busy, so one worker keeps up). Core layout: game threads on the efficiency cores, +3.1%, worst frame 33.5 ms. Readback publishes void only the queued draws that read them (prepare fallbacks −55%, ~0.6% of the command thread); ObtainBuffer memo and a deeper prepare queue ruled out. Lock-free guest reads +1.5%; next: the PrepareBda upload memcpy (4.3% of the command thread). Next for B4's owner: the commit side, which now bounds the pipeline (texture resolve and its validation ~13%, memcpy ~12%, buffer cache ~7%). B1′ (single-thread cuts; B4 went to its own agent): step 1, draw struct copies, +6.0% (6.71 µs/draw); step 2, walker inlining, no gain; step 3a, vertex-decode lookups, +0.5%; walker references skip known values (+2.8% before B4; on B4 the prepare worker 2.98 → 2.80 µs/draw, FPS unchanged since the commit side limits). Walker work now only buys worker headroom. Done: B0, B2 +6.2%, B2b +3.8%, B3a, B3b; B1 dropped, B2c ruled out | texture resolve 16.7% → 8.7%, render-target resolve 10.8% → 5.6% of the GPU thread; B3 inventory in the B3 row |
| C | C1a merged (#6, +5.7%). C2 merged (#19): hot chunks under asynchronous write protection, waits on the command processor's own labels proven and skipped; verifier clean (0 lost writes in 67.5M checks); +2.2% FPS, process CPU 688% → 565%. VRSQRTPS traps measured: ≤ +1.7% ceiling, ~0.8% reachable only through a tail-call heuristic, parked (FINDINGS). Remaining Track C cost is on the GPU thread (ObtainBuffer ~7%, PrepareBda ~3%), overlapping Track B. C3 moved to the lead | C0 in FINDINGS: guest threads fit with room once faults are gone, no Track D. C1 import deferred: labels are written before the host GPU reads. Next: the 1,088 trapped VRSQRTPS sites (the other guest-thread signal storm), then the rest of the GPU thread's buffer-cache path |
| Lead | C3 done: the game times itself with the process time counter; real speed = every flip 1 vblank apart (`KYTY_TIME_DIAG`, `flip_vbl`) | `dev` 458607ad: 24.2–24.4 FPS, frames at 2 or 3 vblanks |
| G | GPU execution time (shader codegen, tiler). G0: compute suite unblocked (#24). G1: `KYTY_GPU_TIME=draws`, `KYTY_SPV_DUMP` (#28). G2: vertex EXEC fold (#29), GPU 16.97 → 16.13 ms/frame, +2.8% FPS. G3: time outside passes split (transfers 2.8, barrier gaps 0.85 ms/frame) and dispatches by shader. G4: render targets tiled and detiled straight from and into the image (#34), GPU 16.26 → 14.89 ms/frame (transfers 2.71 → 1.34). G5: masked EXEC selects in vertex shaders, GPU −0.17 ms/frame (in passes 8.94 → 8.78), top VS 1,848 → 826 selects (#36). G6: `KYTY_PIPELINE_STATS`; indexed VGPR reads as a switch on M0, in passes −0.40 ms/frame (skinned VS family 219 registers, 22 × 116-way select chains) (#39). G7: masked selects for compute and pixel shaders (#42), GPU −0.5 ms/frame (dispatches −0.3). Measured, not landed: masked selects for mesh shaders (within noise). Buffer copies (~1.0 ms/frame) are `PrepareBda` uploads (Track C). Ruled out: per-lane EXEC branches; fused legacy MAD parked (−0.4 ms for a rounding change). Ceilings measured: compute lane mapping −0.4 ms, in-region EXEC selects −0.47 ms. Next: the indexed VGPR range's register pressure (local array, ~0.5 ms ceiling), the 0.6 ms compute dispatch `0ec2f135`; fewer upload copies and barriers is Track C's (~1.0 ms) | GPU ~13.9 ms/frame (8.2 in passes, 3.4 dispatches, ~2.2 between) |
| G3 | Compute dispatches and their barriers. Done: dispatches the guest lets overlap share their barriers (#45): GPU 14.14 → 13.29 ms/frame, dispatches 3.68 → 3.04; with #42 (masked selects for compute, Track G) dispatches are 2.79 ms/frame. The dispatches left unchained wait on the guest's own `WAIT_REG_MEM`/CS_PARTIAL_FLUSH. Ruled out: run-by-run half emission, image accesses in masked selects, same-half DPP shuffles. Left: `0ec2f135` (0.53 ms, 168 registers, spills), whose remaining lever is the parked one-lane-per-invocation mapping | GPU ~12.9 ms/frame (8.2 in passes, 2.8 dispatches) |
| G4 | GPU time outside shader math. Uploads ≥ 256 KiB cross PCIe on the copy engine into a device-local ring, the universal queue copies from there (#48): GPU 13.04 → 12.42 ms/frame (`KYTY_GPU_TIME=1`), transfers 1.35 → 0.58. `PrepareBda`'s uploads share one barrier pair: GPU 12.30 → 12.19, barrier gaps 0.53 → 0.45. Barrier gaps attributed by site (`GPUGAPSITE`): the rest is mostly guest-ordered dispatches (G3) and draw/DMA hazards. Ruled out: ReBAR staging (−1 FPS), attachment accesses on their own stages (no change). Skinned vertex shaders: an unsafe cap of the indexed-read candidates at 16 measured GPU −0.57 ms/frame (213 → 168 registers); `M0` is bounded only by per-vertex data, so the cap has no correctness proof, and exact versions (multiple-of-4 pruning, a local array) are unmeasured; parked as a project decision (FINDINGS). Done | GPU ~12.2 ms/frame |
