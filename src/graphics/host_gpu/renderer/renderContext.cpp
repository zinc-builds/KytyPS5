#include "graphics/host_gpu/renderer/renderContext.h"
#include "common/frameDiag.h"
#include "graphics/guest_gpu/frameSkip.h"
#include "graphics/host_gpu/renderer/drawPrepare.h"

#include "common/assert.h"
#include "common/kernelStats.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memDiag.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <xxhash.h>

#if defined(__linux__)
#include <linux/membarrier.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Libs::Graphics {

namespace {
RenderContext* g_shader_reader_context = nullptr;
void           NoteVerifyInvalidated(uint64_t vaddr, uint64_t size);
} // namespace

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics, true),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_image_heap_layout(graphics), m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager), m_occlusion_queries(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	g_shader_reader_context = this;
	ShaderSetGuestReader([](uint64_t vaddr, void* dst, uint64_t size) -> bool {
		auto* context = g_shader_reader_context;
		if (DrawPrepare::OnWorker()) {
			// The prepare worker never reads in place (a fault there would need the command
			// thread); whether the bytes were GPU-written is checked where the draw is adopted.
			DrawPrepare::NoteRead(vaddr, size);
			return LibKernel::Memory::CopySparseMemory(vaddr, dst, size);
		}
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
		    if (DrawPrepare::OnWorker()) {
			    DrawPrepare::NoteRead(vaddr, sizeof(*value));
			    return LibKernel::Memory::CopySparseMemory(vaddr, value, sizeof(*value))
			               ? SrtWalkerRead::Ok
			               : SrtWalkerRead::Failed;
		    }
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

RenderContextMutex::RenderContextMutex() {
#if defined(__linux__)
	m_biased = syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	m_biased = true;
#endif
	static const char* off = std::getenv("KYTY_RENDER_MUTEX_BIASED");
	if (off != nullptr && off[0] == '0') {
		m_biased = false;
	}
}

void RenderContextMutex::Lock() {
	if (!GuestGpu::IsGpuThread()) {
		m_mutex.lock();
		if (m_biased && m_external_depth++ == 0) {
			m_external.store(true, std::memory_order_relaxed);
			// Every thread's earlier stores are visible after this, so either the command thread
			// shows itself active below, or it sees m_external and takes m_mutex.
#if defined(__linux__)
			EXIT_IF(syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != 0);
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			FlushProcessWriteBuffers();
#endif
			while (m_owner_active.load(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#else
				std::this_thread::yield();
#endif
			}
		}
		return;
	}
	if (m_owner_depth++ != 0) {
		return;
	}
	if (m_biased) {
		m_owner_active.store(true, std::memory_order_relaxed);
		// Only the compiler must keep the store before the load: the other side's membarrier
		// orders them on the CPU.
		std::atomic_signal_fence(std::memory_order_seq_cst);
		if (!m_external.load(std::memory_order_acquire)) {
			return;
		}
		m_owner_active.store(false, std::memory_order_release);
	}
	if (Common::FrameDiag::Enabled()) {
		if (!m_mutex.try_lock()) {
			const auto start = Common::FrameDiag::NowNs();
			m_mutex.lock();
			Common::FrameDiag::Add(Common::FrameDiag::Counter::MutexWaits);
			Common::FrameDiag::Add(Common::FrameDiag::Counter::MutexWaitNs,
			                       Common::FrameDiag::NowNs() - start);
		}
	} else {
		m_mutex.lock();
	}
	m_owner_mutex = true;
}

void RenderContextMutex::Unlock() {
	if (!GuestGpu::IsGpuThread()) {
		if (m_biased && --m_external_depth == 0) {
			m_external.store(false, std::memory_order_release);
		}
		m_mutex.unlock();
		return;
	}
	if (--m_owner_depth != 0) {
		return;
	}
	if (m_owner_mutex) {
		m_owner_mutex = false;
		m_mutex.unlock();
		return;
	}
	m_owner_active.store(false, std::memory_order_release);
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
	// Its reads go through the backing view; serving a fault here would wait for the command
	// thread, which may be waiting for the worker.
	EXIT_IF(DrawPrepare::OnWorker());
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
		if (FrameSkip::DiagEnabled()) {
			FrameSkip::NoteCpuReadback(fault_vaddr);
		}
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
	m_buffer_cache.QueueBdaSync(vaddr, size, BufferCache::BdaSource::Map);
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
	// Every kept write this collection took must be among the writes it handled.
	std::vector<std::pair<uint64_t, uint64_t>> kept;
	LibKernel::Memory::TakeVerifyKeptWrites(kept);
	for (const auto& [begin, end]: kept) {
		for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
			if (inside(written, page)) {
				continue;
			}
			if (lost++ < 64) {
				std::printf("ASYNCWPVERIFY kept write not handled: page=0x%llx\n",
				            static_cast<unsigned long long>(page));
			}
		}
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

// Chunks without write watchers need no scan (their writes invalidate nothing). Read by guest
// threads too (submit scans): the page manager's region lookups and counters are atomic.
static std::atomic<const PageManager*> g_scan_pages {nullptr};

static bool WantsAsyncScan(uint64_t begin, uint64_t end) {
	return g_scan_pages.load(std::memory_order_acquire)->HasWriteWatched(begin, end);
}

ResumeScanMode GetResumeScanMode() {
	static const ResumeScanMode mode = [] {
		const char* value = std::getenv("KYTY_RESUME_SCAN");
		if (value == nullptr) {
			return ResumeScanMode::Skip;
		}
		const std::string_view v(value);
		return v == "1" ? ResumeScanMode::Scan
		       : v == "verify" ? ResumeScanMode::Verify
		                       : ResumeScanMode::Skip;
	}();
	return mode;
}

SubmitScanMode GetSubmitScanMode() {
	static const SubmitScanMode mode = [] {
		const char* value = std::getenv("KYTY_SUBMIT_SCAN");
		if (value == nullptr) {
			return SubmitScanMode::On;
		}
		const std::string_view v(value);
		return v == "0"        ? SubmitScanMode::Off
		       : v == "verify" ? SubmitScanMode::Verify
		       : v == "stats"  ? SubmitScanMode::Stats
		                       : SubmitScanMode::On;
	}();
	return mode;
}

namespace {
// KYTY_SUBMIT_SCAN=stats|verify: submit-scan time by submitting thread.
struct SubmitScanThread {
	char     name[16] {};
	uint64_t calls = 0;
	uint64_t ns    = 0;
};
std::mutex                      g_submit_stats_mutex;
std::array<SubmitScanThread, 8> g_submit_stats {};
uint64_t                        g_submit_deferred_pages = 0; // verify: watched pages P1 defers
uint64_t                        g_submit_verify_scans   = 0;
uint64_t                        g_resume_deferred_pages = 0; // KYTY_RESUME_SCAN=verify
uint64_t                        g_resume_verify_scans   = 0;
} // namespace

bool RenderContext::ScanAsyncWritesAtSubmit() {
	const auto mode = GetSubmitScanMode();
	if (mode == SubmitScanMode::Off) {
		return false;
	}
	const Common::FrameDiag::Scope frame_diag(Common::FrameDiag::Counter::SubmitScans,
	                                          Common::FrameDiag::Counter::SubmitScanNs);
	g_scan_pages.store(&m_page_manager, std::memory_order_release);
	if (mode == SubmitScanMode::On) {
		return LibKernel::Memory::ScanAsyncGuestWritesAtSubmit(WantsAsyncScan);
	}
	const auto start   = std::chrono::steady_clock::now();
	const bool scanned = LibKernel::Memory::ScanAsyncGuestWritesAtSubmit(WantsAsyncScan);
	const auto ns      = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                               std::chrono::steady_clock::now() - start)
	                                               .count());
	char       name[16] {};
#if defined(__linux__)
	pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
	std::lock_guard lock(g_submit_stats_mutex);
	for (auto& entry: g_submit_stats) {
		if (entry.calls == 0 || std::strncmp(entry.name, name, sizeof(name)) == 0) {
			std::memcpy(entry.name, name, sizeof(name));
			entry.calls++;
			entry.ns += ns;
			break;
		}
	}
	return scanned;
}

static void ReportSubmitScans() {
	if (GetSubmitScanMode() != SubmitScanMode::Stats &&
	    GetSubmitScanMode() != SubmitScanMode::Verify &&
	    GetResumeScanMode() != ResumeScanMode::Verify) {
		return;
	}
	static auto last = std::chrono::steady_clock::now();
	const auto  now  = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(1)) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - last).count();
	last                 = now;
	char line[1024];
	int  n = std::snprintf(line, sizeof(line),
	                       "SUBMITSCAN deferred_watched_pages_s=%.0f verify_scans_s=%.0f "
	                       "resume_deferred_watched_pages_s=%.0f resume_verify_scans_s=%.0f",
	                       static_cast<double>(g_submit_deferred_pages) / seconds,
	                       static_cast<double>(g_submit_verify_scans) / seconds,
	                       static_cast<double>(g_resume_deferred_pages) / seconds,
	                       static_cast<double>(g_resume_verify_scans) / seconds);
	g_submit_deferred_pages = 0;
	g_submit_verify_scans   = 0;
	g_resume_deferred_pages = 0;
	g_resume_verify_scans   = 0;
	std::lock_guard lock(g_submit_stats_mutex);
	for (auto& entry: g_submit_stats) {
		if (entry.calls != 0 && n < static_cast<int>(sizeof(line)) - 96) {
			n += std::snprintf(line + n, sizeof(line) - n, " %s=%.0f:%.2fms", entry.name,
			                   static_cast<double>(entry.calls) / seconds,
			                   static_cast<double>(entry.ns) / 1e6 / seconds);
		}
		entry = {};
	}
	std::printf("%s\n", line);
	std::fflush(stdout);
}

// KYTY_PRESTAGE_STATS=1: a PRESTAGE line per second (BDA pre-staging arms, refusals, pre-arm
// stores kept, versions moved by consumed marks, staged pages, remap generation moves).
static void ReportPrestage() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PRESTAGE_STATS");
		return value != nullptr && value[0] == '1';
	}();
	if (!enabled) {
		return;
	}
	static auto     last            = std::chrono::steady_clock::now();
	static uint64_t last_generation = LibKernel::Memory::TakePrestageCounters().remap_generation;
	const auto      now             = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(1)) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - last).count();
	last                 = now;
	const auto c         = LibKernel::Memory::TakePrestageCounters();
	std::printf("PRESTAGE arms_s=%.0f refused_async_s=%.0f refused_alias_s=%.0f "
	            "refused_private_s=%.0f prearm_pages_s=%.0f bumped_pages_s=%.0f staged_pages=%llu "
	            "remap_moves_s=%.1f\n",
	            static_cast<double>(c.arms) / seconds, static_cast<double>(c.refused_async) / seconds,
	            static_cast<double>(c.refused_alias) / seconds,
	            static_cast<double>(c.refused_private) / seconds,
	            static_cast<double>(c.prearm_pages) / seconds,
	            static_cast<double>(c.bumped_pages) / seconds,
	            static_cast<unsigned long long>(c.staged_pages),
	            static_cast<double>(c.remap_generation - last_generation) / seconds);
	last_generation = c.remap_generation;
	std::fflush(stdout);
}

