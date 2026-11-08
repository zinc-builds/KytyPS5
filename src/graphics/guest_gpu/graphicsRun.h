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
	// the work before the label (see LateLabelsEnabled). Effects the CPU performs at decode after
	// such a wait follow those labels: WRITE_DATA and semaphore signals are deferred behind them
	// (in order), DMA_DATA waits for them.
	// Not ordered: a later packet reading a pending label's or deferred write's bytes at decode
	// (as data) reads them before they land; =verify counts binds that do (read_unlanded).
	[[nodiscard]] static int LateLabelsMode() noexcept; // 0 off, 1 on, 2 verify
	// Tests: forces the mode (-1: back to KYTY_LATE_LABELS). Only while the GPU is idle.
	static void SetLateLabelsModeForTests(int mode) noexcept;
	// GPU thread: behind the work recorded so far, `cp` stores `store_size` bytes of `value` at
	// vaddr (none if 0) and then raises its interrupt if `interrupt`. size: the bytes waits may
	// predict from `value` (0: waits never pass on this store).
	void DeferLateLabel(CommandProcessor* cp, uint64_t vaddr, uint64_t value, uint64_t size,
	                    uint64_t store_size, bool interrupt, int event_id, uint32_t context_id);
	// GPU thread: whether a pending label store will satisfy a wait (wait_size bytes) for this
	// address, and so the wait may pass (also fences the labels deferred so far).
	[[nodiscard]] bool LateLabelSatisfies(uint64_t vaddr, uint64_t ref, uint64_t mask,
	                                      uint32_t func, uint64_t wait_size);
	// GPU thread: whether a CP effect the CPU performs at decode must follow pending labels (a
	// wait passed early on them, or an effect before it was deferred).
	[[nodiscard]] bool LateEffectsFenced() const noexcept {
		return !m_late_applying && m_late_fence > m_late_applied;
	}
	// GPU thread: runs `effect` (writing `bytes` at vaddr) after the stores deferred so far, and
	// fences the effects after it behind it.
	void DeferLateEffect(Common::UniqueFunction<void>&& effect, uint64_t vaddr, uint64_t bytes);
	// GPU thread: waits until the fenced stores and effects have landed, so a CP effect performed
	// now (DMA_DATA) lands after them and reads what they and the
	// work before them wrote.
	void DrainLateFence();
	// =verify, GPU thread: a buffer bind reads [vaddr, vaddr + size) at decode; counts it if a
	// passed-early label or deferred write there has not landed yet.
	static inline std::atomic_bool s_late_read_checks {false};
	static void                    NoteLateRead(uint64_t vaddr, uint64_t size);
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
		// The last waits passed on a pending value here. If something else is later seen storing
		// a value that would have released one, waits on this label no longer pass early.
		struct Wait {
			uint64_t ref = 0, mask = 0;
			uint32_t func = 0;
		};
		std::array<Wait, 2> passed {};
		uint32_t            passed_count = 0;
		bool                strict       = false;
		uint32_t            value_size   = 0; // bytes the newest deferred store writes
		// A pending store to a neighbouring label overlaps this one: no prediction until it lands.
		uint64_t            unknown_until = 0;
	};
	struct LateStore {
		uint64_t                     sequence    = 0;
		uint64_t                     vaddr       = 0;
		uint64_t                     value       = 0;
		uint64_t                     size        = 0; // 0: not a tracked label
		uint64_t                     store_size  = 0; // bytes stored at vaddr
		uint64_t                     deferred_ns = 0;
		LateLabel*                   label       = nullptr; // its record, if a tracked label
		CommandProcessor*            cp          = nullptr;
		bool                         interrupt   = false;
		int                          event_id    = 0;
		uint32_t                     context_id  = 0;
		Common::UniqueFunction<void> effect; // a deferred WRITE_DATA or semaphore signal
	};
	// Label records in one open-addressed table (address 0: empty). It only grows or drops idle
	// records when no store is in flight, so a deferred store's record pointer stays valid; when
	// full meanwhile, new labels go untracked (their waits do not pass early).
	struct LateLabelSlot {
		uint64_t  vaddr = 0;
		LateLabel label;
	};
	[[nodiscard]] LateLabel* FindLateLabel(uint64_t vaddr);
	[[nodiscard]] LateLabel* AddLateLabel(uint64_t vaddr);
	void                     MaintainLateLabels();
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
	uint64_t                                       m_late_fence    = 0; // CP effects wait for this
	bool                                           m_late_applying = false; // running an effect
	// =verify: ranges a wait passed on or a deferred write covers, until stored (sequence).
	struct LateUnlanded {
		uint64_t begin = 0, end = 0, sequence = 0;
	};
	std::vector<LateUnlanded>                      m_late_unlanded;
	std::vector<LateLabelSlot>                     m_late_labels = std::vector<LateLabelSlot>(1u << 16u);
	size_t                                         m_late_label_count = 0;
	// Interrupts of the stores run in this drain, raised after it in order (exact repeats once).
	struct LateInterrupt {
		int      event_id   = 0;
		uint32_t context_id = 0;
	};
	std::vector<LateInterrupt>                     m_late_interrupts;
	struct {
		uint64_t deferred = 0, applied = 0, satisfied = 0, foreign = 0, foreign_zero = 0,
		         refused = 0, fenced = 0, drained = 0, read_unlanded = 0, delay_ns = 0, max_delay_ns = 0, start_ns = 0;
		uint64_t releasing_foreign = 0; // other stores that would have released a passed wait
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
