# KytyPS5 Thread_Gpu: can per-draw work be multithreaded? (branch `astro`, 182161eb+)

**Short answer:** splitting the work across threads is possible but expensive. Almost every expensive stage changes shared caches or records Vulkan commands into the one open command buffer. Some stages can also submit or wait in the middle of a draw. Threading only the Vulkan recording, which is what the one fork that tried it did, has a low ceiling (estimate ≤1.1×). The best payoff for the effort is (d): cut per-draw cost first (memoization plus PRs #1284 and #1289). After that, only a "prepare/commit" split is worth threading.

## 1. Per-draw pipeline (facts from code)

| Stage | Where | Pure? |
|---|---|---|
| PM4 decode, register writes | graphicsRun.cpp:543-630 | Pure (register context only). Cheap. |
| Service guest-CPU fault commands between every packet | graphicsRun.cpp:560 → ProcessCommands :87 | Changes caches (readbacks) |
| Drain deferred ops, take the context mutex | renderDraw.cpp:1271, :1275 | Scheduler state |
| Shader lookup + **SRT walk (`MaterializeResources`)** | pipelineCache.cpp:647 → GetProgram :357 → FindModule :282 | Reads guest memory only. **Writes into the shared `Program::resources`** (:395) and the member `m_lookup_key` (:386). Compiles shaders on a miss. |
| Texture resolve / FindImage / BindImage | descriptors.cpp:472, :696, :670-686 | Texture cache + per-image `binding` flags |
| FindBuffers / PrepareBda / ImageHeap::Rebind | descriptors.cpp:735, :866-901; renderContext.cpp:126 | Buffer cache, write tracker, mprotect. "Discovery can read back … and submit the scheduler" (:896). |
| Vertex/index/args buffers, stream uploads | renderDraw.cpp:1104-1114, :666 | Buffer cache. Records copies into the current command buffer (bufferCache.cpp:64-65). |
| Pipeline lookup | renderDraw.cpp:1116 | Pipeline cache |
| AcquireRenderTargets | renderDraw.cpp:1118, :446 | Texture cache |
| CommitBindings: layout transitions, push/update descriptors | descriptors.cpp:903-1088 | Image layout state. Barriers end rendering (image.cpp:225-247). |
| Push constants, dynamic state (already dirty-tracked), barriers, BeginRendering, occlusion query, draw | renderDraw.cpp:1140-1230; dynamicState.cpp:7; context.cpp:43; occlusionQueries.cpp:35 | Command-buffer state, query ring |

Guest threads also enter the caches through fault handling (renderContext.cpp:57). `TextureCache::InvalidateMemory` always takes the spinlock the GPU thread holds while it records (textureCache.cpp:1841, regionManager.h:30).

## 2. Ordering hazards (facts)

- **CPU reads of GPU-written memory:** a read fault calls `BufferCache::ReadMemory` → `SendCommandSync` to the GPU thread (bufferCache.cpp:293). That path waits on GPU ticks (:689-691). Any thread that runs ahead must first drain up to the current packet.
- **Predication:** `SetPredication` publishes the occlusion results and reads them on the CPU, or suspends PM4 (graphicsRun.cpp:671-711). This is a full sync point.
- **Indirect draws:** count, args and NUM_INSTANCES are read on the CPU at decode time (graphicsRun.cpp:655-668, :793-870). The data may have been written by an earlier dispatch.
- **Render passes are dynamic:** any Transit or shader-write barrier (renderDraw.cpp:1194/1228), a GDS barrier, a depth→color copy (:982) or an upload ends rendering mid-draw. Pass extents are known only while recording. This defeats "one secondary command buffer per pass".
- Unmap (renderContext.cpp:96) and eviction (bufferCache.cpp:675-700) call Finish/Wait. Mid-draw submits can happen (renderDraw.cpp:1133).

## 3. Candidate designs

The time split is an **estimate**, using GoW data as the prior. PR #1211 measured `MaterializeResources` at about 50% of Thread_Gpu (42/82.7 ms per frame). PR #1289 put texture resolve at about 16-20%. PR #1284 put mprotect, tracker locks and buffer sync at about 12-20%. That leaves CommitBindings plus Vulkan calls at about 10-15%. Sky Garden has not been profiled. Its fault rate (50-113k/s, about 3-5× GoW per frame) suggests a larger tracking share.

| Design | What stays serialized | Ceiling (est.) | Effort / risk |
|---|---|---|---|
| (a) PM4-decode front-end + recording back-end | All cache work | ≤1.1× (decode is cheap) | ~1.5k LOC. A drain at every sync point. Poor value. |
| (a′) Vulkan-recording thread (yuzu-style deferred commands). PR #937 commit 41ab564 ports IDXTRI's version (`CommandRecorder`, +430 LOC) | Everything except the vkCmd* calls | 1.05-1.15× on NVIDIA. No numbers published. | 1-2 weeks, moderate risk. Raw `Handle()` users must wait for the thread. |
| (a″) Prepare/commit split: thread A does PM4 + SRT walk + T#/V# decode into per-draw packets; thread B does cache lookups + recording | Caches, barriers, pass state. Drain on predication, readbacks, waits, indirect args. | 1.5-1.8× if the SRT walk is about 50% | 3-5 weeks, 2-4k LOC. Per-draw snapshot copies; pipeline-cache locking (async compile PR #2119 helps). High risk. |
| (b) Parallel secondary command buffers | Cache lookups. Pass splits are dynamic. Uploads go into the primary buffer. | ≈ the Vulkan share unless the caches become concurrent | Months. Not recommended. |
| (c) Speculative prep of draw N+k on workers | Needs (a″)'s lookahead state plus read-log validation | Similar to (a″) | Harder than (a″) |
| (d) Cheaper per-draw work, no threads | Nothing new | Additive. See §5. | Days per item. Deterministic. |

## 4. Prior art (facts)

- **PR #937** (itsmemac `wolverine`, open) adds a recording plus submit thread (41ab564be1), pipelines compiled on workers (b804a66cf6), and pinning of the GPU and recording threads to P-cores on hybrid CPUs (024668ee67). It publishes no speedup numbers.
- **PR #2119** (async shader compile, open). PR #1211 is partly upstream (inline Value accessors; eb3cb472 removed the per-draw overlap guard). **PRs #1289, #1284, #1307 (userfaultfd) and #2091 are not in `astro`.** I checked #1289 and #1284 in the source; #1307 and #2091 I inferred only because they are still open. #2091 lets the GPU start sooner but does not reduce CPU work; its author saw no change in Astro Bot pacing.
- Nothing upstream parallelizes draw recording or uses secondary command buffers.
- **shadPS4:** a single command-processor thread (`liverpool.h` `process_thread`) records directly. Its scheduler adds only a priority pending-ops thread, which Kyty's CommandScheduler mirrors (commandScheduler.h:84). It has no multi-threaded recording.

## 5. Recommendation

1. **Measure before designing (an hour, once the emulator is free).** Run `perf record -g -t <KytyGpuCmd tid>` for 10 s in Sky Garden and get the shares for MaterializeResources, FindImage, mprotect/spin and the NVIDIA driver. Check that the thread is busy, not waiting: #1081 found Thread_Gpu about 80% idle during a fault storm. Also try `taskset -pc <P-cores> <tid>`, since the 14600K is 6P+8E.
2. **Cherry-pick #1284** (lock-free fault fast path; atomic dirty bits; shared-mode ProtectTransient). Sky Garden's fault rate is its worst case. Then cherry-pick **#1289**. Both are ready, with tests and measured gains.
3. **One- to two-day prototype: memoize the SRT walk.** In `PipelineCache::Program::FindModule` (pipelineCache.cpp:282) / `GetProgram` (:357):
   - Key a small per-Program LRU on a hash of the user SGPRs.
   - Store the walk's read log with the values it read, plus the resulting `ResourceSnapshot` and `ResourceSpecialization` (ResourceSnapshot.h:33; reads already flow through `SrtReadCapture`, ResourceMaterialization.cpp:1593-1600).
   - On a hit, re-read and compare those words instead of running the IR walk. Point `input_info.stage.resources` (:395) at the memo entry.
   - Add a debug flag that runs both paths and asserts they are equal.

   Estimate: if the walk is about 40-50% of the thread and hit rates are like #1289's (92%), that is about 1.4-1.7×.
4. Go to threading (a″) only if Thread_Gpu is still saturated after that. Start by making `MaterializeResources` output per-draw (no writes to the shared `Program`). That is also the precondition for every threaded design.
