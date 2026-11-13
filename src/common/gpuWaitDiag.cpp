#include "common/gpuWaitDiag.h"

#include "common/stallDiag.h"

#include "common/frameDiag.h"

#include <algorithm>
#if defined(__linux__)
#include <sys/uio.h>
#endif
#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <thread>

#if defined(__linux__)
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
extern "C" char __executable_start; // NOLINT
extern "C" char etext;              // NOLINT
#endif

namespace Common::GpuWaitDiag {

namespace {

constexpr size_t RingSize = 8192;
constexpr int    StackDepth = 10;
constexpr int    StackSkip  = 2; // NoteWait, MasterSemaphore::Wait

struct TickInfo {
	uint64_t tick      = 0;
	uint64_t submit_ns = 0;
	uint64_t done_ns   = 0;
	bool     idle      = false; // nothing unfinished when it was submitted
};

struct SiteTotal {
	const char* file     = nullptr;
	const char* function = nullptr;
	uint32_t    line     = 0;
	uint32_t    id       = 0;
	uint64_t    n        = 0;
	uint64_t    ns       = 0;
	uint64_t    max_ns   = 0;
	uint64_t    backlog  = 0;
	uint64_t    distance = 0;
	uint64_t    age_ns   = 0;
	uint64_t    idle     = 0; // waits whose target submit reached an idle GPU
	uint64_t    current  = 0; // waits for the submit made by the wait itself (distance 0)
};

struct StackTotal {
	std::array<uintptr_t, StackDepth> frames {};
	int                               depth = 0;
	uint64_t                          n     = 0;
	uint64_t                          ns    = 0;
};

struct State {
	std::mutex                                 mutex;
	std::array<TickInfo, RingSize>             ring {};
	uint64_t                                   last_submitted = 0;
	uint64_t                                   done           = 0;
	uint64_t                                   period_start   = 0;
	uint64_t                                   origin         = 0;
	// Per period.
	uint64_t                                   submits     = 0;
	uint64_t                                   submit_ns   = 0;
	uint64_t                                   idle_ns     = 0;
	uint64_t                                   idle_gaps   = 0;
	uint64_t                                   idle_submit = 0; // submits that reached an idle GPU
	uint64_t                                   waits       = 0;
	uint64_t                                   wait_ns     = 0;
	uint64_t                                   flips       = 0;
	uint64_t                                   early       = 0;
	std::unordered_map<uint64_t, SiteTotal>    sites;
	std::unordered_map<uint64_t, StackTotal>   stacks;
	std::array<std::pair<uint64_t, uint64_t>, static_cast<size_t>(Driver::Count)> driver {};
	std::unordered_map<uint64_t, SiteTotal>    direct_sites; // ns = drain wait
	std::array<UploadSample, static_cast<size_t>(UploadPath::Count)> uploads {};
	std::array<uint64_t, static_cast<size_t>(UploadPath::Count)>     upload_n {};
	uint64_t                                   upload_destroy_n  = 0;
	uint64_t                                   upload_destroy_ns = 0;
	uint64_t                                   recycler[4]       = {};
	std::unordered_map<uint64_t, SiteTotal>    sched_sites;  // backlog = ahead, idle = gpu_done
	std::unordered_map<uint64_t, StackTotal>   sched_stacks;
	uint64_t                                   sched_waits   = 0;
	uint64_t                                   sched_wait_ns = 0;
	uint32_t                                   next_site_id = 1;
	std::unordered_map<uint64_t, uint32_t>     site_ids;
	std::string                                trace;
	FILE*                                      trace_file = nullptr;
};

State& Get() {
	static State state;
	return state;
}

uint64_t SiteKey(const std::source_location& site) {
	return (reinterpret_cast<uintptr_t>(site.file_name()) * 1000003ull) ^ site.line();
}

uint64_t Rel(State& s, uint64_t ns) {
	return ns >= s.origin ? ns - s.origin : 0;
}

void Append(State& s, const char* format, ...) __attribute__((format(printf, 2, 3)));
void Append(State& s, const char* format, ...) {
	char    text[256];
	va_list args;
	va_start(args, format);
	const int n = std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (n > 0) {
		s.trace.append(text, std::min<size_t>(static_cast<size_t>(n), sizeof(text) - 1));
	}
}

#if defined(__linux__)
struct OffCpu {
	std::mutex                                           mutex;
	std::unordered_map<std::string, uint64_t>           stacks;
	uint64_t                                             samples  = 0;
	uint64_t                                             sleeping = 0;
};

OffCpu& GetOffCpu() {
	static OffCpu state;
	return state;
}

int ReadFile(int fd, char* buffer, size_t size) {
	const auto n = pread(fd, buffer, size - 1, 0);
	if (n <= 0) {
		return 0;
	}
	buffer[n] = '\0';
	return static_cast<int>(n);
}

void SampleThread(pid_t tid, uintptr_t stack_base, uintptr_t stack_top, bool idle_only) {
	pthread_setname_np(pthread_self(), "KytyOffCpuDiag");
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", static_cast<int>(tid));
	const int stat_fd = open(path, O_RDONLY | O_CLOEXEC);
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", static_cast<int>(tid));
	const int syscall_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (stat_fd < 0 || syscall_fd < 0) {
		std::printf("OFFCPU: cannot open %s\n", path);
		return;
	}
	const auto text_begin = reinterpret_cast<uintptr_t>(&__executable_start);
	const auto text_end   = reinterpret_cast<uintptr_t>(&etext);
	char       buffer[512];
	for (;;) {
		std::this_thread::sleep_for(std::chrono::microseconds(200));
		if (ReadFile(stat_fd, buffer, sizeof(buffer)) == 0) {
			return;
		}
		// With KYTY_FRAME_DIAG=1, a game thread is sampled only while the command thread has no work.
		if (idle_only && !Common::FrameDiag::CmdIdle()) {
			continue;
		}
		const char* close = std::strrchr(buffer, ')');
		const char  state = close != nullptr && close[1] == ' ' ? close[2] : '?';
		auto&       off   = GetOffCpu();
		if (state != 'S' && state != 'D') {
			std::lock_guard lock(off.mutex);
			off.samples++;
			continue;
		}
		if (ReadFile(syscall_fd, buffer, sizeof(buffer)) == 0) {
			continue;
		}
		// "nr a1 a2 a3 a4 a5 a6 sp pc"
		unsigned long long values[9] {};
		int                count = 0;
		char*              cursor = buffer;
		for (; count < 9; count++) {
			char* end         = nullptr;
			values[count]     = std::strtoull(cursor, &end, 0);
			if (end == cursor) {
				break;
			}
			cursor = end;
		}
		std::string key = count >= 1 ? "nr" + std::to_string(values[0]) : "nr?";
		if (count == 9) {
			const auto* sp    = reinterpret_cast<const uintptr_t*>(values[7]);
			int         found = 0;
			// Only words inside the thread's own stack (a signal handler's stack is elsewhere),
			// copied with process_vm_readv: the thread may have exited and its stack gone.
			const auto in_stack = sp != nullptr && values[7] >= stack_base && values[7] < stack_top
			                          ? std::min<uintptr_t>(1024, (stack_top - values[7]) / sizeof(uintptr_t))
			                          : 0;
			std::array<uintptr_t, 1024> copy {};
			iovec local {copy.data(), in_stack * sizeof(uintptr_t)};
			iovec remote {const_cast<uintptr_t*>(sp), in_stack * sizeof(uintptr_t)};
			const auto read  = in_stack != 0 ? process_vm_readv(getpid(), &local, 1, &remote, 1, 0) : 0;
			const auto words = read > 0 ? static_cast<uintptr_t>(read) / sizeof(uintptr_t) : 0;
			sp               = copy.data();
			for (uintptr_t i = 0; i < words && found < 6; i++) {
				const auto value = sp[i];
				if (value > text_begin && value < text_end) {
					char text[24];
					std::snprintf(text, sizeof(text), " %lx", static_cast<unsigned long>(value - text_begin));
					key += text;
					found++;
				}
			}
		}
		std::lock_guard lock(off.mutex);
		off.samples++;
		off.sleeping++;
		off.stacks[key]++;
	}
}
#endif

} // namespace

namespace {

const char* OffCpuThread() noexcept {
	static const char* name = std::getenv("KYTY_OFFCPU_THREAD");
	return name != nullptr && name[0] != '\0' ? name : nullptr;
}

// A game thread runs on its guest stack (stack_base/stack_size given); the command thread on its
// native one (stack_size 0: read from pthread).
void StartSampler(bool idle_only, uintptr_t guest_base = 0, size_t guest_size = 0) {
#if defined(__linux__)
	const char* value = std::getenv("KYTY_OFFCPU_SAMPLE");
	if (!Enabled() || value == nullptr || value[0] == '0') {
		return;
	}
	const auto     tid        = static_cast<pid_t>(syscall(SYS_gettid));
	uintptr_t      stack_base = 0;
	uintptr_t      stack_top  = 0;
	pthread_attr_t attr;
	if (guest_size != 0) {
		stack_base = guest_base;
		stack_top  = guest_base + guest_size;
	} else if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void*  base = nullptr;
		size_t size = 0;
		if (pthread_attr_getstack(&attr, &base, &size) == 0) {
			stack_base = reinterpret_cast<uintptr_t>(base);
			stack_top  = stack_base + size;
		}
		pthread_attr_destroy(&attr);
	}
	std::thread([=] { SampleThread(tid, stack_base, stack_top, idle_only); }).detach();
#else
	(void)idle_only;
	(void)guest_base;
	(void)guest_size;
#endif
}

} // namespace

