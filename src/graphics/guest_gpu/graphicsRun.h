#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class RenderContext;

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// Insert an ordered graphics drain; only a previous suspend point can block the caller.
	void SuspendPoint();
	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;
	// Guest-thread commands wait for ProcessCommands.
	[[nodiscard]] bool HasPendingCommands() const noexcept {
		return m_pending_commands.load(std::memory_order_acquire) != 0;
	}

	// Late labels (C1 stage 1; KYTY_LATE_LABELS=1, =verify also checks order and prints
	// LATELABELS lines): end-of-pipe label stores and interrupts happen once the host GPU has
	// finished the work before them, as on the console, instead of when the packet is decoded.
	// The command processor defers a store here (GPU thread); a priority operation posts it back
	// at the tick's completion; this thread runs posted stores in order, between packets and when
	// idle. A WAIT_REG_MEM that this processor's own pending store will satisfy is satisfied at
	// once: every guest queue records into the one host queue, so the work after it runs after
	// the work before the label (see LateLabelsEnabled). CP stores decoded after such a wait wait
	// for the labels before them.
	[[nodiscard]] static int LateLabelsMode() noexcept; // 0 off, 1 on, 2 verify
	// Tests: forces the mode (-1: back to KYTY_LATE_LABELS). Only while the GPU is idle.
	static void SetLateLabelsModeForTests(int mode) noexcept;
	// GPU thread: defers `store` (writing `size` bytes of `value` at `vaddr`; size 0: no label)
	// behind the work recorded so far.
	void DeferLateStore(uint64_t vaddr, uint64_t value, uint64_t size,
	                    Common::UniqueFunction<void>&& store);
	// GPU thread: whether a pending label store will satisfy a wait for this address, and so the
	// wait may pass (also makes later CP stores wait for the labels deferred so far).
	[[nodiscard]] bool LateLabelSatisfies(uint64_t vaddr, uint64_t ref, uint64_t mask,
	                                      uint32_t func);
	// GPU thread: whether a CP store must be deferred behind pending labels.
	[[nodiscard]] bool LateStoresFenced() const noexcept {
		return !m_late_applying && m_late_fence > m_late_applied;
	}
	// GPU thread: runs posted stores; true if any ran.
	bool ProcessLateLabels();

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Commands, FlipPreparation, SuspendPoint };

	struct Submission {
		SubmissionType            type     = SubmissionType::Commands;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		Pm4Execution              execution;
		bool                      blocked         = false;
		uint64_t                  flip_request_id = 0;
	};

	struct LateLink {
		std::mutex mutex;
		GuestGpu*  gpu = nullptr; // null once the GPU thread stopped
	};
	// What this processor knows about one label address.
	struct LateLabel {
		uint32_t                pending  = 0; // stores deferred, not applied
		uint64_t                value    = 0; // the newest deferred value
		uint64_t                written  = 0; // the last value this processor stored
		bool                    has_written = false;
		// Values found there that something else stored (up to four; more: any value).
		std::array<uint64_t, 4> foreign {};
		uint32_t                foreign_count = 0;
		bool                    foreign_any   = false;
	};
	struct LateStore {
		uint64_t                     sequence    = 0;
		uint64_t                     vaddr       = 0;
		uint64_t                     value       = 0;
		uint64_t                     size        = 0; // 0: not a tracked label
		uint64_t                     deferred_ns = 0;
		LateLabel*                   label       = nullptr; // its record, if a tracked label
		Common::UniqueFunction<void> store;
	};
	// GPU thread, before each submission: hands the stores deferred in the buffer to the priority
	// queue, which posts them back once the buffer's work has finished.
	void FlushLateBatch();
	void PostLateBatch(std::vector<LateStore>&& batch);
	void ApplyLateStore(LateStore& item);
	void ReportLateLabels(uint64_t now_ns);

	void              Enqueue(Submission submission);
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
	Common::Mutex                                  m_queue_mutex;
	std::mutex                                     m_shutdown_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	std::deque<Common::UniqueFunction<void>>       m_commands;
	std::atomic_uint32_t                           m_pending_commands {0};
	// Late labels: posted stores (under m_queue_mutex), stores deferred and not yet applied, and
	// the rest, which only the GPU thread uses.
	std::vector<std::vector<LateStore>>            m_late_posted;
	std::vector<LateStore>                         m_late_batch; // GPU thread
	std::atomic_uint32_t                           m_late_posted_count {0};
	std::atomic_uint64_t                           m_late_in_flight {0};
	std::shared_ptr<LateLink>                      m_late_link = std::make_shared<LateLink>();
	uint64_t                                       m_late_deferred = 0; // sequences handed out
	uint64_t                                       m_late_applied  = 0; // sequences applied
	uint64_t                                       m_late_fence    = 0; // CP stores wait for this
	bool                                           m_late_applying = false; // running a store
	std::unordered_map<uint64_t, LateLabel>        m_late_labels; // by address
	struct {
		uint64_t deferred = 0, applied = 0, satisfied = 0, foreign = 0, foreign_zero = 0,
		         refused = 0, fenced = 0, delay_ns = 0, max_delay_ns = 0, start_ns = 0;
	} m_late_stats;
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;
	// Completion callbacks can outlive GuestGpu during renderer shutdown.
	std::shared_ptr<std::binary_semaphore> m_suspend_point_ready =
	    std::make_shared<std::binary_semaphore>(1);

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	std::atomic_int m_done_num = 0;
	std::jthread    m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
