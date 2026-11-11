#ifndef KYTY_COMMON_BENCH_METRICS_H_
#define KYTY_COMMON_BENCH_METRICS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// Benchmark metrics: cumulative counters for the GPU pipeline threads (command thread, prepare
// workers, recorder), printed as one METRICS line after each KYTY_PERF_LOG PERF line. The bench
// scripts diff two lines at the edges of their frame window and divide by the draws in between,
// so nothing is sampled per draw:
//   - per thread: CPU time (schedstat, precise), its user/sys split (stat ticks), run-queue
//     delay, context switches, and user-mode instructions and cycles from self-monitoring
//     perf_event_open counters (perf_event_paranoid 2 allows user mode on one's own threads;
//     on failure the counters read as absent);
//   - command-thread waits by kind and queue events, counted only on their slow paths (a wait
//     that happens, a drain, a rejected prepared draw): the per-draw path is unchanged.
// Thread counters are opened only when KYTY_PERF_LOG is set (KYTY_BENCH_PERF=0 skips them).
// KYTY_BENCH_METRICS=0 at compile time removes all of it; KYTY_BENCH_COUNTERS=0 removes only
// the slow-path counters below (to measure what they cost).
#ifndef KYTY_BENCH_METRICS
#define KYTY_BENCH_METRICS 1
#endif
#ifndef KYTY_BENCH_COUNTERS
#define KYTY_BENCH_COUNTERS KYTY_BENCH_METRICS
#endif

namespace Common::BenchMetrics {

enum class Role { Cmd, Prep0, Prep1, Prep2, Rec, Count };

enum class Counter {
	Drains,         // FinishQueuedDraws with draws queued
	DrainWaitNs,    // command thread waiting for a prepare worker while draining the queue
	RefillWaitNs,   // ... for the draws posted right after a drain (refilling the queue)
	SteadyWaitNs,   // ... any other queued draw (full queue, slow prepare)
	IdleWaitNs,     // ... for every worker to go idle (after a rejected draw, before direct work)
	PrepareWaits,   // waits of the four kinds above
	RecorderWaitNs, // command thread waiting for the recorder (ring space, drains)
	RecorderWaits,
	Rejected,       // worker-prepared draws redone on the command thread
	Count
};

namespace Detail {
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
} // namespace Detail

// Slow paths only: a locked add is fine there.
inline void Add([[maybe_unused]] Counter counter, [[maybe_unused]] uint64_t value = 1) noexcept {
#if KYTY_BENCH_COUNTERS
	Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
#endif
}
// For a wait timed only for these counters: Stamp() at its start, AddSince at its end.
[[nodiscard]] inline uint64_t Stamp() noexcept {
#if KYTY_BENCH_COUNTERS
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
#else
	return 0;
#endif
}
inline void AddSince([[maybe_unused]] Counter counter, [[maybe_unused]] uint64_t start) noexcept {
#if KYTY_BENCH_COUNTERS
	Add(counter, Stamp() - start);
#endif
}

// Called on the thread itself when it starts; the first thread to take a role keeps it.
void RegisterThread(Role role);
// A prepare worker's cumulative busy time and finished jobs (both relaxed atomics it owns).
void RegisterPrepareWorker(uint32_t index, const std::atomic<uint64_t>* busy_ns,
                           const std::atomic<uint64_t>* done);

// One METRICS line (cumulative values, no trailing newline), or empty when compiled out.
// `frames` and `t` are the PERF line's, so the scripts can match the two lines.
[[nodiscard]] std::string Line(uint64_t frames, double t, uint64_t draws);
// A BUILD line naming the binary's revision.
[[nodiscard]] std::string BuildLine();

} // namespace Common::BenchMetrics

#endif // KYTY_COMMON_BENCH_METRICS_H_
