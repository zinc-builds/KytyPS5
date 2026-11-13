#include "common/playtest.h"

#include "common/assert.h"
#include "common/benchMetrics.h"
#include "common/debugCounters.h"
#include "common/frameDiag.h"
#include "kytyGitVersion.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <dirent.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
extern char** environ; // NOLINT
#endif

namespace Common::Playtest {

namespace {

using DC = Common::DebugCounters::Counter;
using FD = Common::FrameDiag::Counter;
using BM = Common::BenchMetrics::Counter;

uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// ---------------------------------------------------------------------------------------------
// Rings. One writer each; readers (the helper thread, the fatal path) never block it. Each slot
// carries a sequence number (odd while written), so a reader detects a slot overwritten under it.

template <typename T, uint32_t N>
struct Ring {
	// The payload is stored as atomic words: a reader racing the writer gets torn data, which the
	// sequence check rejects, never a data race.
	static_assert(sizeof(T) % sizeof(uint64_t) == 0);
	static constexpr size_t Words = sizeof(T) / sizeof(uint64_t);
	struct Slot {
		std::atomic<uint64_t> seq {0};
		std::atomic<uint64_t> data[Words] {};
	};
	std::atomic<uint64_t> head {0};
	Slot                  slots[N];

	void Push(const T& value) noexcept {
		const uint64_t i    = head.load(std::memory_order_relaxed);
		auto&          slot = slots[i % N];
		uint64_t       words[Words];
		std::memcpy(words, &value, sizeof(T));
		slot.seq.store(2 * i + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		for (size_t w = 0; w < Words; w++) {
			slot.data[w].store(words[w], std::memory_order_relaxed);
		}
		slot.seq.store(2 * i + 2, std::memory_order_release);
		head.store(i + 1, std::memory_order_release);
	}
	bool Read(uint64_t i, T* out) const noexcept {
		const auto&    slot = slots[i % N];
		const uint64_t seq  = slot.seq.load(std::memory_order_acquire);
		if (seq != 2 * i + 2) {
			return false;
		}
		uint64_t words[Words];
		for (size_t w = 0; w < Words; w++) {
			words[w] = slot.data[w].load(std::memory_order_relaxed);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (slot.seq.load(std::memory_order_relaxed) != seq) {
			return false;
		}
		std::memcpy(static_cast<void*>(out), words, sizeof(T));
		return true;
	}
};

// Per-frame counter fields: cumulative values in the ring, differences on the helper thread.
enum Field : uint32_t {
	FCpuNs,       // command thread CPU (CLOCK_THREAD_CPUTIME_ID)
	FUserUs,      // its user and kernel time (getrusage)
	FSysUs,
	FMinFlt,
	FMajFlt,
	FVcs,         // voluntary / involuntary context switches
	FIcs,
	FIdleNs,      // waiting for guest work
	FBlockedNs,   // only blocked submissions queued
	FGpuWaitNs,   // blocked on the GPU timeline
	FMutexNs,     // render-context mutex
	FPrepWaitNs,  // prepare workers (BenchMetrics)
	FRecWaitNs,   // recorder
	FTexUpNs,     // texture uploads: source bytes and the upload's recording (playtest's own)
	FMisses,      // program-cache misses and their translation + compile time (playtest's own)
	FMissNs,
	FGcNs,
	FCollectNs,   // write collections and submit scans
	FTexNew,
	FTexFree,
	FBufNew,
	FDraws,
	FDispatches,
	FSubmits,
	FShaders,
	FShaderNs,
	FPipelines,
	FPipelineNs,
	FTexUpB,
	FBufUpB,
	FTexDnB,
	FBufDnB,
	FTexEvict,
	FBufEvict,
	FFlipsRejected,
	FLateVbl,
	FFileRdB,
	FUnderruns,
	FStubCalls,
	FSigill,
	FWtFaults,
	FFieldCount
};
constexpr std::array<const char*, FFieldCount> kFieldNames {
    "cpu_ns",    "user_us",   "sys_us",   "minflt",     "majflt",      "vcs",
    "ics",       "idle_ns",   "blocked_ns", "gpu_wait_ns", "mutex_ns",   "prep_wait_ns",
    "rec_wait_ns", "tex_up_ns", "misses", "miss_ns", "gc_ns",   "collect_ns", "tex_new",    "tex_free",    "buf_new",
    "draws",     "disp",      "submits",  "shaders",    "shader_ns",   "pipes",
    "pipe_ns",   "tex_up_b",  "buf_up_b", "tex_dn_b",   "buf_dn_b",    "tex_evict",
    "buf_evict", "flips_rej", "late_vbl", "file_rd_b",  "underruns",   "stub_calls",
    "sigill",    "wt_faults"};

struct FrameRec {
	uint64_t t_ns      = 0;
	uint64_t g         = 0; // guest frames since start
	uint64_t presented = 0; // presented flips then
	uint64_t v[FFieldCount] {};
	uint32_t flip_queue = 0; // the deepest flip queue the guest asked with since the last frame
	uint32_t pad        = 0;
};

struct PresentRec {
	uint64_t t_ns        = 0;
	uint64_t flip        = 0;
	uint64_t interval_us = 0;
};

struct EventRec {
	uint64_t t_ns  = 0;
	uint64_t g     = 0;
	uint64_t a     = 0;
	uint64_t b     = 0;
	uint64_t c     = 0;
	uint64_t d     = 0;
	uint16_t kind  = 0;
	uint16_t flags = 0;
	uint32_t tid   = 0;
	char     text[64] {};
};

constexpr uint32_t kFrameRing   = 2048; // ~34 s at 60 FPS: the fatal path reads it directly
constexpr uint32_t kPresentRing = 2048;
constexpr uint32_t kEventRing   = 512;
constexpr int      kMaxThreads  = 256;
constexpr uint32_t kMaxStubs    = 16384;

using EventRing = Ring<EventRec, kEventRing>;

constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> kKindNames {
    "shader_compile", "pipeline_create", "texture_upload", "buffer_upload", "gc",
    "stub_first_call", "audio_underrun", "level", "marker", "inject"};

// ---------------------------------------------------------------------------------------------
// Shared state.

std::atomic<bool>     g_started {false};
std::atomic<bool>     g_fatal {false};
std::atomic<bool>     g_stop {false};
std::atomic<bool>     g_helper_done {false};
char                  g_fatal_text[512] {}; // reason and detail, written before g_stop
std::atomic<int>      g_helper_tid {0};
char                  g_dir[512] {};
uint64_t              g_start_ns = 0;
Ring<FrameRec, kFrameRing>*     g_frames   = nullptr;
Ring<PresentRec, kPresentRing>* g_presents = nullptr;
std::atomic<EventRing*>         g_rings[kMaxThreads] {};
std::atomic<int>                g_ring_count {0};
std::mutex                      g_ring_mutex;
thread_local EventRing*         t_ring       = nullptr;
thread_local bool               t_ring_taken = false;
std::atomic<uint64_t>           g_events_dropped {0};
std::atomic<uint64_t>           g_guest_frames {0};
std::atomic<uint32_t>           g_flip_queue_max {0};
std::atomic<uint8_t>            g_stub_seen[kMaxStubs] {};
std::atomic<uint32_t>           g_stub_calls[kMaxStubs] {};
std::atomic<uint64_t>           g_stub_calls_total {0};
std::atomic<uint64_t>           g_texture_upload_ns {0};
std::atomic<uint64_t>           g_shader_misses {0};
std::atomic<uint64_t>           g_shader_miss_ns {0};
thread_local bool               t_in_texture_upload = false;
std::atomic<uint32_t>           g_markers {0};
std::atomic<uint64_t>           g_toast_until_ns {0};
std::atomic<uint32_t>           g_toast_marker {0};
// The level the game last started (written by guest threads, read by the fatal path; a torn read
// only garbles a name).
char                            g_level[64] {};
char                            g_sublevel[64] {};
std::atomic<int64_t>            g_level_start_g {-1};      // the first StartLevel (injections)
std::atomic<int64_t>            g_last_level_start_g {-1}; // the latest (input records)

// Pad reads that changed, for the helper thread (guest threads, ~120 reads/s: a mutex is fine).
struct InputRec {
	uint64_t             t_ns   = 0;
	uint64_t             g      = 0;
	int64_t              lg     = -1;
	int                  handle = 0;
	int                  count  = 0;
	std::vector<uint8_t> bytes;
};
std::mutex            g_input_mutex;
std::deque<InputRec>  g_input_pending;
struct InputLast {
	int                  handle = -1;
	uint64_t             g      = 0;
	std::vector<uint8_t> bytes;
};
std::array<InputLast, 8> g_input_last;

// Screenshot hand-off. state: 0 idle, 1 pending (path set), 2 taken by the presentation thread.
std::mutex            g_shot_mutex;
std::atomic<int>      g_shot_state {0};
char                  g_shot_path[600] {};
char                  g_taken_path[600] {}; // the shot the presentation thread is taking
bool                  g_shot_is_marker = false;
std::string           g_marker_shot_queued; // a marker's shot waiting for the one in flight
struct ShotPixels {
	std::string          path;
	uint32_t             width  = 0;
	uint32_t             height = 0;
	int                  layout = 0;
	std::vector<uint8_t> pixels;
};
std::deque<ShotPixels> g_shot_out; // under g_shot_mutex
std::deque<std::string> g_shot_failed;

// Injected faults: KYTY_PLAYTEST_INJECT="sleep:<level frame>:<ms>,miss:<level frame>,
// upload:<level frame>:<MiB>,exit:<level frame>,segv:<level frame>" (level frames count from the
// first StartLevel).
struct Injection {
	enum class What { Sleep, Miss, Upload, Exit, Segv } what = What::Sleep;
	uint64_t frame = 0;
	uint64_t value = 0;
	bool     done  = false;
};
std::vector<Injection> g_injections; // command thread only after Init
std::atomic<uint32_t>  g_inject_miss {0}; // shader lookups still to miss

std::mutex              g_helper_mutex;
std::condition_variable g_helper_cv;
std::thread*            g_helper = nullptr; // never destroyed: an exit must not wait on it

int CurrentTid() noexcept {
#if defined(__linux__)
	return static_cast<int>(syscall(SYS_gettid));
#else
	return 0;
#endif
}

void CopyText(char* out, size_t size, const char* text) noexcept {
	if (text == nullptr) {
		out[0] = '\0';
		return;
	}
	size_t i = 0;
	for (; i + 1 < size && text[i] != '\0'; i++) {
		out[i] = text[i];
	}
	out[i] = '\0';
}

EventRing* ThreadRing() noexcept {
	if (!t_ring_taken) {
		t_ring_taken = true;
		std::lock_guard lock(g_ring_mutex);
		const int index = g_ring_count.load(std::memory_order_relaxed);
		if (index < kMaxThreads) {
			auto* ring = new (std::nothrow) EventRing(); // never freed: the helper reads it
			if (ring != nullptr) {
				g_rings[index].store(ring, std::memory_order_release);
				g_ring_count.store(index + 1, std::memory_order_release);
				t_ring = ring;
			}
		}
	}
	return t_ring;
}

// JSON string body (no quotes) of `text`.
std::string Escape(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (const char c: text) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
				} else {
					out += c;
				}
		}
	}
	return out;
}

double Ms(uint64_t ns) {
	return static_cast<double>(ns) / 1e6;
}

double RelS(uint64_t t_ns) {
	return t_ns >= g_start_ns ? static_cast<double>(t_ns - g_start_ns) / 1e9
	                          : -static_cast<double>(g_start_ns - t_ns) / 1e9;
}

void ReadCounters(FrameRec& r) noexcept {
	namespace D = Common::DebugCounters;
	namespace B = Common::BenchMetrics;
	const auto fd = [](FD c) {
		return Common::FrameDiag::Detail::g_counters[static_cast<int>(c)].load(std::memory_order_relaxed);
	};
#if defined(__linux__)
	timespec ts {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
		r.v[FCpuNs] = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
	}
	rusage usage {};
	if (getrusage(RUSAGE_THREAD, &usage) == 0) {
		r.v[FUserUs] = static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1'000'000ull +
		               static_cast<uint64_t>(usage.ru_utime.tv_usec);
		r.v[FSysUs] = static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1'000'000ull +
		              static_cast<uint64_t>(usage.ru_stime.tv_usec);
		r.v[FMinFlt] = static_cast<uint64_t>(usage.ru_minflt);
		r.v[FMajFlt] = static_cast<uint64_t>(usage.ru_majflt);
		r.v[FVcs]    = static_cast<uint64_t>(usage.ru_nvcsw);
		r.v[FIcs]    = static_cast<uint64_t>(usage.ru_nivcsw);
	}
#endif
	r.v[FIdleNs]      = fd(FD::CmdIdleNs);
	r.v[FBlockedNs]   = fd(FD::CmdBlockedNs);
	r.v[FGpuWaitNs]   = fd(FD::CmdGpuWaitNs);
	r.v[FMutexNs]     = fd(FD::MutexWaitNs);
	r.v[FGcNs]        = fd(FD::GcNs);
	r.v[FCollectNs]   = fd(FD::CollectNs) + fd(FD::SubmitScanNs);
	r.v[FTexNew]      = fd(FD::TextureCreates);
	r.v[FTexFree]     = fd(FD::TextureFrees);
	r.v[FBufNew]      = fd(FD::BufferCreates);
	r.v[FPrepWaitNs]  = B::Value(BM::DrainWaitNs) + B::Value(BM::RefillWaitNs) +
	                   B::Value(BM::SteadyWaitNs) + B::Value(BM::IdleWaitNs);
	r.v[FRecWaitNs]   = B::Value(BM::RecorderWaitNs);
	r.v[FTexUpNs]     = g_texture_upload_ns.load(std::memory_order_relaxed);
	r.v[FMisses]      = g_shader_misses.load(std::memory_order_relaxed);
	r.v[FMissNs]      = g_shader_miss_ns.load(std::memory_order_relaxed);
	r.v[FDraws]       = D::Get(DC::Draws);
	r.v[FDispatches]  = D::Get(DC::Dispatches);
	r.v[FSubmits]     = D::Get(DC::QueueSubmits);
	r.v[FShaders]     = D::Get(DC::ShadersCompiled);
	r.v[FShaderNs]    = D::Get(DC::ShaderCompileNs);
	r.v[FPipelines]   = D::Get(DC::PipelinesCreated);
	r.v[FPipelineNs]  = D::Get(DC::PipelineCreateNs);
	r.v[FTexUpB]      = D::Get(DC::TextureUploadBytes);
	r.v[FBufUpB]      = D::Get(DC::BufferUploadBytes);
	r.v[FTexDnB]      = D::Get(DC::TextureDownloadBytes);
	r.v[FBufDnB]      = D::Get(DC::BufferDownloadBytes);
	r.v[FTexEvict]    = D::Get(DC::TextureEvictions);
	r.v[FBufEvict]    = D::Get(DC::BufferEvictions);
	r.v[FFlipsRejected] = D::Get(DC::FlipsRejected);
	r.v[FLateVbl]     = D::Get(DC::LateVblanks);
	r.v[FFileRdB]     = D::Get(DC::GuestFileReadBytes);
	r.v[FUnderruns]   = D::Get(DC::AudioUnderruns);
	r.v[FStubCalls]   = g_stub_calls_total.load(std::memory_order_relaxed);
	r.v[FSigill]      = D::Get(DC::EmulatedInstructions);
	r.v[FWtFaults]    = D::Get(DC::GpuFaults);
}

// ---------------------------------------------------------------------------------------------
// The fatal path's writer: a stack buffer and write(2); no locks, no allocation.

struct RawWriter {
	int    fd = -1;
	char   buf[8192];
	size_t used = 0;

