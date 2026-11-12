#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEVERSIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEVERSIONS_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>

// A version per 4 KiB guest page that moves whenever the page's bytes may have changed under
// something that copied them earlier (the BDA pre-stager, KYTY_BDA_PRESTAGE):
//   - every invalidation of tracked memory (MemoryTracker::InvalidateRegion and
//     MarkRegionAsCpuModified: guest CPU writes found by faults or asynchronous scans, and
//     emulator stores that invalidate), +2;
//   - every emulator store through the backing store (LibKernel::Memory::TryWriteBacking,
//     TryStoreLabelBacking, TryFillBacking: labels, readback and download publishes, fills),
//     +1 before and +1 after, so a copy overlapping the store sees an odd or a moved version;
//   - every write the command processor makes through a guest pointer and reports with
//     RenderContext::NoteGpuWrite (reference clock, occlusion results, CP fills and copies), +2.
// A copy taken while the version was even and equal before and after it holds the page's bytes
// as of that version. Any thread may bump or read; the tables are created on first use and live
// as long as the process.
namespace Libs::Graphics::PageVersions {

inline constexpr uint64_t PageShift   = 12;
inline constexpr uint64_t ChunkShift  = 26; // 64 MiB of pages (64 KiB of versions) per table
inline constexpr uint64_t ChunkPages  = uint64_t {1} << (ChunkShift - PageShift);
inline constexpr uint64_t ChunkCount  = TRACKER_ADDRESS_SIZE >> ChunkShift;

struct Chunk {
	std::atomic<uint32_t> versions[ChunkPages] {};
};

inline std::unique_ptr<std::atomic<Chunk*>[]> g_chunks =
    std::make_unique<std::atomic<Chunk*>[]>(ChunkCount);
inline std::mutex g_create_mutex;

// The version of page number `page` (vaddr >> PageShift), creating its table (a page nobody
// bumped yet is at version 0).
[[nodiscard]] inline std::atomic<uint32_t>* Slot(uint64_t page) noexcept {
	const auto index = page >> (ChunkShift - PageShift);
	if (index >= ChunkCount) {
		return nullptr;
	}
	auto* chunk = g_chunks[index].load(std::memory_order_acquire);
	if (chunk == nullptr) {
		std::scoped_lock lock(g_create_mutex);
		chunk = g_chunks[index].load(std::memory_order_relaxed);
		if (chunk == nullptr) {
			chunk = new Chunk; // never freed: the table lives as long as the process
			g_chunks[index].store(chunk, std::memory_order_release);
		}
	}
	return &chunk->versions[page & (ChunkPages - 1)];
}

template <typename Func>
inline void ForEachPage(uint64_t vaddr, uint64_t size, Func&& func) noexcept {
	if (size == 0 || vaddr > UINT64_MAX - size) {
		return;
	}
	for (uint64_t page = vaddr >> PageShift, last = (vaddr + size - 1) >> PageShift; page <= last;
	     page++) {
		if (auto* slot = Slot(page); slot != nullptr) {
			func(*slot);
		}
	}
}

// Whether versions are kept at all (only the pre-stager reads them): KYTY_BDA_PRESTAGE not 0.
[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BDA_PRESTAGE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// The bytes changed (already, or are about to under a lock the reader cannot see): +2.
inline void Bump(uint64_t vaddr, uint64_t size) noexcept {
	if (Enabled()) {
		ForEachPage(vaddr, size, [](std::atomic<uint32_t>& v) {
			v.fetch_add(2, std::memory_order_acq_rel);
		});
	}
}

// Around an emulator store: odd while it runs.
inline void BeginStore(uint64_t vaddr, uint64_t size) noexcept {
	if (Enabled()) {
		ForEachPage(vaddr, size, [](std::atomic<uint32_t>& v) {
			v.fetch_add(1, std::memory_order_acq_rel);
		});
	}
}
inline void EndStore(uint64_t vaddr, uint64_t size) noexcept {
	BeginStore(vaddr, size);
}

// The version of the page holding vaddr.
[[nodiscard]] inline uint32_t Read(uint64_t vaddr) noexcept {
	auto* slot = Slot(vaddr >> PageShift);
	return slot != nullptr ? slot->load(std::memory_order_acquire) : 1u; // odd: never usable
}

} // namespace Libs::Graphics::PageVersions

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEVERSIONS_H_