void SetCommandThread() noexcept {
	if (OffCpuThread() == nullptr) {
		StartSampler(false);
	}
}

void SetGuestThread(const std::string& name, uintptr_t stack_base, size_t stack_size) noexcept {
	static std::atomic<bool> started {false};
	const char* prefix = OffCpuThread();
	if (prefix != nullptr && name.rfind(prefix, 0) == 0 && !started.exchange(true)) {
		StartSampler(Common::FrameDiag::Enabled(), stack_base, stack_size);
	}
}

int Mode() noexcept {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_GPU_WAIT_DIAG");
		if (value == nullptr || value[0] == '0' || value[0] == '\0') {
			return 0;
		}
		return std::strcmp(value, "trace") == 0 ? 2 : 1;
	}();
	return mode;
}

void NoteSubmit(uint64_t tick, uint64_t start_ns, uint64_t end_ns, uint64_t draws) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	if (s.origin == 0) {
		s.origin = start_ns;
	}
	auto& info     = s.ring[tick % RingSize];
	info           = {};
	info.tick      = tick;
	info.submit_ns = end_ns;
	// Idle: every earlier submit had finished (as far as the poller has seen) before this one.
	if (s.done + 1 >= tick && tick > 1) {
		const auto& previous = s.ring[(tick - 1) % RingSize];
		if (previous.tick == tick - 1 && previous.done_ns != 0) {
			const auto since = std::max(previous.done_ns, s.period_start);
			if (end_ns > since) {
				s.idle_ns += end_ns - since;
				s.idle_gaps++;
			}
		}
		info.idle = true;
		s.idle_submit++;
	}
	s.last_submitted = std::max(s.last_submitted, tick);
	s.submits++;
	s.submit_ns += end_ns - start_ns;
	if (Mode() == 2) {
		Append(s, "S %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %d\n", tick, Rel(s, end_ns),
		       end_ns - start_ns, draws, info.idle ? 1 : 0);
	}
}

