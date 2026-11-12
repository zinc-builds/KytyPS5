#include "graphics/host_gpu/renderer/cache/bdaPrestager.h"

#include "common/assert.h"
#include "graphics/host_gpu/pageVersions.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "kernel/memory.h"

#if defined(__linux__)
#include "common/hybridAffinity.h"

#include <pthread.h>
#endif

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Libs::Graphics {

namespace {

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

uint64_t EnvU64(const char* name, uint64_t fallback) {
	const char* value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? std::strtoull(value, nullptr, 10) : fallback;
}

} // namespace

int BdaPrestager::Mode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_BDA_PRESTAGE");
		if (value == nullptr || value[0] == '\0') {
			return 1;
		}
		if (std::strcmp(value, "verify") == 0) {
			return 2;
		}
		if (std::strcmp(value, "strict") == 0) {
			return 3;
		}
		if (std::strcmp(value, "repair") == 0) {
			return 4;
		}
		return value[0] == '0' ? 0 : 1;
	}();
	return mode;
}

BdaPrestager::BdaPrestager(GraphicContext& graphics, CommandScheduler& scheduler,
                           ForEachMapped for_each_mapped)
    : m_scheduler(&scheduler), m_for_each_mapped(std::move(for_each_mapped)) {
	m_mode = Mode();
	if (m_mode == 0 || !PageVersions::Enabled()) {
		return;
	}
	const uint64_t size = std::max<uint64_t>(EnvU64("KYTY_BDA_PRESTAGE_ARENA", 64), 1) << 20u;
	m_arena = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                   vk::BufferUsageFlagBits::eTransferSrc, size, true);
	m_arena_bytes = m_arena->Mapped().data();
	EXIT_IF(m_arena_bytes == nullptr || m_arena->Mapped().size() < size);
	Init(size);
	m_thread = std::thread([this] { Run(); });
	std::printf("BdaPrestager: %" PRIu64 " MiB arena%s%s\n", size >> 20u,
	            m_mode == 2   ? ", verify"
	            : m_mode == 3 ? ", strict"
	            : m_mode == 4 ? ", repair (diagnostic: differing copies are not served)"
	                          : "",
	            m_drop_arm != 0 ? ", DROPPING ARMS (fault injection)" : "");
}

BdaPrestager::BdaPrestager(TestSetup setup): m_test(std::move(setup)) {
	m_mode        = m_test.mode;
	m_arena_bytes = m_test.arena.data();
	Init(m_test.arena.size());
}

void BdaPrestager::Init(uint64_t arena_bytes) {
	m_slot_count = static_cast<uint32_t>(arena_bytes / PageSize);
	m_free_slots.reserve(m_slot_count);
	for (uint32_t slot = m_slot_count; slot-- > 0;) {
		m_free_slots.push_back(slot);
	}
	if (m_mode >= 2) {
		m_shadow.resize(arena_bytes);
	}
	m_drop_arm = static_cast<uint32_t>(EnvU64("KYTY_BDA_PRESTAGE_DROP_ARM", 0));
	m_stats    = m_mode >= 2 || std::getenv("KYTY_BDA_PRESTAGE_STATS") != nullptr;
	m_trace_every = static_cast<uint32_t>(EnvU64("KYTY_BDA_PRESTAGE_TRACE", 0));
	m_active   = true;
}

std::optional<uint64_t> BdaPrestager::Arm(uint64_t vaddr, uint64_t size) {
	return m_test.arm ? m_test.arm(vaddr, size) : LibKernel::Memory::ArmStaged(vaddr, size);
}

uint64_t BdaPrestager::CurrentTick() const {
	return m_scheduler != nullptr ? m_scheduler->CurrentTick() : m_test.current_tick();
}

bool BdaPrestager::IsFree(uint64_t tick) const {
	return m_scheduler != nullptr ? m_scheduler->GetMasterSemaphore().IsFree(tick)
	                              : m_test.is_free(tick);
}

BdaPrestager::~BdaPrestager() {
	if (m_thread.joinable()) {
		{
			std::scoped_lock lock(m_inbox_mutex);
			m_stop = true;
		}
		m_inbox_cv.notify_one();
		m_thread.join();
	}
}

