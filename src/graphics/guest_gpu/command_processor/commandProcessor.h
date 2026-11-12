#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H

#include "common/assert.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func);

enum class Pm4ProcessResult { Complete, Blocked };

enum class ContextStateOperation : uint32_t {
	Clear     = 0,
	Push      = 1,
	Pop       = 2,
	PushClear = 3,
};

struct IndirectDrawRegisters {
	uint32_t vertex_offset;
	uint32_t instance_offset;
	uint32_t index_offset;
	uint32_t draw_index;
};

class Pm4Execution {
public:
	[[nodiscard]] bool MadeProgress() const noexcept { return m_made_progress; }
	// Suspended at a packet that, once satisfied, requests the collection its release needs
	// (KYTY_RESUME_SCAN).
	[[nodiscard]] bool ResumeCollects() const noexcept { return m_resume_collects; }

private:
	friend class CommandProcessor;
	friend class DecodeThread;

	struct BufferCursor {
		std::span<const uint32_t> commands;
		uint32_t                  offset_dw = 0;
	};

	std::vector<BufferCursor> m_buffer_stack;
	std::span<const uint32_t> m_next_buffer;
	bool                      m_chain         = false;
	bool                      m_suspended       = false;
	bool                      m_made_progress   = false;
	bool                      m_resume_collects = false;
};

class DecodeThread;
class GuestGpu;

class CommandProcessor {
public:
	CommandProcessor(RenderContext& renderer, int interrupt_event_id)
	    : m_renderer(renderer), m_interrupt_event_id(interrupt_event_id) {}
	~CommandProcessor();

	KYTY_CLASS_NO_COPY(CommandProcessor);

	void Reset();
	void ApplyContextStateOperation(ContextStateOperation operation);

	void            BufferInit();
	void            BufferFlush();
	void            BufferWait();
	HW::Context&    GetCtx() { return m_ctx; }
	HW::UserConfig& GetUcfg() { return m_ucfg; }
	HW::Shader&     GetShCtx() { return m_sh_ctx; }

	void SetIndexType(uint32_t index_type_and_size);
	void SetIndexBaseAddress(uint64_t index_base_addr);
	void SetIndexBufferSize(uint32_t index_buffer_size);
	void SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr);
	void SetDispatchIndirectArgsBaseAddress(uint64_t dispatch_indirect_args_base_addr);
	[[nodiscard]] uint64_t GetDispatchIndirectArgsBaseAddress() const {
		return m_dispatch_indirect_args_base_addr;
	}
	void SetNumInstances(uint32_t num_instances);
	void DrawIndex(DrawIndexArgs args);
	void DrawIndexOffset(uint32_t index_offset, uint32_t index_count);
	void DrawIndexAuto(DrawAutoArgs args);
	void DrawIndirect(uint32_t data_offset, IndirectDrawRegisters registers,
	                  uint32_t draw_initiator, bool indexed);
	void DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
	                       const volatile uint32_t* count_addr, uint32_t stride_in_bytes,
	                       IndirectDrawRegisters registers, uint32_t draw_initiator, bool indexed);
	void ReleaseMem(uint32_t data_sel, uint32_t interrupt_selector, void* address, uint64_t data,
	                uint32_t interrupt_context_id);
	void Flip(int handle, int index, int flip_mode, int64_t flip_arg);
	bool SkipDraw(bool indirect, bool indirect_count);
	void PrepareCpuFlip(uint64_t request_id);
	void DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z,
	                    uint32_t mode);
	void DispatchIndirect(uint64_t args_addr, uint32_t mode);
	void WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index);
	void ProcessPssr(const Pssr::Request& request);
	void TriggerEvent(uint32_t event_type, uint32_t event_index, uint64_t event_address = 0);

	void WaitForRewind(bool valid);

	template <typename T>
	void WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t wait_op);
	void MemSemaphore(uint64_t* counter, bool signal, bool write);
	void WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control);
	void WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes);
	// Stores `size` bytes of a label as RELEASE_MEM does: through the backing store beside
	// GPU-written bytes or in asynchronously tracked pages, else through the guest mapping. An
	// ordered queue item wraps it in DrawPrepare::RangedStore.
	void StoreLabel(void* address, const void* value, uint64_t size);
	void GetLodStats(volatile uint32_t* dst, uint32_t size, bool reset);
	// A packet handler stored into guest memory through its mapping (see RenderContext).
	void NoteGuestWrite(const volatile void* address, uint64_t size);
	void DmaData(uint8_t dst_sel, uint64_t dst_address_or_offset, uint8_t src_sel,
	             uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes);
	void SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
	                    const volatile void* address);
	[[nodiscard]] bool ShouldSkipPredicatedPackets() const { return m_predicate_skip; }

	Pm4ProcessResult Process(Pm4Execution& execution, std::span<const uint32_t> commands);
	void             ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain);

	[[nodiscard]] bool IsAsyncComputeQueue() const { return m_interrupt_event_id >= 0x20; }

