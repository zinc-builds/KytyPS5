# Thin serial commit: roadmap and resident-resource design (2026-11-11)

Source: two read-only studies by a research agent on `dev` 85eb0cb6. Nothing here was built or
measured; every ns figure is an estimate from the commit budget on the plain build (expect 14–26%
less on the PGO+ThinLTO build). `file:line` references were verified in the code at that commit.

## Goal

Stop depending on one fast pinned core. Shrink the in-order work per draw until the command thread
mostly validates and hands off; everything else runs on N prepare workers or stays resident on the
GPU. Estimated serial path per draw: ~4.0 µs today → ~0.7–1.1 µs (about 1.3–1.9 µs with dispatch
and kernel work). FPS then follows worker count up to about 3–4 workers; past that the command
thread and the recorder are the serial limits.

## Ruled out: the shader reads guest descriptors itself

- Pipeline choice depends on descriptor contents. Modules are specialized on T#/S#/V# bits
  (dimension, numeric class, conversion format, swizzle, sRGB decode, cube, FMASK, mip count,
  buffer stride and format: `ResourceMaterialization.h:10-52`, `ResourceMaterialization.cpp:917-1010`),
  so the CPU must read every descriptor before it can pick the shader. The SRT walk stays.
- The BDA page table is not exact on first touch: only pages of registered buffers are mapped
  (`bufferCache.cpp:383-407`); an unmapped page reads 0 and sets a fault bit handled at the next GC
  point (`spirvEmitterMemory.cpp:202-223`, `faultManager.cpp:76-152`).
- Freshness would need a `PrepareBda` sync on every draw (`renderContext.cpp:497-510`).
- No CPU saving under exactness: a known T# already costs one memo lookup on the worker.

## Resident resources (the feasible variant)

The tree already has the parts: a 65,536-entry update-after-bind sampled-image array in set 1
(`imageHeap.cpp:76-118`, `ShaderIR.h:346-350`), the "image left its read state" hook
(`image.h:135-139`, `image.cpp:239-246`), and tick-based element retirement (`imageHeap.cpp:388-413`).

- **Stage 1, resident textures and samplers.** Every sampled image slot reads `heap[index]`, with
  the index in shader data. Samplers go in a second array (the sampler cache never evicts,
  `textureCache.h:63-65`). The worker still walks and proposes texture records; the command thread
  stores an element index per record. A reused stage set then costs a few epoch compares instead of
  per-image `BindImage`, `AcquireTexture`, `Transit` and descriptor writes
  (`descriptors.cpp:1369-1382`, `1601-1605`, `1717-1756`, `1816-1826`).
- **Stage 2, buffers by address.** Buffers become a device address plus size in shader data;
  shaders already bounds-check from shader data (`ShaderIR.h:506-515`). The shader-data block is
  passed by address, so a draw is push constants plus the draw call.
- **Ordering.** The GPU never reads guest descriptor memory. Descriptors are read on the CPU at the
  PM4 position, as today. The one new rule: an element is immutable until its tick is free, and a
  changed view gets a new element.

### Per-draw needs become event-driven (stage 1)

| Need | Today | Answer |
|---|---|---|
| CPU-written refresh | `settled` check per image (`textureCache.cpp:1598-1610`) | Global unsettle generation; refresh stays lazy for the images of the set in use |
| Targets sampled as textures | `Transit` per image; feedback detection (`renderDraw.cpp:473`, `508-533`) | Layout/target epoch; feedback draws use a second element carrying the attachment layout |
| LRU and eviction | touch per use | Touch at set resolve plus #74's GC-tick invalidation (`descriptors.cpp:1241`); elements need an ageing policy, since `heap_refs` pins images (`textureCache.cpp:2219`) |
| Colour clears | per draw for flagged proposals (`descriptors.cpp:1375-1379`) | Stays per draw for DCC/CMASK images only |
| Storage-image writes | GENERAL layout, download tracking | Stay on classic bindings |
| Bound marking | `BindImage`/`ResetBindings` (`descriptors.cpp:1187-1211`) | Only for sets holding a target-capable or storage image |
| First-seen texture | worker only finds | Command thread creates it in order, allocates an element and writes the descriptor |

