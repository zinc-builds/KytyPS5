#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/drawPrepare.h"
#include "common/hybridAffinity.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/guestSyncEpoch.h"
#include "graphics/host_gpu/memDiag.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <condition_variable>
#include <string>
#include <thread>
#include <x86intrin.h>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>
#include <xxhash.h>

#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <execinfo.h>
#include <mutex>
#include <unordered_map>

// Local diagnostic (KYTY_FAULT_DIAG=1): remember recent GPU-written buffer ranges and their
// callers, and report which one covered each CPU read that had to download GPU data.
namespace {
struct WrittenRange {
	uint64_t vaddr;
	uint64_t size;
	uint64_t caller;
	uint64_t count;
};
std::unordered_map<uint64_t, WrittenRange> g_written_pages; // 4 KiB page -> last writable bind
std::vector<WrittenRange>                  g_written_big;   // binds over 4 MiB, latest first
std::mutex                                 g_written_mutex;
bool FaultDiag() {
	static const bool on = std::getenv("KYTY_FAULT_DIAG") != nullptr;
	return on;
}
uint64_t CallerOffset(void* ra) {
	Dl_info info {};
	return dladdr(ra, &info) != 0 ? reinterpret_cast<uint64_t>(ra) -
	                                    reinterpret_cast<uint64_t>(info.dli_fbase)
	                              : reinterpret_cast<uint64_t>(ra);
}
void NoteWritten(uint64_t vaddr, uint64_t size, void* ra, void* /*ra2*/) {
	const uint64_t caller = CallerOffset(ra);
	std::scoped_lock lock(g_written_mutex);
	if (size > (4ull << 20u)) {
		for (auto& r: g_written_big) {
			if (r.vaddr == vaddr && r.size == size) {
				r.count++;
				r.caller = caller;
				return;
			}
		}
		g_written_big.insert(g_written_big.begin(), {vaddr, size, caller, 1});
		if (g_written_big.size() > 256) {
			g_written_big.pop_back();
		}
		return;
	}
	for (uint64_t page = vaddr >> 12u; page <= (vaddr + size - 1) >> 12u; page++) {
		auto& r = g_written_pages[page];
		r       = {vaddr, size, caller, r.count + 1};
	}
}
void ReportRead(uint64_t vaddr, uint64_t size, bool gpu_thread, double ms, bool downloaded) {
	static std::atomic<uint64_t> count {0};
	const auto n = count.fetch_add(1);
	if (n > 400 && (n % 100) != 0) {
		return;
	}
	WrittenRange hit {};
	{
		std::scoped_lock lock(g_written_mutex);
		if (auto it = g_written_pages.find(vaddr >> 12u); it != g_written_pages.end()) {
			hit = it->second;
		} else {
			for (const auto& r: g_written_big) {
				if (vaddr >= r.vaddr && vaddr < r.vaddr + r.size) {
					hit = r;
					break;
				}
			}
		}
	}
	char stack[160] = "";
	{
		void* frames[12];
		const int depth = backtrace(frames, 12);
		size_t    used  = 0;
		for (int i = 3; i < depth && used + 20 < sizeof(stack); i++) {
			used += static_cast<size_t>(std::snprintf(stack + used, sizeof(stack) - used, "%llx,",
			                                          static_cast<unsigned long long>(CallerOffset(frames[i]))));
		}
	}
	std::printf("FAULTDIAG stack=%s n=%llu read=0x%llx size=%llu gpu_thread=%d dl=%d ms=%.3f written=0x%llx+0x%llx "
	            "caller=0x%llx binds=%llu\n",
	            stack, static_cast<unsigned long long>(n), static_cast<unsigned long long>(vaddr),
	            static_cast<unsigned long long>(size), gpu_thread ? 1 : 0, downloaded ? 1 : 0, ms,
	            static_cast<unsigned long long>(hit.vaddr), static_cast<unsigned long long>(hit.size),
	            static_cast<unsigned long long>(hit.caller), static_cast<unsigned long long>(hit.count));
	std::fflush(stdout);
}
} // namespace
#endif