void NoteComplete(uint64_t tick, uint64_t now_ns) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	for (uint64_t t = s.done + 1; t <= tick; t++) {
		auto& info = s.ring[t % RingSize];
		if (info.tick == t && info.done_ns == 0) {
			info.done_ns = now_ns;
		}
	}
	if (tick > s.done) {
		if (Mode() == 2 && s.origin != 0) {
			Append(s, "C %" PRIu64 " %" PRIu64 "\n", tick, Rel(s, now_ns));
		}
		s.done = tick;
	}
}

void NoteWait(uint64_t tick, uint64_t current, uint64_t known, uint64_t start_ns, uint64_t end_ns,
              const std::source_location& site) noexcept {
	std::array<void*, StackDepth + StackSkip> raw {};
	int                                       depth = 0;
#if defined(__linux__)
	depth = backtrace(raw.data(), static_cast<int>(raw.size()));
#endif
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	const auto      ns       = end_ns - start_ns;
	const auto      key      = SiteKey(site);
	auto&           total    = s.sites[key];
	const auto&     info     = s.ring[tick % RingSize];
	const bool      known_ok = info.tick == tick;
	if (total.n == 0) {
		total.file     = site.file_name();
		total.function = site.function_name();
		total.line     = site.line();
		auto& id       = s.site_ids[key];
		if (id == 0) {
			id = s.next_site_id++;
			if (Mode() == 2) {
				Append(s, "L %u %s:%u %s\n", id, site.file_name(), site.line(), site.function_name());
			}
		}
		total.id = id;
	}
	total.n++;
	total.ns += ns;
	total.max_ns = std::max(total.max_ns, ns);
	total.backlog += current - 1 - std::min(known, current - 1);
	total.distance += current - 1 - std::min(tick, current - 1);
	if (current - 1 == tick) {
		total.current++;
	}
	if (known_ok) {
		total.age_ns += start_ns > info.submit_ns ? start_ns - info.submit_ns : 0;
		total.idle += info.idle ? 1 : 0;
	}
	s.waits++;
	s.wait_ns += ns;

	uint64_t hash = key;
	StackTotal stack;
	for (int i = StackSkip; i < depth && stack.depth < StackDepth; i++) {
		const auto address         = reinterpret_cast<uintptr_t>(raw[static_cast<size_t>(i)]);
		stack.frames[stack.depth++] = address;
		hash = hash * 1099511628211ull ^ address;
	}
	auto& stack_total = s.stacks[hash];
	if (stack_total.n == 0) {
		stack_total.frames = stack.frames;
		stack_total.depth  = stack.depth;
	}
	stack_total.n++;
	stack_total.ns += ns;

	if (Mode() == 2) {
		Append(s, "W %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %" PRIu64 " %" PRIu64 "\n", tick,
		       Rel(s, start_ns), Rel(s, end_ns), total.id, known, current);
	}
}

