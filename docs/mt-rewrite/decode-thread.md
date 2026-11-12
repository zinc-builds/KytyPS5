# Decode thread, stage 1: design note (B4)

## Shape
- The GPU thread keeps the submission lifecycle (`GuestGpu::Process` → `CommandProcessor::Process`, resume scans #90/#103, `ExecutionScope`) and becomes the **commit thread**.
- `Process` hands the `Pm4Execution` to the **decode thread**, which runs `ProcessPm4`. The commit thread then runs a commit loop until decode reports the end of the loop (buffer stack empty or suspended). After that come the end-of-loop drain and the return as today.
- No decode-ahead across `Process` calls, queues or compute pipes. Only the graphics CP gets a decode thread; compute-queue packets are mostly dispatches, which are syncs anyway.
- `KYTY_DECODE_THREAD=0` runs today's single-thread loop unchanged: the same `ProcessPm4` body, called inline.

## Hand-off: what a posted item carries
- **Draws:** the existing `QueuedDraw` ring (depth 8 + 1, free lanes). The slot is filled by decode exactly as `PostQueuedDraw` does today:
  - register snapshot pointer (#104), user-config and shader-context copies, index or auto args;
  - stamps: hazard generation, unranged epoch, stored sequence, GPU-write sequence;
  - sequence number, then `Pool::Post`.
  - New: a frame-skip note for a skipped draw inside an open occlusion query. Today `SkipDraw` calls `OcclusionQueries::NoteSkippedDraw` at decode; that becomes a flag the commit thread applies in order.
- **Syncs:** one mailbox, since decode stops at a sync and at most one is outstanding. It holds the packet pointer and header, the remaining and total dwords, the cursor, the kind (packet, guest commands, dispatch-chain end), and `after_sequence` (the last draw posted before it).
- **Counters:** `m_queued_count` is split into `m_queued_posted` (decode writes, release) and `m_queued_committed` (commit writes, release). Ring full means decode waits for a commit: back-pressure replaces today's inline `FinishQueuedDraw` in post.

## Ownership
| Owner | State |
|---|---|
| Decode | `Pm4Execution` cursor and buffer stack, live `m_ctx`/`m_ucfg`/`m_sh_ctx` plus written-line flags (#104), index and NUM_INSTANCES state, predication flag (read), frame-skip draw classification, the producer side of the ring: slot fill, register snapshot pool (`RegisterSnapshotFor`, retiring against `m_queued_committed`), `Pool::Post` |
| Commit (GPU thread) | the scheduler's `CommandBuffer` and its bindings, everything `FinishQueuedDraw` does, atomic batch, dispatch chain, early submit (#108), async collect, guest commands, `GuestSyncEpoch::Advance`, every sync-packet handler |
| Baton (sync) | While decode is stopped at a sync, the commit thread may read and write all decode-owned state; handoffs are release/acquire. Frame skip's `OnFlip`, predication, `SET_BASE` targets and chain or IB state are only changed inside syncs |

- **Decode never touches `CurrentBuffer()`.** `QueueDrawIndex`/`QueueDrawAuto`/`NextQueuedDraw` take the live context directly. Today they read it through the `CommandBuffer`, whose pointers `FinishQueuedDraw` rebinds to snapshots on the commit thread.
- **Stamp reads at post:** these are already atomics read by workers. `HazardGeneration` moves from a relaxed to an acquire load. A commit-side change racing a later post only makes adoption more conservative (rejects), and GPU-dirty bytes are still checked at adoption.
- **Epoch semantics:** unchanged, since `Advance` stays in syncs.

## Sync protocol
- **Decode runs** a packet by itself if the packet is in `KeepsQueuedDraw` and none of the dynamic conditions below holds.
- **Otherwise** decode publishes the mailbox and blocks: a short spin, then a futex wait.
- **The commit loop** commits draws in order (`FinishQueuedDraw`, unchanged). When `m_queued_committed == after_sequence`, it runs the sync: today's per-packet prologue (drain no-op, async collect, dispatch-chain end, epoch advance, atomic flush, early submit), then the handler on the commit thread. It then releases decode, with a stop flag if the handler suspended (`WAIT_REG_MEM`, REWIND).
- **The per-packet body** of `ProcessPm4` is factored into one function both threads call, so packet handling exists once.

## Packet classes
- **Decode-local (run ahead):**
  - `SET_CONTEXT_REG`, `SET_SH_REG`, `SET_UCONFIG_REG(_INDEX)`, `SET_{CONTEXT,SH,UCONFIG}_REG_INDIRECT`, `SET_BASE`;
  - `INDEX_BASE`, `INDEX_BUFFER_SIZE`, `INDEX_TYPE`, `NUM_INSTANCES`, `INDIRECT_BUFFER`;
  - queueable `DRAW_INDEX_2`, `DRAW_INDEX_OFFSET_2`, `DRAW_INDEX_MULTI_INST`, `DRAW_INDEX_AUTO`;
  - NOP `R_ZERO`, push/pop marker (no-ops) and context-state;
  - predicated-skipped packets.
- **Dynamic syncs (decode-local packets that sync):**
  - guest commands pending at a packet boundary (pause handshake);
  - async collect requested at a packet outside `DefersAsyncCollect`;
  - a draw that is not queueable, which commits directly with the live registers;
  - a draw reading NUM_INSTANCES from memory;
  - a packet that ends a dispatch chain while one is open: after a dispatch, the first draw syncs so `EndDispatchChain` runs in order.
- **Full syncs (everything else):**
  - `EVENT_WRITE`, `ACQUIRE_MEM`, `PFP_SYNC_ME`, `RELEASE_MEM`, `WRITE_DATA`, `WAIT_REG_MEM(_64)`, `MEM_SEMAPHORE`, `REWIND`;
  - `DISPATCH_DIRECT`/`INDIRECT`, `DRAW_INDIRECT` family, `DMA_DATA`, `COND_EXEC`, `SET_PREDICATION`, `GET_LOD_STATS`;
  - other emulator NOPs (flip, custom draws), unknown packets.
- **Expected sync rate:** ~12.5 per 1,000 draws (73 per guest frame), from the gap statistics, plus the dynamic ones.

## Faults and guest commands (deadlock rules)
- **A decode-time read of a GPU-dirty page** (indirect registers, IB contents) faults on the decode thread. `HandleFault` → `BufferCache::ReadMemory` on a non-GPU thread → `ReadMemoryAsync` → `SendCommandSync`, a guest command for the GPU thread. This is the path guest CPU threads already use.
- **Rule 1:** the commit thread never blocks on decode without servicing guest commands. Its wait-for-decode loop polls `HasPendingCommands()` and runs `ProcessCommands()`. That is legal there because it has committed everything decode posted, which is equivalent to a packet boundary before decode's current packet: decode's in-flight packet has published nothing yet.
- **Rule 2:** decode checks `HasPendingCommands()` at every packet boundary and syncs if set, so the latency of guest readbacks and unmaps stays as today's (next packet) rather than the next natural sync (~80 draws).
- **Other waits:** decode waits only on commit (ring slot or sync release), and commit's other waits (workers, recorder, GPU fences) don't depend on decode. Workers can't fault (existing `EXIT_IF(OnWorker)`).

## Changes since the assessment, handled
- **#104 register snapshots:** decode owns the live context, the line flags and the snapshot pool. A snapshot is retired only when `last_sequence < m_queued_committed + 1`, read with acquire. `m_register_lines_changed` is post-local.
- **Free lanes, 8 ahead:** unchanged for workers. `Pool::Post` runs on decode and `Pool::Wait` on commit. `m_posted_local` becomes decode-only, and `WaitIdle` uses the atomic. Side effect: decode keeps the ring full, so workers run ~8 draws ahead most of the time (a larger exposure window; watch rejections).
- **#78 worker stream copies:** worker-side, untouched. `Retire` stays on the commit thread.
- **#108 early submit:** its trigger is in the sync prologue on the commit thread, unchanged.
- **#98 frame skip:** `OnDraw` classification runs on decode. `OnFlip` runs in a sync. `NoteSkippedDraw` becomes an ordered flag (above).
- **#90/#103 submit scans:** at `Process` start and on resume, on the commit thread, unchanged.

## Verify mode (`KYTY_DECODE_THREAD=verify`)
1. **Per draw:** decode stores a digest (snapshot bytes, user config, shaders, draw args) at post. Commit recomputes it before binding.
2. **Shadow decode on the commit thread:** a second, decode-only command processor state, seeded from the live context at each sync, replays the same packet range. Decode logs IB pointers and offsets, and the hashes of the indirect-read source bytes. Snapshots and draw args are compared per draw. A difference with changed source bytes is counted as a guest race, otherwise it exits.
3. **Sync order:** the decode log of (buffer, offset, opcode, `after_sequence`) is checked against execution on the commit thread.
4. **Ownership asserts:** thread checks on the decode-owned and commit-owned entry points.

## Placement, and the heat guard
- **An E-core is not free here.** The game's threads run on E-cores 12–18, about 45% each, and the recorder has 19.
  - Taking E-core 18 exclusively for the overflow worker made the guest late: +0.5–0.7 ms per frame of guest-late idle, FPS −2.8% to −6.4%.
  - A shared E-core preempted the worker.
- **Measured alternatives, both behind `KYTY_DECODE_CPU`:**
  - an E-core shared with the game, with decode sleeping quickly when blocked;
  - CPU 9, the SMT sibling of a prepare worker, with the emulator's other threads kept off it.
- **Not CPU 11:** the command thread's own sibling.
- **Reported:** guest-late idle and the command thread's effective clock (`cmd_cpd` ÷ `cmd_uns`), on and off.

## Expected size and costs
- **Gross:** decode + post was ~325 ns/draw in the post-#74 profile; I'll re-profile on today's dev first.
- **Overheads:** ~73 syncs per frame × ~1–2 µs handoff, plus guest-command pauses and a slower decode after each sync (an E-core decode is ~600 ns/draw, which lengthens the post-sync refill).
- **Net estimate:** ~1.3–1.6 ms per guest frame off the commit thread.
- **Plan:** one PR with the kill switch, verify mode, Sol review and gate.sh on PGO, frames 1000–1600.