namespace Libs::Graphics::MemDiag {

namespace {
void ReportFaultChunks();
} // namespace


void MaybePrint() noexcept {
	using Clock = std::chrono::steady_clock;
	static Clock::time_point last = Clock::now();
	const auto now = Clock::now();
	if (now - last < std::chrono::seconds(1)) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - last).count();
	last                 = now;
	uint64_t v[StatCount];
	for (int i = 0; i < StatCount; i++) {
		v[i] = g_stats[i].exchange(0, std::memory_order_relaxed);
	}
	const auto r = [&](int i) { return static_cast<double>(v[i]) / seconds; };
	std::printf("MEMDIAG reuse_s=%.0f reuse_old_tick_s=%.0f reuse_old_epoch_s=%.0f "
	            "verify_failed_s=%.0f ",
	            r(StreamReuse), r(StreamReuseOldTick), r(StreamReuseOldEpoch),
	            r(StreamVerifyFailed));
	std::printf("obtain_s=%.0f written_s=%.0f stream_s=%.0f stream_kb_s=%.0f "
	            "rep_label_same_s=%.0f rep_label_changed_s=%.0f rep_submit_same_s=%.0f "
	            "rep_submit_changed_s=%.0f rep_any_same_s=%.0f miss_large_s=%.0f miss_gpu_s=%.0f "
	            "miss_clean_s=%.0f",
	            r(ObtainCalls), r(ObtainWritten), r(StreamCalls), r(StreamBytes) / 1024.0,
	            r(StreamRepeatSame), r(StreamRepeatChanged), r(StreamRepeatSubmitSame),
	            r(StreamRepeatSubmitChanged), r(StreamRepeatAnySame), r(StreamMissLarge),
	            r(StreamMissGpu), r(StreamMissClean));
	static const char* const names[] = {"obtain", "bda", "image"};
	for (int k = 0; k < static_cast<int>(Sync::Count); k++) {
		std::printf(" sync_%s_calls_s=%.0f uploads_s=%.0f kb_s=%.0f ranges_s=%.0f", names[k],
		            r(SyncCalls0 + k), r(SyncUploads0 + k), r(SyncBytes0 + k) / 1024.0,
		            r(SyncRanges0 + k));
	}
	std::printf(" watch_w_pages_s=%.0f unwatch_w_pages_s=%.0f watch_r_pages_s=%.0f "
	            "unwatch_r_pages_s=%.0f wfault_gpu_s=%.0f wfault_other_s=%.0f rfault_gpu_s=%.0f "
	            "rfault_other_s=%.0f labels_s=%.0f submits_s=%.0f",
	            r(WatchWritePages), r(UnwatchWritePages), r(WatchReadPages), r(UnwatchReadPages),
	            r(WriteFaultsGpuThread), r(WriteFaultsOther), r(ReadFaultsGpuThread),
	            r(ReadFaultsOther), r(LabelEpochs), r(Submits));
	std::printf(" wait_collections_s=%.0f wait_on_cp_label_s=%.0f", r(WaitCollections),
	            r(WaitOnCpLabel));
	std::printf(" async_written_pages_s=%.0f async_invalidated_pages_s=%.0f async_scans_s=%.0f "
	            "async_scan_us=%.1f async_chunks=%.0f",
	            r(AsyncWrittenPages), r(AsyncInvalidatedPages), r(AsyncScans),
	            v[AsyncScans] != 0 ? static_cast<double>(v[AsyncScanNs]) / 1000.0 / static_cast<double>(v[AsyncScans]) : 0.0,
	            v[AsyncScans] != 0 ? static_cast<double>(v[AsyncChunks]) / static_cast<double>(v[AsyncScans]) : 0.0);
	if (g_report_hook != nullptr) {
		g_report_hook();
	}
	ReportFaultChunks();
	std::printf("\n");
	std::fflush(stdout);
}

namespace {
std::mutex                             g_fault_mutex;
std::unordered_map<uint64_t, uint64_t> g_fault_pages;  // 4 KiB page -> faults this second
std::unordered_map<uint64_t, uint64_t> g_fault_chunks; // 2 MiB chunk -> faults this second

void ReportFaultChunks() {
	std::unordered_map<uint64_t, uint64_t> pages;
	std::unordered_map<uint64_t, uint64_t> chunks;
	{
		std::scoped_lock lock(g_fault_mutex);
		pages.swap(g_fault_pages);
		chunks.swap(g_fault_chunks);
	}
	std::vector<std::pair<uint64_t, uint64_t>> sorted(chunks.begin(), chunks.end());
	std::sort(sorted.begin(), sorted.end(),
	          [](const auto& a, const auto& b) { return a.second > b.second; });
	uint64_t total = 0;
	for (const auto& [chunk, count]: sorted) {
		total += count;
	}
	uint64_t covered = 0;
	size_t   chunks90 = 0;
	while (chunks90 < sorted.size() && covered * 10 < total * 9) {
		covered += sorted[chunks90++].second;
	}
	std::printf(" | WFAULTS faults=%llu pages=%zu chunks=%zu chunks_for_90%%=%zu top:",
	            static_cast<unsigned long long>(total), pages.size(), sorted.size(), chunks90);
	for (size_t i = 0; i < std::min<size_t>(sorted.size(), 10); i++) {
		std::printf(" %llx=%llu", static_cast<unsigned long long>(sorted[i].first << 21u),
		            static_cast<unsigned long long>(sorted[i].second));
	}
}
} // namespace

void NoteWriteFault(uint64_t vaddr) noexcept {
	std::scoped_lock lock(g_fault_mutex);
	g_fault_pages[vaddr >> 12u]++;
	g_fault_chunks[vaddr >> 21u]++;
}

