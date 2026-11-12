#ifndef KYTY_COMMON_GPU_WAIT_DIAG_H_
#define KYTY_COMMON_GPU_WAIT_DIAG_H_

#include <atomic>
#include <chrono>
#include <cstdint>
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

// Called by the command thread; prints once per second.
void Report() noexcept;

} // namespace Common::GpuWaitDiag

#endif /* KYTY_COMMON_GPU_WAIT_DIAG_H_ */
