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
#include <memory>
#include <mutex>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
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
                               "cmd_gpu_wait_ns", "cmd_idles", "cmd_idle_ns", "cmd_blocks",
                               "cmd_blocked_ns", "wd", "wd_ns", "wd_dw", "label_stale",
                               "label_written", "wd_memo"};
static_assert(OwnNames.size() == static_cast<size_t>(Counter::Count));

struct Totals {
	std::array<uint64_t, DebugFields.size()>                 debug {};
	std::array<uint64_t, static_cast<size_t>(Counter::Count)> own {};
	ThreadStat                                               cmd;
	uint64_t prep_wait_ns = 0, rec_wait_ns = 0, rejected = 0, drains = 0;
	uint64_t prep_busy_ns = 0, prep_cpu_ns = 0, rec_cpu_ns = 0;
	uint64_t proc_minflt = 0, proc_majflt = 0, proc_cpu_ns = 0;
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
		// Every thread's user and system time (guest threads included), µs resolution.
		t.proc_cpu_ns = (static_cast<uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000u +
		                 static_cast<uint64_t>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec)) *
		                1000u;
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
               "prep_cpu_ns=%llu rec_cpu_ns=%llu proc_minflt=%llu proc_majflt=%llu proc_cpu_ns=%llu",
        head, d(now.cmd.cpu_ns, a.cmd.cpu_ns), d(now.cmd.runq_ns, a.cmd.runq_ns),
        d(now.cmd.minflt, a.cmd.minflt), d(now.cmd.majflt, a.cmd.majflt), d(now.cmd.vcs, a.cmd.vcs),
        d(now.cmd.ics, a.cmd.ics), d(now.prep_wait_ns, a.prep_wait_ns), d(now.rec_wait_ns, a.rec_wait_ns),
        d(now.rejected, a.rejected), d(now.drains, a.drains), d(now.prep_busy_ns, a.prep_busy_ns),
        d(now.prep_cpu_ns, a.prep_cpu_ns), d(now.rec_cpu_ns, a.rec_cpu_ns),
        d(now.proc_minflt, a.proc_minflt), d(now.proc_majflt, a.proc_majflt),
        d(now.proc_cpu_ns, a.proc_cpu_ns));
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