namespace {
struct StreamSeen {
	uint64_t label_epoch;
	uint64_t submit_epoch;
	uint64_t hash;
};
// GPU thread only.
void NoteStreamCopy(uint64_t vaddr, uint64_t size, const void* bytes) {
	static std::unordered_map<uint64_t, StreamSeen> seen;
	if (seen.size() > (1u << 20u)) {
		seen.clear();
	}
	const auto label  = g_label_epoch.load(std::memory_order_relaxed);
	const auto submit = g_submit_epoch.load(std::memory_order_relaxed);
	const auto hash   = XXH3_64bits(bytes, size);
	const auto [it, inserted] =
	    seen.try_emplace(vaddr ^ (size << 44u), StreamSeen {label, submit, hash});
	if (!inserted) {
		const bool same = it->second.hash == hash;
		if (it->second.label_epoch == label) {
			Add(same ? StreamRepeatSame : StreamRepeatChanged);
		}
		if (it->second.submit_epoch == submit) {
			Add(same ? StreamRepeatSubmitSame : StreamRepeatSubmitChanged);
		}
		if (same) {
			Add(StreamRepeatAnySame);
		}
		it->second = {label, submit, hash};
	}
	Add(StreamCalls);
	Add(StreamBytes, size);
}
} // namespace

} // namespace Libs::Graphics::MemDiag