private:
	friend class DecodeThread;
	// ProcessPm4, or with KYTY_DECODE_THREAD=1 the decode thread's loop.
	void RunPacketLoop(Pm4Execution& execution);
	void ProcessPm4(Pm4Execution& execution);
	// One packet as the loop sees it (ProcessPm4, and the decode thread's loop and its syncs).
	struct PacketView {
		const uint32_t* packet;
		uint32_t        header;
		uint32_t        opcode;
		uint32_t        remaining_dw;
		uint32_t        total_dw;
	};
	// The per-packet work before the handler: drains, collection, dispatch chain, epoch, atomic
	// batch, early submit.
	void RunPrologue(const PacketView& view, bool& first_packet);
	// The handler, then the cursor (chains, calls). False if the packet suspended the execution.
	bool RunHandler(Pm4Execution& execution, const PacketView& view);
	// KYTY_DECODE_THREAD: the packet loop on the decode thread, the GPU thread committing.
	void ProcessPm4Decode(Pm4Execution& execution);
	// Decode thread: has the GPU thread run `view` (or the guest commands, without a view) after
	// committing everything posted. True if the loop must stop (the packet suspended).
	bool DecodeSync(const PacketView* view);
	[[nodiscard]] static bool OnDecodeThread() noexcept;
	// resume_collects: the suspending packet runs again on resume and, when satisfied, requests
	// the collection its release needs (RequestAsyncCollect), so the resume needs no scan.
	void SuspendPm4(bool resume_collects = false);
	uint32_t            NumInstances();
	const void*         IndexAddress(uint32_t first_index) const;
	CommandScheduler&   GetScheduler() const { return m_renderer.GetCommandScheduler(); }
	CommandBuffer&      CurrentBuffer() { return GetScheduler().Current(); }

	RenderContext& m_renderer;
	bool           m_in_dispatch_packet = false; // ProcessPm4 runs a dispatch packet's handler
	struct DecodeThreadDeleter {
		void operator()(DecodeThread* decode) const;
	};
	std::unique_ptr<DecodeThread, DecodeThreadDeleter> m_decode;
	// Decode thread: the handler found the packet must run on the GPU thread, and did nothing.
	bool m_decode_bail = false;
	// KYTY_DECODE_THREAD=verify: a shadow processor replays the decode thread's packets on the GPU
	// thread; its draws are captured instead of queued.
	struct ShadowDraw {
		bool          valid   = false;
		bool          indexed = false;
		DrawIndexArgs index {};
		DrawAutoArgs  automatic {};
	};
	bool       m_shadow = false;
	ShadowDraw m_shadow_draw;
	void       CopyDecodeState(const CommandProcessor& from);
	// Empty if this processor's decode state equals `other`'s, else what differs.
	[[nodiscard]] std::string DecodeStateDiff(const CommandProcessor& other) const;
	// The shadow's last draw against posted draw `sequence`; empty if equal.
	[[nodiscard]] std::string ShadowDrawDiff(const CommandProcessor& shadow, uint64_t sequence) const;
	HW::Context    m_ctx;
	std::optional<HW::Context> m_saved_ctx;
	HW::UserConfig m_ucfg;
	HW::Shader     m_sh_ctx;
	uint32_t       m_index_type_and_size              = 0;
	uint32_t       m_index_buffer_size                = 0;
	uint64_t       m_index_base_addr                  = 0;
	uint64_t       m_draw_indirect_args_base_addr     = 0;
	uint64_t       m_dispatch_indirect_args_base_addr = 0;
	// Persistent draw state. Indirect draws leave the address of their last instance count,
	// read only when a later draw inherits it.
	uint32_t m_num_instances      = 1;
	uint64_t m_num_instances_addr = 0;

	const int m_interrupt_event_id;
	uint64_t  m_synthetic_occlusion_counter = 0;
	bool      m_predicate_skip              = false;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMMAND_PROCESSOR_H
