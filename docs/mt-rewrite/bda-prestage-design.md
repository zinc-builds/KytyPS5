# BDA pre-staging: joint design (Track B + Track K)

**Status (2026-11-12): approved, built, parked.** The tracker side (#116) is in dev; the stager is on the unmerged branch `trackB/bda-prestage-pr`. It passed the correctness gate but cost more than it saved: see FINDINGS, "BDA pre-staging". Departures from this text in what was built:
- #116 bumps versions where a write mark is consumed, so there are no pre-arm tags.
- Staged runs were copied from the arena on the universal queue. `CopyFromHost` was not built.
- The stager's CPU is a switch (`KYTY_BDA_PRESTAGE_CPU`, `KYTY_BDA_PRESTAGE_OWN_CORE`). By default it shares E-core 18 with the game.
- Verify classifies each difference as marked or unmarked and checks the loop. The gate was zero unmarked differences.

## Goal
PrepareBda's guest→staging copy (~5% of the command thread on PGO, 7.9% plain, ~1 GB/s, 99.1% async-tracked pages) moves to an E-core stager. PrepareBda records GPU copies of staged pages instead of copying them.

## Invariant (one sentence)
A staged copy of a page is used only if the page was armed (async-tracked, not aliased) before the copy began, its version was even and unchanged across the copy and is still the same at use, and the guest remap generation is unchanged since the arm. Every report of a later guest store, every explicit invalidation, and every emulator store moves the version; every remap, alias or zeroing moves the generation.

## Page versions (Track B, `src/graphics/host_gpu/pageVersions.h`, on `trackB/bda-prestage`)
- One `std::atomic<uint32_t>` per 4 KiB page, in two-level tables (64 MiB per table, created on first use). Any thread may read or bump.
- `PageVersions::Bump(vaddr, size)`: +2. Called by:
  - `MemoryTracker::InvalidateRegion` and `MarkRegionAsCpuModified` (guest writes from faults and scans, and invalidating emulator stores);
  - `RenderContext::NoteGpuWrite` (raw-pointer CP writes: reference clock, occlusion results, CP fills and copies);
  - **new, Track K:** collection reports for staged pages (below). Remaps, aliases, unmaps and zeroing are covered by the remap generation instead (tracker side, (c)).
- `PageVersions::BeginStore / EndStore(vaddr, size)`: +1 / +1 around `TryWriteBacking`, `TryStoreLabelBacking` and `TryFillBacking` (labels, readback and download publishes, fills). Odd during the store.
- `PageVersions::Read(page)`: acquire load.

## Tracker side (Track K, PR 1)
- **(a) Arm: `ArmStaged(vaddr, size) -> optional<uint64_t>`.** It returns the remap generation the arm is valid for, or nothing.
  - **Refused (returns nothing; the stager skips the range):**
    - a page outside asynchronous tracking (fault-tracked, dry run, not write-protected);
    - an **aliased** page: another guest view of the same bytes. A store through the other view never marks this view's page table, so a copy could never be invalidated. This is the same rule `ProtectAsyncPage` applies to labels.
    - a page overlapping a private mapping.
  - **Under shared `m_mutex` + `m_kept_mutex`, in this order:**
    1. Add the pages to the staged set: a page bitmap, plus a count per 2 MiB chunk for the `wanted` filter.
    2. Advance their label tokens **per page**. No floor bump for long runs: staged ranges are long, and a floor bump would void every label token on each arm, undoing #106.
    3. Re-protect the range with `PAGEMAP_SCAN`, clearing the written marks, and keep what it finds.
  - **Found writes predate the copy.** Kept entries from an arm are tagged *pre-arm*.
    - A collection still invalidates write-watched pages for them; other caches may watch these pages.
    - It does **not** bump the staged version for them.
    - Without the tag, nearly every staged page would be discarded: a page is queued for upload because it was written, so the arm almost always finds that write.
  - **The stager reads each page's version after `ArmStaged` returns,** then copies. A collection that took a mark between step 1 and that read bumps the version first, so the copy is taken at the new version (correct), or the version moves during the copy and it is discarded (conservative).
- **(b) Report.** Chunks with staged pages count as `wanted` in every scan: GPU-thread collections and submit scans.
  - In `CollectAsyncWrites`, every written range that is not pre-arm and intersects the staged set gets `PageVersions::Bump` for its staged pages, in addition to today's watched-page invalidation.
  - This covers full collections, kept-only collections (submit-scan finds via #90/#101, skipped resumes via #103), and the writes `ProtectAsyncPage` / `KeepAsyncWritesUnlocked` / `ProtectWritesUnlocked` keep.
  - It does not unstage. The stager drops the entry when the version mismatches at use.
- **(c) Transitions: the remap generation.** `g_remap_generation` already advances on every change that alters bytes or views without a store through the view:
  - `ZeroRange` (backing zeroed on reuse);
  - `MapFixed` and the view creation after it (new mappings and new aliases);
  - `UnmapOne`.
  - A staged entry records the generation `ArmStaged` returned, and is used only if `GuestRemapGeneration()` still equals it. Labels already use this rule (`IsCpLabel`).
  - No per-page hooks are needed. Chunks never leave asynchronous tracking: there is no demotion path, and promotion only applies to fault-tracked chunks.
  - Remaps are rare in the steady state, so this discards little. To be confirmed by a counter: generation moves per second.
- **(d) Verifier.**
  - `KYTY_ASYNC_WP_VERIFY` stays as it is. The kept-write rule applies to pre-arm entries as to any other: every kept write a collection takes must be handled.
  - The staged copy itself is checked by PR 2's `KYTY_BDA_PRESTAGE=verify`, which compares arena bytes with guest memory at use. That catches anything the tracker misses, including writes no mark can see.
  - PR 1 adds per-second counters: arms, refused (async, aliased, private), pre-arm writes kept, staged-version bumps from reports.
- **`Unstage(vaddr, size)`:** the stager calls it when an entry is used, discarded or evicted. It removes the pages from the staged set. The page stays write-protected; it is an async chunk.
- **Kill switch:** nothing is staged without PR 2. In PR 1 the hook costs only a test of the per-chunk count on written ranges.
- **Cost to watch:** chunks with staged pages are scanned even without watchers. The ~80% of hot chunks that have watchers are already scanned.

## Stager side (Track B, PR 2)
- **Input.** Ranges from `QueueBdaSync` that lie in BDA-mapped ranges, i.e. pages PrepareBda will take. Other pages are left alone, so binds still copy at use.
- **Per range, in order:** `ArmStaged` (skip if refused; keep the returned generation) → read each page's version (skip odd pages) → copy the page into the arena → fence → read the version again → publish the entry `{page → arena slot, version}` if unchanged; otherwise discard and `Unstage`.
- **Use (PrepareBda / UploadCopies, the Bda kind).** For each CPU-dirty page uploaded, if there is an entry, `entry.version == Read(page)` (even) and `entry.generation == GuestRemapGeneration()`, the GPU copies it from the arena. Otherwise the page is copied now, as today. Either way the entry is removed and the page unstaged.
- **GPU copies.** The arena is a host-visible Upload buffer.
  - With the async uploader, staged runs go through the transfer queue into its device ring. A new `AsyncUploader::CopyFromHost(src, regions)` call means the universal queue copies ring → destination as today.
  - Without it, the universal queue copies from the arena directly.
  - Arena slots are freed once the transfer value or scheduler tick that read them is done.
- **Arena bound.** Default 64 MiB (`KYTY_BDA_PRESTAGE_ARENA`).
  - When full, the stager evicts the oldest entries (unstaging them, so those pages are copied at use) and never blocks.
  - If the oldest entry is pinned by an in-flight GPU copy, it skips that range for now.
- **No epoch limit** by default. `KYTY_BDA_PRESTAGE_MAX_EPOCHS=1` brings back the one-collection limit as a fallback.
- **Thread.** One stager (`KYTY_BDA_PRESTAGE_THREADS`), pinned to an E-core other than the recorder's: CPU 18 here, removed from the guest-thread set. Never a worker, command or recorder core or their siblings. Its busy share is reported.
- **Switches.** `KYTY_BDA_PRESTAGE=0|1|verify`.
  - verify: for every staged page used (or a sample, `KYTY_BDA_PRESTAGE_VERIFY_EVERY=N`), however old, compare the arena bytes with guest memory at use. Count differing pages per second and as a share of staged uploads; it must read 0.
  - Strict: exit if an entry is used whose version is not the current even version.
- **Fault injection.** `KYTY_BDA_PRESTAGE_DROP_ARM=N` skips the arm on every Nth staged range. The verify probe must catch the stale copy even when it is used seconds later; there is a unit test for it, plus an in-game run.
- **Unit test.** Stage a page, store into it through `TryWriteBacking`, and check that the entry is not used. The same for `NoteGpuWrite` and for a collection report.

## What it leaves and what it costs
- **Eligible bytes:** 99.1% async-tracked. Lead time from queueing to use: ~2.5% under 100 µs, ~20–29% 100–250 µs, the rest longer. Expected served: ~75–95% of PrepareBda's bytes, depending on stager throughput.
- **Costs to measure:**
  - one `PAGEMAP_SCAN` per staged range (on the stager);
  - more collection scan hits on staged chunks (submit thread and command thread);
  - one in-kernel write fault per re-armed page the guest rewrites (~0.5% of pages);
  - ~1 GB/s of copying on an E-core, which competes for bandwidth with everything else.
- **Gate:** `cmd_ns` and cycles, same binary through `KYTY_BDA_PRESTAGE=0`, 3+ runs a side, on the plain and the PGO build, plus gate.sh and Sol (on the version rule and the interleavings). Track K reviews PR 2; Track B reviews PR 1.

## Approved (lead, 2026-11-12), with these to report alongside the PRs
1. The game's own pace: `KYTY_FRAME_DIAG` on vs off (the "guest late" idle and the share of frames spanning two refreshes), next to `cmd_ns`. E-core 18 leaves the guest-thread set, and armed pages cost the guest an in-kernel write fault when rewritten.
2. GPU side: GPU ms per frame (`KYTY_GPU_TIME`), and the command thread's GPU wait (`KYTY_GPU_WAIT_DIAG`, once #107 is in), on vs off.
3. Served share of PrepareBda's bytes, and fallbacks by reason: not staged yet, version moved, evicted, refused (aliased or fault-tracked), generation moved.
4. Wasted stager work: bytes staged and discarded without use, per second.
5. Remap-generation moves per second.

Rules:
- **The verify probe is the gate.** It must read zero on staged pages over the full window on both builds, and the injected fault must trip it at any age, before any performance number counts.
- **If PR 1 changes anything the kept-write rule or `KYTY_ASYNC_WP_VERIFY` relies on,** Track K says so in the PR and re-runs `KYTY_ASYNC_WP_VERIFY_DROP_KEPT`.
- Merge each PR on a `!` in `cmd_ns` or cycles through `KYTY_BDA_PRESTAGE=0`, once the above holds.