// KYTY_COLLECT_OBSERVE (see renderContext.h). GPU thread only.
namespace {
int ObserveMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_COLLECT_OBSERVE");
		if (value == nullptr || value[0] == '\0' || value[0] == '0') {
			return 0;
		}
		return std::string_view(value) == "count" ? 2 : 1;
	}();
	return mode;
}
struct Observe {
	std::vector<uint64_t> pending; // watched pages with pending writes at a publication
	uint64_t publications = 0, pending_pages = 0, checked = 0, failures = 0, dropped = 0;
	uint64_t skips[3] {}, skip_pages[3] {};
	uint64_t peek_ns = 0;
	uint64_t last_ns = 0;
};
Observe g_observe;
uint64_t ObserveNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
} // namespace

bool RenderContext::ObserveCollectEnabled() noexcept {
	return ObserveMode() != 0;
}

// The watched pages whose guest writes no collection has taken yet: page-table marks and
// kept writes, read without consuming them.
static void ObservePeek(const PageManager& pages, std::vector<std::pair<uint64_t, uint64_t>>& out) {
	std::vector<std::pair<uint64_t, uint64_t>> written;
	// Only pages in write-protected views: a populated page elsewhere reads as written, but no
	// collection's protecting scan reports it (nor needs to: its watch is mprotect-based).
	LibKernel::Memory::PeekAsyncGuestWrites(written, true);
	for (const auto& [begin, end]: written) {
		pages.AppendWriteWatched(begin, end, out);
	}
}