bool DriverStats() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DRIVER_STATS");
		return Enabled() && value != nullptr && value[0] != '0';
	}();
	return enabled;
}

DriverScope::DriverScope(Driver kind) noexcept: m_kind(kind) {
	if (DriverStats() && StallDiag::OnCommandThread()) {
		m_start = NowNs();
	}
}

void NoteDriver(Driver kind, uint64_t ns) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	auto&           d = s.driver[static_cast<size_t>(kind)];
	d.first++;
	d.second += ns;
}

bool UploadTouch() noexcept {
	static const bool touch = [] {
		const char* value = std::getenv("KYTY_DRIVER_STATS");
		return DriverStats() && value != nullptr && std::strcmp(value, "touch") == 0;
	}();
	return touch;
}

uint64_t ThreadMinorFaults() noexcept {
#if defined(__linux__)
	rusage usage {};
	getrusage(RUSAGE_THREAD, &usage);
	return static_cast<uint64_t>(usage.ru_minflt);
#else
	return 0;
#endif
}

void NoteUpload(UploadPath path, const UploadSample& sample) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	auto&           u = s.uploads[static_cast<size_t>(path)];
	s.upload_n[static_cast<size_t>(path)]++;
	u.bytes += sample.bytes;
	u.alloc_ns += sample.alloc_ns;
	u.touch_ns += sample.touch_ns;
	u.copy_ns += sample.copy_ns;
	u.flush_ns += sample.flush_ns;
	u.touch_flt += sample.touch_flt;
	u.copy_flt += sample.copy_flt;
}

