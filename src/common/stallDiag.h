#ifndef KYTY_COMMON_STALL_DIAG_H_
#define KYTY_COMMON_STALL_DIAG_H_

#include "common/envSwitch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// KYTY_STALL_DIAG=1: where the GPU command thread, the draw-prepare worker and the recorder spend
// their time, as shares of wall time, printed once per second as a STALL line on stdout. An
// interval counts when it ends, so a sleep across reports (loading, idle) shows in the line after
// it wakes; during steady rendering the shares are exact to a few percent.
namespace Common::StallDiag {

enum class Cause {
	// GPU command thread.
	CmdNoWork,       // no command and no submission queued: waiting for the guest
	CmdBlocked,      // only blocked submissions (unsatisfied waits) are queued
	CmdPrepare,      // waiting for the prepare worker's draw
	CmdRecorder,     // waiting for recorder ring space or a drain
	CmdGpu,          // waiting for a GPU timeline value while processing PM4
	CmdCommand,      // running another thread's synchronous command (readbacks, unmaps)
	CmdCommandGpu,   // of which waiting for a GPU timeline value
	CmdScheduler,    // waiting for the scheduler's completion callbacks
	// Draw-prepare worker.
	WorkerBusy,
	WorkerSpin,      // spinning for the next post
	WorkerSleep,
	// Recorder.
	RecorderBusy,
	RecorderSpin,
	RecorderSleep,
	Count
};

namespace Detail {
inline std::atomic<uint64_t> g_ns[static_cast<int>(Cause::Count)] {};
inline std::atomic<uint64_t> g_start_ns {0};
inline thread_local bool     t_cmd_thread = false;
inline thread_local bool     t_in_command = false;

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = [] {
		return Common::EnvFlag("KYTY_STALL_DIAG");
	}();
	return enabled;
}

[[nodiscard]] inline uint64_t Now() noexcept {
	return Enabled() ? Detail::NowNs() : 0;
}

inline void Add(Cause cause, uint64_t start_ns) noexcept {
	if (start_ns == 0) {
		return;
	}
	Detail::g_ns[static_cast<int>(cause)].fetch_add(Detail::NowNs() - start_ns,
	                                                std::memory_order_relaxed);
}

// Marks the calling thread as the GPU command thread: command-thread causes that other threads
// can also hit (GPU and scheduler waits) count only there.
inline void SetCommandThread() noexcept {
	Detail::t_cmd_thread = true;
}

[[nodiscard]] inline bool OnCommandThread() noexcept {
	return Detail::t_cmd_thread;
}

// Brackets another thread's command run on the GPU command thread.
class CommandScope final {
public:
	CommandScope() noexcept: m_start(Now()) { Detail::t_in_command = true; }
	~CommandScope() {
		Detail::t_in_command = false;
		Add(Cause::CmdCommand, m_start);
	}
	CommandScope(const CommandScope&)            = delete;
	CommandScope& operator=(const CommandScope&) = delete;

private:
	uint64_t m_start;
};

// The GPU-wait cause for the calling thread's context.
[[nodiscard]] inline Cause GpuWaitCause() noexcept {
	return Detail::t_in_command ? Cause::CmdCommandGpu : Cause::CmdGpu;
}

class Scope final {
public:
	explicit Scope(Cause cause, bool counted = true) noexcept
	    : m_cause(cause), m_start(counted ? Now() : 0) {}
	~Scope() { Add(m_cause, m_start); }
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Cause    m_cause;
	uint64_t m_start;
};

// Called by the GPU command thread; prints and resets the shares once per second.
inline void Report() noexcept {
	if (!Enabled()) {
		return;
	}
	const auto now   = Detail::NowNs();
	auto       start = Detail::g_start_ns.load(std::memory_order_relaxed);
	if (start == 0) {
		Detail::g_start_ns.store(now, std::memory_order_relaxed);
		return;
	}
	if (now - start < 1'000'000'000ull) {
		return;
	}
	Detail::g_start_ns.store(now, std::memory_order_relaxed);
	double share[static_cast<int>(Cause::Count)];
	for (int i = 0; i < static_cast<int>(Cause::Count); i++) {
		share[i] = 100.0 * static_cast<double>(Detail::g_ns[i].exchange(0, std::memory_order_relaxed)) /
		           static_cast<double>(now - start);
	}
	const auto at = [&](Cause cause) { return share[static_cast<int>(cause)]; };
	std::printf("STALL cmd: no_work=%.1f%% blocked=%.1f%% prepare=%.1f%% recorder=%.1f%% gpu=%.1f%% "
	            "command=%.1f%% (gpu %.1f%%) scheduler=%.1f%% | worker: busy=%.1f%% spin=%.1f%% sleep=%.1f%% | recorder: "
	            "busy=%.1f%% spin=%.1f%% sleep=%.1f%%\n",
	            at(Cause::CmdNoWork), at(Cause::CmdBlocked), at(Cause::CmdPrepare),
	            at(Cause::CmdRecorder), at(Cause::CmdGpu), at(Cause::CmdCommand),
	            at(Cause::CmdCommandGpu), at(Cause::CmdScheduler),
	            at(Cause::WorkerBusy), at(Cause::WorkerSpin), at(Cause::WorkerSleep),
	            at(Cause::RecorderBusy), at(Cause::RecorderSpin), at(Cause::RecorderSleep));
	std::fflush(stdout);
}

} // namespace Common::StallDiag

#endif /* KYTY_COMMON_STALL_DIAG_H_ */
