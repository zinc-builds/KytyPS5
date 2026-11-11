#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTSYNCEPOCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTSYNCEPOCH_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics::GuestSyncEpoch {

// Advances whenever the guest may legitimately rewrite memory that queued GPU work reads: a new
// or resumed submission, any PM4 packet other than state, draws and markers (labels, waits,
// DMA, dispatches, events), and any emulator write into guest memory on the GPU's behalf
// (downloads, label stores through the backing). Between two advances the guest has seen no
// sign that the GPU consumed earlier work, so the bytes that work reads cannot change.
inline std::atomic<uint64_t> g_epoch {1};
// The same without emulator stores whose ranges DrawPrepare records (AdvanceRanged): queued
// draws check those by range instead.
inline std::atomic<uint64_t> g_unranged_epoch {1};

// Set while the caller records the ranges of the emulator's label stores itself
// (DrawPrepare::NoteStoredRange): those stores then advance only the ranged epoch.
inline thread_local bool g_label_stores_ranged = false;

inline void Advance() noexcept {
	g_unranged_epoch.fetch_add(1, std::memory_order_acq_rel);
	g_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// For a store whose range the caller also records with DrawPrepare::NoteStoredRange.
inline void AdvanceRanged() noexcept {
	g_epoch.fetch_add(1, std::memory_order_acq_rel);
}

[[nodiscard]] inline uint64_t Current() noexcept {
	return g_epoch.load(std::memory_order_acquire);
}

[[nodiscard]] inline uint64_t CurrentUnranged() noexcept {
	return g_unranged_epoch.load(std::memory_order_acquire);
}

} // namespace Libs::Graphics::GuestSyncEpoch

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTSYNCEPOCH_H_