### Exactness risks

- Module variant count is unchanged (same specialization). Dynamic-mip slots take one element per mip.
- Every module recompiles once; the codegen toggle needs its own install directory or a key bit.
- Sampler count must stay under `maxSamplerAllocationCount` (4,000 minimum, from memory); the
  distinct count in Sky Garden is unmeasured.
- Sampling an image in attachment layout through a descriptor that claims the read layout is
  invalid. The heap path already does this; stage 1 should not.

### Portability

Needs Vulkan 1.2 descriptor indexing (update-after-bind, partially bound,
update-unused-while-pending) and `bufferDeviceAddress`, all already required
(`vulkanWindow.cpp:60-67`). Indices are uniform per draw, so no non-uniform indexing. Machine
assumptions in today's code: pinning only engages on Intel hybrid CPUs under Linux sysfs
(`hybridAffinity.h:86-90`); the heap exits below 65,536 update-after-bind images
(`imageHeap.cpp:83-86`); an NVIDIA `OpArrayLength` workaround (`ShaderIR.h:506-507`); an AMD-only
float min/max path (`pipelineCache.cpp:266`); mesh shaders.

## The serial floor

| In-order work per draw | Today (ns) | Floor (ns) | Why it stays |
|---|---|---|---|
| PM4 decode, registers | ~175 | ~175 | sequential state; decode-time guest reads |
| Post (register snapshot) | ~90 | ~60 | hand-off (`renderDraw.cpp:1832-1848`) |
| Wait for the worker | 260 | 0–80 | latency, not work |
| Adoption checks | part of "own" | 80–150 | epochs, read log, per-range checks (`renderDraw.cpp:2166-2182`) |
| Targets and pass state | ~580 | 60–120 | pass state is serial; run-level when unchanged |
| Bindings (431 + 414 + 365) | 1,210 | 40–80 | epoch compares and push-data dedupe |
| `ObtainBuffer` | 181 | 0–30 | #78 leaves range validation |
| Pipeline handle | ~95 | 20–40 | map is command-thread only; one lookup per run |
| Dedupe, record, publish | ~210 | 100–180 | "last recorded" state is sequential |
| Scheduler pop, context mutex, labels | ~170 | 100–150 | per draw today (`renderDraw.cpp:1509`, `1513`) |
| In-order exceptions, amortized | – | 80–200 | texture creation, barriers, rejections, slow-path draws |

On top of the draw path: `PrepareBda` 320 ns, compute about 100 ns, kernel about 450 ns.
Worker cost rises to about 4.5–5 µs/draw, so 60 FPS in Sky Garden needs about 1.5 worker cores.
The recorder is the second serial wall; its ns/draw is unmeasured.

## What blocks worker-built draw packets today

| Blocker | Evidence | Removed by |
|---|---|---|
| Descriptor writes built from image layouts and buffer infos | `descriptors.cpp:1755`, `1758-1826` | stages 1+2 |
| Per-image mutation (`BindImage`, acquire, `Transit`) | `descriptors.cpp:1187-1211`, `1717-1756` | stage 1 (epochs) |
| Uploads allocate from the command thread's stream ring | `descriptors.cpp:1179-1185`, `1551-1582` | #78's per-worker rings, extended to shader data and flattened SRT |
| Recorder ring is single-producer, records are closures | `deferredRecorder.h:20-25` | own change: a POD packet |
| Bind and dynamic-state dedupe is command-thread state | `dynamicState.cpp:86-120` | keep the dedupe on the command thread, fed from the packet |
| Barriers and `EndRendering` record directly, draining the recorder | `context.cpp:118-124`, `image.cpp:283-295` | queue them as ordered items |
| Occlusion queries use `Handle()` per draw | `renderDraw.cpp:1403-1405`, `1446-1448` | same as above |
| Target resolve, meta-clear consumption, feedback detection | `renderDraw.cpp:454-579`, `898-973` | run-level targets; stays in order |
| Atomic-write batch conflict checks | `renderDraw.cpp:1304-1399` | stays in order; workers can pre-sort ranges |
| Pipeline map lookup | `pipelineCache.h:392` | one lookup per run |
| 8-bit index expansion on the command thread | `renderDraw.cpp:1574-1584` | move to the worker |
| Workers only find; a rejection idles every worker | `drawPrepare.h:30-32`, `renderDraw.cpp:2184-2190` | re-post voided draws |
| Strict lane alternation, at most 4 workers | `render.h:482-485`, `ShaderIR.h:736` | free-lane assignment |

