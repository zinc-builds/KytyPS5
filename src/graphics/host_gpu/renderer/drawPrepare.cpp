#include "graphics/host_gpu/renderer/drawPrepare.h"

#include "common/benchMetrics.h"
#include "common/hybridAffinity.h"
#include "common/kernelStats.h"
#include "common/profiler.h"
#include "common/stallDiag.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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

// Ranges recorded with a sequence number, any thread. A range may stay open (a store between its
// opening and closing records, keyed by the opening sequence): a draw posted after the opening
// record may have read the bytes before the store and be adopted before the closing record, so
// the sequence alone does not show it.
class RangeHistory {
public:
	// Enough for every record between a draw's post and its adoption at any realistic rate.
	static constexpr size_t Capacity = 4096;

	uint64_t Record(uint64_t vaddr, uint64_t size) {
		std::scoped_lock lock(m_mutex);
		const auto sequence = Push(vaddr, size);
		m_sequence.store(sequence, std::memory_order_seq_cst);
		return sequence;
	}

	uint64_t Open(uint64_t vaddr, uint64_t size) {
		std::scoped_lock lock(m_mutex);
		m_open_count.fetch_add(1, std::memory_order_seq_cst);
		const auto open = Push(vaddr, size);
		m_open.push_back({open, vaddr, vaddr + size});
		m_sequence.store(open, std::memory_order_seq_cst);
		return open;
	}

	void Close(uint64_t open) {
		std::scoped_lock lock(m_mutex);
		const auto it = std::ranges::find(m_open, open, &StoredRange::sequence);
		EXIT_IF(it == m_open.end());
		Push(it->begin, it->end - it->begin);
		m_open.erase(it);
		m_sequence.store(m_last, std::memory_order_seq_cst);
		m_open_count.fetch_sub(1, std::memory_order_seq_cst);
	}

	[[nodiscard]] uint64_t Sequence() const noexcept {
		return m_sequence.load(std::memory_order_seq_cst);
	}

	// Whether a range recorded after `sequence`, or one still open, satisfies `hit`; also true when
	// the history no longer reaches back that far.
	template <typename Hit>
	[[nodiscard]] bool Since(uint64_t sequence, Hit hit) {
		// The count is read before the sequence: an open range raises it before its opening
		// record and lowers it after its closing one.
		if (m_open_count.load(std::memory_order_seq_cst) == 0 &&
		    m_sequence.load(std::memory_order_seq_cst) == sequence) {
			return false;
		}
		std::scoped_lock lock(m_mutex);
		for (const auto& open: m_open) {
			if (hit(open)) {
				return true;
			}
		}
		if (m_last == sequence) {
			return false;
		}
		if (m_ranges.empty() || m_ranges.front().sequence > sequence + 1) {
			return true;
		}
		for (auto it = m_ranges.rbegin(); it != m_ranges.rend() && it->sequence > sequence; ++it) {
			if (hit(*it)) {
				return true;
			}
		}
		return false;
	}

private:
	// Under m_mutex.
	uint64_t Push(uint64_t vaddr, uint64_t size) {
		m_ranges.push_back({++m_last, vaddr, vaddr + size});
		if (m_ranges.size() > Capacity) {
			m_ranges.pop_front();
		}
		return m_last;
	}

	std::mutex               m_mutex;
	std::deque<StoredRange>  m_ranges;
	uint64_t                 m_last = 0; // under m_mutex
	std::atomic<uint64_t>    m_sequence {0};
	std::vector<StoredRange> m_open; // under m_mutex
	std::atomic<uint32_t>    m_open_count {0};
};

RangeHistory g_stored;      // emulator stores into guest memory
RangeHistory g_gpu_written; // guest ranges newly marked GPU-written

bool ReadsHit(const std::vector<std::pair<uint64_t, uint64_t>>& reads, const StoredRange& range) {
	return std::ranges::any_of(reads, [&](const auto& read) {
		return read.first < range.end && range.begin < read.first + read.second;
	});
}

bool PagesHit(const std::vector<uint64_t>& pages, const StoredRange& range) {
	const auto first = range.begin >> 12u;
	const auto last  = (range.end - 1) >> 12u;
	return std::ranges::any_of(pages, [&](uint64_t page) { return page >= first && page <= last; });
}

bool LogHit(const Log& log, const StoredRange& range) {
	return ReadsHit(log.reads, range) || PagesHit(log.pages, range);
}

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}
} // namespace