void RenderContext::ObservePublication() {
	EXIT_IF(g_on_decode_thread);
	const auto start = ObserveNowNs();
	std::vector<std::pair<uint64_t, uint64_t>> watched;
	ObservePeek(m_page_manager, watched);
	for (const auto& [begin, end]: watched) {
		for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
			g_observe.pending.push_back(page);
		}
	}
	g_observe.publications++;
	g_observe.peek_ns += ObserveNowNs() - start;
}

void RenderContext::ObserveSkip(ObserveSkipKind kind) {
	// A peek at every skip costs ~60 us each (~7.5k own-label waits a second in Sky Garden: half
	// the command thread), so these statistics are opt-in: KYTY_COLLECT_OBSERVE_SKIPS=1.
	static const bool skips = [] {
		const char* value = std::getenv("KYTY_COLLECT_OBSERVE_SKIPS");
		return value != nullptr && value[0] == '1';
	}();
	if (!ObserveCollectEnabled() || !skips) {
		return;
	}
	const auto start = ObserveNowNs();
	std::vector<std::pair<uint64_t, uint64_t>> watched;
	ObservePeek(m_page_manager, watched);
	const auto k = static_cast<int>(kind);
	g_observe.skips[k]++;
	for (const auto& [begin, end]: watched) {
		g_observe.skip_pages[k] += (end - begin) / TRACKER_PAGE_SIZE;
	}
	g_observe.peek_ns += ObserveNowNs() - start;
}