	void Flush() noexcept {
#if defined(__linux__)
		size_t off = 0;
		while (fd >= 0 && off < used) {
			const auto n = ::write(fd, buf + off, used - off);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) {
					continue;
				}
				break;
			}
			off += static_cast<size_t>(n);
		}
#endif
		used = 0;
	}
	void Put(const char* s, size_t n) noexcept {
		while (n > 0) {
			if (used == sizeof(buf)) {
				Flush();
			}
			const size_t take = std::min(n, sizeof(buf) - used);
			std::memcpy(buf + used, s, take);
			used += take;
			s += take;
			n -= take;
		}
	}
	void Str(const char* s) noexcept { Put(s, std::strlen(s)); }
	void Esc(const char* s) noexcept {
		for (; s != nullptr && *s != '\0'; s++) {
			const char c = *s;
			if (c == '"' || c == '\\') {
				Put("\\", 1);
				Put(&c, 1);
			} else if (static_cast<unsigned char>(c) < 0x20) {
				Put(" ", 1);
			} else {
				Put(&c, 1);
			}
		}
	}
	[[gnu::format(printf, 2, 3)]] void F(const char* format, ...) noexcept {
		char    line[512];
		va_list args;
		va_start(args, format);
		const int n = std::vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		if (n > 0) {
			Put(line, std::min(static_cast<size_t>(n), sizeof(line) - 1));
		}
	}
};

// ---------------------------------------------------------------------------------------------
// The helper thread.

struct Frame {
	FrameRec d;           // differences against the previous record (t_ns, g, presented: current)
	double   ms   = 0.0;  // guest frame time (between this guest flip and the previous one)
	bool     load = false;
};

struct Cause {
	std::string key;
	std::string text;
	double      ms     = 0.0; // in the worst frame
	double      excess = 0.0; // over the window's usual frames
};

struct OpenIncident {
	int                        id       = 0;
	uint64_t                   open_ns  = 0;
	uint64_t                   last_ns  = 0;
	std::map<std::string, int> triggers;
	std::string                shot;
	std::string                phase;
	std::string                detail; // e.g. the stub's name
};

struct PendingMarker {
	uint32_t    number   = 0;
	uint64_t    press_ns = 0;
	std::string shot;
};

// A reserved-idle CPU this busy (percent of a 100 ms sample) during a slow frame marks it as
// outside load. Quiet: ~5%; the fan daemon's 47 ms poll reads ~47%.
constexpr double OutsideBusy = 25.0;

const char* FieldName(uint32_t i) {
	return kFieldNames[i];
}

class Helper {
public:
	void Run() {
		g_helper_tid.store(CurrentTid(), std::memory_order_release);
#if defined(__linux__)
		// Below the emulator's threads; on the efficiency cores when there are any.
		setpriority(PRIO_PROCESS, static_cast<id_t>(CurrentTid()), 10);
		if (FILE* f = std::fopen("/sys/devices/cpu_atom/cpus", "r"); f != nullptr) {
			char list[128] {};
			if (std::fgets(list, sizeof(list), f) != nullptr) {
				cpu_set_t set;
				CPU_ZERO(&set);
				int         count = 0;
				const char* p     = list;
				while (*p != '\0' && *p != '\n') {
					char*      end = nullptr;
					const long lo  = std::strtol(p, &end, 10);
					long       hi  = lo;
					if (end == p) {
						break;
					}
					p = end;
					if (*p == '-') {
						hi = std::strtol(p + 1, &end, 10);
						p  = end;
					}
					for (long c = lo; c <= hi && c < CPU_SETSIZE; c++) {
						CPU_SET(static_cast<int>(c), &set);
						count++;
					}
					if (*p == ',') {
						p++;
					}
				}
				if (count > 0) {
					sched_setaffinity(0, sizeof(set), &set);
				}
			}
			std::fclose(f);
		}
#endif
		Open();
		std::unique_lock lock(g_helper_mutex);
		while (!g_stop.load(std::memory_order_acquire)) {
			lock.unlock();
			Tick();
			lock.lock();
			g_helper_cv.wait_for(lock, std::chrono::milliseconds(50),
			                     [] { return g_stop.load(std::memory_order_acquire); });
		}
		lock.unlock();
		Tick();
		Finish();
		g_helper_done.store(true, std::memory_order_release);
	}

private:
	FILE* Create(const char* name, const char* mode = "w") {
		const auto path = fmt::format("{}/{}", g_dir, name);
		FILE*      f    = std::fopen(path.c_str(), mode);
		if (f == nullptr) {
			std::printf("PLAYTEST cannot write %s\n", path.c_str());
		}
		return f;
	}

	void Open() {
		m_frames_csv  = Create("frames.csv");
		m_present_csv = Create("present.csv");
		m_events      = Create("events.jsonl", "a");
		m_input       = Create("input.jsonl", "a");
		m_reserved    = Create("reserved_cpus.csv");
		if (m_reserved != nullptr) {
			std::fprintf(m_reserved, "t,cpus,mean_busy,max_busy\n");
		}
		if (m_frames_csv != nullptr) {
			std::fprintf(m_frames_csv, "t,g,presented,ms,load,flip_queue,rss_mb");
			for (uint32_t i = 0; i < FFieldCount; i++) {
				std::fprintf(m_frames_csv, ",%s", FieldName(i));
			}
			std::fprintf(m_frames_csv, "\n");
		}
		if (m_present_csv != nullptr) {
			std::fprintf(m_present_csv, "t,flip,ms\n");
		}
	}

	void Tick() {
		const auto now = NowNs();
		DrainFrames();
		DrainPresents();
		DrainEvents();
		DrainInput();
		SampleRss(now);
		SampleReserved(now);
		CheckFreeze(now);
		WriteShots();
		RequestQueuedMarkerShot();
		FinishMarkers(now, false);
		MaybeCloseIncident(now, false);
		Trim(now);
		if (m_frames_csv != nullptr) {
			std::fflush(m_frames_csv);
		}
		if (m_present_csv != nullptr) {
			std::fflush(m_present_csv);
		}
		if (m_events != nullptr) {
			std::fflush(m_events);
		}
		if (m_input != nullptr) {
			std::fflush(m_input);
		}
		if (m_reserved != nullptr) {
			std::fflush(m_reserved);
		}
	}

	// --- outside load ------------------------------------------------------------------------
	// The CPUs the layout leaves idle on purpose (the SMT siblings of the command thread's and the
	// prepare workers' CPUs, and KYTY_RESERVE_CPUS, that no pinned emulator thread runs on). Host work
	// landing there slows the command thread or a worker (FINDINGS, "outside load"); a spike there
	// during a slow frame is the incident's cause, not the emulator.
	static std::vector<int> ParseCpuList(const char* text) {
		std::vector<int> out;
		const char*      p = text;
		while (p != nullptr && *p != '\0' && *p != '\n') {
			char*      end = nullptr;
			const long lo  = std::strtol(p, &end, 10);
			if (end == p) {
				break;
			}
			long hi = lo;
			p       = end;
			if (*p == '-') {
				hi = std::strtol(p + 1, &end, 10);
				p  = end;
			}
			for (long c = lo; c <= hi && c < 4096; c++) {
				out.push_back(static_cast<int>(c));
			}
			if (*p == ',') {
				p++;
			}
		}
		return out;
	}