vk::Buffer BdaPrestager::ArenaHandle() const noexcept {
	return m_arena != nullptr ? m_arena->Handle() : nullptr;
}

void BdaPrestager::Enqueue(uint64_t begin, uint64_t end) {
	if (!m_active || begin >= end) {
		return;
	}
	if (m_trace_every != 0) {
		const auto       now = NowNs();
		std::scoped_lock lock(m_mutex);
		for (uint64_t page = begin & ~(PageSize - 1); page < end; page += PageSize) {
			m_trace[page].queued_ns = now;
		}
	}
	{
		std::scoped_lock lock(m_inbox_mutex);
		if (m_stats && m_inbox.empty()) {
			m_inbox_oldest_ns = NowNs();
		}
		m_inbox.emplace_back(begin, end);
	}
	m_inbox_cv.notify_one();
}

void BdaPrestager::Run() {
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyBdaStage");
	Common::HybridAffinity::ApplyToStagerThread();
	{
		std::string cpus;
		for (const int cpu: Common::HybridAffinity::StagerCpus()) {
			cpus += (cpus.empty() ? "" : ",") + std::to_string(cpu);
		}
		std::printf("BdaPrestager: stager thread on CPUs %s%s\n", cpus.empty() ? "(any)" : cpus.c_str(),
		            Common::HybridAffinity::StagerOwnsCores() ? " (taken from the game's set)"
		                                                      : " (shared with the game)");
	}
#endif
	std::vector<std::pair<uint64_t, uint64_t>> work;
	uint64_t                                   oldest_ns = 0;
	for (;;) {
		{
			std::unique_lock lock(m_inbox_mutex);
			m_inbox_cv.wait_for(lock, std::chrono::milliseconds(2),
			                    [this] { return m_stop || !m_inbox.empty(); });
			if (m_stop) {
				return;
			}
			work.swap(m_inbox);
			oldest_ns = m_inbox_oldest_ns;
		}
		// A page queued several times stages once.
		std::sort(work.begin(), work.end());
		uint64_t queued = 0, coalesced = 0;
		size_t   out    = 0;
		for (const auto& range: work) {
			queued += range.second - range.first;
			if (out != 0 && range.first <= work[out - 1].second) {
				work[out - 1].second = std::max(work[out - 1].second, range.second);
			} else {
				work[out++] = range;
			}
		}
		work.resize(out);
		for (const auto& [begin, end]: work) {
			coalesced += end - begin;
			m_for_each_mapped(begin, end - begin,
			                  [this](uint64_t b, uint64_t e) { StageRange(b, e); });
		}
		work.clear();
		if (m_stats && out != 0) {
			const auto lag = NowNs() - oldest_ns;
			m_stat.batches.fetch_add(1, std::memory_order_relaxed);
			m_stat.queued_bytes.fetch_add(queued, std::memory_order_relaxed);
			m_stat.coalesced_bytes.fetch_add(coalesced, std::memory_order_relaxed);
			m_stat.lag_ns.fetch_add(lag, std::memory_order_relaxed);
			if (lag > m_stat.lag_max_ns.load(std::memory_order_relaxed)) {
				m_stat.lag_max_ns.store(lag, std::memory_order_relaxed);
			}
		}
		FlushReleases();
		if (m_stats) {
			ReportIfDue();
		}
	}
}

void BdaPrestager::ReclaimRetired() {
	std::erase_if(m_retired, [this](const std::pair<uint32_t, uint64_t>& retired) {
		if (!IsFree(retired.second)) {
			return false;
		}
		m_free_slots.push_back(retired.first);
		return true;
	});
}

bool BdaPrestager::AllocateSlot(uint32_t& slot) {
	if (m_free_slots.empty()) {
		ReclaimRetired();
	}
	// Full: drop the oldest copy (its page copies at use). Entries hold no GPU reference: a
	// copy in use was taken out of the entries and retired.
	while (m_free_slots.empty() && !m_order.empty()) {
		const auto [page, serial] = m_order.front();
		m_order.pop_front();
		auto it = m_entries.find(page);
		if (it == m_entries.end() || it->second.serial != serial) {
			continue;
		}
		m_free_slots.push_back(it->second.slot);
		m_entries.erase(it);
		m_released.push_back(page);
		m_stat.evicted.fetch_add(1, std::memory_order_relaxed);
		if (m_stats) {
			m_misses[page] = Miss::Evicted;
		}
	}
	if (m_free_slots.empty()) {
		return false;
	}
	slot = m_free_slots.back();
	m_free_slots.pop_back();
	return true;
}