namespace Libs::Graphics {

namespace {

thread_local MemDiag::Sync g_sync_kind = MemDiag::Sync::Obtain;

size_t StreamCopySlot(uint64_t vaddr, uint64_t size) {
	const uint64_t key = (vaddr ^ (size << 40u)) * 0x9e3779b97f4a7c15ull;
	return static_cast<size_t>(key >> 51u); // 13 bits: STREAM_COPY_SLOTS
}

bool AsyncUploadVerifyEnabled() {
	static const bool on = std::getenv("KYTY_ASYNC_UPLOAD_VERIFY") != nullptr;
	return on;
}

// KYTY_STREAM_VERIFY=1: compare every reused stream copy against the guest bytes.
bool StreamVerifyEnabled() {
	static const bool on = std::getenv("KYTY_STREAM_VERIFY") != nullptr;
	return on;
}

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;
// GPU-modified ranges come as many small scattered islands, so a readback widens to a window.
constexpr uint64_t DownloadWindowSize = 512 * 1024;

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		m_buffers.emplace(buffer.CpuAddress(), id);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		m_buffers.erase(m_buffers.find(buffer.CpuAddress()));
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
}

void BufferCache::DeleteBuffer(BufferId id) {
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                       std::shared_ptr<PendingRead> read) {
	EXIT_IF(!async && read != nullptr);
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(OwnedRanges(), address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	if (read != nullptr) {
		read->ranges.reserve(copies.size());
		for (const auto& copy: copies) {
			read->ranges.emplace_back(buffer_address + copy.srcOffset,
			                          buffer_address + copy.srcOffset + copy.size);
		}
	}
	auto publish = [this, mapped, offset, total_size, buffer_address, read = std::move(read),
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			// The pages stay GPU-owned, so read-watched, until after this store (Unmark or the
			// release command follows publication): a prepared draw that read these bytes
			// logged them, and DrawPrepare checks it by range.
			const auto address = buffer_address + copy.srcOffset;
			DrawPrepare::NoteStoredRange(address, copy.size);
			Libs::LibKernel::Memory::WriteBacking(address, mapped + (copy.dstOffset - offset),
			                                      copy.size, true);
			DrawPrepare::NoteStoredRange(address, copy.size);
		}
		if (read != nullptr) {
			read->published.store(true, std::memory_order_release);
			read->published.notify_all();
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

// KYTY_UPLOAD_COPY_THREADS=<n> helpers (default 2, 0: the caller copies alone).
class ParallelGuestCopy {
public:
	struct Piece {
		uint64_t address;
		uint8_t* destination;
		uint64_t size;
	};
	// Guest bytes [address, address + size) to place at destination + offset.
	struct Copy {
		uint64_t address;
		uint64_t size;
		uint64_t offset;
	};

	ParallelGuestCopy() {
		const char* value   = std::getenv("KYTY_UPLOAD_COPY_THREADS");
		const int   helpers = value != nullptr ? std::clamp(std::atoi(value), 0, 8) : 2;
		for (int i = 0; i < helpers; i++) {
			m_threads.emplace_back([this, i] { Loop(i); });
		}
	}
	~ParallelGuestCopy() {
		{
			std::scoped_lock lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		for (auto& thread: m_threads) {
			thread.join();
		}
	}
	KYTY_CLASS_NO_COPY(ParallelGuestCopy);

	// Copies [address, address + size) of every copy to destination + offset; returns when done.
	// One caller at a time.
	void Run(std::span<const Copy> copies, uint8_t* destination) {
		m_pieces.clear();
		for (const auto& copy: copies) {
			for (uint64_t done = 0; done < copy.size; done += ChunkSize) {
				m_pieces.push_back({copy.address + done, destination + copy.offset + done,
				                    std::min<uint64_t>(ChunkSize, copy.size - done)});
			}
		}
		if (m_threads.empty() || m_pieces.size() < 2) {
			for (const auto& piece: m_pieces) {
				CopyPiece(piece);
			}
			return;
		}
		uint64_t generation = 0;
		{
			std::scoped_lock lock(m_mutex);
			generation = ++m_generation;
			m_count    = m_pieces.size();
			m_done.store(0, std::memory_order_relaxed);
			m_ticket.store(generation << 32u, std::memory_order_release);
		}
		m_cv.notify_all();
		Work(generation, m_pieces.size());
		while (m_done.load(std::memory_order_acquire) != m_pieces.size()) {
			_mm_pause();
		}
	}

private:
	static constexpr uint64_t ChunkSize = 256 * 1024;

	static void CopyPiece(const Piece& piece) {
		if (!LibKernel::Memory::CopySparseMemory(piece.address, piece.destination, piece.size)) {
			EXIT("BufferCache: failed to read sparse guest buffer memory\n");
		}
	}

	// Claims and copies pieces of this run; a ticket holds the run in its high half, so a late
	// helper never claims a piece of the next run.
	size_t Work(uint64_t generation, size_t count) {
		size_t copied = 0;
		for (;;) {
			auto ticket = m_ticket.load(std::memory_order_acquire);
			if ((ticket >> 32u) != generation || (ticket & 0xffffffffu) >= count) {
				return copied;
			}
			if (!m_ticket.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel)) {
				continue;
			}
			CopyPiece(m_pieces[ticket & 0xffffffffu]);
			copied++;
			m_done.fetch_add(1, std::memory_order_release);
		}
	}

	void Loop(int index) {
#if defined(__linux__)
		pthread_setname_np(pthread_self(), ("KytyUploadCopy" + std::to_string(index)).c_str());
		Common::HybridAffinity::ApplyToEmulatorThread();
#endif
		uint64_t seen = 0;
		for (;;) {
			uint64_t generation = 0;
			size_t   count      = 0;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [&] { return m_stop || m_generation != seen; });
				if (m_stop) {
					return;
				}
				seen       = m_generation;
				generation = m_generation;
				count      = m_count;
			}
			Work(generation, count);
		}
	}

	std::vector<std::thread>  m_threads;
	std::vector<Piece>        m_pieces; // read by helpers only while a run claims them
	std::mutex                m_mutex;
	std::condition_variable   m_cv;
	uint64_t                  m_generation = 0;
	size_t                    m_count      = 0;
	bool                      m_stop       = false;
	std::atomic<uint64_t>     m_ticket {0};
	std::atomic<size_t>       m_done {0};
};

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_async_uploader(graphics, scheduler),
      m_parallel_copy(std::make_unique<ParallelGuestCopy>()),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	// A guest thread can stop waiting for its read only by exiting; its download has landed.
	while (!m_pending_reads.empty() &&
	       m_pending_reads.front()->published.load(std::memory_order_acquire)) {
		const auto read = m_pending_reads.front(); // FinishRead drops the list's reference
		FinishRead(*read);
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
	QueueBdaSync(vaddr, size);
}

namespace {
bool SyncReadback() {
	static const bool sync = [] {
		const char* value = std::getenv("KYTY_SYNC_READBACK");
		return value != nullptr && value[0] != '0';
	}();
	return sync;
}

bool ReadbackVerify() {
	static const bool verify = std::getenv("KYTY_READBACK_VERIFY") != nullptr;
	return verify;
}

// The download window around a read: what one fault fetches.
std::pair<uint64_t, uint64_t> DownloadWindow(const Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto start = std::max(Common::AlignDown(vaddr, DownloadWindowSize), buffer.CpuAddress());
	const auto end   = std::min(std::max(start + DownloadWindowSize, vaddr + size),
	                            buffer.CpuAddress() + buffer.Size());
	return {start, end - start};
}

bool WindowsOverlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
	const auto a_begin = Common::AlignDown(a, TRACKER_PAGE_SIZE);
	const auto a_end   = Common::AlignUp(a + a_size, TRACKER_PAGE_SIZE);
	const auto b_begin = Common::AlignDown(b, TRACKER_PAGE_SIZE);
	const auto b_end   = Common::AlignUp(b + b_size, TRACKER_PAGE_SIZE);
	return a_begin < b_end && b_begin < a_end;
}
} // namespace

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
#if defined(__linux__)
	const bool diag       = FaultDiag();
	const bool gpu_thread = GuestGpu::IsGpuThread();
	const auto diag_start = std::chrono::steady_clock::now();
#endif
	bool downloaded = false;
	if (!GuestGpu::IsGpuThread() && !SyncReadback()) {
		downloaded = ReadMemoryAsync(vaddr, size, is_write);
	} else {
		m_scheduler.Context().GetGpu().SendCommandSync([&, this, vaddr, size, is_write] {
			if (is_write && !IsRegionRegistered(vaddr, size)) {
				return;
			}
			auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
			if (IsRegionGpuModified(vaddr, size)) {
				downloaded              = true;
				const auto [start, len] = DownloadWindow(buffer, vaddr, size);
				// Reads in flight publish first: their bytes are no longer GPU-dirty.
				RetireReads(start, len);
				if (DownloadBufferMemory<false>(buffer, start, len)) {
					m_memory_tracker.UnmarkRegionAsGpuModified(start, len);
				}
			}
			if (is_write) {
				m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
				QueueBdaSync(vaddr, size);
			}
		});
	}
#if defined(__linux__)
	if (diag) {
		ReportRead(vaddr, size, gpu_thread,
		           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
		                                                     diag_start)
		               .count(),
		           downloaded);
	}
