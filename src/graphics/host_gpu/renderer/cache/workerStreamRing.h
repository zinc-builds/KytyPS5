#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WORKERSTREAMRING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WORKERSTREAMRING_H_

#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

namespace Libs::Graphics {

// Stream copies a draw prepare worker takes for its queued draws (BufferCache::SpeculateLookup):
// one ring per worker. The worker allocates in draw order and never waits; space a draw used is
// reused once the command thread has retired that draw (recorded it, or dropped its copies) and
// the GPU has passed the tick it was recorded at. A draw the ring has no room for falls back to
// the command thread's own stream copy.
class WorkerStreamRing {
public:
	WorkerStreamRing(GraphicContext& graphics, CommandScheduler& scheduler, uint64_t size);
	KYTY_CLASS_NO_COPY(WorkerStreamRing);

	[[nodiscard]] Buffer& GetBuffer() noexcept { return m_buffer; }

	// Worker: the draw the next allocations belong to (sequences ascend).
	void BeginDraw(uint64_t sequence) noexcept { m_sequence = sequence; }
	// Worker: room for `size` bytes at `alignment`, or null. Flush() it once written.
	[[nodiscard]] std::pair<uint8_t*, uint64_t> Allocate(uint64_t size, uint64_t alignment);
	void Flush(uint64_t offset, uint64_t size);

	// Command thread: draw `sequence` of this worker was recorded into tick `tick` (or never
	// used its copies). In sequence order.
	void Retire(uint64_t sequence, uint64_t tick) noexcept {
		m_ticks[sequence % TICK_SLOTS].store(tick, std::memory_order_relaxed);
		m_retired.store(sequence, std::memory_order_release);
	}

	// Worker: an earlier copy of the same range still in the ring, taken at the same
	// GuestSyncEpoch (the guest has had no chance to rewrite it since) and CPU-dirty growth; it
	// then stays until this draw retires. UINT64_MAX: none.
	[[nodiscard]] uint64_t FindCopy(uint64_t vaddr, uint64_t size, uint64_t epoch,
	                                uint64_t growth, uint64_t& hash);
	// Worker: a copy just allocated (by the last Allocate) and written. hash: of its bytes
	// (KYTY_WORKER_STREAM=verify), else 0.
	void NoteCopy(uint64_t vaddr, uint64_t size, uint64_t offset, uint64_t epoch,
	              uint64_t growth, uint64_t hash);

	[[nodiscard]] uint64_t Full() const noexcept { return m_full.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t Copies() const noexcept {
		return m_copies.load(std::memory_order_relaxed);
	}
	[[nodiscard]] uint64_t Reuses() const noexcept {
		return m_reuses.load(std::memory_order_relaxed);
	}

private:
	struct Allocation {
		uint64_t start    = 0;
		uint64_t end      = 0; // offset past the draw's allocations
		uint64_t sequence = 0;
	};
	static constexpr uint64_t TICK_SLOTS = 256;

	// Worker: drops the oldest allocations the GPU no longer reads.
	void Release();

	StreamBuffer        m_buffer;
	const uint64_t      m_atom;
	CommandScheduler&   m_scheduler;
	// Worker only.
	uint64_t               m_sequence = 0;
	uint64_t               m_head     = 0; // next free offset
	uint64_t               m_tail     = 0; // start of the oldest live allocation
	std::deque<Allocation> m_live;
	uint64_t               m_popped = 0; // allocations released so far (index of m_live[0])
	struct Copy {
		uint64_t vaddr  = 0;
		uint64_t size   = 0;
		uint64_t offset = 0;
		uint64_t epoch  = 0;
		uint64_t growth = 0;
		uint64_t entry  = UINT64_MAX; // its allocation's index
		uint64_t hash   = 0;
	};
	static constexpr size_t COPY_SLOTS = 4096;
	std::unique_ptr<std::array<Copy, COPY_SLOTS>> m_copies_by_range =
	    std::make_unique<std::array<Copy, COPY_SLOTS>>();
	std::atomic<uint64_t> m_copies {0};
	std::atomic<uint64_t> m_reuses {0};
	// Command thread writes, worker reads. A slot read after a later retire of the same slot
	// holds a later (larger) tick, which only delays reuse.
	std::array<std::atomic<uint64_t>, TICK_SLOTS> m_ticks {};
	std::atomic<uint64_t>                         m_retired {0};
	std::atomic<uint64_t>                         m_full {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_WORKERSTREAMRING_H_
