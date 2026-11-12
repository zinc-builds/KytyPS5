# Astro Bot Sky Garden: performance findings (2026-11-06)

Host: Intel i5-14600K (6 P-cores = CPUs 0-11 with SMT, 8 E-cores = CPUs 12-19), RTX 4080 Super
16 GB (NVIDIA 615.78), 32 GB RAM, CachyOS, kernel 7.3. Game: Astro Bot PPSA21567 v01.019.000
(`~/Games/PS5/PPSA21567.zar`). Benchmark: `tools/astro-bench/garden.sh` — scripted route from
save slot 1 into Sky Garden (`aerial_garden`), 45 s average starting 25 s after `StartLevel`.
Since 2026-11-07 it boots straight into the level and measures game frames 1000–1600 after
`StartLevel` (see "Benchmark window" below): run-to-run noise is about ±0.1 FPS and one run per
configuration is enough. Before that, noise was about ±0.3 FPS with two runs per configuration.

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
| 2026-11-07 | Vertex shaders stop predicating every instruction on an always-set EXEC bit (Track G; same batch: ref `dev` a456fbcb 27.61 / 27.62; GPU 16.97 → 16.13 ms/frame, in passes 9.78 → 8.91) | 28.09 / 28.68 | 152.2k / 154.4k | 50.8 (max 99.9) | 19.8 |
| 2026-11-07 | Render targets tiled and detiled straight from and into the image (Track G; menu route; same batch: ref `dev` f2848f29 27.97 / 27.36, with `KYTY_GPU_TIME` 26.09 / 27.40; GPU 16.26 → 14.89 ms/frame, transfers 2.71 → 1.34; FPS unchanged, the command thread limits) | 27.63 / 25.85 | 150.1k / 141.1k | 53.0 (max 83.1) | 19.3 |
| 2026-11-07 | **Route changed:** `garden.sh` boots straight into the level (`GARDEN_ROUTE=lvl`, see "Benchmark route" below). Same binary (`dev` f2848f29 + `KYTY_APP0_OVERLAY`), same batch, menu route: 27.36 / 27.94, worst_ms 50.8 / 50.9, low1 19.7 / 19.5. Compare later rows with this one, not with the rows above | 27.89 / 28.24 | 151.2k / 153.3k | 51.1 (max 66.9) | 19.5 |
| 2026-11-07 | **Benchmark method changed:** frames 1000–1600 after level start instead of a 25 s + 45 s time window, warm-up run first, one run per configuration (see "Benchmark window"). `dev` 4516792f, four identical runs (worst_ms 47.0 / 48.5 / 46.1 / 46.4, low1 19.8 / 20.0 / 20.5 / 20.9). Compare later rows with this one only | 29.56 / 29.48 / 29.65 / 29.61 | 153.9k / 155.0k | 47.0 (max 68.1) | 20.3 |
| 2026-11-07 | Vertex shaders drop EXEC selects whose skipped lanes nothing reads (Track G; same binary, same batch, `KYTY_MASKED_SELECTS=0`: 29.53; in passes 9.14 → 8.93 ms/frame, GPU 15.14 → 14.77; FPS unchanged, the command thread limits. Re-measured after an external load overlapped the first runs) | 29.31 | 153.0k | 48.9 (max 65.4) | 20.1 |
| 2026-11-07 | Draw programs looked up on a worker thread from a copy of the registers, up to 4 draws ahead (Track B4, #37; re-run without the external build's load, same batch: `dev` 4d170b12 before it 29.56 / 29.21; 6.53 → 5.47 µs/draw) | 34.99 / 34.84 | 183.2k / 182.5k | 35.7 / 37.3 (max 50.1) | 28.6 / 28.4 |
| 2026-11-07 | SRT walker references skip the thunk call for values already known; short unknown exits, one-byte state offsets (Track B1′. Measured before B4, two pairs against `dev` 84e51e33 29.41 / 29.39: 30.15 / 30.32, walker 12.8% → 10.2% of the GPU thread. On top of B4, re-measured without the external build's load, two pairs against `dev` 204abd61 34.68 / 34.38: FPS unchanged, prepare worker 2.90 → 2.76 µs/draw, busy 52% → 49%) | 34.43 / 34.47 | 180.6k / 180.7k | – | – |
| 2026-11-07 | Indexed VGPR reads (`V_MOVRELS`) are a switch on M0, not a select per VGPR (Track G; same batch: ref `dev` 204abd61 34.46; in passes 8.90 → 8.43 ms/frame, GPU 14.79 → 14.38; against `dev` 4d170b12 earlier: in passes 8.93 → 8.53) | 34.80 | 181.7k | 40.9 (max 65.5) | 25.0 |
| 2026-11-07 | Compute and pixel shaders drop EXEC selects whose skipped lanes nothing reads (Track G; same batch: ref `dev` ae6d8c77 35.09; GPU 14.34 → 13.86 ms/frame, dispatches 3.68 → 3.39, in passes 8.42 → 8.23; FPS unchanged, the command thread limits) | 35.21 | 184.1k | 40.0 (max 66.6) | 26.1 |
| 2026-11-07 | Consecutive dispatches the guest lets overlap share their barriers (Track G3; same batch: ref `dev` ae6d8c77 34.81; with `KYTY_GPU_TIME=passes`, GPU 14.14 → 13.29 ms/frame, dispatches 3.68 → 3.04, barrier gaps 0.80 → 0.54) | 35.19 | 183.4k | 38.5 (max 50.6) | 27.4 |
| 2026-11-07 | Game-created threads on the efficiency cores, the game's render thread and the emulator on the performance cores (same batch: ref `dev` e5ef1e84 35.03 / 35.27, worst_ms 39.0 / 38.5) | 36.02 / 36.45 | 188.8k / 191.7k | 33.5 (max 33.5) | 28.7 / 29.9 |
| 2026-11-07 | Large buffer uploads cross PCIe on the copy engine into a device-local ring; the universal queue copies from there at VRAM speed (Track G4; same batch: ref `dev` abc268e0 35.01; `KYTY_GPU_TIME=1` GPU 13.04 → 12.42 ms/frame; with `=passes` 13.25 → 12.47, transfers 1.35 → 0.58; FPS unchanged, the command thread limits) | 35.28 | 185.0k | 38.3 (max 51.0) | 26.7 |
| 2026-11-07 | `PrepareBda`'s uploads share one barrier pair (Track G4; same binary, `KYTY_UPLOAD_BATCH=0`: 35.92; `KYTY_GPU_TIME=1` GPU 12.30 → 12.19 ms/frame, barrier gaps 0.53 → 0.45) | 35.84 | 188.7k | 33.5 (max 33.5) | 29.9 |
| 2026-11-07 | Guest read faults on GPU-written memory no longer stall the command thread: it records and submits the download in PM4 order and moves on, the faulting thread waits (Track B; same batch: ref `dev` c7600e89 36.40 / 36.11; `KYTY_STALL_DIAG` command-thread time in readbacks 9.4–10.2% → 0.8%, of it on the GPU 8.4–9.3% → 0.0%; a single longer frame per run, max 50–83 ms vs 33.8) | 38.99 / 38.90 | 204.3k / 204.8k | 35.6 (max 66.7) / 35.6 (max 50.0) | 27.8 / 28.7 |
| 2026-11-07 | Guest reads inside one backing mapping copy without locks: a per-thread translation cache checked against a sequence count that backing-store writers make odd (same batch: ref `dev` 75f3eebc 38.97, worst_ms 33.7) | 39.56 | 208.0k | 33.5 (max 33.6) | 29.2 |
| 2026-11-07 | Per-draw fences off the command thread: lookups of unchanged images skip the texture cache's lock (a per-image atomic "settled" state that CPU write faults clear), retired-operation checks skip the scheduler's mutex, recorder hand-offs skip the full fence, overlay counters are per thread (Track B; same batch: ref `dev` ab1429f1 39.21 / 39.52; before #55, on 75f3eebc +1.5% and on b4a91152 4 pairs +1.9%; samples right after a locked instruction 8.1% → 1.1% of the command thread; `KYTY_TC_SETTLED_VERIFY` 104M checks clean) | 39.98 / 39.78 | 209.3k / 209.9k | 33.5 (max 33.6) / 33.5 (max 33.7) | 28.7 / 28.6 |
| 2026-11-08 | The last per-draw fences: the render context mutex is biased to the command thread (other threads force a `membarrier` and wait; `KYTY_RENDER_MUTEX_BIASED=0` turns it off), `FindDepthTarget` skips the texture cache lock for settled images, `ClampRangeSize` remembers answers per thread until the ranges change (Track B; same batches: ref `dev` ee714f06 40.70 / 40.83 / 40.68 / 40.46; settled verify 106M checks clean) | 40.78 / 41.06 / 41.01 / 40.99 | 214.2k / 215.1k / 215.8k / 215.6k | 33.5–35.7 (max 33.6–66.7) | 28.7 / 29.9 / – / – |
| 2026-11-08 | The prepare worker looks up the draw's clean buffers too: storage buffer sources and vertex ranges, validated at the commit by the buffer registration sequence and a global CPU-dirty generation (Track B4 item 2; same batch: ref `dev` 1ed41724 40.55, worst_ms 33.5; buffer acquisition 668 → 519 ns/draw on ab1429f1, worker 2.33 → 2.57 µs/draw, 51% busy) | 42.13 | 221.0k | 33.5 (max 33.6) | 29.9 |
| 2026-11-08 | Texture resolves proposed by the prepare worker from its own resolve memo (one record per resource and T#, description precomputed there); the command thread keeps the registration check, LRU touch and color-clear work, and resolves misses with the worker's description (Track B; same binary, `KYTY_WORKER_TEXTURES=0`: 41.01 / 41.34; `KYTY_WORKER_TEXTURES_VERIFY` 84M checks clean) | 41.41 / 41.96 | 217.9k / 220.1k | 33.5 (max 33.5) / 33.5 (max 33.6) | – |
| 2026-11-08 | The prepare worker's texture memo is a flat open-addressed table instead of `std::unordered_map` (Track B; same batch: ref `dev` 78fa7f97 41.03 / 41.29; worker 2.90 → 2.80 µs/draw, command thread's wait for it 316 → 223 ns/draw, `KYTY_PREPARE_STATS`; verify 81M checks clean) | 42.18 / 42.42 | – | – | – |
| 2026-11-08 | Texture bindings keep the worker record's desc when the record has not changed instead of copying ~1.1 KB per binding; record generations refresh in place instead of a 3.7 KB copy (Track B; same batch: ref `dev` 4a7f63c4 42.76 / 42.88; worker verify 82M checks and `KYTY_DTC_VERIFY` clean) | 43.62 / 43.57 | 228.8k / 229.1k | 33.5 | – |
| 2026-11-08 | Two prepare workers, draws split alternately, the second on its own performance core (CPU 6, taken from the emulator's set); each keeps its own texture memo and SRT walk scratch (Track B4, plan item 2; same batch: ref `dev` 94c0492e 43.54 / 43.62; same binary with `KYTY_PREPARE_WORKERS=1` 43.17 → 44.66, command thread waiting for the worker 7.7% → 2.5%) | 44.92 / 44.41 | 235.7k / 233.5k | 33.5 (max 34.0) / 34.7 (max 50.0) | 27.2 / 27.3 |
| 2026-11-08 | One texture memo for both prepare workers instead of one each: lock-free lookups, records filed by compare-and-swap, a replaced record or table freed two rings later (Track B; same batch: ref `dev` 26c29910 44.64; same binary with `KYTY_PREPARE_STATS=1`, `KYTY_SHARED_TEXTURE_MEMO=0` 43.64 / 43.86 → 45.49 / 45.11; the command thread's per-page re-checks of adopted records 84k → 51k/s (one worker: 48k/s), worker 3.2 µs/draw unchanged; `KYTY_WORKER_TEXTURES_VERIFY=1` 83M checks clean) | 45.30 | 238.4k | 38.2 (max 83.4) | 27.9 |
| 2026-11-08 | Per-draw struct zeroing and copies on the command thread: vertex bindings (772 B zeroed, then copied from a return value) are filled in place, the merged vertex ranges are no longer zeroed (~2.8 KB per draw on the command thread and again on the worker), and the last-pipeline check compares the key's used bytes (~200 of 516) (Track B4; same batch, 4 pairs: ref `dev` 23191d32 45.93 / 45.25 / 45.53 / 45.25, mean 45.49; +0.5%, at the noise floor, 3 of 4 pairs up) | 45.48 / 45.67 / 45.97 / 45.80 | 238.4k / 239.3k / 241.4k / 240.4k | 35.9 (max 66.6) / 33.4 (max 33.5) / 33.5 (max 33.7) / 33.5 (max 34.1) | 27.3 / 29.9 / 28.6 / 28.8 |
| 2026-11-08 | Color targets proposed by the prepare worker: it decodes each target from the queued draw's registers (or reuses a per-slot record) and writes it into the draw's state, so the command thread no longer copies the ~1 KB `RenderColorInfo` per target; it keeps the registration check (generation refreshed in place), binding, and `FindImage` for targets it has not resolved yet (Track B4; same batch: ref `dev` 23191d32 46.12 / 45.95; `KYTY_WORKER_TARGETS_VERIFY` 22M checks clean) | 46.47 / 46.72 | 244.2k / 244.4k | 33.5 (max 34.0) / 34.8 (max 50.0) | 29.8 / 29.1 |
| 2026-11-08 | Depth target proposed by the prepare worker, like the color targets: it decodes the depth/stencil state and reuses a per-register record for the description, written into the draw's state; the command thread keeps the registration check, binding and `FindImage` for new targets (Track B4; same batch: ref `dev` 700d3924 46.75 / 46.20; `KYTY_WORKER_TARGETS_VERIFY` clean) | 47.33 / 47.11 | 248.7k / 247.8k | 34.7 (max 50.1) / 33.5 (max 33.6) | 29.9 / 28.4 |
| 2026-11-08 | The draw atomic-write batch keeps its guest ranges in sorted vectors instead of `std::map`s: 15% of draws run the batching code, and each added ~13 ranges to a set of ~690 (8.4 already covered), ~1,770 TSC cycles per such draw in tree lookups and node allocations (Track B4; same batch, 4 pairs: ref `dev` c65c7dd8 47.13 / 47.92 / 47.90 / 47.70; +0.7%, all 4 pairs up) | 47.41 / 48.23 / 48.05 / 48.30 | 248.7k / 253.0k / 250.6k / 252.4k | 37.3 (max 66.7) / 33.5 (max 33.5) / 33.5 (max 33.5) / 33.5 (max 33.8) | 27.6 / 29.9 / 28.5 / 28.3 |
| 2026-11-08 | Stage binding-set reuse: a stage whose module, worker texture records (by serial) and sampler values equal the last draw's keeps its images, views and samplers while the texture cache's binding epoch and GC tick hold; the color-clear step, the image refresh and tracking (`AcquireTexture`, `FindTexture` without the view) and the per-draw bound marking still run (~75% of stage sets) (Track B4; same batch: ref `dev` c7bdab8f 48.08 / 48.18; `KYTY_BINDING_REUSE_VERIFY` 12M stages clean) | 49.90 / 49.62 | 261.3k / 261.1k | 33.5 (max 33.6) / 33.5 (max 33.6) | 29.9 / 29.9 |
| 2026-11-08 | Small cuts on the command thread: the texture proposal carries its record's serial and color-clear flag, so a reused stage reads no record (nor restores the layout `CommitBindings` sets); `IsMetaCleared` reads the GPU-thread-only metadata map without the cache lock (`KYTY_META_LOCK=1` restores it); images and buffers mirror their LRU tick and skip `Touch` when already touched this GC tick (`KYTY_LRU_MIRROR=0` disables; `Touch` was already a no-op then, so eviction order is unchanged); the vertex-range merge runs on the prepare worker (`KYTY_WORKER_BUFFERS=verify` merges again and compares) (Track B4; same batch: ref `dev` f1386607 48.43 / 49.27; `KYTY_DTC_VERIFY` + `KYTY_PREPARE_VERIFY` and the buffer, texture and binding-reuse verify modes clean) | 48.53 / 49.83 | 256.5k / 261.7k | 33.5 (max 33.6) / 33.4 (max 33.5) | 29.9 / 29.9 |
| 2026-11-08 | Pipeline key on the prepare worker: the worker builds the graphics pipeline key and its hash from its programs, proposed targets and the queued registers; the command thread checks the final targets (formats, samples, slots, export mappings, depth, topology, restart) and looks the pipeline up with the worker's hash (hash first on the last-pipeline check, last key kept as a pointer, keys hash only their used bytes). Command thread `GetGraphicsPipeline` 4.1% → 2.4% (~162 → ~95 ns/draw); 99.6% of draws use the worker's key, 0 target mismatches, `KYTY_WORKER_PIPELINE_KEY_VERIFY` 8M checks clean. Worker time counts: a first version without the used-bytes hash and the prefetch was −0.5% (the same binary building the key but ignoring it, −2.4%) (Track B5; same batch, 2 pairs: ref `dev` 93ac4f97 49.48 / 50.00; +0.9%, both pairs up; earlier bases +0.7% on f1386607, +1.4% on c7bdab8f) | 50.07 / 50.33 | 263.1k / 259.1k | 33.4 (max 33.5) / 38.5 (max 99.5) | 29.9 / 26.9 |
| 2026-11-08 | `KYTY_BDA_STATS` diagnostic for `PrepareBda`'s uploads, off by default (Track B; findings under "Buffer uploads": the bytes are real, DMA shaders read almost none of them and 56% are replaced before any overlapping bind, so pointing the BDA page table at guest memory (C1) could remove the eager copy, with the binds' share to be measured on the prototype; with the diagnostic off, same binary after an access run 45.28, `dev` 23191d32 45.30) | 45.28 | 238.0k | 37.0 (max 83.4) | 27.3 |
| 2026-11-11 | The emulator's context-state NOP (push/pop/clear of the context registers) keeps the draw queue, like a register write: queued draws hold their own register copy, and it touches no memory and signals nothing (`KYTY_CONTEXT_STATE_DRAIN=1` drains again). `KYTY_DRAIN_STATS=1` attributes queue drains and the prepare waits they cause (while draining, and refilling after) by packet: all drains cost ~2.1% of the command thread (RELEASE_MEM 0.8%, EVENT_WRITE 0.5%, context state 0.24%, ACQUIRE_MEM 0.2%, guest commands 0.2%, dispatches 0.15%); `KYTY_STALL_DIAG` roughly doubles the measured prepare wait (217 vs 101 ns/draw) (Track B4; same batch: ref `dev` e0999e89 47.93 / 48.64; `KYTY_PREPARE_VERIFY` clean) | 48.38 / 49.46 | 252.5k / 260.5k | 38.5 (max 50.1) / 36.2 (max 50.1) | 28.4 / 29.6 |
| 2026-11-11 | `KYTY_KERNEL_STATS` diagnostic: the GPU command thread's kernel time by call site, off by default (Track K; findings under "Kernel time on the command thread": ~12% of the thread is sys time, half of it `PAGEMAP_SCAN` walks for async write collection and label stores; same batch: ref `dev` 084ca762 49.70, this build with the diagnostic on 49.86) | 49.70 | 261.0k | 38.5 (max 50.0) | 27.6 |
| 2026-11-11 | `DrawPrepare::RangedStore` for emulator stores between queued draws (for an ordered RELEASE_MEM item) and a public `CommandProcessor::StoreLabel`; the color-clear expansion uses it (image download publishes stay unranged: they run on the priority thread, where read watches may go while they store). A store is ranged only if the SRT walker logs reads of every page it writes (read-watched pages); the walker reads other pages in place, unlogged, so stores there still drop every queued draw. Most texture publishes land on unwatched pages: prepare hazard drops ~21 → ~15 per frame (ranged without that rule: ~1.8, and unsound) (Track B; same batch, 2 pairs: ref `dev` 85eb0cb6 49.09 / 49.15; +0.3%, neutral; `KYTY_PREPARE_VERIFY` clean) | 49.34 / 49.15 | 259.6k / 257.2k | 33.5 (max 33.9) / 33.4 (max 33.5) | 28.5 / 29.9 |
| 2026-11-12 | The collection of guest writes at each fresh submission's start runs on the submitting guest thread before it enqueues (`KYTY_SUBMIT_SCAN=0` off; Track K; findings under "Kernel time on the command thread": command-thread sys time `cmd_sns` 447 → 396 ns/draw (−11.4% ± 1.3), collection ~60 ns/draw, `DrawThread` pays 15–17 ms/s and `FluidCalculatio` 7–9; gate.sh, same binary, 3 runs a side: off 48.94 / 48.69 / 48.90, FPS +1.26% ± 0.37; `KYTY_ASYNC_WP_VERIFY` 0 lost writes, and it reports dropped kept writes) | 49.52 / 49.40 / 49.89 | 259.5k / 260.2k / 262.3k | 45.0 (max 83.3) / 37.3 (max 50.0) / 34.7 (max 50.0) | 23.3 / 29.1 / 29.9 |
| 2026-11-11 | SRT walker page log: on the prepare workers the walker records the 4 KiB pages of every guest load (a load crossing a page logs both; ~3.2 distinct pages per draw, at most 7), so every worker read of guest memory is logged and every emulator store between queued draws is ranged (`RangedStore`, buffer readbacks); a queued draw is dropped only if a store since its post, or one still open, overlaps its reads or pages (`OpenStoredRange`/`CloseStoredRange`; a store open at post time used to escape). Prepare rejections from emulator stores ~715/s → 0, ~700 draws/s kept by the range check; the ~240/s left are GPU-written bytes (gate < 300/s met); prepare wait and worker time unchanged (~100–140 and ~3.55 µs/draw), so FPS is flat: the wait is slow-draw latency, not rejections. `KYTY_WALKER_PAGE_LOG=0` restores the read-watched-only rule (Track B4; same batch: ref `dev` 85eb0cb6 48.93 / 49.62; earlier ABBA against the kill switch +0.25%; `KYTY_PREPARE_VERIFY` + `KYTY_WORKER_BUFFERS=verify` + `KYTY_WORKER_TEXTURES_VERIFY` 93.7k adopted draws clean) | 49.60 / 49.01 | 257.8k / 257.2k | 41.3 (max 133.1) / 41.1 (max 66.7) | 27.2 / 26.5 |
| 2026-11-11 | Ranged GPU-write hazard: guest bytes newly marked GPU-written (`ObtainBuffer` for a written buffer) are recorded by range, like emulator stores, and drop only the queued draws whose logged reads or walker pages they overlap, instead of every queued draw. Sound because the mark comes after the pages turn read-watched, so later worker reads are logged and checked by `HasGpuDirtyBytes`; buffer, texture, target and pipeline-key proposals never read GPU-written state (they keep their own checks). Prepare rejections 265/s → ~1/s (all hazards gone; ~95 draws/s kept by the range check), prepare wait unchanged within noise (`KYTY_RANGED_GPU_HAZARD=0` restores the generation) (Track B4; same batch: ref `dev` 85eb0cb6 49.82 / 48.61, kill switch 50.06 / 49.73, +0.5% over it; `KYTY_PREPARE_VERIFY` + `KYTY_WORKER_BUFFERS=verify` + `KYTY_WORKER_TEXTURES_VERIFY` 93.5k adopted draws clean) | 50.01 / 50.24 | 262.6k / 263.2k | 33.5 (max 33.7) / 33.5 (max 33.9) | 29.9 / 29.9 |
| 2026-11-11 | Two texture-cache cuts on the command thread: a depth+stencil target whose image and stencil plane are unchanged since the last acquisition skips the cache lock, `AssociateStencil` and both refreshes (`KYTY_STENCIL_TARGET_FAST=0|verify`; `FindDepthTarget` 1.46% → 0.73% of the thread); `MaterializeColorClear` remembers a call that had nothing to do and skips the metadata reads while the guest-sync epoch, the prepare hazard generation and the buffer cache's color-metadata check generation hold (`KYTY_COLOR_CLEAR_MEMO=0|verify`; 2.41% → 1.66%) (Track B; same batch, 4 pairs: ref `dev` 85eb0cb6 47.83 / 46.26 / 49.05 / 49.01, mean 48.04; +1.8%, 3 of 4 pairs up; both verify modes clean in game and in the tests) | 47.28 / 48.41 / 49.82 / 50.03 | 248.9k / 254.6k / 259.9k / 264.2k | 33.6 (max 33.9) / 33.7 (max 34.0) / 33.5 (max 33.8) / 33.5 (max 33.5) | 29.1 / 28.3 / 29.8 / 29.9 |
| 2026-11-12 | **Build mode:** PGO + ThinLTO, no source change (`KYTY_BUILD_MODE=pgo`, profile trained on this bench by `pgo-train.sh`; Track P). Same batch, plain build of the same commit (`dev` ffcface5 + the tooling): 48.34 / 48.94, worst_ms 42.6 / 33.5. Command thread: instructions/draw 27.5k → 20.1k (−26.9% ±0.2), cycles/draw 16.9k → 14.7k (−13.4% ±0.8), FPS +11.1% ±0.9. Rows from here on say which mode they were measured in; the protocol is "Build modes" in PLAN-60fps.md | 53.89 / 54.21 | 282.1k / 284.6k | 50.2 (max 199.7) / 34.8 (max 50.0) | 27.4 / 28.6 |
| 2026-11-12 | Free lanes (`DrawPrepare::Pool`): with two prepare workers, each worker takes the oldest draw nobody has taken, instead of every second draw, and the queue defaults to 8 draws ahead (`KYTY_FREE_LANES=0` restores alternation at 4). The command thread's prepare wait (user-mode spinning) falls ~140–170 → ~100 ns/draw, and the cycles it saves show up 1:1 in command-thread cycles/draw: same-binary A/Bs −2.67%±0.47 and −1.77%±0.47 cycles/draw (instructions unchanged, command-thread clock 5.08–5.11 GHz in every run, workers 5.07), FPS +1.9% and +2.5%. Either part alone is noise (free lanes at depth 4: −0.17% cycles; alternation at depth 8: −0.51%): with alternation the extra depth queues behind a slow draw, and without the depth a free worker has nothing to run ahead on. The earlier "wait fell, FPS flat" readings were FPS noise between separate runs and binaries, not a mechanism. Not kept: a third worker (+0.96% cycles/draw, it takes a performance core); the workers' SMT siblings left free (`KYTY_FREE_PREPARE_SIBLINGS`, branch trackB/prep-siblings: workers 4–7% faster, command thread unchanged); `KYTY_PIPELINE_NICE=-10` (no change: in the steady window the pipeline threads wait ≤ 2 ms/s for their CPU, and what runs on their cores is mostly kernel threads) (Track B4; gate.sh: ref `dev` 353a51a1 50.03 / 50.62, verify clean) | 51.11 / 50.78 | 268.0k / 266.4k | 36.2 (max 66.7) / 33.5 (max 33.6) | 27.4 / 29.9 |
| 2026-11-12 | Stream copies on the prepare workers (#78): for small CPU-dirty buffer ranges the worker copies the guest bytes into a ring of its own (or reuses an earlier copy of the same range at the same guest-sync epoch and CPU-dirty growth), and the command thread adopts the copy at commit after checking stores, GPU writes and CPU-dirty growth since; ~480k copies/s adopted (~960 MB/s, ~75% reuses, ring never full). Same binary through `KYTY_WORKER_STREAM=0`: command-thread cycles/draw −1.80%±0.47, instructions/draw +0.99% (the wait's spinning: worker time per draw 3.76 → 4.19 µs, prepare wait +48 to +75 ns/draw), FPS +1.15%±0.39. Measured flat under the old FPS-pair procedure. `KYTY_WORKER_STREAM=verify` re-copies and compares (Track B4; gate.sh: ref `dev` 506f3f22 51.00 / 51.23, verify clean) | 51.42 / 51.29 | 270.2k / 269.7k | 33.5 (max 33.6) / 33.6 (max 34.3) | 27.4 / 29.8 |
| 2026-11-12 | Not kept (Track B4, same-binary A/Bs on dev 506f3f22): #82's register-copy skip (`KYTY_POST_COPY`), cycles/draw −0.27%±0.47: register and shader copies still happen on 100% of draws, and `KYTY_POST_COPY=verify` found a context write that bypasses `GetCtx`. Earlier, under FPS pairs: per-unit materialization trims (`t_walked_plan` scratch cache, `Program::last_found`), worker prepare time −3%, FPS flat (branch trackB/mat-trims); RELEASE_MEM as an ordered queue item, no gain: its drain moves to the next cache operation (EVENT_WRITE/ACQUIRE_MEM/WAIT_REG_MEM always follow it in Sky Garden) (branch trackB/release-mem-item) | – | – | – | – |
| 2026-11-12 | `FillBuffer` fills CPU-owned guest memory through the backing alias and invalidates the range itself, instead of writing through the guest mapping and taking write faults (`KYTY_FILL_BACKING=0` restores it; Track K; same binary, 3 runs a side, frames 1000-1600: command-thread sys time 404 → 359 ns/draw (−9.4% ± 3.0), `cmd_ns` −0.2% (noise); `KYTY_ASYNC_WP_VERIFY` 0 lost) | 52.77 / 52.91 / 52.88 | | | |
| 2026-11-12 | Submit scans no longer make the GPU thread wait: a submit scan finds written pages without re-protecting them, then, in one hold of the kept-writes lock, advances those pages' label tokens, re-protects exactly those pages and keeps what that finds; label peeks and kept-write collection need no submit-scan mutex (`KYTY_SUBMIT_SCAN_SERIAL=1` restores #90's mutex; Track K; PGO, same binary, 3 runs a side: command-thread voluntary context switches 5.63 → 4.91 per 1k draws, `cmd_ns` −0.65% ± 0.74, FPS +0.03% ± 0.58 (the frame waits on the GPU until early submit lands); `KYTY_ASYNC_WP_VERIFY` 0 lost, `DROP_KEPT=1000` reports 26) | 55.50 / 55.14 / 54.84 | | | |
| 2026-11-12 | Hand-off stage A, shared register snapshots: a queued draw points at a command-thread-owned `HW::Context` snapshot instead of carrying a 2.7 KB copy; consecutive draws share one while no written register line changed (89% of draws in Sky Garden). Setters mark the lines they write with flag bytes and the post compares only those lines with the previous snapshot (`m_register_lines_changed`, also for Track B's run check). Per draw, same binary through `KYTY_REGISTER_SNAPSHOT=0` (3 runs a side): command-thread cycles −3.26%±1.02, cross-core snoop-hit loads 42.5 → 38.0 on the command thread and 29.7 → 24.6 on the workers, L1-missing loads 385 → 347. Against dev (two binaries): cycles −2.54%±1.14 and −1.21%±0.76 in two batches, instructions +0.8% (the marking). Two first versions lost the gain: a compare in every setter, then a mask word every setter read-modify-wrote (a store-forwarding chain through runs of register writes). The 55-cycle heuristic undercounts: −4.5 snoop loads would be ~250 cycles, the copy's stores and L1 misses account for the rest (Track B4; gate.sh: ref `dev` e1ad8deb 52.01 / 52.01; `KYTY_REGISTER_SNAPSHOT=verify` + `KYTY_PREPARE_VERIFY` clean) | 52.84 / 52.57 | 275.1k / 275.4k | 38.5 (max 98.5) / 33.5 (max 33.6) | 26.9 / 29.9 |
| 2026-11-12 | A CP label store skips its one-page `PAGEMAP_SCAN` when the label's page still holds a current token (its marks were cleared when that token was made; stores since stay marked, so the next peek collects and the next scan reports them): label-store scans ~67k → ~5.9k/s, ~70 more collections/s from peeks that now see a store (`KYTY_LABEL_SCAN_SKIP=0` scans always; Track K; gate.sh, same binary, 3 runs a side: `cmd_ns` −2.4% ± 0.5, `cmd_sns` −20.4% ± 1.8, FPS +2.0% ± 0.6; `KYTY_ASYNC_WP_VERIFY` 0 lost). Not done: one lock for the label store's protect and backing store (~4 locked operations per store, ~4 ns/draw, no system calls) | | | | |
| 2026-11-12 | A resumed submission no longer scans for guest writes at its start when the packet it stopped at collects on its own once satisfied (WAIT_REG_MEM, MEM_SEMAPHORE wait, REWIND; a predication wait still scans): ~520 scans/s fewer on the GPU thread (`KYTY_RESUME_SCAN=1` scans; `=verify` counts the write-watched pages the skip defers, ~175k/s, written while blocked, collected at the wait's release or the next scan; Track K; a pending collection request still forces a full scan; gate.sh, same binary, 3 runs a side: `cmd_ns` −1.3% ± 0.4, `cmd_sns` −19.2% ± 2.6, FPS +2.0% ± 0.7; `KYTY_ASYNC_WP_VERIFY` 0 lost) | | | | |
| 2026-11-12 | Early submit (`KYTY_EARLY_SUBMIT`, Track W): where a packet already drains, the open command buffer is submitted if the GPU has finished everything earlier and the buffer is ≥300 µs old, so the end-of-frame GPU work no longer waits for the guest's command buffer to end. Same binary, `KYTY_EARLY_SUBMIT=0` as reference, PGO, frames 1000–1600 (gate.sh): FPS +5.00% ±0.39 (ref 55.66 / 54.32; on dev ab68b255+#107; +3.62% on a6a007df), `cmd_ns` +0.4% (noise; ~5.5 extra submits per frame); command-thread GPU wait 0.62 → 0.08 ms/frame, flip completion 4.25 → 2.39 ms after decode | 57.92 / 57.56 | 303.3k / 299.7k | 33.5 / 36.2 (max 66.8) | 27.2 / 27.4 |
| 2026-11-12 | Per-draw scheduler retirement no longer waits for the priority-operation thread (#112, Track W; `KYTY_RETIRE_NOWAIT=0` restores): when a free tick's normal operation still has a priority operation of that tick or earlier queued or running, it stays queued for a later call; explicit `PopPendingOperations` callers still wait. In every such wait the GPU had already finished the tick. Same binary, PGO, frames 1000–1600 (gate.sh): `cmd_ns` −1.11% ±0.37, FPS −0.39% (noise; the freed time became waiting for guest work, `no_work` 4.4–7.9%); blocking `WaitPriorityOperations` 13.9 → 0.1–0.6 ms/s | – | – | – | – |
| 2026-11-12 | Hand-off stages after A, not kept (Track B4; PMU builds, frames 1000–1600, 3 runs a side). **B**, slot laid out by writer (command-thread post fields, the worker's ready side with the pool's done flag moved into the slot, worker bulk, commit-side vectors on separate lines) and `Pool::Post` reading `m_spinning` only when a worker sleeps: command-thread snoop-hit loads 37.2 → 36.5 per draw (estimate −4 to −5), workers 23.4 → 24.9, cycles/draw −1.51%±0.80 against dev (no switch; noise), so not merged (branch trackB/slot-header). **D**, `FindBuffers` swapping the worker's buffer-source vectors instead of copying them: cycles +0.06%±0.55, `cmd_ns` +0.27% (same binary, `KYTY_BUFFER_SOURCES_SWAP=0`); the copy's cost was reading the worker-written lines, which the swap still does (branch trackB/handoff-copies). **C** not built: a cycles profile with LBR callers puts all hand-off copies left on the command thread at ~1.5% of its cycles (`FindBuffers` ~0.8%, texture binding assignment ~0.6%, `PostQueuedDraw` ~0.2%); the large `color_info`/`depth_info` copies are the worker's stores, which the command thread only reads. As Track R's prefetch showed (snoop-hit loads −26–29%, cycles flat), loads that only arrive from another core are mostly overlapped; stage A paid because it deleted a copy, its stores and ~38 L1 misses. The command thread's largest copy is now `BufferCache::UploadCopies` (CPU-dirty buffer uploads through `CopySparseMemory`/`TryReadFast`), ~5% of its cycles, and the prepare wait's spinning in `Pool::Wait` ~6% | – | – | – | – |
| 2026-11-12 | Cache-operation drains: **parked, not built** (Track B4; diagnostics `KYTY_CACHE_OP_KEEP`, `KYTY_GAP_STATS` on branch trackB/drain-rule, Track B's probe on trackB/drain-probe). The guest synchronizes in one cluster between draw groups, typically `EVENT_WRITE` PS_PARTIAL_FLUSH and writebacks, `WRITE_DATA`, `RELEASE_MEM`, `WAIT_REG_MEM` on that label, `ACQUIRE_MEM`, then a decode-time indirect register load, and each of these packets drains. Signatures of the packets between consecutive queued draws (1.5M gaps): 80.7% hold nothing that drains, 18.1% only decode-time reads; drains 12.46 per 1,000 draws. Keeping the queue across cache operations alone: 12.46 (measured same binary, 3 runs a side: drains/kdraw 12.18 → 12.20, `cmd_ns` −0.15%±1.4, `wait_prep` −3.5%±4.7, all noise; the drain moves to `WRITE_DATA`); plus `WRITE_DATA`/`RELEASE_MEM` as ordered queue items: 11.80; plus a `WAIT_REG_MEM` on a label a pending item stores, resolved against the item's value: 8.60 (−31%). Left: guest commands 2.5, dispatches 2.4, occlusion dumps ~1.6 per kdraw. Soundness: GPU-written pages are read-watched, so a decode-time read of a committed draw's write faults and downloads; only still-queued draws are uncovered. A decode-time read follows the cluster in 8.2 of the 12.5 drains per kdraw, so a kept cluster needs a read check on almost every occurrence: (A) wait for the pre-mark draws' preparation and drain only if the read meets their prepared write set, or (B) a write summary at post (target ranges from the registers, a per-shader-address memo of "writes no memory but targets"), falling back to A; backstop: at commit, a pre-mark draw's writes against the pages read after its mark (exit under verify). Track B's probe: 0 of ~340k decode-time reads touched a page the drained draws wrote, 0 touched any page ever GPU-written. Value: the removed drains carry ~20% of the prepare wait (~40 ns/draw, ~0.24 ms per guest frame) as an upper bound; with A about the refill part only (~18 ns/draw, ~0.1 ms). Not built: three ordering mechanisms plus a read check for 0.1–0.24 ms per frame | – | – | – | – |
| 2026-11-12 | Overflow prepare worker (Track B4): with free lanes, a third worker on the SMT sibling of the last worker's core (CPU 7 here; the emulator's other threads now stay off both workers' siblings) takes only draws at least 3 past the oldest untaken one, so it never holds a draw the command thread wants next (per-slot claim words; the regular workers skip what it took). Same binary, PGO, frames 1000–1600, 3 runs a side (`KYTY_PREPARE_OVERFLOW=0` as reference): cycles/draw −3.10%±0.83 (!), `wait_prep` 204 → 132 ns/draw (−34%±8, !), `cmd_ns` −2.25%±0.77 (?), FPS +2.5%±1.2 (?); per guest frame (`KYTY_FRAME_DIAG` replay): prepare/recorder waits 1.22 → 0.85 ms, command-thread work 16.67 → 16.28 ms, wall 17.38 → 17.08 ms, guest-late idle 0.35 → 0.46 ms, frames taking 2+ refreshes 3.9% → 2.5%; command thread's effective clock 5.045 → 5.015 GHz; `prep_ns` +7% (the sibling worker is slower). Not kept, same binary, plain build, frames 200–500: the worker siblings freed alone (`KYTY_FREE_PREPARE_SIBLINGS=1`) prepare −4%, waits −0.24 ms/frame, wall +0.16 ms (guest-late +0.08), 2+ refreshes unchanged; the overflow worker on an E-core of its own (18, game threads on 12–17) waits −0.22/−0.38 ms/frame (k=5/3) but guest-late idle +0.5/+0.7 ms/frame and FPS −2.8%/−6.4% (the game's threads use all seven E-cores, ~45% each; a shared E-core preempts the worker); on CPU 7 with only that CPU reserved: weaker (waits −0.32 ms/frame). Workers spinning 500 µs instead of 50 before sleeping (`KYTY_PREPARE_SPIN_US`; all workers are asleep at ~22% of the first posts after a drain): wait unchanged, `cmd_ns` +2.7% (!), FPS −4% (!) | – | – | – | – |
| 2026-11-12 | The command thread's direct Vulkan calls move to the recording thread (Track W; `KYTY_DIRECT_RECORD=1` / `KYTY_SUBMIT_THREAD=0` restore them): the compute dispatch path (binds, barriers, dispatch, the deferred chain barrier), `EndRendering`, `Image::Transit`, draw and atomic-batch write barriers, stream/upload copies and fills, the tiler's image record, transfer clears; and `vkEndCommandBuffer`/`vkQueueSubmit`/`vkBeginCommandBuffer`, with the tick still taken on the command thread. Same binary, both switches as reference, PGO, frames 1000–1600: `cmd_ns` −4.72% ±0.37, cycles/draw −3.99%, FPS +1.31% ±0.39; command-thread CPU 16.3–16.8 → 15.7–15.9 ms per guest frame; recorder drains back to baseline. The dispatch path alone made the remaining direct calls drain the recorder (`wait_rec` 30 → 108 ns/draw); submit alone drained it too | 58.80 / 56.83 | 309.6k / 298.7k | 31.9 / 35.0 (max 50.1) | 35.3 / 29.9 |
| 2026-11-12 | Decode thread, stage 1 (Track B4; `KYTY_DECODE_THREAD=1|verify`, off by default; docs/mt-rewrite/decode-thread.md): the PM4 loop runs on a decode thread across the packets that keep the draw queue and posts queued draws; every other packet is a sync the GPU thread runs after committing what was posted, then it goes on with the packets itself up to the next draw (without that, every non-draw packet was a round trip: 122k syncs/s, the GPU thread idle 33 ms/s; with it 5.2k/s, ~88 per frame, ~1.1 µs each on its critical path). Same binary, PGO, frames 1000–1600, 3 runs a side, decode thread on the emulator's P-cores: `cmd_ns` −2.24%±0.48 (!), cycles/draw −2.49% (!), instructions/draw −8.0% (!), `wait_prep` +40 ns/draw (!); per guest frame (`KYTY_FRAME_DIAG`): command thread busy 16.39 → 16.06 ms, no guest work 0.39 → 0.60 ms, frames taking 2+ refreshes 2.2% → 1.6%; effective clock unchanged. On CPU 9 (worker 8's sibling): `wait_prep` +71 ns, no gain; on an E-core shared with the game: −27% FPS (the decode thread cannot keep up). Verify (a shadow single-thread decode on the GPU thread compares every posted draw's registers and arguments, the state and order at every sync): clean over frames 0–1600 on PGO, plain and with resident textures (8.8M draws, 146k syncs, 0 guest races); with frame skip every draw synced, so the decode thread now steps aside while frame skipping. Of the ~8% of instructions moved, only ~2.5% of cycles came off: the `wait_prep` rise and cross-core reads of decode-written slots are next, before the default flips; placement is decided with Track R's thread map | – | – | – | – |

¹ From a short screenshot run; the two baseline runs predate the `worst_ms`/`low1` columns.

## Benchmark route (2026-11-07)

`garden.sh` used to drive the menus with the virtual pad: boot, title, save slot 1, galaxy map,
Gorilla Nebula, fly to Sky Garden, dive in. That took ~55 s to `StartLevel aerial_garden`
(boot → title ~7 s, title → galaxy map 14 s, galaxy-map flight 31 s) and sometimes needed nudges.

The dump is a developer build (`debuginfo.txt`: Asobi autobuild 2026-05-07). At boot the game
reads its arguments from `/app0/args.txt`:
`-package -sequence product -mode product -odxBinary -perfDisplay false -debugDisplay false -debugInput false`.
`ProductNextSequence` also takes `-lvl <level>`, which sets the next level and enters state 7
(level start), skipping the title and galaxy map. It also takes `-worldmap`, `-title`,
`-skipIntro`, `-showLogo`, `-finalStage` and `-intent <x>`. Registered sequences: `product`
(ProductNextSequence), `drs` (DebugLaunchSequence), `main` (GameMain), `demo` (ASOBI HAPTICS DEMO).

`KYTY_APP0_OVERLAY=<dir>` makes files in `<dir>` replace `/app0` files of the same path.
`garden.sh` writes a temporary `args.txt` with `-lvl aerial_garden` added. The level starts
~14 s after launch, and a whole run takes ~90 s instead of ~133 s. It needs no vpad or save.

Same binary, same batch, two runs each (warm-up run first):

| Route | FPS | draws/frame | worst_ms | low1 |
|---|---|---|---|---|
| `lvl` | 27.89 / 28.24 | 5447 / 5454 | 50.1 / 52.0 | 19.9 / 19.0 |
| `menu` | 27.36 / 27.94 | 5472 / 5429 | 50.8 / 50.9 | 19.7 / 19.5 |

The scene is the same (draws/frame match, same opening flight). `lvl` reads 0.4 FPS higher on
average, which is about the run-to-run spread (0.35 and 0.58 within each route) but above the
±0.3 bar. So the progress table marks the route change, and A/B pairs must use one route.
`GARDEN_ROUTE=menu` keeps the old route.

### Benchmark window (2026-11-07)

Four identical runs of one binary, per-second FPS from `StartLevel`, all follow the same curve:
a rise to ~29, a dip to ~25.5 around 27–35 s, then a plateau at 29.5–30.5. It is the same
scripted flight every time. The time window (25 s settle, 45 s) still spread 1.15 FPS (27.66 /
28.51 / 28.36 / 28.81): part of it is the first run of the batch reading slow, the rest is that a
time window samples a different part of the flight whenever the curve shifts by a second. And
because the game is frame-locked, a faster build reaches later scene content within the same
seconds, so an A/B by time compares different scenes.

`garden.sh` now measures game frames 1000–1600 after `StartLevel` (the plateau; fps = frames over
the seconds between the two `PERF` lines, which now carry `frames=` and `t=`), and `batch.sh`
starts with a short warm-up run. Four identical runs of one build: **29.56 / 29.48 / 29.65 /
29.61**, spread 0.17 (sd 0.07). A run takes ~78 s at 29 FPS instead of ~90 s, and gets shorter as
FPS rises (frame 1600 comes ~27 s after level start at 60 FPS). **Benchmark method changed:**
rows from this point read about 1 FPS higher than the time window on the same build (the plateau
instead of the whole flight); compare them only with each other.

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

### Vertex shading cost, Track G (2026-11-07)

`KYTY_GPU_TIME=draws` (timestamp after every draw, grouped by vertex and pixel module) on `dev`
2d24ba32: 17.3 ms/frame of GPU time, 9.9 in passes, 3.8 in dispatches, 3.6 between. No shader
dominates: the costliest vertex shader is 8% of in-pass time, the costliest pixel shader 11%.
Per-draw times are far above what the ALU work explains (a 2,450-index draw of the top vertex
shader takes 6.4 µs; switching pipelines costs ~5×), so draws are latency-bound: the time of one
warp through a long shader, plus instruction-cache warm-up after a switch.

The shaders are long. `KYTY_PIPELINE_STATS` (local experiment, `VK_KHR_pipeline_executable_properties`):

| Module | Guest code | SPIR-V | NVIDIA binary | Registers |
|---|---|---|---|---|
| top VS (`28eadb3f`) | 12.5 KB | 227 KB | 92 KB | 111 |
| VS `2054c572` | 14.4 KB | 353 KB | 131 KB | 220 |
| VS `0c4216ab` (12k draws/s) | 3.3 KB | 60 KB | 36 KB | 238 |
| MS `dc33907e` (GS on mesh, two guest lanes per invocation) | 15.8 KB | 599 KB | 729 KB | 255 |

**EXEC fold (landed).** A guest NGG vertex shader builds EXEC from the vertex count the
translator passes in s3, the popcount of the entry `Ballot(true)`, and every VGPR write is a
`Select` on the lane's EXEC bit. The host compiler couldn't prove that bit is set, so whole
vertex shaders ran predicated (1,845 `OpSelect` in the top one). The translator already assumes a
vertex subgroup's invocations are its lowest lanes (that makes the popcount a vertex count);
constant propagation now evaluates such lane predicates for every prefix-shaped subgroup and
folds them when they are constant. GPU 16.97 → 16.13 ms/frame, +2.8% FPS (table above).

**Ruled out:**
- Branching per lane in the remaining data-dependent EXEC regions (`if (subgroupAny(exec))` with
  predicated bodies): an unsafe experiment emitting `if (exec)` for vertex shaders changed in-pass
  time by 0.01 ms.

**Measured, not landed: fused legacy MAD.** The game's shaders use `V_MAD_F32`/`V_MAC_F32` (not
fused on RDNA2), emitted as a multiply and an add with `NoContraction` since d8be245c, which keeps
the hardware's intermediate rounding. Emitting them as `FMA` (same binary, warmed shader stores,
2 runs each): GPU 16.19 → 15.75 ms/frame, in passes 8.97 → 8.58; FPS within noise. Not worth
giving up the hardware-exact rounding (and its unit test) for 0.4 ms. (An earlier run showing
−1.7 ms had shader-compile stalls in its window.)

**Outside passes (`KYTY_GPU_TIME=passes`, `GPUGAP`/`GPUXFER`, `dev` a456fbcb, per frame at ~27 FPS):**

| | ms/frame |
|---|---|
| Transfers | 2.8 |
| ↳ buffer copies (~96, 11 µs each) | 1.0 |
| ↳ buffer → image (~9, 96 µs each) | 0.9 |
| ↳ image → buffer (~4, 167 µs each) | 0.7 |
| ↳ fills, blits, image copies | 0.2 |
| Gaps after barriers (~600 per frame) | 0.85 |
| Clears, command buffer start/end | 0.15 |

The image transfers are round-trips of 1920×1080 RGBA16F render targets that compute passes also
use as raw memory (`KYTY_TEX_DIAG`, a local trace): one dispatch reads `0x514080000` through a
texel buffer, so its image is tiled back into buffer memory (2× per frame, 16.6 MB each), and
another binds `0x53ad00000` as a written storage buffer, which marks that image buffer-modified,
so the next sampling re-creates and detiles it (2× per frame). The copies run at 100–170 GB/s,
well below VRAM bandwidth: each round-trip is a copy plus a separate tiling dispatch.

**Masked selects (landed).** Inside a guest branch region every VGPR write is
`Select(exec, value, old)`, so region temporaries keep their old values live and every write is
predicated. `EliminateMaskedSelects` (vertex and local stages) first folds repeated writes under
one condition (`Select(c, a, Select(c, b, d))` → `Select(c, a, d)`), then replaces `Select(c, a,
b)` with `a` when no lane where `c` is false can observe it: every path goes through lane-local
arithmetic and phis to a select on the same `c` that takes it only where `c` holds, or into a
buffer/shared access or export whose EXEC argument is `c`. Past a phi on a CFG cycle the masking
is not trusted (the next iteration may recompute `c`). The top vertex shader goes from 1,848 to
826 selects and from 227 to 174 KB of SPIR-V; the 4,150-select ones only to ~3,320, because what
remains are region outputs (merged by phis, or read by an enclosing region). Same binary, same
batch, `KYTY_MASKED_SELECTS=0` as the reference: in passes 8.91 / 8.97 → 8.75 / 8.81 ms/frame
(time window, two runs each), and 9.14 → 8.93 with the frame-range method in a batch free of
outside load (GPU 15.14 → 14.77); FPS unchanged. An earlier pair against the `dev` build showed
no difference (9.15 / 9.13) and ran while an external build loaded the machine. That is about a third of the
−0.47 ms ceiling below; the rest needs the region outputs unpredicated (a per-lane `if (exec)`
around the region with phis at its end).

**Masked selects in compute and pixel shaders (landed).** The pass's analysis is per guest lane
and treats every cross-lane operation (ballots, shuffles, DPP, derivatives through implicit-LOD
sampling) as an observer, so it holds for compute (two guest lanes per invocation, each IR value
still one lane's) and pixel shaders too. Compute shaders are predicated on an EXEC that is not
folded to a constant, so almost every temporary was a select. Same binary first (`KYTY_MASKED_SELECTS`
for compute and pixel off/on): GPU 14.42 → 13.68 ms/frame, dispatches 3.67 → 3.32, in passes
8.47 → 8.12, top dispatch `0ec2f135` 702 → 592 µs. Against `dev` ae6d8c77: GPU 14.34 → 13.86.

**Vertex-shader code size (`KYTY_PIPELINE_STATS`).** The driver's own statistics per pipeline,
joined with `KYTY_GPU_TIME=draws` by module id (Sky Garden, `dev` 4d170b12):

| Module | GPU ms/s | µs/draw | Registers | NVIDIA binary |
|---|---|---|---|---|
| VS `28eadb3f` | 18.0 | 6.4 | 96 | 80 KB |
| MS `74d9612c` | 14.3 | 9.5 | 255 | 500 KB |
| VS `301b4a23` (also `2054c572`, `d4c03955`: same code) | 13.6 + 11.3 + 9.8 | 10–29 | 219 | 122 KB |
| MS `dc33907e` | 8.8 | 20.4 | 255 | 729 KB |
| VS `90036de5` | 7.1 | 1.1 | 221 | 124 KB |

The 219-register family is skinned meshes. It reads a per-lane array kept in VGPRs with
`V_MOVRELS_B32` (VGPR[base + M0], M0 = 4·bone + row from a loop counter or a vertex attribute).
The translator writes each read as a chain `Select(M0 == k, v[base + k], …)` over every VGPR
above the base: 22 reads × 116 candidates in `301b4a23`. Capping the chains at 32 candidates
(unsafe experiment) gave in passes 9.01 → 8.03 ms/frame and 219 → 168 registers.

**Switch on M0 (landed).** The emitter writes the outermost link of such a chain (eight or more
links on one selector) as an `OpSwitch` on M0 with a phi; M0 is scalar in the guest, so the switch
doesn't diverge. In passes 8.90 → 8.43 ms/frame against `dev` 204abd61 (same batch), the
`301b4a23` family 13.8 → 10.2 ms/s at 213 registers (binary 122 → 149 KB). The rest of the ceiling is register pressure: every
candidate stays live at each read. Holding the indexed range in a function-local array instead
would release them, but every other access to those VGPRs would then go through memory.

**Where the remaining GPU time is (`dev` 8df89588, ~13.8 ms/frame at ~34.5 FPS):** in passes ~8.2,
dispatches ~3.4, transfers ~1.3 (buffer copies ~1.0), gaps after barriers ~0.8.

- **Buffer copies (~1.0 ms/frame) are Track C's uploads.** A local counter on `vkCmdCopyBuffer`
  with `KYTY_VK_COUNT=sites`: ~95 copies and ~19.6 MB per frame (about 4 of 2–16 MB carry most
  bytes, ~55 of 4–32 KB, ~30 under 512 B). 20 MB is ~0.05 ms of bandwidth, so the time is
  per-copy overhead and the barriers around them. By call site, 66% come from `PrepareBda` →
  `SynchronizeBuffersInRange` → `SynchronizeBuffer` (CPU-written guest pages uploaded before
  dispatches), 15% from `CpOpDmaData`, 10% from `ReadMemory` downloads, 5% from new-buffer
  joins. Batching those uploads into fewer copies and barriers belongs to the buffer cache.
- **Masked selects in mesh shaders: not worth it.** Enabled for the mesh stage (the GS-on-mesh
  modules, also two guest lanes per invocation with EXEC-guarded exports): `74d9612c` 17.6 →
  16.4 ms/s, `dc33907e` 10.3 → 9.4, but GPU 13.82 → 13.78 ms/frame, within noise. Left off.
- **Indexed VGPR range, rest of the ceiling (~0.5 ms):** every candidate VGPR stays live at each
  `V_MOVRELS` read (213 registers in the skinned family after the switch). A function-local array
  for the indexed range would free them, but each other access to those VGPRs would become a
  local-memory load or store; worth trying only with the range limited to VGPRs the guest writes
  as an array.

**More ceilings (local experiments, same binary, warmed shader stores, `KYTY_GPU_TIME=passes`):**

| Experiment | GPU effect | Status |
|---|---|---|
| Wave64 compute run one guest lane per invocation (like vertex/pixel shaders), all compute shaders including unsafe ones | dispatches 3.66 → 3.26 ms/frame; top shader 673 → 522 µs | ceiling −0.4 ms; safe subset smaller; not pursued |
| Vertex VGPR writes unpredicated (drops every in-region EXEC select; unsafe) | in passes 8.97 → 8.50 ms/frame | ceiling −0.47 ms for a safe `if (exec)` conversion |
| Fused legacy MAD (above) | −0.4 ms | parked |
| GPU clocks during the level | 2700 MHz, P2, ~43% busy | not a clock problem |

**Single-pass tiling of render targets (landed).** Each 1080p RGBA16F round-trip was a tiling
dispatch plus a separate 16.6 MB copy between the image and a linear scratch buffer (buffer →
image 0.85 ms/frame, image → buffer 0.67 ms/frame, plus zero-fills of the scratch). The nine
tiler families are now also built with the linear side in an image (`TILER_IMAGE` in
`gpu_tiler_common.inc`): detiling stores into a raw-bit storage view of the image, tiling reads
a sampled view of the same format with `texelFetch`. The view format is the unsigned integer
format of the element size (`R8`/`R16`/`R32`/`R32G32`/`R32G32B32A32_UINT`), so neither
`shaderStorageImageReadWithoutFormat` nor per-size image bindings are needed; write without
format was already required. It covers single-level, single-sample 2D tiled color images whose
host texel size equals the element size; mip chains, volumes, block formats and depth keep the
buffer path. Same batch, `KYTY_GPU_TIME=passes`, two runs each:

| | `dev` f2848f29 | image path |
|---|---|---|
| GPU ms/frame | 16.26 / 16.26 | 14.93 / 14.84 |
| transfers | 2.71 | 1.34 |
| ↳ buffer → image | 0.85 | 0.10 |
| ↳ image → buffer | 0.67 | 0.08 |
| in passes / dispatches | 8.98 / 3.68 | 9.00 / 3.66 |

FPS did not move (27.97 / 27.36 vs 27.63 / 25.85 without the diagnostic): the command thread
still limits. `KYTY_TILER_VERIFY=1` checked 7.6k transfers over 68 image shapes in Sky Garden
bit-exact against the buffer path, and the `gpu_tiler` suite checks both directions for every
2D family and element size, array layers and the three color transforms.

**Dispatches (`KYTY_GPU_TIME=draws`, grouped by compute module):** 3.65 ms/frame. One shader
(`0ec2f135`, one dispatch per frame) takes 0.69 ms; then `d637f68e` (40 per frame, 11 µs),
the emulator's own tiling/blit dispatches (~0.3 ms), `2a150e06` (18 per frame, 15 µs), and a
tail of one-per-frame passes of 90–160 µs. The large ones are wave64 guest code run with two
guest lanes per host invocation (NVIDIA subgroups are 32 wide): 763 KB and 1.2 MB of SPIR-V,
with thousands of selects and hundreds of ballots and shuffles.

### Compute dispatches, Track G3 (2026-11-07)

`KYTY_GPU_TIME=draws` on `dev` ae6d8c77 (~34 FPS): GPU 14.26 ms/frame, 8.39 in passes, 3.69 in
248 dispatches, 1.30 transfers, 0.80 barrier gaps. The compute modules that cost most per frame:

| Module | ms/frame | per frame | Registers | NVIDIA binary | Local size (host) |
|---|---|---|---|---|---|
| `0ec2f135` | 0.64 | 1 | 168 | 335 KB | 128 (256 guest lanes) |
| `d637f68e` | 0.42 | 38 | 41 | 23 KB | 32 |
| emulator tiling/blits (`cs=0`) | ~0.35, spiky | ~12 | | | |
| `2a150e06` | 0.26 | 17 | 73 | 70 KB | 32 |
| `abbbd166` | 0.16 | 1 | 255 | 522 KB | 32 |
| `c4fbbdb0` | 0.13 | 1 | 100 | 28 KB | 32 |

Most are wave64 guest code with one 64-lane wave per workgroup, run as 32 host invocations with
two guest lanes each. `0ec2f135` is an image pass under a bounds check: ~2k of its 6.3k selects
are EXEC selects on "this pixel is inside the image", ~650 are the denormal flushes of
comparisons.

**Dispatch chains (landed).** Every dispatch was bracketed by an `ALL_COMMANDS → COMPUTE` hazard
barrier (when it writes) and a `COMPUTE → ALL_COMMANDS` barrier after it, so back-to-back
dispatches never overlapped. On the guest, consecutive dispatches do overlap unless a packet
between them waits (`EVENT_WRITE` CS_PARTIAL_FLUSH, `ACQUIRE_MEM`, `RELEASE_MEM`,
`WAIT_REG_MEM`, ...), so the game synchronizes explicitly wherever it needs ordering. Now a
direct dispatch from the packet loop defers its closing barrier; anything recorded afterwards
(`CommandBuffer::Handle()`, `Record()`, `End()`) records it first. A direct dispatch with only
register writes and markers before it drops it, and within such a chain only the first writing
dispatch records the hazard barrier (it orders everything before the chain against all of its
dispatches). Dispatches using the LDS utility buffer, GDS, BDA (the fault bitmap is updated
without atomics), ordered append, and indirect dispatches keep both barriers. 54% of dispatches
chain (4.1k of 7.7k per second). Same batch, `KYTY_GPU_TIME=passes`, against `dev` ae6d8c77:
GPU 14.14 → 13.29 ms/frame, dispatches 3.68 → 3.04, barrier gaps 0.80 → 0.54. FPS unchanged
(the command thread limits). `KYTY_DISPATCH_CHAIN=0` records every barrier again,
`=stats` prints `DISPCHAIN` lines.

**Where dispatch time stands after #42 and #45** (`dev` abc268e0, `KYTY_GPU_TIME=draws`, ~34.5
FPS): GPU 12.93 ms/frame, dispatches 2.79 (248 per frame), barrier gaps 0.54. Chains cut the small
repeated dispatches most: `d637f68e` 14.1 → 4.5 ms/s, `2a150e06` 8.9 → 3.4 ms/s. Per frame now:

| Module | ms/frame | Notes |
|---|---|---|
| `0ec2f135` | 0.53 | 168 registers, 33.8 KB shared (7.5 KB guest LDS), 1 dispatch |
| emulator tiling/blits (`cs=0`) | 0.3–0.45 | spiky run to run |
| `abbbd166` | 0.15 | 255 registers |
| `d637f68e` | 0.13 | 38 dispatches |
| `c4fbbdb0`, `2a150e06`, `62d38f5b` | ~0.1 each | `2a150e06`: 48 KB guest LDS for one 64-lane wave |

Driver `Shared_Memory_Size` above the guest LDS (`0ec2f135`: +26 KB, `a0cae5af`: 12.6 KB with
no LDS) comes and goes with codegen changes and looks like register spill space.

**Why the other 46% of dispatches don't chain** (local diagnostic, breaks per second at ~35 FPS):
the packet before them is `WAIT_REG_MEM_64` (~1.7k) or `EVENT_WRITE` CS_PARTIAL_FLUSH (~1.2k),
usually after a `RELEASE_MEM` (2.7k); the dispatch itself can't chain (GDS, LDS buffer, ordered
append: ~0.3k); or something was recorded during its preparation (~70). The waits are the
game's own ordering, so dispatch serialization is now at the guest's level. Letting a chain run
through `RELEASE_MEM` (which records nothing here) changed nothing: the wait follows it.

**Ruled out (measured, not landed):**
- Wave64 halves emitted run by run (all of half 0's lane-local instructions between two cross-half
  points, then half 1's, instead of interleaved): registers, shared memory and times unchanged;
  the NVIDIA compiler schedules both halves together anyway.
- Image reads and writes as EXEC-guarded accesses in masked-select elimination (on top of #42):
  GPU 12.90 → 12.89 ms/frame.
- DPP and `V_PERMLANE16` moves shuffling only the caller's half: only 18 of `0ec2f135`'s 230
  shuffles are DPP (the rest are lane reads across halves); no change.
- Comparison denormal flushes against constants: only ~120 of `0ec2f135`'s 474 flushes feed
  comparisons; the rest feed sqrt, division, reciprocal square root and log2, where the
  guest's flush is needed.

### Buffer uploads, Track G4 (2026-11-07)

`KYTY_GPU_TIME=passes` now splits `vkCmdCopyBuffer` time by size (`GPUCOPY` lines: time, copies,
MB and the idle gaps before and after, per second) and, with `KYTY_VK_COUNT=sites`, by recording
call site and its caller (`GPUCOPYSITE`, addresses for `symbolize.py`). On `dev` abc268e0, per
frame at ~33 FPS:

| Copies | ms | per frame | MB |
|---|---|---|---|
| ≥ 4 MB | 0.74 | 2.0 | 17.5 |
| 64 KB – 1 MB | 0.14 | 9.8 | 2.9 |
| 4–64 KB | 0.11 | 64 | 0.9 |
| < 4 KB | 0.02 | 22 | 0.0 |

By site, `SynchronizeBuffersInRange` (`PrepareBda`) → `SynchronizeBuffer` is 29 of the 31 ms/s,
at ~610 MB/s. So the copies were not per-copy overhead: two ~8.7 MB uploads per frame ran at
~22 GB/s, the PCIe bandwidth of a copy from host memory, with the universal queue doing nothing
else (the copy sits between an `ALL_COMMANDS → TRANSFER` and a `TRANSFER → ALL_COMMANDS`
barrier). The bytes are real: #44 found 94% of `PrepareBda`'s pages changed since their last upload.

**Ruled out: staging in device-local, host-visible memory (ReBAR).** Same binary, staging ring
chosen by an env switch: GPU 13.39 → 12.42 ms/frame (the ≥ 4 MB copies 0.75 → 0.05 ms), but
35.13 → 34.10 FPS: the command thread's copy into staging now crosses PCIe as write-combined
stores (~600 MB/s more of them).

**Uploads on the copy engine (landed).** Uploads of at least 256 KiB (`SynchronizeBuffer`, ~5 per
frame, ~620 MB/s) still go into host memory, then a transfer-only queue copies them into a
256 MiB device-local ring right away, while the universal queue runs earlier work. The ring
range is fresh, so nothing queued earlier can read it (ranges are reused only after the command
buffer that read them completes). In the command buffer where the upload would have been, the
universal queue copies ring → destination with the same barriers as before, and that submission
waits for the transfer's timeline value at the transfer stage. The destination's write-after-read
ordering is unchanged; only the PCIe part leaves the universal queue. Same batch against `dev`
abc268e0: `KYTY_GPU_TIME=1` 13.04 → 12.42 ms/frame, `=passes` 13.25 → 12.47 (transfers 1.35 →
0.58, the ≥ 4 MB copies 0.74 → 0.05 ms). FPS 35.01 → 35.28 (unchanged; the CPU side is the same
copy into host memory plus ~5 transfer submits per frame). `KYTY_ASYNC_UPLOAD_VERIFY=1` read
every asynchronous upload's destination back after its copy and compared it with the bytes the
host wrote: 7.4k uploads, 30 GB, 0 mismatches. `KYTY_ASYNC_UPLOAD=0` turns it off (no transfer
queue), `=stats` prints `ASYNCUPLOAD` lines, `KYTY_ASYNC_UPLOAD_MIN=<bytes>` sets the threshold.

**Waits after barriers** (`KYTY_GPU_TIME=passes` with `KYTY_VK_COUNT=sites`: `GPUGAPKIND` splits the
idle gaps after barriers by the kinds of the segments around them, `GPUGAPSITE` by the call site
and stage masks of the last barrier before each gap). On `dev` 79d64348, ~0.53 ms/frame in ~450
gaps; per second at ~34 FPS:

| Last barrier before the gap | ms/s | gaps/s |
|---|---|---|
| `ShaderWriteHazardBarrier` before a dispatch that doesn't chain (`ALL_COMMANDS → COMPUTE`) | 5.4 | 3.3k |
| `SynchronizeBuffer` from `PrepareBda` (a barrier pair per uploaded buffer) | 3.2 | 2.3k |
| `ShaderWriteHazardBarrier` before a draw whose fragment shader writes buffers | 2.6 | 1.1k |
| `Buffer::Fill` / `Buffer::CopyFrom` from `CpOpDmaData` | 2.3 | 1.5k |
| `Image::Transit` (clears, bindings, render targets) | 1.3 | 2.9k |
| indirect-dispatch arguments (`→ DRAW_INDIRECT`) | 1.0 | 0.4k |

By kind: dispatch → dispatch 6.0 ms/s, transfer → transfer 4.4, pass → pass 2.6, dispatch → pass
1.3, dispatch ↔ transfer 2.2. The unchained dispatches wait because the guest put a wait between
them (Track G3).

**Ruled out: attachment accesses on their own stages.** `Image::Transit` gives every non-transfer
access `ALL_GRAPHICS | COMPUTE`, so a render-target transition also holds back the next pass's
vertex and fragment shading. Narrowing color attachments to `COLOR_ATTACHMENT_OUTPUT` and depth to
the fragment-test stages (same binary, env switch): GPU 12.33 → 12.33 ms/frame. The driver drains
at these pass boundaries either way.

**One barrier pair per `PrepareBda` (landed).** Its uploads (often several buffers in a row)
each recorded a buffer barrier before and after the copy. Between `BufferCache::BeginUploadBatch`
and `EndUploadBatch` (around `PrepareBda`'s loop) the first copy records one global
`ALL_COMMANDS → TRANSFER` barrier and the end one `TRANSFER → ALL_COMMANDS`; nothing else is
recorded in between. Same binary, `KYTY_UPLOAD_BATCH=0` as the reference: `KYTY_GPU_TIME=1`
12.30 → 12.19 ms/frame; `=passes` barrier gaps 0.53 → 0.45 ms/frame (447 → 387 gaps),
transfer → transfer gaps 4.4 → 1.5 ms/s; FPS unchanged.

**Skinned vertex shaders' indexed reads (measured, not landed).** The 213-register family
(`301b4a23`, `2054c572`, `d4c03955`: one guest program, hash `663ef8a1a281b3fc`) has 22
`V_MOVRELS_B32` reads with base `v0`–`v3`. The guest keeps three 4-dword entries in `v0`–`v11` and
walks them in loops: `M0 = 4 · s98` (and `+ 4`), where `s98` counts iterations and each lane stays
in the loop while `s98 < v69`, a per-vertex count. In Sky Garden `M0` stays below 16, but nothing
in the program bounds it from above: a vertex with a larger `v69` reads `v12`, `v13`, … on the
hardware. The translator conservatively offers every VGPR from the base up to the limit (116
candidates). Capping them at 16 (an unsafe experiment, separate shader stores, same binary):

| | 116 candidates | 16 candidates (unsafe) |
|---|---|---|
| GPU (`KYTY_GPU_TIME=1`) | 12.24 ms/frame | 11.67 |
| in passes (`=passes`) | 8.46 | 7.63 |
| registers (`KYTY_PIPELINE_STATS`) | 213 | 168 |

The cap has no correctness proof: it changes results for vertices whose count exceeds three.
Exact options, none measured:
- Every `M0` here is a multiple of 4 (shifted counters, and first-lane reads of shifted values in
  the later loops), so each read could drop the candidates outside `base + 4k` (29 instead of 116).
  The reads use all four bases `v0`–`v3` close together, so between them every register stays a
  candidate; this may not shorten any live range.
- A local array: every definition of a high register that reaches a read also stores to it, and
  the reads load from it. Its register count and local-memory cost are unknown.
Parked like the fused MAD; whether a bound on the per-vertex count may be assumed is a project
decision. The cap is one line in `V_MOVRELS_B32` (`KYTY_MOVRELS_CAP`, local only).

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

### Threaded draw preparation, Track B4 (2026-11-07)

**Design (the "snapshot" variant of B4).** A direct draw whose registers describe an ordinary
draw is queued instead of drawn: the command thread copies the register state (`HW::Context`,
`UserConfig`, `Shader`) into a ring slot and posts it to one worker thread, which runs
`PipelineCache::GetGraphicsPrograms` (program lookup, SRT walk, resource materialization,
module selection) from the copy into the slot's `DrawRenderState` and `StageResources`. Up to
four draws are queued; the command thread commits the oldest one (render targets, bindings,
buffers, recording) with `CommandBuffer`'s register pointers bound to the slot's copy. It adopts
the worker's lookup only if:
- the worker did not fail. It only finds: a program or module miss, a call-target program, a new
  tessellation layout or an image heap fails the preparation;
- nothing it read could have changed since the post: `DrawPrepare::HazardGeneration` (advanced
  when the buffer cache's GPU-written set grows) and `GuestSyncEpoch` (emulator stores into
  guest memory) are unchanged;
- no guest range it read through the backing view (watched pages, buffer-backing reads) is
  GPU-written now. The worker never reads in place, so it never faults into the caches.

Otherwise the command thread waits for the worker to go idle and looks the programs up itself.
Packets other than register writes (direct and indirect), `SET_BASE`/`INDEX_*`/`NUM_INSTANCES`,
`INDIRECT_BUFFER`, marker NOPs and direct draws commit the queue first, as do pending
guest-thread commands, a requested asynchronous-write collection, `NUM_INSTANCES` read from
memory and the end of each `CommandProcessor::Process`.

`KYTY_PREPARE_VERIFY=1` looks every adopted draw up again on the command thread and exits on
any difference (module, resources, specialization, static inputs, vertex inputs). Clean over
full benches at queue depths 1 and 4 (3–4M draws each). Rejected lookups: ~0.5% (hazards), 0
failed, 0 GPU-written reads.

**Where the worker runs decides the gain.** Same binary, one run each (old time-window bench,
`dev` 3bafedba + #26):

| Worker CPU | FPS | Worker µs/draw |
|---|---|---|
| none (`KYTY_PREPARE_THREAD=0`) | 24.65 | – |
| efficiency core 18 (next to the recorder) | 26.62 | 5.0 |
| efficiency core 15 (other cluster) | 26.52 | 5.0 |
| performance CPU 8, shared with guest threads | 29.48 | 3.2 |
| performance CPU 8, taken from the guests (sibling 9 kept) | **31.27** | 3.2 |
| performance CPU 8 and 9 both taken | 31.23 | 3.0 |
| CPU 8 taken, recorder moved to its sibling 9 | 30.46 | 3.5 |

The worker now takes the first thread of the second-to-last performance core, which the
emulator's affinity mask leaves out (`KYTY_PREPARE_CPU` overrides). Queue depth: 1 draw 31.31,
2 → 31.90, 4 → 32.12, 8 → 32.14 FPS; rejections grow with depth (0.1% → 0.7%), so 4 it is.

**The indirect register writes must not drain the queue.** Sky Garden loads register blocks
from memory (`SET_CONTEXT_REG_INDIRECT`, `SET_UCONFIG_REG_INDIRECT`) for nearly every draw.
Draining the queue at them (a review suggestion: they read guest memory a queued draw might
write) made 33k drains/s, about one every 5 draws, and the command thread waited 0.8 µs/draw
for the worker; 94% of the drains came from those two packets. They now keep the queue: the
hardware command processor reads them long before earlier draws' shaders run, so a guest
that reads shader output there must put a wait or a cache operation in between, and those
packets drain the queue. Drains fell to 2.4k/s and waits to 0.2 µs/draw.

**Result** (frame-range bench; re-run after the external build on efficiency cores 12–15 was
paused during benchmarks, two runs each, `dev` 4d170b12 against 204abd61): 29.56 / 29.21 →
**34.99 / 34.84 FPS (+18.8%)**, 153.3k → 182.9k draws/s (6.53 → 5.47 µs per draw on the
command thread), worst frame 48.8 → 36.5 ms, 1% low 20.1 → 28.5. (The PR's own batch ran with
that load: 29.61 → 34.42.) The worker spends 2.9 µs per draw
(53% busy); the command thread waits for it 0.16 µs per draw, mostly when a queue drains.
Before #25 (which removed the 16 KB of per-draw state zeroing the worker had taken over) the
same design gave +27% over its reference.

What is left on the command thread (perf, flat, user mode; LBR call graphs need
`perf_event_paranoid` ≤ 1, which was 2 this time): texture resolve and its validation
(`RefindExactImage` 4.9%, `FindTextureDescription` 2.6%, `ResolveStageTexture` 2.0%, texture
cache hash lookups ~2.5%, `FindView` 1.2%), libc `memcpy` and friends ~12% (stream copies and
per-draw structs), `ObtainBuffer`/`FindBuffer`/`SynchronizeBuffer`/`FindBuffers`/`RebindBuffers`
~6.6%, `pthread_rwlock_*` ~4% (the guest address-space lock in `CopySparseMemory`),
`CommitBindings` 2.1%, the NVIDIA driver 2.2% (pushes and binds not deferred to the recorder).
**Correction: the walker is bound by executed instructions, not by instruction fetch** (B1′,
measured on 84e51e33). A denser encoding cut its L1i misses by 24% (115M → 88M per 8 s). It used
short jumps to a `ret` placed before each thunk, and reach and state bytes through rbx with
one-byte offsets. Its cycles did not move (12.8% → 13.4% of the thread) and FPS was noise
(29.29–29.58 vs 29.16–29.56). The walker runs at IPC ≈ 1.9 and retires about 4,500 instructions
per draw. Most values are shared and referenced again once known: every read through a V#
references its words. So each reference now tests the value's state byte and calls its thunk
only when the value is not known yet. That cut walker cycles from 12.8% to 10.2% of the thread,
for +2.5% / +3.2% FPS. The L1i-miss share was large but cheap: the misses were served from L2,
overlapped with other work. Checking the page tracker inline at each load instead of in a stub
cut walker instructions by 12% but not its cycles (10.1% → 10.4%), because loads wait on memory;
not merged. Since B4 the walker runs on the prepare worker, which is half idle, so walker
speedups now buy worker headroom (for example on a smaller core), not FPS.

**Where the command thread's time goes after B4** (`dev` 204abd61, ~34 FPS, 5.6 µs per draw;
TSC marks around each phase of a queued draw's commit, a local diagnostic, ns per draw):

| Phase | ns |
|---|---|
| texture bindings (`PrepareBindings`: stage texture resolve, memo validation, samplers, views) | 900 |
| buffer bindings (`PrepareGraphicsBindings`: `FindBuffers`, BDA, rebinds) | 625 |
| render targets (color and depth resolve, memo hits) | 445 |
| vertex and index buffers (`ObtainBuffer`) | 420 |
| record (vertex/index commit, pushes, dynamic state, atomic batches, pass begin, draw) | 300 |
| `CommitBindings` | 295 |
| waiting for the worker | ~200 |
| `AcquireRenderTargets` | 195 |
| `PopPendingOperations` (clock read, timeline refresh every 20 µs, lock, retired operations) | 185 |
| adopting the worker's lookup | 95 |
| pipeline lookup | 65 |
| everything else in the commit (checks, index setup, register rebinding, tail) | ~200 |
| register copy at queue time | 35 |
| **commit total** | **~3,950** |

The other ~1.7 µs per draw is outside the draw commit: PM4 parsing and register writes
(Sky Garden loads register blocks with `SET_*_REG_INDIRECT` for nearly every draw),
dispatches (~234 per frame at ~11 µs each, mostly `PrepareBda` and `SynchronizeBuffer`, about
0.5 µs per draw), labels and guest-thread commands.

**The buffer side of the commit** (`dev` 34fd87d1, ~34 FPS; same TSC marks, ns per draw, and
`KYTY_MEM_DIAG=1` per second):
- Buffer acquisition takes ~730 ns per draw: storage buffers and per-draw uploads
  (`RebindBuffers`: `ObtainBuffer` per buffer, the flattened SRT and shader data) 297, the
  index buffer 250, vertex buffers 182; `FindBuffers` adds 127. That is ~11.5 `ObtainBuffer`
  calls per draw (1.95M/s): 1.27M/s are clean ranges (a tracker check and a
  `SynchronizeBuffer` that uploads nothing), 348k/s are larger than the stream path, 244k/s
  reuse a stream copy, 62k/s make one (215 MB/s).
- `PrepareBda` (dispatches and DMA draws) takes 43 ms per second of the command thread
  (4.3%), in ~6.2k calls; single calls reach 1.2–2 ms. It uploads ~600 MB/s, ~150k pages
  over 28.6k distinct pages, so each hot page about 5 times a second. 83% of its time is the
  copy from guest pages into the staging buffer, already at ~18 GB/s.
- Ruled out for `PrepareBda`: skipping pages whose bytes did not change since their last
  upload (only 5.6% did not). Not tried: splitting the copy across threads. The copy is
  ~33 ms/s (3.3% of the thread) and already runs at ~18 GB/s, so a split could save at most
  that and probably much less if it is bandwidth-bound; unmeasured. Only not copying would
  remove it: reading those pages in place (C1 host import), which still waits on labels being
  written at host-GPU completion.

Ruled out: skipping `PopPendingOperations`' lock when no operation is ready (an atomic copy of
the queue's front tick). Its 73 ns per call measured under TSC marks did not show up in FPS
(same batch, without the external load: `dev` 204abd61 34.99 / 34.84, with the change 34.26).

### Core layout after C2 and B4 (2026-11-07)

The rule "guest threads on performance cores" dates from when they spent ~40% of their time in
write-fault signals. Since C2 they are light, and the efficiency cores 12–18 sat idle. Same
binary, layouts set through `KYTY_GUEST_THREAD_CPUS`, `KYTY_GUEST_PCORE_NAMES`,
`KYTY_RESERVE_CPUS` and `KYTY_RECORDER_CPU`; FPS per run:

| Layout | FPS | worst_ms | low1 |
|---|---|---|---|
| (a) all game threads on the performance cores (before) | 35.20 / 35.07 / 35.46 | 39–40 | 24.8–27.4 |
| (b0) game-created threads on efficiency cores 12–18 | 35.77 / 35.68 / 35.45 | 33.5–34.5 | 28.2–29.9 |
| (d0) as b0, but `DrawThread` (the game's render thread) stays on the performance cores | 36.38 / 36.35 | 33.5 (max 33.9) | 29.9 |
| (b) as d0 plus the recorder on performance CPU 6, taken from the emulator | 36.20 / 36.62 / 36.45 | 33.5–34.5 | 27.8–29.8 |
| (c) as b0 plus the recorder on CPU 9 (the prepare worker's SMT sibling) | 35.26 | 33.5 | 28.7 |

d0 is now the default: threads the game creates run on the efficiency cores except the
recorder's, and those named in `KYTY_GUEST_PCORE_NAMES` (default `DrawThread`) stay with the
emulator. The game's main thread is the process's main thread and is unaffected. Same batch
against `dev` e5ef1e84: 35.03 / 35.27 → **36.02 / 36.45 FPS (+3.1%)**, worst frame 38.8 →
33.5 ms (no frame over two vblanks), 1% low 25.3 → 29.3. Moving the recorder to a
performance core costs nothing (b), so a performance core is free for it if it gets more work.

### Prepare waits after asynchronous readbacks (2026-11-07)

After #52 the command thread is the gate (~98% busy). Where it waits for the prepare worker, per
second on `dev` b4a91152 (temporary instrumentation, one run):

| Wait | Waits/s | ms/s | Cause |
|---|---|---|---|
| Ring full | 1,875 | 10 | The worker is 4 draws behind, on a slow draw (~5.4 µs per wait) |
| Draw not adopted | 1,400 | 6, plus ~5 re-looking up | ~2,000 hazards/s: 77% guest-sync epoch, 21% GPU-written growth |
| Drain at a packet | 1,650 | 4.8 | Mostly the last posted draw (~2.7 µs): RELEASE_MEM, EVENT_WRITE, NOP sub-ops, ACQUIRE_MEM |

The worker was never asleep when a wait started, so the spin-then-sleep wake-up is not a
cause. Nearly all epoch hazards (~1,500 draws/s) were buffer readbacks publishing into guest
memory through the backing. Those pages stay GPU-owned, so read-watched, until after
publication, which means a prepared draw that read the bytes logged the read. Readbacks now
record their ranges with DrawPrepare instead of advancing the epoch that queued draws check,
and a draw is voided only if a published range overlaps its logged reads. Hazards fall from
1,850 to 820/s, about 1,000 fewer fallbacks per second (~0.6% of the command thread). Same batch
against `dev` b4a91152: 39.36 → 39.50 FPS (noise). `KYTY_PREPARE_VERIFY=1` is clean, and no
published range overlapped a logged read (`stored=0` in `KYTY_PREPARE_STATS`).

Ruled out:
- A deeper prepare queue (`KYTY_PREPARE_DEPTH=12`): ring waits fall from 9.4 to 5.6 ms/s, but
  every hazard now voids more queued draws, so fallbacks rise from 4.9 to 13.4 ms/s
  (38.90 → 38.62 FPS).
- Remembering clean read-only `ObtainBuffer` lookups per (address, size), invalidated by the
  tracker region's CPU-dirty growth and buffer (un)registration. Verify-clean, 49% hits
  (1.1M/s), but no saving. With the memo on, `SynchronizeBuffer` and tracker queries lose ~1.2%
  of the command thread, and the memo's own hit path gains ~1.3% (table and region cache
  misses). Buffer-cache code totals 8.46% vs 8.52% of the thread, and the FPS change is noise
  (39.28 → 38.97).
- Building and deduplicating graphics descriptor writes on the recording thread. One run, `dev`
  4c86dc01, ns per graphics draw in `CommitBindings`: image transitions 104 (they must stay),
  descriptor dedupe and record 97, write building 69, push-constant dedupe and record 35,
  setup 11. Moving the dedupe of push constants and push descriptors to the recorder, with an
  epoch for invalidation and `KYTY_BIND_VERIFY` comparing both sides' decisions (7M pushes
  matched), measured 38.90 → 38.97 FPS. In the command thread's profile, `PushDescriptors` +
  `PushConstants` fall from 1.11% to 0.87%, but `Reserve`, `Publish` and `WaitDone` rise from 2.17%
  to 2.23%. The compare was cheap; the cost is copying into the ring. The command thread copies
  only changed runs today, and the recorder needs every write. Building the writes on the
  recorder (69 ns) still needs the same descriptor infos copied, so it is not pursued. Unmerged
  branch `trackB/recorder-descriptors`.

### Kernel time on the command thread (Track K, 2026-11-11)

The budget tables above come from user-mode samples, so they miss the command thread's kernel
time: ~12% of `KytyGpuCmd` (116 ms/s of `stime` in `/proc/self/task/<tid>/stat`, ~440 ns/draw).
`KYTY_KERNEL_STATS=1` times every system call the emulator makes on purpose on that thread with
rdtsc and charges it to the innermost call-site scope; `tools/astro-bench/kstats.py <label>`
averages the `KSTATS` lines over `garden.sh`'s frame window. On `dev` 084ca762 (261.6k draws/s),
the timed calls cover 97 of the 116 ms/s; the rest is page faults (~6.5k minor faults/s), futexes
inside locks, and signal delivery. Kernel time by site:

| Site | What | calls/s | ns/call | ns/draw |
|---|---|---|---|---|
| Async write collection (`CollectAsyncWrites`) | `PAGEMAP_SCAN` over ~68 hot 2 MiB chunks, ~21 calls and ~34k pages per collection, ~1,160 collections/s | 22.4k empty + 2.4k with written pages | 1,584 / 6,455 | 136 + 59 |
| CP label stores (`StoreCpLabel` → `ProtectAsyncPage`) | one-page `PAGEMAP_SCAN` with re-protect | 61.3k empty + 2.6k with written pages | 382 / 2,392 | 90 + 24 |
| Page-tracker watch changes | `mprotect` 1.2k/s, `PAGEMAP_SCAN` 4.4k/s, `UFFDIO_WRITEPROTECT` 0.6k/s | 6.2k | 1,435 avg | 34 |
| WAIT_REG_MEM label peeks (`AsyncPageUntouched`) | one-page `PAGEMAP_SCAN`, no re-protect | 12.4k | 358 | 17 |
| Worker/recorder wakes, spin-wait yields | futex wakes 3.5k/s, `sched_yield` 4.8k/s | 8.3k | ~280 avg | 9 |
| `FillBuffer` through the guest mapping | 92 SIGBUS/s (handler 3.9 µs, delivery not visible) plus write faults; whole fill region 8.1 µs × 838/s | 838 | 8,086 (region) | 26 (region) |
| Compute/graphics SRT walk on the command thread | 797 SIGSEGV/s, handler 125 ns | 797 | 125 | 0.4 |

- **The collection walk is memory-bound in the kernel:** ~1.06 ns per PTE for an empty scan
  (it is already on `PAGEMAP_SCAN`'s fast path: category and return mask exactly
  `PAGE_IS_WRITTEN`). A scan that finds and re-protects written pages costs ~3.7 µs more (TLB
  shootdown). About 900 of the ~1,160 collections/s are the one at the start of
  `GuestGpu::Process`: ~450 for fresh submissions and ~450 for blocked submissions that resume;
  ~240 come from waits. Only 53% of the scanned pages have a write watcher
  (18.3k of 34.6k per collection, in ~87 runs).
- **Label stores:** 89% hit a page a previous label store protected, with no guest store since,
  but that is only known by scanning; skipping the scan would leave the guest's earlier stores
  in the page, so the next peek on it would collect (~46 µs) instead.
- The research figure "FillBuffer 11% of kernel entries" is entries, not time: the fill path
  costs ~26 ns/draw in all, and the SRT-walk SIGSEGVs ~0.4 ns/draw of handler time.

**Submit-time collection (P1, `KYTY_SUBMIT_SCAN`).** The collection at a fresh submission's start
moves to the submitting guest thread. It scans before the submission is enqueued (so before the
command thread can see it), and the pages it finds are re-protected by the same `PAGEMAP_SCAN`
call and kept, with their label tokens advanced. Every GPU-thread collection takes the kept list
after any submit scan in flight; submit scans run one at a time, so a write can be reported
twice but never lost. A store made after the submit scan is collected where a command buffer can
first depend on it: a WAIT_REG_MEM satisfied by a guest-written value, a semaphore wait, a valid
rewind (all `RequestAsyncCollect`), or a resumed submission (which still scans at `Process`).
Flip preparation scans at its submit. The wait skip for the processor's own labels cannot hide
such a store, because kept pages' tokens advance. In verify mode ~8.5k write-watched pages/s
are written between the submit scan and processing (large dynamic ranges); those are
invalidated at the next collection instead of at processing start.
- Measured (same binary, two pairs): collection 206 → 155 and 216 → 145 ns/draw (~60 ns/draw,
  not the ~160 first estimated, because only ~450 collections/s are fresh submissions); command
  thread `stime` 112 → 103 and 115 → 100 ms/s; FPS 49.62 → 49.92 and 48.67 → 49.48. The scans
  cost the submitting threads `DrawThread` 15–17 ms/s (~400 scans/s, ~40 µs each) and
  `FluidCalculatio` 7–9 ms/s.
- `KYTY_ASYNC_WP_VERIFY` counts kept writes as pending report, as it counts written marks still
  in the page tables (otherwise a submit scan taking a mark between a collection and the
  verifier's peek reads as a loss: ~368k false reports), and checks that every kept write a
  collection takes is among the writes it handled. With P1 on it reports 0 lost writes over the
  bench window. `KYTY_ASYNC_WP_VERIFY_DROP_KEPT=<n>` (test only) drops every n-th kept write: with
  n=50 the verifier reports 16 lost writes on watched pages, after which the stale data makes
  the host GPU fail (device lost at a semaphore wait).

### Where the command thread's library time goes (2026-11-07)

Call stacks of `KytyGpuCmd` on `dev` 4c86dc01 (`perf record -e cpu_core/cycles/u --call-graph lbr
--user-callchains -t <tid>`; without `--user-callchains`, perf refuses any call graph at
`perf_event_paranoid=2`). Shares of the thread's cycles:

| Leaf | Share | Callers |
|---|---|---|
| memcpy (libc+0x1b1c87) | 6.4% | `PrepareBda` uploads through `AsyncUploader::Upload`'s fill 4.3%, the staging ring 0.3%; `ObtainBuffer` stream copies 1.2%; `PostQueuedDraw`'s register snapshot 0.6% |
| `pthread_mutex_lock`/`unlock` | 4.0% | `CommandScheduler::PopPendingOperations` per draw 2.3%, `ClampRangeSize` 0.6%, `MakeDepthTargetDesc` 0.2% |
| `pthread_rwlock_unlock`/`rdlock` | 4.5% | `CopySparseMemory` from `ObtainBuffer` 2.9%, `TryReadBacking` from `MaterializeColorClear` 0.9% |

The guest-memory reader locks are gone: reads inside one backing mapping copy straight from the
backing alias. A per-thread translation cache is validated by a sequence count that every
`GuestBackingStore` writer makes odd while it holds its lock (`KYTY_FAST_GUEST_READ=0|verify`).
Result: 38.97 → 39.56 FPS. The scheduler mutex went to Track B with its per-draw locks. The
`PrepareBda` memcpy (~680 MB/s, two ~8.7 MB ranges a frame) is the largest single item left.

Two attempts on that memcpy (~200 uploads/s of ~3.4 MB, ~41 ms/s of the command thread):

- Copying on a helper thread after the upload is queued is unsound. Labels are stored when the
  command thread decodes their packet, so the guest may rewrite a buffer before a later copy
  reads it. With verification, 9% of uploads changed between queueing and the helper's copy.
  Waiting for the helper at the next packet that can signal the guest
  (`GuestSyncEpoch::Advance`) puts all the time back: ~40 waits/s totalling ~40 ms/s, because
  such a packet follows almost every large upload.
- Splitting the copy over the caller and two helpers, and returning once all of it is copied,
  keeps the snapshot. The caller's share falls to ~25 ms/s, but the copy is memory-bound
  (~32 GB/s with three threads, ~17 GB/s alone). FPS: same binary, helpers off 38.99 → on 39.21;
  against `dev` ab1429f1, 39.93 → 39.87. Not merged (branch `trackB/upload-helper`).

**What `PrepareBda` uploads, and who reads it (Track B, `KYTY_BDA_STATS`, 2026-11-08).**
On `dev` 23191d32, about 580 MB/s while measuring (145k 4 KiB pages/s, ~4.7k a frame; the
diagnostic halves FPS, so per frame is the comparable figure):

- Source: all but ~0.1 MB/s was queued by `InvalidateMemory` (write faults, collected
  asynchronous writes, CPU label stores). New buffers add ~0.1 MB/s; CPU-write readbacks and
  `MapMemory` are about 0.
- Pages uploaded again within the same frame: 0.6%.
- Over-copy from 4 KiB granularity: a 1-in-8 sample of pages, compared as staged with their
  previous staging, has 2.83 KB of 4 KB changed over a 3.7 KB span, and 57 of 64 cache lines
  changed (87% of pages have 49–64 lines changed, 6% none). Copying only changed lines would
  save ~11% at best and need a second read of each page to find them.
- Read by DMA shaders: with `KYTY_BDA_STATS=access`, every `get_bda_pointer` call marks its
  16 KiB page in a bitmap. The 22 `uses_dma` shaders read **one 16 KiB page per second**, so
  almost none of the upload is read through the BDA page table. `uses_dma` is set for any
  address or GPU-selected buffer load, and in Sky Garden those paths rarely run.
- Bind coverage: of the uploaded bytes, ~258 MB/s (44%) belong to 4 KiB pages that an
  `ObtainBuffer` or `ObtainBufferForImage` request overlapped before the page was uploaded
  again; **~325 MB/s (56%) were replaced by the next upload of the page with no overlapping bind
  in between**. Both are coverage observations: a bind counts the whole uploaded page for any
  overlap, whether it then returned cached storage or copied newer bytes itself, so the 44% is
  not the copy volume a bind-time sync would make.

So sub-page copies and in-frame de-duplication are ruled out, but the PrepareBda sync itself is
mostly early: the guest rewrites more than half of what it uploads before anything binds it,
and DMA shaders, the reason for the sync, read almost none of it. The sync cannot simply be
dropped, because a DMA read of a page that is not current would see stale bytes, and which pages
a DMA shader reads is only known on the GPU. Pointing the BDA page table at the guest's own
memory for CPU-written pages (C1) would make the upload unnecessary for DMA reads. How much of
the copy then moves to the binds' own syncs and stream copies is not measured here; the
coverage figures above bound neither side, so C1's saving has to be measured on its prototype.

### C1 (in-place guest reads): parked (Track B, 2026-11-11)

C1 had two stages:
- **Stage 1, late labels:** labels at host GPU completion.
- **Stage 2, in-place reads:** the BDA page table points at the guest's own memory.

Stage 2 needs stage 1, because the guest must not see a label, and reuse the memory behind it,
before the GPU has read that memory. **A correct stage 1 costs 15%, and stage 2 recovers almost
none of it, so C1 is parked.** The code stays on unmerged branches: `trackB/late-labels` (draft
PR #70) and `trackB/bda-inplace`.

**Late labels.** `KYTY_LATE_LABELS=1` stores RELEASE_MEM labels, and raises their interrupts,
when the command buffer they were recorded in completes.

| Version | FPS | Note |
|---|---|---|
| Naive | 43.6 → 9.7 | every own-label wait becomes a GPU round trip |
| Pass waits on pending values (on `dev` c7bdab8f) | off 47.45 / 47.25, on 47.42 / 47.42 | had an ordering bug (below) |
| Correct (on `dev` e0999e89, same binary) | off 49.2, on 41.6 (−15%) | |
| Correct, bind waits skipped (unsafe test) | 45.5 | |

How waits pass:
- A WAIT_REG_MEM that this processor's own pending label satisfies passes at once. All guest
  queues record into one host queue, so the work after the wait runs after the work before the
  label.
- This happens only while no other writer was seen storing a value that would release the wait.
- Reference-clock labels keep the clock read at decode. Read at completion instead, the game
  sees its real GPU time and runs 4% slower.

**The ordering bug.** A `=verify` counter (`read_unlanded`) showed that **later packets read
pending bytes as data at decode**, 12–130 times a second:
- 256-byte WRITE_DATA or DMA_DATA blocks, written after an early-passed wait, read by 512-byte
  binds;
- 4- and 8-byte labels and sync words read by binds, the same addresses again and again (for
  example a 4-byte bind of `0x5707e6020`).

The version that measured free deferred those writes and uploaded stale bytes.

**The correct version** (`trackB/late-labels` @ 8ea2e78d):
- After an early wait, only CP writes of 8 bytes or less (WRITE_DATA, semaphore signals, CPU
  fills and copies) are deferred, in order. Larger writes are data and run at decode.
- A bind that would copy unlanded bytes waits for them: a GPU round trip, ~55–60 a second.
- A wait is predicted only from the bytes the pending store writes, and not while an
  overlapping deferred write is pending.

**Where the 15% goes:**
- About 8% is the bind waits.
- About 7% is not found. The flat profile has +3.5% in a libc memcpy. Per frame,
  `KYTY_MEM_DIAG` shows stream and BDA bytes equal with late labels on and off,
  wait-triggered async collects ~3× higher, and fewer stream reuses.

**In-place reads (stage 2 prototype, with late labels on).** `KYTY_BDA_INPLACE=1|verify`:
- **Mechanism:** the guest backing memfd is imported in 2 MiB chunks (~1.1 GB pinned, 545
  chunks). Page-table entries of CPU-dirty, whole, not GPU-written 16 KiB pages point at it, and
  are restored before GPU writes, on (un)registration and remap, and for dispatches that store
  through addresses.
- **Copies:** PrepareBda uploads went from 830 MB/s to 0. Stream copies rose from 300 to
  433 MB/s, and bind syncs from 0.7 to 58 MB/s. Total copied bytes fell 57%.
- **Result:** GPU time fell ~0.7 ms/frame, but FPS was flat. The upload's share is ~270 ns/draw,
  and pages the binds then copy themselves give part of it back. Verify (page-granular, not
  narrowed to pages a DMA shader read) found 2.3% of redirected pages changed between record and
  completion.

**What reviving C1 would need:**
- Late labels near free and still ordered. Binds would get a pending label's or a deferred
  write's known value written into the bound GPU buffer, instead of waiting for it to land; the
  buffer cache would have to keep that consistent until the guest store lands.
- The unexplained ~7% found.
- A stage 2 that also avoids the bind-time copies it causes; the planned hybrid kept eager
  uploads for pages that binds read.

**For other work that orders label stores:** queued draws do read label bytes as data, so any
change that moves a label store between queued draws must check the draws' read ranges against
it.

### Resident textures, stage 1a shadow mode (Track R, 2026-11-12)

`KYTY_RESIDENT_TEXTURES=shadow` (design: `RESEARCH-thin-commit.md`). Every texture a worker record resolves gets an element of the image heap's set-1 array, and every sampler a stable index. Nothing reads them; the classic path still binds. Per draw, the commit predicts which images a resident binding could skip, then checks that the classic path did nothing for them: no refresh, no barrier, no state change, the element's view and read layout. The first difference exits. The prediction uses the binding epoch, the GC tick and `Image::unsettle_generation` / `read_layout_epoch`, or per image `Image::residency_version`.

Results, Sky Garden frames 1000–1600 (plain build):
- Two full runs, 0 mismatches. A predictor with the counters disabled exits within seconds, so the check is live.

| | global counters | per-image versions |
|---|---|---|
| sampled images skippable | 94.2% | 96.4% |
| stage sets with images, skippable | 84.3% | 87.0% |
| draws with every set skippable | 82.7% | 85.3% |
| command-thread time in code a resident binding deletes | 191 ns/draw | 217 ns/draw |

- Ceiling for that code (all sets with worker records): 296 ns/draw.
- Images that stay per draw:
  - colour-clear step (DCC/CMASK): 3.2%;
  - resolved without a worker record: 0.9%;
  - storage: 0.06%.
  - There were no target/feedback conflicts and no dynamic-mip slots.
- Elements: ~1.1k distinct views alive (run peak 1.5k of 65,536). About 1 allocation and 1 retirement per frame with an 8-GC-tick ageing (a GC tick is ~1 per frame). 127 distinct samplers.
- Off mode: neutral. `ab.sh` against the merge-base: ipd +0.33% ±0.20, cpd −0.34% ±0.80, FPS +0.43% ±1.39, all noise. The first version cost ~1.6% FPS with its hooks inside the hot loops. They are now compiled into a second copy of each loop.

### Resident textures, stage 1b (Track R, 2026-11-12)

`KYTY_RESIDENT_TEXTURES=1` (off by default; `=verify` also runs the classic acquisition and checks each skip; `=shadow` is 1a). Draw stages (never compute) sample their plain sampled images and, when no sampler is indirect, all their samplers through the image heap's set 1 (binding 0 images, binding 1: 4,096 samplers) by 16-bit index words at the end of the stage's shader data. Not resident: storage, heap, atomic, indirectly selected and dynamic-mip images. Per slot, RebindImages skips (the record's remembered element, no acquisition, transition or descriptor write) when the binding epoch, GC tick and `Image::unsettle_generation` / `read_layout_epoch` (or the image's `residency_version`) are those of the last draw that left the image settled in its read layout. A reused set whose counters are unchanged is skipped whole. A slot whose image is also a target or storage image of the draw (`Image::resident_live`, checked in `BindRenderTarget`/`BindImage`) gets a one-draw element in the layout the classic commit picks. Elements are written on the command thread after the draw's commit (update-after-bind, before the draw is published), shared per (image serial, view), retired 8 GC ticks after last use or when the image is unregistered, and reused only once their retirement tick is free.

- **The mode keys the shader store** (`Spirv::Profile::resident_textures` in the store header): switching it recompiles every module once. A/Bs use two install dirs with the same binary.
- **Capabilities:** without `shaderSampledImageArrayDynamicIndexing` or enough update-after-bind images, samplers or per-stage resources, startup logs "Resident textures unavailable" and draws keep classic bindings.
- **Validation caught what the gate missed.** Warm-up from the shader store created pipeline layouts before `ImageHeap` existed, so every warmed pipeline had a null set 1 and resident mode drew black. `gate.sh` passed, because its screenshots are compared by eye. The image heap's set layout is now a `RenderContext` member made before the pipeline cache, and creating a heap pipeline layout without it exits. After a change to pipeline layouts or to the warm-up order, run once under `--vulkan-validation`.
- **Verify found one real bug.** A skip took the read-layout epoch at the end of RebindImages, so an image moved during it (a copy source for another slot's acquisition) was sampled in General through a read-layout element. The epoch is now the prediction's snapshot.

Results (plain build, same binary, mode on vs off):

| | 200–500 (`r1b-ab5`) | 1000–1600, `KYTY_BENCH_PMU=1` (`r1b-pmu`) |
|---|---|---|
| `cmd_ns` | −3.93% ±0.77 `!` | −1.85% ±0.37 `!` |
| `cpd` | −3.42% ±0.53 `!` | −2.50% ±0.47 `!` |
| `ipd` | −11.27% ±0.17 `!` | −10.10% ±0.15 `!` |
| `wait_prep` | +86 ns `!` | +66 ns `?` |
| FPS | +5.19% ±1.14 `!` | +0.53% ±0.39 `~` |

- The command thread's saving turns into prepare wait (worker latency co-limit), so FPS of record barely moves.
- **Where it comes from** (`r1b-pmu`, per draw): L1 misses 366 → 335, L2 misses 51.7 → 51.5, L3 6.9 → 7.2, cross-core snoop hits 40.5 → 39.4. Top-down: retiring −441 cycles, memory-bound −252, core-bound +207.
  - Precise L1-miss samples: the bound-image path loses ~41 (CommitBindings −11, `GetBarriers` −10, `AcquireTexture` −7.5, `FindView` −4.4, `TouchImage` −4.1, `FindRenderTarget` −3.5).
  - New ones: `ResidentStore::TrySkip` +6.6 (its entry and element tables) and `AcquireRenderTargets` +6.1 (target `Image`s the texture path used to warm).
  - The removed misses hit in L2, so they cost little. The thread's expensive misses (worker-written lines, DRAM) are elsewhere.
- **Verify:** 3 of 3 full runs clean (~6.7M draws each). Per run: 0–1 late transits and 0–1 verify races.
- **Stats** (`KYTY_RESIDENT_STATS=1`, one full run):
  - slots: 92% skip (global counters), 1.5% skip (per-image), 0.5% conflict, 3.1% null;
  - stages: 9.8% keep shader data in a buffer, 3.4% only because of the index words;
  - index words per stage (0 / 1–2 / 3–4 / 5–8 / 9–16 / 17+): 27% / 16% / 21% / 0.6% / 23% / 12%;
  - elements: ~1.08k alive (peak 1.5k), ~470k one-draw conflict elements per run, 127 samplers.
- **Off mode** against the merge-base: `cmd_ns` −0.94% ±0.98 `~`, `cpd` −0.22% `~`, `ipd` +0.74% `!` (two binaries).

### Run-level commit: parked, instruction cuts that don't become cycles (Track B, 2026-11-12)

A draw that continues a run (same target registers, nothing drawn in between, no run epoch moved) skips work the previous draw already did. Three pieces were built, verified, and parked on branches:

| Piece | Branch | Hit rate | ipd | cpd | FPS |
|---|---|---|---|---|---|
| Run-level render targets (#89, draft) | `trackB/run-targets` | 86% of draws continue a run; 42% keep their targets | −4.64% ±0.12 `!` | −1.23% ±1.00 `~` | −0.3% `~` |
| + a depth target tracked per subresource keeps the run (`Image::TransitKeeps`) | `trackB/run-target-regs` | targets kept 41% → 75% | −1.16% ±0.25 `!` | −0.93% ±1.28 `~` | +2.9% `~` |
| + the pipeline by interned worker key id (cut 3, `KYTY_PIPELINE_IDS`) | `trackB/pipeline-ids` | 68.5% of draws reuse the last pipeline | −1.13% ±0.15 `!` | +0.46% ±0.89 `~` | −1.3% `~` |

- All are same-binary A/Bs through the kill switch, 4 runs a side, frames 200–500, plain build. Verify is clean for each (1.5M run draws).
- **Conclusion:** instruction cuts of 1–5% on the command thread land at 0 to −1.2% cycles. The thread is ~41% memory-stalled, and the removed work overlapped the stalls. Resident textures showed the same pattern (−10% instructions, −2.5% cycles).
- **When to revisit:** after B4's hand-off stages B–D and the prefetch change the stall picture. Measure all three together, same binary, on the PGO build.
- **Correctness lessons** (Sol, three rounds on #89):
  - **The run baseline must be the epochs from before the first target resolve.** It must also be rechecked at every skip. Otherwise an epoch move during the commit (this draw's bindings marking a target's metadata GPU-written; another thread's store) is absorbed and a later draw skips a needed color clear.
  - **Verify must assert the skipped work had no effect at all** (clear work, stores by this thread, flags, layout), not only that it found the same image. The global guest-sync epoch also moves for the priority thread's stores, so verify counts this thread's own stores.
- Value-change detection for the target registers, done by comparing the registers at post, cost what it saved (cycles +0.7% `~`). B4's line-change mask (#104) replaces it.

### Upload copies and guest writes nobody observes (Track B, 2026-11-12)

PrepareBda's uploads, measured on a plain build in Sky Garden:
- **Volume:** ~760 MB/s to ~1 GB/s (~18.6 MB a frame, ~3.6 KB a draw). That is ~2,900 uploads/s, each one contiguous range of ~260 KB.
- **Cost:** 7.9% of the command thread's user cycles (~270 ns/draw); ~5% on the PGO build.
- **Source:** almost entirely pages the guest CPU wrote, found by the asynchronous write scans. Per page, 69% of the bytes and 88% of the cache lines change.
- **Readers:** with `KYTY_BDA_STATS=access`, **no DMA shader read any of the uploaded bytes** (22 shaders instrumented, one access bit in total). Binds later cover 44% of the bytes; 56% is rewritten before any bind reads it. This makes "upload only what a DMA shader or bind can read" the strongest research lead for removing the copy, if a sound bound on DMA reads can be found.
- **Lead time** from a range being queued (collection or store) to the PrepareBda that takes it, by bytes: <100 µs 2.5%, 100–250 µs ~20–29%, 250–500 µs ~35%, 0.5–1 ms ~10%, >1 ms ~20–35%.

**CPU-dirty pages are unwatched, so writes to them are never observed.**
- When a page becomes CPU-dirty, the tracker stops watching it (`RegionManager::ChangeState<Cpu,true>` → `UpdateProtection`). Later guest writes raise no fault and no scan report until the page is uploaded and watched again.
- A probe hashed a 1/16 sample of pages as they were queued and compared them at upload: **0.4–0.6% had been rewritten** with no re-queue in between (~1,000 pages/s). Usually most of the page differed (1.5–4 KB of 4 KB).
- Queue to upload took a median of 7.7 s, a p10 of 1.3 ms and a p90 of 31 s: mostly pages PrepareBda doesn't take, which a bind uploads much later.
- Today this is harmless, because uploads read guest memory at upload time.
- **Anyone who copies guest data earlier than its use** (pre-staging, worker-side copies, snapshots at a collection) must re-arm write tracking on what they copy, or they will upload stale bytes. That holds even where the guest orders its write before the draw with a label and a wait, because that wait's collection doesn't see unwatched pages.
- Probe and page versions: branch `trackB/bda-prestage`, `KYTY_BDA_LEAD_STATS`. Pre-staging, which re-arms tracking, was then built and parked: see the next entry.

### BDA pre-staging: correct, but tracking the pages costs more than copying them (Track B + Track K, 2026-11-12)

PrepareBda's guest→staging copy was moved to a stager thread. The thread arms each queued range (`LibKernel::Memory::ArmStaged`, #116) and copies its pages into a host-visible arena ahead of the draw. PrepareBda then records GPU copies from the arena for the pages whose version is still the one copied. Design: `docs/mt-rewrite/bda-prestage-design.md`. **Parked, unmerged:** branch `trackB/bda-prestage-pr`, kill switch `KYTY_BDA_PRESTAGE=0|1|verify|strict`.

**Correctness: the strongest evidence we have for the asynchronous write tracker.** In `verify` mode, every staged page used is compared with guest memory at use. A difference is classified by whether the page still holds an unconsumed write mark (`PeekAsyncGuestWrites`):
- **marked:** a store after the last collection. It has no ordering against this draw: every legitimate ordering goes through a collection (submit scan; a wait, semaphore or rewind satisfied by a guest value; a collecting resume), and that collection consumes the mark and moves the version first.
- **unmarked:** a real loss.

For every marked difference, the page's next use must see a newer version (the "loop" check). Plain build, Sky Garden 1000–1600:

| Run | Unmarked | Marked | Loop closed / open |
|---|---|---|---|
| verify | **0** | 41 | 41 / 0 |
| `--frame-skip auto` | **0** | 12 | 12 / 0 |
| `KYTY_RESIDENT_TEXTURES=1` | **0** | 14 | 14 / 0 |
| fault injected (`KYTY_BDA_PRESTAGE_DROP_ARM=1000`: the arm is skipped) | **8 caught** (copies 0.8–14 ms old) | 11 | 11 / 0 |

- With `DROP_ARM=64`, the stale copies crashed the game during loading (a host SIGSEGV at a guest address).
- The unit test (`virtual_memory_allocation_tests --bda-prestage-only`) catches a stale copy used 1.5 s after staging.
- **Marked differences:** 1–4 pages/s out of ~150k compared. Size: median 1.4 KB, range 1 byte to 2.1 KB.
  - **55% hit one buffer at 0x55aaf0000:** ~37 contiguous pages of floats, all rewritten together ~10 ms after staging (per-frame animation or particle data).
  - A one-byte flag at offset 0x740 of page 0x50ecf3000 recurs (01 → 00), most likely a guest-side status word.
  - The rest is spread over ~10 buffers.
- The PGO build was not run.

**Performance: a regression.** Same binary, `KYTY_BDA_PRESTAGE=0` vs on, 3 runs a side, plain build: `cmd_ns` **+4.6% ±0.5 `!`**, cycles/draw +0.0% `~`, `cmd_sns` +60% `!`, ipd +2.4% `!`, FPS **−7.0% `!`**.

| | Effect | Why |
|---|---|---|
| Saved | memcpy 7.9% → 3.0% of the command thread's user samples. With the stager on a P-core, cycles/draw −3.3 to −3.5% | 63–72% of PrepareBda's ~1 GB/s served from staging (E-core), 95–99% (P-core) |
| Kernel | `cmd_sns` 238 → 387 (E-core) / ~500 ns/draw (P-core) | Collection scans go from 5.0k to 14.4k calls/s (+25 ms/s, ≈ +95 ns/draw): #116 must scan every chunk that holds staged pages, and 8–12k pages stay staged at any time. Voluntary context switches go from 369 to 8,640/s (the command thread blocks on the stager's mutex) |
| GPU | `KYTY_GPU_TIME` busy 627 → 660 ms/s (+5%) | Staged runs were copied from the host-visible arena on the universal queue, not through the transfer queue into a device ring |
| User | Take 1.7% of the command thread | A per-page hash map and its allocations |
| FPS | −5.5 to −7% at every placement | Not core competition (see below). GPU time and the command thread's kernel time |

**Placement** (stager busy share and the share of bytes it serves, at ~1 GB/s):

| Stager CPU | Busy | Served | FPS (54.5 off) |
|---|---|---|---|
| E-core 18, shared with the game | 34–36% | 63–72% | 51.5 |
| CPU 4 | 18.7% | 95% | 51.5 |
| CPU 9 | 20.3% | 99% | 51.3 |

On an E-core, the misses come from lag: pages re-queued 0.3–5 ms before use that the stager hasn't reached yet. Coalescing the inbox and skipping copies that are still current raised the served share from ~40% to ~70%.

**Conclusion: for the pages the game rewrites every frame, tracking them costs more than copying them.** A staged copy is only safe if the page is re-armed, and every armed page has to be scanned at every collection.
- Fixing the GPU path and the lock would at best leave ~−1% `cmd_ns`, possibly flat, against carrying a second writer of guest-data copies with its own validation rule.
- Upstream's alternative (0ec2cd5d, Track U) was flat too.
- The next question is the reverse: leave those pages unwatched and treat them as dirty at every use ("hot-dirty", Track K + Track B).

**#116 stays in dev** (`ArmStaged`/`Unstage`, versions bumped where a write mark is consumed, staged chunks in every scan). With nothing staged it is neutral (gate.sh, `cmd_ns` −0.05%). Its arm API has no caller on dev; Track K may reuse the page versions. `pageVersions.h` is on dev; its bump hooks in `MemoryTracker`, `NoteGpuWrite` and the backing-store writes are only on the parked branch.

### PrepareBda's upload copies: parked after four attempts (Track B + Track K, 2026-11-12)

**Conclusion.** PrepareBda's guest→GPU copy (~760 MB/s, ~270 ns/draw plain, ~5% of the command thread on PGO) stays. Four attempts each ran into the same two facts:
- **binds consume ~44% of these bytes** (#114), so any scheme that skips the eager upload moves those copies to bind time;
- **tracking a page costs more than copying it**, for the pages the game rewrites on a schedule.

| Attempt | Result |
|---|---|
| Pre-staging (copy ahead on a stager, re-arm tracking) | Correct (0 unmarked mismatches), but `cmd_ns` +4.6%, FPS −7%: arming forces scans (+95 ns/draw sys). Entry above |
| Upstream 0ec2cd5d (re-queue only DMA-accessed pages; Track U port) | Cycles flat, ipd +7.6%, prepare_ns +29%: uploads move to bind time on the workers. Not taken |
| Hot-dirty (leave rewritten pages unwatched, dirty at every use; Track K + Track B probes) | Not built. The churn (~4,650 invalidated pages/frame) is 94% pages rewritten **every 3rd frame** (triple-buffered). Between writes they get ~13–19 clean binds (#60 worker lookups); "always dirty" turns each into a copy: ~5.6 command-thread copies per write vs 1.0 today, and #60/#78 stop applying. ~58% of those writes are read only by PrepareBda's upload, but an unwatched page can never be queued for PrepareBda again, so it would be synced on every PrepareBda (~180/frame, ~5 GB/frame) |
| In-place reads (C1 stage 2, and a hybrid limited to pages no bind reads) | **Unsound with decode-time labels**, hybrid included: see below |

**Why in-place reads can't be made exact cheaply.** An in-place read sees guest memory at host GPU execution time, not at the dispatch's PM4 position.
- EOP and RELEASE_MEM labels are stored when the command thread decodes them, before the host GPU runs the work.
- A guest that waits for such a label and then rewrites a page is fully ordered on a PS5. Here its store can land while the host GPU still reads the page, so the read returns bytes the real GPU could never have seen. This is not the "unordered store" case pre-staging's gate accepted (a pending write mark): the store is ordered *after* the draw.
- The pages a hybrid would redirect are exactly the scheduled, every-3rd-frame rewrites. C1 measured the exposure: 2.3% of redirected pages changed between record and completion.
- Nothing in a page classifier bounds that window, since it depends on how far the host GPU lags the decoder, not on guest ordering.
- Making it exact means stopping the guest's store until every dispatch that could read the page has finished. Two ways, both too costly:
  - **Late labels** (C1 stage 1): −15% FPS.
  - **Per page:** keep redirected pages under the synchronous write-fault path (SIGBUS, several µs plus lock contention per fault, "Write tracking without signals"), and in the handler either (i) block the store until the host GPU passes the reading dispatches, or (ii) swap the page-table entry to a copy of the old bytes first.
    - (ii) races the GPU. A shader that already loaded the old entry, the pointer into guest memory, can still read after the store lands. Nothing on the GPU side tells the CPU which entries are held.
    - (i) is exact, but costs a synchronous fault on ~2.5k pages/frame (≥ ~10 ms/frame of game-thread time at several µs each) plus stalls of up to the GPU lag. That is several times the ~1.4 ms/frame the copy costs.
- A static bound on DMA reads (below) would avoid all of this. It is blocked by pointer-chasing loads.

**DMA read paths (for whoever bounds DMA reads next).** File references are to `dev` at this entry.
- **What makes a shader DMA-using** (`uses_dma`, ShaderInfoCollection.cpp `Visit` ~:317–334, :520): set on the live IR after specialization (upstream 0b82396f, ours cb32ec2c; no change in Sky Garden). The triggers:
  - FLAT/GLOBAL/SMEM address loads (not scratch, not `planning_only`). Address *stores* fail, so DMA is read-only.
  - `IndirectBuffer` buffer ops: V# words the host can't evaluate (ResourceTracking.cpp ~:4232).
  - BVH intersects.
  - A scalar load with a host-evaluable base and an immediate offset is `planning_only`: the SRT walker runs it on the host (ResourceTracking.cpp ~:875–934).
- **How a load is emitted** (spirvEmitterMemory.cpp `get_bda_pointer` ~:1385):
  - A 16 KiB page lookup in `m_bda_pagetable_buffer` (entries written on buffer (un)registration, whatever the sync state).
  - **Entry 0:** `RecordBdaFault` and a read of 0 (~0 in BVH shaders). The fault makes `FindBuffer` register the page at the next frame boundary (faultManager.cpp ~:136), with no retry.
  - **A backed page with unsynced CPU writes reads stale bytes, undetected.** That is what PrepareBda prevents.
- **When PrepareBda runs:** once per draw or dispatch with any `uses_dma` stage (descriptors.cpp ~:2128, renderCompute.cpp ~:416/:596), ~155–180 per frame. It drains the whole global `m_bda_sync` queue, not per shader.
- **Where addresses come from:**
  - SRT-sourced global bases are recorded (`address_bases`, ResourceTracking.cpp ~:4296), but with **no offset bound**: an SGPR base plus a VGPR offset reaches base + 4 GiB.
  - Full 64-bit VGPR addresses have no recorded origin.
  - `IndirectBuffer` loads are bounds-checked per load against their runtime V#, but the host can't evaluate it.
  - BVH reads chase node pointers.
  - Nothing rules out a scalar or global load feeding a buffer descriptor.
  - `WalkSrt`, `AffineIndex` and `EvaluateSource` (SrtWalker.h) are the building blocks a static range would need.
- **Options for a bound:**
  - (a) A static range per load, from `address_bases` plus value ranges for offsets: 2–3 weeks.
  - (b) A per-shader reachable-buffer set: ~1 week.
  - Both become "all memory" for any shader with a full-VGPR, `IndirectBuffer` or BVH load. **Start with a census of the DMA shaders' load kinds** (22 in Sky Garden per #114) before building either.
  - (c) A runtime guard (in-place redirect, or upstream's first-touch tag) is unsound or inexact, as above.
- **Measuring DMA reads:** `KYTY_BDA_STATS=access` sets an access bit per 16 KiB page in `get_bda_pointer` (an atomic per call, which halves FPS) and matches it against upload windows (`BdaStats`). Result: ~1 page/s read across 22 shaders.

### The command thread's own data misses, by level (Track B, 2026-11-12)

How it was measured:
- Precise `mem_load_retired.l1_miss` / `l2_miss` samples with data addresses on KytyGpuCmd: plain -O3 -g build of #89, Sky Garden frames 1000+, 10 s.
- Data addresses were mapped to `Image` objects through the SlotVector's 256-slot chunks, with field offsets from an `offsetof` probe. The method is in `/tmp/trackB/mem-method.md` while the machine keeps it.
- Totals per draw: 7,700 loads, 329 L1 misses, 53 L2 misses.
- **Weigh misses by level.** An L1 miss that hits L2 costs ~16 cycles and mostly overlaps. Resident textures removed 31.6 L1 misses per draw for −2.5% cycles.

| Producer | L1/draw | L2/draw | Status |
|---|---|---|---|
| Hand-off reads of the QueuedDraw slot and worker records (ExecutePreparedDraw, CanReuseBindings, AdoptPrograms, FinishQueuedDraw, ResolveProposedTexture, PrepareDrawRenderState, PostQueuedDraw) | ~62 | ~18 | B4's hand-off stages |
| libc copies and compares (ResolveProposedTexture 6.4, FindGraphicsPipeline's key compare 2.9, FindBuffers 2.9, PostQueuedDraw's register copy 2.7, TryReadFast 2.4) | 26.6 | ~7 | B4's stages; cut 3 for the key |
| NVIDIA driver on the command thread (DispatchDirect, Submit, MasterSemaphore::Refresh, Image::Transit, EndRendering) | 10.6 | ~2 | roadmap step 7 |
| Bound-image path in `Image` objects (7 of `Image`'s 25 lines per bound image) and `views` arrays | ~43 | ~3 | removed by resident textures (#100); `ImageHot` dropped |
| **Buffer cache** (HasGpuDirtyBytes' std::map walk 8.9 / 0.0, ObtainBuffer 7.3 / 2.4, RebindBuffers 5.3 / 0.5, FindBuffer, IsRegionGpuModified, TouchBuffer) | 35 | 3.6 | **not worth flattening:** a ceiling of ~500 cycles before overlap |
| `hw_check` (per-draw register validation, reads the register copy) | 7.5 | 2.8 | to be skipped under B4's register change flags |
| Recorder ring (Reserve, WaitDone) | 1.9 | 2.0 | |

### Commit-side prefetch: early hand-off lines don't buy cycles (Track R, 2026-11-12)

The question was whether the command thread's cross-core hand-off loads cost cycles because they are late. The test: while committing queued draw N, the command thread prefetches what it will read for the draws behind it, one pointer level per draw of distance. Prototype on branch `trackR/commit-prefetch` (`KYTY_COMMIT_PREFETCH=1`, `KYTY_COMMIT_PREFETCH_LEVELS` bit mask), not merged. The levels:
- 1: slot N+3's lines the commit reads (B4's hand-off map; not the register snapshot, which the command thread owns);
- 2: what slot N+2 points at: proposal arrays, log and buffer vectors, target records;
- 4: texture records of N+1;
- 8: images of N.

Records and images that the previous draw also proposed are skipped by a pointer compare: only 0.76 new records and 3.2 new images per draw remain.

PGO, same binary (dev ab68b255 + the prototype), frames 1000–1600, `KYTY_BENCH_PMU=1`, n=3 each, interleaved. Changes vs off:

| Command thread, per draw | all (15) | without images (7) | slot only (3) |
|---|---|---|---|
| `cmd_ns` (off 3203) | −0.44% ±0.44 | +0.09% ±0.64 | −0.05% ±0.85 |
| cycles (off 14413) | −0.56% ±0.33 | +0.02% | −0.25% |
| instructions (off 21715) | +2.66% | +1.74% | +1.04% |
| forwarded snoop-hit loads (off 29.4) | −29.0% | −29.6% | −26.0% |
| L2 misses (off 47.9) | −21.2% | −19.4% | −16.2% |
| L3 misses (off 7.3) | −8.4% | +0.9% | +0.1% |
| memory-bound cycles | −4.7% | −3.3% | −6.6% |

- **Removing the snoop-hit loads did not save cycles.** The slot levels alone (3) cut forwarded snoop hits by 26% (29.4 → 21.8 per draw, 7.6 fewer), against 29% (8.5 fewer) with all levels. Memory-bound cycles fall 3–7%, yet total cycles and `cmd_ns` don't move.
  - The likeliest reading is that out-of-order execution already hid most of these loads' latency, so the ~55 cycles per snoop-hit load estimated in "Where the command thread's cycles go" overstates their cost.
  - The other reading is that the prefetch's own instructions cancel the saving. These runs can't tell the two apart. Level 3 adds ~225 instructions per draw (+1.04%), takes most of the snoop reduction, and moves cycles by −0.25%, within noise. Either way, the snoop-hit loads it removed were worth no more than those instructions cost.
- **So judge hand-off work by lines and copies removed, not by snoop counts.** These measurements test early arrival by prefetch, not by a layout change. A layout that only makes the same lines arrive earlier would be expected to save little too. Removing the lines, or the work that reads them, is what can save cycles.
- An earlier version, which prefetched every image and record with no pointer compare, also took a third of the snoop hits for ~690 more instructions per draw: cycles −0.97%. Prefetch in the prepare worker, so the line sits in the worker's cache, changed nothing measurable.
- Draws already prepared at distance d when draw N commits: d1 81%, d2 66%, d3 57%.

### Build modes: PGO + ThinLTO (Track P, 2026-11-11)

The research pass (branch `research/60fps-gap`) found PGO + ThinLTO worth +12.5% with no source
change: ThinLTO alone +4.7%, PGO alone +7.4%; `-march=native`, no frame pointers and split
functions added nothing. Track P made it a build mode (CMake `KYTY_LTO`, `KYTY_PGO`,
`KYTY_PGO_PROFILE`; `tools/astro-bench/{buildmode,build,pgo-train,refbuild}.sh`). The profile
comes from an instrumented build (`-fprofile-generate`) running the bench: the bench kills the
emulator, so `KYTY_PGO_DUMP=<prefix>` makes it write its counters every 10 s, alternating between
two files so a kill mid-write leaves the other whole. (The profile runtime truncates a file only
once per process and appends after that, so the dumper deletes a slot before rewriting it; a
first version summed ~4 snapshots per file, which over-weighted the first seconds of the run.)

On `dev` ffcface5 + the tooling (44954b73), same batch, frames 1000–1600: plain 48.34 / 48.94,
PGO + ThinLTO 53.89 / 54.21; command thread instructions/draw −26.9% ±0.2, cycles/draw
−13.4% ±0.8, FPS +11.1% ±0.9; prepare worker 3.40 → 2.97 µs/draw. (Before the dumper fix, on
084ca762: 49.29 / 48.56 → 55.04 / 54.76, +12.2%; with the fixed profile, 49.35 / 49.56 →
54.14 / 54.69, +10.3%. Between-binary FPS pairs carry ~1% layout noise, so these agree.)

Does a profile trained on the merge-base under-serve a branch's new code? #83 (texture-cache depth
fast path and color-clear memo, +1.8% FPS on plain builds), all PGO + ThinLTO.

FPS only, #83 merged onto 178862af (profiles from the first dumper), two batches ordered dev,
#83/dev profile, #83/own profile, then reversed:

| Build | Runs | Mean |
|---|---|---|
| `dev` with the `dev` profile | 55.32 / 54.23 / 55.04 / 54.59 | 54.80 |
| #83 with the `dev` profile | 54.25 / 54.79 / 55.14 / 55.12 | 54.83 |
| #83 with a profile retrained on #83 | 54.74 / 55.32 / 53.87 / 54.86 | 54.70 |

Per-draw metrics, #83 merged onto 44954b73 (fixed dumper), one batch, 3 runs a side, frames
200–500 (`results.py verdict`, ± is one standard deviation of the difference):

| Comparison | instructions/draw | cycles/draw | FPS |
|---|---|---|---|
| #83: own profile vs `dev` profile (same source) | −2.46% ±0.24 ! | +2.06% ±1.09 ~ | −4.3% ±2.0 ? |
| #83 vs `dev`, both on the `dev` profile | −1.02% ±0.24 ! | −2.33% ±1.09 ? | +4.2% ±1.9 ? |
| #83 on its own profile vs `dev` on its own | −3.46% ±0.17 ! | −0.32% ±0.74 ~ | −0.3% ±1.4 ~ |

A stale profile changes the branch's instruction count measurably (PGO inlines and lays out
uncounted code differently), but its cycles/draw and FPS cost is invisible next to the ~1% layout
noise between two binaries (and the 4-run FPS batch above shows none). So A/Bs can share a profile
for cycles and FPS; instructions/draw is screened in plain mode or same-binary instead. #83's own
gain mostly disappears on PGO builds (cycles/draw −0.3%..−2.3% depending on the profile): PGO's
layout and inlining already capture much of what it saves.

Correctness of the PGO + ThinLTO build: the unit suites pass (`build_flags_tests`, which checks
that `-ffp-contract=off` survives, `shader_recompiler_compute_tests` and the other suites listed
in the PR); all four verify modes
(`KYTY_PREPARE_VERIFY`, `KYTY_BINDING_REUSE_VERIFY`, `KYTY_WORKER_TARGETS_VERIFY`,
`KYTY_WORKER_PIPELINE_KEY_VERIFY`) ran clean together to frame 1600 (11.8M reused stages, 22M
target and 8M pipeline-key checks); the level renders the same as a plain build.

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
- A helper that starts `vpad.py` from inside a benchmark must close the bench-lock descriptors
  (`9>&- 8>&- 7>&-`): otherwise vpad inherits the exclusive lock and holds it after the
  benchmark exits, blocking every agent (it happened once, for ~25 min).
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

### Game speed is frame-locked; optional frame skipping (Track S, 2026-11-12)

- **The game steps a fixed amount per flip.** The game logs `SubLevelLocator sublevel_locator_veg_chibi … 6->7` ~850 flips after `StartLevel` in every bench log from 15 to 59 FPS (≈280 logs), while the wall time to it runs from 57.6 s down to 17.4 s. At 15 FPS each frame spans 4 vblanks, so it counts flips, not vblanks or measured time. The eboot has ~150 sites that load the literal 1/60 (`eboot+0x834aeb0`) as a per-update step (for example `eboot+0xf182f7`, `+0x40040fd`). So game speed = flips/s ÷ 60: 0.82× at 49 FPS. No boot argument changes the step, and the clock reads don't feed it. `eboot+0xe07df0` is the audio subsystem timing its stages.
- **Check it on any bench log** with `tools/re/gamespeed.py /tmp/bench-<label>.log`.
- **`--frame-skip auto`** (off by default; docs/frame-skip.md) drops the draws of some frames and still flips on every vblank. On `dev` 891ff146, Sky Garden frames 1000–1600:

| | Flips/s (game speed) | Shown/s | Worst gap between shown images |
|---|---|---|---|
| Off | 49.4 (0.82×) | 49 | 1–3 vblanks |
| Auto, two runs | 57.4 / 58.8 (0.96× / 0.98×) | ~28 / ~29 | 3 vblanks (6–7 only in level-start stalls) |

- **Why auto stops short of 1.0× today:** a skipped frame still costs the command thread ~8 ms. Most of that is the ~620 draws a frame it must keep: atomic feedback writes into three 128-byte buffers the CPU reads back, and storage-image writes. A drawn frame costs ~22 ms, so with at most one skipped frame in a row the skip share sits at its 50% cap. The gap closes as the base frame rate rises. `KYTY_FRAME_SKIP_MAX=2` lifts the cap and trades smoothness for speed.

### Dynamic resolution reacts to skipped frames (Track S, 2026-11-12)

With frame skipping on, Sky Garden's main pass sometimes moves from its 1920×1080 targets to 2432×1368 ones: 35 of 600 frames in one run, not seen in the mode-off diagnostic runs. The game's dynamic resolution sees cheap frames and raises the resolution.

The emulator writes guest GPU timestamps when the command thread decodes the packet: RELEASE_MEM/EOP `kRefClk` from the host TSC (`Sync::ReadReferenceClock`), and the AMPR time-counter writes. A skipped frame's "GPU time" is therefore its short decode time. It is not traced which of these the game's dynamic resolution reads.

Two effects:
- A target unused while the scene sits at another resolution is not a "periodic" target. Frame skipping's keep rule uses write runs for this reason.
- Raised resolution adds host GPU work to the drawn frames.

### GPU waits and the frame timeline (Track W, 2026-11-12)

Three PGO builds of `dev` with a cheaper command thread each (`cmd_ns` 3385 → 3332 → 3270) all
ran ~54.8 FPS: the command thread just became less busy (95.5% → 93.6%). `KYTY_GPU_WAIT_DIAG`
(below) on `dev` e1ad8deb, frames 1000–1600, ~18.2 ms per frame:

- **One site waits on the GPU:** `BufferCache::DownloadBufferMemory<false>` (`bufferCache.cpp`, the
  synchronous branch), from `GuestGpu::Process` → `RenderContext::CollectAsyncWrites` →
  `BufferCache::InvalidateMemory` → `ReadMemory`. At the start of a guest submission, the write
  scan finds a CPU write on a page the GPU wrote, and the GPU's bytes are downloaded before the page
  is invalidated. Almost always one 12 MiB buffer (`0x55b408000`), a 32 KiB window. 0.23–0.29
  waits per frame, 2.8–2.9 ms each (max 4 ms), each for its own fresh submit with 10–11 earlier
  submits still on the GPU: 0.62 ms per frame, 3.4% of the command thread.
- **The GPU is busy 12.3 ms per frame and idle 5.9**, in ~5.5 gaps per frame; 23 submits per frame,
  21% of them reaching an idle GPU. The main pass is 1920×1080 (5 targets) and GPU time matches
  the ~12.2 ms of Track G4, so dynamic resolution has not raised it.
- **A frame (flip decoded at t = 0):** the previous frame's last ~620 draws are recorded from
  −4.2 ms but submitted only at −0.05 ms, after the GPU sat idle for 3.5 ms; they take the GPU
  5.5 ms. From 0.2 to 4.4 ms the command thread decodes one guest command buffer (1,218 draws)
  without submitting; its pieces queue behind that chunk. At 5.1 ms the next submission's write
  scan downloads and waits 3.0 ms. The flip's own submit completes 4.25 ms after the flip decodes.
- **So the frame was bounded by the command thread plus GPU pipeline latency, not GPU
  throughput:** every ms the command thread saved before the download became wait there, because
  the heavy end-of-frame chunk reached the GPU late and in one piece.
- **Other sleeps of the command thread, ~3%** (`KYTY_OFFCPU_SAMPLE=1`): label peeks
  (`AsyncPageUntouched` in `WaitRegMem`) blocked on the submit-scan mutex ~0.8%,
  `WaitPriorityOperations` ~0.8%, `TakeKeptWrites` waiting for in-flight submit scans ~0.3%.
- Ruled out: vsync (the command thread never runs out of work; flips 1 vblank apart at p50), the
  game's threads, the present path (its own scheduler; the command thread never waits there).
- **Early submit (`KYTY_EARLY_SUBMIT`, default on, `KYTY_EARLY_SUBMIT_US`=300):** at packets that
  already drain the draw queue and end dispatch chains and atomic batches, the command processor
  submits the open command buffer when every earlier submission has finished on the GPU and the
  buffer is at least 300 µs old. FPS +3.6% / +5.0% (same binary, two bases); GPU wait 0.62 → 0.08 ms per frame
  (average wait 2.8 → 0.41 ms), flip completion 4.25 → 2.39 ms after decode (measured at 150 µs), ~5.5 early submits
  per frame; the command thread is ~97% busy again, so FPS follows `cmd_ns` again. 150 vs 300 µs:
  `cmd_ns` equal, GPU wait 2.5–4.7 vs 1.8–2.1 ms/s, so 300.
- **Scheduler waits (`SCHEDWAIT` lines, `KYTY_GPU_WAIT_DIAG=1`; after early submit, dev 2662671e):**
  the command thread blocked in `WaitPriorityOperations` 166 times/s, 84 µs each (13.9 ms/s, 1.4%),
  almost all from the per-draw `PopPendingOperations` (`DrawIndex`, `DrawAuto`, dispatches). In
  100% of them the GPU had already finished the tick: the wait was for the priority thread to wake
  from its own semaphore wait and run the tick's callbacks (interrupts, readback publishes, flips).
  `RetirePendingOperations` (the per-draw/per-dispatch callers) now leaves such an operation queued
  for a later draw instead (`KYTY_RETIRE_NOWAIT=0` waits again): 13.9 → 0.1–0.6 ms/s.
- **Left on purpose:** the same synchronous download still waits ~0.4 ms, 0.2 times per frame
  (~0.08 ms per frame, `bufferCache.cpp` `DownloadBufferMemory<false>` from
  `CollectAsyncWrites`). Making it asynchronous is worth little now; re-check with
  `KYTY_GPU_WAIT_DIAG=1` (`GPUWAITSITE`) if it grows as the command thread gets cheaper.

### Driver work on the command thread (Track W, 2026-11-12)

`KYTY_DRIVER_STATS=1` (with `KYTY_GPU_WAIT_DIAG=1`), an LBR profile of the command thread and
`KYTY_KERNEL_STATS`, PGO `dev` 245d7f2b, 58.5 FPS: user 899 ms/s, sys 63 ms/s (47 timed by the
emulator's own syscall sites, ~16 untimed: driver ioctls, page faults); 6.0% of user samples
are in the NVIDIA driver (~0.9 ms per guest frame).

| Path | Per frame | Command-thread ms/frame |
|---|---|---|
| Texture-cache images: `FindImage` creates ~2 per frame, all VMA dedicated; deferred `TextureCache::DeleteImage` frees them in retirement | 2 + 2 | 0.20 + 0.17 (mostly kernel) |
| Direct recording (dispatches, barriers, `EndRendering`, clears, descriptor sets) | ~1,700 calls | ~0.45 driver user time, plus 0.15 recorder drains before them |
| `Submit` (drain, end, `vkQueueSubmit`) + `BeginCommand` | 29 | 0.18 + 0.07 |
| `MasterSemaphore::Refresh` | ~620 | 0.06 |

Moving direct recording and submission to the recording thread (progress row above) removes
~0.75 ms per frame of command-thread CPU. The two only pay together: with one moved, the other's
direct calls drain the recorder, which then holds more work. Remaining direct calls (readback
downloads, the tiler's buffer path, `CopyImage`, the fault buffer) drain ~15 ms/s; image churn
(~0.37 ms per frame) is next.

## Critical path after B4 (2026-11-07)

`KYTY_STALL_DIAG=1` (dev abc268e0, ~35 FPS). Shares of wall time, steady over each second:

| Thread | Busy | Waiting |
|---|---|---|
| GPU command thread | ~86% | another thread's synchronous command 9.7–10.9% (of which on the GPU 8.2–9.0%), prepare worker 2.2–2.3%, scheduler callbacks 1.7–2.6%, recorder ring 0.3–0.5%, no guest work 0%, blocked guest waits 0% |
| Prepare worker | ~50% | spinning ~23%, sleeping ~26% |
| Recorder | ~9% | spinning ~63%, sleeping ~26% |

- **The command thread is the gate, and the guest is always ahead of it.** It never runs out of
  submitted work. Waits on PM4 GPU work are 0%. Its waits on the GPU all happen inside other
  threads' synchronous commands: a guest read fault on GPU-written memory runs
  `BufferCache::ReadMemory` → `DownloadBufferMemory` → `CommandScheduler::Wait` on the command
  thread, which then waits for the GPU to finish everything queued before the download (gdb
  stack sample: `MasterSemaphore::Wait <- CommandScheduler::Wait <- DownloadBufferMemory <-
  ReadMemory <- SendCommandSync <- GuestGpu::ProcessCommands <- CommandProcessor::ProcessPm4`).
  That is ~9% of the command thread doing nothing.
- **The worker and the recorder have headroom.** Their CPU use (~73% each in `top`) is mostly
  spinning for the next draw. The recorder does ~9% real work.
- **Vsync.** `KYTY_TIME_DIAG=1`: 35 flips/s, `flip_vbl` p50 2, p99 2, max 3, so frames take 1 or
  2 vblanks. FPS equals command-thread throughput: 181k draws/s ÷ 5.2k draws per frame. No stage
  waits on vblank. For 60 FPS every frame has to fit one vblank: ~3.2 µs per draw on the
  command thread.
- **The game's threads.** gdb samples of the busiest guest thread (35% CPU): 26 of 40 samples are
  blocked on a guest pthread mutex inside the game, and one guest site calls `usleep` ~1M times a
  second (polling). The tbb workers mostly wait on guest condition variables. None of the game's
  threads limits the frame: the command thread always has work queued.
- **Locked instructions.** ~50 per draw on the command thread (`perf` event
  `mem_inst_retired.lock_loads`). Removing the texture cache's lock from memo hits moved its cost
  to the next locked instruction: no gain. Resolving texture memo misses into their slot (two
  1.2 KB copies instead of five) gave no gain either. Neither is merged.
- **Asynchronous readbacks (after the above).** A guest thread's read fault now costs the command
  thread only the download's recording and submission; the faulting thread waits for the
  priority runner to publish the bytes, then a second short command releases the pages
  (`BufferCache::ReadMemoryAsync`). Pages stay GPU-owned until no newer GPU write or download
  covers them; faults on a page with a download in flight join it. `KYTY_SYNC_READBACK=1` restores
  the blocking path; `KYTY_READBACK_VERIFY=1` downloads each published range again before its
  pages are released and compares: 26,000 checks in a Sky Garden run, 0 mismatches. The command
  thread is now ~98% busy (STALL: prepare ~2%, scheduler ~0.6%, everything else < 1%), so FPS is
  its per-draw cost alone. Note `KYTY_FAULT_DIAG=1` itself costs ~9% FPS (a locked map update per
  written bind): don't compare FPS from runs with it.

- **Long frames in Sky Garden (checked after #52).** Both modes show 50–100 ms frames in the same
  stretch of wall time, 22–36 s after level start (in-level frames over 40 ms per run: async
  2.4, `KYTY_SYNC_READBACK=1` and the pre-#52 reference 2.25; 12 and 8 runs). They are not
  readback latency: capping the GPU backlog a read can see at 2 or 4 submissions
  (`KYTY_READBACK_MAX_BACKLOG`, local experiment) leaves them unchanged. Async runs show them
  in the 1000–1600 window more often only because a faster run reaches frame 1000 earlier in
  wall time; their peaks are sometimes one vblank longer (66.7 vs 50 ms). Likely streaming or
  first-use work tied to wall time; not investigated further.
- **Fences on the command thread (dev b4a91152).** A locked instruction waits for the store
  buffer to drain, and the command thread stores a lot between them, so each lock costs what
  the stores before it cost. Samples right after a locked instruction: 8.1% of the thread (the
  texture cache's spin lock 6.7%, 4.8% of it in `RefindExactImage`; recorder `Publish` 0.7%;
  overlay counters 0.3%), plus the scheduler mutex in `PopPendingOperations` ~2.5%. Removing
  them cut that to 1.1% and gave +1.3–1.9%. The drain cost moved to the locks left: the
  guest-memory rwlocks (now gone, #55) and the render context mutex `DrawIndex`/`DrawAuto` take
  per draw (~2.5%). A local experiment without those as well measured 40.18 / 40.33 against
  38.69 / 38.59 on b4a91152 (+4.2% in all). Any one lock left absorbs the drain, so they pay
  only together. With the render context mutex, `FindDepthTarget`'s lock
  and `ClampRangeSize`'s mutex gone too, +0.7% more (4 pairs on ee714f06).

- **Texture resolve on the prepare worker (dev 4cd7da6a).** The command thread's
  `ResolveStageTexture` fell from 13.5% to 6.2% of its time (misses through `FindImage` and
  `FindTextureDescription` were 7.7% of it; the worker's memo holds every resource and T# it has
  seen, where the command thread's kept one per module slot). The worker pays for it: 2.53 →
  2.94 µs/draw, busy 55% → 64%, and the command thread now waits for it 8.2% of the time
  (2.6% before; 351 vs 107 ns/draw). FPS +1.2%: the worker co-limits the pipeline in bursts, so
  most of the command thread's saving waits for a second worker. A deeper queue
  (`KYTY_PREPARE_DEPTH=8`) does not help. Making the worker's memo a flat table (−0.1 µs/draw on
  the worker) gave +2.8%: the waits are bursty, so worker latency counts more than its average.

- **Copies on the command thread (dev 4a7f63c4).** libc was 19.8% of the thread, nearly all
  memcpy. LBR call stacks: guest data 7.9% (`TryReadFast`: stream copies, uploads); bookkeeping
  structs ~8%: texture bindings' `ImageDesc` (1,134 B, 1.55%), records copied to refresh their
  registration generation (3,688 B plus the allocation, ~1.4%), `RenderColorInfo` from the color
  target memo (1,160 B, ~1.8%), `ExecutePreparedDraw` (772 B, 0.7%), `GetGraphicsPipeline`
  (516 B, 0.9%), `PostQueuedDraw`'s registers (2,708 B, 0.4%). `ImageDesc` is most of a KB
  because `ImageInfo::mip_layout` holds 16 mips. The texture side is fixed (+1.8%); the color
  target copy goes away with render-target resolve on the worker, which builds the target in the
  queued draw's own state.
- **Two prepare workers (Track B4, plan item 2).** Draw `n` goes to worker `(n - 1) % 2`; the ring's
  size is a multiple of 2, so a slot always goes back to the same worker with the resolves the
  command thread hands back for its memo. Each worker has its own SRT walk scratch per plan
  (`ResourcePlan::prepare_scratch[worker]`), memo and record serials (odd, strided by worker).
  The second worker gets CPU 6 to itself and the emulator keeps 8 performance threads (was 9).
  Same binary on dev 94c0492e, `KYTY_PREPARE_WORKERS=1` vs 2 with `KYTY_STALL_DIAG`: the command
  thread's wait for the worker fell from 6.6–8.8% to 2.5% (289–461 → ~84 ns/draw), and what is
  left is at packet drains (waits ≈ drains, ~3.4k/s); FPS 43.17 → 44.66. Each worker's job costs
  more (2.80 → 3.10 µs/draw: each memo and scratch sees half the draws), each worker is 36% busy.
  Before the in-place generation refresh (#65), records refreshed through the slot reached one
  worker only: generation refreshes on the command thread went 100k → 128k/s and two workers
  gained under 1%; with it they are 48k/s (one worker) and 83k/s (two). Game threads do not lose
  out: per-thread run-queue delay over 10 s windows is the same on both sides (`DrawThread`
  ~0.1%, `tbb_thead` 23–26% on the efficiency cores, as before). A/B against dev 94c0492e
  +3.2% / +1.8%. `KYTY_PREPARE_WORKERS=1` restores one worker and CPU 6 to the emulator;
  `KYTY_PREPARE_CPUS=<list>` places them.

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
- Tiler image path (Track G): `KYTY_TILER_DIRECT=0` tiles and detiles render targets through a
  linear buffer and a copy again; `KYTY_TILER_VERIFY=1` also runs that path for every transfer
  the image path takes and compares the bytes on the host (`TILERVERIFY` lines, one per image
  shape, a count every 64 checks; slow).
- `KYTY_PIPELINE_STATS=1` (Track G): the driver's statistics for every pipeline it creates
  (`PIPESTATS kind=… vs=<module id> ps=<module id> exec=… Register_Count=… Binary_Size=…` on
  NVIDIA), with the module ids `KYTY_GPU_TIME=draws` and `KYTY_SPV_DUMP` use.
- `KYTY_MASKED_SELECTS=0` (Track G): vertex shaders keep every EXEC select. For A/B only, in
  an install dir of its own: the shader store doesn't record the setting.
- Core layout: `KYTY_GUEST_THREAD_CPUS=<list>|none` (where the game's threads run, default the
  efficiency cores but the recorder's), `KYTY_GUEST_PCORE_NAMES=<name,...>` (game threads kept
  on the performance cores, default `DrawThread`), `KYTY_RESERVE_CPUS=<list>` (performance CPUs
  the emulator leaves free, e.g. for `KYTY_RECORDER_CPU`).
- Pipeline key on the worker (Track B5): `KYTY_WORKER_PIPELINE_KEY=0` builds every key on the command
  thread again, `=stats` prints `WORKERPIPELINEKEY` lines (draws, worker keys used, none, target
  mismatches) once per second; `KYTY_WORKER_PIPELINE_KEY_VERIFY=1` builds every used key again
  and exits naming the first differing field.
- Worker buffer lookups (Track B4 item 2): `KYTY_WORKER_BUFFERS=0` off, `=verify` checks every
  answer against a full acquisition, `=stats` prints WORKERBUFFERS lines (hits, stale, unlisted).
- Threaded draw preparation (Track B4): `KYTY_PREPARE_THREAD=0` draws at once on the command
  thread, `KYTY_PREPARE_DEPTH=<n>` queues up to n draws (default 4), `KYTY_PREPARE_CPU=<n>` pins
  the worker elsewhere, `KYTY_PREPARE_VERIFY=1` looks every adopted draw up again and exits on a
  difference, `KYTY_PREPARE_STATS=1` prints `PREPARE` lines once per second (adopted and rejected
  draws, drains, the command thread's wait and the worker's time per draw, texture resolves handed
  back to the workers' memo and adopted records whose page check ran again). `KYTY_SHARED_TEXTURE_MEMO=0`
  gives each worker its own texture memo again.
- BDA pre-staging, tracker side (Track K, `docs/mt-rewrite/bda-prestage-design.md`): `KYTY_PRESTAGE_STATS=1` prints a `PRESTAGE` line per second (arms, refusals by reason, pre-arm stores kept, staged versions moved by consumed written marks, staged pages, remap-generation moves; in Sky Garden's steady state the remap generation moves 0 times per second). `KYTY_PRESTAGE_SELFTEST=1` (test only) stages written runs from the GPU thread so the arm, consumption and unstage paths run under `KYTY_ASYNC_WP_VERIFY` without the stager.
- Kernel time (Track K): `KYTY_KERNEL_STATS=1` prints a `KSTATS` line once per second with the
  GPU command thread's `/proc` user/sys time, faults and context switches, and per call site the
  calls/s and µs/s of each timed system call kind (`PAGEMAP_SCAN` with and without written pages,
  with pages walked; `UFFDIO_WRITEPROTECT`, uffd register, `mprotect`, futex wakes, `sched_yield`,
  signal handlers) plus each site's inclusive wall time; `tools/astro-bench/kstats.py <label>`
  averages them over the `garden.sh` frame window. Costs nothing measurable when off.
- GPU waits (Track W): `KYTY_GPU_WAIT_DIAG=1` prints once per second a `GPUWAIT` line (the
  command thread's GPU waits, GPU idle time and gaps, submits, `vkQueueSubmit` time, flips), one
  `GPUWAITSITE` line per waiting call site (file:line through a defaulted `std::source_location`
  on `CommandScheduler::Wait`/`Finish` and `MasterSemaphore::Wait`; backlog, own-submit share,
  whether the waited submit reached an idle GPU) and `GPUWAITSTACK` return-address stacks (offsets
  from the executable start; symbolize with the `.map`). `=trace` also writes every submit,
  completion, wait, guest submission and flip to `$KYTY_GPU_WAIT_TRACE` (default
  `/tmp/gpuwait-trace.txt`) and prints `GPUWAITDL` per synchronous download;
  `tools/astro-bench/gpuwait.py <trace>` turns it into per-frame numbers and a timeline (`--show`).
  `KYTY_OFFCPU_SAMPLE=1` adds `OFFCPU` lines: a sampler reads the command thread's
  `/proc/self/task/<tid>/syscall` every ~200 µs and keys its sleeps by syscall and the return
  addresses on its stack (heuristic: stale words appear). Perf can't record context switches
  here (`perf_event_paranoid` 2).
- Early submission (Track W): `KYTY_EARLY_SUBMIT=0` submits only where guest command buffers end
  (and at the existing internal flushes); `KYTY_EARLY_SUBMIT_US=<µs>` sets the minimum age of the
  open command buffer (default 300). With `KYTY_GPU_WAIT_DIAG`, `GPUWAIT` shows `early_submits_s`.
- Driver work (Track W): `KYTY_DRIVER_STATS=1` with `KYTY_GPU_WAIT_DIAG=1` prints a `DRIVER`
  line per second (calls/s and ms/s of the command thread's submit, `vkQueueSubmit`, semaphore
  refresh, command-buffer begin, image and buffer create (dedicated or not) and delete) and
  `DRIVERSITE` lines: direct recordings (`CommandBuffer::Handle`/`BindingHandle`) per call site,
  how many found the recorder busy and the drain time. `KYTY_DIRECT_RECORD=1` records the converted
  sites directly on the command thread again; `KYTY_SUBMIT_THREAD=0` ends, submits and begins
  command buffers there again.
- Critical path: `KYTY_STALL_DIAG=1` prints a `STALL` line once per second with the GPU command
  thread's waits by cause (no guest work, blocked guest waits, prepare worker, recorder ring,
  GPU, other threads' synchronous commands and the GPU waits inside them, scheduler callbacks)
  and the prepare worker's and recorder's busy, spinning and sleeping shares.
- BDA uploads (Track B): `KYTY_BDA_STATS=1` prints a `BDASTATS` line once per second for
  `PrepareBda`'s uploads: KB/s by what queued them (invalidate, CPU write, new buffer, map),
  pages uploaded again within one frame, changed bytes and 64-byte lines per page for a 1-in-8
  sample against its previous staging, and the uploads a bind later covered (`consumed_kb_s`)
  or that the next upload of the page replaced with no bind in between (`superseded_kb_s`;
  `unknown_kb` counts unresolved uploads dropped when the tracking map reaches its cap).
  `=access` also makes DMA shaders mark the 16 KiB pages they read (atomics; slow) and reports
  uploaded bytes that no DMA shader read (`read_kb` / `unread_kb`; "unread" means the page was
  not marked in the access window of the upload or the next one); its shaders are kept apart
  in the shader store. Both modes halve FPS; the A/B figures in a run with them are not FPS.
- Dispatch chains (Track G3): `KYTY_DISPATCH_CHAIN=0` records every dispatch's barriers again,
  `KYTY_DISPATCH_CHAIN=stats` prints `DISPCHAIN` lines (dispatches and chained dispatches per second).
- Draw state caches (Track B): `KYTY_DTC=0` turns them off, `KYTY_DTC_VERIFY=1` recomputes every
  hit and exits on a disagreement, `KYTY_DTC_STATS=1` prints `DTC` hit-rate lines once per second.
- Uploads (Track G4): `KYTY_GPU_TIME=passes` prints `GPUCOPY` (buffer copies by size: ms/copies/MB
  and the idle gap before + after, per second); with `KYTY_VK_COUNT=sites` also `GPUCOPYSITE
  addr=… caller=…`. `KYTY_ASYNC_UPLOAD=0` uploads from host memory on the universal queue again,
  `=stats` prints `ASYNCUPLOAD` lines, `KYTY_ASYNC_UPLOAD_MIN=<bytes>` (default 262144) is the
  smallest upload sent through the transfer queue, `KYTY_ASYNC_UPLOAD_VERIFY=1` reads each one's
  destination back and compares (`ASYNCVERIFY` lines; slow).
- Barrier gaps (Track G4): `KYTY_GPU_TIME=passes` prints `GPUGAPKIND` (gaps after barriers by the
  kinds of segment before and after); with `KYTY_VK_COUNT=sites` also `GPUGAPSITE addr=… caller=…
  src=… dst=…` (the last barrier before the gaps; bit 63 of `src` marks a synchronization2 barrier).
  `KYTY_UPLOAD_BATCH=0` gives every `PrepareBda` upload its own barrier pair again.