void BdaPrestager::Release(uint64_t page) {
	auto it = m_arm_refs.find(page);
	if (it == m_arm_refs.end() || --it->second != 0) {
		return;
	}
	m_arm_refs.erase(it);
	if (!m_unstage.empty() && m_unstage.back().second == page) {
		m_unstage.back().second += PageSize;
	} else {
		m_unstage.emplace_back(page, page + PageSize);
	}
}

void BdaPrestager::FlushReleases() {
	{
		std::scoped_lock lock(m_mutex);
		m_releasing.swap(m_released);
	}
	std::sort(m_releasing.begin(), m_releasing.end());
	for (const auto page: m_releasing) {
		Release(page);
	}
	m_releasing.clear();
	for (const auto& [begin, end]: m_unstage) {
		LibKernel::Memory::Unstage(begin, end - begin);
	}
	m_unstage.clear();
}

void BdaPrestager::NoteOutcome(uint64_t page, Outcome outcome, uint32_t version) {
	auto& trace      = m_trace[page];
	trace.outcome    = outcome;
	trace.outcome_ns = NowNs();
	trace.version    = version;
}

void BdaPrestager::StageRange(uint64_t begin, uint64_t end) {
	begin = begin & ~(PageSize - 1);
	end   = (end + PageSize - 1) & ~(PageSize - 1);
	for (uint64_t chunk = begin; chunk < end; chunk += ChunkBytes) {
		const auto chunk_end = std::min(end, chunk + ChunkBytes);
		const auto start_ns  = NowNs();
		FlushReleases();
		// Armed before any byte is read: a later store is reported and moves the versions.
		std::optional<uint64_t> generation;
		if (m_drop_arm != 0 && (++m_ranges % m_drop_arm) == 0) {
			generation = LibKernel::Memory::GuestRemapGeneration(); // fault injection: no arm
		} else {
			generation = Arm(chunk, chunk_end - chunk);
		}
		m_stat.arms.fetch_add(1, std::memory_order_relaxed);
		if (!generation) {
			m_stat.refused.fetch_add((chunk_end - chunk) / PageSize, std::memory_order_relaxed);
			if (m_stats) {
				std::scoped_lock lock(m_mutex);
				for (uint64_t page = chunk; page < chunk_end; page += PageSize) {
					if (!m_entries.contains(page)) {
						m_misses[page] = Miss::Refused;
					}
					if (m_trace_every != 0) {
						NoteOutcome(page, Outcome::Refused, 0);
					}
				}
			}
			continue;
		}
		// The arm holds every page of the chunk; each published entry keeps one reference, the
		// rest are released below. The tracker's staged set drops a page when its last goes.
		for (uint64_t page = chunk; page < chunk_end; page += PageSize) {
			m_arm_refs[page]++;
		}
		StageChunk(chunk, chunk_end, *generation);
		FlushReleases();
		m_stat.busy_ns.fetch_add(NowNs() - start_ns, std::memory_order_relaxed);
	}
}

