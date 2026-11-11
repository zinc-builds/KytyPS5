#ifndef KYTY_COMMON_KERNEL_STATS_H_
#define KYTY_COMMON_KERNEL_STATS_H_

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif
#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// KYTY_KERNEL_STATS=1: kernel time on the GPU command thread by user call site. Every system
// call the emulator makes on purpose (PAGEMAP_SCAN, userfaultfd write protection, mprotect,
// futex wakes, sched_yield) is timed with rdtsc and charged to the innermost site scope open on
// the command thread; signal handlers are counted with their handler time; each site also gets
// its inclusive wall time, which covers the page faults its stores take. Once per second a
// KSTATS line on stdout gives, per site, calls/s and µs/s of each kind, next to the thread's
// sys time from /proc/self/task/<tid>/stat (tick-sampled; compare over several seconds). Other
// threads record nothing.
namespace Common::KernelStats {

enum class Site : uint8_t {
	None,              // outside every scope below
	LabelStore,        // RenderContext::StoreCpLabel (ProtectAsyncPage + backing store)
	LabelPeek,         // RenderContext::IsCpLabel (AsyncPageUntouched)
	CollectScan,       // CollectAsyncWrites: the PAGEMAP_SCAN pass
	CollectInvalidate, // CollectAsyncWrites: invalidating the written pages
	Fill,              // BufferCache::FillBuffer through the guest mapping
	Promote,           // promoting chunks to asynchronous write protection
	Watch,             // page-tracker protection changes (watchers added or removed)
	SrtWalk,           // PipelineCache::Program::FindModule on the command thread (SRT walk)
	Command,           // another thread's synchronous command run on the command thread
	PrepareWait,       // waiting for a prepare worker's draw
	RecorderWait,      // waiting for recorder ring space or a drain
	GpuWait,           // waiting for a GPU timeline value
	SchedulerWait,     // waiting for the scheduler's completion callbacks
	Idle,              // no runnable work (blocked on the queue)
	Count
};

enum class Kind : uint8_t {
	Region,  // the scope's own inclusive wall time (calls = scope entries)
	Scan,    // ioctl(PAGEMAP_SCAN) that found no written page
	ScanHit, // ioctl(PAGEMAP_SCAN) that found written pages (and re-protected them, if asked)
	UffdWp,  // ioctl(UFFDIO_WRITEPROTECT)
	UffdReg, // ioctl(UFFDIO_REGISTER / UNREGISTER)
	Mprotect,
	Wake,   // futex wake (atomic notify)
	Yield,  // sched_yield
	Signal, // SIGSEGV/SIGBUS/SIGILL handler (handler time only; delivery is not visible)
	Count
};

namespace Detail {
constexpr int SITES = static_cast<int>(Site::Count);
constexpr int KINDS = static_cast<int>(Kind::Count);

struct Counters {
	uint64_t calls[SITES][KINDS];
	uint64_t cycles[SITES][KINDS];
	uint64_t units[SITES][KINDS]; // kind-specific amount (PAGEMAP_SCAN: pages walked)
};

// Plain thread-local storage (no constructor): only the command thread writes or reads it,
// including from its own signal handlers.
inline thread_local bool     t_on        = false;
inline thread_local Site     t_site      = Site::None;
inline thread_local uint32_t t_in_signal = 0;
inline thread_local Counters t_counters;

[[nodiscard]] inline uint64_t Cycles() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	return __rdtsc();
#else
	return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline void Record(Site site, Kind kind, uint64_t cycles) noexcept {
	const auto s = static_cast<int>(site);
	const auto k = static_cast<int>(kind);
	t_counters.calls[s][k]++;
	t_counters.cycles[s][k] += cycles;
}
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_KERNEL_STATS");
		return value != nullptr && value[0] != '0';
	}();
	return enabled;
}

// Called once by the GPU command thread.
inline void SetCommandThread() noexcept {
	Detail::t_on = Enabled();
}

[[nodiscard]] inline bool Active() noexcept {
	return Detail::t_on;
}

// A user call site: system calls inside it are charged to it.
class SiteScope final {
public:
	explicit SiteScope(Site site) noexcept {
		if (Detail::t_on) {
			m_on           = true;
			m_previous     = Detail::t_site;
			Detail::t_site = site;
			m_start        = Detail::Cycles();
		}
	}
	~SiteScope() {
		if (m_on) {
			Detail::Record(Detail::t_site, Kind::Region, Detail::Cycles() - m_start);
			Detail::t_site = m_previous;
		}
	}
	SiteScope(const SiteScope&)            = delete;
	SiteScope& operator=(const SiteScope&) = delete;

private:
	bool     m_on       = false;
	Site     m_previous = Site::None;
	uint64_t m_start    = 0;
};