	void FindReservedCpus() {
#if defined(__linux__)
		std::vector<int> hot, allowed;
		if (const char* reserve = std::getenv("KYTY_RESERVE_CPUS"); reserve != nullptr) {
			for (const int c: ParseCpuList(reserve)) {
				hot.push_back(c);
			}
		}
		if (auto* dir = opendir("/proc/self/task"); dir != nullptr) {
			while (const auto* entry = readdir(dir)) {
				if (entry->d_name[0] == '.') {
					continue;
				}
				const auto base = fmt::format("/proc/self/task/{}/", entry->d_name);
				char       comm[32] {};
				if (FILE* f = std::fopen((base + "comm").c_str(), "r"); f != nullptr) {
					if (std::fgets(comm, sizeof(comm), f) == nullptr) {
						comm[0] = '\0';
					}
					std::fclose(f);
				}
				if (std::strncmp(comm, "KytyGpuCmd", 10) == 0 || std::strncmp(comm, "KytyGpuPrep", 11) == 0) {
					if (FILE* f = std::fopen((base + "stat").c_str(), "r"); f != nullptr) {
						char buf[1024] {};
						const auto n = std::fread(buf, 1, sizeof(buf) - 1, f);
						std::fclose(f);
						buf[n]           = '\0';
						const char* rest = std::strrchr(buf, ')');
						int         cpu  = -1;
						// processor is field 39; the space after ")" starts field 3 (state).
						for (int field = 2; rest != nullptr && *rest != '\0'; rest++) {
							if (*rest == ' ') {
								field++;
								if (field == 39) {
									cpu = std::atoi(rest + 1);
									break;
								}
							}
						}
						if (cpu >= 0) {
							if (FILE* s = std::fopen(fmt::format("/sys/devices/system/cpu/cpu{}/topology/thread_siblings_list", cpu).c_str(), "r");
							    s != nullptr) {
								char list[64] {};
								if (std::fgets(list, sizeof(list), s) != nullptr) {
									for (const int c: ParseCpuList(list)) {
										hot.push_back(c);
									}
								}
								std::fclose(s);
							}
						}
					}
				}
				if (FILE* f = std::fopen((base + "status").c_str(), "r"); f != nullptr) {
					char line[512];
					while (std::fgets(line, sizeof(line), f) != nullptr) {
						if (std::strncmp(line, "Cpus_allowed_list:", 18) == 0) {
							const char* p = line + 18;
							while (*p == ' ' || *p == '\t') {
								p++;
							}
							// Only pinned threads (one CPU) occupy a CPU: unpinned emulator threads
							// (the main thread, SDL's, ...) may list every CPU but rarely run.
							if (const auto list = ParseCpuList(p); list.size() == 1) {
								allowed.push_back(list[0]);
							}
						}
					}
					std::fclose(f);
				}
			}
			closedir(dir);
		}
		std::vector<int> watch;
		for (const int c: hot) {
			if (std::find(allowed.begin(), allowed.end(), c) == allowed.end() &&
			    std::find(watch.begin(), watch.end(), c) == watch.end()) {
				watch.push_back(c);
			}
		}
		std::sort(watch.begin(), watch.end());
		if (watch != m_reserved_cpus) {
			m_reserved_cpus = watch;
			m_cpu_prev.clear();
			std::string list;
			for (const int c: watch) {
				list += fmt::format("{}{}", list.empty() ? "" : " ", c);
			}
			m_reserved_list = list.empty() ? "none" : list;
			std::printf("PLAYTEST reserved-idle CPUs watched for outside load: %s\n", m_reserved_list.c_str());
			std::fflush(stdout);
		}
#endif
	}

	// Every 100 ms: the busy share of each reserved CPU (/proc/stat).
	void SampleReserved(uint64_t now) {
		if (now - m_last_layout_ns >= (now - g_start_ns < 60'000'000'000ull ? 5'000'000'000ull : 30'000'000'000ull)) {
			m_last_layout_ns = now;
			FindReservedCpus();
		}
		if (m_reserved_cpus.empty() || now - m_last_reserved_ns < 100'000'000ull) {
			return;
		}
		const uint64_t sample_start = m_last_reserved_ns;
		m_last_reserved_ns          = now;
		FILE* f            = std::fopen("/proc/stat", "r");
		if (f == nullptr) {
			return;
		}
		char   line[512];
		double sum = 0, max = 0;
		int    n   = 0;
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			if (std::strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') {
				continue;
			}
			const int cpu = std::atoi(line + 3);
			if (std::find(m_reserved_cpus.begin(), m_reserved_cpus.end(), cpu) == m_reserved_cpus.end()) {
				continue;
			}
			unsigned long long v[10] {};
			const char*        p = std::strchr(line, ' ');
			for (int i = 0; i < 10 && p != nullptr; i++) {
				char* end = nullptr;
				v[i]      = std::strtoull(p, &end, 10);
				p         = end;
			}
			uint64_t total = 0;
			for (const auto x: v) {
				total += x;
			}
			const uint64_t idle = v[3] + v[4];
			auto&          prev = m_cpu_prev[cpu];
			if (prev.first != 0 && total > prev.first) {
				const double busy = 100.0 * (1.0 - static_cast<double>(idle - prev.second) / static_cast<double>(total - prev.first));
				sum += busy;
				max = std::max(max, busy);
				n++;
			}
			prev = {total, idle};
		}
		std::fclose(f);
		if (n == 0) {
			return;
		}
		m_reserved_samples.push_back({sample_start, now, sum / n, max});
		if (m_reserved != nullptr) {
			std::fprintf(m_reserved, "%.3f,%s,%.1f,%.1f\n", RelS(now), m_reserved_list.c_str(), sum / n, max);
		}
	}

	// A slow frame is outside load when a reserved CPU spiked during it and the emulator's own top
	// cause is only what contention on the SMT siblings looks like from inside (slower CPU work,
	// off-CPU time, waits on the workers or the recorder, or nothing). A compile, an upload, a GC,
	// a GPU wait, a collection or the mutex explains the frame by itself.
	static bool OutsideLoad(double busy, const std::vector<Cause>& causes) {
		if (busy < OutsideBusy) {
			return false;
		}
		if (causes.empty() || causes[0].excess < 1.0) {
			return true;
		}
		const auto& top = causes[0].key;
		return top == "command thread CPU (draw work)" || top == "command thread off-CPU" ||
		       top == "prepare-worker wait" || top == "recorder wait";
	}

	// The busiest reserved CPU over the samples that overlap (from, to], or -1 with no samples.
	[[nodiscard]] double ReservedBusy(uint64_t from, uint64_t to) const {
		double best = -1;
		for (const auto& s: m_reserved_samples) {
			if (s.t_ns > from && s.start_ns < to) {
				best = std::max(best, s.max);
			}
		}
		return best;
	}

	// Pad records: the PadData bytes as hex, and its first fields decoded (buttons, sticks,
	// triggers; libs/padData.h).
	void DrainInput() {
		std::deque<InputRec> recs;
		{
			std::lock_guard lock(g_input_mutex);
			recs.swap(g_input_pending);
		}
		if (m_input == nullptr) {
			return;
		}
		for (const auto& r: recs) {
			std::string hex;
			hex.reserve(r.bytes.size() * 2);
			for (const auto b: r.bytes) {
				hex += fmt::format("{:02x}", b);
			}
			uint32_t buttons = 0;
			if (r.bytes.size() >= 10) {
				std::memcpy(&buttons, r.bytes.data(), 4);
			}
			const auto at = [&](size_t i) { return i < r.bytes.size() ? r.bytes[i] : 0; };
			std::fprintf(m_input,
			             "{\"t\":%.4f,\"g\":%" PRIu64 ",\"lg\":%" PRId64 ",\"h\":%d,\"n\":%d,\"buttons\":\"0x%08x\","
			             "\"ls\":[%u,%u],\"rs\":[%u,%u],\"l2\":%u,\"r2\":%u,\"raw\":\"%s\"}\n",
			             RelS(r.t_ns), r.g, r.lg, r.handle, r.count, buttons, at(4), at(5), at(6), at(7), at(8), at(9),
			             hex.c_str());
		}
	}

	void Finish() {
		const auto now = NowNs();
		WriteShots();
		FinishMarkers(now, true);
		if (g_fatal.load(std::memory_order_acquire)) {
			Trigger(now, "fatal", g_fatal_text);
		}
		MaybeCloseIncident(now, true);
		// Session summary.
		std::string stubs;
		for (const auto& [id, name]: m_stub_names) {
			stubs += fmt::format("{}{{\"name\":\"{}\",\"calls\":{}}}", stubs.empty() ? "" : ",",
			                     Escape(name), g_stub_calls[id].load(std::memory_order_relaxed));
		}
		if (FILE* f = Create("session_end.json"); f != nullptr) {
			std::fprintf(f,
			             "{\"end_wall\":\"%s\",\"duration_s\":%.1f,\"guest_frames\":%" PRIu64
			             ",\"presented\":%" PRIu64 ",\"incidents\":%d,\"incidents_unwritten\":%d,"
			             "\"markers\":%u,\"fatal\":%s,\"events_dropped\":%" PRIu64
			             ",\"frames_dropped\":%" PRIu64 ",\"level\":\"%s\",\"stubs\":[%s]}\n",
			             WallNow().c_str(), RelS(now), m_last_g, m_last_presented, m_incident_count,
			             m_incidents_unwritten, g_markers.load(), g_fatal.load() ? "true" : "false",
			             g_events_dropped.load(), m_frames_dropped, Escape(m_level).c_str(), stubs.c_str());
			std::fclose(f);
		}
		DrainInput();
		for (FILE* f: {m_frames_csv, m_present_csv, m_events, m_input, m_reserved}) {
			if (f != nullptr) {
				std::fclose(f);
			}
		}
		m_frames_csv = m_present_csv = m_events = m_input = m_reserved = nullptr;
	}

	static std::string WallNow() {
		const auto now = std::chrono::system_clock::now();
		const auto t   = std::chrono::system_clock::to_time_t(now);
		std::tm    tm {};
		localtime_r(&t, &tm);
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
		return buf;
	}

	// --- frames ----------------------------------------------------------------------------

	void DrainFrames() {
		const auto head = g_frames->head.load(std::memory_order_acquire);
		if (m_frame_cursor + kFrameRing < head) {
			m_frames_dropped += head - kFrameRing - m_frame_cursor;
			m_frame_cursor = head - kFrameRing;
			m_have_prev    = false;
		}
		for (; m_frame_cursor < head; m_frame_cursor++) {
			FrameRec r;
			if (!g_frames->Read(m_frame_cursor, &r)) {
				m_frames_dropped++;
				m_have_prev = false;
				continue;
			}
			if (m_have_prev) {
				Frame f;
				f.d.t_ns       = r.t_ns;
				f.d.g          = r.g;
				f.d.presented  = r.presented;
				f.d.flip_queue = r.flip_queue;
				for (uint32_t i = 0; i < FFieldCount; i++) {
					f.d.v[i] = r.v[i] >= m_prev.v[i] ? r.v[i] - m_prev.v[i] : 0;
				}
				f.ms   = Ms(r.t_ns - m_prev.t_ns);
				UpdateLoad(r.t_ns, f.ms);
				f.load = Phase(r.t_ns) != "play";
				OnFrame(f);
			}
			m_prev      = r;
			m_have_prev = true;
			m_last_g    = r.g;
			m_last_presented = r.presented;
		}
	}

	// A level load lasts from StartLevel until 3 s pass without a frame over 50 ms (at most 90 s).
	void UpdateLoad(uint64_t t_ns, double ms) {
		if (!m_in_load || t_ns < m_level_start_ns) {
			return;
		}
		if (ms > 50.0) {
			m_load_last_slow_ns = t_ns;
		}
		const auto since = std::max(m_level_start_ns, m_load_last_slow_ns);
		if (t_ns - since >= 3'000'000'000ull || t_ns - m_level_start_ns >= 90'000'000'000ull) {
			m_in_load = false;
		}
	}

	[[nodiscard]] std::string Phase(uint64_t t_ns) const {
		if (m_in_load && t_ns >= m_level_start_ns) {
			return "load";
		}
		if (t_ns - g_start_ns < 60'000'000'000ull && m_level_start_ns == 0) {
			return "boot";
		}
		return "play";
	}