#endif
}

bool BufferCache::ReadMemoryAsync(uint64_t vaddr, uint64_t size, bool is_write) {
	auto&                        gpu = m_scheduler.Context().GetGpu();
	std::shared_ptr<PendingRead> read;
	bool                         waited = false;
	for (;;) {
		// The GPU thread records (or joins) the download and moves on; this thread waits for it.
		gpu.SendCommandSync([&, this] {
			if (read != nullptr) {
				FinishRead(*read);
				read.reset();
			}
			if (is_write && !IsRegionRegistered(vaddr, size)) {
				return;
			}
			auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
			if (IsRegionGpuModified(vaddr, size)) {
				const auto [start, len] = DownloadWindow(buffer, vaddr, size);
				read                    = StartRead(buffer, start, len);
				if (read != nullptr) {
					return;
				}
			}
			if (is_write) {
				m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
				QueueBdaSync(vaddr, size);
			}
		});
		if (read == nullptr) {
			return waited;
		}
		waited = true;
		read->published.wait(false, std::memory_order_acquire);
	}
}

std::shared_ptr<BufferCache::PendingRead> BufferCache::StartRead(Buffer& buffer, uint64_t start,
                                                                 uint64_t size) {
	if (m_gpu_modified_ranges.Intersects(start, size)) {
		auto read   = std::make_shared<PendingRead>();
		read->start = start;
		read->size  = size;
		read->tick  = m_scheduler.CurrentTick();
		if (DownloadBufferMemory<true>(buffer, start, size, read)) {
			for (const auto& [begin, end]: read->ranges) {
				m_inflight_ranges.Add(begin, end - begin);
			}
			m_pending_reads.push_back(read);
			// Submit now: the guest waits for this tick, the GPU thread does not.
			m_scheduler.Flush();
			return read;
		}
	}
	// Nothing newer to download: share the latest read in flight over the window.
	for (auto it = m_pending_reads.rbegin(); it != m_pending_reads.rend(); ++it) {
		if (WindowsOverlap((*it)->start, (*it)->size, start, size)) {
			return *it;
		}
	}
	UnmarkCleanPages(start, size);
	return nullptr;
}

void BufferCache::FinishRead(PendingRead& read) {
	if (read.finished) {
		return;
	}
	EXIT_IF(!read.published.load(std::memory_order_acquire));
	read.finished = true;
	m_pending_reads.erase(std::find_if(m_pending_reads.begin(), m_pending_reads.end(),
	                                   [&](const auto& pending) { return pending.get() == &read; }));
	m_inflight_ranges = {};
	for (const auto& pending: m_pending_reads) {
		for (const auto& [begin, end]: pending->ranges) {
			m_inflight_ranges.Add(begin, end - begin);
		}
	}
	if (ReadbackVerify()) {
		VerifyRead(read);
	}
	UnmarkCleanPages(read.start, read.size);
}

void BufferCache::RetireReads(uint64_t start, uint64_t size) {
	uint64_t tick = 0;
	for (const auto& pending: m_pending_reads) {
		if (WindowsOverlap(pending->start, pending->size, start, size)) {
			tick = std::max(tick, pending->tick);
		}
	}
	if (tick == 0) {
		return;
	}
	m_scheduler.Wait(tick);
	m_scheduler.WaitPriorityOperations(tick);
	const auto reads = m_pending_reads;
	for (const auto& pending: reads) {
		if (WindowsOverlap(pending->start, pending->size, start, size)) {
			FinishRead(*pending);
		}
	}
}

