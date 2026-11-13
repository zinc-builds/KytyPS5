#include "loader/trapDiag.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <immintrin.h>

namespace Loader::TrapDiag {

namespace Detail {
std::atomic<bool> g_on {false};
} // namespace Detail

} // namespace Loader::TrapDiag

#if defined(__linux__)

#include <sys/syscall.h>
#include <unistd.h>

namespace Loader::TrapDiag {

namespace {

constexpr int      THREADS  = 256;
constexpr uint32_t SITES    = 2048; // per thread, open addressing
constexpr uint32_t PROBES   = 16;

struct Site {
	std::atomic<uint64_t> rip {0};
	std::atomic<uint64_t> count {0};
	std::atomic<uint32_t> bytes {0};
};

struct Slot {
	std::atomic<int>      tid {0};
	std::atomic<uint64_t> traps {0};
	std::atomic<uint64_t> tsc {0};     // handler rdtsc ticks
	std::atomic<uint64_t> overflow {0}; // traps whose site found no free entry
	Site                  sites[SITES];
};

Slot              g_slots[THREADS];
std::atomic<int>  g_slot_count {0};
thread_local Slot* t_slot      = nullptr;
thread_local bool  t_slot_full = false;

Slot* ThreadSlot() noexcept {
	if (t_slot != nullptr || t_slot_full) {
		return t_slot;
	}
	const int index = g_slot_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= THREADS) {
		t_slot_full = true;
		return nullptr;
	}
	auto& slot = g_slots[index];
	slot.tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
	t_slot = &slot;
	return t_slot;
}

// The thread's name and its user and system CPU in clock ticks (/proc/self/task/<tid>/stat).
bool ReadThread(int tid, char* name, size_t name_size, uint64_t& utime, uint64_t& stime) {
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
	FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return false;
	}
	char buf[1024] {};
	const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
	std::fclose(f);
	buf[n]            = '\0';
	const char* open  = std::strchr(buf, '(');
	const char* close = std::strrchr(buf, ')');
	if (open == nullptr || close == nullptr || close < open) {
		return false;
	}
	const size_t len = std::min<size_t>(close - open - 1, name_size - 1);
	std::memcpy(name, open + 1, len);
	name[len] = '\0';
	for (size_t i = 0; i < len; i++) {
		if (name[i] == ' ') name[i] = '_';
	}
	// Fields after the name: state is field 3; utime 14, stime 15.
	unsigned long long u = 0, s = 0;
	if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &u, &s) != 2) {
		return false;
	}
	utime = u;
	stime = s;
	return true;
}

struct Prev {
	uint64_t traps = 0, tsc = 0, utime = 0, stime = 0;
	std::vector<uint64_t> site_counts = std::vector<uint64_t>(SITES, 0);
};

