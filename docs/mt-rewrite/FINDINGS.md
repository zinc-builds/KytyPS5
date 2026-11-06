# Astro Bot Sky Garden: performance findings (2026-11-06)

Host: Intel i5-14600K (6 P-cores = CPUs 0-11 with SMT, 8 E-cores = CPUs 12-19), RTX 4080 Super
16 GB (NVIDIA 615.78), 32 GB RAM, CachyOS, kernel 7.3. Game: Astro Bot PPSA21567 v01.019.000
(`~/Games/PS5/PPSA21567.zar`). Benchmark: `tools/astro-bench/garden.sh` — scripted route from
save slot 1 into Sky Garden (`aerial_garden`), 45 s average starting 25 s after `StartLevel`.
Run-to-run noise is about ±0.3 FPS; always compare two runs per configuration.

## Where Sky Garden stands

| Step | Sky Garden FPS |
|---|---|
| Upstream main 182161eb + mesh-slices fix (`astro`) | 7.95 |
| + PRs #1284, #1289 ported (`astro-perf`) | ~8.2 |
| + vertex-table / SRT-walker reads through the backing view (no GPU sync) | ~8.3 |
| + `--amd-cpu` rewriting EXTRQ (1.4 M SIGILL/s → 0) | 13.5 |
| + PR #1307 userfaultfd write tracking (`astro-uffd`) | 14.1 |
| + hybrid-CPU pinning (`common/hybridAffinity.h`) | 16.4 |
| + AMD-exact VRSQRTPS kept as default (77k SIGILL/s) | 15.9–16.0 |

`dev` = `astro-uffd` = everything above. ~5,800 draws/frame, ~93k draws/s, ~130k write-tracking
faults/s.

## Progress toward 60 FPS

Target: 60 FPS with stable pacing (16.7 ms frames). Astro Bot's game speed follows its frame
rate, so anything below 60 plays in slow motion. At ~5.8k draws/frame that means ~350k draws/s
on the GPU command thread, about 3.7× the starting point. Two `garden.sh` runs per row,
`--amd-cpu`. `worst_ms` is the mean over 1 s refreshes of the slowest frame (max in brackets),
and `low1` is the mean 1% low FPS.

| Date | Change | FPS | draws/s | worst_ms | low1 |
|---|---|---|---|---|---|
| 2026-11-06 | `dev` 48b2101a (baseline) | 15.83 / 15.91 | 92.0k / 92.5k | 67.5 (max 68.3)¹ | 13.7¹ |
| 2026-11-06 | SRT walker reads read-watched pages without faulting | 17.65 / 17.98 | 101.6k / 102.6k | 69.0 (max 99.4) | 14.3 |
| 2026-11-06 | Atomic-only shader writes share one barrier per batch | 18.92 / 19.18 | 107.6k / 108.4k | 66.2 (max 85.0) | 14.4 |

¹ From a short screenshot run; the two baseline runs predate the `worst_ms`/`low1` columns.

## The bottleneck now

The GPU command thread (`GuestGpu::ThreadRun`, OS name `KytyGpuCmd`, Tracy `Thread_Gpu`) is the
critical path: Tracy shows `CommandProcessor::Process` covering ~100% of wall time; the thread is
~80% on-CPU, the rest is GPU-synchronization waits.

Tracy, 10 s of Sky Garden on `astro-uffd` (pinned, `--amd-cpu`):

| Zone | Total | Count | Mean |
|---|---|---|---|
| CommandProcessor::Process | 10.05 s | 2.3k | 4.3 ms |
| DrawIndex | 7.50 s | 714k | 10.5 µs |
| CpOpDispatchIndirect | 1.46 s | 1.4k | 1.0 ms |
| ↳ DI:GetComputeProgram | 1.25 s | 1.4k | 0.88 ms (GPU sync: reads GPU-written workgroup counts) |
| PrepareBindings | 1.41 s | 1.34 M | 1.1 µs |
| PrepareBda | 0.19 s | 21k | 9 µs |

