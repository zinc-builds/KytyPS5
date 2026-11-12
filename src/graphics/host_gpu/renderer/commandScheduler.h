#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	// record_thread: draws record their final commands on a second thread (KYTY_RECORD_THREAD=0
	// turns it off).
	CommandScheduler(RenderContext& context, GraphicContext& graphics, bool record_thread = false);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           EndRendering() { m_command.EndRendering(); }
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           Finish(const std::source_location& site = std::source_location::current());
	// Early submission (KYTY_EARLY_SUBMIT, on by default; =0 off). The command processor calls
	// this at packets where nothing is held open across command buffers (no queued draw,
	// dispatch chain or atomic batch). When every earlier submission has finished on the GPU
	// (it would sit idle) and the open command buffer has been recording for at least
	// KYTY_EARLY_SUBMIT_US µs (default 300), it is submitted now rather than when the guest's
	// command buffer ends. Returns whether it submitted.
	bool           SubmitIfGpuIdle();
	// Sets the early-submission age in µs (< 0: off), for tests.
	void           SetEarlySubmitUs(int64_t age_us) noexcept {
		m_early_submit_age =
		    age_us < 0 ? -1
		               : std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		                     std::chrono::microseconds(age_us))
		                     .count();
	}
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// The next submission waits (at the transfer stage) until semaphore reaches value: work the
	// current command buffer consumes from another queue.
	void           WaitBeforeNextSubmit(vk::Semaphore semaphore, uint64_t value);
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void Wait(uint64_t tick, const std::source_location& site = std::source_location::current());
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	// Guest-memory completions use the priority queue; normal callbacks maintain GPU resources.
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] bool        HasPendingPriorityOperations();
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void QueueOperation(Common::UniqueFunction<void>&& operation, bool priority);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	std::unique_ptr<DeferredRecorder> m_recorder;
	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	std::queue<PendingOperation> m_pending_operations;
	// The oldest pending operation's tick, UINT64_MAX when none; written under m_operation_mutex.
	std::atomic<uint64_t>        m_pending_front_tick {UINT64_MAX};
	void                         NotePendingFront() noexcept {
		m_pending_front_tick.store(m_pending_operations.empty() ? UINT64_MAX
		                                                        : m_pending_operations.front().tick,
		                           std::memory_order_release);
	}
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	uint64_t                     m_priority_active_tick = 0; // 0 while none runs
	std::atomic<int64_t>         m_last_refresh {0}; // steady_clock ticks of the last tick read
	OperationState               m_operation_state      = OperationState::Open;
	vk::Semaphore                m_next_wait_semaphore  = nullptr;
	uint64_t                     m_next_wait_value      = 0;
	int64_t                      m_command_begin        = 0;  // steady_clock ticks at BeginCommand
	int64_t                      m_early_submit_age     = -1; // steady_clock ticks; < 0: off
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