void Reporter(double period) {
	std::vector<Prev> prev(THREADS);
	const double      tick_ns = 1e9 / static_cast<double>(sysconf(_SC_CLK_TCK));
	// rdtsc ticks per ns, measured once.
	const auto     c0 = std::chrono::steady_clock::now();
	const uint64_t r0 = __rdtsc();
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const double tsc_per_ns =
	    static_cast<double>(__rdtsc() - r0) /
	    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count();
	auto last = std::chrono::steady_clock::now();
	for (uint64_t window = 1;; window++) {
		std::this_thread::sleep_for(std::chrono::duration<double>(period));
		const auto   now     = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(now - last).count();
		last                 = now;
		const int    count   = std::min(g_slot_count.load(std::memory_order_acquire), THREADS);
		uint64_t     total   = 0;
		struct SiteRow {
			uint64_t rip, count;
			uint32_t bytes;
			int      slot;
		};
		std::vector<SiteRow> sites;
		for (int i = 0; i < count; i++) {
			auto&     slot = g_slots[i];
			const int tid  = slot.tid.load(std::memory_order_acquire);
			if (tid == 0) continue;
			auto&          p     = prev[i];
			const uint64_t traps = slot.traps.load(std::memory_order_relaxed);
			const uint64_t tsc   = slot.tsc.load(std::memory_order_relaxed);
			char           name[32] = "?";
			uint64_t       ut = p.utime, st = p.stime;
			ReadThread(tid, name, sizeof(name), ut, st);
			const uint64_t d_traps = traps - p.traps;
			total += d_traps;
			if (d_traps != 0) {
				std::printf("TRAPDIAG thread window=%" PRIu64 " s=%.2f tid=%d name=%s traps_s=%.0f "
				            "handler_ns_trap=%.0f handler_ms_s=%.2f user_ms_s=%.1f sys_ms_s=%.1f "
				            "overflow=%" PRIu64 "\n",
				            window, seconds, tid, name, static_cast<double>(d_traps) / seconds,
				            static_cast<double>(tsc - p.tsc) / tsc_per_ns / static_cast<double>(d_traps),
				            static_cast<double>(tsc - p.tsc) / tsc_per_ns / 1e6 / seconds,
				            static_cast<double>(ut - p.utime) * tick_ns / 1e6 / seconds,
				            static_cast<double>(st - p.stime) * tick_ns / 1e6 / seconds,
				            slot.overflow.load(std::memory_order_relaxed));
			}
			p.traps = traps;
			p.tsc   = tsc;
			p.utime = ut;
			p.stime = st;
			for (uint32_t k = 0; k < SITES; k++) {
				const uint64_t rip = slot.sites[k].rip.load(std::memory_order_acquire);
				if (rip == 0) continue;
				const uint64_t c = slot.sites[k].count.load(std::memory_order_relaxed);
				if (c != p.site_counts[k]) {
					sites.push_back({rip, c - p.site_counts[k],
					                 slot.sites[k].bytes.load(std::memory_order_relaxed), i});
					p.site_counts[k] = c;
				}
			}
		}
		// Per site over all threads, with the top threads' share.
		std::sort(sites.begin(), sites.end(),
		          [](const SiteRow& a, const SiteRow& b) { return a.rip < b.rip; });
		struct Agg {
			uint64_t rip, count;
			uint32_t bytes;
			int      threads;
		};
		std::vector<Agg> agg;
		for (const auto& r: sites) {
			if (!agg.empty() && agg.back().rip == r.rip) {
				agg.back().count += r.count;
				agg.back().threads++;
			} else {
				agg.push_back({r.rip, r.count, r.bytes, 1});
			}
		}
		std::sort(agg.begin(), agg.end(), [](const Agg& a, const Agg& b) { return a.count > b.count; });
		std::printf("TRAPDIAG total window=%" PRIu64 " s=%.2f traps_s=%.0f sites=%zu threads=%d\n",
		            window, seconds, static_cast<double>(total) / seconds, agg.size(), count);
		for (size_t k = 0; k < agg.size() && k < 64; k++) {
			std::printf("TRAPDIAG site window=%" PRIu64 " rip=0x%" PRIx64 " bytes=%08x traps_s=%.0f "
			            "threads=%d\n",
			            window, agg[k].rip, agg[k].bytes, static_cast<double>(agg[k].count) / seconds,
			            agg[k].threads);
		}
		// Per (site, thread) for the top sites: who takes them.
		for (size_t k = 0; k < agg.size() && k < 16; k++) {
			for (const auto& r: sites) {
				if (r.rip == agg[k].rip && r.count * 50 >= agg[k].count) {
					std::printf("TRAPDIAG sitethread window=%" PRIu64 " rip=0x%" PRIx64
					            " tid=%d traps_s=%.0f\n",
					            window, r.rip, g_slots[r.slot].tid.load(), static_cast<double>(r.count) / seconds);
				}
			}
		}
		std::fflush(stdout);
	}
}

} // namespace

void Init() {
	static std::atomic<bool> s_once {false};
	if (s_once.exchange(true)) {
		return;
	}
	const char* env = std::getenv("KYTY_TRAP_DIAG");
	if (env == nullptr || *env != '1') {
		return;
	}
	double period = 5.0;
	if (const char* p = std::getenv("KYTY_TRAP_DIAG_PERIOD"); p != nullptr && std::atof(p) > 0.1) {
		period = std::atof(p);
	}
	Detail::g_on.store(true, std::memory_order_relaxed);
	std::thread(Reporter, period).detach();
}

void Note(uint64_t rip, uint32_t bytes, uint64_t start_tsc) noexcept {
	Slot* slot = ThreadSlot();
	if (slot == nullptr) {
		return;
	}
	slot->traps.fetch_add(1, std::memory_order_relaxed);
	uint32_t h = static_cast<uint32_t>((rip * 0x9e3779b97f4a7c15ull) >> 40u) & (SITES - 1);
	bool     found = false;
	for (uint32_t i = 0; i < PROBES && !found; i++, h = (h + 1) & (SITES - 1)) {
		auto&    site = slot->sites[h];
		uint64_t cur  = site.rip.load(std::memory_order_relaxed);
		if (cur == 0) {
			// Only this thread writes its slot: no race on claiming.
			site.bytes.store(bytes, std::memory_order_relaxed);
			site.rip.store(rip, std::memory_order_release);
			cur = rip;
		}
		if (cur == rip) {
			site.count.fetch_add(1, std::memory_order_relaxed);
			found = true;
		}
	}
	if (!found) {
		slot->overflow.fetch_add(1, std::memory_order_relaxed);
	}
	slot->tsc.fetch_add(__rdtsc() - start_tsc, std::memory_order_relaxed);
}

} // namespace Loader::TrapDiag

#else

namespace Loader::TrapDiag {
void Init() {}
void Note(uint64_t /*rip*/, uint32_t /*bytes*/, uint64_t /*start_tsc*/) noexcept {}
} // namespace Loader::TrapDiag

#endif