// One chunk (at most ChunkPages pages), taking m_mutex twice: to allocate its slots and to
// publish its entries, so the command thread's Take seldom waits for it.
void BdaPrestager::StageChunk(uint64_t chunk, uint64_t chunk_end, uint64_t generation) {
	struct Page {
		uint64_t address;
		uint32_t version;
		uint32_t slot;
		bool     published;
	};
	std::array<Page, ChunkPages> pages {};
	uint32_t                     count = 0;
	for (uint64_t page = chunk; page < chunk_end; page += PageSize) {
		const auto version = PageVersions::Read(page);
		if ((version & 1u) != 0) {
			m_stat.odd.fetch_add(1, std::memory_order_relaxed);
			if (m_trace_every != 0) {
				std::scoped_lock lock(m_mutex);
				NoteOutcome(page, Outcome::Odd, version);
			}
			Release(page);
			continue;
		}
		pages[count++] = {page, version, 0, false};
	}
	uint32_t allocated = 0;
	uint32_t current   = 0;
	{
		std::scoped_lock lock(m_mutex);
		// A copy already at the page's version and generation stays: nothing to do.
		uint32_t kept = 0;
		for (uint32_t i = 0; i < count; i++) {
			const auto it = m_entries.find(pages[i].address);
			if (it != m_entries.end() && it->second.version == pages[i].version &&
			    it->second.generation == generation) {
				m_released.push_back(pages[i].address); // this arm's reference
				current++;
			} else {
				pages[kept++] = pages[i];
			}
		}
		count = kept;
		while (allocated < count && AllocateSlot(pages[allocated].slot)) {
			allocated++;
		}
	}
	m_stat.current.fetch_add(current, std::memory_order_relaxed);
	if (allocated < count) {
		m_stat.arena_full.fetch_add(count - allocated, std::memory_order_relaxed);
		if (m_trace_every != 0) {
			std::scoped_lock lock(m_mutex);
			for (uint32_t i = allocated; i < count; i++) {
				NoteOutcome(pages[i].address, Outcome::Full, pages[i].version);
			}
		}
		for (uint32_t i = allocated; i < count; i++) {
			Release(pages[i].address);
		}
		count = allocated;
	}
	for (uint32_t i = 0; i < count; i++) {
		auto&      page   = pages[i];
		uint8_t*   target = m_arena_bytes + uint64_t {page.slot} * PageSize;
		uint8_t*   copy   = m_mode >= 2 ? m_shadow.data() + uint64_t {page.slot} * PageSize : target;
		const bool read   = LibKernel::Memory::CopySparseMemory(page.address, copy, PageSize);
		if (read && copy != target) {
			std::memcpy(target, copy, PageSize);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		page.published = read && PageVersions::Read(page.address) == page.version;
		if (!page.published) {
			m_stat.moved_in_copy.fetch_add(1, std::memory_order_relaxed);
		} else if (m_arena != nullptr && !m_arena->IsCoherent()) {
			m_arena->Flush(uint64_t {page.slot} * PageSize, PageSize);
		}
	}
	_mm_sfence(); // write-combined stores land before the entries are published
	const auto now_ns     = m_stats ? NowNs() : 0;
	uint32_t   superseded = 0;
	{
		std::scoped_lock lock(m_mutex);
		for (uint32_t i = 0; i < count; i++) {
			auto& page = pages[i];
			if (m_trace_every != 0) {
				NoteOutcome(page.address, page.published ? Outcome::Staged : Outcome::Moved,
				            page.version);
			}
			if (!page.published) {
				m_free_slots.push_back(page.slot);
				continue;
			}
			auto [it, inserted] = m_entries.try_emplace(page.address);
			if (!inserted) {
				m_free_slots.push_back(it->second.slot); // an unused older copy
				m_released.push_back(page.address);    // its reference, released below
				superseded++;
			}
			it->second = {page.slot, page.version, generation, ++m_serial, m_stats ? now_ns : 0};
			m_order.emplace_back(page.address, m_serial);
			if (m_stats) {
				m_misses.erase(page.address);
			}
		}
	}
	uint32_t staged = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (!pages[i].published) {
			Release(pages[i].address);
		} else {
			staged++;
		}
	}
	m_stat.superseded.fetch_add(superseded, std::memory_order_relaxed);
	m_stat.staged.fetch_add(staged, std::memory_order_relaxed);
}

