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
| 2026-11-06 | Stream copies reused within a guest-sync epoch (same batch: merge-base 18.67 / 19.32) | 19.88 / 20.28 | 111.6k / 113.4k | 59.9 (max 100.0) | 16.1 |
| 2026-11-06 | Read-only depth attachments; no barrier for read after read (A/B, same batch: ref 18.71 / 18.99) | 18.48 / 18.91 | 105.6k / 107.2k | 67.1 (max 98.7) | 14.7 |
| 2026-11-06 | Indirect dispatches no longer wait for the GPU in their SRT walk (same batch: merge-base 20.06 / 20.16) | 21.94 / 22.17 | 122.1k / 123.3k | 55.3 (max 84.5) | 18.0 |
| 2026-11-06 | Stage image slots reuse their last texture resolve (Track B2; measured on 582e4043, without the stream-copy row: ref 19.33 / 19.57) | 20.68 / 20.66 | 115.6k / 115.5k | 58.3 (max 83.9) | 16.7 |
| 2026-11-07 | Color and depth target slots reuse their last resolve (Track B2b; same batch: ref `dev` 277a708e 24.24 / 24.24) | 24.95 / 25.36 | 136.8k / 139.0k | 53.0 (max 82.2) | 19.2 |
| 2026-11-07 | Draws re-record only a changed pipeline, push constants and push descriptors (Track A; same batch: ref `dev` 9af34700 24.78 / 24.87) | 24.90 / 25.38 | 136.7k / 139.1k | 52.5 (max 82.6) | 19.1 |
| 2026-11-07 | Draws record their Vulkan commands on a second thread (Track A; same batch: ref `dev` 7f737011 24.99 / 25.19) | 25.50 / 25.55 | 139.8k / 139.9k | 52.9 (max 68.7) | 19.2 |
| 2026-11-07 | Hot write-tracked chunks use asynchronous write protection (same binary, async off: 25.24 / 25.37; guest main thread 15.0 → 11.3 ms/frame) | 25.88 / 25.85 | 141.6k / 141.4k | 52.7 (max 83.4) | 19.5 |
| 2026-11-07 | Draws construct only the color attachments and vertex stages they use; texture bindings copied once (Track B1′ step 1; same batch: ref `dev` 05ffc8b5 25.90 / 25.91; 7.06 → 6.71 µs/draw) | 27.40 / 27.55 | 148.8k / 149.1k | 52.6 (max 83.2) | 19.5 |
| 2026-11-07 | Shader-map lookups cached per thread; vertex tables copied without zeroing (Track B1′ step 3a; 4 pairs, same batch: ref `dev` 07379452 27.37 / 28.27 / 28.10 / 28.04; +0.5% on runs 2–4, 6.60 → 6.56 µs/draw) | 27.96 / 28.41 / 28.23 / 28.22 | 151.6k / 153.0k / 152.1k / 152.1k | 51.5 (max 67.8) | 19.4 |

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

**Read-only depth and read-after-read (landed).** `KYTY_VK_COUNT=1` (now in the tree, with
`KYTY_GPU_TIME=1`) breaks image barriers down by layout pair. The largest pair was
`DEPTH_STENCIL_READ_ONLY → DEPTH_STENCIL_READ_ONLY` at ~415 per frame: a depth buffer
alternately sampled and bound as a read-only depth attachment, where only the access mask
changed. Changes:
- Attachments in a read-only depth/stencil layout use `STORE_OP_NONE` and read-only access.
- A barrier into a read-only access makes earlier writes visible to all read types at all
  stages.
- `Image::GetBarriers` then skips barriers for further reads in the same layout, merging their
  stages and accesses so a later writer waits for every reader.
- Collapsing per-subresource states keeps the union of their readers.

Measured with diagnostics builds, same scene:

| Build | GPU time | Image barriers | Render passes |
|---|---|---|---|
| Before | 21.6 ms/frame | ~17.3k/s | ~430/frame |
| After | 19.8 ms/frame | ~7k/s | ~180/frame |

FPS is unchanged (A/B in one batch), because the command thread still limits.

**Ruled out: a reader-distributed lock for the guest address space and backing store.**
`pthread_rwlock_rdunlock` showed 8% self time on the GPU thread, under `CopySparseMemory` from
stream-buffer uploads. Replacing both `shared_mutex`es with per-thread-slot reader counts gave
17.36 / 17.67 FPS (no change). The samples moved into `ReadSparseBacking`'s memcpy: the cost is
copying cold, freshly written guest data, not the lock.

## Guest memory traffic and the guest threads (Track C0, 2026-11-06)

Measured on `dev` 582e4043 with `KYTY_MEM_DIAG=1` and `tools/astro-bench/c0-profile.sh` (per-thread
`/proc` CPU time, `perf stat --per-thread`, and `perf record` of every thread for 10 s in Sky
Garden). The diagnostic and perf cost ~10%, so this run did 16–17 FPS. Per frame means ÷16.5.

**GPU thread, guest-memory traffic (per second):**