void NoteUploadDestroy(uint64_t ns) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	s.upload_destroy_n++;
	s.upload_destroy_ns += ns;
}

void NoteRecyclerState(uint64_t entries, uint64_t bytes, uint64_t evicted, uint64_t destroy_queue) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	s.recycler[0] = entries;
	s.recycler[1] = bytes;
	s.recycler[2] = evicted;
	s.recycler[3] = destroy_queue;
}

void NoteDirectRecord(const std::source_location& site, uint64_t drain_ns) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	auto&           t = s.direct_sites[SiteKey(site)];
	if (t.n == 0) {
		t.file     = site.file_name();
		t.function = site.function_name();
		t.line     = site.line();
	}
	t.n++;
	t.ns += drain_ns;
	t.idle += drain_ns != 0 ? 1 : 0; // drains that waited
}

void NoteSchedulerWait(uint64_t start_ns, uint64_t end_ns, bool gpu_done, uint32_t ahead,
                       const std::source_location& site) noexcept {
	std::array<void*, StackDepth + StackSkip> raw {};
	int                                       depth = 0;
#if defined(__linux__)
	depth = backtrace(raw.data(), static_cast<int>(raw.size()));
#endif
	auto&           s     = Get();
	std::lock_guard lock(s.mutex);
	const auto      ns    = end_ns - start_ns;
	const auto      key   = SiteKey(site);
	auto&           total = s.sched_sites[key];
	if (total.n == 0) {
		total.file     = site.file_name();
		total.function = site.function_name();
		total.line     = site.line();
	}
	total.n++;
	total.ns += ns;
	total.max_ns = std::max(total.max_ns, ns);
	total.backlog += ahead;
	total.idle += gpu_done ? 1 : 0;
	s.sched_waits++;
	s.sched_wait_ns += ns;
	uint64_t   hash = key;
	StackTotal stack;
	for (int i = StackSkip; i < depth && stack.depth < StackDepth; i++) {
		const auto address          = reinterpret_cast<uintptr_t>(raw[static_cast<size_t>(i)]);
		stack.frames[stack.depth++] = address;
		hash                        = hash * 1099511628211ull ^ address;
	}
	auto& stack_total = s.sched_stacks[hash];
	if (stack_total.n == 0) {
		stack_total.frames = stack.frames;
		stack_total.depth  = stack.depth;
	}
	stack_total.n++;
	stack_total.ns += ns;
	if (Mode() == 2 && s.origin != 0) {
		Append(s, "P %" PRIu64 " %" PRIu64 " %d %u\n", Rel(s, start_ns), Rel(s, end_ns), gpu_done ? 1 : 0,
		       ahead);
	}
}

void NoteFlip(uint64_t tick) noexcept {
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	s.flips++;
	if (Mode() == 2 && s.origin != 0) {
		Append(s, "F %" PRIu64 " %" PRIu64 "\n", tick, Rel(s, NowNs()));
	}
}

void NoteEarlySubmit() noexcept {
	if (!Enabled()) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	s.early++;
	if (Mode() == 2 && s.origin != 0) {
		Append(s, "E %" PRIu64 "\n", Rel(s, NowNs()));
	}
}

