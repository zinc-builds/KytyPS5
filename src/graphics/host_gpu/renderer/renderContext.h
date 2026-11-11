#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/kernelStats.h"
#include "common/threads.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
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
	void               CollectAsyncWrites();
	// Where the guest may have published data mid-stream (a satisfied wait), the collection
	// can wait until the next packet that reads guest data: see CollectAsyncWritesIfRequested.
	void RequestAsyncCollect() noexcept { m_async_collect_requested = true; }
	[[nodiscard]] bool AsyncCollectRequested() const noexcept { return m_async_collect_requested; }
	void CollectAsyncWritesIfRequested() {
		if (m_async_collect_requested) {
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
	bool                      m_async_collect_requested = false;
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
