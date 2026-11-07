#include "graphics/host_gpu/renderer/drawPrepare.h"

#include "common/hybridAffinity.h"
#include "common/profiler.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <chrono>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define KYTY_PREPARE_PAUSE() _mm_pause()
#else
#define KYTY_PREPARE_PAUSE() std::this_thread::yield()
#endif

namespace Libs::Graphics::DrawPrepare {

namespace {
thread_local bool    t_worker = false;
thread_local Log*    t_log    = nullptr;
std::atomic<uint64_t> g_hazard_generation {0};

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
} // namespace

bool OnWorker() noexcept {
	return t_worker;
}

Log* CurrentLog() noexcept {
	return t_log;
}

void SetLog(Log* log) noexcept {
	t_log = log;
}

void NoteHazard() noexcept {
	g_hazard_generation.fetch_add(1, std::memory_order_relaxed);
}

uint64_t HazardGeneration() noexcept {
	return g_hazard_generation.load(std::memory_order_relaxed);
}

Worker::Worker(Run run, void* context): m_run(run), m_context(context), m_thread([this] { Loop(); }) {}

Worker::~Worker() {
	WaitIdle();
	m_stop.store(true, std::memory_order_seq_cst);
	m_sleeping.store(false, std::memory_order_seq_cst);
	m_sleeping.notify_one();
	m_thread.join();
}

void Worker::Wake() {
	if (m_sleeping.load(std::memory_order_seq_cst)) {
		m_sleeping.store(false, std::memory_order_seq_cst);
		m_sleeping.notify_one();
	}
}

uint64_t Worker::Post() {
	// No fence: a worker that goes to sleep without seeing this post is woken by Wait.
	const auto sequence = ++m_posted_local;
	m_posted.store(sequence, std::memory_order_release);
	if (m_sleeping.load(std::memory_order_relaxed)) {
		Wake();
	}
	return sequence;
}

uint64_t Worker::Wait(uint64_t sequence) {
	if (m_done.load(std::memory_order_acquire) >= sequence) {
		return 0;
	}
	const auto start = NowNs();
	Wake();
	for (uint32_t spins = 0; m_done.load(std::memory_order_acquire) < sequence; spins++) {
		if ((spins & 1023u) == 1023u) {
			Wake();
		}
		if (spins < 16384) {
			KYTY_PREPARE_PAUSE();
		} else {
			std::this_thread::yield();
		}
	}
	return NowNs() - start;
}

void Worker::Loop() {
	KYTY_PROFILER_THREAD("Thread_GpuPrepare");
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyGpuPrep");
	Common::HybridAffinity::ApplyToPrepareThread();
#endif
	t_worker = true;
	ShaderRecompiler::IR::SetSrtWalkPrepareThread(true);
	uint64_t done = 0;
	for (;;) {
		auto posted = m_posted.load(std::memory_order_acquire);
		if (posted == done) {
			if (m_stop.load(std::memory_order_acquire)) {
				return;
			}
			// Draws arrive every few microseconds: spin a while before sleeping.
			const auto start = std::chrono::steady_clock::now();
			for (uint32_t spins = 1;; spins++) {
				KYTY_PREPARE_PAUSE();
				posted = m_posted.load(std::memory_order_acquire);
				if (posted != done || m_stop.load(std::memory_order_relaxed)) {
					break;
				}
				if ((spins & 255u) == 0 &&
				    std::chrono::steady_clock::now() - start > std::chrono::microseconds(50)) {
					m_sleeping.store(true, std::memory_order_seq_cst);
					if (m_posted.load(std::memory_order_seq_cst) == done &&
					    !m_stop.load(std::memory_order_seq_cst)) {
						m_sleeping.wait(true, std::memory_order_seq_cst);
					}
					m_sleeping.store(false, std::memory_order_relaxed);
					break;
				}
			}
			continue;
		}
		const auto start = NowNs();
		while (done != posted) {
			m_run(m_context, ++done);
			m_done.store(done, std::memory_order_release);
		}
		m_busy_ns.fetch_add(NowNs() - start, std::memory_order_relaxed);
	}
}

} // namespace Libs::Graphics::DrawPrepare