	void OnFrame(const Frame& f) {
		m_frames.push_back(f);
		m_last_frame_ns = f.d.t_ns;
		m_frozen        = false;
		if (m_frames_csv != nullptr) {
			std::fprintf(m_frames_csv, "%.4f,%" PRIu64 ",%" PRIu64 ",%.3f,%d,%u,%.1f", RelS(f.d.t_ns),
			             f.d.g, f.d.presented, f.ms, f.load ? 1 : 0, f.d.flip_queue, m_rss_mb);
			for (uint32_t i = 0; i < FFieldCount; i++) {
				std::fprintf(m_frames_csv, ",%" PRIu64, f.d.v[i]);
			}
			std::fprintf(m_frames_csv, "\n");
		}
		// Triggers.
		if (f.ms > 33.4) {
			Trigger(f.d.t_ns, "frame>33ms");
		}
		if (f.ms > 25.0) {
			if (m_last_over25_ns != 0 && f.d.t_ns - m_last_over25_ns < 1'000'000'000ull) {
				Trigger(f.d.t_ns, "2x>25ms/1s");
			}
			m_last_over25_ns = f.d.t_ns;
		}
		// FPS over the last second, once a full second of frames is there (since the start or a
		// freeze).
		if (m_run_start_ns == 0) {
			m_run_start_ns = f.d.t_ns;
		}
		if (f.d.t_ns - m_run_start_ns >= 1'000'000'000ull) {
			int count = 0;
			for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it) {
				if (f.d.t_ns - it->d.t_ns >= 1'000'000'000ull) {
					break;
				}
				count++;
			}
			if (count < 57) {
				Trigger(f.d.t_ns, "fps<57/1s");
			}
		}
	}

	void DrainPresents() {
		const auto head = g_presents->head.load(std::memory_order_acquire);
		if (m_present_cursor + kPresentRing < head) {
			m_present_cursor = head - kPresentRing;
		}
		for (; m_present_cursor < head; m_present_cursor++) {
			PresentRec r;
			if (!g_presents->Read(m_present_cursor, &r)) {
				continue;
			}
			m_presents.push_back(r);
			if (m_present_csv != nullptr) {
				std::fprintf(m_present_csv, "%.4f,%" PRIu64 ",%.3f\n", RelS(r.t_ns), r.flip,
				             static_cast<double>(r.interval_us) / 1000.0);
			}
		}
	}

	// --- events ----------------------------------------------------------------------------

	void DrainEvents() {
		const int count = g_ring_count.load(std::memory_order_acquire);
		if (static_cast<int>(m_event_cursors.size()) < count) {
			m_event_cursors.resize(static_cast<size_t>(count), 0);
		}
		std::vector<EventRec> fresh;
		for (int i = 0; i < count; i++) {
			auto* ring = g_rings[i].load(std::memory_order_acquire);
			if (ring == nullptr) {
				continue;
			}
			auto&      cursor = m_event_cursors[static_cast<size_t>(i)];
			const auto head   = ring->head.load(std::memory_order_acquire);
			if (cursor + kEventRing < head) {
				g_events_dropped.fetch_add(head - kEventRing - cursor, std::memory_order_relaxed);
				cursor = head - kEventRing;
			}
			for (; cursor < head; cursor++) {
				EventRec e;
				if (ring->Read(cursor, &e)) {
					fresh.push_back(e);
				} else {
					g_events_dropped.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		std::sort(fresh.begin(), fresh.end(), [](const auto& x, const auto& y) { return x.t_ns < y.t_ns; });
		for (const auto& e: fresh) {
			OnEvent(e);
		}
	}

	std::string EventJson(const EventRec& e) const {
		const auto kind = static_cast<Kind>(e.kind);
		std::string out = fmt::format("{{\"t\":{:.4f},\"g\":{},\"type\":\"{}\"", RelS(e.t_ns), e.g,
		                              e.kind < kKindNames.size() ? kKindNames[e.kind] : "?");
		switch (kind) {
			case Kind::ShaderCompile:
				out += fmt::format(",\"ms\":{:.3f},\"hash\":\"{:016x}\",\"stage\":\"{}\",\"cache\":\"miss\"{}",
				                   Ms(e.a), e.b, Escape(e.text), (e.flags & 1) != 0 ? ",\"injected\":true" : "");
				break;
			case Kind::PipelineCreate:
				out += fmt::format(",\"ms\":{:.3f},\"pipeline\":\"{}\"", Ms(e.a), e.b != 0 ? "compute" : "graphics");
				break;
			case Kind::TextureUpload:
				out += fmt::format(",\"mb\":{:.2f},\"width\":{},\"height\":{},\"format\":\"{}\",\"mips\":{},\"ms\":{:.3f}",
				                   static_cast<double>(e.a) / 1048576.0, e.b & 0xffffffffu, e.b >> 32u,
				                   Escape(e.text), e.c >> 32u, Ms(e.d));
				break;
			case Kind::BufferUpload:
				out += fmt::format(",\"mb\":{:.2f},\"copy_ms\":{:.3f}{}", static_cast<double>(e.a) / 1048576.0, Ms(e.b),
				                   (e.flags & 1) != 0 ? ",\"for_texture\":true" : "");
				break;
			case Kind::GarbageCollect: out += fmt::format(",\"ms\":{:.3f}", Ms(e.a)); break;
			case Kind::StubFirstCall: out += fmt::format(",\"name\":\"{}\",\"record\":{}", Escape(e.text), e.a); break;
			case Kind::Level:
				out += fmt::format(",\"name\":\"{}\",\"kind\":\"{}\"", Escape(e.text), e.a == 0 ? "StartLevel" : "started");
				break;
			case Kind::Marker: out += fmt::format(",\"number\":{}", e.a); break;
			case Kind::Inject: out += fmt::format(",\"what\":\"{}\",\"value\":{}", Escape(e.text), e.a); break;
			default: break;
		}
		out += "}";
		return out;
	}

	void OnEvent(const EventRec& e) {
		m_events_hist.push_back(e);
		if (m_events != nullptr) {
			std::fprintf(m_events, "%s\n", EventJson(e).c_str());
		}
		switch (static_cast<Kind>(e.kind)) {
			case Kind::StubFirstCall:
				m_stub_names[std::min<uint64_t>(e.a, kMaxStubs - 1)] = e.text;
				Trigger(e.t_ns, "new-stub", e.text);
				break;
			case Kind::Level:
				if (e.a == 0) {
					m_level             = e.text;
					m_level_start_ns    = e.t_ns;
					m_load_last_slow_ns = e.t_ns;
					m_in_load           = true;
				}
				break;
			case Kind::Marker: {
				PendingMarker marker;
				marker.number   = static_cast<uint32_t>(e.a);
				marker.press_ns = e.t_ns;
				marker.shot     = fmt::format("shots/marker-{:03}.ppm", marker.number);
				m_markers.push_back(marker);
				break;
			}
			default: break;
		}
	}

	// --- system samples ----------------------------------------------------------------------

	void SampleRss(uint64_t now) {
		if (now - m_last_rss_ns < 1'000'000'000ull) {
			return;
		}
		m_last_rss_ns = now;
#if defined(__linux__)
		if (FILE* f = std::fopen("/proc/self/statm", "r"); f != nullptr) {
			unsigned long long size = 0, resident = 0;
			if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) {
				m_rss_mb = static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / 1048576.0;
			}
			std::fclose(f);
		}
#endif
	}

	void CheckFreeze(uint64_t now) {
		if (m_frozen && m_open) {
			m_open->last_ns = std::max(m_open->last_ns, now); // open until the flips resume
		}
		if (m_last_frame_ns == 0 || m_frozen || now - m_last_frame_ns < 2'000'000'000ull) {
			return;
		}
		m_frozen       = true;
		m_run_start_ns = 0;
		Trigger(now, "freeze>2s");
	}

	// --- screenshots -------------------------------------------------------------------------

	bool RequestShot(const std::string& relative, bool marker) {
		std::lock_guard lock(g_shot_mutex);
		const auto      path = fmt::format("{}/{}", g_dir, relative);
		const int       state = g_shot_state.load(std::memory_order_acquire);
		if (state == 0 || (state == 1 && marker && !g_shot_is_marker)) {
			CopyText(g_shot_path, sizeof(g_shot_path), path.c_str());
			g_shot_is_marker = marker;
			g_shot_state.store(1, std::memory_order_release);
			return true;
		}
		if (marker) {
			g_marker_shot_queued = path;
			return true;
		}
		return false;
	}

	void RequestQueuedMarkerShot() {
		std::lock_guard lock(g_shot_mutex);
		if (!g_marker_shot_queued.empty() && g_shot_state.load(std::memory_order_acquire) == 0) {
			CopyText(g_shot_path, sizeof(g_shot_path), g_marker_shot_queued.c_str());
			g_shot_is_marker = true;
			g_marker_shot_queued.clear();
			g_shot_state.store(1, std::memory_order_release);
		}
	}

	void WriteShots() {
		std::deque<ShotPixels> out;
		{
			std::lock_guard lock(g_shot_mutex);
			out.swap(g_shot_out);
			for (auto& failed: g_shot_failed) {
				m_failed_shots.push_back(std::move(failed));
			}
			g_shot_failed.clear();
		}
		for (auto& shot: out) {
			FILE* f = std::fopen(shot.path.c_str(), "wb");
			if (f == nullptr) {
				continue;
			}
			std::fprintf(f, "P6\n%u %u\n255\n", shot.width, shot.height);
			std::vector<uint8_t> row(static_cast<size_t>(shot.width) * 3);
			const uint8_t*       src = shot.pixels.data();
			for (uint32_t y = 0; y < shot.height; y++) {
				for (uint32_t x = 0; x < shot.width; x++, src += 4) {
					uint8_t* o = &row[static_cast<size_t>(x) * 3];
					if (shot.layout == 0 || shot.layout == 1) {
						const bool bgra = shot.layout == 1;
						o[0]            = bgra ? src[2] : src[0];
						o[1]            = src[1];
						o[2]            = bgra ? src[0] : src[2];
					} else {
						uint32_t v = 0;
						std::memcpy(&v, src, 4);
						const uint32_t low = (v >> 2u) & 0xffu, mid = (v >> 12u) & 0xffu, high = (v >> 22u) & 0xffu;
						const bool     rgb = shot.layout == 2;
						o[0]               = static_cast<uint8_t>(rgb ? high : low);
						o[1]               = static_cast<uint8_t>(mid);
						o[2]               = static_cast<uint8_t>(rgb ? low : high);
					}
				}
				std::fwrite(row.data(), 1, row.size(), f);
			}
			std::fclose(f);
			m_written_shots.push_back(shot.path);
		}
	}

	[[nodiscard]] std::string ShotStatus(const std::string& relative) const {
		const auto path = fmt::format("{}/{}", g_dir, relative);
		for (const auto& p: m_written_shots) {
			if (p == path) {
				return "written";
			}
		}
		for (const auto& p: m_failed_shots) {
			if (p.rfind(path, 0) == 0) {
				return "failed: " + p.substr(std::min(p.size(), path.size() + 1));
			}
		}
		return "pending";
	}

	// --- incidents ---------------------------------------------------------------------------

	void Trigger(uint64_t t_ns, const std::string& what, const std::string& detail = {}) {
		if (m_open && t_ns >= m_open->last_ns && t_ns - m_open->last_ns > 3'000'000'000ull) {
			MaybeCloseIncident(t_ns, true);
		}
		if (!m_open) {
			m_open.emplace();
			m_open->id      = ++m_incident_count;
			m_open->open_ns = t_ns;
			m_open->last_ns = t_ns;
			m_open->phase   = Phase(t_ns);
			// A screenshot where cheap: at most one every 10 s, and not for every incident of a
			// bad stretch.
			if (m_incident_count <= 200 && t_ns - m_last_shot_ns > 10'000'000'000ull) {
				const auto shot = fmt::format("shots/incident-{:04}.ppm", m_open->id);
				if (RequestShot(shot, false)) {
					m_open->shot   = shot;
					m_last_shot_ns = t_ns;
				}
			}
		}
		m_open->last_ns = std::max(m_open->last_ns, t_ns);
		m_open->triggers[what]++;
		if (!detail.empty() && m_open->detail.find(detail) == std::string::npos) {
			m_open->detail += (m_open->detail.empty() ? "" : "; ") + detail;
		}
	}

	void MaybeCloseIncident(uint64_t now, bool force) {
		if (!m_open) {
			return;
		}
		// Merge: a bad stretch is one record. Close 3 s after the last trigger, or after 30 s.
		const bool quiet = now >= m_open->last_ns && now - m_open->last_ns >= 3'000'000'000ull;
		const bool long_ = now - m_open->open_ns >= 30'000'000'000ull;
		if (!force && !quiet && !long_) {
			return;
		}
		auto incident = std::move(*m_open);
		m_open.reset();
		WriteRecord("incident", incident.id, incident.open_ns, incident.last_ns, incident.triggers, incident.shot,
		            incident.phase, incident.detail, 0);
	}

	void FinishMarkers(uint64_t now, bool force) {
		while (!m_markers.empty()) {
			auto& marker = m_markers.front();
			if (!force && now - marker.press_ns < 1'500'000'000ull) {
				break;
			}
			// Wait up to 3 s for its screenshot.
			if (!force && ShotStatus(marker.shot) == "pending" && now - marker.press_ns < 3'000'000'000ull) {
				break;
			}
			std::map<std::string, int> triggers {{"marker", 1}};
			WriteRecord("marker", static_cast<int>(marker.number), marker.press_ns, marker.press_ns, triggers,
			            marker.shot, Phase(marker.press_ns), {}, marker.number);
			m_markers.pop_front();
		}
	}

	struct FrameCauses {
		double compile = 0, pipeline = 0, gpu_wait = 0, no_work = 0, prep = 0, rec = 0, mutex = 0, gc = 0,
		       collect = 0, tex_upload = 0, upload_copy = 0, offcpu = 0, cpu_other = 0, sys = 0;
	};

	FrameCauses Causes(const Frame& f, double upload_copy_ms) const {
		FrameCauses c;
		const auto& v = f.d.v;
		c.compile     = Ms(std::max(v[FMissNs], v[FShaderNs]));
		c.pipeline    = Ms(v[FPipelineNs]);
		c.gpu_wait    = Ms(v[FGpuWaitNs]);
		c.no_work     = Ms(v[FIdleNs] + v[FBlockedNs]);
		c.prep        = Ms(v[FPrepWaitNs]);
		c.rec         = Ms(v[FRecWaitNs]);
		c.mutex       = Ms(v[FMutexNs]);
		c.gc          = Ms(v[FGcNs]);
		c.collect     = Ms(v[FCollectNs]);
		c.tex_upload  = Ms(v[FTexUpNs]);
		c.upload_copy = upload_copy_ms;
		c.sys         = static_cast<double>(v[FSysUs]) / 1000.0;
		const double cpu = Ms(v[FCpuNs]);
		c.offcpu         = std::max(0.0, f.ms - cpu - c.no_work - c.gpu_wait - c.mutex - c.prep - c.rec);
		c.cpu_other =
		    std::max(0.0, cpu - c.compile - c.pipeline - c.gc - c.collect - c.tex_upload - c.upload_copy);
		return c;
	}

	static std::vector<std::pair<std::string, double FrameCauses::*>> CauseFields() {
		return {{"shader compiles", &FrameCauses::compile},
		        {"pipeline creation", &FrameCauses::pipeline},
		        {"GPU wait", &FrameCauses::gpu_wait},
		        {"game late (no GPU work queued)", &FrameCauses::no_work},
		        {"prepare-worker wait", &FrameCauses::prep},
		        {"recorder wait", &FrameCauses::rec},
		        {"render-context mutex", &FrameCauses::mutex},
		        {"texture/buffer GC", &FrameCauses::gc},
		        {"write collection", &FrameCauses::collect},
		        {"texture uploads", &FrameCauses::tex_upload},
		        {"large buffer upload copies", &FrameCauses::upload_copy},
		        {"command thread off-CPU", &FrameCauses::offcpu},
		        {"command thread CPU (draw work)", &FrameCauses::cpu_other}};
	}

	// Upload copy time of large buffer uploads inside (from, to].
	double UploadCopyMs(uint64_t from, uint64_t to) const {
		double ms = 0;
		for (const auto& e: m_events_hist) {
			if (e.kind == static_cast<uint16_t>(Kind::BufferUpload) && (e.flags & 1) == 0 && e.t_ns > from &&
			    e.t_ns <= to) {
				ms += Ms(e.b);
			}
		}
		return ms;
	}

	// The usual value of each cause over a window: the median over its frames under 20 ms
	// (remembered for the window last asked about).
	const std::vector<double>& Baseline(const std::vector<const Frame*>& window) {
		if (window.data() == m_base_key && window.size() == m_base_n) {
			return m_base;
		}
		const auto fields = CauseFields();
		// Large buffer-upload copy time per frame, in one pass over the events.
		std::vector<double>   upload(window.size(), 0.0);
		std::vector<uint64_t> times(window.size());
		for (size_t i = 0; i < window.size(); i++) {
			times[i] = window[i]->d.t_ns;
		}
		for (const auto& e: m_events_hist) {
			if (e.kind != static_cast<uint16_t>(Kind::BufferUpload) || (e.flags & 1) != 0) {
				continue;
			}
			const auto it = std::lower_bound(times.begin(), times.end(), e.t_ns);
			if (it != times.end()) {
				upload[static_cast<size_t>(it - times.begin())] += Ms(e.b);
			}
		}
		std::vector<std::vector<double>> usual(fields.size());
		for (size_t k = 0; k < window.size(); k++) {
			if (window[k]->ms >= 20.0) {
				continue;
			}
			const auto c = Causes(*window[k], upload[k]);
			for (size_t i = 0; i < fields.size(); i++) {
				usual[i].push_back(c.*(fields[i].second));
			}
		}
		m_base.assign(fields.size(), 0.0);
		for (size_t i = 0; i < fields.size(); i++) {
			auto& u = usual[i];
			if (!u.empty()) {
				std::nth_element(u.begin(), u.begin() + static_cast<ptrdiff_t>(u.size() / 2), u.end());
				m_base[i] = u[u.size() / 2];
			}
		}
		m_base_key = window.data();
		m_base_n   = window.size();
		return m_base;
	}

	std::vector<Cause> Attribute(const std::vector<const Frame*>& window, const Frame& worst, uint64_t worst_from) {
		const auto  fields = CauseFields();
		const auto& basev  = Baseline(window);
		const auto  c      = Causes(worst, UploadCopyMs(worst_from, worst.d.t_ns));
		std::vector<Cause> causes;
		for (size_t i = 0; i < fields.size(); i++) {
			const double base = basev[i];
			Cause cause;
			cause.key    = fields[i].first;
			cause.ms     = c.*(fields[i].second);
			cause.excess = cause.ms - base;
			causes.push_back(cause);
		}
		std::sort(causes.begin(), causes.end(), [](const auto& x, const auto& y) { return x.excess > y.excess; });
		// Details.
		const auto& v = worst.d.v;
		for (auto& cause: causes) {
			if (cause.key == "shader compiles") {
				cause.text = fmt::format("{} program-cache miss(es), of which driver compile {:.1f} ms", v[FMisses],
				                         Ms(v[FShaderNs]));
			} else if (cause.key == "pipeline creation") {
				cause.text = fmt::format("{} pipeline(s)", v[FPipelines]);
			} else if (cause.key == "command thread off-CPU") {
				cause.text = fmt::format("{} voluntary / {} involuntary switches, {} major faults", v[FVcs], v[FIcs],
				                         v[FMajFlt]);
			} else if (cause.key == "command thread CPU (draw work)") {
				cause.text = fmt::format("{} draws, {} dispatches, kernel {:.1f} ms, uploads {:.1f} MB", v[FDraws],
				                         v[FDispatches], static_cast<double>(v[FSysUs]) / 1000.0,
				                         static_cast<double>(v[FTexUpB] + v[FBufUpB]) / 1048576.0);
			} else if (cause.key == "texture uploads") {
				cause.text = fmt::format("{} new textures, {:.1f} MB", v[FTexNew], static_cast<double>(v[FTexUpB]) / 1048576.0);
			} else if (cause.key == "texture/buffer GC") {
				cause.text = fmt::format("{} texture / {} buffer evictions", v[FTexEvict], v[FBufEvict]);
			}
		}
		return causes;
	}

	void WriteRecord(const char* type, int id, uint64_t open_ns, uint64_t last_ns,
	                 const std::map<std::string, int>& triggers, const std::string& shot, const std::string& phase,
	                 const std::string& detail, uint32_t marker) {
		if (std::strcmp(type, "incident") == 0) {
			// Rate limit: at most 300 full records; past that, one line each.
			if (m_incident_files >= 300) {
				m_incidents_unwritten++;
				if (m_events != nullptr) {
					std::fprintf(m_events, "{\"t\":%.4f,\"type\":\"incident\",\"id\":%d,\"unwritten\":true}\n",
					             RelS(open_ns), id);
				}
				return;
			}
			m_incident_files++;
		}
		m_base_key = nullptr; // a new window
		const uint64_t from = open_ns > 10'000'000'000ull ? open_ns - 10'000'000'000ull : 0;
		const uint64_t to   = last_ns + 1'500'000'000ull;
		std::vector<const Frame*> window;
		const Frame*              worst = nullptr;
		uint64_t                  worst_from = 0, prev_t = 0;
		for (const auto& f: m_frames) {
			if (f.d.t_ns >= from && f.d.t_ns <= to) {
				window.push_back(&f);
				const bool in_incident = f.d.t_ns >= open_ns - std::min<uint64_t>(open_ns, 1'000'000'000ull) &&
				                         f.d.t_ns <= last_ns + 100'000'000ull;
				if ((marker != 0 || in_incident) && (worst == nullptr || f.ms > worst->ms)) {
					worst      = &f;
					worst_from = prev_t;
				}
			}
			prev_t = f.d.t_ns;
		}
		std::string out;
		out += fmt::format("{{\"type\":\"{}\",\"id\":{},\"wall\":\"{}\",\"t\":{:.4f},\"t_end\":{:.4f},\"phase\":\"{}\","
		                   "\"level\":\"{}\",\"sublevel\":\"{}\"",
		                   type, id, WallNow(), RelS(open_ns), RelS(last_ns), phase, Escape(m_level),
		                   Escape(g_sublevel));
		if (marker != 0) {
			out += fmt::format(",\"marker\":{}", marker);
		}
		out += ",\"triggers\":{";
		bool first = true;
		for (const auto& [name, count]: triggers) {
			out += fmt::format("{}\"{}\":{}", first ? "" : ",", Escape(name), count);
			first = false;
		}
		out += "}";
		if (!detail.empty()) {
			out += fmt::format(",\"detail\":\"{}\"", Escape(detail));
		}
		std::string summary;
		if (worst != nullptr) {
			out += fmt::format(",\"g\":{},\"worst_ms\":{:.2f},\"worst_g\":{}", worst->d.g, worst->ms, worst->d.g);
			const auto causes = Attribute(window, *worst, worst_from);
			// Large uploads in the bad frames (from the frame before the worst to it), by size.
			std::vector<const EventRec*> uploads;
			for (const auto& e: m_events_hist) {
				if (e.kind == static_cast<uint16_t>(Kind::TextureUpload) && e.t_ns > worst_from && e.t_ns <= worst->d.t_ns) {
					uploads.push_back(&e);
				}
			}
			std::sort(uploads.begin(), uploads.end(), [](const auto* x, const auto* y) { return x->a > y->a; });
			summary = fmt::format("{:.0f} ms frame", worst->ms);
			const double busy    = ReservedBusy(worst->d.t_ns - static_cast<uint64_t>(worst->ms * 1e6), worst->d.t_ns);
			const bool   outside = OutsideLoad(busy, causes);
			out += fmt::format(",\"reserved_cpus\":\"{}\",\"reserved_busy\":{:.1f},\"outside_load\":{}", m_reserved_list,
			                   busy, outside ? "true" : "false");
			if (outside) {
				summary += fmt::format(": outside load (host task on a reserved core: CPUs {} up to {:.0f}% busy during the "
				                       "frame); emulator side",
				                       m_reserved_list, busy);
			}
			int shown = 0;
			for (const auto& cause: causes) {
				if (cause.excess < 1.0 || shown == 3) {
					break;
				}
				summary += fmt::format("{} +{:.0f} ms {}{}", shown == 0 ? ":" : ",", cause.excess, cause.key,
				                       cause.text.empty() ? "" : " (" + cause.text + ")");
				shown++;
			}
			if (shown == 0) {
				summary += ": no single cause over 1 ms above the usual frame";
			}
			if (!uploads.empty()) {
				summary += "; uploads:";
				for (size_t i = 0; i < uploads.size() && i < 3; i++) {
					const auto* e = uploads[i];
					summary += fmt::format(" {}x{} {} {:.1f} MB", e->b & 0xffffffffu, e->b >> 32u, e->text,
					                       static_cast<double>(e->a) / 1048576.0);
				}
				if (uploads.size() > 3) {
					summary += fmt::format(" (+{} more)", uploads.size() - 3);
				}
			}
			// Merged stretches: every other long frame with its own top cause.
			std::string others;
			int         listed = 0;
			uint64_t    prev   = 0;
			double      lost   = 0; // every long frame's time over one refresh (the list is capped)
			out += ",\"long_frames\":[";
			for (const auto* f: window) {
				const auto from = prev;
				prev            = f->d.t_ns;
				if (f->ms <= 33.4 || f->d.t_ns < open_ns - std::min<uint64_t>(open_ns, 1'000'000'000ull) ||
				    f->d.t_ns > last_ns + 100'000'000ull) {
					continue;
				}
				lost += f->ms - 1000.0 / 60.0;
				if (listed == 30) {
					continue;
				}
				const auto fc   = Attribute(window, *f, from);
				const auto fbusy = ReservedBusy(f->d.t_ns - static_cast<uint64_t>(f->ms * 1e6), f->d.t_ns);
				const auto top  = OutsideLoad(fbusy, fc) ? std::string("outside load (host task on a reserved core)")
				                  : fc.empty() || fc[0].excess < 1.0 ? std::string("none")
				                                                    : fc[0].key;
				out += fmt::format("{}{{\"g\":{},\"t\":{:.4f},\"ms\":{:.2f},\"cause\":\"{}\",\"excess_ms\":{:.2f}}}",
				                   listed == 0 ? "" : ",", f->d.g, RelS(f->d.t_ns), f->ms, Escape(top),
				                   fc.empty() ? 0.0 : fc[0].excess);
				if (f != worst && listed < 30) {
					others += fmt::format("{}{:.0f} ms {}", others.empty() ? "" : ", ", f->ms, top);
				}
				listed++;
			}
			out += fmt::format("],\"lost_ms\":{:.2f}", lost);
			// Session times of every frame over 25 ms in the incident that reads as outside load
			// (the report counts their period and matches host processes to them).
			out += ",\"outside_frames\":[";
			{
				int      tagged = 0;
				uint64_t prev2  = 0;
				for (const auto* f: window) {
					const auto from2 = prev2;
					prev2            = f->d.t_ns;
					if (f->ms <= 25.0 || tagged == 200 ||
					    f->d.t_ns < open_ns - std::min<uint64_t>(open_ns, 1'000'000'000ull) ||
					    f->d.t_ns > last_ns + 100'000'000ull) {
						continue;
					}
					const auto fbusy = ReservedBusy(f->d.t_ns - static_cast<uint64_t>(f->ms * 1e6), f->d.t_ns);
					if (fbusy >= OutsideBusy && OutsideLoad(fbusy, Attribute(window, *f, from2))) {
						out += fmt::format("{}{:.4f}", tagged == 0 ? "" : ",", RelS(f->d.t_ns));
						tagged++;
					}
				}
			}
			out += "]";
			if (!others.empty()) {
				summary += "; other long frames: " + others;
			}
			if (triggers.contains("fatal")) {
				summary = fmt::format("FATAL {}; before it: {}", detail, summary);
			} else if (triggers.contains("new-stub")) {
				summary = fmt::format("{}; new unresolved import: {}", summary, detail);
			}
			out += fmt::format(",\"primary_cause\":\"{}\",\"summary\":\"{}\",\"causes\":[",
			                   outside ? "outside load (host task on a reserved core)"
			                   : causes.empty() || causes[0].excess < 1.0 ? "none"
			                                                              : Escape(causes[0].key),
			                   Escape(summary));
			for (size_t i = 0; i < causes.size(); i++) {
				out += fmt::format("{}{{\"cause\":\"{}\",\"ms\":{:.2f},\"excess_ms\":{:.2f},\"detail\":\"{}\"}}",
				                   i == 0 ? "" : ",", Escape(causes[i].key), causes[i].ms, causes[i].excess,
				                   Escape(causes[i].text));
			}
			out += "]";
		}
		if (!shot.empty()) {
			out += fmt::format(",\"screenshot\":\"{}\",\"screenshot_status\":\"{}\"", shot, Escape(ShotStatus(shot)));
		}
		out += fmt::format(",\"rss_mb\":{:.0f}", m_rss_mb);
		// The window: frames and events.
		out += ",\"fields\":[\"t\",\"g\",\"presented\",\"ms\",\"flip_queue\"";
		for (uint32_t i = 0; i < FFieldCount; i++) {
			out += fmt::format(",\"{}\"", FieldName(i));
		}
		out += "],\"frames\":[";
		for (size_t i = 0; i < window.size(); i++) {
			const auto* f = window[i];
			out += fmt::format("{}[{:.4f},{},{},{:.3f},{}", i == 0 ? "" : ",", RelS(f->d.t_ns), f->d.g, f->d.presented,
			                   f->ms, f->d.flip_queue);
			for (uint32_t k = 0; k < FFieldCount; k++) {
				out += fmt::format(",{}", f->d.v[k]);
			}
			out += "]";
		}
		out += "],\"events\":[";
		first = true;
		for (const auto& e: m_events_hist) {
			if (e.t_ns >= from && e.t_ns <= to) {
				out += (first ? "" : ",") + EventJson(e);
				first = false;
			}
		}
		out += "],\"presents_ms\":[";
		first = true;
		for (const auto& p: m_presents) {
			if (p.t_ns >= from && p.t_ns <= to) {
				out += fmt::format("{}{:.2f}", first ? "" : ",", static_cast<double>(p.interval_us) / 1000.0);
				first = false;
			}
		}
		out += "]}\n";
		const auto name = std::strcmp(type, "marker") == 0 ? fmt::format("incidents/marker-{:03}.json", id)
		                                                   : fmt::format("incidents/incident-{:04}.json", id);
		if (FILE* f = Create(name.c_str()); f != nullptr) {
			std::fwrite(out.data(), 1, out.size(), f);
			std::fclose(f);
		}
		// One line in events.jsonl and in the emulator log.
		std::string trig;
		for (const auto& [n, count]: triggers) {
			trig += fmt::format("{}{}x{}", trig.empty() ? "" : ",", n, count);
		}
		if (m_events != nullptr) {
			std::fprintf(m_events,
			             "{\"t\":%.4f,\"type\":\"%s\",\"id\":%d,\"phase\":\"%s\",\"triggers\":\"%s\",\"summary\":\"%s\","
			             "\"file\":\"%s\",\"screenshot\":\"%s\"}\n",
			             RelS(open_ns), type, id, phase.c_str(), Escape(trig).c_str(), Escape(summary).c_str(),
			             name.c_str(), shot.c_str());
		}
		std::printf("PLAYTEST %s %d [%s] %s: %s\n", type, id, phase.c_str(), trig.c_str(), summary.c_str());
		std::fflush(stdout);
	}

	void Trim(uint64_t now) {
		constexpr uint64_t keep = 60'000'000'000ull;
		const auto         old  = [&](uint64_t t) { return now > t && now - t > keep; };
		while (!m_frames.empty() && old(m_frames.front().d.t_ns)) {
			m_frames.pop_front();
		}
		while (!m_presents.empty() && old(m_presents.front().t_ns)) {
			m_presents.pop_front();
		}
		while (!m_events_hist.empty() && old(m_events_hist.front().t_ns)) {
			m_events_hist.pop_front();
		}
		while (!m_reserved_samples.empty() && old(m_reserved_samples.front().t_ns)) {
			m_reserved_samples.pop_front();
		}
	}

private:
	FILE*                         m_frames_csv  = nullptr;
	FILE*                         m_present_csv = nullptr;
	FILE*                         m_events      = nullptr;
	FILE*                         m_input       = nullptr;
	FILE*                         m_reserved    = nullptr;
	struct ReservedSample {
		uint64_t start_ns = 0; // the previous /proc/stat read: the busy share covers [start, t]
		uint64_t t_ns     = 0;
		double   mean = 0, max = 0;
	};
	std::deque<ReservedSample>    m_reserved_samples;
	std::vector<int>              m_reserved_cpus;
	std::string                   m_reserved_list = "none";
	std::map<int, std::pair<uint64_t, uint64_t>> m_cpu_prev;
	uint64_t                      m_last_layout_ns   = 0;
	uint64_t                      m_last_reserved_ns = 0;
	uint64_t                      m_frame_cursor   = 0;
	uint64_t                      m_present_cursor = 0;
	std::vector<uint64_t>         m_event_cursors;
	FrameRec                      m_prev;
	bool                          m_have_prev = false;
	uint64_t                      m_frames_dropped = 0;
	uint64_t                      m_last_g = 0, m_last_presented = 0;
	std::deque<Frame>             m_frames;
	std::deque<PresentRec>        m_presents;
	std::deque<EventRec>          m_events_hist;
	std::map<uint64_t, std::string> m_stub_names;
	std::string                   m_level;
	uint64_t                      m_level_start_ns = 0;
	uint64_t                      m_load_last_slow_ns = 0;
	bool                          m_in_load        = false;
	uint64_t                      m_last_frame_ns  = 0;
	uint64_t                      m_last_over25_ns = 0;
	uint64_t                      m_run_start_ns   = 0;
	bool                          m_frozen         = false;
	uint64_t                      m_last_rss_ns    = 0;
	double                        m_rss_mb         = 0;
	uint64_t                      m_last_shot_ns   = 0;
	std::optional<OpenIncident>   m_open;
	const void*                   m_base_key = nullptr; // Baseline's memo
	size_t                        m_base_n   = 0;
	std::vector<double>           m_base;
	int                           m_incident_count = 0;
	int                           m_incident_files = 0;
	int                           m_incidents_unwritten = 0;
	std::deque<PendingMarker>     m_markers;
	std::vector<std::string>      m_written_shots;
	std::vector<std::string>      m_failed_shots;
};

Helper* g_helper_obj = nullptr;

void ParseInjections() {
	const char* spec = std::getenv("KYTY_PLAYTEST_INJECT");
	if (spec == nullptr) {
		return;
	}
	std::string_view rest(spec);
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		const auto item  = rest.substr(0, comma);
		rest             = comma == std::string_view::npos ? std::string_view {} : rest.substr(comma + 1);
		Injection inj;
		unsigned long long frame = 0, value = 0;
		const std::string  s(item);
		if (std::sscanf(s.c_str(), "sleep:%llu:%llu", &frame, &value) == 2) {
			inj.what = Injection::What::Sleep;
		} else if (std::sscanf(s.c_str(), "miss:%llu:%llu", &frame, &value) >= 1) {
			inj.what = Injection::What::Miss;
			value    = std::max<unsigned long long>(value, 1);
		} else if (std::sscanf(s.c_str(), "upload:%llu:%llu", &frame, &value) == 2) {
			inj.what = Injection::What::Upload;
		} else if (std::sscanf(s.c_str(), "exit:%llu", &frame) == 1) {
			inj.what = Injection::What::Exit;
		} else if (std::sscanf(s.c_str(), "segv:%llu", &frame) == 1) {
			inj.what = Injection::What::Segv;
		} else {
			std::printf("PLAYTEST: ignoring injection '%s'\n", s.c_str());
			continue;
		}
		inj.frame = frame;
		inj.value = value;
		g_injections.push_back(inj);
	}
}

void WriteSessionJson() {
	std::string env;
	for (char** e = environ; e != nullptr && *e != nullptr; e++) {
		const std::string_view kv(*e);
		if (kv.starts_with("KYTY_") || kv.starts_with("SDL_") || kv.starts_with("__GL") ||
		    kv.starts_with("DXVK") || kv.starts_with("VK_")) {
			env += fmt::format("{}\"{}\"", env.empty() ? "" : ",", Escape(kv));
		}
	}
	std::string argv;
	std::string exe;
	std::string build_mode;
#if defined(__linux__)
	if (FILE* f = std::fopen("/proc/self/cmdline", "rb"); f != nullptr) {
		std::string all;
		char        buf[4096];
		size_t      n = 0;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
			all.append(buf, n);
		}
		std::fclose(f);
		size_t start = 0;
		for (size_t i = 0; i <= all.size(); i++) {
			if (i == all.size() || all[i] == '\0') {
				if (i > start) {
					argv += fmt::format("{}\"{}\"", argv.empty() ? "" : ",", Escape(all.substr(start, i - start)));
				}
				start = i + 1;
			}
		}
	}
	char path[1024] {};
	const auto n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n > 0) {
		exe = std::string(path, static_cast<size_t>(n));
		const auto slash = exe.rfind('/');
		if (slash != std::string::npos) {
			if (FILE* f = std::fopen((exe.substr(0, slash) + "/../BUILD_MODE").c_str(), "r"); f != nullptr) {
				char line[512] {};
				if (std::fgets(line, sizeof(line), f) != nullptr) {
					build_mode = line;
					while (!build_mode.empty() && (build_mode.back() == '\n' || build_mode.back() == '\r')) {
						build_mode.pop_back();
					}
				}
				std::fclose(f);
			}
		}
	}
	char cwd[1024] {};
	if (getcwd(cwd, sizeof(cwd) - 1) == nullptr) {
		cwd[0] = '\0';
	}
#endif
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm    tm {};
	localtime_r(&now, &tm);
	char wall[64];
	std::strftime(wall, sizeof(wall), "%Y-%m-%dT%H:%M:%S%z", &tm);
	const auto json = fmt::format(
	    "{{\"start_wall\":\"{}\",\"start_mono_s\":{:.3f},\"pid\":{},\"git_hash\":\"{}\",\"git_revision\":\"{}\",\"build_label\":\"{}\","
	    "\"build_mode\":\"{}\",\"exe\":\"{}\",\"cwd\":\"{}\",\"argv\":[{}],\"env\":[{}],\"inject\":\"{}\"}}\n",
	    wall, static_cast<double>(g_start_ns) / 1e9, getpid(), KYTY_GIT_HASH, KYTY_GIT_REVISION, Escape(KYTY_BUILD_LABEL), Escape(build_mode), Escape(exe),
	    Escape(cwd), argv, env, Escape(std::getenv("KYTY_PLAYTEST_INJECT") != nullptr ? std::getenv("KYTY_PLAYTEST_INJECT") : ""));
	if (FILE* f = std::fopen(fmt::format("{}/session.json", g_dir).c_str(), "w"); f != nullptr) {
		std::fwrite(json.data(), 1, json.size(), f);
		std::fclose(f);
	}
}

void FatalSignalHandler(int signal_number) {
	char detail[64];
	std::snprintf(detail, sizeof(detail), "signal %d", signal_number);
	OnFatal(signal_number == SIGABRT ? "SIGABRT" : "SIGFPE", detail);
	std::signal(signal_number, SIG_DFL);
	std::raise(signal_number);
}

std::terminate_handler g_previous_terminate = nullptr;

[[noreturn]] void TerminateHandler() {
	// Built without exceptions: this is std::terminate from the runtime or a library.
	OnFatal("terminate", "std::terminate");
	if (g_previous_terminate != nullptr) {
		g_previous_terminate();
	}
	std::abort();
}

void MakeDirs(const std::string& path) {
#if defined(__linux__)
	std::string partial;
	for (size_t i = 0; i < path.size(); i++) {
		partial += path[i];
		if (path[i] == '/' && i > 0) {
			::mkdir(partial.c_str(), 0755);
		}
	}
	::mkdir(path.c_str(), 0755);
#endif
}

} // namespace

