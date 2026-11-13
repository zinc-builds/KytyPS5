#ifndef KYTY_COMMON_GPU_WAIT_DIAG_H_
#define KYTY_COMMON_GPU_WAIT_DIAG_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <source_location>

// KYTY_GPU_WAIT_DIAG=1|trace: where the GPU command thread waits for the host GPU, and what the
// GPU was doing meanwhile. Only the main render scheduler's timeline is tracked.
// - Once per second, a GPUWAIT line: the command thread's GPU waits (count, ms), GPU idle time
//   (nothing submitted is unfinished), submits and the time spent in vkQueueSubmit; then one
//   GPUWAITSITE line per call site (file:line of the scheduler/semaphore caller) and the top
//   GPUWAITSTACK return-address stacks (offsets from the executable's start: symbolize with the
//   .map file).
// - Per wait: backlog (submits unfinished when the wait began), distance (submits after the
//   waited one), age (µs since the waited submit reached the queue), and whether the GPU was
//   idle when the waited submit arrived.
// - trace: also appends every submit, completion, wait, guest flip and guest submission start
//   to $KYTY_GPU_WAIT_TRACE (default /tmp/gpuwait-trace.txt), as text, once per second.
// KYTY_OFFCPU_SAMPLE=1 (with the above): a thread samples the command thread every ~200 µs;
//   when it is asleep in a system call it records the syscall and the first return addresses
//   found on its stack, and prints OFFCPU lines (share of wall time per stack) once per second.
// Completion times come from a thread that blocks on the timeline semaphore's next value, so
// they are late by its wake-up latency (tens of µs).
namespace Common::GpuWaitDiag {

[[nodiscard]] int Mode() noexcept; // 0 off, 1 stats, 2 trace

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = Mode() != 0;
	return enabled;
}

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// The tracked timeline (main scheduler): a submit reached the queue at end_ns; the
// vkQueueSubmit call began at start_ns.
void NoteSubmit(uint64_t tick, uint64_t start_ns, uint64_t end_ns, uint64_t draws) noexcept;
// The poller saw every tick up to `tick` finished at now_ns.
void NoteComplete(uint64_t tick, uint64_t now_ns) noexcept;
// A command-thread wait on the tracked timeline for `tick`; current = the next tick to be
// submitted, known = the GPU tick known when the wait began.
void NoteWait(uint64_t tick, uint64_t current, uint64_t known, uint64_t start_ns, uint64_t end_ns,
              const std::source_location& site) noexcept;
// A command-thread wait for the priority-operation thread (CommandScheduler::WaitPriorityOperations)
// that blocked: gpu_done = the GPU had already finished the blocking operation's tick when the wait
// began (so the wait is for the runner's wake-up and callbacks); ahead = priority operations queued
// or running at or before `tick`.
void NoteSchedulerWait(uint64_t start_ns, uint64_t end_ns, bool gpu_done, uint32_t ahead,
                       const std::source_location& site) noexcept;
// Guest events on the command thread (trace only).
void NoteFlip(uint64_t tick) noexcept;
void NoteGuestSubmission(uint32_t queue) noexcept;
void NoteFlipDone(uint64_t tick) noexcept; // any thread
// The command thread submitted early because the GPU ran out of work (KYTY_EARLY_SUBMIT).
void NoteEarlySubmit() noexcept;

// The command thread registers itself (for the off-CPU sampler).
void SetCommandThread() noexcept;
// KYTY_OFFCPU_THREAD=<name prefix> (with KYTY_OFFCPU_SAMPLE=1): sample the first game thread
// whose name starts with it instead of the command thread; with KYTY_FRAME_DIAG=1 only while the
// command thread has no work. Call from a game thread once it has its name, with its guest stack.
void SetGuestThread(const std::string& name, uintptr_t stack_base, size_t stack_size) noexcept;

// KYTY_DRIVER_STATS=1 (with KYTY_GPU_WAIT_DIAG): wall time of the command thread's own driver
// paths, per kind, and its direct recordings (CommandBuffer::Handle/BindingHandle) per call site
// with the recorder drain they waited for. DRIVER / DRIVERSITE lines once per second.
enum class Driver : uint8_t {
	Submit,         // CommandScheduler::Submit (recorder drain, vkEndCommandBuffer, vkQueueSubmit)
	QueueSubmit,    // of which vkQueueSubmit
	Refresh,        // MasterSemaphore::Refresh (vkGetSemaphoreCounterValue)
	BeginCommand,   // CommandScheduler::BeginCommand (pool search, vkBeginCommandBuffer)
	ImageCreate,    // GraphicContext::CreateImage (vmaCreateImage)
	ImageCreateDedicated,
	ImageDelete,
	BufferCreate,   // Buffer (vmaCreateBuffer)
	BufferCreateDedicated,
	BufferDelete,
	ImageRecycled,  // TextureCache image taken from the recycler instead of created
	ImageRetire,    // an image's Vulkan objects handed to the recycler or destroy thread
	ImageViewCreate,
	ImageFormatQuery,
	Count
};
[[nodiscard]] bool DriverStats() noexcept;
void NoteDriver(Driver kind, uint64_t ns) noexcept;
void NoteDirectRecord(const std::source_location& site, uint64_t drain_ns) noexcept;
// Host upload paths (KYTY_DRIVER_STATS; =touch also touches each destination page before the
// copy so first-touch faults are timed apart from the copy). UPLOAD lines once per second.
enum class UploadPath : uint8_t { RingFirstLap, Ring, Async, Temporary, Count };
struct UploadSample {
	uint64_t bytes      = 0;
	uint64_t alloc_ns   = 0; // temporary buffer creation
	uint64_t touch_ns   = 0; // =touch only
	uint64_t copy_ns    = 0; // the write itself (includes first-touch faults unless =touch)
	uint64_t flush_ns   = 0;
	uint64_t touch_flt  = 0; // minor faults during the touch
	uint64_t copy_flt   = 0; // minor faults during the write
};
[[nodiscard]] bool UploadTouch() noexcept;
[[nodiscard]] uint64_t ThreadMinorFaults() noexcept;
void NoteUpload(UploadPath path, const UploadSample& sample) noexcept;
void NoteUploadDestroy(uint64_t ns) noexcept; // a temporary upload buffer's destruction
// Image recycler state, sampled at report time.
void NoteRecyclerState(uint64_t entries, uint64_t bytes, uint64_t evicted, uint64_t destroy_queue) noexcept;
// Times its scope on the command thread when KYTY_DRIVER_STATS is on.
class DriverScope final {
public:
	explicit DriverScope(Driver kind) noexcept;
	~DriverScope() {
		if (m_start != 0) {
			NoteDriver(m_kind, NowNs() - m_start);
		}
	}
	void SetKind(Driver kind) noexcept { m_kind = kind; }
	DriverScope(const DriverScope&)            = delete;
	DriverScope& operator=(const DriverScope&) = delete;

private:
	Driver   m_kind;
	uint64_t m_start = 0;
};

// Called by the command thread; prints once per second.
void Report() noexcept;

} // namespace Common::GpuWaitDiag

#endif /* KYTY_COMMON_GPU_WAIT_DIAG_H_ */
