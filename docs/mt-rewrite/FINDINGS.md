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

## Remaining costs and candidate directions

1. **Per-draw CPU cost (~10.5 µs × 5.8k draws).** Spread thinly; only structural change helps:
   see `RESEARCH-threaded-draw.md` (pipelined front-end/back-end split, prepare/commit split,
   secondary command buffers, memoized SRT walk).
2. **Indirect-dispatch sync (~12% of the thread).** `MaterializeResources` reads GPU-written
   workgroup counts (`runtime.indirect_args_address`) because descriptors are selected per
   workgroup (`EvaluateIndirectCount`, ResourceMaterialization.cpp). Avoiding it means binding an
   upper bound of descriptors (selector table maximum) instead of the exact count.
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