void NoteFlipDone(uint64_t tick) noexcept {
	if (Mode() != 2) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	if (s.origin != 0) {
		Append(s, "D %" PRIu64 " %" PRIu64 "\n", tick, Rel(s, NowNs()));
	}
}

void NoteGuestSubmission(uint32_t queue) noexcept {
	if (Mode() != 2) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	if (s.origin != 0) {
		Append(s, "G %u %" PRIu64 "\n", queue, Rel(s, NowNs()));
	}
}

void Report() noexcept {
	if (!Enabled()) {
		return;
	}
	auto&      s   = Get();
	const auto now = NowNs();
	std::unique_lock lock(s.mutex);
	if (s.period_start == 0) {
		s.period_start = now;
		return;
	}
	if (now - s.period_start < 1'000'000'000ull) {
		return;
	}
	const double secs = static_cast<double>(now - s.period_start) / 1e9;
	std::printf("GPUWAIT waits_s=%.0f wait_ms_s=%.2f gpu_idle_ms_s=%.2f idle_gaps_s=%.0f "
	            "submits_s=%.0f submits_to_idle_s=%.0f early_submits_s=%.0f submit_call_ms_s=%.2f flips_s=%.1f\n",
	            static_cast<double>(s.waits) / secs, static_cast<double>(s.wait_ns) / 1e6 / secs,
	            static_cast<double>(s.idle_ns) / 1e6 / secs, static_cast<double>(s.idle_gaps) / secs,
	            static_cast<double>(s.submits) / secs, static_cast<double>(s.idle_submit) / secs,
	            static_cast<double>(s.early) / secs, static_cast<double>(s.submit_ns) / 1e6 / secs, static_cast<double>(s.flips) / secs);
	std::vector<const SiteTotal*> sites;
	for (const auto& [key, total]: s.sites) {
		if (total.n != 0) {
			sites.push_back(&total);
		}
	}
	std::sort(sites.begin(), sites.end(), [](const auto* a, const auto* b) { return a->ns > b->ns; });
	for (const auto* t: sites) {
		const double n = static_cast<double>(t->n);
		const char*  file = std::strstr(t->file, "src/");
		std::printf("GPUWAITSITE id=%u %s:%u waits_s=%.1f ms_s=%.3f avg_us=%.1f max_us=%.1f "
		            "backlog=%.2f distance=%.2f own_submit=%.0f%% age_us=%.1f target_idle=%.0f%% fn=%s\n",
		            t->id, file != nullptr ? file : t->file, t->line, n / secs,
		            static_cast<double>(t->ns) / 1e6 / secs, static_cast<double>(t->ns) / 1e3 / n,
		            static_cast<double>(t->max_ns) / 1e3, static_cast<double>(t->backlog) / n,
		            static_cast<double>(t->distance) / n, 100.0 * static_cast<double>(t->current) / n,
		            static_cast<double>(t->age_ns) / 1e3 / n, 100.0 * static_cast<double>(t->idle) / n,
		            t->function);
	}
	if (DriverStats()) {
		static constexpr std::array<const char*, static_cast<size_t>(Driver::Count)> names = {
		    "submit", "queue_submit", "refresh", "begin_command", "image_create", "image_create_dedicated",
		    "image_delete", "buffer_create", "buffer_create_dedicated", "buffer_delete",
		    "image_recycled", "image_retire", "view_create", "format_query"};
		std::string line = "DRIVER";
		for (size_t i = 0; i < names.size(); i++) {
			char text[96];
			std::snprintf(text, sizeof(text), " %s=%.0f/%.3fms", names[i],
			              static_cast<double>(s.driver[i].first) / secs,
			              static_cast<double>(s.driver[i].second) / 1e6 / secs);
			line += text;
		}
		std::printf("%s\n", line.c_str());
		s.driver = {};
		static constexpr std::array<const char*, static_cast<size_t>(UploadPath::Count)> paths = {
		    "ring_lap0", "ring", "async", "temporary"};
		for (size_t i = 0; i < paths.size(); i++) {
			const auto& u = s.uploads[i];
			const auto  n = s.upload_n[i];
			if (n == 0) {
				continue;
			}
			std::printf("UPLOAD %s n_s=%.1f MB_s=%.2f alloc_ms_s=%.3f touch_ms_s=%.3f copy_ms_s=%.3f "
			            "flush_ms_s=%.3f touch_flt_s=%.0f copy_flt_s=%.0f GBps=%.2f\n",
			            paths[i], static_cast<double>(n) / secs, static_cast<double>(u.bytes) / 1e6 / secs,
			            static_cast<double>(u.alloc_ns) / 1e6 / secs,
			            static_cast<double>(u.touch_ns) / 1e6 / secs,
			            static_cast<double>(u.copy_ns) / 1e6 / secs,
			            static_cast<double>(u.flush_ns) / 1e6 / secs,
			            static_cast<double>(u.touch_flt) / secs, static_cast<double>(u.copy_flt) / secs,
			            u.copy_ns != 0 ? static_cast<double>(u.bytes) / static_cast<double>(u.copy_ns) : 0.0);
		}
		if (s.upload_destroy_n != 0) {
			std::printf("UPLOAD temporary_destroy n_s=%.1f ms_s=%.3f\n",
			            static_cast<double>(s.upload_destroy_n) / secs,
			            static_cast<double>(s.upload_destroy_ns) / 1e6 / secs);
		}
		s.uploads          = {};
		s.upload_n         = {};
		s.upload_destroy_n = s.upload_destroy_ns = 0;
		std::printf("RECYCLER entries=%" PRIu64 " MB=%.1f evicted=%" PRIu64 " destroy_queue=%" PRIu64 "\n",
		            s.recycler[0], static_cast<double>(s.recycler[1]) / 1e6, s.recycler[2], s.recycler[3]);
		std::vector<const SiteTotal*> direct;
		for (const auto& [key, t]: s.direct_sites) {
			direct.push_back(&t);
		}
		std::sort(direct.begin(), direct.end(), [](const auto* a, const auto* b) { return a->n > b->n; });
		for (size_t i = 0; i < direct.size() && i < 20; i++) {
			const auto* t    = direct[i];
			const char* file = std::strstr(t->file, "src/");
			std::printf("DRIVERSITE %s:%u direct_s=%.0f drained_s=%.0f drain_ms_s=%.3f fn=%s\n",
			            file != nullptr ? file : t->file, t->line, static_cast<double>(t->n) / secs,
			            static_cast<double>(t->idle) / secs, static_cast<double>(t->ns) / 1e6 / secs,
			            t->function);
		}
		s.direct_sites.clear();
	}
	std::printf("SCHEDWAIT waits_s=%.0f wait_ms_s=%.2f\n", static_cast<double>(s.sched_waits) / secs,
	            static_cast<double>(s.sched_wait_ns) / 1e6 / secs);
	for (const auto& [key, t]: s.sched_sites) {
		const double n    = static_cast<double>(t.n);
		const char*  file = std::strstr(t.file, "src/");
		std::printf("SCHEDWAITSITE %s:%u waits_s=%.1f ms_s=%.3f avg_us=%.1f max_us=%.1f ahead=%.2f "
		            "gpu_done=%.0f%% fn=%s\n",
		            file != nullptr ? file : t.file, t.line, n / secs, static_cast<double>(t.ns) / 1e6 / secs,
		            static_cast<double>(t.ns) / 1e3 / n, static_cast<double>(t.max_ns) / 1e3,
		            static_cast<double>(t.backlog) / n, 100.0 * static_cast<double>(t.idle) / n, t.function);
	}
	{
		std::vector<const StackTotal*> top;
		for (const auto& [key, total]: s.sched_stacks) {
			top.push_back(&total);
		}
		std::sort(top.begin(), top.end(), [](const auto* a, const auto* b) { return a->ns > b->ns; });
#if defined(__linux__)
		const auto base = reinterpret_cast<uintptr_t>(&__executable_start);
#else
		const uintptr_t base = 0;
#endif
		for (size_t i = 0; i < top.size() && i < 6; i++) {
			std::string frames;
			for (int f = 0; f < top[i]->depth; f++) {
				char text[32];
				std::snprintf(text, sizeof(text), " %lx",
				              static_cast<unsigned long>(top[i]->frames[static_cast<size_t>(f)] - base));
				frames += text;
			}
			std::printf("SCHEDWAITSTACK waits_s=%.1f ms_s=%.3f at%s\n", static_cast<double>(top[i]->n) / secs,
			            static_cast<double>(top[i]->ns) / 1e6 / secs, frames.c_str());
		}
	}
	std::vector<const StackTotal*> stacks;
	for (const auto& [key, total]: s.stacks) {
		stacks.push_back(&total);
	}
	std::sort(stacks.begin(), stacks.end(), [](const auto* a, const auto* b) { return a->ns > b->ns; });
	for (size_t i = 0; i < stacks.size() && i < 8; i++) {
		std::string frames;
#if defined(__linux__)
		const auto base = reinterpret_cast<uintptr_t>(&__executable_start);
#else
		const uintptr_t base = 0;
#endif
		for (int f = 0; f < stacks[i]->depth; f++) {
			char text[32];
			std::snprintf(text, sizeof(text), " %lx",
			              static_cast<unsigned long>(stacks[i]->frames[static_cast<size_t>(f)] - base));
			frames += text;
		}
		std::printf("GPUWAITSTACK waits_s=%.1f ms_s=%.3f at%s\n",
		            static_cast<double>(stacks[i]->n) / secs,
		            static_cast<double>(stacks[i]->ns) / 1e6 / secs, frames.c_str());
	}
#if defined(__linux__)
	{
		auto&           off = GetOffCpu();
		std::lock_guard off_lock(off.mutex);
		if (off.samples != 0) {
			std::printf("OFFCPU samples=%llu asleep=%.1f%%\n", static_cast<unsigned long long>(off.samples),
			            100.0 * static_cast<double>(off.sleeping) / static_cast<double>(off.samples));
			std::vector<std::pair<uint64_t, const std::string*>> top;
			for (const auto& [key, n]: off.stacks) {
				top.emplace_back(n, &key);
			}
			std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (size_t i = 0; i < top.size() && i < 12; i++) {
				std::printf("OFFCPU share=%.2f%% %s\n",
				            100.0 * static_cast<double>(top[i].first) / static_cast<double>(off.samples),
				            top[i].second->c_str());
			}
			off.stacks.clear();
			off.samples = off.sleeping = 0;
		}
	}
#endif
	std::fflush(stdout);
	s.submits = s.submit_ns = s.idle_ns = s.idle_gaps = s.idle_submit = 0;
	s.waits = s.wait_ns = s.flips = s.early = 0;
	s.sites.clear();
	s.stacks.clear();
	s.sched_sites.clear();
	s.sched_stacks.clear();
	s.sched_waits = s.sched_wait_ns = 0;
	s.period_start = now;
	if (Mode() == 2 && !s.trace.empty()) {
		std::string trace;
		trace.swap(s.trace);
		lock.unlock();
		if (s.trace_file == nullptr) {
			const char* path = std::getenv("KYTY_GPU_WAIT_TRACE");
			s.trace_file     = std::fopen(path != nullptr ? path : "/tmp/gpuwait-trace.txt", "w");
		}
		if (s.trace_file != nullptr) {
			std::fwrite(trace.data(), 1, trace.size(), s.trace_file);
			std::fflush(s.trace_file);
		}
	}
}

} // namespace Common::GpuWaitDiag
