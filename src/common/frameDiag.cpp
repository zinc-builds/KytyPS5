#include "common/frameDiag.h"

#include "common/benchMetrics.h"
#include "common/debugCounters.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace Common::FrameDiag {

namespace {

using DC = Common::DebugCounters::Counter;
using BM = Common::BenchMetrics::Counter;
using Role = Common::BenchMetrics::Role;

struct ThreadStat {
	uint64_t cpu_ns = 0, runq_ns = 0, minflt = 0, majflt = 0, vcs = 0, ics = 0;
};

// CPU time of a thread of this process by its id (the kernel's per-thread scheduler clock).
uint64_t ThreadCpuNs(int tid) {
#if defined(__linux__)
	if (tid <= 0) {
		return 0;
	}
	const clockid_t id = static_cast<clockid_t>((~static_cast<unsigned>(tid)) << 3u) | 6;
	timespec        ts {};
	if (clock_gettime(id, &ts) != 0) {
		return 0;
	}
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
#else
	(void)tid;
	return 0;
#endif
}

// The command thread's run-queue delay, faults and context switches (/proc, three small reads).
ThreadStat ReadThread(int tid) {
	ThreadStat out;
	out.cpu_ns = ThreadCpuNs(tid);
#if defined(__linux__)
	if (tid <= 0) {
		return out;
	}
	char path[96];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
	if (FILE* f = std::fopen(path, "r"); f != nullptr) {
		unsigned long long run = 0, wait = 0;
		if (std::fscanf(f, "%llu %llu", &run, &wait) == 2) {
			out.runq_ns = wait;
		}
		std::fclose(f);
	}
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
	if (FILE* f = std::fopen(path, "r"); f != nullptr) {
		char buf[1024];
		const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
		std::fclose(f);
		buf[n]           = '\0';
		const char* rest = std::strrchr(buf, ')');
		if (rest != nullptr) {
			// After ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt
			unsigned long long minflt = 0, majflt = 0;
			if (std::sscanf(rest + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu", &minflt, &majflt) == 2) {
				out.minflt = minflt;
				out.majflt = majflt;
			}
		}
	}
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
	if (FILE* f = std::fopen(path, "r"); f != nullptr) {
		char line[256];
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			unsigned long long v = 0;
			if (std::sscanf(line, "voluntary_ctxt_switches: %llu", &v) == 1) {
				out.vcs = v;
			} else if (std::sscanf(line, "nonvoluntary_ctxt_switches: %llu", &v) == 1) {
				out.ics = v;
			}
		}
		std::fclose(f);
	}
#endif
	return out;
}

constexpr std::array DebugFields {
    std::pair {DC::Draws, "draws"},
    std::pair {DC::Dispatches, "disp"},
    std::pair {DC::QueueSubmits, "submits"},
    std::pair {DC::GpuWaits, "gpu_waits"},
    std::pair {DC::GpuWaitNs, "gpu_wait_ns"},
    std::pair {DC::GpuFaults, "wt_faults"},
    std::pair {DC::ShadersCompiled, "shaders"},
    std::pair {DC::ShaderCompileNs, "shader_ns"},
    std::pair {DC::PipelinesCreated, "pipelines"},
    std::pair {DC::PipelineCreateNs, "pipeline_ns"},
    std::pair {DC::TextureUploadBytes, "tex_up_b"},
    std::pair {DC::TextureDownloadBytes, "tex_dn_b"},
    std::pair {DC::TextureEvictions, "tex_evict"},
    std::pair {DC::ColorMetadataReadbacks, "meta_rb"},
    std::pair {DC::ColorMetadataReadbackNs, "meta_rb_ns"},
    std::pair {DC::BufferUploadBytes, "buf_up_b"},
    std::pair {DC::BufferDownloadBytes, "buf_dn_b"},
    std::pair {DC::BufferEvictions, "buf_evict"},
    std::pair {DC::FlipsRejected, "flips_rejected"},
    std::pair {DC::LateVblanks, "late_vbl"},
    std::pair {DC::DroppedPresents, "dropped"},
    std::pair {DC::EmulatedInstructions, "sigill"},
    std::pair {DC::GuestFileReadBytes, "file_rd_b"},
};
constexpr std::array OwnNames {"mutex_wait_ns", "mutex_waits", "gc", "gc_ns", "collects", "collect_ns",
                               "submit_scans", "submit_scan_ns", "tex_new", "buf_new", "cmd_gpu_waits",
                               "cmd_gpu_wait_ns"};
static_assert(OwnNames.size() == static_cast<size_t>(Counter::Count));

struct Totals {
	std::array<uint64_t, DebugFields.size()>                 debug {};
	std::array<uint64_t, static_cast<size_t>(Counter::Count)> own {};
	ThreadStat                                               cmd;
	uint64_t prep_wait_ns = 0, rec_wait_ns = 0, rejected = 0, drains = 0;
	uint64_t prep_busy_ns = 0, prep_cpu_ns = 0, rec_cpu_ns = 0;
	uint64_t proc_minflt = 0, proc_majflt = 0;
};

// On the command thread itself, its own clock and rusage are cheaper than /proc.
ThreadStat ReadSelf() {
	ThreadStat out;
#if defined(__linux__)
	timespec ts {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
		out.cpu_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
	}
	rusage usage {};
	if (getrusage(RUSAGE_THREAD, &usage) == 0) {
		out.minflt = static_cast<uint64_t>(usage.ru_minflt);
		out.majflt = static_cast<uint64_t>(usage.ru_majflt);
		out.vcs    = static_cast<uint64_t>(usage.ru_nvcsw);
		out.ics    = static_cast<uint64_t>(usage.ru_nivcsw);
	}
	if (FILE* f = std::fopen("/proc/thread-self/schedstat", "r"); f != nullptr) {
		unsigned long long run = 0, wait = 0;
		if (std::fscanf(f, "%llu %llu", &run, &wait) == 2) {
			out.runq_ns = wait;
		}
		std::fclose(f);
	}
#endif
	return out;
}

Totals Read(bool self) {
	Totals t;
	for (size_t i = 0; i < DebugFields.size(); i++) {
		t.debug[i] = Common::DebugCounters::Get(DebugFields[i].first);
	}
	for (size_t i = 0; i < t.own.size(); i++) {
		t.own[i] = Detail::g_counters[i].load(std::memory_order_relaxed);
	}
	namespace B = Common::BenchMetrics;
	t.cmd          = self ? ReadSelf() : ReadThread(B::ThreadId(Role::Cmd));
	t.prep_wait_ns = B::Value(BM::DrainWaitNs) + B::Value(BM::RefillWaitNs) +
	                 B::Value(BM::SteadyWaitNs) + B::Value(BM::IdleWaitNs);
	t.rec_wait_ns  = B::Value(BM::RecorderWaitNs);
	t.rejected     = B::Value(BM::Rejected);
	t.drains       = B::Value(BM::Drains);
	t.prep_busy_ns = B::PrepareBusyNs();
	t.prep_cpu_ns  = ThreadCpuNs(B::ThreadId(Role::Prep0)) + ThreadCpuNs(B::ThreadId(Role::Prep1)) +
	                ThreadCpuNs(B::ThreadId(Role::Prep2));
	t.rec_cpu_ns = ThreadCpuNs(B::ThreadId(Role::Rec));
#if defined(__linux__)
	rusage usage {};
	if (getrusage(RUSAGE_SELF, &usage) == 0) {
		t.proc_minflt = static_cast<uint64_t>(usage.ru_minflt);
		t.proc_majflt = static_cast<uint64_t>(usage.ru_majflt);
	}
#endif
	return t;
}

struct State {
	bool     have = false;
	Totals   last;
	uint64_t last_ns = 0;
	uint64_t count   = 0;
};
State g_presented; // presentation thread only
State g_guest;     // command thread only

void Print(const char* head, const Totals& now, const Totals& a) {
	const auto d = [](uint64_t x, uint64_t y) { return static_cast<unsigned long long>(x - y); };
	char       line[2304];
	int        n = std::snprintf(
        line, sizeof(line),
        "%s cmd_cpu_ns=%llu cmd_runq_ns=%llu cmd_minflt=%llu cmd_majflt=%llu cmd_vcs=%llu "
               "cmd_ics=%llu prep_wait_ns=%llu rec_wait_ns=%llu rejected=%llu drains=%llu prep_busy_ns=%llu "
               "prep_cpu_ns=%llu rec_cpu_ns=%llu proc_minflt=%llu proc_majflt=%llu",
        head, d(now.cmd.cpu_ns, a.cmd.cpu_ns), d(now.cmd.runq_ns, a.cmd.runq_ns),
        d(now.cmd.minflt, a.cmd.minflt), d(now.cmd.majflt, a.cmd.majflt), d(now.cmd.vcs, a.cmd.vcs),
        d(now.cmd.ics, a.cmd.ics), d(now.prep_wait_ns, a.prep_wait_ns), d(now.rec_wait_ns, a.rec_wait_ns),
        d(now.rejected, a.rejected), d(now.drains, a.drains), d(now.prep_busy_ns, a.prep_busy_ns),
        d(now.prep_cpu_ns, a.prep_cpu_ns), d(now.rec_cpu_ns, a.rec_cpu_ns),
        d(now.proc_minflt, a.proc_minflt), d(now.proc_majflt, a.proc_majflt));
	for (size_t i = 0; i < DebugFields.size() && n < static_cast<int>(sizeof(line)) - 48; i++) {
		n += std::snprintf(line + n, sizeof(line) - n, " %s=%llu", DebugFields[i].second,
		                   d(now.debug[i], a.debug[i]));
	}
	for (size_t i = 0; i < OwnNames.size() && n < static_cast<int>(sizeof(line)) - 48; i++) {
		n += std::snprintf(line + n, sizeof(line) - n, " %s=%llu", OwnNames[i], d(now.own[i], a.own[i]));
	}
	std::printf("%s\n", line);
	std::fflush(stdout);
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FRAME_DIAG");
		return value != nullptr && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

void OnFlip(uint64_t frame, double interval_ms) noexcept {
	if (!Enabled()) {
		return;
	}
	const auto now = Read(false);
	if (g_presented.have) {
		char head[160];
		std::snprintf(head, sizeof(head), "FRAME f=%" PRIu64 " ms=%.2f vbl=%.0f", frame, interval_ms,
		              std::max(1.0, std::round(interval_ms / (1000.0 / 60.0))));
		Print(head, now, g_presented.last);
	}
	g_presented.have = true;
	g_presented.last = now;
}

void OnGuestFlip(uint64_t frame) noexcept {
	if (!Enabled()) {
		return;
	}
	const auto now    = Read(true);
	const auto now_ns = NowNs();
	if (g_guest.have) {
		char head[160];
		std::snprintf(head, sizeof(head), "GFRAME g=%" PRIu64 " f=%" PRIu64 " t=%" PRIu64 " ms=%.3f",
		              g_guest.count, frame, now_ns, static_cast<double>(now_ns - g_guest.last_ns) / 1e6);
		Print(head, now, g_guest.last);
	}
	g_guest.have    = true;
	g_guest.last    = now;
	g_guest.last_ns = now_ns;
	g_guest.count++;
}

} // namespace Common::FrameDiag
