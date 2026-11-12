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
	CmdGpuWaits,  // the command thread blocked on the GPU timeline (MasterSemaphore::Wait)
	CmdGpuWaitNs,
	CmdIdles,     // the command thread waited for guest work (nothing queued)
	CmdIdleNs,
	CmdBlocks,    // the command thread waited with only blocked submissions queued
	CmdBlockedNs,
	WriteDatas,   // CommandProcessor::WriteData packets
	WriteDataNs,
	WriteDataDwords,
	LabelStale,   // IsCpLabel: the label matched but its token was voided (the wait collects)
	LabelWritten, // IsCpLabel: the label matched but the guest stored to its page
	WriteDataMemoHits, // StoreCpLabels through the page's memo (KYTY_WRITE_DATA_MEMO)
	IndirectDraws,      // indirect draw packets, single and multi (KYTY_INDIRECT_QUEUE)
	IndirectQueued,     // of those, queued and prepared
	IndirectGpuCount,   // of those, with the record count read by the GPU
	IndirectPatched,    // drawn at once: the shaders need per-record offsets patched
	IndirectUnqueued,   // drawn at once: switch off, frame skip, decode thread, over the device
	                    // bound, no index buffer, records needed on the CPU (tess, quad list, mesh)
	IndirectPredicated, // multi-draw indirect packets skipped by predication
	IndirectVerified,   // queued draws whose GPU count was checked (KYTY_INDIRECT_QUEUE=verify)
	TextureFrees,
	Count
};

namespace Detail {
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
} // namespace Detail

[[nodiscard]] bool ReadEnabled() noexcept;
// Inline so that the disabled path costs a guard check, not a call.
[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = ReadEnabled();
	return enabled;
}
// The counters above (and the command thread's idle timing) also run for the playtest recorder
// (KYTY_PLAYTEST, common/playtest.h), which prints nothing per frame; the lines, guest-wait
// timing and GIDLE stay KYTY_FRAME_DIAG's.
[[nodiscard]] bool ReadCounting() noexcept;
[[nodiscard]] inline bool Counting() noexcept {
	static const bool counting = ReadCounting();
	return counting;
}

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	if (Counting()) {
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
	    : m_count(count), m_ns(ns), m_start(Counting() ? NowNs() : 0) {}
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

// The command thread, after it decoded and flushed a guest flip: one GFRAME line with the same
// fields for the work between that flip and the previous one (`frame`: the presented flip count
// then, to place the line in a bench window; t=: steady-clock ns, CLOCK_MONOTONIC on Linux, to
// line it up with `perf record -k CLOCK_MONOTONIC`).
void OnGuestFlip(uint64_t frame) noexcept;

// The presentation thread, at each presented flip of the main bus: `frame` is the flip count
// (PERF lines' frames=) and `interval_ms` the time since the previous one.
void OnFlip(uint64_t frame, double interval_ms) noexcept;

// What the guest's submitting thread did while the command thread had no work. Guest waits
// (the emulator's implementations of the calls below) are timed per thread; those of 20 µs or
// more go into a per-thread ring, shorter ones only into per-kind totals. Each guest submission
// marks its thread as the submitter. When the command thread's wait for guest work lasts 1 ms or
// more, it prints one GIDLE line: the gap, how long after the last guest submission the command
// thread woke, and the submitter's waits inside the gap by kind (clipped to it), plus its last wait
// before the gap ended (kind, ident, length, and how long before the end it returned).
enum class GuestWait : uint8_t { Equeue, Sleep, Cond, Mutex, Sema, EventFlag, Count };

class GuestWaitScope {
public:
	// Inline so that the disabled path costs a guard check, not a call.
	explicit GuestWaitScope(GuestWait kind, uint64_t ident = 0) noexcept: m_ident(ident), m_kind(kind) {
		if (Enabled()) {
			Begin();
		}
	}
	~GuestWaitScope() {
		if (Enabled()) {
			End();
		}
	}
	// For waits whose ident is known only after they return (equeue: the first event's filter and
	// ident).
	void SetIdent(uint64_t ident) noexcept { m_ident = ident; }
	GuestWaitScope(const GuestWaitScope&)            = delete;
	GuestWaitScope& operator=(const GuestWaitScope&) = delete;

private:
	void Begin() noexcept;
	void End() noexcept;

	uint64_t  m_start = 0;
	uint64_t  m_ident = 0;
	GuestWait m_kind;
};

// A guest thread submitted a command buffer; only graphics submissions mark the submitter.
void OnGuestSubmit(bool graphics) noexcept;

// A guest thread signalled or broadcast a condition variable: GIDLE names the thread that last
// signalled the condition the submitter waited on last, and that thread's own last wait.
void OnGuestCondSignal(const void* cond) noexcept;

// The command thread begins waiting for guest work; then it waited from `start_ns` to `end_ns`
// (NowNs()). Short waits (under 20 µs) count when they ended in the gap.
void OnCmdIdleBegin() noexcept;
// Whether the command thread is waiting for guest work now (any thread; diagnostic only).
[[nodiscard]] bool CmdIdle() noexcept;
void OnCmdIdle(uint64_t start_ns, uint64_t end_ns) noexcept;

} // namespace Common::FrameDiag

#endif // KYTY_COMMON_FRAME_DIAG_H_
