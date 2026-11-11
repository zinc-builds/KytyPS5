#include "common/benchMetrics.h"

#include "kytyGitVersion.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::BenchMetrics {

#if KYTY_BENCH_METRICS

namespace {

constexpr int RoleCount = static_cast<int>(Role::Count);
constexpr int MaxPmus   = 2; // a hybrid CPU's core and atom PMUs, or one

constexpr std::array<const char*, RoleCount> RoleNames = {"cmd", "prep0", "prep1", "prep2", "rec"};
constexpr std::array<const char*, static_cast<int>(Counter::Count)> CounterNames = {
    "drains",   "drain_wait_ns",   "refill_wait_ns", "steady_wait_ns", "idle_wait_ns",
    "waits",    "rec_wait_ns",     "rec_waits",      "rejected"};

struct ThreadSlot {
	std::atomic<int> tid {0};
	int              group_fd[MaxPmus] {-1, -1}; // leader: cycles; member: instructions
	int              pmus = 0;
};

std::array<ThreadSlot, RoleCount> g_threads;
constexpr int                     MaxWorkers = 3;
std::atomic<const std::atomic<uint64_t>*> g_worker_busy[MaxWorkers] {};
std::atomic<const std::atomic<uint64_t>*> g_worker_done[MaxWorkers] {};

bool PerfLogEnabled() {
	static const bool enabled = std::getenv("KYTY_PERF_LOG") != nullptr;
	return enabled;
}

#if defined(__linux__)
bool CountersEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BENCH_PERF");
		return PerfLogEnabled() && (value == nullptr || value[0] != '0');
	}();
	return enabled;
}

int ReadPmuType(const char* name) {
	char path[128];
	std::snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", name);
	FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return -1;
	}
	int type = -1;
	if (std::fscanf(f, "%d", &type) != 1) {
		type = -1;
	}
	std::fclose(f);
	return type;
}

// Hardware event configs for each PMU: on a hybrid CPU, the PMU type goes in the config's upper
// half (Linux 5.13+); otherwise the plain generic event.
int PmuExtendedTypes(std::array<uint64_t, MaxPmus>& types) {
	static const std::array<int, MaxPmus> hybrid = {ReadPmuType("cpu_core"), ReadPmuType("cpu_atom")};
	int count = 0;
	for (int type: hybrid) {
		if (type >= 0) {
			types[count++] = static_cast<uint64_t>(type) << 32u;
		}
	}
	if (count == 0) {
		types[count++] = 0;
	}
	return count;
}

int OpenCounter(uint64_t config, int group_fd) {
	perf_event_attr attr {};
	attr.size           = sizeof(attr);
	attr.type           = PERF_TYPE_HARDWARE;
	attr.config         = config;
	attr.exclude_kernel = 1;
	attr.exclude_hv     = 1;
	attr.read_format    = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
	                   PERF_FORMAT_TOTAL_TIME_RUNNING;
	return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, group_fd, PERF_FLAG_FD_CLOEXEC));
}

struct GroupRead {
	uint64_t nr = 0, enabled = 0, running = 0;
	uint64_t values[2] {};
};

// Cycles and instructions summed over the PMUs. Each hybrid group runs only while the thread is
// on its core type, so the groups' running times add up to the enabled time unless the counters
// were multiplexed: running/enabled below 1 means the sums are scaled up to cover the gap.
bool ReadCounters(const ThreadSlot& slot, uint64_t& cycles, uint64_t& instructions,
                  double& running_share) {
	cycles = instructions = 0;
	uint64_t enabled = 0, running = 0;
	bool     any     = false;
	for (int p = 0; p < slot.pmus; p++) {
		if (slot.group_fd[p] < 0) {
			continue;
		}
		GroupRead r;
		if (read(slot.group_fd[p], &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r)) || r.nr != 2) {
			continue;
		}
		any = true;
		cycles += r.values[0];
		instructions += r.values[1];
		enabled = std::max(enabled, r.enabled);
		running += r.running;
	}
	running_share = enabled != 0 ? static_cast<double>(running) / static_cast<double>(enabled) : 0.0;
	if (running_share > 0.0 && running_share < 1.0) {
		cycles       = static_cast<uint64_t>(static_cast<double>(cycles) / running_share);
		instructions = static_cast<uint64_t>(static_cast<double>(instructions) / running_share);
	}
	return any;
}

// /proc/self/task/<tid>/stat fields 14 and 15 (after the parenthesised name), in clock ticks.
bool ReadTicks(const char* path, uint64_t& utime, uint64_t& stime) {
	FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return false;
	}
	char buf[1024];
	const auto n = std::fread(buf, 1, sizeof(buf) - 1, f);
	std::fclose(f);
	buf[n]        = '\0';
	const char* p = std::strrchr(buf, ')');
	if (p == nullptr) {
		return false;
	}
	unsigned long long ut = 0, st = 0;
	// state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
	if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) {
		return false;
	}
	utime = ut;
	stime = st;
	return true;
}

// schedstat: on-CPU ns, run-queue wait ns, timeslices.
bool ReadSchedstat(const char* path, uint64_t& cpu_ns, uint64_t& runq_ns) {
	FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return false;
	}
	unsigned long long cpu = 0, runq = 0;
	const bool ok = std::fscanf(f, "%llu %llu", &cpu, &runq) == 2;
	std::fclose(f);
	cpu_ns  = cpu;
	runq_ns = runq;
	return ok;
}