// After a collection a publication forced: every pending page still watched must have been
// invalidated by it.
static void ObserveCheck(const PageManager& pages, const std::vector<uint64_t>& invalidated) {
	auto& o = g_observe;
	std::vector<uint64_t> done = invalidated;
	std::sort(done.begin(), done.end());
	std::sort(o.pending.begin(), o.pending.end());
	o.pending.erase(std::unique(o.pending.begin(), o.pending.end()), o.pending.end());
	for (const auto page: o.pending) {
		o.checked++;
		if (std::binary_search(done.begin(), done.end(), page)) {
			continue;
		}
		// No longer watched: the caches hold nothing from it to invalidate.
		if (!pages.HasWriteWatchedPage(page)) {
			continue;
		}
		o.failures++;
		std::printf("COLLECTOBSERVE FAIL: page 0x%llx had a guest write pending at a publication, "
		            "and the collection it forced did not invalidate it\n",
		            static_cast<unsigned long long>(page));
		std::fflush(stdout);
		if (ObserveMode() == 1) {
			EXIT("KYTY_COLLECT_OBSERVE: a published guest write was not invalidated by the "
			     "collection its publication forced (page 0x%llx)\n",
			     static_cast<unsigned long long>(page));
		}
	}
	o.pending_pages += o.pending.size();
	o.pending.clear();
}

static void ObserveReport() {
	auto&      o   = g_observe;
	const auto now = ObserveNowNs();
	if (o.last_ns == 0) {
		o.last_ns = now;
		return;
	}
	if (now - o.last_ns < 1'000'000'000ull) {
		return;
	}
	const double s = static_cast<double>(now - o.last_ns) / 1e9;
	std::printf("COLLECTOBSERVE publications_s=%.0f pending_pages_per=%.1f checked_s=%.0f failures=%llu "
	            "dropped=%llu skips_s(submit,resume,cplabel)=%.0f,%.0f,%.0f "
	            "deferred_pages_per_skip=%.1f,%.1f,%.1f peek_ms_s=%.2f\n",
	            o.publications / s,
	            o.publications != 0 ? static_cast<double>(o.pending_pages) / o.publications : 0.0,
	            o.checked / s, static_cast<unsigned long long>(o.failures),
	            static_cast<unsigned long long>(o.dropped), o.skips[0] / s, o.skips[1] / s,
	            o.skips[2] / s,
	            o.skips[0] != 0 ? static_cast<double>(o.skip_pages[0]) / o.skips[0] : 0.0,
	            o.skips[1] != 0 ? static_cast<double>(o.skip_pages[1]) / o.skips[1] : 0.0,
	            o.skips[2] != 0 ? static_cast<double>(o.skip_pages[2]) / o.skips[2] : 0.0,
	            static_cast<double>(o.peek_ns) / 1e6 / s);
	std::fflush(stdout);
	o.publications = o.pending_pages = o.checked = o.peek_ns = 0;
	for (int k = 0; k < 3; k++) {
		o.skips[k] = o.skip_pages[k] = 0;
	}
	o.last_ns = now;
}

