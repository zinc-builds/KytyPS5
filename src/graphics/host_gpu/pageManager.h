#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

	// Whether a fault at vaddr is resolved by page tracking: the page is watched against the
	// access, or it is not (a watch released since the fault) and the access now succeeds.
	[[nodiscard]] bool IsTrackingFault(uint64_t vaddr, PageFaultAccess access) const noexcept;

	// KYTY_ASYNC_WP_VERIFY: takes the pages that lost their last write watcher.
	static void TakeVerifyUnwatched(std::vector<uint64_t>& pages);

	// Appends the runs of write-watched pages within [begin, end).
	void AppendWriteWatched(uint64_t begin, uint64_t end,
	                        std::vector<std::pair<uint64_t, uint64_t>>& runs) const;

	// The region table and the offset of a region's page-state bytes, for lock-free readers
	// that test SrtWalkerReadWatchedBit; {nullptr, 0} when the layout does not match.
	[[nodiscard]] std::pair<const void* const*, uint64_t> ReadWatchTable() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