void BdaPrestager::Take(uint64_t address, uint64_t size, uint64_t dst_offset,
                        std::vector<vk::BufferCopy>&                 staged,
                        std::vector<std::pair<uint64_t, uint64_t>>& fresh) {
	const auto add_fresh = [&](uint64_t piece, uint64_t bytes) {
		if (!fresh.empty() && fresh.back().first + fresh.back().second == piece) {
			fresh.back().second += bytes;
		} else {
			fresh.emplace_back(piece, bytes);
		}
	};
	if (!m_active || size == 0) {
		if (size != 0) {
			add_fresh(address, size);
		}
		return;
	}
	const auto generation = LibKernel::Memory::GuestRemapGeneration();
	const auto tick       = CurrentTick();
	const auto end        = address + size;
	{
		std::scoped_lock lock(m_mutex);
		for (uint64_t page = address & ~(PageSize - 1); page < end; page += PageSize) {
			// The part of this page being uploaded (a buffer need not start or end on a page).
			const auto piece       = std::max(page, address);
			const auto piece_bytes = std::min(end, page + PageSize) - piece;
			if (m_mode >= 2 && !m_marked.empty()) {
				if (auto marked = m_marked.find(page); marked != m_marked.end()) {
					if (PageVersions::Read(page) != marked->second) {
						m_stat.loop_closed.fetch_add(1, std::memory_order_relaxed);
					} else {
						m_stat.loop_open.fetch_add(1, std::memory_order_relaxed);
						std::printf("BDAPRESTAGE_LOOP_OPEN page=0x%016" PRIx64
						            " version=%u: used again with no newer version after a marked "
						            "mismatch\n",
						            page, marked->second);
						if (m_mode == 3) {
							EXIT("KYTY_BDA_PRESTAGE=strict: page 0x%016" PRIx64
							     " was used again without the pending store being reported\n",
							     page);
						}
					}
					m_marked.erase(marked);
				}
			}
			auto       it          = m_entries.find(page);
			if (it == m_entries.end()) {
				auto reason = Miss::None;
				if (m_stats) {
					if (auto miss = m_misses.find(page); miss != m_misses.end()) {
						reason = miss->second;
						m_misses.erase(miss);
					}
				}
				(reason == Miss::Refused   ? m_stat.fresh_refused
				 : reason == Miss::Evicted ? m_stat.fresh_evicted
				                           : m_stat.fresh_missing)
				    .fetch_add(1, std::memory_order_relaxed);
				add_fresh(piece, piece_bytes);
				continue;
			}
			const auto entry = it->second;
			m_entries.erase(it);
			m_released.push_back(page); // the stager drops its arm (only it arms and unstages)
			const auto version = PageVersions::Read(page);
			if (version != entry.version || entry.generation != generation) {
				(version != entry.version ? m_stat.fresh_version : m_stat.fresh_generation)
				    .fetch_add(1, std::memory_order_relaxed);
				if (m_stats && version != entry.version) {
					const auto age   = NowNs() - entry.staged_ns;
					const auto moved = version - entry.version;
					m_stat.moved_age[age < 100'000 ? 0 : age < 1'000'000 ? 1 : age < 10'000'000 ? 2 : 3]
					    .fetch_add(1, std::memory_order_relaxed);
					m_stat.moved_by[moved == 1 ? 0 : moved == 2 ? 1 : moved <= 6 ? 2 : 3].fetch_add(
					    1, std::memory_order_relaxed);
					if (m_trace_every != 0 && (++m_trace_count % m_trace_every) == 0) {
						static const char* names[] = {"none", "staged", "refused", "odd", "moved", "full"};
						const auto now   = NowNs();
						const auto trace = m_trace[page];
						std::printf("BDAPRESTAGE_TRACE page=0x%" PRIx64 " entry_v=%u now_v=%u "
						            "entry_age_ms=%.2f queued_ago_ms=%.2f last=%s@v%u %.2f ms ago\n",
						            page, entry.version, version, static_cast<double>(age) / 1e6,
						            trace.queued_ns != 0
						                ? static_cast<double>(now - trace.queued_ns) / 1e6
						                : -1.0,
						            names[static_cast<int>(trace.outcome)], trace.version,
						            trace.outcome_ns != 0
						                ? static_cast<double>(now - trace.outcome_ns) / 1e6
						                : -1.0);
					}
				}
				m_free_slots.push_back(entry.slot);
				add_fresh(piece, piece_bytes);
				continue;
			}
			if (m_mode >= 2) {
				// The whole page, not only the piece: an unchanged version vouches for all of it.
				alignas(64) static uint8_t now[PageSize];
				if (LibKernel::Memory::CopySparseMemory(page, now, PageSize)) {
					m_stat.compared.fetch_add(1, std::memory_order_relaxed);
					if (std::memcmp(now, m_shadow.data() + uint64_t {entry.slot} * PageSize,
					                PageSize) != 0) {
						m_stat.mismatched.fetch_add(1, std::memory_order_relaxed);
						{
							// Sample: where the bytes differ, how old the copy is and what the
							// stager last did with the page.
							const auto* staged_bytes =
							    m_shadow.data() + uint64_t {entry.slot} * PageSize;
							uint32_t first = PageSize, last = 0, bytes = 0;
							for (uint32_t i = 0; i < PageSize; i++) {
								if (now[i] != staged_bytes[i]) {
									first = std::min(first, i);
									last  = i;
									bytes++;
								}
							}
							const auto trace = m_trace.find(page);
							// A write mark not yet taken: the store is after the last scan, so a
							// later collection reports it (a store for later work); none: unexplained.
							std::vector<std::pair<uint64_t, uint64_t>> pending;
							LibKernel::Memory::PeekAsyncGuestWrites(pending);
							const bool marked = std::any_of(
							    pending.begin(), pending.end(), [&](const auto& range) {
								    return range.first < page + PageSize && page < range.second;
							    });
							(marked ? m_stat.mismatched_marked : m_stat.mismatched_unmarked)
							    .fetch_add(1, std::memory_order_relaxed);
							if (marked) {
								m_marked[page] = version;
							} else if (m_mode == 3) {
								EXIT("KYTY_BDA_PRESTAGE=strict: the staged copy of page 0x%016" PRIx64
								     " differs from guest memory at use with no pending write mark\n",
								     page);
							}
							std::printf("BDAPRESTAGE_MISMATCH buffer=0x%016" PRIx64 "+0x%" PRIx64
							            " page=0x%016" PRIx64
							            " version=%u generation=%" PRIu64 " age_ms=%.2f "
							            "differing_bytes=%u first=0x%x last=0x%x staged=%02x%02x%02x%02x "
							            "now=%02x%02x%02x%02x queued_ago_ms=%.2f pending_mark=%d\n",
							            address - dst_offset, page - (address - dst_offset), page,
							            version, generation,
							            entry.staged_ns != 0
							                ? static_cast<double>(NowNs() - entry.staged_ns) / 1e6
							                : -1.0,
							            bytes, first, last, staged_bytes[first & ~3u],
							            staged_bytes[(first & ~3u) + 1], staged_bytes[(first & ~3u) + 2],
							            staged_bytes[(first & ~3u) + 3], now[first & ~3u],
							            now[(first & ~3u) + 1], now[(first & ~3u) + 2],
							            now[(first & ~3u) + 3],
							            trace != m_trace.end() && trace->second.queued_ns != 0
							                ? static_cast<double>(NowNs() - trace->second.queued_ns) /
							                      1e6
							                : -1.0,
							            marked ? 1 : 0);
						}
						if (m_mode == 4) { // repair (diagnostic): never serve a differing copy
							m_free_slots.push_back(entry.slot);
							add_fresh(piece, piece_bytes);
							continue;
						}

					}
				}
			}
			m_retired.emplace_back(entry.slot, tick);
			m_stat.served.fetch_add(1, std::memory_order_relaxed);
			const vk::BufferCopy copy {uint64_t {entry.slot} * PageSize + (piece - page),
			                           dst_offset + (piece - address), piece_bytes};
			if (!staged.empty() && staged.back().srcOffset + staged.back().size == copy.srcOffset &&
			    staged.back().dstOffset + staged.back().size == copy.dstOffset) {
				staged.back().size += copy.size;
			} else {
				staged.push_back(copy);
			}
		}
	}
}

void BdaPrestager::ReportIfDue() {
	const auto now = NowNs();
	if (m_report_start == 0) {
		m_report_start = now;
		return;
	}
	if (now - m_report_start < 1'000'000'000ull) {
		return;
	}
	const double seconds = static_cast<double>(now - m_report_start) / 1e9;
	const auto   take    = [](std::atomic<uint64_t>& value) {
        return value.exchange(0, std::memory_order_relaxed);
	};
	const auto served = take(m_stat.served), missing = take(m_stat.fresh_missing),
	           refused = take(m_stat.fresh_refused), evicted = take(m_stat.fresh_evicted),
	           version = take(m_stat.fresh_version), generation = take(m_stat.fresh_generation);
	const auto uses   = served + missing + refused + evicted + version + generation;
	const auto pct    = [&](uint64_t value) {
        return uses != 0 ? 100.0 * static_cast<double>(value) / static_cast<double>(uses) : 0.0;
	};
	const auto staged = take(m_stat.staged), superseded = take(m_stat.superseded),
	           evicted_staged = take(m_stat.evicted), moved = take(m_stat.moved_in_copy);
	const auto compared = take(m_stat.compared), mismatched = take(m_stat.mismatched);
	std::printf(
	    "BDAPRESTAGE served=%.1f%% of %.0f MB/s | fallback: not_staged=%.1f%% refused=%.1f%% "
	    "evicted=%.1f%% version=%.1f%% generation=%.1f%% | stager: staged_mb_s=%.0f "
	    "wasted_mb_s=%.0f (superseded %.0f, evicted %.0f, moved_in_copy %.0f) refused_pages_s=%.0f "
	    "odd_s=%.0f arena_full_s=%.0f arms_s=%.0f busy=%.1f%% | verify: compared_s=%.0f "
	    "mismatched=%" PRIu64 " (pending_mark %" PRIu64 ", unmarked %" PRIu64 ") loop_closed=%" PRIu64
	    " loop_open=%" PRIu64 "\n",
	    pct(served), static_cast<double>(uses * PageSize) / 1e6 / seconds, pct(missing),
	    pct(refused), pct(evicted), pct(version), pct(generation),
	    static_cast<double>(staged * PageSize) / 1e6 / seconds,
	    static_cast<double>((superseded + evicted_staged + moved) * PageSize) / 1e6 / seconds,
	    static_cast<double>(superseded * PageSize) / 1e6 / seconds,
	    static_cast<double>(evicted_staged * PageSize) / 1e6 / seconds,
	    static_cast<double>(moved * PageSize) / 1e6 / seconds,
	    static_cast<double>(take(m_stat.refused)) / seconds,
	    static_cast<double>(take(m_stat.odd)) / seconds,
	    static_cast<double>(take(m_stat.arena_full)) / seconds,
	    static_cast<double>(take(m_stat.arms)) / seconds,
	    100.0 * static_cast<double>(take(m_stat.busy_ns)) / (seconds * 1e9),
	    static_cast<double>(compared) / seconds, mismatched, take(m_stat.mismatched_marked),
	    take(m_stat.mismatched_unmarked), take(m_stat.loop_closed), take(m_stat.loop_open));
	uint64_t age[4], by[4];
	for (int i = 0; i < 4; i++) {
		age[i] = take(m_stat.moved_age[i]);
		by[i]  = take(m_stat.moved_by[i]);
	}
	const auto batches = take(m_stat.batches);
	std::printf("BDAPRESTAGE_INBOX batches_s=%.0f queued_mb_s=%.0f coalesced_mb_s=%.0f "
	            "current_pages_s=%.0f lag_ms mean=%.2f max=%.2f\n",
	            static_cast<double>(batches) / seconds,
	            static_cast<double>(take(m_stat.queued_bytes)) / 1e6 / seconds,
	            static_cast<double>(take(m_stat.coalesced_bytes)) / 1e6 / seconds,
	            static_cast<double>(take(m_stat.current)) / seconds,
	            batches != 0 ? static_cast<double>(take(m_stat.lag_ns)) / 1e6 /
	                               static_cast<double>(batches)
	                         : 0.0,
	            static_cast<double>(take(m_stat.lag_max_ns)) / 1e6);
	std::printf("BDAPRESTAGE_MOVED age<100us/1ms/10ms/more=%" PRIu64 "/%" PRIu64 "/%" PRIu64
	            "/%" PRIu64 " by=1/2/3-6/more=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
	            age[0], age[1], age[2], age[3], by[0], by[1], by[2], by[3]);
	std::fflush(stdout);
	m_report_start = now;
}

} // namespace Libs::Graphics