void BufferCache::UnmarkCleanPages(uint64_t start, uint64_t size) {
	const auto begin = Common::AlignDown(start, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(start + size, TRACKER_PAGE_SIZE);
	std::vector<std::pair<uint64_t, uint64_t>> owned;
	const auto note = [&](uint64_t first, uint64_t last) {
		owned.emplace_back(Common::AlignDown(first, TRACKER_PAGE_SIZE),
		                   Common::AlignUp(last, TRACKER_PAGE_SIZE));
	};
	m_gpu_modified_ranges.ForEachInRange(begin, end - begin, note);
	m_inflight_ranges.ForEachInRange(begin, end - begin, note);
	std::sort(owned.begin(), owned.end());
	auto cursor = begin;
	for (const auto& [first, last]: owned) {
		if (first > cursor) {
			m_memory_tracker.UnmarkRegionAsGpuModified(cursor, first - cursor);
		}
		cursor = std::max(cursor, last);
	}
	if (cursor < end) {
		m_memory_tracker.UnmarkRegionAsGpuModified(cursor, end - cursor);
	}
}

// KYTY_READBACK_VERIFY=1: when a read's pages are about to lose GPU ownership and nothing newer
// covers its bytes, the cached buffer still holds what the read published: download those bytes
// again, synchronously, and compare them with the backing store.
void BufferCache::VerifyRead(const PendingRead& read) {
	static std::atomic<uint64_t> checks {0};
	static std::atomic<uint64_t> mismatches {0};
	for (const auto& [begin, end]: read.ranges) {
		const auto bytes = end - begin;
		if (m_gpu_modified_ranges.Intersects(begin, bytes) ||
		    m_inflight_ranges.Intersects(begin, bytes)) {
			continue;
		}
		const auto* owner = m_page_table.Find(begin >> PageTable::kPageBits);
		if (owner == nullptr || !*owner || IsBufferInvalid(*owner) ||
		    !m_slot_buffers[*owner].IsInBounds(begin, bytes)) {
			continue;
		}
		auto&  buffer = m_slot_buffers[*owner];
		Buffer staging(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		               vk::BufferUsageFlagBits::eTransferDst, bytes);
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto              native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
		                       nullptr);
		const vk::BufferCopy copy {buffer.Offset(begin), 0, bytes};
		native.copyBuffer(buffer.Handle(), staging.Handle(), 1, &copy);
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eHostRead;
		after.buffer        = staging.Handle();
		after.size          = bytes;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &after, 0,
		                       nullptr);
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		staging.Invalidate(0, bytes);
		std::vector<uint8_t> backing(bytes);
		const bool read_ok = LibKernel::Memory::TryReadBacking(begin, backing.data(), bytes);
		const auto n       = checks.fetch_add(1) + 1;
		if (!read_ok || std::memcmp(backing.data(), staging.Mapped().data(), bytes) != 0) {
			const auto bad = mismatches.fetch_add(1) + 1;
			std::printf("READBACK_VERIFY mismatch addr=0x%016" PRIx64 " size=%" PRIu64
			            " read_ok=%d (%" PRIu64 " of %" PRIu64 ")\n",
			            begin, bytes, read_ok ? 1 : 0, bad, n);
		} else if ((n % 1000) == 0) {
			std::printf("READBACK_VERIFY checks=%" PRIu64 " mismatches=%" PRIu64 "\n", n,
			            mismatches.load());
		}
		std::fflush(stdout);
	}
}

const RangeSet& BufferCache::OwnedRanges() {
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	if (!m_inflight_ranges.Empty()) {
		m_owned_scratch = m_gpu_modified_ranges;
		m_inflight_ranges.ForEach(
		    [this](uint64_t begin, uint64_t end) { m_owned_scratch.Add(begin, end - begin); });
		return m_owned_scratch;
	}
#endif
	return m_gpu_modified_ranges;
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (MemDiag::Enabled()) {
		MemDiag::Add(MemDiag::SyncCalls0, g_sync_kind, 1);
		if (source) {
			MemDiag::Add(MemDiag::SyncUploads0, g_sync_kind, 1);
			MemDiag::Add(MemDiag::SyncBytes0, g_sync_kind, total_size);
			MemDiag::Add(MemDiag::SyncRanges0, g_sync_kind, copies.size());
		}
	}
#if defined(__linux__)
	if (FaultDiag()) {
		struct Stat {
			uint64_t calls, uploads, bytes, ranges, written;
		};
		static std::unordered_map<uint64_t, Stat> stats;
		static auto                               last = std::chrono::steady_clock::now();
		auto& st = stats[CallerOffset(__builtin_return_address(0))];
		st.calls++;
		st.written += is_written ? 1 : 0;
		if (source) {
			st.uploads++;
			st.bytes += total_size;
			st.ranges += copies.size();
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - last >= std::chrono::seconds(1)) {
			last = now;
			for (const auto& [caller, v]: stats) {
				std::printf("SYNCDIAG caller=0x%llx calls=%llu written=%llu uploads=%llu bytes=%llu "
				            "ranges=%llu\n",
				            static_cast<unsigned long long>(caller),
				            static_cast<unsigned long long>(v.calls),
				            static_cast<unsigned long long>(v.written),
				            static_cast<unsigned long long>(v.uploads),
				            static_cast<unsigned long long>(v.bytes),
				            static_cast<unsigned long long>(v.ranges));
			}
			std::fflush(stdout);
			stats.clear();
		}
	}
#endif
	if (source && m_upload_batch.active && !AsyncUploadVerifyEnabled()) {
		// One barrier pair for the whole batch: the global barriers cover every destination.
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		if (!m_upload_batch.open) {
			m_upload_batch.open = true;
			const vk::MemoryBarrier before {
			    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
			                     vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
			    .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
			native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
			                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 1,
			                       &before, 0, nullptr, 0, nullptr);
		}
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
	} else if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
		if (m_last_upload_async && AsyncUploadVerifyEnabled()) {
			VerifyAsyncUpload(buffer, copies, total_size);
		}
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	// Fills the staging bytes; copy source offsets are still relative to their start.
	const auto write = [&](uint8_t* mapped) {
		for (const auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset, copy.size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
		}
	};
	vk::Buffer source;
	uint64_t   base_offset = 0;
	m_last_upload_async    = false;
	if (m_async_uploader.Enabled() && total_size >= m_async_uploader.MinSize()) {
		std::tie(source, base_offset) = m_async_uploader.Upload(total_size, [&](uint8_t* mapped) {
			thread_local std::vector<ParallelGuestCopy::Copy> guest;
			guest.clear();
			for (const auto& copy: copies) {
				guest.push_back({buffer.CpuAddress() + copy.dstOffset, copy.size, copy.srcOffset});
			}
			m_parallel_copy->Run(guest, mapped);
			if (AsyncUploadVerifyEnabled()) {
				m_async_verify_bytes.assign(mapped, mapped + total_size);
			}
		});
		m_last_upload_async = static_cast<bool>(source);
	}
	if (!source) {
		const auto staged = UploadMemory(m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
		    vk::BufferUsageFlagBits::eTransferSrc,
		    [&](uint8_t* mapped, uint64_t) { write(mapped); });
		source      = staged.first->Handle();
		base_offset = staged.second;
	}
	for (auto& copy: copies) {
		copy.srcOffset += base_offset;
	}
	return source;
}

