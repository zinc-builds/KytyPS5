#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memDiag.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {
RenderContext* g_shader_reader_context = nullptr;
void           NoteVerifyInvalidated(uint64_t vaddr, uint64_t size);
} // namespace

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics, true),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager), m_occlusion_queries(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	g_shader_reader_context = this;
	ShaderSetGuestReader([](uint64_t vaddr, void* dst, uint64_t size) -> bool {
		auto* context = g_shader_reader_context;
		static const bool disabled = std::getenv("KYTY_NO_SHADER_FASTREAD") != nullptr;
		if (disabled || context == nullptr || !GuestGpu::IsGpuThread() ||
		    context->GetBufferCache().HasGpuDirtyBytes(vaddr, size)) {
			return false; // read in place: GPU-written bytes must be synchronized first
		}
		return LibKernel::Memory::CopySparseMemory(vaddr, dst, size);
	});
	const auto [watch_table, pages_offset] = m_page_manager.ReadWatchTable();
	using ShaderRecompiler::IR::SrtWalkerRead;
	ShaderRecompiler::IR::SetSrtWalkerGuestAccess(
	    watch_table, pages_offset, [](uint64_t vaddr, uint32_t* value) {
		    auto* context = g_shader_reader_context;
		    if (context != nullptr && GuestGpu::IsGpuThread() &&
		        context->GetBufferCache().HasGpuDirtyBytes(vaddr, sizeof(*value))) {
			    return SrtWalkerRead::GpuWritten;
		    }
		    return ShaderTryReadGuest(vaddr, value, sizeof(*value)) ? SrtWalkerRead::Ok
		                                                            : SrtWalkerRead::Failed;
	    });
}

