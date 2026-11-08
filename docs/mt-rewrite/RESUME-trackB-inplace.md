# C1 stage 2 (BDA in place): resume note, paused 2026-11-08

## State

- **Prototype:** commit 824868ce, flag `KYTY_BDA_INPLACE=1|verify`. It needs `KYTY_LATE_LABELS` and `VK_EXT_external_memory_host`. It is based on an old late-labels head (176f8948), not on #70's head.
- **Mechanism:**
  - The guest backing memfd is imported in 2 MiB chunks.
  - BDA page-table entries for CPU-dirty, whole, non-GPU-modified 16 KiB pages are redirected to the imported memory instead of being uploaded.
  - Entries are restored before GPU writes, on register/unregister and remap, and before address-write dispatches.

## Last numbers (late labels on in both runs)

| Metric | Before | After |
|---|---|---|
| PrepareBda uploads | 830 MB/s | 0 |
| Stream copies | 300 MB/s | 433 MB/s |
| Bind syncs | 0.7 MB/s | 58 MB/s |

- Total bytes copied fell 57%, GPU time fell ~0.7 ms/frame, and FPS was flat.
- Verify (page-granular) found 2.3% of redirected pages changed between record and completion. This count is not yet narrowed to pages a DMA shader read.
- Pinned memory: ~1.1 GB in 545 chunks.

## Next steps, in order

1. **Narrowed verify, the correctness gate.** Wait for #66 to merge. Rebase this branch onto dev with #66 and #70. Combine the verify hashes with `KYTY_BDA_STATS=access` marks so that only changed pages a DMA shader read in the window count. If any such page exists, stop and report to the lead.
2. **Hybrid.**
   - Keep a per-16 KiB "bound since last PrepareBda" bitmap, set in ObtainBuffer and ObtainBufferForImage (tell B4 first: phase B touches ObtainBuffer).
   - In SynchronizeBuffersInRange's in-place branch, eagerly upload pages whose bit is set (restoring them if redirected), then clear the bit. Redirect the rest.
   - The aim is to win back the stream copies and bind syncs that redirected pages caused.
3. **Gate (lead, updated).** Compare hybrid on vs off, with late labels on in both, in one A/B pair. Need ≥ +1%; otherwise park C1, keeping both switches off by default and recording the numbers in FINDINGS. Keep `KYTY_BDA_INPLACE=verify`, and keep the restore counts and the pinned-memory figure in the stats.
4. Merge dev first: #76 fixed refbuild, which now uses a cached FFmpeg package.