void RenderContext::CollectAsyncWrites(bool kept_only, KeptVerify verify, bool resume) {
	EXIT_IF(g_on_decode_thread); // GPU thread only (Process start, sync prologues)
	ReportPrestage();
	const Common::FrameDiag::Scope frame_diag(Common::FrameDiag::Counter::Collects,
	                                          Common::FrameDiag::Counter::CollectNs);
	// A pending request (a satisfied wait published guest data, maybe in another submission
	// that is now blocked) needs a full scan: only a full scan settles it.
	const bool requested = AsyncCollectRequested();
	if (requested) {
		kept_only = false;
		verify    = KeptVerify::None;
	}
	m_async_collect_requested.store(false, std::memory_order_relaxed);
	const bool observe = ObserveCollectEnabled();
	if (observe) {
		ObserveReport();
	}
	// Pages the observer requires this collection to invalidate (a publication forced it).
	static std::vector<uint64_t> observed;
	observed.clear();
	static std::vector<std::pair<uint64_t, uint64_t>> written;
	static std::vector<std::pair<uint64_t, uint64_t>> watched;
	written.clear();
	g_scan_pages.store(&m_page_manager, std::memory_order_release);
	ReportSubmitScans();
	if (kept_only) {
		Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::CollectKept);
		LibKernel::Memory::TakeKeptAsyncGuestWrites(written);
		if (verify != KeptVerify::None) {
			// What the GPU thread's own scan would add here: watched pages written after the
			// submit scan, or while a resumed submission was blocked. The scan's finds are
			// handled too, as without the skip.
			const bool resume = verify == KeptVerify::Resume;
			std::vector<std::pair<uint64_t, uint64_t>> late;
			std::vector<std::pair<uint64_t, uint64_t>> late_watched;
			LibKernel::Memory::CollectAsyncGuestWrites(late, WantsAsyncScan);
			for (const auto& [begin, end]: late) {
				m_page_manager.AppendWriteWatched(begin, end, late_watched);
			}
			(resume ? g_resume_verify_scans : g_submit_verify_scans)++;
			for (const auto& [begin, end]: late_watched) {
				(resume ? g_resume_deferred_pages : g_submit_deferred_pages) +=
				    (end - begin) / TRACKER_PAGE_SIZE;
				static uint32_t logged = 0;
				if (logged < 64) {
					logged++;
					std::printf("SUBMITSCAN %s deferred: 0x%llx+0x%llx\n", resume ? "resume" : "submit",
					            static_cast<unsigned long long>(begin),
					            static_cast<unsigned long long>(end - begin));
				}
			}
			written.insert(written.end(), late.begin(), late.end());
		}
		if (observe) {
			// What this kept-only collection leaves for later (after the kept writes it takes).
			ObserveSkip(resume ? ObserveSkipKind::Resume : ObserveSkipKind::Submit);
		}
	} else {
		Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::CollectScan);
		LibKernel::Memory::CollectAsyncGuestWrites(written, WantsAsyncScan);
	}
	// KYTY_PRESTAGE_SELFTEST=1 (test only, no stager): stage up to 16 pages of the first written
	// run each collection, and unstage them all every 16th collection, to run the arm (including
	// re-arms of staged pages), consumption and unstage paths under KYTY_ASYNC_WP_VERIFY.
	static const bool selftest = [] {
		const char* value = std::getenv("KYTY_PRESTAGE_SELFTEST");
		return value != nullptr && value[0] == '1';
	}();
	if (selftest) {
		static std::vector<std::pair<uint64_t, uint64_t>> armed;
		static uint32_t                                   collections = 0;
		if (++collections % 16 == 0) {
			for (const auto& [begin, size]: armed) {
				LibKernel::Memory::Unstage(begin, size);
			}
			armed.clear();
		}
		if (!written.empty()) {
			const auto size = std::min<uint64_t>(written.front().second - written.front().first,
			                                     16 * TRACKER_PAGE_SIZE);
			if (LibKernel::Memory::ArmStaged(written.front().first, size)) {
				armed.emplace_back(written.front().first, size);
			}
		}
	}
	if (written.empty()) {
		VerifyAsyncWrites(written, true);
		VerifyAsyncWrites(written, false);
		if (observe && requested) {
			ObserveCheck(m_page_manager, observed);
		}
		return;
	}
	KYTY_PROFILER_BLOCK("CollectAsyncWrites");
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::CollectInvalidate);
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
	// KYTY_OBSERVE_DROP=<n> (test only, with KYTY_COLLECT_OBSERVE): this collection leaves every
	// n-th page a publication made visible stale, as a production bug would; observe must fail.
	static const uint64_t drop_every = [] {
		const char* value = std::getenv("KYTY_OBSERVE_DROP");
		return value != nullptr && ObserveCollectEnabled() ? std::strtoull(value, nullptr, 10) : 0;
	}();
	std::vector<uint64_t> drop;
	if (observe && requested && drop_every != 0) {
		static uint64_t counter = 0;
		for (const auto page: g_observe.pending) {
			if (++counter % drop_every == 0) {
				drop.push_back(page);
			}
		}
		std::sort(drop.begin(), drop.end());
		g_observe.dropped += drop.size();
	}
	for (const auto& [begin, end]: watched) {
		// The same work as a write fault on each page (HandleFault).
		MemDiag::Add(MemDiag::AsyncInvalidatedPages, (end - begin) / TRACKER_PAGE_SIZE);
		if (observe && requested) {
			for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
				if (std::binary_search(drop.begin(), drop.end(), page)) {
					continue;
				}
				observed.push_back(page);
			}
			if (!drop.empty()) {
				for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
					if (!std::binary_search(drop.begin(), drop.end(), page)) {
						(void)InvalidateMemory(page, TRACKER_PAGE_SIZE);
					}
				}
				continue;
			}
		}
		if (IsMapped(begin, end - begin)) {
			(void)InvalidateMemory(begin, end - begin);
			continue;
		}
		for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
			(void)InvalidateMemory(page, TRACKER_PAGE_SIZE);
		}
	}
	VerifyAsyncWrites(written, false);
	if (observe && requested) {
		ObserveCheck(m_page_manager, observed);
	}
}