| What | Rate | Per frame |
|---|---|---|
| `ObtainBuffer` calls | 1.13 M | ~68k (~12 per draw) |
| ↳ clean ranges (tracker check only) | 735k | |
| ↳ too large for the stream path (> 16 KiB) | 196k | |
| ↳ written by the shader | 15k | |
| ↳ **stream copies** of CPU-dirty ranges | 185k, 338 MB | 11k, 20 MB (1.8 KiB mean) |
| stream copies repeating an exact range already copied since the last guest-visible GPU signal | 151k (81%), **bytes changed: 0** | |
| stream copies whose bytes equal the previous copy of that range (any time) | 183k (99%) | |
| `SynchronizeBuffer` from `ObtainBuffer` | 930k calls, 88 uploads, 3.6 MB | |
| `SynchronizeBuffer` from `PrepareBda` | 1.1k calls, 1.0k uploads, **330 MB** | 20 MB |
| pages re-write-protected after an upload / unprotected by a fault | 82.6k / 82.8k | ~5k (20 MB) |
| guest write faults (other threads / GPU thread) | 131k / 0.9k | ~8k |
| labels (`RELEASE_MEM`, `WRITE_DATA`) / guest submissions | 13k / 316 | 790 / 19 |

So each frame the guest rewrites ~5k pages (20 MB) of data that DMA shaders read through the BDA
page table. `PrepareBda` uploads them, re-protects them, and the next frame faults on every one
again (1.6 faults per page). Separately, the stream path copies 20 MB/frame of small CPU-written
ranges, and 81–90% of those copies repeat a copy made moments earlier with the same bytes.

**Guest threads (CPU time per frame at 16.1 FPS):**

| Thread | ms/frame | What it does |
|---|---|---|
| `KytyGpuCmd` | 61.4 | the emulator's draw thread (critical path) |
| `kyty_emulator` (game main thread) | 14.3 | 55% guest code, 27% kernel, 13% emulator |
| `FluidCalculation` | 13.5 | 74% kernel: a `sched_yield` spin-wait, not work |
| 12 × `tbb_thead` (game job workers) | 12.6–13.3 each | 54% guest code, 29% kernel, 15% emulator |
| `DrawThread` | 7.5 | 43% guest code, 39% kernel |

The tbb workers' kernel time is mostly `native_queued_spin_lock_slowpath` (12% of their time). Their
emulator time is spinning on tracker locks while handling write faults: the region spinlock in
`BufferCache::InvalidateMemory` (7.5%) and the page-manager region lock in `IsTrackingFault`
(4.2%), both held by the GPU thread while it uploads and re-protects. About 40% of the guest
threads' time is write-fault handling. One guest address takes 13% of tbb time (a hot loop, likely
a job-system spin-wait). Read faults are negligible (~270/s).

**Verdict on the guest side:** the busiest guest threads use 13–14 ms of CPU per frame, under the
16.7 ms frame but at the plan's 14 ms budget. About 40% of that is fault handling that C2 removes,
which would leave ~8 ms. The fault count per frame is fixed (~8k), so at 60 FPS the rate would
quadruple to ~500k/s and the lock contention would get worse, not better. **No Track D for now:**
the guest threads fit once the fault storm is gone. Re-measure after C2.

### Host-memory import (C1): feasible, but not safe with how labels work today

