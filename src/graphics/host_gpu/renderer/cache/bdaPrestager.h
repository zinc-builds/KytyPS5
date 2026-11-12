#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPRESTAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPRESTAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
struct GraphicContext;

// Copies guest pages queued for PrepareBda into a host-visible arena on an efficiency core, ahead
// of the draw that uploads them, so that draw's upload only records GPU copies
// (docs/mt-rewrite/bda-prestage-design.md). A staged copy of a page is used only if the page was
// armed before the copy began (LibKernel::Memory::ArmStaged), its version (PageVersions) was even
// and unchanged across the copy and still is at use, and the guest remap generation has not moved
// since the arm. Anything else copies at use, as before.
//
// KYTY_BDA_PRESTAGE: 1 (default) on, 0 off, verify: also compare every staged page used with
// guest memory at use and count differences, strict: exit on the first one, repair (diagnostic):
// count them and copy those pages fresh.
// KYTY_BDA_PRESTAGE_ARENA=<MiB> (64), KYTY_BDA_PRESTAGE_CPU=<cpu>, KYTY_BDA_PRESTAGE_STATS=1:
// BDAPRESTAGE lines. KYTY_BDA_PRESTAGE_DROP_ARM=<n>: fault injection, skips the arm of every nth
// range (verify must then find differences).
class BdaPrestager {
public:
	// for_each_mapped(vaddr, size, fn): fn(begin, end) for the parts PrepareBda takes.
	using ForEachMapped = std::function<void(uint64_t, uint64_t,
	                                         const std::function<void(uint64_t, uint64_t)>&)>;

	BdaPrestager(GraphicContext& graphics, CommandScheduler& scheduler,
	             ForEachMapped for_each_mapped);
	// Tests: no Vulkan and no thread; Stage() copies on the caller's thread.
	struct TestSetup {
		int                                                        mode = 2;
		std::span<uint8_t>                                         arena;
		std::function<std::optional<uint64_t>(uint64_t, uint64_t)> arm;
		std::function<uint64_t()>                                  current_tick;
		std::function<bool(uint64_t)>                              is_free;
	};
	explicit BdaPrestager(TestSetup setup);
	~BdaPrestager();
	KYTY_CLASS_NO_COPY(BdaPrestager);

	[[nodiscard]] static int Mode(); // 0 off, 1 on, 2 verify, 3 strict, 4 repair
	[[nodiscard]] bool       Active() const noexcept { return m_active; }

	// Any thread: pages queued for PrepareBda's next sync.
	void Enqueue(uint64_t begin, uint64_t end);

	// Command thread, while uploading [address, address + size) to `dst_offset` of a buffer: the
	// pages served from staging append to `staged` (srcOffset: arena offset), the others to
	// `fresh` (guest address, size). The staged slots stay pinned until the current scheduler tick
	// is free.
	void Take(uint64_t address, uint64_t size, uint64_t dst_offset,
	          std::vector<vk::BufferCopy>& staged, std::vector<std::pair<uint64_t, uint64_t>>& fresh);

	[[nodiscard]] vk::Buffer ArenaHandle() const noexcept;

	// Tests: stages [begin, end) now, on this thread.
	void Stage(uint64_t begin, uint64_t end) { StageRange(begin, end); }

	struct Stats {
		std::atomic<uint64_t> staged {0}, refused {0}, odd {0}, moved_in_copy {0}, superseded {0},
		    evicted {0}, arena_full {0}, served {0}, fresh_missing {0}, fresh_refused {0},
		    fresh_evicted {0}, fresh_version {0}, fresh_generation {0}, compared {0},
		    mismatched {0}, busy_ns {0}, arms {0};
		// Version fallbacks by age of the copy (<100 µs, <1 ms, <10 ms, more) and by how far the
		// version moved (1: a store in progress, 2: one bump, 3-6, more).
		std::atomic<uint64_t> moved_age[4] {}, moved_by[4] {};
		// Batches taken from the inbox: queued and coalesced bytes, and their lag (oldest queueing
		// to the batch's end).
		std::atomic<uint64_t> batches {0}, queued_bytes {0}, coalesced_bytes {0}, lag_ns {0},
		    lag_max_ns {0}, current {0};
	};
	[[nodiscard]] const Stats& Counters() const noexcept { return m_stat; }

private:
	static constexpr uint64_t PageSize   = 4096;
	static constexpr uint64_t ChunkBytes = 256 * 1024; // staged (armed) at a time
	static constexpr uint64_t ChunkPages = ChunkBytes / PageSize;