// Adds a kind-specific amount (pages walked by a PAGEMAP_SCAN) to the current site.
inline void AddUnits(Kind kind, uint64_t units) noexcept {
	if (Detail::t_on && Detail::t_in_signal == 0) {
		Detail::t_counters.units[static_cast<int>(Detail::t_site)][static_cast<int>(kind)] += units;
	}
}

// One system call (or a signal handler) of the given kind.
class Syscall final {
public:
	explicit Syscall(Kind kind) noexcept {
		if (Detail::t_on && Detail::t_in_signal == 0) {
			m_kind  = kind;
			m_on    = true;
			m_start = Detail::Cycles();
			if (kind == Kind::Signal) {
				Detail::t_in_signal++;
			}
		}
	}
	~Syscall() {
		if (m_on) {
			if (m_kind == Kind::Signal) {
				Detail::t_in_signal--;
			}
			Detail::Record(Detail::t_site, m_kind, Detail::Cycles() - m_start);
		}
	}
	Syscall(const Syscall&)            = delete;
	Syscall& operator=(const Syscall&) = delete;

	// Reclassifies the call once its result is known.
	void SetKind(Kind kind) noexcept { m_kind = kind; }

private:
	bool     m_on    = false;
	Kind     m_kind  = Kind::Region;
	uint64_t m_start = 0;
};

namespace Detail {
struct ThreadStat {
	uint64_t utime = 0, stime = 0, minflt = 0, majflt = 0, run_ns = 0, vcsw = 0, ivcsw = 0;
};

[[nodiscard]] inline bool ReadFile(const char* path, char* buffer, size_t size) noexcept {
#if defined(__linux__)
	const int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	const auto n = read(fd, buffer, size - 1);
	close(fd);
	if (n <= 0) {
		return false;
	}
	buffer[n] = '\0';
	return true;
#else
	(void)path;
	(void)buffer;
	(void)size;
	return false;
#endif
}

[[nodiscard]] inline ThreadStat ReadThreadStat() noexcept {
	ThreadStat stat;
#if defined(__linux__)
	const auto tid = static_cast<long>(syscall(SYS_gettid));
	char       path[64];
	char       buffer[2048];
	std::snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
	if (ReadFile(path, buffer, sizeof(buffer))) {
		// Fields after the command name (which may hold spaces): state is field 3.
		const char*        p = std::strrchr(buffer, ')');
		unsigned long long v[13] {};
		if (p != nullptr &&
		    std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu %*u %llu %llu", &v[0],
		                &v[1], &v[2], &v[3]) == 4) {
			stat.minflt = v[0];
			stat.majflt = v[1];
			stat.utime  = v[2];
			stat.stime  = v[3];
		}
	}
	std::snprintf(path, sizeof(path), "/proc/self/task/%ld/schedstat", tid);
	if (ReadFile(path, buffer, sizeof(buffer))) {
		unsigned long long run = 0;
		if (std::sscanf(buffer, "%llu", &run) == 1) {
			stat.run_ns = run;
		}
	}
	std::snprintf(path, sizeof(path), "/proc/self/task/%ld/status", tid);
	if (ReadFile(path, buffer, sizeof(buffer))) {
		unsigned long long value = 0;
		if (const char* v = std::strstr(buffer, "\nvoluntary_ctxt_switches:");
		    v != nullptr && std::sscanf(v + 25, "%llu", &value) == 1) {
			stat.vcsw = value;
		}
		if (const char* v = std::strstr(buffer, "\nnonvoluntary_ctxt_switches:");
		    v != nullptr && std::sscanf(v + 28, "%llu", &value) == 1) {
			stat.ivcsw = value;
		}
	}
#endif
	return stat;
}

inline const char* SiteName(int site) noexcept {
	static constexpr const char* names[] = {
	    "none",         "label_store",   "label_peek", "collect_scan", "collect_inval",
	    "fill",         "promote",       "watch",      "srt_walk",     "command",
	    "prepare_wait", "recorder_wait", "gpu_wait",   "sched_wait",   "idle"};
	static_assert(sizeof(names) / sizeof(names[0]) == SITES);
	return names[site];
}