void BufferCache::VerifyAsyncUpload(const Buffer& buffer, std::span<const vk::BufferCopy> copies,
                                    uint64_t total_size) {
	struct Totals {
		std::atomic<uint64_t> uploads {0}, bytes {0}, mismatches {0};
	};
	static Totals totals;
	auto readback = std::make_shared<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
	                                         vk::BufferUsageFlagBits::eTransferDst, total_size);
	std::vector<vk::BufferCopy> back;
	back.reserve(copies.size());
	for (const auto& copy: copies) {
		back.emplace_back(copy.dstOffset, copy.srcOffset - copies[0].srcOffset, copy.size);
	}
	const auto native = m_scheduler.Current().Handle();
	native.copyBuffer(buffer.Handle(), readback->Handle(), static_cast<uint32_t>(back.size()),
	                  back.data());
	const vk::MemoryBarrier to_host {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                                 .dstAccessMask = vk::AccessFlagBits::eHostRead};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                       vk::DependencyFlags {}, 1, &to_host, 0, nullptr, 0, nullptr);
	m_scheduler.DeferOperation([readback, expected = std::move(m_async_verify_bytes),
	                            address = buffer.CpuAddress(), size = total_size,
	                            first = copies[0].dstOffset]() mutable {
		readback->Invalidate(0, size);
		const bool same = std::memcmp(readback->Mapped().data(), expected.data(), size) == 0;
		const auto n    = totals.uploads.fetch_add(1) + 1;
		totals.bytes.fetch_add(size);
		if (!same) {
			totals.mismatches.fetch_add(1);
			std::printf("ASYNCVERIFY mismatch buffer=0x%llx first=0x%llx size=0x%llx\n",
			            static_cast<unsigned long long>(address),
			            static_cast<unsigned long long>(address + first),
			            static_cast<unsigned long long>(size));
		}
		if (n % 256 == 0 || !same) {
			std::printf("ASYNCVERIFY checked=%llu bytes=%llu mismatches=%llu\n",
			            static_cast<unsigned long long>(n),
			            static_cast<unsigned long long>(totals.bytes.load()),
			            static_cast<unsigned long long>(totals.mismatches.load()));
			std::fflush(stdout);
		}
	});
	m_async_verify_bytes = {};
}

