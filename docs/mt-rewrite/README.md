# KytyPS5 multithreaded draw path (fork project)

Goal: raise draw throughput of the GPU command thread so draw-heavy scenes stop being bound by a
single host thread. Reference workload: Astro Bot's Sky Garden, ~5,800 draws/frame, currently
~16 FPS on an i5-14600K + RTX 4080 Super with every local fix applied (see `FINDINGS.md`).

This is a private line of work in the fork `zinc-builds/KytyPS5`. Upstream `KytyPS5/KytyPS5`
is never pushed to and never receives PRs from here.

## Branches

| Branch | Content |
|---|---|
| `main` | mirror of upstream main (fetch `upstream`, fast-forward only) |
| `astro` | upstream 182161eb + mesh-instance slicing (Sky Garden exits on NVIDIA without it, #624) + benchmarking aids |
| `astro-perf` | + PRs #1284 and #1289 ported, + vertex-table / SRT-walker fast reads |
| `astro-uffd` | + PR #1307 (userfaultfd, both commits), hybrid-CPU pinning, `--amd-cpu` VRSQRTPS knob |
| `dev` | integration branch for this project (default branch); feature work branches off it and lands through PRs |

## Documents

- `PLAN-60fps.md` — the plan to reach 60 FPS: per-frame budget, three parallel tracks (GPU
  execution layer, draw state cache, guest memory without copies), owners, milestones, and the
  shared rules for several agents on one machine. Read it first.
- `FINDINGS.md` — every measurement so far: what helped, what was ruled out, current profile.
- `RESEARCH-threaded-draw.md` — design study of the per-draw pipeline: stage purity, ordering
  hazards, candidate designs (pipelined split, prepare/commit split, secondary command buffers,
  memoized SRT walk) with effort and expected-gain estimates.
- `mesh-instance-slices-v2.diff` — the original issue #745 patch the slicing fix was ported from.

## Working rules

- Several agents share this machine: follow the bench-lock rules in `PLAN-60fps.md`
  (`tools/astro-bench/benchlock.sh`).
- Measure with `tools/astro-bench` (two runs per configuration, ±0.3 FPS noise) and Tracy; record
  each result in `FINDINGS.md`.
- Correctness first: Sky Garden must render the same (compare screenshots) and not crash; run the
  touched unit tests one suite at a time under `systemd-run --user --scope -p MemoryMax=10G`
  (parallel shader-recompiler suites OOM a 32 GB machine).
- Build: `cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
  -DCMAKE_CXX_COMPILER=clang++`, then `cmake --build _Build/linux --target launcher` and
  `cmake --install _Build/linux --prefix _Build/linux/install`.
- The user plays from a separate checkout (`~/AI/emu/KytyPS5`, launcher installed from there);
  never install into or rebuild that one, and never run benchmarks while they are playing.
