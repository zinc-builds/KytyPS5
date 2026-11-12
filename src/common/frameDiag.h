#ifndef KYTY_COMMON_FRAME_DIAG_H_
#define KYTY_COMMON_FRAME_DIAG_H_

#include <atomic>
#include <chrono>
#include <cstdint>

// KYTY_FRAME_DIAG=1 (off by default): one FRAME line per presented flip on stdout, with what the
// GPU pipeline did between that flip and the previous one, to tell slow frames from fast ones:
// the interval and its refreshes, the command thread's CPU time, run-queue delay, faults and
// context switches, its waits by kind (prepare workers, recorder, GPU, the render-context mutex),
// draws, dispatches, submits, uploads, texture and buffer creations and evictions, shader and
// pipeline compiles, garbage collections, write collections and submit scans, the prepare
// workers' busy time and the recorder's CPU time. Counters come from DebugCounters (needs the
// performance overlay's counters on: --perf-overlay), BenchMetrics (needs KYTY_PERF_LOG for the
// thread registry) and the few below. tools/astro-bench/framediag.py summarizes the lines.
namespace Common::FrameDiag {

enum class Counter {
	MutexWaitNs,  // command thread blocked on the render-context mutex
	MutexWaits,
	GcRuns,       // RenderContext::RunGarbageCollector
	GcNs,
	Collects,     // RenderContext::CollectAsyncWrites
	CollectNs,
	SubmitScans,  // RenderContext::ScanAsyncWritesAtSubmit
	SubmitScanNs,
	TextureCreates,
	BufferCreates,
	Count
};

namespace Detail {
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
} // namespace Detail

[[nodiscard]] bool Enabled() noexcept;

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	if (Enabled()) {
		Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
	}
}
[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
// Times a scope into `ns` and counts it in `count`, when enabled.
class Scope {
public:
	Scope(Counter count, Counter ns) noexcept
	    : m_count(count), m_ns(ns), m_start(Enabled() ? NowNs() : 0) {}
	~Scope() {
		if (m_start != 0) {
			Add(m_count);
			Add(m_ns, NowNs() - m_start);
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Counter  m_count;
	Counter  m_ns;
	uint64_t m_start;
};

// The presentation thread, at each presented flip of the main bus: `frame` is the flip count
// (PERF lines' frames=) and `interval_ms` the time since the previous one.
void OnFlip(uint64_t frame, double interval_ms) noexcept;

} // namespace Common::FrameDiag

#endif // KYTY_COMMON_FRAME_DIAG_H_
