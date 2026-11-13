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

## Playing with the fast build

One supported way to get the configuration every number in `FINDINGS.md` is measured with:

```bash
tools/fast.sh build                          # PGO + ThinLTO kyty_emulator and launcher -> _Build/linux-fast/install
tools/fast.sh run ~/Games/PS5/PPSA21567.zar  # launch with the measured settings; extra flags go last
tools/fast.sh run ~/Games/PS5/PPSA21567.zar --fullscreen --frame-skip auto
```

- **Build:** ThinLTO plus PGO with `~/.cache/kyty-pgo/current.profdata` (or `KYTY_PGO_PROFILE`),
  ~+11% FPS in Sky Garden over a plain build ("Build modes" in `PLAN-60fps.md`). Without a
  profile it builds ThinLTO only (~+4.7%) and says so; `tools/astro-bench/pgo-train.sh` trains one
  (~5 minutes; it runs the Sky Garden bench, so not while playing). A profile from an older commit
  still works: code changed since then is compiled without counts.
- **Settings (`tools/fast.sh flags`):** `--amd-cpu --shader-cache --present-mode Mailbox
  --vblank-frequency 60`. The launcher's `_Build/linux-fast/install/kyty_launcher` works too; tick
  "AMD CPU" in the game's configuration to match.
- **`--amd-cpu`.** The game is built for the PS5's AMD CPU and uses AMD-only instructions: SSE4a
  `EXTRQ`/`INSERTQ` in hot loops, and sometimes `RDPID` and `CLWB`. A host without them (any
  Intel CPU; Rosetta) traps on each one and emulates it in a signal handler, ~1.7M times a second
  in Sky Garden, which drops it to ~10 FPS. The emulator can rewrite those instructions to native
  code when it loads the game (~7 s more boot for Astro Bot's 312k functions). Three modes:
  - **auto (the default since #PR):** rewrites what CPUID says the host lacks and, on a host that
    is not AMD, every `VRSQRTPS` as `--amd-cpu` does; so on Intel auto is `--amd-cpu`. An AMD host
    that has SSE4a, RDPID and CLWB is not patched at all and pays no boot time. (Upstream's #1439
    leaves `VRSQRTPS` opt-in; here it is not, because the host's own `VRSQRTPS` crashes Sky Aztec
    on Intel: `KYTY_AMD_SKIP_RSQRT` below.)
  - **`--amd-cpu`:** always rewrites every `VRSQRTPS` to a 1/√x computed in double precision and
    rounded to float (within about half a float ulp, where the hardware instruction only promises
    1.5·2⁻¹² and AMD and Intel each return their own approximation). It is not AMD's table, but it
    is what every number of record used. Unrelocatable sites trap (~266k/s in Sky Garden, about 3%
    FPS). Unlike auto, it aborts on an SSE4a/RDPID/CLWB site it cannot patch safely (Windows red zone);
    such a VRSQRTPS site is left native in both.
  - **`--no-amd-cpu`:** rewrites nothing.
  - **`KYTY_AMD_SKIP_RSQRT=1`** (experiments only) leaves `VRSQRTPS` at the host's precision: no
    traps, about 3% faster in Sky Garden, but on Intel Sky Aztec crashes near level frame 400 (a
    null read in game code, 2 of 2 runs).
  `KYTY_AMD_CPU=auto|1|0` sets the mode when neither flag is given; any other value is an error.
  The log's `AMD CPU compatibility: mode …` line says what was chosen and why, and each module's
  line gives found/native/trapped/skipped counts per instruction and its load time.
- **The profile comes from Sky Garden's first 1,600 frames only.** Other levels exercise code it
  saw rarely, so they may gain less (FINDINGS, "Build modes", for the levels measured).
- `tools/astro-bench/build.sh` builds the same thing for `kyty_emulator` alone in a bench checkout
  (`KYTY_BUILD_MODE=pgo`).

### What the emulator actually ran with

The log starts with a `CONFIG` block, and after the game's 120th flip repeats it with every
feature's state as `CONFIG summary begin` … `end` (the bench saves that block with each run as
`/tmp/garden-<label>.config` and in the run's JSON):

- `CONFIG build` — the build mode and PGO profile hash (`plain`, `lto=thin pgo=use profile=…`).
- `CONFIG switch KYTY_X=v (kind, default d, now on|off)` — every `KYTY_*` variable set, with its
  kind and default from `SWITCHES.md`; `CONFIG warning:` for a name this build does not read (a
  typo), a verify mode whose feature is turned off, pins ignored under `KYTY_NO_PIN`, and
  `KYTY_STALL_DIAG` during a bench run.
- `CONFIG feature …` — what was chosen and what fell back, and why: `amd-cpu` (mode, the CPUID bits
  missing, per-module found/native/trapped/skipped), `write tracking` (asynchronous userfaultfd or
  the mprotect fallback), `core placement` and `core <thread>` (the CPUs of the GPU command thread,
  each prepare worker, the recorder, the decode thread and each game-thread group, or "FAILED" when
  pinning was refused), `transfer uploads`, `recorder thread`, `submit thread`, `decode thread`,
  `resident textures`, `frame skip`.

`SWITCHES.md` lists every `KYTY_*` switch: kind, default, values, where it is read. It is generated
by `tools/switches.py` (`--check` fails when it is stale). On/off switches read through
`Common::EnvFlag` treat `0`, `false`, `off` and `no` as off.

### Machines this is tuned for, and what others would need (not built)

Everything here is measured on one machine: an Intel i5-14600K (a hybrid CPU) and an RTX 4080
Super, Linux. What depends on that, and what the `CONFIG` block shows on another machine:

- **Core placement** needs an Intel hybrid CPU under Linux (`/sys/devices/cpu_core` and
  `cpu_atom`). Elsewhere it is off and every thread runs where the scheduler puts it; the reserved
  cores, the prepare workers' own cores and the game threads on efficiency cores were each worth
  several percent here. A plan for a non-hybrid CPU (reserve the last cores by count) is not built.
- **Write tracking** wants userfaultfd with shared-memory write protection, and
  `UFFD_FEATURE_WP_ASYNC` plus `PAGEMAP_SCAN` (Linux 6.7+) for the asynchronous mode; without them
  it falls back (mprotect: every guest write fault is a signal).
- **`--amd-cpu`** matters on hosts without SSE4a (every Intel CPU); auto handles it.
- **GPU:** other GPUs and weaker cards are out of scope for now (the user's decision). The shader
  code generation was tuned against NVIDIA's compiler (EXEC selects, indexed-register switches),
  the copy-engine upload path needs a transfer-only queue family, and resident textures need
  update-after-bind descriptor limits (`CONFIG feature resident textures` says which is missing).
  An upstream report of driver timeouts in Astro Bot on an AMD RX 6700 XT has not been looked at.