RenderContext::~RenderContext() {
	ShaderRecompiler::IR::SetSrtWalkerGuestAccess(nullptr, 0, nullptr);
	ShaderSetGuestReader(nullptr);
	g_shader_reader_context = nullptr;
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size) ||
	    !m_page_manager.IsTrackingFault(fault_vaddr, access)) {
		return false;
	}
	if (MemDiag::Enabled()) {
		if (access == PageFaultAccess::Write) {
			MemDiag::NoteWriteFault(fault_vaddr);
		}
		const bool gpu = GuestGpu::IsGpuThread();
		MemDiag::Add(access == PageFaultAccess::Write
		                 ? (gpu ? MemDiag::WriteFaultsGpuThread : MemDiag::WriteFaultsOther)
		                 : (gpu ? MemDiag::ReadFaultsGpuThread : MemDiag::ReadFaultsOther));
	}
	if (MemDiag::Traced(fault_vaddr, 1)) {
		MemDiag::Trace(access == PageFaultAccess::Write ? "fault-write" : "fault-read", fault_vaddr, 1);
	}
	if (access == PageFaultAccess::Write) {
		NoteVerifyInvalidated(fault_vaddr & ~(TRACKER_PAGE_SIZE - 1), TRACKER_PAGE_SIZE);
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_image_heap.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

namespace {
// KYTY_ASYNC_WP_VERIFY: ranges invalidated between two scans; their snapshots are void.
std::mutex                                 g_verify_mutex;
std::unordered_map<uint64_t, uint64_t>     g_verify_gpu_writes;   // page -> count, all time
std::unordered_map<uint64_t, uint64_t>     g_verify_alias_writes; // page -> count, all time
std::vector<std::pair<uint64_t, uint64_t>> g_verify_invalidated;

void NoteVerifyInvalidated(uint64_t vaddr, uint64_t size) {
	if (MemDiag::Traced(vaddr, size)) {
		MemDiag::Trace("invalidate", vaddr, size);
	}
	static const bool verify = std::getenv("KYTY_ASYNC_WP_VERIFY") != nullptr;
	if (verify) {
		std::lock_guard lock(g_verify_mutex);
		g_verify_invalidated.emplace_back(vaddr, vaddr + size);
	}
}
} // namespace

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	NoteVerifyInvalidated(vaddr, size);
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_image_heap.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.QueueBdaSync(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

// KYTY_ASYNC_WP_VERIFY=1: every write-watched page of an asynchronous range must keep its
// bytes until a scan reports it written. Every 8th scan snapshots those pages after handling
// the scan; the next scan compares them before handling its own reports. Stores through the
// backing alias (downloads, zeroing) change bytes without a fault in any mode and are skipped.
void RenderContext::VerifyNoteGpuWrite(uint64_t vaddr, uint64_t size) {
	if (MemDiag::Traced(vaddr, size)) {
		MemDiag::Trace("gpu-thread-store", vaddr, size);
	}
	static const bool enabled = std::getenv("KYTY_ASYNC_WP_VERIFY") != nullptr;
	if (enabled && size != 0) {
		for (auto page = vaddr & ~(TRACKER_PAGE_SIZE - 1); page < vaddr + size; page += TRACKER_PAGE_SIZE) {
			g_verify_gpu_writes[page]++;
		}
	}
}

void RenderContext::VerifyAsyncWrites(const std::vector<std::pair<uint64_t, uint64_t>>& written,
                                      bool before_handling) {
	static const bool enabled = std::getenv("KYTY_ASYNC_WP_VERIFY") != nullptr;
	if (!enabled) {
		return;
	}
	static std::unordered_map<uint64_t, uint64_t> snapshots;
	static uint64_t                               scans   = 0;
	static uint64_t                               checked = 0;
	static uint64_t                               lost    = 0;
	const auto inside = [](const std::vector<std::pair<uint64_t, uint64_t>>& runs, uint64_t page) {
		return std::any_of(runs.begin(), runs.end(), [&](const auto& run) {
			return page + TRACKER_PAGE_SIZE > run.first && page < run.second;
		});
	};
	static std::array<uint8_t, TRACKER_PAGE_SIZE> bytes;
	// Read the pages before taking the logs: a store that changed the bytes read here was
	// logged (fault, invalidation, alias store) or marked written before the read.
	std::vector<std::pair<uint64_t, uint64_t>> changed; // page, current hash
	if (before_handling) {
		for (const auto& [page, hash]: snapshots) {
			if (LibKernel::Memory::TryReadBacking(page, bytes.data(), bytes.size())) {
				checked++;
				const auto now = XXH3_64bits(bytes.data(), bytes.size());
				if (MemDiag::Traced(page, TRACKER_PAGE_SIZE)) {
					MemDiag::Trace("compare", page, TRACKER_PAGE_SIZE, now);
				}
				if (now != hash) {
					changed.emplace_back(page, now);
				}
			}
		}
		snapshots.clear();
	}
	std::vector<std::pair<uint64_t, uint64_t>> alias;
	LibKernel::Memory::TakeAliasWrites(alias);
	for (const auto& [begin, end]: alias) {
		for (auto page = begin & ~(TRACKER_PAGE_SIZE - 1); page < end; page += TRACKER_PAGE_SIZE) {
			g_verify_alias_writes[page]++;
		}
	}
	std::vector<std::pair<uint64_t, uint64_t>> invalidated;
	{
		std::lock_guard lock(g_verify_mutex);
		invalidated.swap(g_verify_invalidated);
	}
	std::vector<uint64_t> unwatched_pages;
	PageManager::TakeVerifyUnwatched(unwatched_pages);
	for (const auto page: unwatched_pages) {
		invalidated.emplace_back(page, page + TRACKER_PAGE_SIZE);
	}
	if (before_handling) {
		std::vector<std::pair<uint64_t, uint64_t>> late;
		if (!changed.empty()) {
			LibKernel::Memory::PeekAsyncGuestWrites(late);
		}
		for (const auto& [page, now]: changed) {
			if (inside(written, page) || inside(alias, page) || inside(invalidated, page) ||
			    inside(late, page)) {
				continue;
			}
			if (lost++ < 64) {
				std::vector<std::pair<uint64_t, uint64_t>> still;
				m_page_manager.AppendWriteWatched(page, page + TRACKER_PAGE_SIZE, still);
				std::printf("ASYNCWPVERIFY lost write: page=0x%llx async=%d mapped=%d watched=%d "
				            "gpu_writes=%llu alias_writes=%llu\n",
				            static_cast<unsigned long long>(page),
				            LibKernel::Memory::IsAsyncWriteTracked(page, TRACKER_PAGE_SIZE) ? 1 : 0,
				            IsMapped(page, TRACKER_PAGE_SIZE) ? 1 : 0, still.empty() ? 0 : 1,
				            static_cast<unsigned long long>(g_verify_gpu_writes[page]),
				            static_cast<unsigned long long>(g_verify_alias_writes[page]));
			}
		}
		return;
	}
	if ((scans++ % 8) != 0) {
		return;
	}
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	std::vector<std::pair<uint64_t, uint64_t>> watched;
	LibKernel::Memory::AsyncGuestRanges(ranges);
	for (const auto& [begin, end]: ranges) {
		m_page_manager.AppendWriteWatched(begin, end, watched);
	}
	for (const auto& [begin, end]: watched) {
		for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
			if (LibKernel::Memory::TryReadBacking(page, bytes.data(), bytes.size())) {
				snapshots[page] = XXH3_64bits(bytes.data(), bytes.size());
				if (MemDiag::Traced(page, TRACKER_PAGE_SIZE)) {
					MemDiag::Trace("snapshot", page, TRACKER_PAGE_SIZE, snapshots[page]);
				}
			}
		}
	}
	static auto last = std::chrono::steady_clock::now();
	if (std::chrono::steady_clock::now() - last >= std::chrono::seconds(1)) {
		last = std::chrono::steady_clock::now();
		std::printf("ASYNCWPVERIFY checked=%llu lost=%llu watched_pages=%zu\n",
		            static_cast<unsigned long long>(checked), static_cast<unsigned long long>(lost),
		            snapshots.size());
		std::fflush(stdout);
	}
}

void RenderContext::CollectAsyncWrites() {
	m_async_collect_requested = false;
	static std::vector<std::pair<uint64_t, uint64_t>> written;
	static std::vector<std::pair<uint64_t, uint64_t>> watched;
	written.clear();
	// Chunks without write watchers need no scan (their writes invalidate nothing).
	static const PageManager* pages = nullptr;
	pages = &m_page_manager;
	LibKernel::Memory::CollectAsyncGuestWrites(written, [](uint64_t begin, uint64_t end) {
		return pages->HasWriteWatched(begin, end);
	});
	if (written.empty()) {
		VerifyAsyncWrites(written, true);
		VerifyAsyncWrites(written, false);
		return;
	}
	KYTY_PROFILER_BLOCK("CollectAsyncWrites");
	watched.clear();
	for (const auto& [begin, end]: written) {
		m_page_manager.AppendWriteWatched(begin, end, watched);
	}
	MemDiag::Add(MemDiag::AsyncWrittenPages, [&] {
		uint64_t pages = 0;
		for (const auto& [begin, end]: written) {
			pages += (end - begin) / TRACKER_PAGE_SIZE;
		}
		return pages;
	}());
	VerifyAsyncWrites(written, true);
	for (const auto& [begin, end]: watched) {
		// The same work as a write fault on each page (HandleFault).
		MemDiag::Add(MemDiag::AsyncInvalidatedPages, (end - begin) / TRACKER_PAGE_SIZE);
		if (IsMapped(begin, end - begin)) {
			(void)InvalidateMemory(begin, end - begin);
			continue;
		}
		for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
			(void)InvalidateMemory(page, TRACKER_PAGE_SIZE);
		}
	}
	VerifyAsyncWrites(written, false);
}

