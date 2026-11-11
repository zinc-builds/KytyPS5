#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_

// Guest-memory traffic diagnostic (KYTY_MEM_DIAG=1): per-second MEMDIAG lines on stdout with
// stream copies, buffer uploads by caller, write-watch changes and write faults by thread kind.
// Relaxed atomics; costs one predictable branch per event when off.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#if defined(__linux__)
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>

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
	WaitCollections,        // satisfied waits that collected asynchronous writes
	WaitOnCpLabel,          // satisfied waits on the processor's own untouched label (no collection)
	AsyncScans,
	AsyncScanNs,
	AsyncChunks,            // chunks under async write protection (sampled per scan)
	AsyncWrittenPages,      // pages the async write scan reported written
	AsyncInvalidatedPages,  // of those, pages a cache watched (invalidated like a fault)
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

// Write-fault addresses, summarized per 2 MiB chunk in the per-second report.
void NoteWriteFault(uint64_t vaddr) noexcept;

// Extra per-second report (the page manager's watch footprint and scan cost).
inline void (*g_report_hook)() = nullptr;

// KYTY_ASYNC_WP_TRACE=<hex page>: print every tracking event that touches one page.
inline uint64_t TracePage() noexcept {
	static const uint64_t page = [] {
		const char* value = std::getenv("KYTY_ASYNC_WP_TRACE");
		return value != nullptr ? std::strtoull(value, nullptr, 16) & ~uint64_t {0xfff} : 0;
	}();
	return page;
}
inline bool Traced(uint64_t vaddr, uint64_t size) noexcept {
	const auto page = TracePage();
	return page != 0 && vaddr < page + 0x1000 && page < vaddr + size;
}
inline void Trace(const char* event, uint64_t vaddr, uint64_t size, uint64_t extra = 0) noexcept {
	static std::mutex lock;
	std::scoped_lock  guard(lock);
	std::printf("WPTRACE %lld tid=%ld %s 0x%llx+0x%llx extra=0x%llx\n",
	            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                       std::chrono::steady_clock::now().time_since_epoch())
	                                       .count()),
#if defined(__linux__)
	            static_cast<long>(gettid()),
#else
	            0L,
#endif
	            event, static_cast<unsigned long long>(vaddr),
	            static_cast<unsigned long long>(size), static_cast<unsigned long long>(extra));
	std::fflush(stdout);
}

// Prints and resets the counters at most once per second (call from the GPU thread).
void MaybePrint() noexcept;

// KYTY_BDA_STATS=1|access: per-second BDASTATS lines about PrepareBda's uploads (what queued
// them, pages uploaded again within one frame, and for a 1/8 sample of pages how many bytes
// changed since their previous upload). `access` also makes DMA shaders mark the 16 KiB pages
// they read in a second bitmap after the fault bitmap (atomics on every DMA access: slow),
// and reports uploaded bytes that no DMA shader read in the same or the next second.
inline const int g_bda_stats_mode = [] {
	const char* value = std::getenv("KYTY_BDA_STATS");
	if (value == nullptr || *value == '\0' || std::strcmp(value, "0") == 0) {
		return 0;
	}
	return std::strcmp(value, "access") == 0 ? 2 : 1;
}();
// A plain load: ObtainBuffer checks it ~2M times a second.
[[nodiscard]] inline int BdaStatsMode() noexcept {
	return g_bda_stats_mode;
}
inline std::atomic<uint64_t> g_flips {0}; // video-out flips queued by the command thread
inline std::atomic<uint64_t> g_bda_access_shaders {0}; // shaders emitted with access marks

} // namespace Libs::Graphics::MemDiag

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMDIAG_H_