bool RenderContext::StoreCpLabel(uint64_t vaddr, const void* value, uint64_t size,
                                 bool invalidate, uint32_t source) {
	EXIT_IF(g_on_decode_thread); // GPU thread only (sync packets)
	const auto page = vaddr & ~uint64_t {0xfff};
	if (size == 0 || size > sizeof(uint64_t) || ((vaddr + size - 1) & ~uint64_t {0xfff}) != page) {
		return false;
	}
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::LabelStore);
	// Read first: a remap after this read voids the label.
	const auto generation = LibKernel::Memory::GuestRemapGeneration();
	uint64_t   token      = 0;
	if (!LibKernel::Memory::ProtectAsyncGuestPage(page, token)) {
		LibKernel::Memory::RequestAsyncWriteTracking(vaddr);
		return false;
	}
	uint64_t stored = 0;
	std::memcpy(&stored, value, size);
	if (!LibKernel::Memory::TryStoreLabelBacking(vaddr, stored, size)) {
		return false;
	}
	if (invalidate) {
		(void)InvalidateMemory(vaddr, size);
	}
	if (!FillsLabelSlots(source)) {
		return true;
	}
	const auto bytes = static_cast<uint32_t>(size);
	m_cp_labels[(vaddr >> 2u) % m_cp_labels.size()] = {vaddr, stored, bytes, source, generation,
	                                                   token};
	if (size == sizeof(uint64_t)) {
		m_cp_labels[((vaddr + 4) >> 2u) % m_cp_labels.size()] = {vaddr + 4, stored >> 32u, 4,
		                                                           source, generation, token};
	}
	if (LabelHitStatsEnabled()) {
		NoteLabelStores(source, size == sizeof(uint64_t) ? 2 : 1);
	}
	return true;
}

namespace {

enum class WriteDataMode { PerDword, Ranged, Verify };

WriteDataMode ReadWriteDataMode() {
	const char* value = std::getenv("KYTY_WRITE_DATA_RANGED");
	if (value == nullptr) {
		return WriteDataMode::Ranged;
	}
	if (std::strcmp(value, "verify") == 0) {
		return WriteDataMode::Verify;
	}
	return value[0] == '0' ? WriteDataMode::PerDword : WriteDataMode::Ranged;
}

WriteDataMode GetWriteDataMode() {
	static const WriteDataMode mode = ReadWriteDataMode();
	return mode;
}

// KYTY_WRITE_DATA_RANGED=verify: what the comparisons found (GPU thread only).
struct WriteDataVerify {
	uint64_t runs        = 0;
	uint64_t elems       = 0;
	uint64_t raced       = 0; // the guest stored to the page meanwhile, or it was remapped
	uint64_t voided      = 0; // the per-dword store got a newer token: the ranged one was voided
	                          // meanwhile (another page took its token slot, or a collection
	                          // raised the floor); a voided token only fails IsCpLabel
	uint64_t ranged_inv  = 0; // bytes the ranged stores invalidated
	uint64_t last_report = 0;
};
WriteDataVerify g_wd_verify;

} // namespace

bool RenderContext::FillsLabelSlots(uint32_t source) noexcept {
	static const bool all = [] {
		const char* value = std::getenv("KYTY_WRITE_DATA_LABELS");
		return value != nullptr && std::strcmp(value, "all") == 0;
	}();
	return all || source <= kWriteDataLabelMaxDwords;
}

bool RenderContext::WriteDataRanged() noexcept {
	return GetWriteDataMode() != WriteDataMode::PerDword;
}

bool RenderContext::StoreCpLabels(uint64_t vaddr, const void* values, uint32_t elem, uint32_t count,
                                  uint32_t stride, bool invalidate, uint32_t source) {
	EXIT_IF(g_on_decode_thread); // GPU thread only (sync packets)
	if (count == 0 || (elem != 4 && elem != 8) || stride < elem) {
		return false;
	}
	const auto     page = vaddr & ~uint64_t {0xfff};
	const uint64_t span = uint64_t {count - 1} * stride + elem;
	if (((vaddr + span - 1) & ~uint64_t {0xfff}) != page) {
		return false;
	}
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::LabelStore);
	// Read first: a remap after this read voids the labels.
	const auto generation = LibKernel::Memory::GuestRemapGeneration();
	uint64_t   token      = 0;
	if (!LibKernel::Memory::ProtectAsyncGuestPage(page, token)) {
		LibKernel::Memory::RequestAsyncWriteTracking(vaddr);
		return false;
	}
	// All or nothing: it fails before storing anything.
	if (!LibKernel::Memory::TryStoreLabelsBacking(vaddr, values, elem, count, stride)) {
		return false;
	}
	uint64_t invalidated = 0;
	if (invalidate) {
		if (stride == elem) {
			(void)InvalidateMemory(vaddr, span);
			invalidated = span;
		} else {
			for (uint32_t i = 0; i < count; i++) {
				(void)InvalidateMemory(vaddr + uint64_t {i} * stride, elem);
				invalidated += elem;
			}
		}
	}
	const auto* bytes = static_cast<const uint8_t*>(values);
	for (uint32_t i = 0; FillsLabelSlots(source) && i < count; i++) {
		const uint64_t at     = vaddr + uint64_t {i} * stride;
		uint64_t       stored = 0;
		std::memcpy(&stored, bytes + uint64_t {i} * elem, elem);
		m_cp_labels[(at >> 2u) % m_cp_labels.size()] = {at, stored, elem, source, generation, token};
		if (elem == sizeof(uint64_t)) {
			m_cp_labels[((at + 4) >> 2u) % m_cp_labels.size()] = {at + 4, stored >> 32u, 4, source,
			                                                       generation, token};
		}
	}
	if (LabelHitStatsEnabled() && FillsLabelSlots(source)) {
		NoteLabelStores(source, count * (elem / 4));
	}
	if (GetWriteDataMode() == WriteDataMode::Verify) {
		VerifyCpLabels(vaddr, values, elem, count, stride, invalidate, token, invalidated, source,
		               generation);
	}
	return true;
}