bool RenderContext::StoreCpLabel(uint64_t vaddr, const void* value, uint64_t size,
                                 bool invalidate) {
	const auto page = vaddr & ~uint64_t {0xfff};
	if (size == 0 || size > sizeof(uint64_t) || ((vaddr + size - 1) & ~uint64_t {0xfff}) != page) {
		return false;
	}
	// Read first: a remap after this read voids the label.
	const auto generation = LibKernel::Memory::GuestRemapGeneration();
	uint64_t   token      = 0;
	if (!LibKernel::Memory::ProtectAsyncGuestPage(page, token)) {
		LibKernel::Memory::RequestAsyncWriteTracking(vaddr);
		return false;
	}
	if (!LibKernel::Memory::TryWriteBacking(vaddr, value, size)) {
		return false;
	}
	if (invalidate) {
		(void)InvalidateMemory(vaddr, size);
	}
	uint64_t stored = 0;
	std::memcpy(&stored, value, size);
	m_cp_labels[(vaddr >> 2u) % m_cp_labels.size()] = {vaddr, stored, size, generation, token};
	if (size == sizeof(uint64_t)) {
		m_cp_labels[((vaddr + 4) >> 2u) % m_cp_labels.size()] = {vaddr + 4, stored >> 32u, 4,
		                                                           generation, token};
	}
	return true;
}

void RenderContext::PrepareBda() {
	KYTY_PROFILER_FUNCTION();
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.TakeBdaSync().ForEach([this](uint64_t begin, uint64_t end) {
		m_mapped_ranges.ForEachInRange(begin, end - begin, [this](uint64_t start, uint64_t finish) {
			m_buffer_cache.SynchronizeBuffersInRange(start, finish - start);
		});
	});
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_image_heap.RunGarbageCollector();
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
