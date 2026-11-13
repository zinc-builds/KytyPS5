# Eboot reverse-engineering helpers (Track S)

Addresses below are for the Astro Bot dump (PPSA21567) eboot.

- `zx.cpp`: list/extract files from the `.zar` dump. Build against a build dir's zarchive and zstd:
  `clang++ -O2 -std=c++20 zx.cpp -I<build>/_deps/zarchive_source-src/include <build>/3rdparty/libzarchive.a <build>/_deps/zstd-build/lib/libzstd.a -o zx`;
  `zx PPSA21567.zar ls ""`, `zx PPSA21567.zar get eboot.bin out.bin`.
- `self2img.py eboot.bin eboot.img`: flat image, file offset == `eboot+0x...` in TIMEDIAG/perf.
  Disassemble with `objdump -D -b binary -m i386:x86-64 -M intel --start-address=A --stop-address=B eboot.img`.
- `imports.py eboot.img`: PLT index -> NID; PLT stub address = 0x74bc460 + (index - 1453) * 16.
- `callers.py eboot.img <target>...`: rel32 call/jmp sites; `refs.py` / `riprefs.py`: RIP-relative references.
- `gamespeed.py [bench logs...]`: game speed from the game's own `veg_chibi 6->7` event and from
  frames 1000-1600 (see the docstring); default all `/tmp/bench-*.log`.
- `patchstats.cpp` (target `patch_stats`; `tools/astro-bench/build.sh patch_stats`): the guest
  instruction patcher (`--amd-cpu`) run offline on an `eboot.img`, with why each VRSQRTPS site
  still traps: `patch_stats eboot.img 0x74bd57c 0x74c0000 0x269b04` (exec segment size,
  eh_frame_hdr offset and size for Astro Bot). In the emulator, `KYTY_TRAP_DIAG=1` prints which
  sites and threads actually trap.