void ReadSwitches(const char* path, uint64_t& voluntary, uint64_t& involuntary) {
	FILE* f = std::fopen(path, "r");
	if (f == nullptr) {
		return;
	}
	char               line[256];
	unsigned long long v = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (std::sscanf(line, "voluntary_ctxt_switches: %llu", &v) == 1) {
			voluntary = v;
		} else if (std::sscanf(line, "nonvoluntary_ctxt_switches: %llu", &v) == 1) {
			involuntary = v;
		}
	}
	std::fclose(f);
}
#endif

void Append(std::string& out, const char* fmt, auto... args) {
	char buf[192];
	const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
	if (n > 0) {
		out.append(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
	}
}

} // namespace

void RegisterThread(Role role) {
#if defined(__linux__)
	if (!PerfLogEnabled()) {
		return;
	}
	auto& slot = g_threads[static_cast<int>(role)];
	// The first thread to take a role keeps it (the counters stay bound to that thread).
	if (slot.tid.load(std::memory_order_relaxed) != 0) {
		return;
	}
	if (CountersEnabled()) {
		std::array<uint64_t, MaxPmus> types {};
		slot.pmus = PmuExtendedTypes(types);
		for (int p = 0; p < slot.pmus; p++) {
			const int leader = OpenCounter(types[p] | PERF_COUNT_HW_CPU_CYCLES, -1);
			if (leader < 0) {
				continue;
			}
			if (OpenCounter(types[p] | PERF_COUNT_HW_INSTRUCTIONS, leader) < 0) {
				close(leader);
				continue;
			}
			slot.group_fd[p] = leader;
		}
	}
	slot.tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
#else
	(void)role;
#endif
}

void RegisterPrepareWorker(uint32_t index, const std::atomic<uint64_t>* busy_ns,
                           const std::atomic<uint64_t>* done) {
	if (index < MaxWorkers) {
		g_worker_busy[index].store(busy_ns, std::memory_order_release);
		g_worker_done[index].store(done, std::memory_order_release);
	}
}

std::string BuildLine() {
	return std::string("BUILD rev=") + KYTY_GIT_REVISION + " hash=" + KYTY_GIT_HASH;
}

std::string Line(uint64_t frames, double t, uint64_t draws) {
	std::string out;
	out.reserve(1024);
	Append(out, "METRICS v=1 frames=%llu t=%.4f draws=%llu", static_cast<unsigned long long>(frames),
	       t, static_cast<unsigned long long>(draws));
	for (int i = 0; i < static_cast<int>(Counter::Count); i++) {
		Append(out, " %s=%llu", CounterNames[i],
		       static_cast<unsigned long long>(Detail::g_counters[i].load(std::memory_order_relaxed)));
	}
	uint64_t busy = 0, jobs = 0;
	for (int i = 0; i < MaxWorkers; i++) {
		const auto* b = g_worker_busy[i].load(std::memory_order_acquire);
		const auto* d = g_worker_done[i].load(std::memory_order_acquire);
		if (b != nullptr && d != nullptr) {
			busy += b->load(std::memory_order_relaxed);
			jobs += d->load(std::memory_order_relaxed);
		}
	}
	Append(out, " prep_busy_ns=%llu prep_jobs=%llu", static_cast<unsigned long long>(busy),
	       static_cast<unsigned long long>(jobs));
#if defined(__linux__)
	timespec ts {};
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	uint64_t pu = 0, ps = 0;
	ReadTicks("/proc/self/stat", pu, ps);
	Append(out, " proc.cpu_ns=%llu proc.ut=%llu proc.st=%llu",
	       static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
	           static_cast<unsigned long long>(ts.tv_nsec),
	       static_cast<unsigned long long>(pu), static_cast<unsigned long long>(ps));
	for (int r = 0; r < RoleCount; r++) {
		const auto& slot = g_threads[r];
		const int   tid  = slot.tid.load(std::memory_order_acquire);
		if (tid == 0) {
			continue;
		}
		char path[96];
		uint64_t cpu_ns = 0, runq_ns = 0, ut = 0, st = 0, vcs = 0, ics = 0;
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
		if (!ReadSchedstat(path, cpu_ns, runq_ns)) {
			continue;
		}
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
		ReadTicks(path, ut, st);
		std::snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
		ReadSwitches(path, vcs, ics);
		const char* name = RoleNames[r];
		Append(out, " %s.cpu_ns=%llu %s.ut=%llu %s.st=%llu %s.runq_ns=%llu %s.vcs=%llu %s.ics=%llu", name,
		       static_cast<unsigned long long>(cpu_ns), name, static_cast<unsigned long long>(ut),
		       name, static_cast<unsigned long long>(st), name,
		       static_cast<unsigned long long>(runq_ns), name, static_cast<unsigned long long>(vcs),
		       name, static_cast<unsigned long long>(ics));
		uint64_t cycles = 0, instructions = 0;
		double   share  = 0.0;
		if (ReadCounters(slot, cycles, instructions, share)) {
			Append(out, " %s.cyc=%llu %s.inst=%llu %s.pmu_run=%.3f", name,
			       static_cast<unsigned long long>(cycles), name,
			       static_cast<unsigned long long>(instructions), name, share);
		}
	}
#endif
	return out;
}

#else // KYTY_BENCH_METRICS

void RegisterThread(Role) {}
void RegisterPrepareWorker(uint32_t, const std::atomic<uint64_t>*, const std::atomic<uint64_t>*) {}
std::string BuildLine() {
	return {};
}
std::string Line(uint64_t, double, uint64_t) {
	return {};
}

#endif // KYTY_BENCH_METRICS

} // namespace Common::BenchMetrics
