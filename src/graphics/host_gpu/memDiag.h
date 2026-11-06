#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_

// Guest-memory traffic diagnostic (KYTY_MEM_DIAG=1): per-second MEMDIAG lines on stdout with
// stream copies, buffer uploads by caller, write-watch changes and write faults by thread kind.
// Relaxed atomics; costs one predictable branch per event when off.

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics::MemDiag {

enum class Sync { Obtain, Bda, Image, Count };

enum Stat : int {
	ObtainCalls,
	ObtainWritten,
	StreamCalls,
	StreamBytes,
	// Stream copies whose exact (address, size) was copied before in the same guest label epoch
	// (no completion label written in between), split by whether the bytes changed.
	StreamRepeatSame,
	StreamRepeatChanged,
	// Same, but within one guest command submission.
	StreamRepeatSubmitSame,
	StreamRepeatSubmitChanged,
	// Exact range copied before in any epoch with the same bytes.
	StreamRepeatAnySame,
	StreamReuse,          // stream copy served from an earlier copy (no copy)
	StreamReuseOldTick,   // same range and epoch, but an earlier command buffer
	StreamReuseOldEpoch,  // same range, but the guest may have rewritten it since
	StreamVerifyFailed,   // KYTY_STREAM_VERIFY: a reuse whose bytes changed
	StreamMissLarge,  // stream candidate rejected: larger than one caching page
	StreamMissGpu,    // GPU-modified bytes
	StreamMissClean,  // not CPU-modified: no copy needed
	SyncCalls0,
	SyncUploads0 = SyncCalls0 + static_cast<int>(Sync::Count),
	SyncBytes0   = SyncUploads0 + static_cast<int>(Sync::Count),
	SyncRanges0  = SyncBytes0 + static_cast<int>(Sync::Count),
	WatchWritePages = SyncRanges0 + static_cast<int>(Sync::Count), // pages newly write-watched
	UnwatchWritePages,
	WatchReadPages,
	UnwatchReadPages,
	WriteFaultsGpuThread,
	WriteFaultsOther,
	ReadFaultsGpuThread,
	ReadFaultsOther,
	LabelEpochs,
	Submits,
	StatCount
};

inline std::atomic<uint64_t> g_stats[StatCount] {};
inline std::atomic<uint64_t> g_label_epoch {0};
inline std::atomic<uint64_t> g_submit_epoch {0};

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool on = std::getenv("KYTY_MEM_DIAG") != nullptr;
	return on;
}

inline void Add(int stat, uint64_t value = 1) noexcept {
	if (Enabled()) {
		g_stats[stat].fetch_add(value, std::memory_order_relaxed);
	}
}

inline void Add(int base, Sync kind, uint64_t value) noexcept {
	Add(base + static_cast<int>(kind), value);
}

// A completion label (or other GPU-to-guest write) became visible to the guest.
inline void NoteLabel() noexcept {
	if (Enabled()) {
		g_label_epoch.fetch_add(1, std::memory_order_relaxed);
		g_stats[LabelEpochs].fetch_add(1, std::memory_order_relaxed);
	}
}

inline void NoteSubmit() noexcept {
	if (Enabled()) {
		g_submit_epoch.fetch_add(1, std::memory_order_relaxed);
		g_stats[Submits].fetch_add(1, std::memory_order_relaxed);
	}
}

// Prints and resets the counters at most once per second (call from the GPU thread).
void MaybePrint() noexcept;

} // namespace Libs::Graphics::MemDiag

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_