bool ReadEnabled() noexcept {
	const char* value = std::getenv("KYTY_PLAYTEST");
	return value != nullptr && std::strcmp(value, "0") != 0;
}

void Init() {
	if (!Enabled() || g_started.load(std::memory_order_acquire)) {
		return;
	}
	std::string dir;
	if (const char* d = std::getenv("KYTY_PLAYTEST_DIR"); d != nullptr && *d != '\0') {
		dir = d;
	} else {
		const char* home = std::getenv("HOME");
		const auto  now  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm     tm {};
		localtime_r(&now, &tm);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
		dir = fmt::format("{}/.cache/kyty-playtest/{}", home != nullptr ? home : "/tmp", stamp);
	}
	MakeDirs(dir + "/shots");
	MakeDirs(dir + "/incidents");
	CopyText(g_dir, sizeof(g_dir), dir.c_str());
	g_start_ns = NowNs();
	g_frames   = new Ring<FrameRec, kFrameRing>();
	g_presents = new Ring<PresentRec, kPresentRing>();
	ParseInjections();
	WriteSessionJson();
	g_helper_obj = new Helper();
	g_helper     = new std::thread([] { g_helper_obj->Run(); });
	g_started.store(true, std::memory_order_release);
	// Flush on the way out: a normal exit leaves through quick_exit (emulator.cpp), a guest exit
	// through quick_exit or exit.
	std::at_quick_exit([] { Shutdown(); });
	std::atexit([] { Shutdown(); });
	g_previous_terminate = std::set_terminate(TerminateHandler);
	std::signal(SIGABRT, FatalSignalHandler);
	std::signal(SIGFPE, FatalSignalHandler);
	std::printf("PLAYTEST session %s\n", g_dir);
	std::fflush(stdout);
}

