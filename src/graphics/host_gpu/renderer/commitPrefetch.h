#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMITPREFETCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMITPREFETCH_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

namespace Libs::Graphics {

// KYTY_COMMIT_PREFETCH (prototype; off by default): software prefetches for what the command
// thread reads when it commits a queued draw.
// - cmd: while committing draw N, the command thread prefetches for the draws behind it, one
//   pointer level per draw of distance, so no level needs a load that misses: the slot's header
//   lines at N+4, its proposal arrays and target records at N+3, texture records and target
//   images at N+2, texture images at N+1. A draw whose preparation is not finished is skipped
//   at that distance (nothing writes a finished slot until it commits).
// - worker: the prepare worker prefetches a memo record's command-thread lines when it proposes
//   the record.
// A prefetch never faults and reads nothing guest-visible. Every pointer followed is alive: a
// queued draw's records are freed only after the draw commits (SharedTextureMemo::Collect,
// WorkerTargetMemo's retired lists), and images are addressed through the texture cache's slot
// vector, whose chunks are never freed.
enum class CommitPrefetch : uint8_t { Off = 0, Worker = 1, Cmd = 2, Both = 3 };

[[nodiscard]] inline CommitPrefetch CommitPrefetchMode() {
	static const CommitPrefetch mode = [] {
		const char* value = std::getenv("KYTY_COMMIT_PREFETCH");
		if (value == nullptr) return CommitPrefetch::Off;
		if (std::strcmp(value, "worker") == 0) return CommitPrefetch::Worker;
		if (std::strcmp(value, "cmd") == 0) return CommitPrefetch::Cmd;
		if (std::strcmp(value, "both") == 0) return CommitPrefetch::Both;
		return CommitPrefetch::Off;
	}();
	return mode;
}
[[nodiscard]] inline bool CommitPrefetchCmd() {
	return (static_cast<uint8_t>(CommitPrefetchMode()) & 2u) != 0;
}
[[nodiscard]] inline bool CommitPrefetchWorker() {
	return (static_cast<uint8_t>(CommitPrefetchMode()) & 1u) != 0;
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