inline const char* KindName(int kind) noexcept {
	static constexpr const char* names[] = {"region",   "scan", "scan_hit", "uffd_wp", "uffd_reg",
	                                        "mprotect", "wake", "yield",    "signal"};
	static_assert(sizeof(names) / sizeof(names[0]) == KINDS);
	return names[kind];
}
} // namespace Detail

// Called by the GPU command thread; prints and resets the counters once per second.
inline void Report() noexcept {
	if (!Detail::t_on) {
		return;
	}
	using namespace Detail;
	static thread_local uint64_t   start_ns     = 0;
	static thread_local uint64_t   start_cycles = 0;
	static thread_local ThreadStat start_stat;
	const auto                     now_ns =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                              std::chrono::steady_clock::now().time_since_epoch())
		                          .count());
	if (start_ns == 0) {
		start_ns     = now_ns;
		start_cycles = Cycles();
		start_stat   = ReadThreadStat();
		std::memset(&t_counters, 0, sizeof(t_counters));
		return;
	}
	if (now_ns - start_ns < 1'000'000'000ull) {
		return;
	}
	const auto now_cycles = Cycles();
	const auto stat       = ReadThreadStat();
	const auto ns         = static_cast<double>(now_ns - start_ns);
	// Values are per second of wall time; cycles convert at the measured TSC rate.
	const double per_s     = 1e9 / ns;
	const double us_per_cy = ns / 1e3 / static_cast<double>(now_cycles - start_cycles);
#if defined(__linux__)
	const double tick_ms = 1000.0 / static_cast<double>(sysconf(_SC_CLK_TCK));
#else
	const double tick_ms = 10.0; // no /proc: the thread columns read 0
#endif

	double measured_sys = 0;
	for (int s = 0; s < SITES; s++) {
		for (int k = 1; k < KINDS; k++) {
			measured_sys += static_cast<double>(t_counters.cycles[s][k]);
		}
	}
	char line[8192];
	int  n = std::snprintf(
	    line, sizeof(line),
	    "KSTATS dt=%.3f run_ms=%.1f utime_ms=%.1f stime_ms=%.1f minflt=%.0f majflt=%.0f vcsw=%.0f "
	    "ivcsw=%.0f measured_ms=%.2f |",
	    ns / 1e9, static_cast<double>(stat.run_ns - start_stat.run_ns) / 1e6 * per_s,
	    static_cast<double>(stat.utime - start_stat.utime) * tick_ms * per_s,
	    static_cast<double>(stat.stime - start_stat.stime) * tick_ms * per_s,
	    static_cast<double>(stat.minflt - start_stat.minflt) * per_s,
	    static_cast<double>(stat.majflt - start_stat.majflt) * per_s,
	    static_cast<double>(stat.vcsw - start_stat.vcsw) * per_s,
	    static_cast<double>(stat.ivcsw - start_stat.ivcsw) * per_s,
	    measured_sys * us_per_cy / 1e3 * per_s);
	for (int s = 0; s < SITES && n > 0 && n < static_cast<int>(sizeof(line)) - 256; s++) {
		bool any = false;
		for (int k = 0; k < KINDS; k++) {
			any = any || t_counters.calls[s][k] != 0;
		}
		if (!any) {
			continue;
		}
		n += std::snprintf(line + n, sizeof(line) - n, " %s", SiteName(s));
		for (int k = 0; k < KINDS && n < static_cast<int>(sizeof(line)) - 64; k++) {
			if (t_counters.calls[s][k] == 0) {
				continue;
			}
			// kind=calls/s:us/s[:units/s]
			n += std::snprintf(line + n, sizeof(line) - n, " %s=%.0f:%.0f", KindName(k),
			                   static_cast<double>(t_counters.calls[s][k]) * per_s,
			                   static_cast<double>(t_counters.cycles[s][k]) * us_per_cy * per_s);
			if (t_counters.units[s][k] != 0) {
				n += std::snprintf(line + n, sizeof(line) - n, ":%.0f",
				                   static_cast<double>(t_counters.units[s][k]) * per_s);
			}
		}
		n += std::snprintf(line + n, sizeof(line) - n, " ;");
	}
	std::printf("%s\n", line);
	std::fflush(stdout);
	std::memset(&t_counters, 0, sizeof(t_counters));
	start_ns     = now_ns;
	start_cycles = now_cycles;
	start_stat   = stat;
}

} // namespace Common::KernelStats

#endif /* KYTY_COMMON_KERNEL_STATS_H_ */