namespace {
void WaitHelper(uint64_t timeout_ms) noexcept {
#if defined(__linux__)
	if (CurrentTid() == g_helper_tid.load(std::memory_order_acquire)) {
		return;
	}
	for (uint64_t waited = 0; waited < timeout_ms && !g_helper_done.load(std::memory_order_acquire); waited += 10) {
		timespec ts {0, 10'000'000};
		nanosleep(&ts, nullptr);
	}
#endif
}
} // namespace

void Shutdown() noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return;
	}
	if (g_stop.exchange(true)) {
		WaitHelper(3000);
		return;
	}
	g_helper_cv.notify_all();
	WaitHelper(3000);
	if (g_helper_done.load(std::memory_order_acquire) && g_helper != nullptr && g_helper->joinable()) {
		g_helper->join();
	}
}

void Event(Kind kind, uint64_t a, uint64_t b, uint64_t c, const char* text, uint16_t flags, uint64_t d) noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return;
	}
	auto* ring = ThreadRing();
	if (ring == nullptr) {
		g_events_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	EventRec e;
	e.t_ns  = NowNs();
	e.g     = g_guest_frames.load(std::memory_order_relaxed);
	e.a     = a;
	e.b     = b;
	e.c     = c;
	e.d     = d;
	e.kind  = static_cast<uint16_t>(kind);
	e.flags = flags;
	CopyText(e.text, sizeof(e.text), text);
	ring->Push(e);
}

