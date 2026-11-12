#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_DRAINPROBE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_DRAINPROBE_H_

// WIP stats (KYTY_DRAIN_PROBE=1, not for merge): do decode-time guest reads touch pages that the
// draws a cache-op drain commits GPU-write?
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace Libs::Graphics::DrainProbe {

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_DRAIN_PROBE") != nullptr;
	return enabled;
}
inline bool                          g_capture = false;
inline std::unordered_set<uint64_t>  g_drained_pages; // written by the last cache-op drain's draws
inline std::unordered_set<uint64_t>  g_history;       // every page any draw GPU-wrote
inline uint64_t g_reads = 0, g_read_bytes = 0, g_hit_drained = 0, g_hit_history = 0, g_drains = 0,
                g_drained_draws = 0, g_ib_reads = 0, g_reads_after_drain = 0, g_since_drain = 0;
inline uint64_t g_start_ns = 0;

inline uint64_t Now() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

inline std::unordered_set<uint64_t> g_seen_ranges; // address ^ size << 1: expanded once
inline void NoteWrite(uint64_t address, uint64_t size) {
	if (!Enabled() || size == 0) {
		return;
	}
	if (!g_capture && !g_seen_ranges.insert(address ^ (size << 1u)).second) {
		return;
	}
	for (uint64_t page = address >> 12u; page <= (address + size - 1) >> 12u; page++) {
		g_history.insert(page);
		if (g_capture) {
			g_drained_pages.insert(page);
		}
	}
}

inline void Print() {
	const auto now = Now();
	if (g_start_ns == 0) {
		g_start_ns = now;
		return;
	}
	if (now - g_start_ns < 1'000'000'000ull) {
		return;
	}
	std::printf("DRAINPROBE cache_drains=%" PRIu64 " drained_draws=%" PRIu64 " decode_reads=%" PRIu64
	            " (ib %" PRIu64 ", bytes %" PRIu64 ") reads_in_drain_window=%" PRIu64
	            " hit_drained_pages=%" PRIu64 " hit_any_gpu_written_page=%" PRIu64
	            " history_pages=%zu\n",
	            g_drains, g_drained_draws, g_reads, g_ib_reads, g_read_bytes, g_reads_after_drain,
	            g_hit_drained, g_hit_history, g_history.size());
	std::fflush(stdout);
	g_drains = g_drained_draws = g_reads = g_ib_reads = g_read_bytes = g_reads_after_drain =
	    g_hit_drained = g_hit_history = 0;
	g_start_ns = now;
}

// A decode-time read of guest memory. window: the read comes after a cache-op drain and before
// the next draw is posted... approximated as: since the last cache-op drain.
inline void NoteRead(const void* pointer, uint64_t size, bool indirect_buffer) {
	if (!Enabled() || size == 0) {
		return;
	}
	const auto address = reinterpret_cast<uint64_t>(pointer);
	g_reads++;
	g_ib_reads += indirect_buffer ? 1 : 0;
	g_read_bytes += size;
	g_reads_after_drain += g_since_drain != 0 ? 1 : 0;
	bool drained = false, history = false;
	for (uint64_t page = address >> 12u; page <= (address + size - 1) >> 12u; page++) {
		drained = drained || g_drained_pages.contains(page);
		history = history || g_history.contains(page);
	}
	g_hit_drained += drained ? 1 : 0;
	g_hit_history += history ? 1 : 0;
	Print();
}

inline void BeginCacheDrain(uint64_t queued) {
	if (!Enabled()) {
		return;
	}
	g_drains++;
	g_drained_draws += queued;
	g_drained_pages.clear();
	g_capture     = true;
	g_since_drain = 1;
}
inline void EndCacheDrain() {
	g_capture = false;
}

} // namespace Libs::Graphics::DrainProbe

#endif