// KYTY_WRITE_DATA_RANGED=verify, after StoreCpLabels stored [vaddr, ...): what the per-dword path
// would have done, without storing anything again (so a guest store since is never overwritten).
// For each value it takes the token StoreCpLabel would take (ProtectAsyncGuestPage, as that path
// calls it) and checks the ranged label slots against the slots StoreCpLabel would write: same
// address, value, size and remap generation, and the same token or an older one (the ranged token
// voided since: waiters on it collect, more conservative, never less). It checks that the guest
// reads each value back unless the guest stored to the page since (raced), and that the bytes
// invalidated are the values' bytes. A remap since counts as raced.
void RenderContext::VerifyCpLabels(uint64_t vaddr, const void* values, uint32_t elem,
                                   uint32_t count, uint32_t stride, bool invalidate,
                                   uint64_t token, uint64_t invalidated, uint32_t source,
                                   uint64_t generation) {
	const auto  page       = vaddr & ~uint64_t {0xfff};
	// Packets that leave the label slots alone (FillsLabelSlots) leave them alone on both paths.
	const bool  slots      = FillsLabelSlots(source);
	const auto* bytes      = static_cast<const uint8_t*>(values);
	auto&       v          = g_wd_verify;
	v.runs++;
	v.elems += count;
	if (LibKernel::Memory::GuestRemapGeneration() != generation) {
		v.raced++;
		return;
	}
	const auto fail = [&](const char* what, uint64_t at, const CpLabel& ranged, const CpLabel& dword) {
		EXIT("WDVERIFY FAIL (%s): 0x%016" PRIx64 " ranged label {0x%" PRIx64 " 0x%" PRIx64 " %" PRIu64
		     " gen %" PRIu64 " token %" PRIu64 "}, per-dword {0x%" PRIx64 " 0x%" PRIx64 " %" PRIu64
		     " gen %" PRIu64 " token %" PRIu64 "}\n",
		     what, at, ranged.vaddr, ranged.value, uint64_t {ranged.size}, ranged.generation, ranged.token,
		     dword.vaddr, dword.value, uint64_t {dword.size}, dword.generation, dword.token);
	};
	// The ranged token, or one it was voided for.
	const auto same = [&](const CpLabel& ranged, const CpLabel& dword) {
		if (ranged.vaddr != dword.vaddr || ranged.value != dword.value || ranged.size != dword.size ||
		    ranged.generation != dword.generation || dword.token < ranged.token) {
			return false;
		}
		v.voided += dword.token > ranged.token ? 1 : 0;
		return true;
	};
	bool raced = false;
	for (uint32_t i = 0; i < count; i++) {
		if (LibKernel::Memory::GuestRemapGeneration() != generation) {
			raced = true; // the per-dword path would label the rest with the new generation
			break;
		}
		const uint64_t at       = vaddr + uint64_t {i} * stride;
		uint64_t       expected = 0;
		std::memcpy(&expected, bytes + uint64_t {i} * elem, elem);
		uint64_t dword_token = 0;
		if (!LibKernel::Memory::ProtectAsyncGuestPage(page, dword_token)) {
			if (LibKernel::Memory::GuestRemapGeneration() != generation) {
				raced = true;
				break;
			}
			fail("the per-dword protection fails", at, {}, {});
		}
		const CpLabel dword {at, expected, elem, source, generation, dword_token};
		const CpLabel dword_hi {at + 4, expected >> 32u, 4, source, generation, dword_token};
		const auto&   ranged    = m_cp_labels[(at >> 2u) % m_cp_labels.size()];
		const auto&   ranged_hi = m_cp_labels[((at + 4) >> 2u) % m_cp_labels.size()];
		if (slots && !same(ranged, dword)) {
			fail("label slot", at, ranged, dword);
		}
		if (slots && elem == 8 && !same(ranged_hi, dword_hi)) {
			fail("label slot, high dword", at + 4, ranged_hi, dword_hi);
		}
		uint64_t seen = 0;
		std::memcpy(&seen, reinterpret_cast<const void*>(at), elem);
		if (seen != expected) {
			if (!LibKernel::Memory::AsyncGuestPageUntouched(page, token)) {
				raced = true; // the guest stored to the page since
				continue;
			}
			EXIT("WDVERIFY FAIL: 0x%016" PRIx64 " guest reads 0x%" PRIx64 ", stored 0x%" PRIx64
			     " with the page untouched since\n",
			     at, seen, expected);
		}
	}
	v.raced += raced ? 1 : 0;
	const uint64_t dword_inv = invalidate ? uint64_t {count} * elem : 0;
	v.ranged_inv += invalidated;
	if (invalidated != dword_inv) {
		EXIT("WDVERIFY FAIL: the ranged store invalidated %" PRIu64 " bytes, per-dword %" PRIu64 "\n",
		     invalidated, dword_inv);
	}
	const auto now = Common::FrameDiag::NowNs();
	if (now - v.last_report >= 1'000'000'000ull) {
		std::printf("WDVERIFY runs=%" PRIu64 " elems=%" PRIu64 " raced=%" PRIu64 " voided=%" PRIu64
		            " inv_b=%" PRIu64 " fail=0\n",
		            v.runs, v.elems, v.raced, v.voided, v.ranged_inv);
		std::fflush(stdout);
		v.last_report = now;
	}
}