bool BufferCache::VerifyStreamHit(uint64_t vaddr, uint64_t size, uint64_t offset) {
	static thread_local uint8_t current[CACHING_PAGESIZE];
	if (!LibKernel::Memory::CopySparseMemory(vaddr, current, size)) {
		EXIT("BufferCache: failed to read sparse guest buffer memory\n");
	}
	// The stream mapping is host-visible; reading it is slow but this is a debug mode.
	if (std::memcmp(current, m_stream_buffer.Mapped().data() + offset, size) == 0) {
		return true;
	}
	static std::atomic<uint64_t> failures {0};
	const auto                   n = failures.fetch_add(1, std::memory_order_relaxed);
	if (n < 64 || (n % 1000) == 0) {
		std::printf("STREAMVERIFY mismatch n=%llu addr=0x%llx size=%llu epoch=%llu tick=%llu\n",
		            static_cast<unsigned long long>(n), static_cast<unsigned long long>(vaddr),
		            static_cast<unsigned long long>(size),
		            static_cast<unsigned long long>(GuestSyncEpoch::Current()),
		            static_cast<unsigned long long>(m_scheduler.CurrentTick()));
		std::fflush(stdout);
	}
	MemDiag::Add(MemDiag::StreamVerifyFailed);
	return false;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	EXIT_IF(!(GuestRange {vaddr, size}.Valid()));
	if (MemDiag::Enabled()) {
		MemDiag::MaybePrint();
		MemDiag::Add(MemDiag::ObtainCalls);
		if (is_written) {
			MemDiag::Add(MemDiag::ObtainWritten);
		} else if (size > CACHING_PAGESIZE) {
			MemDiag::Add(MemDiag::StreamMissLarge);
		} else if (m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
			MemDiag::Add(MemDiag::StreamMissGpu);
		} else if (!m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
			MemDiag::Add(MemDiag::StreamMissClean);
		}
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		static const bool reuse_disabled = std::getenv("KYTY_NO_STREAM_REUSE") != nullptr;
		const auto        epoch          = GuestSyncEpoch::Current();
		const auto        tick           = m_scheduler.CurrentTick();
		auto&             copy           = (*m_stream_copies)[StreamCopySlot(vaddr, size)];
		if (!reuse_disabled && copy.vaddr == vaddr && copy.size == size) {
			if (copy.epoch == epoch && copy.tick == tick && copy.lap == m_stream_buffer.Laps()) {
				if (!StreamVerifyEnabled() || VerifyStreamHit(vaddr, size, copy.offset)) {
					MemDiag::Add(MemDiag::StreamReuse);
					return {&m_stream_buffer, copy.offset};
				}
			} else {
				MemDiag::Add(copy.epoch == epoch ? MemDiag::StreamReuseOldTick
				                                 : MemDiag::StreamReuseOldEpoch);
			}
		}
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			// Map can wrap the ring, so read the lap after it.
			copy = {vaddr, size, offset, tick, epoch, m_stream_buffer.Laps()};
			if (MemDiag::Enabled()) {
				// Hash a cached copy, not the (possibly write-combined) stream mapping.
				static thread_local uint8_t snapshot[CACHING_PAGESIZE];
				(void)LibKernel::Memory::CopySparseMemory(vaddr, snapshot, size);
				MemDiag::NoteStreamCopy(vaddr, size, snapshot);
			}
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
#if defined(__linux__)
		if (FaultDiag()) {
			NoteWritten(vaddr, size, __builtin_return_address(0), nullptr);
		}
#endif
		if (!m_gpu_modified_ranges.Contains(vaddr, size)) {
			// A draw prepared on the worker may have read these bytes before they became
			// GPU-written.
			DrawPrepare::NoteHazard();
		}
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			g_sync_kind = MemDiag::Sync::Image;
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			g_sync_kind = MemDiag::Sync::Obtain;
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		m_scheduler.Context().NoteGpuWrite(vaddr, size);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		m_scheduler.Context().NoteGpuWrite(dst_vaddr, size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

void BufferCache::MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0) {
			m_checked_color_metadata[slot].Add(vaddr, size);
		}
	}
}

bool BufferCache::IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0 && !m_checked_color_metadata[slot].Contains(vaddr, size)) {
			return false;
		}
	}
	return true;
}

void BufferCache::ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size) {
	for (auto& checked: m_checked_color_metadata) {
		checked.Subtract(vaddr, size);
	}
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	// Bytes of a read in flight are not in the backing store yet.
	return m_gpu_modified_ranges.Intersects(vaddr, size) ||
	       (!m_inflight_ranges.Empty() && m_inflight_ranges.Intersects(vaddr, size));
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.ValidateGpuDirtyOwnership(OwnedRanges(), buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			// Its pages may hold only bytes of reads in flight: those publish by the same wait.
			EXIT_NOT_IMPLEMENTED(
			    !DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()) &&
			    !m_inflight_ranges.Intersects(buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		RetireReads(buffer.CpuAddress(), buffer.Size());
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::QueueBdaSync(uint64_t vaddr, uint64_t size) {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	std::lock_guard lock(m_bda_sync_mutex);
	m_bda_sync.Add(begin, end - begin);
}

RangeSet BufferCache::TakeBdaSync() {
	std::lock_guard lock(m_bda_sync_mutex);
	return std::exchange(m_bda_sync, {});
}

void BufferCache::BeginUploadBatch() {
	static const bool enabled = [] {
		const char* env = std::getenv("KYTY_UPLOAD_BATCH");
		return env == nullptr || std::strcmp(env, "0") != 0;
	}();
	EXIT_IF(m_upload_batch.active);
	m_upload_batch = {enabled, false};
}

void BufferCache::EndUploadBatch() {
	if (m_upload_batch.open) {
		const vk::MemoryBarrier after {
		    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
		    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite};
		auto& command = m_scheduler.Current();
		command.EndRendering();
		command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                                 vk::PipelineStageFlagBits::eAllCommands,
		                                 vk::DependencyFlags {}, 1, &after, 0, nullptr, 0, nullptr);
	}
	m_upload_batch = {};
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			g_sync_kind = MemDiag::Sync::Bda;
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
			g_sync_kind = MemDiag::Sync::Obtain;
		}
	}
}

} // namespace Libs::Graphics