	struct Entry {
		uint32_t slot       = 0;
		uint32_t version    = 0;
		uint64_t generation = 0;
		uint64_t serial     = 0; // eviction order
		uint64_t staged_ns  = 0; // statistics: when it was published
	};
	enum class Miss : uint8_t { None, Refused, Evicted };

	void Run();
	void StageRange(uint64_t begin, uint64_t end);
	void StageChunk(uint64_t chunk, uint64_t chunk_end, uint64_t generation);
	bool AllocateSlot(uint32_t& slot); // under m_mutex
	void Release(uint64_t page);       // stager thread: drops one arm reference
	void FlushReleases();              // stager thread: m_released, then the tracker's Unstage
	void ReclaimRetired();             // under m_mutex
	void ReportIfDue();                // stager thread
	void Init(uint64_t arena_bytes);
	[[nodiscard]] std::optional<uint64_t> Arm(uint64_t vaddr, uint64_t size);
	[[nodiscard]] uint64_t                CurrentTick() const;
	[[nodiscard]] bool                    IsFree(uint64_t tick) const;

	CommandScheduler* m_scheduler = nullptr; // null in tests (m_test)
	TestSetup         m_test;
	ForEachMapped     m_for_each_mapped;
	int               m_mode   = 0;
	bool              m_active = false;
	uint32_t          m_drop_arm = 0;
	bool              m_stats    = false;

	std::unique_ptr<Buffer> m_arena;
	uint8_t*                m_arena_bytes = nullptr;
	uint32_t                m_slot_count  = 0;
	std::vector<uint8_t>    m_shadow; // verify: the staged bytes, readable (the arena is write-combined)

	// Inbox (any thread → stager).
	std::mutex                                    m_inbox_mutex;
	std::condition_variable                       m_inbox_cv;
	std::vector<std::pair<uint64_t, uint64_t>>    m_inbox;
	bool                                          m_stop = false;
	uint64_t                                      m_inbox_oldest_ns = 0; // statistics

	// Entries, slots (stager ↔ command thread).
	mutable std::mutex                            m_mutex;
	std::unordered_map<uint64_t, Entry>           m_entries; // by page address
	std::vector<uint32_t>                         m_free_slots;
	std::deque<std::pair<uint64_t, uint64_t>>     m_order;   // (page, serial), oldest first
	std::vector<std::pair<uint32_t, uint64_t>>    m_retired; // (slot, scheduler tick)
	std::unordered_map<uint64_t, Miss>            m_misses;  // why a page has no entry
	uint64_t                                      m_serial = 0;
	uint64_t                                      m_ranges = 0; // staged ranges (drop-arm)
	std::vector<uint64_t>                         m_released; // pages whose entry went (Take, eviction)

	// Stager thread only: every ArmStaged and Unstage call happens here, so an Unstage can never
	// undo an arm taken for a newer copy. A page holds one reference per live entry and per arm in
	// progress; the tracker is told to unstage it when the last goes.
	std::unordered_map<uint64_t, uint32_t>        m_arm_refs;
	std::vector<uint64_t>                         m_releasing;
	std::vector<std::pair<uint64_t, uint64_t>>    m_unstage; // [begin, end) runs

	std::thread m_thread;

	// Statistics (KYTY_BDA_PRESTAGE_STATS, verify).
	Stats m_stat;
	uint64_t m_report_start = 0;

	// KYTY_BDA_PRESTAGE_TRACE=<n> (diagnostic): every nth version fallback prints the page's last
	// queueing and what the stager last did with it.
	enum class Outcome : uint8_t { None, Staged, Refused, Odd, Moved, Full };
	struct Trace {
		uint64_t queued_ns  = 0;
		uint64_t outcome_ns = 0;
		uint32_t version    = 0;
		Outcome  outcome    = Outcome::None;
	};
	uint32_t                                m_trace_every = 0;
	uint64_t                                m_trace_count = 0;
	std::unordered_map<uint64_t, Trace>     m_trace; // under m_mutex
	void NoteOutcome(uint64_t page, Outcome outcome, uint32_t version); // under m_mutex
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPRESTAGER_H_