## Roadmap

| Step | Gate | Effort, risk | Worth it alone? |
|---|---|---|---|
| 0. Stats: slow-path share, recorder ns/draw, stage 0 (time in the code stage 1 deletes) | numbers in FINDINGS | 1–2 days, none | yes |
| 1. Latency: walker read logging, ranged hazards, depth sweep, free lanes, third worker | wait ≤ 80 ns/draw at depth ≥ 8; rejections < 300/s | 1–1.5 weeks, low-medium | yes |
| 2. Run-level targets, programs and pipeline | −270 and −150 ns | in progress | yes |
| 3. Stage 1, resident textures and samplers | −150 ns on PGO; verify clean | 2–3 weeks, low-medium | yes |
| 4. #78 revived, covering all per-draw uploads | `ObtainBuffer` and uploads −150 ns | ~1 week, medium | only after step 1 |
| 5. Stage 2, buffers by address, packet filled by the worker | no `pushDescriptorSet` on queued draws; `CommitBindings` ≤ 100 ns | 2–3 weeks, medium | yes |
| 6. Run-level bindings | trivial after 5 | days | yes |
| 7. No direct recording on the draw path | draw-path recorder drains at 0 | ~1 week, medium | no; prepares step 8 |
| 8. Parallel recording with secondary command buffers | only if the recorder gates | 2–4 weeks, high | – |
| 9. Decode thread | only if decode gates | ~1 week+, high | – |

Rule learned from #77, #78 and #82: worker time is not free. Under the latency co-limit every
later step measures flat, so step 1 comes first.

## In-place guest memory (C1)

Buffer data stays copied, by workers. In-place memory stays parked, with one gating experiment.

- Decode-time labels with in-place reads is unsound: the guest rewrites memory after a label
  (2.3% of pages, 9% of late copies).
- A correct design without round trips exists: store forwarding. Keep a table of pending stores and
  a page flag; every emulator-side read at decode consults it (binds, stream and worker copies,
  staged uploads, indirect register loads, waits, walker loads through the watched-page hook,
  `SrtWalker.h:106-118`). Pages holding a pending store are excluded from in-place reads.
- Under thin commit the prize shrinks to `PrepareBda`'s ~320 ns plus worker copy time, and a missed
  read path uploads stale bytes silently.
- Gate: on `trackB/late-labels`, replace bind waits with forwarding for binds only, under the
  `read_unlanded` verify. Revisit C1 only if late labels then cost ≤ 1%.

## First task: stage 1a in shadow mode (about a week, no shader change)

`KYTY_RESIDENT_TEXTURES=shadow`.

- Build: an element allocator on the existing set 1, shared with `ImageHeap`. Each resolved
  `WorkerTextureRecord` gets an element index (`workerTextures.h:24-42`) and each sampler a stable
  index. Add the unsettle generation and the layout/target epoch.
- Verify, per draw at commit: each element's view, sampler and layout equal the classic binding's;
  and whenever the epochs say "skip", the classic per-image path still runs and must do nothing
  (no refresh, no barrier, same view).
- Output: a mismatch exits with the first difference. The skip rate it prints is the measured size
  of stage 1's saving.
