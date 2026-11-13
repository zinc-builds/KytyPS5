#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/kernelStats.h"
#include "common/threads.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/decodeHandoff.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "kernel/eventQueue.h"

#include <atomic>
#include <mutex>
#include <array>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "kernel/memory.h"

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

// KYTY_SUBMIT_SCAN: 0 off; unset/1 on; stats adds SUBMITSCAN lines (scan time per submitting
// thread); verify also scans on the GPU thread at the submission's start and counts the
// write-watched pages that scan would add (stores between submit and processing).
enum class SubmitScanMode { Off, On, Stats, Verify };
SubmitScanMode GetSubmitScanMode();
// KYTY_RESUME_SCAN: unset: a resumed submission whose suspending packet collects when it is
// satisfied (WAIT_REG_MEM, MEM_SEMAPHORE wait, REWIND) does not scan at its start; 1 scans
// as before; verify scans too and counts the write-watched pages the skip defers.
enum class ResumeScanMode { Skip, Scan, Verify };
ResumeScanMode GetResumeScanMode();

class GuestGpu;

// The render context's lock. The GPU command thread takes it for every draw and dispatch, the
// window and RenderDoc threads only at shutdown and capture boundaries. The command thread's side
// is two plain stores: another thread announces itself, forces a memory barrier on every thread
// of the process (membarrier), and waits until the command thread is outside its section, which
// then takes the real mutex while the other thread holds it. Recursive, like Common::Mutex.
class RenderContextMutex {
public:
	RenderContextMutex();
	KYTY_CLASS_NO_COPY(RenderContextMutex);

	void Lock();
	void Unlock();

private:
	std::recursive_mutex m_mutex;
	bool                 m_biased = false; // the platform can force a barrier on other threads
	alignas(64) std::atomic<bool> m_owner_active {false};
	alignas(64) std::atomic<bool> m_external {false};
	uint32_t m_owner_depth    = 0; // command thread only
	bool     m_owner_mutex    = false;
	uint32_t m_external_depth = 0; // under m_mutex
};

class RenderContext {
public:
	explicit RenderContext(GraphicContext&              graphics,
	                       const std::filesystem::path& shader_store = ShaderStorePath());
	~RenderContext();
	KYTY_CLASS_NO_COPY(RenderContext);

	[[nodiscard]] GraphicContext&           GetGraphics() const noexcept { return m_graphics; }
	void                                    InitializeGpu(VideoOut::VideoOutDriver* video_out);
	void                                    ShutdownGpu();
	[[nodiscard]] GuestGpu&                 GetGpu() const;
	[[nodiscard]] VideoOut::VideoOutDriver& GetVideoOut() const;