uint64_t BeginTextureUpload() noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return 0;
	}
	t_in_texture_upload = true;
	return NowNs();
}

void EndTextureUpload(uint64_t start, uint64_t bytes, uint32_t width, uint32_t height, uint32_t format,
                      uint32_t mips, const char* format_name) noexcept {
	if (start == 0) {
		return;
	}
	t_in_texture_upload = false;
	const auto ns       = NowNs() - start;
	g_texture_upload_ns.fetch_add(ns, std::memory_order_relaxed);
	if (bytes >= TextureUploadEventBytes) {
		Event(Kind::TextureUpload, bytes, width | (static_cast<uint64_t>(height) << 32u),
		      format | (static_cast<uint64_t>(mips) << 32u), format_name, 0, ns);
	}
}

uint64_t BeginShaderMiss() noexcept {
	return Enabled() && g_started.load(std::memory_order_acquire) ? NowNs() : 0;
}

void EndShaderMiss(uint64_t start, uint64_t hash, const char* stage, bool injected) noexcept {
	if (start == 0) {
		return;
	}
	const auto ns = NowNs() - start;
	g_shader_misses.fetch_add(1, std::memory_order_relaxed);
	g_shader_miss_ns.fetch_add(ns, std::memory_order_relaxed);
	Event(Kind::ShaderCompile, ns, hash, 0, stage, injected ? 1 : 0);
}

bool InTextureUpload() noexcept {
	return t_in_texture_upload;
}

uint64_t OnGuestFlip(uint64_t presented_flips) noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return 0;
	}
	FrameRec r;
	r.t_ns       = NowNs();
	r.g          = g_guest_frames.fetch_add(1, std::memory_order_relaxed) + 1;
	r.presented  = presented_flips;
	r.flip_queue = g_flip_queue_max.exchange(0, std::memory_order_relaxed);
	ReadCounters(r);
	g_frames->Push(r);
	if (g_injections.empty()) {
		return 0;
	}
	const auto start = g_level_start_g.load(std::memory_order_relaxed);
	if (start < 0) {
		return 0;
	}
	uint64_t upload = 0;
	for (auto& inj: g_injections) {
		if (inj.done || r.g < static_cast<uint64_t>(start) + inj.frame) {
			continue;
		}
		inj.done = true;
		switch (inj.what) {
			case Injection::What::Sleep:
				Event(Kind::Inject, inj.value, 0, 0, "sleep");
				std::this_thread::sleep_for(std::chrono::milliseconds(inj.value));
				break;
			case Injection::What::Miss:
				Event(Kind::Inject, inj.value, 0, 0, "shader-miss");
				g_inject_miss.store(static_cast<uint32_t>(inj.value), std::memory_order_relaxed);
				break;
			case Injection::What::Upload:
				Event(Kind::Inject, inj.value, 0, 0, "upload");
				upload += inj.value << 20u;
				break;
			case Injection::What::Exit:
				Event(Kind::Inject, 0, 0, 0, "exit");
				EXIT("playtest: injected EXIT at guest frame %llu\n", static_cast<unsigned long long>(r.g));
				break;
			case Injection::What::Segv: {
				Event(Kind::Inject, 0, 0, 0, "segv");
#if defined(__linux__)
				// A real access fault on an inaccessible page, as a host bug would make.
				void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (page != MAP_FAILED) {
					*static_cast<volatile uint32_t*>(page) = 1;
				}
#endif
				break;
			}
		}
	}
	return upload;
}