- NVIDIA 615 imports a `MAP_SHARED` memfd mapping (the guest backing store's own type) through
  `VK_EXT_external_memory_host`: `minImportedHostPointerAlignment` is 4 KiB, the importable memory
  types are host-visible + coherent (one also cached), importing 256 MiB takes 41 ms (it pins the
  pages), GPU copies from it run at 13–14 GB/s, and GPU writes are visible through a second view of
  the same pages (probe: `/tmp/trackC-probe/probe.cpp`, kept out of the tree).
- **The blocker is timing, not the API.** `RELEASE_MEM` and `WRITE_DATA` labels are written when
  the GPU thread *decodes* the packet, before the host GPU has executed (or even been sent) the
  preceding draws. The guest takes a label as permission to rewrite the memory those draws read.
  With copies that is fine, because a copy snapshots the bytes at record time. An in-place read
  would see the guest's next-frame bytes. Making import safe means deferring every guest-visible
  GPU signal to host-GPU completion, which also changes how far the guest can run ahead.
- **Re-plan:** because 81–90% of the copies repeat a range copied moments earlier, reusing copies
  removes most of the copy cost without that risk (C1a, below). After C1a about 19k copies/s
  remain (~1.2k per frame). Import is deferred until the label timing changes for another reason.

### Stream-copy reuse (C1a, landed)

`BufferCache::ObtainBuffer` keeps a direct-mapped table (8,192 slots) of recent stream copies keyed
by exact (address, size). A copy is reused while all of these hold:
- the `GuestSyncEpoch` hasn't advanced. It advances on every guest submission, on every PM4
  packet other than register writes, draws and markers (the same set that closes atomic-write
  batches: labels, waits, DMA, dispatches, events), and on both sides of every emulator store into
  guest memory (`TryWriteBacking`: downloads, labels);
- the same command buffer (scheduler tick), so the stream ring's existing watch covers the reuse;
- the same stream-ring lap, so the bytes haven't been overwritten.

Between two epoch advances the guest has had no GPU signal that the earlier draw finished, so it
can't legally rewrite the range. `KYTY_STREAM_VERIFY=1` compares every reuse against the guest
bytes: 0 mismatches over a 30 s Sky Garden run, with 31k reuses/s against 3.4k copies/s (90%
reused). `KYTY_NO_STREAM_REUSE=1` turns it off. Result: 19.00 → 20.08 FPS (+5.7%, same batch).

### Write tracking without signals (C2)

The kernel (7.3) supports userfaultfd **asynchronous** write protection (`UFFD_FEATURE_WP_ASYNC`)
on shared-memory views, plus `PAGEMAP_SCAN` to collect written pages and re-protect them in one
ioctl. Probe on a 1 GiB memfd view: an async write fault costs 0.47 µs, resolved in the kernel
with no signal and no `siglock`, against several µs plus lock contention for today's SIGBUS. A
scan of a clean 1 GiB costs 0.15 ms, and 0.27 ms after 5,000 written pages.

**Where the faults land** (`KYTY_MEM_DIAG=1`, `WFAULTS` and `MEMSCAN`):
- ~140k write faults/s hit only ~16k distinct pages, in 55 2-MiB chunks (mostly
  0x50b600000–0x50ee00000). 27 chunks take 90% of them.
- The page tracker watches ~0.94–1.03 M pages (3.7–4 GB) in ~1,430 regions. One `PAGEMAP_SCAN`
  over all of it costs ~1.5 ms, so scanning everything at each submission (19 per frame) is
  out. Only the hot chunks can be scanned.

**Design:**
- A chunk with ≥ 64 SIGBUS write faults in one second moves to a second userfaultfd with
  `WP_ASYNC`. A chunk the command processor stores a label into is promoted too (see below).
- At each submission start `PAGEMAP_SCAN` with `WP_MATCHING` collects the written pages of the
  async chunks and re-protects them. Written pages that a cache still watches are invalidated,
  as a fault would do. Chunks with no write-watched page are skipped (a page that gains a
  watcher is protected then, so earlier stores need no report).
- A satisfied `WAIT_REG_MEM`, memory semaphore or valid `REWIND` may publish guest data, so it
  requests a collection, run before the next packet that reads guest data (register writes,
  waits and markers in between don't).
- Unless the wait is provably on the command processor's own label. `RELEASE_MEM` and
  `WRITE_DATA` into an async page re-protect that page (one-page `PAGEMAP_SCAN`), then store
  through the backing alias with one scalar store (the guest may reuse a label at once), which
  leaves no written mark, and record (address, value, page token). Pages whose bytes another
  guest view also maps get no proof: a store through the alias would not mark this view. A wait whose value matches, whose page token is still current and whose page shows no
  store since (a one-page scan without `WP_MATCHING`) skips the collection: the value is the
  processor's own, so the guest published nothing by it.
- The page token advances whenever a page's written state is cleared outside a collection (the
  label store's re-protect, a new write watcher, the end of a read watch), since that would hide
  a guest store from the "written since?" check; those writes are kept for the next collection
  (a new watcher's range can span pages already watched). Remaps and zeroing void all labels.
- Inside async chunks a page stays write-protected after its last watcher leaves.
- Earlier versions: scanning at every satisfied wait took 17% of the GPU thread (−21% FPS);
  skipping waits on any value the processor had stored was unsound (the guest can store the
  same value); collecting whenever a label store found its page written cost ~63 scans per
  frame (−2%), because the guest resets labels before reuse.

**Result on `dev` 71d59e85** (same binary, `KYTY_NO_ASYNC_WP=1` as the reference, one batch):

| 2 runs each | async off | async on |
|---|---|---|
| FPS | 25.24 / 25.37 | 25.88 / 25.85 |
| SIGBUS write faults/s | ~196k | ~410 |
| Whole process, CPU (10 s profile, review-round-1 build) | 688% | 565% |
| Game main thread, ms of CPU per frame | 15.0 | 11.3 |
| Each `tbb_thead` worker, ms per frame | 13.6 | 9.8 |
| GPU thread, ms per frame | 42.5 | 44.4 |

~100 chunks are async (label promotion added ~75); a collection scan takes ~40 µs, ~280 per
second. ~3,000 satisfied waits/s are proven label waits and skip collection; ~50/s collect.
The GPU thread pays ~2 ms/frame more (scans, label stores), but with the draw recorder on its
own thread (#18) the freed cores are worth more: on `dev` 838723bc, before #18, the same design
was FPS-neutral (25.07 vs 25.04).

**Correctness.** Asynchronous protection moves the moment a write is noticed from the store to
the next collection, so these had to hold. The verifier found the first two:
- The GPU thread's own CPU stores into guest memory (labels, `WRITE_DATA`, CPU-side DMA fills
  and copies, occlusion results, LOD stats, GDS reads, memory semaphores) raise no fault in an
  async range. Each one invalidates explicitly (`RenderContext::NoteGpuWrite`).
- Lifting the read protection of a GPU-dirty page re-applies the write protection, which would
  erase the written state of an earlier guest store. The range is scanned first, and its writes
  are handed to the next collection.
- Collections happen wherever the guest may have published data: each submission, and each
  satisfied wait, memory semaphore or `REWIND` except a wait proven to be on the processor's
  own label (above). Review found the earlier "any stored value" skip unsound.

`KYTY_ASYNC_WP_VERIFY=1` snapshots the write-watched pages of async ranges after a collection.
It compares them at the next one and flags any page whose bytes changed without being reported
written, invalidated, unwatched, remapped or stored through the backing alias. It reads the
pages before taking those logs, so a store racing the check can't be misreported.
`KYTY_ASYNC_WP_DRYRUN=1` runs the same bookkeeping with the chunks left on the faulting path.
That is the control: 0 lost writes in 323M page checks. With async on, in Sky Garden: 0 in 67.5M.
`KYTY_ASYNC_WP_TRACE=<page>` prints every tracking event of one page.

Earlier verifier reports (~0.1% "lost writes", even in the dry run) were the verifier's own
race. It took its logs, then a guest thread faulted, logged and wrote the page, and only then
did the compare read it.

**The other signal source on guest threads:** the `siglock` contention in the C0 profile is
shared. Write-fault SIGBUS is ~3% of tbb time. SIGILL from the 1,088 VRSQRTPS sites the
`--amd-cpu` patcher couldn't relocate (~85k traps/s) is ~6%.

### The trapped VRSQRTPS sites (measured, not pursued)

After C2 the SIGILL traps (~123k/s) are the main signal source on guest threads.
- **Ceiling:** `KYTY_AMD_SKIP_RSQRT=1` (host-precision VRSQRTPS, no traps) against the default on
  `dev` 74b1d351: 26.48 / 26.25 vs 25.79 / 26.05 FPS, at most **+1.7%**. Draws per frame also
  change (5,400 vs 5,490): the precision changes game state, so AMD-exact results stay.
- **Why they trap:** all 1,088 sites are 4-byte `vrsqrtps xmm, xmm` in 472 functions with an
  indirect jump the patcher can't resolve, so it may not move neighbouring instructions to fit a
  5-byte jump.
- **Padding caves:** only 7 sites have ≥ 5 bytes of int3 padding within short-jump reach.
- **Treating `jmp [reg+disp]` as a tail call** (it leaves the function) would unblock 516 sites
  (47%). The other functions also contain `jmp reg`, which can't be told apart from an
  unresolved jump table. Expected gain: about half the ceiling (~0.8%), for a guest-code
  heuristic. Not worth the risk now; revisit if guest threads become the bottleneck.

### Notes for Tracks B and C (from the Track A agent, 2026-11-06)

Track B (draw state cache):
- Profile on `dev` 5864233b (perf LBR, GPU thread, on-CPU share). `GetGraphicsPrograms` 24%,
  of which `MaterializeResources` 16.5% and the JIT walker code ~10%. Walker time is mostly
  loads of guest tables freshly written by other cores. `ResolveTexture` 17.5%: `FindImage`
  13.4%, `ExactImageKey` hashing ~6–10%, `FindTextureDescription` 5.4%.
  `ResolveRenderColorTarget` 6.7%, `ResolveRenderDepthTarget` 2.3%, `CommitBindings` 5.3%, the
  NVIDIA driver 8.3%.
- Tracy: `DrawIndex` 7.7 µs mean over ~97k draws/s, about 75% of wall time.
- `FindImage` already caches exact keys (`m_exact_images` with registration generations). A
  memo of the result in each `m_texture_descriptions` entry, revalidated with
  `RegistrationsSince`, would skip the `ExactImageKey` build and hash.
- Not measured: whether user SGPRs (SRT pointers) repeat frame to frame (B0).

Track C (guest memory):
- `CopySparseMemory` from `ObtainBuffer`'s stream path (CPU-modified, read-only, ≤ one caching
  page) is 11–15% of the GPU thread. A lock rewrite didn't help (above). The cost is the memcpy
  of cold, freshly written guest data into the stream buffer.
- `pthread_rwlock_*` samples there are cache misses on the lock line plus the copy itself.
- Write-tracking faults: ~140–150k/s. `PrepareBda` uploads: ~700 `SynchronizeBuffersInRange`
  calls/s (earlier measurement).
- Frame delta (#1931): the upstream issue has no technical analysis; replies point to forks and
  VRAM. Nothing was measured yet.

### No CPU-side GPU syncs for indirect dispatches (A4, landed)

The indirect-dispatch syncs (~5k per 45 s window, ~0.95 s per 10 s of the command thread) come
from two compute shaders, hashes `0xc5ee521b8849cf83` and `0x25cdbea1f5cbfaa6`. They start like
this (decoded RDNA2):

```
S_BUFFER_LOAD_DWORD vcc_lo, s8, offset=28   ; element count, written by an earlier GPU pass (≈4)
V_CMP_LE_U32        vcc_lo, vcc_lo, v0      ; thread id >= count?
S_MOV_B64           exec_lo, !vcc
S_CBRANCH_EXECZ     end                     ; the whole body is skipped past the count
```

The shader reads the count itself, on the GPU (`ReadConstBuffer`, a bound buffer). The CPU-side
SRT walk only needs it to decide block reachability, and the walker already treats an unknown
branch condition conservatively (every successor reachable). The fix:
- During an indirect dispatch's program lookup (`SrtWalkerUnknownGpuWrites`), a walker load of
  bytes the GPU wrote, and that are not yet read back, evaluates as unknown instead of waiting.
- If a walk then fails because it needed such a value (for a descriptor or a flattened read),
  `MaterializeResources` repeats it once with the waiting load.

Same-batch A/B on `dev` fc4c96a2: 20.06 / 20.16 → 21.94 / 22.17 FPS (+9.7%); on `dev` +
read-only depth earlier, 18.62 / 18.94 → 21.45 / 21.49. Syncs in the level went from ~5k to
0–1; worst frame ~61 → ~55 ms; 1% low ~15.6 → ~18. Call-target walks (`RefreshShaderSource`)
keep waiting: a call target must be known.

### A2: remaining barriers and render passes (2026-11-06)

`KYTY_VK_COUNT=sites` on `dev` + read-only depth, per frame:

| Source | Per frame |
|---|---|
| Dispatches: `ShaderWriteHazardBarrier` before and `ShaderAccessBarrier` after each (~222 dispatches) | ~445 |
| `Image::Transit` (`CommitBindings` 176, `AcquireRenderTargets` 151) | ~345 |
| Buffer uploads, fills, copies (`SynchronizeBuffersInRange` 107, `CpOpDmaData` 91, downloads 25) | ~230 |
| Draw shader-write barriers left after the atomic batches | ~15 |
| Render passes | ~174 |

Dropping every dispatch barrier (an unsafe experiment) only moved GPU time from 20.3 to
19.1 ms/frame. A range-tracking dependency layer for dispatches is not worth its risk now.

What ends render passes (`KYTY_VK_COUNT=sites` with `vkCmdEndRendering`, per frame): ~119 come
from `Image::Transit` in `AcquireRenderTargets`, i.e. render-target switches; ~16 from
draw-path barriers; ~11 from dispatches; ~10 from uploads, downloads and fills. **A3 as planned
(moving transfers to a pre-pass) has little to gain**: transfers end ~10 passes per frame.

Where the GPU time goes (`KYTY_GPU_TIME=passes`, timestamps around each pass and dispatch;
~18.4 ms busy per frame at 19.8 FPS):

| | Per frame |
|---|---|
| Inside render passes (~174 passes, ~5.8k draws) | 11.0 ms |
| ↳ 1920×1080, 5 color targets (G-buffer), ~9 passes | 4.6 ms |
| ↳ 1920×1080, 1 color target, ~72 passes | 3.0 ms |
| ↳ 2048² shadow (depth only), ~5 passes | 0.8 ms |
| ↳ 1080p depth only, ~13 passes | 0.7 ms |
| Dispatches (~236) | 4.0 ms |
| Between them (transfers, barriers, idle) | ~3.4 ms |

That is ~2 µs of GPU per draw inside passes, high for this GPU. Ruled out as the cause:
- Redundant pipeline binds: 80% of `vkCmdBindPipeline` re-bind the same pipeline, but skipping
  them (an experiment) left GPU time unchanged.
- Occlusion queries: none are active in Sky Garden.
- Avoidable pass breaks: an experiment that resumed the previous pass when the next one used
  the same attachments found only ~16 of ~175 begins per frame that it could avoid.

The command thread rarely waits for the GPU now: `Scheduler::Wait` under `Process` is ~3.5% of
its time (Tracy), and comes from servicing guest-thread readbacks (Track C).

Root-only Nsight metrics would separate front-end from shading cost; not available tonight.

### Recording cost and GPU time per draw, Track A (2026-11-07)

**Recording on the command thread is ~10%, spread thin.** perf (LBR, 10 s, `dev` 458607ad at
~24 FPS): the NVIDIA driver is 9.6% of the thread. By call site: `vkCmdPushDescriptorSetKHR`
2.3%, `vkCmdBindPipeline` ~1.0% (80% of binds re-bound the bound pipeline), the descriptor-set
path for layouts too large to push (`DescriptorHeap::Allocate` + `vkUpdateDescriptorSets`) ~1.5%,
`bindVertexBuffers2`, `drawIndexed` and `pushConstants` ~0.3% each, `CommitBindings` itself
~1.7% self. ~70 ns per Vulkan call.

**Bind dedupe (landed).** `GraphicsBindings` (in `DynamicState`) keeps the graphics pipeline,
push-constant dwords and push-descriptor elements recorded last in the command buffer. A draw
binds only a different pipeline, pushes only changed dword ranges, and pushes only changed runs
of descriptor array elements (`dstArrayElement`) while the pipeline layout is the one last pushed
with (each pipeline owns its layout). Per second: pipeline binds 142.7k → 34.6k, push-constant
calls 146k → 79k, pushed descriptors 16.8 → 6.0 per push. FPS +1.2% (A/B above).
`KYTY_BIND_DEDUPE=0` turns it off; `KYTY_VK_COUNT` now also counts
`push_descriptor_elements`.

**GPU time in passes is per-draw vertex/front-end cost, not shading.** A local experiment
(`KYTY_EXP_DRAW`, not merged) with `KYTY_GPU_TIME=passes`, per frame at ~23 FPS:

| Draws | In passes | Dispatches |
|---|---|---|
| normal | 9.9 ms | 3.9 ms |
| not recorded (all state still recorded) | 1.0 ms | 6.1 ms¹ |
| 1×1 scissor (almost no fragments) | 8.3 ms | 3.3 ms |

¹ Dispatches that read the skipped draws' output change their work.

Fragment work is only ~1.6 ms per frame; ~7.3 ms is ~1.3 µs per draw that remains with nothing
rasterized. **It is vertex shading, not per-draw state:** with every draw recorded at zero
instances (all binds, pushes and dynamic state still recorded), passes take 0.7 ms per frame.

Per-draw GPU timestamps (a timestamp after each draw, grouped by draw shape; local diagnostic)
put 53% of in-pass time in the 18% of draws that switch pipelines (5.9 µs vs 1.1 µs per draw).
The same draw shape costs ~1.0–1.3 µs more after a switch (G-buffer draws with 512–4k indices:
0.58 → 1.87 µs). Only 0.1% of those switches keep the same VS and PS, so making blend, color
mask or topology dynamic would not avoid them: they are shader changes, and part of the cost is
the new material's own vertex work. Other heavy groups: mesh-shader draws (~16% of in-pass
time), full-screen passes (3–6 indices, 37–53 µs each), instanced draws with 9–64 instances.

**Recording thread (landed).** `DeferredRecorder` takes each draw's final Vulkan commands
(descriptor pushes and the non-push set update and bind, push constants, vertex/index binds,
dynamic state, the pass begin, the pipeline bind, the draw) as closures in a 4 MB SPSC ring, and
a recording thread pinned to an efficiency core records them while the command thread prepares
the next draw. `CommandBuffer::Handle()` drains the ring, so all other recording keeps its order.
`Image::Transit` has an overload that takes the command buffer only when it emits a barrier;
before it, `AcquireRenderTargets` drained every draw (4.3% of the thread waiting). After:
the driver is 5.0% of the command thread (was 9.1%), waits for the recorder 1.8%
(`EndRendering` before downloads and dispatches, `Submit`). On the SMT sibling of the command
thread's core the recorder costs 11% FPS; on an efficiency core it gains ~2%.
`KYTY_RECORD_THREAD=0` turns it off. Also: descriptor pools 4× larger (they refilled every ~300
sets with failed allocations; 19.6k non-push sets/s), and the GPU tick is read at most every
20 µs per draw.

**Dispatches cost ~11 µs each on the command thread** (~236/frame, 6.5%), almost all in
`PrepareBda` and `SynchronizeBuffer` (Track C).

### Draw state cache, Track B (2026-11-06)

**B0: per-draw state does not repeat; its shape does.** A walk memo keyed on a program's user
SGPRs (plus shader base and push-data cursor), validated by reloading every guest DWORD the walk
read, hit only **7–8%** of the ~11k graphics program lookups per frame (VS + PS per draw): 91%
of keys were new, 1–2% were known with changed memory. The hits were trivial walks (~1.5 loads);
a typical walk reads ~50 guest DWORDs, and validation has to reload all of them, so it saves
little even when it hits. A walk costs ~890 ns per lookup (~9.8 ms/frame). Comparing each lookup
with the previous lookup of the same program (`KYTY_DTC_DIAG`, a local diagnostic):

| Same as the program's previous lookup | Share |
|---|---|
| Specialization (selects the module) | 99.7% |
| Module | 99.2% |
| Samplers | 99.5% |
| Images (T#s) | 91% |
| Buffers (V#s), exact | 21% |
| Buffers, ignoring base-address bits | 86% |
| Flattened SRT | 53% |
| Whole snapshot seen before in this program, exact | 23% |

Buffer descriptors point at per-frame allocations, so walk outputs repeat rarely; images,
samplers and the selected module repeat almost always. This killed B1 as planned (cross-frame
hit rate < 50%; see `PLAN-60fps.md` for the re-plan).

**The walk is compute-bound, not load-latency-bound.** Timing one lookup's `FindModule`, then
`MaterializeResources` and the walk again right away (guest tables now in cache): vertex
programs (63k lookups/s) 1415 ns cold, 1249 ns warm, 582 ns for the warm walk alone; pixel
programs (85k/s) 421 / 355 / 173 ns. Warm costs nearly as much as cold, so the walker's code
(per-node thunk calls and state bytes) and the rest of `MaterializeResources` are the cost, not
cache misses on freshly written tables. Within `MaterializeResources` the JIT walker is 54%,
its own code 20%, `WalkedDescriptor` 4%.

**B2: texture resolve memo (merged).** Each stage module keeps, per image slot, its last
`ResolveTexture` result. A slot is reused when its T# words are unchanged and `TextureCache`
confirms the image `FindImage` returned as a kept exact match would be returned again: no image
was registered or unregistered on its pages since (global generation check first, then the page
stamps). A hit repeats `FindImage`'s side effects (LRU touch, color-clear materialization) and
skips `FindTextureDescription` and the exact-image hash lookup. Hit rate **89.6%** (9.4% changed
descriptors, 1% not reusable); `KYTY_DTC_VERIFY=1` resolved every hit again over a full bench
and found no disagreement. Texture resolve went from 16.7% to 8.7% of the GPU thread (perf,
10 s), and FPS +6.2%. The misses now cost more than the hits: a slot keeps only one descriptor,
and programs that alternate between textures miss every time.

| GPU thread, inclusive (perf, 10 s) | `dev` 582e4043 | + B2 |
|---|---|---|
| `GetGraphicsPrograms` | 23.7% | 26.4% |
| ↳ `MaterializeResources` (↳ `WalkSrt` + JIT walker) | 16.5% (11.0%) | 18.5% (12.4%) |
| Texture resolve (`ResolveTexture` / `ResolveStageTexture`) | 16.7% | 8.7% |
| ↳ `FindImage` (all callers) | 13.0% | 7.6% |
| `BufferCache::ObtainBuffer` | 13.2% | 14.0% |
| NVIDIA driver | 9.4% | 9.6% |

**B2b: render-target resolve memo (merged).** `ResolveRenderColorTarget` and
`ResolveRenderDepthTarget` rebuilt each target's description (tiling layout, `UpdateSize`) and
searched the texture cache every draw: 10.8% of the GPU thread on `dev` 277a708e, though the
`CB_COLOR*`/`DB_*` registers rarely change within a pass. Each color slot and the depth target
now keep their last resolve, keyed by the raw register bytes (plus the target mask and flags for
color), reused when `RefindExactImage` confirms the image. Depth and stencil test state is still
derived from the other registers each draw. 99.7% of color target resolves hit, verify clean.
Render-target resolve went from 10.8% to 5.6% of the thread; +3.8% FPS. `RefindExactImage` is
now 4.6% of the thread (texture and target hits each lock the texture cache); batching those
touches per draw is the next small step.

**Ruled out: two ways per texture slot plus one lock per stage (B2c).** Keeping each slot's two
latest resolves raised texture hits from 89.7% to 93.2%, and consecutive hits of a stage shared one
texture-cache lock. Four A/B pairs against `dev` 96c2d1c0: 25.00 → 25.15 FPS (+0.6%, inside the
noise). Texture resolve went from 10.6% to 10.0% of the thread. Not merged.

**Where the GPU thread goes after B2b** (perf, 10 s, `dev` 96c2d1c0, ~25 FPS, ~7.3 µs per draw):

| Path | Share |
|---|---|
| `GetGraphicsPrograms` | 30% |
| ↳ `MaterializeResources` (↳ JIT walker code 12%) | 21% |
| ↳ `PrepareProgram` (static vertex input decode) | 5% |
| `PrepareBindings` (↳ texture resolve 10%, ↳ `RefindExactImage` 4%) | 14% |
| libc `memcpy`/`memset`, self | 14% |
| ↳ under `CopySparseMemory` (stream copies, Track C) | 3.8% |
| ↳ under `DrawIndex`, `ExecutePreparedDraw`, `PrepareBindings`, `ResolveStageTexture`, target resolve | ~7% |
| NVIDIA driver | 11% |
| `CommitBindings` | 7% |

- **Large per-draw structs.** About 7% of the thread zeroes or copies draw state: each
  `TextureCache::ImageDesc` is ~1 KB (a 16-entry mip layout). `DrawRenderState` holds nine of
  them (eight color targets and depth), zeroed every draw although `ResolveRenderColorTarget`
  rewrites each target it uses. Each texture binding is also copied twice on its way into
  `PreparedBindings`.
- **The JIT walker.** Dumping each walker's code and mapping perf samples onto it: 445 walkers,
  the hot ones 30–80 KB of code for 100–340 SRT reads. The load stub takes 25% of walker time
  (the guest load itself 9%). The other 75% is spread across the per-node thunks (call, state
  byte checks, slot stores) with no single hot spot. Halving it needs a different code shape
  (straight-line evaluation of the reachable value graph instead of memoized thunks), not a
  local fix.

**The walker is front-end bound, not thunk-bound** (B1′ step 2, measured on #25). Thunk
mechanics take about half of the walker's samples: the state-byte `cmp` 15%, `ret` 13%, `jne`
12% and `call` 11%. A first cut emitted every value with a single reference inline, keeping
thunks only for shared values, Phis and control values. A side-by-side check against the
thunk-only walker over 17.8M walks found no difference. But it inlined little: walker code went
from 7.48 to 7.26 MB and call sites from 63k to 58k, because most values are shared (a
buffer's V# words feed every read through it). Same-batch A/B: 27.17 / 27.65 → 27.31 / 27.73
FPS, which is noise, and walker self time went from 11.8% to 11.5%. Not merged.

Hardware events show why. The thread is 34% retiring, 29% front-end bound (fetch latency 13%),
33% back-end bound (memory 24%) and 4% bad speculation. JIT walker code is 13% of the thread's
cycles but 35% of its L1i misses, and 4% of its L2 data misses. So the walker mostly waits on
instruction fetch: 446 walkers, 30–80 KB each and 7.2 MB in all, against a 2 MB L2. The guest
data it reads mostly hits. What would help is less walker code per walk, for example
computing a buffer's base and size once per V# instead of per read, or a denser thunk
encoding. Removing calls alone does not help.

### Tooling notes

- `perf record -e sched:sched_switch` records, but `perf script` fails here with "broken or
  missing trace data". Off-CPU time came from Tracy zones instead.
- RenderDoc 1.46 cannot run the emulator. It lacks `VK_KHR_wayland_surface` (use
  `SDL_VIDEO_DRIVER=x11`), and on X11 the emulator dies silently right after device creation.
  It is also launched by absolute path, so `^\./kyty_emulator` patterns miss it; match
  `^[^ ]*kyty_emulator --game`.
- Vulkan synchronization validation was not run: installing `vulkan-validation-layers` needs
  root, and the prompt went unanswered.
- GPU metrics need admin (`RmProfilingAdminOnly=1`). They are device-wide, so a root `nsys`
  sampling `sleep N` works while the emulator runs as the user.
- Keep a copy of `kyty_emulator_clang_lld.map` with each perf capture. A rebuild overwrites it
  and old captures no longer symbolize.
- `garden.sh` used to exit on a timeout (level never started) without stopping the emulator,
  which then kept holding the bench lock for everyone. It now stops its own emulator on any
  failure, by parent PID: a pattern `pkill -9` can kill the emulator of whoever takes the lock
  next, and did once.
- Do not pipe `garden.sh` through `head`. SIGPIPE kills the script before its cleanup and leaves
  the emulator running.
- `cmake --install` fails resolving the Qt launcher's image-plugin dependencies (`libheif`,
  `libjxrglue`). `kyty_emulator` is installed before that step, so benchmarking is unaffected.

### Game timing and the real-speed check (C3, 2026-11-07)

There is no PS5 to compare against, so the check for "the game runs at real speed" has to come
from inside the emulator. `KYTY_TIME_DIAG=1` counts the game's own clock and video-out calls
(only the guest entry points, not the emulator's internal ones), records their call sites, and
times every flip that reaches the screen. Sky Garden on `dev` 458607ad, per second:

| | Value |
|---|---|
| Flip mode | vsync (`mode1`), flip rate 0 (60 Hz), all flips submitted from the GPU (EOP) |
| Vblanks | 60/s, steady |
| Flips on screen | ~23.5/s, each **2 or 3 vblanks** after the previous one (33 or 50 ms; p50 3, p99 3) |
| `sceKernelGetProcessTimeCounter` | ~65k (≈ 2.8k per frame; ~50k from one site, `eboot+0xd30cec`) |
| TSC / process-counter frequency queries | ~15.5k |
| `sceKernelGetProcessTime` | ~630 |
| `sceKernelReadTsc` | ~230 |
| Flip status / vblank status / wait-vblank | 0 / ~0 / 0 |
| `usleep(0)` | **~1.85 M**, all from `eboot+0x114b315` |

What this means:
- **The game times itself with the process time counter, not with vblank or flip counts.** About
  55 call sites read a clock exactly once per flip; the first one disassembled (`eboot+0xe07df0`)
  converts a TSC delta into milliseconds per update stage. How the simulation step is derived and
  clamped was not traced further, because the check below doesn't depend on it.
- **The real-speed check is the flip cadence.** On a PS5 this game flips on consecutive vblanks.
  When every flip that reaches the screen is 1 vblank after the previous one (`flip_vbl` p99 = 1)
  at 60 flips/s, the game sees exactly the timing it sees on hardware, however it computes its
  delta. That is the definition of done, alongside the FPS columns.
- **FPS is quantized by vsync.** A frame shows after 1, 2 or 3 vblanks; "24 FPS" is a mix of
  30 FPS and 20 FPS frames. Speedups show up as frames moving from 3 vblanks to 2, then from 2
  to 1: a frame that takes 17 ms still costs two vblanks, so to reach 60 every frame must fit in
  16.7 ms, not just the average.
- **`usleep(0)` is a busy spin.** The emulator returns from `usleep(0)` immediately, so the game's
  worker threads spin there (~10 `tbb_thead`s at ~27% CPU each). Yielding on `usleep(0)` changed
  nothing (A/B, same batch: 24.41 / 24.22 FPS without, 24.17 / 24.23 with), and the hybrid-CPU
  pinning already keeps the spinners off the GPU command thread's core.

`dev` 458607ad (everything through #11): 24.20 / 24.41 / 24.22 FPS, worst_ms ~52–54, low1 ~19.

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
- `KYTY_TIME_DIAG=1` (costs ~1 FPS while on): one `TIMEDIAG` line per second: vblanks, flips on screen with their interval
  in ms and in vblanks (`flip_vbl`, the real-speed check), flip modes, the game's clock and
  video-out calls per second with their top call sites (`eboot.bin+0x…`), and the sites read
  about once per flip (`per_flip:`). `KYTY_TIME_DIAG=dump` also writes the code around every
  call site to `/tmp/timediag-<offset>.bin` (disassemble with `objdump -D -b binary -mi386:x86-64
  --adjust-vma=<offset-1024>`).
- `KYTY_MEM_DIAG=1`: one `MEMDIAG` line per second with `ObtainBuffer` outcomes, stream copies and
  their repetition, `SynchronizeBuffer` uploads by caller, write-watch changes, faults by thread
  kind, labels and submissions. `tools/astro-bench/c0-profile.sh <label>` adds per-thread CPU time,
  `perf stat --per-thread` and a `perf record` of every thread (`/tmp/c0-<label>/`).
- `KYTY_STREAM_VERIFY=1` checks every reused stream copy against the guest bytes (slow);
  `KYTY_NO_STREAM_REUSE=1` disables reuse.
- Env knobs: `KYTY_NO_PIN=1`, `KYTY_AMD_SKIP_RSQRT=1`, `KYTY_NO_SHADER_FASTREAD=1`.
- Draw state caches (Track B): `KYTY_DTC=0` turns them off, `KYTY_DTC_VERIFY=1` recomputes every
  hit and exits on a disagreement, `KYTY_DTC_STATS=1` prints `DTC` hit-rate lines once per second.
