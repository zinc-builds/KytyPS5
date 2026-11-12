#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMITPREFETCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMITPREFETCH_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

namespace Libs::Graphics {

// KYTY_COMMIT_PREFETCH (RenderExecutor::PrefetchQueued): while the command thread commits queued
// draw N, it prefetches what it will read for the draws behind it, one pointer level per draw of
// distance, so no level needs a load that misses. Most of what it pulls was written by a prepare
// worker on another core (the slot, its vectors, the worker's records).
// Levels (KYTY_COMMIT_PREFETCH_LEVELS, a bit mask; default all):
//   1 H: slot N+3's lines the commit reads;
//   2 S: what slot N+2 points at: proposal arrays, log and buffer vectors, target records;
//   4 R: texture records of N+1 (not those the previous draw proposed at the same place);
//   8 I: images of N's texture records and targets (likewise only new ones).
// A level is skipped for a draw not yet prepared at its distance (nothing writes a prepared slot
// until it commits). A prefetch never faults and reads nothing guest-visible, and every pointer
// followed is alive: a queued draw's records are freed only after the draw commits
// (SharedTextureMemo::Collect, WorkerTargetMemo's retired lists), and images are addressed
// through the texture cache's slot vector, whose chunks are never freed.
// KYTY_COMMIT_PREFETCH=0 turns it off.
[[nodiscard]] inline uint32_t CommitPrefetchLevels() {
	static const uint32_t levels = [] {
		const char* mode = std::getenv("KYTY_COMMIT_PREFETCH");
		if (mode == nullptr || std::strcmp(mode, "0") == 0) {
			return 0u;
		}
		const char* mask = std::getenv("KYTY_COMMIT_PREFETCH_LEVELS");
		return mask == nullptr ? 15u : static_cast<uint32_t>(std::strtoul(mask, nullptr, 0)) & 15u;
	}();
	return levels;
}

inline void PrefetchLine(const void* address) {
	_mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
}
// Every 64-byte line of [address, address + bytes), at most `max_lines`.
inline void PrefetchBytes(const void* address, size_t bytes, size_t max_lines = 16) {
	const auto begin = reinterpret_cast<uintptr_t>(address) & ~uintptr_t {63};
	const auto end   = reinterpret_cast<uintptr_t>(address) + bytes;
	for (auto line = begin; line < end && max_lines != 0; line += 64, --max_lines) {
		_mm_prefetch(reinterpret_cast<const char*>(line), _MM_HINT_T0);
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMITPREFETCH_H_
