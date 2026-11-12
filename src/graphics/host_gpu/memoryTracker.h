#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageVersions.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	// Queries, uploads, downloads and GPU unmarks run one at a time (on the GPU thread). They do
	// not wait for CPU write faults of other threads, which only add CPU-dirty pages.
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Any thread: true only if every tracked page of the range exists and is CPU-clean (a page
	// of a region not created yet counts as dirty). Creates nothing.
	// Calls func(region or null, offset, bytes) for each region part of a range that lies below
	// REGION_COUNT regions (callers check the bounds).
	template <typename Func>
	void ForEachRegionNoCreate(uint64_t vaddr, uint64_t size, Func&& func) const {
		uint64_t remaining = size;
		uint64_t index     = vaddr / TRACKER_REGION_SIZE;
		uint64_t offset    = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0 && index < REGION_COUNT) {
			const auto bytes = std::min(TRACKER_REGION_SIZE - offset, remaining);
			func(static_cast<const RegionManager*>(m_regions[index].load(std::memory_order_acquire)),
			     offset, bytes);
			remaining -= bytes;
			offset = 0;
			index++;
		}
	}
	[[nodiscard]] bool IsRegionCpuCleanNoCreate(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr / TRACKER_REGION_SIZE >= REGION_COUNT ||
		    (vaddr + size - 1) / TRACKER_REGION_SIZE >= REGION_COUNT) {
			return false;
		}
		uint64_t remaining = size;
		uint64_t index     = vaddr / TRACKER_REGION_SIZE;
		uint64_t offset    = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			const auto* region = m_regions[index].load(std::memory_order_acquire);
			if (region == nullptr || region->IsModifiedUnlocked<DirtySource::Cpu>(offset, bytes)) {
				return false;
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return true;
	}
	// Any thread, for a stream copy taken ahead: whether every page of the range exists, none is
	// GPU-dirty and one is CPU-dirty; `growth` gets RegionManager::CpuGrowth summed over the
	// range (read before the bits). Creates nothing.
	[[nodiscard]] bool IsRegionStreamableNoCreate(uint64_t vaddr, uint64_t size,
	                                              uint64_t& growth) const {
		if (size == 0 || vaddr / TRACKER_REGION_SIZE >= REGION_COUNT ||
		    (vaddr + size - 1) / TRACKER_REGION_SIZE >= REGION_COUNT) {
			return false;
		}
		growth        = 0;
		bool cpu      = false;
		bool complete = true;
		ForEachRegionNoCreate(vaddr, size, [&](const RegionManager* region, uint64_t offset,
		                                       uint64_t bytes) {
			if (region == nullptr) {
				complete = false;
				return;
			}
			growth += region->CpuGrowth(offset, bytes);
		});
		if (!complete) {
			return false;
		}
		ForEachRegionNoCreate(vaddr, size, [&](const RegionManager* region, uint64_t offset,
		                                       uint64_t bytes) {
			if (region->IsModifiedUnlocked<DirtySource::Gpu>(offset, bytes)) {
				complete = false;
			}
			cpu |= region->IsModifiedUnlocked<DirtySource::Cpu>(offset, bytes);
		});
		return complete && cpu;
	}
	// Any thread: the same sum now (0 if a region is missing).
	[[nodiscard]] uint64_t CpuGrowthNoCreate(uint64_t vaddr, uint64_t size) const {
		uint64_t growth = 0;
		ForEachRegionNoCreate(vaddr, size, [&](const RegionManager* region, uint64_t offset,
		                                       uint64_t bytes) {
			growth += region != nullptr ? region->CpuGrowth(offset, bytes) : 0;
		});
		return growth;
	}
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	// Removes protection from a range and flushes GPU-owned data when required.
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		PageVersions::Bump(vaddr, size);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const bool should_flush = [&] {
				// Perform both the GPU modification check and CPU state change with the lock in
				// case the GPU thread is racing to mark the page modified. If a flush is needed,
				// on_flush performs the CPU state change.
				std::scoped_lock lock(manager->lock);
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(offset, bytes);
				return false;
			}();
			if (should_flush) {
				on_flush();
			}
		});
	}
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		const OwnerScope owner(*this);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			if (!manager->IsModifiedUnlocked<DirtySource::Gpu>(offset, bytes)) {
				return;
			}
			std::scoped_lock lock(manager->lock);
			manager->ForEachModifiedRange<DirtySource::Gpu, false>(offset, bytes, func);
			if constexpr (clear) {
				manager->ChangeState<DirtySource::Gpu, false>(offset, bytes);
			}
		});
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		const OwnerScope owner(*this);
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			// A written range takes every lock: they stay held until it is marked GPU-dirty.
			if (!is_written && !manager->IsModifiedUnlocked<DirtySource::Cpu>(offset, bytes)) {
				return;
			}
			manager->lock.lock();
			manager->ForEachModifiedRange<DirtySource::Cpu, true>(offset, bytes, range_func);
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		upload_func();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->ChangeState<DirtySource::Gpu, true>(offset, bytes);
				               manager->lock.unlock();
			               });
		}
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	// Uploads, downloads and GPU unmarks never overlap: the lock-free dirty reads rely on it.
	class OwnerScope final {
	public:
		explicit OwnerScope(MemoryTracker& tracker) noexcept;
		~OwnerScope();
		KYTY_CLASS_NO_COPY(OwnerScope);

	private:
		MemoryTracker* m_tracker = nullptr;
	};
#else
	struct OwnerScope final {
		explicit OwnerScope(MemoryTracker& /*tracker*/) noexcept {}
	};
#endif

	static void    ValidateRange(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	PageManager&                                   m_page_manager;
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::atomic<std::thread::id> m_owner_thread {};
#endif
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