	RenderContextMutex& GetMutex() { return m_mutex; }
	CommandScheduler&   GetCommandScheduler() { return m_command_scheduler; }
	PipelineCache&      GetPipelineCache() { return m_pipeline_cache; }
	DescriptorHeap&     GetDescriptorHeap() { return m_descriptor_heap; }
	ImageHeap&          GetImageHeap() { return m_image_heap; }
	SamplerCache&       GetSamplerCache() { return m_sampler_cache; }
	BufferCache&        GetBufferCache() { return m_buffer_cache; }
	TextureCache&       GetTextureCache() { return m_texture_cache; }
	RenderExecutor&     GetRenderExecutor() { return m_render_executor; }
	OcclusionQueries&   GetOcclusionQueries() { return m_occlusion_queries; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               PrepareBda();
	// Applies guest writes to asynchronously write-protected ranges as if they had faulted.
	// Call where the guest may have published new data: a submission, a satisfied wait.
	// kept_only: no scan of its own, for a submission whose submitting thread scanned
	// (ScanAsyncWritesAtSubmit); applies what that and other scans kept.
	// verify (KYTY_SUBMIT_SCAN=verify, KYTY_RESUME_SCAN=verify): with kept_only, also scan and
	// count the write-watched pages the skipped scan would have found.
	enum class KeptVerify { None, Submit, Resume };
	// resume: a kept-only collection because a resumed submission skips its scan (#103), not
	// because its submitting thread scanned (observe statistics only).
	void CollectAsyncWrites(bool kept_only = false, KeptVerify verify = KeptVerify::None,
	                        bool resume = false);
	// KYTY_COLLECT_OBSERVE=1 (or =count): checks that every guest write a publication (a
	// satisfied wait that requests a collection) can make visible is invalidated by the
	// collection that publication forces, without changing what production collects. At each
	// RequestAsyncCollect it records the watched pages with pending writes (page-table marks and
	// kept writes, read without consuming them); the forced collection must invalidate each one
	// still watched, or the run fails (=1 exits, =count counts). With
	// KYTY_COLLECT_OBSERVE_SKIPS=1 the scan skips (kept-only collections after a submit scan or
	// on a resume, and waits on the processor's own label) peek too, for statistics: what was
	// deferred there (slow: a peek at each of thousands of skips a second). KYTY_OBSERVE_DROP=<n> (test only)
	// makes production skip the invalidation of every n-th such page, which observe must catch.
	// Unlike KYTY_SUBMIT_SCAN=verify and KYTY_RESUME_SCAN=verify, which handle what the
	// skipped scan would have found (nothing is lost for good), this checks that the collection
	// a publication forces covers every write pending at it. It assumes publications are
	// classified correctly and that no guest-reading packet runs before that collection.
	[[nodiscard]] static bool ObserveCollectEnabled() noexcept;
	// A collection limited to the page-aligned [begin, end) (GPU thread, no tracker lock held):
	// the guest writes pending there (page-table marks, re-protecting the pages, and kept writes)
	// are taken and their write-watched pages invalidated, exactly as CollectAsyncWrites handles
	// them; writes elsewhere stay for the next collection. For a buffer sync about to take GPU
	// ownership of the range (Track W): a store before the take is invalidated (CPU-dirty, so the
	// sync uploads it); one after leaves a new mark for the next collection. False when nothing
	// was done (asynchronous protection off): the caller keeps its fallback.
	// KYTY_ASYNC_WP_VERIFY and KYTY_COLLECT_OBSERVE see the takes at the next collection.
	bool TakeAsyncWrites(uint64_t begin, uint64_t end);
	enum class ObserveSkipKind { Submit, Resume, CpLabel };
	void ObservePublication();
	void ObserveSkip(ObserveSkipKind kind);
	// KYTY_SUBMIT_SCAN (default on): a guest thread's scan before it enqueues a submission.
	// False when it did not scan (switched off, or no asynchronous protection).
	bool ScanAsyncWritesAtSubmit();
	// Where the guest may have published data mid-stream (a satisfied wait), the collection
	// can wait until the next packet that reads guest data: see CollectAsyncWritesIfRequested.
	// Set and cleared on the GPU thread, only inside a sync while the decode thread
	// (KYTY_DECODE_THREAD) is parked: its read is ordered by the sync's release/acquire. A setter
	// outside a sync (a guest command, say) would need a sync first.
	void RequestAsyncCollect() noexcept {
		EXIT_IF(g_on_decode_thread);
		if (ObserveCollectEnabled()) {
			ObservePublication();
		}
		m_async_collect_requested.store(true, std::memory_order_relaxed);
	}
	[[nodiscard]] bool AsyncCollectRequested() const noexcept {
		return m_async_collect_requested.load(std::memory_order_relaxed);
	}
	void CollectAsyncWritesIfRequested() {
		if (AsyncCollectRequested()) {
			CollectAsyncWrites();
		}
	}
	// The GPU thread stored into guest memory through its mapping (labels, CPU-side DMA).
	// Where that raises no write fault (asynchronous write protection), invalidate as one would.
	void NoteGpuWrite(uint64_t vaddr, uint64_t size) {
		VerifyNoteGpuWrite(vaddr, size);
		if (LibKernel::Memory::IsAsyncWriteTracked(vaddr, size)) {
			(void)InvalidateMemory(vaddr, size);
		}
	}
	static void VerifyNoteGpuWrite(uint64_t vaddr, uint64_t size);
	// A label the command processor stores (ReleaseMem, WriteData). Into an asynchronously
	// tracked page it re-protects the page, then stores through the backing, so the store leaves
	// no mark: while the page then shows no guest store, the label still holds the processor's
	// value (IsCpLabel). invalidate: drop cached copies, as the guest mapping's fault would.
	// False: store through the guest mapping instead (and the page's chunk is asked to become
	// asynchronous).
	bool StoreCpLabel(uint64_t vaddr, const void* value, uint64_t size, bool invalidate);
	// Whether the value at vaddr is the processor's own label store, untouched by the guest: a
	// wait it satisfies publishes no guest data, so needs no collection.
	template <typename T>
	[[nodiscard]] bool IsCpLabel(uint64_t vaddr, T value) const {
		EXIT_IF(g_on_decode_thread);
		const auto& label = m_cp_labels[(vaddr >> 2u) % m_cp_labels.size()];
		if (!(label.vaddr == vaddr && sizeof(T) <= label.size &&
		      static_cast<T>(label.value) == value &&
		      label.generation == LibKernel::Memory::GuestRemapGeneration())) {
			return false;
		}
		Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::LabelPeek);
		return LibKernel::Memory::AsyncGuestPageUntouched(vaddr & ~uint64_t {0xfff}, label.token);
	}
	void VerifyAsyncWrites(const std::vector<std::pair<uint64_t, uint64_t>>& written,
	                       bool before_handling);
	void               RunGarbageCollector();

	void AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void TriggerInterrupt(int event_id, uint32_t context_id);

private:
	struct InterruptEqRegistration {
		LibKernel::EventQueue::KernelEqueue eq       = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
		int                                 event_id = 0;
	};

	GraphicContext&           m_graphics;
	RenderContextMutex        m_mutex;
	RenderExecutor            m_render_executor;
	CommandScheduler          m_command_scheduler;
	DescriptorHeap            m_descriptor_heap;
	ImageHeapLayout           m_image_heap_layout; // before m_pipeline_cache (its warm-up)
	PipelineCache             m_pipeline_cache;
	SamplerCache              m_sampler_cache;
	PageManager               m_page_manager;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	ImageHeap                 m_image_heap;
	OcclusionQueries          m_occlusion_queries;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	std::unique_ptr<GuestGpu> m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;
	bool                      m_fault_process_pending = false;
	std::atomic<bool>         m_async_collect_requested {false};
	struct CpLabel {
		uint64_t vaddr      = 0;
		uint64_t value      = 0;
		uint64_t size       = 0;
		uint64_t generation = 0;
		uint64_t token      = 0; // LibKernel::Memory::ProtectAsyncGuestPage
	};
	std::array<CpLabel, 1024> m_cp_labels {};

	Common::Mutex                        m_interrupt_mutex;
	std::vector<InterruptEqRegistration> m_interrupt_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