perf (draw thread, pinned): flat — `pthread_rwlock_*` ~9% (CopySparseMemory in
`BufferCache::ObtainBuffer`'s stream-buffer path), memcpy ~6%, `FindTextureDescription` ~3-8%,
texture-cache hash lookups ~5-7%, `MaterializeResources`/`WalkSrt` ~5%, `SynchronizeBuffer` ~5%.

GPU: ~50% busy at 65 W, VRAM 13.8 / 16 GB — not GPU-bound.

## What we learned (keep in mind for the rewrite)

- **Signals were the dominant cost.** Without `--amd-cpu`, 97 EXTRQ sites in eboot.bin trap 1.4 M
  times/s; every signal takes the process-wide `sighand->siglock`, so 11 guest `tbb_thead`
  workers and the game main thread spent ~45% in `native_queued_spin_lock_slowpath`. Any design
  that adds per-draw signals or faults on the GPU thread will hurt for the same reason.
- **GPU-dirty pages are page-granular.** Tiny writable shader buffers (8–32 bytes, bound through
  `RebindBuffers`) share 4 KiB pages with CPU-written vertex/descriptor tables. Reading those tables
  on the GPU thread faulted into `BufferCache::ReadMemory` → full GPU sync (~3–5 ms) ~125×/s.
  Fixed locally by reading through the unprotected backing view when `HasGpuDirtyBytes` is false
  (`ShaderSetGuestReader` in shader.cpp, `ShaderTryReadGuest` + `CompleteLoad32` for SRT-walker
  loads in runtimeLinker.cpp). Removing the sync did not raise FPS by itself: those syncs had been
  cleaning 512 KiB windows, so later binds took slower paths — measure end to end.
- **Write-fault storm:** `PrepareBda` uploads every page the guest wrote (~280 MB/s, ~700
  `SynchronizeBuffersInRange` calls/s) and re-protects it; the game rewrites them next frame.
  Each re-protect costs a fault next frame. userfaultfd (PR #1307) made faults cheaper (+4%).
- **Core placement matters once contention is gone:** pinning everything to P-cores 0-9 and the
  GPU thread alone on CPU 10 (SMT sibling 11 idle) gave +16% in the benchmark window (+30% in a
  later, lighter scene). E-cores slow critical guest threads.
- **`top` lies about the GPU thread:** it showed ~70% while Tracy showed it busy 100% of wall time.
  Use Tracy zones or `perf` + scheduler tracing.
- **Ruled out:** core pinning *before* the signal fix (no gain), `--occlusion-queries` (12.9 FPS,
  draws/frame unchanged — the game doesn't skip draws on them), sharded reader-writer lock for the
  guest address space (no change), CPU `performance` power profile (no change), jemalloc (+2%,
  noise), PR #1211 (its useful parts are upstream already), emk530 NoRT/GI-probe game patches
  (no change; GPU-side), cold vs warm pipeline cache (warm it once).

## Profile on `dev` 48b2101a (2026-11-06)

perf with LBR call stacks (`perf record -t <KytyGpuCmd> --call-graph lbr`, 10 s, ~80% on-CPU),
inclusive share of on-CPU samples:

| Path | Share |
|---|---|
| `PipelineCache::GetGraphicsPrograms` (SRT walk + `MaterializeResources` + module match) | 27% |
| ↳ JIT-compiled SRT walker code itself | ~9% |
| `ResolveTexture` (`FindTextureDescription`, `FindImage` hash lookups) | 15% |
| `BufferCache::ObtainBuffer` stream copies (`CopySparseMemory`) | 11-13% |
| ↳ `pthread_rwlock_unlock`/`rdlock` on the address-space and backing locks | ~7% |
| NVIDIA driver (`libnvidia-glcore`) | 10% |
| Kernel page faults + signal delivery (`asm_exc_page_fault`, sigframes) | ~10% |
| `CommitBindings` | 5.6% |
| `SynchronizeBuffer` (uploads of CPU-written pages) | 4% |

Tracy (10 s): `DrawIndex` 7.19 s over 803k draws (9.0 µs mean); `DI:GetComputeProgram` 1.48 s
over 1,680 indirect dispatches. With zones on the sync points, 620 of those dispatches spend
1.46 s (14.5% of wall time) in `BufferCache::ReadMemory`, 2.4 ms each.

### SRT walker faults (fixed)

The x86 SRT walker read guest memory with plain loads. Pages the buffer cache protects against
CPU reads (GPU-modified bytes somewhere on the page) faulted on each load. The exception handler
then served the DWORD from the backing view. That happened ~72k times/s on the GPU thread, about
0.8 per draw, at ~1.3 µs of kernel entry, signal frame and sigreturn each. The walker's loads now
call a stub inside its code. The stub checks the page tracker's per-page read-watch bit (a flat
region table, no locks). Watched pages are read through `ShaderTryReadGuest`. Everything else is
still a plain load, and the old fault path remains the fallback for GPU-dirty bytes and races.
Walker backing faults went from ~72k/s to 0, for +12% FPS.

### Indirect-dispatch GPU syncs (open)

The ~1.5 s/10 s of `DI:GetComputeProgram` syncs are **not** the workgroup-count read of the
dispatch arguments (`EvaluateIndirectCount` never hits GPU-dirty bytes). They are SRT-walker
loads of two GPU-written DWORDs (`0x57989017c` and `0x57980739c` in this run). A compute pass
writes them and the next indirect dispatches' SRT walks read them, so the walk must wait for the
GPU (`HandleFault` → `ReadMemory`). Removing that needs the walk not to depend on GPU-written
values: bind a superset of descriptors, or resolve the value on the GPU.

### The GPU is a second bottleneck (2026-11-06)

Measured by bracketing each submitted command buffer with Vulkan timestamps (a local diagnostic
build), the GPU executes **~610 ms of work per second at 17 FPS, about 35 ms per frame**. That
alone caps Sky Garden near 28 FPS. Nsight Systems GPU metrics (`pkexec nsys profile
--gpu-metrics-devices=all`, sampling device-wide while the emulator runs as the user) show the
time is not shading work:

| Metric (mean over 4 s) | Value |
|---|---|
| GR Active | 52% |
| SMs Active | 24% |
| SM Issue | 7.5% |
| DRAM read / write bandwidth | 2.8% / 2.6% |

The GPU is serialized, not saturated. Counting recorded Vulkan commands per call site (a
diagnostic build wrapping vulkan.hpp's dispatcher) gives, per frame:

| Source | Per frame |
|---|---|
| `ShaderWriteHazardBarrier` + `ShaderWriteBarrier` (draws with shader buffer writes; a full `ALL_COMMANDS` drain before, a write barrier after, and both end the render pass) | ~1,340 |
| `Image::Transit` layout transitions (`CommitBindings`, `AcquireRenderTargets`) | ~900 |
| `ShaderAccessBarrier` after dispatches | ~220 |
| `vkCmdBeginRendering` | ~1,000 |

About 560 draws per frame write a shader buffer. Almost all write a single DWORD, atomically, in
the vertex shader: one of three rotating 128-byte slots. The first indirect-dispatch sync of a
frame waits ~9 ms because it drains the previous frame's GPU work, so GPU time also stalls the
command thread.

**Atomic write batches (landed).** The recompiler now records whether a buffer has any plain
(non-atomic) access (`BufferResource::plain_access`). A draw whose shader buffer writes all go to
atomic-only buffers joins a batch. The first draw keeps the opening hazard barrier. Later ones
skip both barriers and the render-pass breaks. The batch's closing `ShaderWriteBarrier` is
recorded before:
- any draw whose plain buffer, vertex, index or indirect ranges overlap the batch's atomics;
- any draw that writes plainly or through raw addresses;
- an atomic draw whose range overlaps a plain access made during the batch;
- any dispatch;
- any PM4 packet other than draws, register writes and markers.

Buffer-cache transfers already open with `ALL_COMMANDS` barriers. Result: GPU time ~35 → ~24.5
ms/frame, barriers ~2,900 → ~2,000 and render passes ~1,000 → ~500 per frame, and +7.5% FPS.
An unsafe experiment that skipped these barriers outright gave the same FPS, so batching loses
nothing.

**Ruled out: a reader-distributed lock for the guest address space and backing store.**
`pthread_rwlock_rdunlock` showed 8% self time on the GPU thread, under `CopySparseMemory` from
stream-buffer uploads. Replacing both `shared_mutex`es with per-thread-slot reader counts gave
17.36 / 17.67 FPS (no change). The samples moved into `ReadSparseBacking`'s memcpy: the cost is
copying cold, freshly written guest data, not the lock.

### Tooling notes

- `perf record -e sched:sched_switch` records, but `perf script` fails here with "broken or
  missing trace data". Off-CPU time came from Tracy zones instead.
- RenderDoc 1.46 cannot run the emulator. It lacks `VK_KHR_wayland_surface` (use
  `SDL_VIDEO_DRIVER=x11`), and on X11 the emulator dies silently right after device creation.
  It is also launched by absolute path, so `^\./kyty_emulator` patterns miss it; match
  `^[^ ]*kyty_emulator --game`.
- GPU metrics need admin (`RmProfilingAdminOnly=1`). They are device-wide, so a root `nsys`
  sampling `sleep N` works while the emulator runs as the user.
- Keep a copy of `kyty_emulator_clang_lld.map` with each perf capture. A rebuild overwrites it
  and old captures no longer symbolize.
- Do not pipe `garden.sh` through `head`. SIGPIPE kills the script before its cleanup and leaves
  the emulator running.
- `cmake --install` fails resolving the Qt launcher's image-plugin dependencies (`libheif`,
  `libjxrglue`). `kyty_emulator` is installed before that step, so benchmarking is unaffected.

## Remaining costs and candidate directions

1. **Per-draw CPU cost (~10.5 µs × 5.8k draws).** Spread thinly; only structural change helps:
   see `RESEARCH-threaded-draw.md` (pipelined front-end/back-end split, prepare/commit split,
   secondary command buffers, memoized SRT walk).
2. **Indirect-dispatch sync (~14% of wall time).** The SRT walk of a few indirect dispatches
   reads GPU-written words (see above; not the workgroup counts as first assumed).
3. **Guest write faults (~130k/s)** — no longer on the critical path after pinning (`tbb_thead`
   ~17% each), but they will be once the GPU thread gets faster.

## Instrumentation already in the tree

- `KYTY_PERF_LOG=1` (needs `--perf-overlay`): one `PERF fps=… draws_frame=… wt_faults_s=…
  sigill_s=… threads:…` line per overlay refresh on stdout.
- `KYTY_FAULT_DIAG=1`: `FAULTDIAG` lines for CPU reads that had to download GPU data (with the
  writable bind that dirtied the page and a short stack), `SYNCDIAG` once per second with
  upload counts/bytes per `SynchronizeBuffer` caller. Summarise with
  `tools/astro-bench/faultsum.py`.
- Tracy: `--profile`, capture with `tracy-capture` v0.14.1 built from `3rdparty/tracy/capture`
  (`~/AI/emu/tracy-build` has binaries); export with `tracy-csvexport`. Tracy renames the main
  thread, so match the process by command line (`^\./kyty_emulator --game`), never by name.
- Thread name `KytyGpuCmd` for the GPU thread (top/perf/taskset).
- Env knobs: `KYTY_NO_PIN=1`, `KYTY_AMD_SKIP_RSQRT=1`, `KYTY_NO_SHADER_FASTREAD=1`.