bool ReadEnabled() noexcept {
	const char* value = std::getenv("KYTY_FRAME_DIAG");
	return value != nullptr && std::strcmp(value, "0") != 0;
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

namespace {

constexpr uint64_t kRingMinNs = 20'000;
constexpr uint64_t kIdleMinNs = 1'000'000;
constexpr uint32_t kRing      = 512;
constexpr int      kKinds     = static_cast<int>(GuestWait::Count);
constexpr const char* kKindNames[kKinds] = {"eq", "sleep", "cond", "mutex", "sema", "ef"};

// One guest thread's waits. Written only by its thread; read by the command thread.
struct GuestThread {
	struct Entry {
		std::atomic<uint64_t> start {0};
		std::atomic<uint64_t> end {0};
		std::atomic<uint64_t> ident {0}; // kind in the top 8 bits
	};
	int                   tid = 0;
	char                  name[20] {};
	std::atomic<uint64_t> head {0};
	Entry                 ring[kRing];
	std::atomic<uint64_t> short_ns[kKinds] {};
	std::atomic<uint64_t> short_count[kKinds] {};
};

std::mutex                                 g_threads_mutex;
std::vector<std::unique_ptr<GuestThread>>  g_threads; // never freed: the command thread reads them
std::atomic<GuestThread*>                  g_submitter {nullptr};
std::atomic<uint64_t>                      g_last_submit_ns {0};
// The submitter's short-wait totals when the command thread began waiting (command thread only).
GuestThread*                               g_idle_sub = nullptr;
uint64_t                                   g_idle_short_ns[kKinds] {};
uint64_t                                   g_idle_short_count[kKinds] {};
thread_local GuestThread*                  t_guest       = nullptr;
thread_local int                           t_guest_depth = 0;

GuestThread* Self() {
	if (t_guest == nullptr) {
		auto thread = std::make_unique<GuestThread>();
#if defined(__linux__)
		thread->tid = static_cast<int>(syscall(SYS_gettid));
		if (FILE* f = std::fopen("/proc/thread-self/comm", "r"); f != nullptr) {
			if (std::fgets(thread->name, sizeof(thread->name), f) != nullptr) {
				thread->name[std::strcspn(thread->name, "\n ")] = '\0';
			}
			std::fclose(f);
		}
#endif
		t_guest = thread.get();
		std::lock_guard lock(g_threads_mutex);
		g_threads.push_back(std::move(thread));
	}
	return t_guest;
}

} // namespace

void GuestWaitScope::Begin() noexcept {
	// Only the outermost wait of a thread counts (a timed wait may be built on another).
	if (t_guest_depth++ == 0) {
		m_start = NowNs();
	}
}

void GuestWaitScope::End() noexcept {
	t_guest_depth--;
	if (m_start == 0) {
		return;
	}
	const auto end  = NowNs();
	auto*      self = Self();
	const auto kind = static_cast<int>(m_kind);
	if (end - m_start < kRingMinNs) {
		self->short_ns[kind].fetch_add(end - m_start, std::memory_order_relaxed);
		self->short_count[kind].fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto head  = self->head.load(std::memory_order_relaxed);
	auto&      entry = self->ring[head % kRing];
	entry.start.store(m_start, std::memory_order_relaxed);
	entry.end.store(end, std::memory_order_relaxed);
	entry.ident.store((static_cast<uint64_t>(kind) << 56) | (m_ident & ((uint64_t {1} << 56) - 1)),
	                  std::memory_order_relaxed);
	self->head.store(head + 1, std::memory_order_release);
}

void OnGuestSubmit(bool graphics) noexcept {
	if (!Enabled() || !graphics) {
		return;
	}
	// Release: the thread record (tid, name) is complete before a reader can see it.
	g_submitter.store(Self(), std::memory_order_release);
	g_last_submit_ns.store(NowNs(), std::memory_order_relaxed);
}

namespace {

// The last signal of each condition variable, by address (a direct-mapped table; a collision
// only loses an attribution).
struct CondSignal {
	std::atomic<uint64_t>     cond {0};
	std::atomic<uint64_t>     ns {0};
	std::atomic<GuestThread*> thread {nullptr};
	std::atomic<uint64_t>     last_ident {0}; // the signaller's last wait before the signal
	std::atomic<uint64_t>     last_start {0};
	std::atomic<uint64_t>     last_end {0};
};
constexpr uint32_t kConds = 4096;
CondSignal         g_conds[kConds];

CondSignal& CondSlot(uint64_t cond) {
	return g_conds[(cond >> 4) * 0x9E3779B97F4A7C15ull >> 52 & (kConds - 1)];
}

} // namespace

void OnGuestCondSignal(const void* cond) noexcept {
	if (!Enabled()) {
		return;
	}
	auto*      self = Self();
	const auto key  = reinterpret_cast<uint64_t>(cond);
	auto&      slot = CondSlot(key);
	const auto head = self->head.load(std::memory_order_relaxed);
	if (head != 0) {
		const auto& entry = self->ring[(head - 1) % kRing];
		slot.last_ident.store(entry.ident.load(std::memory_order_relaxed), std::memory_order_relaxed);
		slot.last_start.store(entry.start.load(std::memory_order_relaxed), std::memory_order_relaxed);
		slot.last_end.store(entry.end.load(std::memory_order_relaxed), std::memory_order_relaxed);
	} else {
		slot.last_ident.store(~uint64_t {0}, std::memory_order_relaxed);
	}
	slot.thread.store(self, std::memory_order_release);
	slot.ns.store(NowNs(), std::memory_order_relaxed);
	slot.cond.store(key, std::memory_order_release);
}

namespace {
std::atomic<bool> g_cmd_idle {false};
} // namespace

bool CmdIdle() noexcept {
	return g_cmd_idle.load(std::memory_order_relaxed);
}

void OnCmdIdleBegin() noexcept {
	if (!Enabled()) {
		return;
	}
	g_cmd_idle.store(true, std::memory_order_relaxed);
	g_idle_sub = g_submitter.load(std::memory_order_acquire);
	if (g_idle_sub != nullptr) {
		for (int k = 0; k < kKinds; k++) {
			g_idle_short_ns[k]    = g_idle_sub->short_ns[k].load(std::memory_order_relaxed);
			g_idle_short_count[k] = g_idle_sub->short_count[k].load(std::memory_order_relaxed);
		}
	}
}

void OnCmdIdle(uint64_t start_ns, uint64_t end_ns) noexcept {
	g_cmd_idle.store(false, std::memory_order_relaxed);
	if (!Enabled()) {
		return;
	}
	Add(Counter::CmdIdles);
	Add(Counter::CmdIdleNs, end_ns - start_ns);
	if (end_ns - start_ns < kIdleMinNs) {
		return;
	}
	auto* sub = g_submitter.load(std::memory_order_acquire);
	if (sub == nullptr) {
		return;
	}
	const auto submit_ns = g_last_submit_ns.load(std::memory_order_relaxed);
	uint64_t   in_gap[kKinds] {};
	uint64_t   count[kKinds] {};
	uint64_t   last_end = 0, last_start = 0, last_ident = 0;
	const auto head     = sub->head.load(std::memory_order_acquire);
	const auto first    = head > kRing ? head - kRing : 0;
	for (auto i = head; i > first; i--) {
		const auto& entry = sub->ring[(i - 1) % kRing];
		const auto  start = entry.start.load(std::memory_order_relaxed);
		const auto  end   = entry.end.load(std::memory_order_relaxed);
		const auto  ident = entry.ident.load(std::memory_order_relaxed);
		if (end <= start_ns) {
			break; // the ring is in end order
		}
		if (end <= end_ns && end > last_end) {
			last_end   = end;
			last_start = start;
			last_ident = ident;
		}
		const auto kind = static_cast<int>(ident >> 56);
		const auto lo   = std::max(start, start_ns);
		const auto hi   = std::min(end, end_ns);
		if (kind < kKinds && hi > lo) {
			in_gap[kind] += hi - lo;
			count[kind]++;
		}
	}
	char line[512];
	int  n = std::snprintf(line, sizeof(line),
	                       "GIDLE g=%" PRIu64 " t=%" PRIu64 " ms=%.3f wake_ms=%.3f sub=%d:%s", g_guest.count,
	                       end_ns, static_cast<double>(end_ns - start_ns) / 1e6,
	                       submit_ns > start_ns && submit_ns <= end_ns
	                           ? static_cast<double>(end_ns - submit_ns) / 1e6
	                           : -1.0,
	                       sub->tid, sub->name);
	if (sub == g_idle_sub) {
		for (int k = 0; k < kKinds; k++) {
			in_gap[k] += sub->short_ns[k].load(std::memory_order_relaxed) - g_idle_short_ns[k];
			count[k] += sub->short_count[k].load(std::memory_order_relaxed) - g_idle_short_count[k];
		}
	}
	for (int k = 0; k < kKinds && n > 0 && n < static_cast<int>(sizeof(line)); k++) {
		if (count[k] != 0) {
			n += std::snprintf(line + n, sizeof(line) - n, " %s_ms=%.3f %s_n=%" PRIu64, kKindNames[k],
			                   static_cast<double>(in_gap[k]) / 1e6, kKindNames[k], count[k]);
		}
	}
	if (last_end != 0 && n > 0 && n < static_cast<int>(sizeof(line))) {
		const auto kind = static_cast<int>(last_ident >> 56);
		n += std::snprintf(line + n, sizeof(line) - n,
		                   " last=%s last_id=0x%" PRIx64 " last_ms=%.3f last_before_ms=%.3f",
		                   kind < kKinds ? kKindNames[kind] : "?", last_ident & ((uint64_t {1} << 56) - 1),
		                   static_cast<double>(last_end - last_start) / 1e6,
		                   static_cast<double>(end_ns - last_end) / 1e6);
	}
	const auto mask = (uint64_t {1} << 56) - 1;
	if (last_end != 0 && (last_ident >> 56) == static_cast<uint64_t>(GuestWait::Cond) && n > 0 &&
	    n < static_cast<int>(sizeof(line))) {
		auto& slot = CondSlot(last_ident & mask);
		if (slot.cond.load(std::memory_order_acquire) == (last_ident & mask)) {
			// Acquire: whichever signaller's record this is, it is complete (slot fields may still
			// mix two signals; the line is diagnostic).
			const auto* sig      = slot.thread.load(std::memory_order_acquire);
			if (sig == nullptr) {
				return;
			}
			const auto  sig_ns   = slot.ns.load(std::memory_order_relaxed);
			const auto  sig_last = slot.last_ident.load(std::memory_order_relaxed);
			const auto  s_start  = slot.last_start.load(std::memory_order_relaxed);
			const auto  s_end    = slot.last_end.load(std::memory_order_relaxed);
			const auto  kind     = static_cast<int>(sig_last >> 56);
			n += std::snprintf(line + n, sizeof(line) - n, " sig=%d:%s sig_before_ms=%.3f", sig->tid,
			                   sig->name, (static_cast<double>(end_ns) - static_cast<double>(sig_ns)) / 1e6);
			if (sig_last != ~uint64_t {0} && n > 0 && n < static_cast<int>(sizeof(line))) {
				n += std::snprintf(line + n, sizeof(line) - n,
				                   " sig_last=%s sig_last_id=0x%" PRIx64 " sig_last_ms=%.3f sig_last_before_ms=%.3f",
				                   kind < kKinds ? kKindNames[kind] : "?", sig_last & mask,
				                   static_cast<double>(s_end - s_start) / 1e6,
				                   (static_cast<double>(sig_ns) - static_cast<double>(s_end)) / 1e6);
			}
		}
	}
	if (n > 0 && n < static_cast<int>(sizeof(line))) {
		std::printf("%s\n", line);
	}
}

} // namespace Common::FrameDiag