void OnPresent(uint64_t flip, double interval_ms) noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return;
	}
	PresentRec r;
	r.t_ns        = NowNs();
	r.flip        = flip;
	r.interval_us = static_cast<uint64_t>(std::max(0.0, interval_ms) * 1000.0);
	g_presents->Push(r);
}

void NoteFlipQueue(uint32_t depth) noexcept {
	if (!Enabled()) {
		return;
	}
	auto current = g_flip_queue_max.load(std::memory_order_relaxed);
	while (current < depth &&
	       !g_flip_queue_max.compare_exchange_weak(current, depth, std::memory_order_relaxed)) {
	}
}

void OnStubCall(uint64_t record_id, const char* name) noexcept {
	if (!Enabled()) {
		return;
	}
	g_stub_calls_total.fetch_add(1, std::memory_order_relaxed);
	const auto id = std::min<uint64_t>(record_id, kMaxStubs - 1);
	g_stub_calls[id].fetch_add(1, std::memory_order_relaxed);
	if (g_stub_seen[id].exchange(1, std::memory_order_relaxed) == 0) {
		Event(Kind::StubFirstCall, record_id, 0, 0, name);
	}
}

void OnGuestOutput(const void* data, size_t size) noexcept {
	if (!Enabled() || data == nullptr) {
		return;
	}
	const std::string_view text(static_cast<const char*>(data), size);
	const auto             name_at = [&](size_t pos) {
        std::string name;
        for (; pos < text.size() && name.size() < 63; pos++) {
            const char c = text[pos];
            if (c == ' ' || c == ':' || c == '\n' || c == '\r' || c == '\t') {
                break;
            }
            name += c;
        }
        return name;
	};
	if (const auto pos = text.find("StartLevel "); pos != std::string_view::npos) {
		const auto name = name_at(pos + 11);
		if (!name.empty()) {
			CopyText(g_level, sizeof(g_level), name.c_str());
			int64_t    expected = -1;
			const auto now_g    = static_cast<int64_t>(g_guest_frames.load(std::memory_order_relaxed));
			g_level_start_g.compare_exchange_strong(expected, now_g);
			g_last_level_start_g.store(now_g, std::memory_order_relaxed);
			Event(Kind::Level, 0, 0, 0, name.c_str());
		}
	}
	if (const auto pos = text.find("Level has started: "); pos != std::string_view::npos) {
		const auto name = name_at(pos + 19);
		if (!name.empty()) {
			CopyText(g_sublevel, sizeof(g_sublevel), name.c_str());
			Event(Kind::Level, 1, 0, 0, name.c_str());
		}
	}
}

void Marker() noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire)) {
		return;
	}
	const auto number = g_markers.fetch_add(1, std::memory_order_relaxed) + 1;
	g_toast_marker.store(number, std::memory_order_relaxed);
	g_toast_until_ns.store(NowNs() + 2'500'000'000ull, std::memory_order_release);
	// The screenshot first, so it shows the frame the user reacted to.
	{
		std::lock_guard lock(g_shot_mutex);
		const auto      path  = fmt::format("{}/shots/marker-{:03}.ppm", g_dir, number);
		const int       state = g_shot_state.load(std::memory_order_acquire);
		if (state == 0 || (state == 1 && !g_shot_is_marker)) {
			CopyText(g_shot_path, sizeof(g_shot_path), path.c_str());
			g_shot_is_marker = true;
			g_shot_state.store(1, std::memory_order_release);
		} else {
			g_marker_shot_queued = path;
		}
	}
	Event(Kind::Marker, number);
	std::printf("PLAYTEST marker %u\n", number);
	std::fflush(stdout);
}

void OnPadRead(int handle, const void* data, size_t size, int count) noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire) || data == nullptr || size == 0) {
		return;
	}
	const auto* bytes = static_cast<const uint8_t*>(data);
	const auto  g     = g_guest_frames.load(std::memory_order_relaxed);
	const auto  start = g_last_level_start_g.load(std::memory_order_relaxed);
	std::lock_guard lock(g_input_mutex);
	InputLast*      last = nullptr;
	for (auto& slot: g_input_last) {
		if (slot.handle == handle) {
			last = &slot;
			break;
		}
		if (slot.handle == -1 && last == nullptr) {
			last = &slot;
		}
	}
	if (last == nullptr) {
		last = &g_input_last[0];
	}
	// Unchanged: skip, but keep one record every 600 frames as a checkpoint.
	if (last->handle == handle && last->bytes.size() == size && std::memcmp(last->bytes.data(), bytes, size) == 0 &&
	    g - last->g < 600) {
		return;
	}
	last->handle = handle;
	last->g      = g;
	last->bytes.assign(bytes, bytes + size);
	if (g_input_pending.size() >= 100'000) {
		return; // the helper thread is stuck; don't grow without bound
	}
	InputRec rec;
	rec.t_ns   = NowNs();
	rec.g      = g;
	rec.lg     = start >= 0 ? static_cast<int64_t>(g) - start : -1;
	rec.handle = handle;
	rec.count  = count;
	rec.bytes  = last->bytes;
	g_input_pending.push_back(std::move(rec));
}

bool InjectedMissPending() noexcept {
	return Enabled() && g_inject_miss.load(std::memory_order_relaxed) != 0;
}

bool TakeInjectedMiss() noexcept {
	if (!Enabled() || g_inject_miss.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	// Only the command thread looks up with a possible compile, so this is its counter.
	g_inject_miss.fetch_sub(1, std::memory_order_relaxed);
	return true;
}

const char* PendingShot() noexcept {
	if (!Enabled() || g_shot_state.load(std::memory_order_acquire) != 1) {
		return nullptr;
	}
	std::lock_guard lock(g_shot_mutex);
	if (g_shot_state.load(std::memory_order_acquire) != 1) {
		return nullptr;
	}
	CopyText(g_taken_path, sizeof(g_taken_path), g_shot_path);
	g_shot_state.store(2, std::memory_order_release);
	return g_taken_path;
}

void SubmitShot(uint32_t width, uint32_t height, int layout, std::vector<uint8_t>&& pixels) noexcept {
	std::lock_guard lock(g_shot_mutex);
	ShotPixels      shot;
	shot.path   = g_taken_path;
	shot.width  = width;
	shot.height = height;
	shot.layout = layout;
	shot.pixels = std::move(pixels);
	g_shot_out.push_back(std::move(shot));
	g_shot_state.store(0, std::memory_order_release);
}

void FailShot(const char* why) noexcept {
	std::lock_guard lock(g_shot_mutex);
	g_shot_failed.push_back(std::string(g_taken_path) + " " + (why != nullptr ? why : "failed"));
	g_shot_state.store(0, std::memory_order_release);
}

uint32_t ToastMarker() noexcept {
	if (!Enabled()) {
		return 0;
	}
	return NowNs() < g_toast_until_ns.load(std::memory_order_acquire) ? g_toast_marker.load(std::memory_order_relaxed)
	                                                                  : 0;
}

void OnFatal(const char* reason, const char* detail) noexcept {
	if (!Enabled() || !g_started.load(std::memory_order_acquire) || g_fatal.exchange(true)) {
		return;
	}
	// First, without stdio or allocation: hand the helper its flush request (it polls every
	// 50 ms), so it closes the open incident and flushes while this thread writes crash.json.
	{
		size_t n = 0;
		for (const char* part: {reason, ": ", detail}) {
			for (const char* c = part; c != nullptr && *c != '\0' && n + 1 < sizeof(g_fatal_text); c++) {
				g_fatal_text[n++] = *c;
			}
		}
		g_fatal_text[n] = '\0';
		g_stop.store(true, std::memory_order_release);
	}
#if defined(__linux__)
	char path[600];
	std::snprintf(path, sizeof(path), "%s/crash.json", g_dir);
	RawWriter w;
	w.fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	const auto now = NowNs();
	w.Str("{\"type\":\"fatal\",\"reason\":\"");
	w.Esc(reason);
	w.Str("\",\"detail\":\"");
	w.Esc(detail);
	w.F("\",\"t\":%.4f,\"g\":%" PRIu64 ",\"level\":\"", RelS(now), g_guest_frames.load(std::memory_order_relaxed));
	w.Esc(g_level);
	w.Str("\",\"sublevel\":\"");
	w.Esc(g_sublevel);
	w.F("\",\"markers\":%u,\"thread\":%d,\"fields\":[\"t\",\"g\",\"presented\",\"ms\",\"flip_queue\"",
	    g_markers.load(std::memory_order_relaxed), CurrentTid());
	for (uint32_t i = 0; i < FFieldCount; i++) {
		w.F(",\"%s\"", kFieldNames[i]);
	}
	// The last ~10 s of frames, straight from the command thread's ring.
	w.Str("],\"frames\":[");
	const auto head  = g_frames->head.load(std::memory_order_acquire);
	const auto first = head > 700 ? head - 700 : 0;
	FrameRec   prev;
	bool       have = false, comma = false;
	for (auto i = first; i < head; i++) {
		FrameRec r;
		if (!g_frames->Read(i, &r)) {
			have = false;
			continue;
		}
		if (have && r.t_ns >= now - std::min<uint64_t>(now, 10'500'000'000ull)) {
			w.F("%s[%.4f,%" PRIu64 ",%" PRIu64 ",%.3f,%u", comma ? "," : "", RelS(r.t_ns), r.g, r.presented,
			    Ms(r.t_ns - prev.t_ns), r.flip_queue);
			for (uint32_t k = 0; k < FFieldCount; k++) {
				w.F(",%" PRIu64, r.v[k] >= prev.v[k] ? r.v[k] - prev.v[k] : 0);
			}
			w.Str("]");
			comma = true;
		}
		prev = r;
		have = true;
	}
	// Every thread's events of the last 15 s (unsorted; the report sorts them).
	w.Str("],\"events\":[");
	comma            = false;
	const int rings = g_ring_count.load(std::memory_order_acquire);
	for (int ri = 0; ri < rings; ri++) {
		auto* ring = g_rings[ri].load(std::memory_order_acquire);
		if (ring == nullptr) {
			continue;
		}
		const auto rh = ring->head.load(std::memory_order_acquire);
		for (auto i = rh > kEventRing ? rh - kEventRing : 0; i < rh; i++) {
			EventRec e;
			if (!ring->Read(i, &e) || e.t_ns + 15'000'000'000ull < now) {
				continue;
			}
			w.F("%s{\"t\":%.4f,\"g\":%" PRIu64 ",\"type\":\"%s\",\"a\":%" PRIu64 ",\"b\":%" PRIu64 ",\"c\":%" PRIu64
			    ",\"text\":\"",
			    comma ? "," : "", RelS(e.t_ns), e.g, e.kind < kKindNames.size() ? kKindNames[e.kind] : "?", e.a, e.b,
			    e.c);
			w.Esc(e.text);
			w.Str("\"}");
			comma = true;
		}
	}
	w.Str("]}\n");
	w.Flush();
	if (w.fd >= 0) {
		::close(w.fd);
	}
	{
		RawWriter out;
		out.fd = 1;
		out.Str("PLAYTEST fatal: crash.json written to ");
		out.Str(g_dir);
		out.Str("\n");
		out.Flush();
	}
#endif
	// Give the helper up to 1.5 s to close the open incident (as "fatal") and flush its files.
	WaitHelper(1500);
}

} // namespace Common::Playtest
