#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/asyncUploader.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/cache/workerStreamRing.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {},
	                                                        uint32_t speculated      = UINT32_MAX);
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	// Advances whenever a color-metadata check is forgotten (GPU thread).
	[[nodiscard]] uint64_t ColorChecksGeneration() const noexcept { return m_color_checks_generation; }
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	// Uploads between these two share one barrier pair (global barriers before the first copy and
	// after the last) instead of a pair per copy. Nothing but uploads may be recorded in between.
	// KYTY_UPLOAD_BATCH=0: a pair per copy again.
	void               BeginUploadBatch();
	void               EndUploadBatch();
	// Queue after the range's state change becomes visible, or a drain could consume it first.
	// What made a range need a BDA sync (KYTY_BDA_STATS splits uploads by it). Invalidate: any
	// InvalidateMemory (write faults, collected asynchronous writes, CPU label stores).
	enum class BdaSource : uint8_t { Invalidate, CpuWrite, NewBuffer, Map, Count };
	void                   QueueBdaSync(uint64_t vaddr, uint64_t size, BdaSource source);
	[[nodiscard]] RangeSet TakeBdaSync();
	// KYTY_BDA_STATS: per-second report; with `access`, also reads back the DMA access bitmap.
	void                   BdaStatsTick();
	void               RunGarbageCollector();

	// Buffer lookups made ahead on the draw prepare worker (KYTY_WORKER_BUFFERS=0|verify|stats).
	// A lookup covers a read-only range that one registered buffer holds and that had no
	// CPU-dirty page. It stays valid while no page anywhere newly became CPU-dirty
	// (g_cpu_dirty_generation) and no buffer was registered or unregistered since the
	// generations were read, which happens before the lookups.
	//
	// A CPU-written range the command thread would stream-copy (read-only, at most a page, no
	// GPU-dirty page, a CPU-dirty one) is copied by the worker instead, into its WorkerStreamRing
	// (`stream`). The worker runs after the draw's PM4 position, so the copy is no older than it.
	// The command thread uses the copy if the range is still in that state and no page of the
	// 64-page words holding it newly became CPU-dirty since (`growth`, per range, not global);
	// the draw's adoption checks it against GPU writes and emulator stores like its other reads.
	// KYTY_WORKER_STREAM=0 turns this off, =verify copies again at commit and compares, =stats
	// prints WORKERSTREAM lines.
	struct SpeculatedRange {
		uint64_t vaddr  = 0;
		uint64_t size   = 0;
		uint64_t offset = 0; // in the buffer, or in the stream ring
		BufferId id {};
		bool     stream = false;
		uint64_t growth = 0; // stream: MemoryTracker::CpuGrowthNoCreate when it was copied
		uint64_t hash   = 0; // stream, KYTY_WORKER_STREAM=verify: of the bytes copied
	};
	struct Speculation {
		uint64_t                     cpu_dirty_generation = 0;
		uint64_t                     registration_seq     = 1; // odd: none
		std::vector<SpeculatedRange> ranges;
		WorkerStreamRing*            ring = nullptr; // null: no stream copies
	};
	[[nodiscard]] static bool WorkerBuffersEnabled();
	[[nodiscard]] static bool WorkerBuffersVerify();
	[[nodiscard]] static bool WorkerStreamEnabled();
	// Command thread, at setup: a ring for one prepare worker (owned by the cache).
	[[nodiscard]] WorkerStreamRing* CreateWorkerStreamRing();
	// Prepare worker: the generations a speculation is taken against, read before any lookup.
	void BeginSpeculation(Speculation& speculation) const;
	// Prepare worker: records the buffer holding [vaddr, vaddr + size) if it is CPU-clean and
	// returns its index in the speculation (pass it to ObtainBuffer), else UINT32_MAX.
	[[nodiscard]] uint32_t SpeculateLookup(Speculation& speculation, uint64_t vaddr,
	                                       uint64_t size) const;
	// GPU command thread: whether a speculation still holds now.
	[[nodiscard]] bool SpeculationValid(const Speculation& speculation) const;
	// GPU command thread: ObtainBuffer answers read-only ranges given a speculated index from
	// `speculation` while it holds (null: none).
	void SetSpeculation(const Speculation* speculation) { m_speculation = speculation; }

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const { return !id || m_slot_buffers[id].is_deleted; }

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// KYTY_ASYNC_UPLOAD_VERIFY=1: reads the destination ranges of the last asynchronous upload
	// back after its copy and compares them with the bytes written to the host ring.
	void VerifyAsyncUpload(const Buffer& buffer, std::span<const vk::BufferCopy> copies,
	                       uint64_t total_size);
	// A readback for a guest thread (KYTY_SYNC_READBACK=1 turns these off). The GPU thread records
	// its download at its point in PM4 order and submits it without waiting; the priority runner
	// publishes the bytes after the tick; the first waiter's next command finishes it: its pages
	// lose GPU ownership (and protection) only once no newer GPU write or download covers them.
	struct PendingRead {
		uint64_t                                   start = 0; // download window
		uint64_t                                   size  = 0;
		uint64_t                                   tick  = 0;
		std::vector<std::pair<uint64_t, uint64_t>> ranges; // downloaded bytes, [begin, end)
		std::atomic<bool>                          published {false};
		bool                                       finished = false; // GPU thread only
	};
	[[nodiscard]] bool ReadMemoryAsync(uint64_t vaddr, uint64_t size, bool is_write);
	[[nodiscard]] std::shared_ptr<PendingRead> StartRead(Buffer& buffer, uint64_t start,
	                                                     uint64_t size);
	void FinishRead(PendingRead& read);
	// Waits for and finishes every pending read whose window overlaps the range.
	void RetireReads(uint64_t start, uint64_t size);
	// Releases GPU ownership of the range's pages that hold no GPU-dirty or in-flight bytes.
	void UnmarkCleanPages(uint64_t start, uint64_t size);
	void VerifyRead(const PendingRead& read);
	// GPU-dirty plus in-flight bytes: what the tracker's GPU-dirty pages must cover.
	[[nodiscard]] const RangeSet& OwnedRanges();

	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	// An asynchronous download for `read` records its bytes there and marks it published.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                        std::shared_ptr<PendingRead> read = {});
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool VerifyStreamHit(uint64_t vaddr, uint64_t size, uint64_t offset);
	[[nodiscard]] uint32_t SpeculateClean(Speculation& speculation, uint64_t vaddr,
	                                      uint64_t size) const;
	// The command thread's use of a worker stream copy (SpeculatedRange::stream).
	[[nodiscard]] bool AdoptWorkerStream(const SpeculatedRange& range);
	std::vector<std::unique_ptr<WorkerStreamRing>> m_worker_stream_rings;

	// Stream copies of CPU-written ranges, reused for the same (address, size) while the guest
	// has had no chance to rewrite them (GuestSyncEpoch), in the same command buffer and stream
	// lap, so the stream buffer's existing watch still covers the reuse.
	struct StreamCopy {
		uint64_t vaddr  = 0;
		uint64_t size   = 0;
		uint64_t offset = 0;
		uint64_t tick   = 0;
		uint64_t epoch  = 0;
		uint64_t lap    = 0;
	};
	static constexpr size_t STREAM_COPY_SLOTS = 8192;
	std::unique_ptr<std::array<StreamCopy, STREAM_COPY_SLOTS>> m_stream_copies =
	    std::make_unique<std::array<StreamCopy, STREAM_COPY_SLOTS>>();

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	std::vector<std::shared_ptr<PendingRead>>         m_pending_reads; // in record order
	RangeSet                                          m_inflight_ranges; // their bytes
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	RangeSet                                          m_owned_scratch;
#endif
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	uint64_t                                          m_color_checks_generation = 0;
	MemoryTracker                                     m_memory_tracker;
	std::mutex                                        m_bda_sync_mutex;
	RangeSet                                          m_bda_sync;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	AsyncUploader                                     m_async_uploader;
	std::unique_ptr<Buffer>                           m_bda_access_download; // KYTY_BDA_STATS=access
	bool                                              m_last_upload_async = false;
	struct UploadBatch {
		bool active = false; // between BeginUploadBatch and EndUploadBatch
		bool open   = false; // its opening barrier is recorded
	};
	UploadBatch                                       m_upload_batch;
	std::vector<uint8_t>                              m_async_verify_bytes;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;

	// The page table's owners again, readable by the prepare worker: atomic bucket pointers and
	// entries (owner index + 1, 0 for none), written next to m_page_table. Buckets live as long
	// as the cache.
	static constexpr size_t SHADOW_BUCKET_BITS = 10;
	static constexpr size_t SHADOW_BUCKETS     = PageTable::kPageCount >> SHADOW_BUCKET_BITS;
	std::unique_ptr<std::atomic<std::atomic<uint32_t>*>[]> m_shadow_pages;
	void                     SetShadowOwner(size_t page, uint32_t value);
	[[nodiscard]] uint32_t   ShadowOwner(size_t page) const;
	// Worker-readable buffer extents, set and cleared under an odd m_registration_seq.
	static constexpr size_t                    EXTENT_CAPACITY = size_t {1} << 18u;
	std::atomic<uint64_t>                      m_registration_seq {0};
	std::unique_ptr<std::atomic<uint64_t>[]>   m_extent_begin;
	std::unique_ptr<std::atomic<uint64_t>[]>   m_extent_end;
	const Speculation*                         m_speculation = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