namespace Chaos {
namespace {
std::array<std::atomic<uint64_t>, static_cast<size_t>(Site::Count)> g_delays {};
} // namespace

void Delay(Site site) noexcept {
	thread_local uint64_t state = 0;
	if (state == 0) {
		state = g_seed.load(std::memory_order_relaxed) ^
		        (std::hash<std::thread::id> {}(std::this_thread::get_id()) * 0x9e3779b97f4a7c15ull) ^
		        NowNs();
		state |= 1;
	}
	// xorshift64*
	state ^= state >> 12u;
	state ^= state << 25u;
	state ^= state >> 27u;
	const uint64_t r = (state * 0x2545f4914f6cdd1dull) ^ (static_cast<uint64_t>(site) << 56u);
	if ((r & 7u) == 0) {
		// A short spin: a few cache-line hand-offs to a preemption's worth.
		const auto until = NowNs() + ((r >> 8u) % 20'000u);
		while (NowNs() < until) {
			KYTY_PREPARE_PAUSE();
		}
	} else if ((r & 63u) == 1) {
		std::this_thread::yield();
	} else if ((r & 1023u) == 2) {
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	} else {
		return;
	}
	g_delays[static_cast<size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t Delays(Site site) noexcept {
	return g_delays[static_cast<size_t>(site)].load(std::memory_order_relaxed);
}

void Checked() noexcept {
	static uint64_t checked = 0; // the command thread's
	static uint64_t last_ns = 0;
	checked++;
	const auto now = NowNs();
	if (last_ns == 0) {
		last_ns = now;
	}
	if (now - last_ns < 5'000'000'000ull) {
		return;
	}
	last_ns = now;
	std::printf("PREPARE_CHAOS seed=%llu checked=%llu claim=%llu publish=%llu commit_wait=%llu "
	            "adopt=%llu release=%llu slot_reuse=%llu snapshot_retire=%llu decode_post=%llu "
	            "decode_sync=%llu\n",
	            static_cast<unsigned long long>(g_seed.load(std::memory_order_relaxed)),
	            static_cast<unsigned long long>(checked),
	            static_cast<unsigned long long>(Delays(Site::WorkerClaim)),
	            static_cast<unsigned long long>(Delays(Site::WorkerPublish)),
	            static_cast<unsigned long long>(Delays(Site::CommitWait)),
	            static_cast<unsigned long long>(Delays(Site::Adopt)),
	            static_cast<unsigned long long>(Delays(Site::Release)),
	            static_cast<unsigned long long>(Delays(Site::SlotReuse)),
	            static_cast<unsigned long long>(Delays(Site::SnapshotRetire)),
	            static_cast<unsigned long long>(Delays(Site::DecodePost)),
	            static_cast<unsigned long long>(Delays(Site::DecodeSync)));
	std::fflush(stdout);
}
} // namespace Chaos

bool OnWorker() noexcept {
	return t_worker;
}

Log* CurrentLog() noexcept {
	return t_log;
}

void SetLog(Log* log) noexcept {
	t_log = log;
	static const bool pages = ShaderRecompiler::IR::SrtWalkerPageLogEnabled();
	if (pages) {
		ShaderRecompiler::IR::SetSrtWalkerPageLog(log != nullptr ? &log->pages : nullptr);
	}
}

void NoteHazard() noexcept {
	g_hazard_generation.fetch_add(1, std::memory_order_relaxed);
}

uint64_t HazardGeneration() noexcept {
	return g_hazard_generation.load(std::memory_order_relaxed);
}

bool RangedGpuWritesEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RANGED_GPU_HAZARD");
		// Pages are needed: without them the walker's raw loads are not logged.
		return (value == nullptr || std::strcmp(value, "0") != 0) &&
		       ShaderRecompiler::IR::SrtWalkerPageLogEnabled();
	}();
	return enabled;
}

void NoteGpuWrite(uint64_t vaddr, uint64_t size) {
	NoteHazard();
	if (RangedGpuWritesEnabled()) {
		(void)g_gpu_written.Record(vaddr, size);
		// The bytes change under every guest view of them: record the other views too, since
		// adoption compares guest addresses.
		if (LibKernel::Memory::AnyGuestAliases()) {
			thread_local std::vector<std::pair<uint64_t, uint64_t>> aliases;
			aliases.clear();
			LibKernel::Memory::GuestAliasesOf(vaddr, size, aliases);
			for (const auto& [alias, bytes]: aliases) {
				(void)g_gpu_written.Record(alias, bytes);
			}
			AliasStats::Note(AliasStats::GpuWrite, !aliases.empty());
		}
	}
}

uint64_t GpuWriteSequence() noexcept {
	return g_gpu_written.Sequence();
}

bool GpuWrittenSince(uint64_t sequence, const Log& log) {
	return g_gpu_written.Since(sequence, [&](const StoredRange& range) { return LogHit(log, range); });
}

namespace AliasStats {
namespace {
std::array<std::atomic<uint64_t>, Kinds> g_aliased {};
std::array<std::atomic<uint64_t>, Kinds> g_hit {};
std::atomic<uint64_t>                    g_last_report_ns {0};
} // namespace

void NoteAliased(Kind kind, bool hit) noexcept {
	if (!Enabled()) {
		return;
	}
	g_aliased[kind].fetch_add(1, std::memory_order_relaxed);
	if (hit) {
		g_hit[kind].fetch_add(1, std::memory_order_relaxed);
	}
}

void Report() {
	const auto now  = NowNs();
	auto       last = g_last_report_ns.load(std::memory_order_relaxed);
	if (now - last < 5'000'000'000ull ||
	    !g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
		return;
	}
	uint64_t views = 0, ranges = 0, bytes = 0;
	LibKernel::Memory::GuestAliasStats(views, ranges, bytes);
	std::printf("ALIAS_STATS views=%" PRIu64 " aliased_ranges=%" PRIu64 " aliased_bytes=%" PRIu64
	            " gpu_writes_aliased=%" PRIu64 " dirty_checks_aliased=%" PRIu64
	            " dirty_hits_via_alias=%" PRIu64 " ranged_stores_aliased=%" PRIu64
	            " prepared_reads_aliased=%" PRIu64 "\n",
	            views, ranges, bytes, g_aliased[GpuWrite].load(), g_aliased[DirtyCheck].load(),
	            g_hit[DirtyCheck].load(), g_aliased[RangedStore].load(),
	            g_aliased[PreparedRead].load());
	std::fflush(stdout);
}
} // namespace AliasStats

RangedStore::RangedStore(uint64_t vaddr, uint64_t size) noexcept
    : m_outer(GuestSyncEpoch::g_stores_ranged),
      // With the walker's page log, every worker read of guest memory is logged.
      m_ranged((ShaderRecompiler::IR::SrtWalkerPageLogEnabled() ||
                ShaderRecompiler::IR::SrtWalkerReadsLogged(vaddr, size)) &&
               // Recorded by guest range, which readers through another view would miss.
               !LibKernel::Memory::GuestRangeAliased(vaddr, size)) {
	if (m_ranged) {
		m_open = OpenStoredRange(vaddr, size);
	}
	GuestSyncEpoch::g_stores_ranged = m_ranged;
}

RangedStore::~RangedStore() {
	GuestSyncEpoch::g_stores_ranged = m_outer;
	if (m_ranged) {
		CloseStoredRange(m_open);
	}
}

uint64_t OpenStoredRange(uint64_t vaddr, uint64_t size) {
	return g_stored.Open(vaddr, size);
}

void CloseStoredRange(uint64_t open) {
	g_stored.Close(open);
}

uint64_t StoredSequence() noexcept {
	return g_stored.Sequence();
}

bool StoredSince(uint64_t sequence, const std::vector<std::pair<uint64_t, uint64_t>>& reads) {
	return g_stored.Since(sequence, [&](const StoredRange& range) { return ReadsHit(reads, range); });
}

bool StoredSince(uint64_t sequence, const Log& log) {
	return g_stored.Since(sequence, [&](const StoredRange& range) { return LogHit(log, range); });
}

// How long an idle worker spins before it sleeps (KYTY_PREPARE_SPIN_US, default 50): draws
// arrive every few microseconds, and a sleeping worker's first draw after a pause waits for it to
// wake.
static uint64_t SpinUs() {
	static const uint64_t us = [] {
		const char* value = std::getenv("KYTY_PREPARE_SPIN_US");
		return value != nullptr ? std::strtoull(value, nullptr, 10) : 50ull;
	}();
	return us;
}

Pool::Pool(uint32_t slots, uint32_t overflow_k)
    : m_slots(slots), m_overflow_k(overflow_k),
      m_done(std::make_unique<std::atomic<uint64_t>[]>(slots)) {
	for (uint32_t i = 0; i < slots; i++) {
		m_done[i].store(0, std::memory_order_relaxed);
	}
	if (overflow_k != 0) {
		m_claim = std::make_unique<std::atomic<uint64_t>[]>(slots);
		for (uint32_t i = 0; i < slots; i++) {
			m_claim[i].store(0, std::memory_order_relaxed);
		}
	}
}

bool Pool::ClaimSlot(uint64_t sequence) {
	auto& slot    = m_claim[(sequence - 1) % m_slots];
	auto  claimed = slot.load(std::memory_order_relaxed);
	while (claimed < sequence) {
		if (slot.compare_exchange_weak(claimed, sequence, std::memory_order_acq_rel,
		                               std::memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

uint32_t Pool::SleepingWorkers() const {
	uint32_t sleeping = 0;
	for (const auto* worker: m_workers) {
		sleeping += worker->m_sleeping.load(std::memory_order_relaxed) ? 1 : 0;
	}
	return sleeping;
}

uint64_t Pool::Post() {
	// No fence, as Worker::Post: a worker that goes to sleep without seeing this post is woken by
	// Wait. Wake one sleeper only if every worker is busy or asleep.
	const auto sequence = ++m_posted_local;
	EXIT_IF(sequence > m_slots && !Done(sequence - m_slots));
	m_posted.store(sequence, std::memory_order_release);
	if (m_spinning.load(std::memory_order_relaxed) == 0) {
		for (auto* worker: m_workers) {
			if (worker->m_sleeping.load(std::memory_order_relaxed)) {
				worker->Wake();
				break;
			}
		}
	}
	// The overflow worker only when a draw it may take is waiting.
	if (m_overflow != nullptr && m_overflow->m_sleeping.load(std::memory_order_relaxed) &&
	    sequence > m_claimed.load(std::memory_order_relaxed) + m_overflow_k) {
		m_overflow->Wake();
	}
	return sequence;
}

void Pool::WakeAll() {
	for (auto* worker: m_workers) {
		worker->Wake();
	}
}

uint64_t Pool::Wait(uint64_t sequence) {
	if (Done(sequence)) {
		return 0;
	}
	const auto start = NowNs();
	Common::StallDiag::Scope stall(Common::StallDiag::Cause::CmdPrepare);
	WakeAll();
	for (uint32_t spins = 0; !Done(sequence); spins++) {
		if ((spins & 1023u) == 1023u) {
			WakeAll();
		}
		if (spins < 16384) {
			KYTY_PREPARE_PAUSE();
		} else {
			std::this_thread::yield();
		}
	}
	return NowNs() - start;
}

uint64_t Pool::WaitIdle() {
	uint64_t waited = 0;
	// The poster may be another thread (the decode thread): what it has published.
	const auto posted = m_posted.load(std::memory_order_acquire);
	for (auto sequence = posted > m_slots ? posted - m_slots + 1 : 1; sequence <= posted; sequence++) {
		waited += Wait(sequence);
	}
	return waited;
}

Worker::Worker(Run run, void* context, uint32_t index, Pool* pool, bool overflow)
    : m_run(run), m_context(context), m_index(index), m_pool(pool), m_overflow(overflow) {
	if (pool != nullptr && overflow) {
		EXIT_IF(pool->m_overflow != nullptr || pool->m_claim == nullptr);
		pool->m_overflow = this;
	} else if (pool != nullptr) {
		pool->m_workers.push_back(this);
	}
	m_thread = std::thread([this] { Loop(); });
}

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
	if (m_pool != nullptr) {
		if (m_overflow) {
			LoopOverflow();
		} else {
			LoopPool();
		}
		return;
	}
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
				    std::chrono::steady_clock::now() - start > std::chrono::microseconds(SpinUs())) {
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
			Chaos::Point(Chaos::Site::WorkerClaim);
			m_run(m_context, ++done);
			Chaos::Point(Chaos::Site::WorkerPublish);
			m_done.store(done, std::memory_order_release);
		}
		m_busy_ns.fetch_add(NowNs() - start, std::memory_order_relaxed);
	}
}

void Worker::LoopPool() {
	auto& pool = *m_pool;
	for (;;) {
		auto claimed = pool.m_claimed.load(std::memory_order_acquire);
		if (claimed >= pool.m_posted.load(std::memory_order_acquire)) {
			if (m_stop.load(std::memory_order_acquire)) {
				return;
			}
			// Draws arrive every few microseconds: spin a while before sleeping.
			pool.m_spinning.fetch_add(1, std::memory_order_relaxed);
			const auto start = std::chrono::steady_clock::now();
			auto       stall = Common::StallDiag::Now();
			for (uint32_t spins = 1;; spins++) {
				KYTY_PREPARE_PAUSE();
				if (pool.m_claimed.load(std::memory_order_relaxed) <
				        pool.m_posted.load(std::memory_order_acquire) ||
				    m_stop.load(std::memory_order_relaxed)) {
					pool.m_spinning.fetch_sub(1, std::memory_order_relaxed);
					break;
				}
				if ((spins & 255u) == 0 &&
				    std::chrono::steady_clock::now() - start > std::chrono::microseconds(SpinUs())) {
					Common::StallDiag::Add(Common::StallDiag::Cause::WorkerSpin, stall);
					stall = Common::StallDiag::Now();
					pool.m_spinning.fetch_sub(1, std::memory_order_relaxed);
					m_sleeping.store(true, std::memory_order_seq_cst);
					if (pool.m_claimed.load(std::memory_order_seq_cst) >=
					        pool.m_posted.load(std::memory_order_seq_cst) &&
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
		if (!pool.m_claimed.compare_exchange_weak(claimed, claimed + 1, std::memory_order_acq_rel,
		                                          std::memory_order_relaxed)) {
			continue;
		}
		if (pool.m_claim != nullptr && !pool.ClaimSlot(claimed + 1)) {
			continue; // the overflow worker has it
		}
		Chaos::Point(Chaos::Site::WorkerClaim);
		Common::StallDiag::Scope busy(Common::StallDiag::Cause::WorkerBusy);
		const auto start = NowNs();
		m_run(m_context, claimed + 1);
		Chaos::Point(Chaos::Site::WorkerPublish);
		pool.m_done[claimed % pool.m_slots].store(claimed + 1, std::memory_order_release);
		// This worker's job count (BenchMetrics' prep_jobs; nothing waits on it in pool mode).
		m_done.store(m_done.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		m_busy_ns.fetch_add(NowNs() - start, std::memory_order_relaxed);
	}
}

// Takes the first unclaimed draw at least overflow_k past the oldest untaken one; spins briefly
// (KYTY_PREPARE_OVERFLOW_SPIN_US, default 50), then sleeps until Post finds such a draw.
void Worker::LoopOverflow() {
	auto&                 pool    = *m_pool;
	static const uint64_t spin_us = [] {
		const char* value = std::getenv("KYTY_PREPARE_OVERFLOW_SPIN_US");
		return value != nullptr ? std::strtoull(value, nullptr, 10) : 50ull;
	}();
	const auto available = [&] {
		return pool.m_posted.load(std::memory_order_acquire) >
		       pool.m_claimed.load(std::memory_order_acquire) + pool.m_overflow_k;
	};
	for (;;) {
		const auto posted   = pool.m_posted.load(std::memory_order_acquire);
		uint64_t   sequence = 0;
		for (auto s = pool.m_claimed.load(std::memory_order_acquire) + 1 + pool.m_overflow_k;
		     s <= posted; s++) {
			if (pool.ClaimSlot(s)) {
				sequence = s;
				break;
			}
		}
		if (sequence != 0) {
			Chaos::Point(Chaos::Site::WorkerClaim);
			Common::StallDiag::Scope busy(Common::StallDiag::Cause::WorkerBusy);
			const auto start = NowNs();
			m_run(m_context, sequence);
			Chaos::Point(Chaos::Site::WorkerPublish);
			pool.m_done[(sequence - 1) % pool.m_slots].store(sequence, std::memory_order_release);
			m_done.store(m_done.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
			m_busy_ns.fetch_add(NowNs() - start, std::memory_order_relaxed);
			continue;
		}
		if (m_stop.load(std::memory_order_acquire)) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t spins = 1;; spins++) {
			KYTY_PREPARE_PAUSE();
			if (available() || m_stop.load(std::memory_order_relaxed)) {
				break;
			}
			if ((spins & 63u) == 0 &&
			    std::chrono::steady_clock::now() - start > std::chrono::microseconds(spin_us)) {
				m_sleeping.store(true, std::memory_order_seq_cst);
				if (!available() && !m_stop.load(std::memory_order_seq_cst)) {
					m_sleeping.wait(true, std::memory_order_seq_cst);
				}
				m_sleeping.store(false, std::memory_order_relaxed);
				break;
			}
		}
	}
}

} // namespace Libs::Graphics::DrawPrepare
