#include "graphics/host_gpu/renderer/drawPrepare.h"

#include "common/benchMetrics.h"
#include "common/hybridAffinity.h"
#include "common/kernelStats.h"
#include "common/profiler.h"
#include "common/stallDiag.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

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

struct StoredRange {
	uint64_t sequence;
	uint64_t begin;
	uint64_t end;
};
// Enough for every store between a draw's post and its adoption at any realistic rate.
constexpr size_t           StoredHistory = 4096;
std::mutex                 g_stored_mutex;
std::deque<StoredRange>    g_stored;
uint64_t                   g_stored_last = 0; // under g_stored_mutex
std::atomic<uint64_t>      g_stored_sequence {0};

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

void NoteStoredRange(uint64_t vaddr, uint64_t size) {
	std::scoped_lock lock(g_stored_mutex);
	g_stored.push_back({++g_stored_last, vaddr, vaddr + size});
	if (g_stored.size() > StoredHistory) {
		g_stored.pop_front();
	}
	g_stored_sequence.store(g_stored_last, std::memory_order_seq_cst);
}

uint64_t StoredSequence() noexcept {
	return g_stored_sequence.load(std::memory_order_seq_cst);
}

bool StoredSince(uint64_t sequence, const std::vector<std::pair<uint64_t, uint64_t>>& reads) {
	if (StoredSequence() == sequence) {
		return false;
	}
	std::scoped_lock lock(g_stored_mutex);
	if (g_stored.empty() || g_stored.front().sequence > sequence + 1) {
		return true;
	}
	for (auto it = g_stored.rbegin(); it != g_stored.rend() && it->sequence > sequence; ++it) {
		for (const auto& [address, size]: reads) {
			if (address < it->end && it->begin < address + size) {
				return true;
			}
		}
	}
	return false;
}

Worker::Worker(Run run, void* context, uint32_t index)
    : m_run(run), m_context(context), m_index(index), m_thread([this] { Loop(); }) {}

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
		Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::Wake);
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
	Common::StallDiag::Scope       stall(Common::StallDiag::Cause::CmdPrepare);
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::PrepareWait);
	Wake();
	for (uint32_t spins = 0; m_done.load(std::memory_order_acquire) < sequence; spins++) {
		if ((spins & 1023u) == 1023u) {
			Wake();
		}
		if (spins < 16384) {
			KYTY_PREPARE_PAUSE();
		} else {
			Common::KernelStats::Syscall yield(Common::KernelStats::Kind::Yield);
			std::this_thread::yield();
		}
	}
	return NowNs() - start;
}

void Worker::Loop() {
	KYTY_PROFILER_THREAD("Thread_GpuPrepare");
#if defined(__linux__)
	char name[16] = "KytyGpuPrep";
	if (m_index != 0) {
		std::snprintf(name, sizeof(name), "KytyGpuPrep%u", m_index);
	}
	pthread_setname_np(pthread_self(), name);
	Common::HybridAffinity::ApplyToPrepareThread(m_index);
#endif
	t_worker = true;
	ShaderRecompiler::IR::SetSrtWalkPrepareThread(m_index + 1);
	if (m_index < 3) {
		using Common::BenchMetrics::Role;
		Common::BenchMetrics::RegisterThread(
		    static_cast<Role>(static_cast<int>(Role::Prep0) + static_cast<int>(m_index)));
	}
	Common::BenchMetrics::RegisterPrepareWorker(m_index, &m_busy_ns, &m_done);
	uint64_t done = 0;
	for (;;) {
		auto posted = m_posted.load(std::memory_order_acquire);
		if (posted == done) {
			if (m_stop.load(std::memory_order_acquire)) {
				return;
			}
			// Draws arrive every few microseconds: spin a while before sleeping.
			const auto start = std::chrono::steady_clock::now();
			auto       stall = Common::StallDiag::Now();
			for (uint32_t spins = 1;; spins++) {
				KYTY_PREPARE_PAUSE();
				posted = m_posted.load(std::memory_order_acquire);
				if (posted != done || m_stop.load(std::memory_order_relaxed)) {
					break;
				}
				if ((spins & 255u) == 0 &&
				    std::chrono::steady_clock::now() - start > std::chrono::microseconds(50)) {
					Common::StallDiag::Add(Common::StallDiag::Cause::WorkerSpin, stall);
					stall = Common::StallDiag::Now();
					m_sleeping.store(true, std::memory_order_seq_cst);
					if (m_posted.load(std::memory_order_seq_cst) == done &&
					    !m_stop.load(std::memory_order_seq_cst)) {
						m_sleeping.wait(true, std::memory_order_seq_cst);
					}
					m_sleeping.store(false, std::memory_order_relaxed);
					Common::StallDiag::Add(Common::StallDiag::Cause::WorkerSleep, stall);
					stall = 0;
					break;
				}
			}
			Common::StallDiag::Add(Common::StallDiag::Cause::WorkerSpin, stall);
			continue;
		}
		Common::StallDiag::Scope busy(Common::StallDiag::Cause::WorkerBusy);
		const auto start = NowNs();
		while (done != posted) {
			m_run(m_context, ++done);
			m_done.store(done, std::memory_order_release);
		}
		m_busy_ns.fetch_add(NowNs() - start, std::memory_order_relaxed);
	}
}

} // namespace Libs::Graphics::DrawPrepare