namespace {

// KYTY_LABEL_HIT_STATS: buckets by the WRITE_DATA packet's dwords (0: another label store).
constexpr int         kLabelBuckets                 = 7;
constexpr const char* kLabelBucketNames[kLabelBuckets] = {"other", "1", "2", "3-4", "5-16", "17-64", "65+"};
int LabelBucket(uint32_t source) {
	return source == 0 ? 0 : source == 1 ? 1 : source == 2 ? 2 : source <= 4 ? 3 : source <= 16 ? 4 : source <= 64 ? 5 : 6;
}
struct LabelHitStats {
	uint64_t checks[5][kLabelBuckets] {}; // by RenderContext::LabelCheck
	uint64_t slots[kLabelBuckets] {};
	uint64_t last_report = 0;
};
LabelHitStats g_label_hits; // GPU thread only

void ReportLabelHits() {
	auto&      s   = g_label_hits;
	const auto now = Common::FrameDiag::NowNs();
	if (now - s.last_report < 1'000'000'000ull) {
		return;
	}
	s.last_report = now;
	char line[1024];
	int  n = std::snprintf(line, sizeof(line), "LABELHIT (cumulative; per bucket slots/other_addr/mismatch/hit/stale/written)");
	for (int b = 0; b < kLabelBuckets && n < static_cast<int>(sizeof(line)) - 96; b++) {
		n += std::snprintf(line + n, sizeof(line) - n, " %s=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64,
		                   kLabelBucketNames[b], s.slots[b], s.checks[0][b], s.checks[1][b], s.checks[2][b],
		                   s.checks[3][b], s.checks[4][b]);
	}
	std::printf("%s\n", line);
	std::fflush(stdout);
}

} // namespace

void RenderContext::NoteLabelCheck(LabelCheck check, uint32_t source) noexcept {
	g_label_hits.checks[static_cast<int>(check)][LabelBucket(source)]++;
	ReportLabelHits();
}

void RenderContext::NoteLabelStores(uint32_t source, uint32_t slots) noexcept {
	g_label_hits.slots[LabelBucket(source)] += slots;
	ReportLabelHits();
}

void RenderContext::PrepareBda() {
	KYTY_PROFILER_FUNCTION();
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.BeginUploadBatch();
	m_buffer_cache.TakeBdaSync().ForEach([this](uint64_t begin, uint64_t end) {
		m_mapped_ranges.ForEachInRange(begin, end - begin, [this](uint64_t start, uint64_t finish) {
			m_buffer_cache.SynchronizeBuffersInRange(start, finish - start);
		});
	});
	m_buffer_cache.EndUploadBatch();
	if (MemDiag::BdaStatsMode() != 0) {
		m_buffer_cache.BdaStatsTick();
	}
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	const Common::FrameDiag::Scope frame_diag(Common::FrameDiag::Counter::GcRuns,
	                                          Common::FrameDiag::Counter::GcNs);
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
