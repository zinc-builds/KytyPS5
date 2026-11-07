#include "kernel/timeDiag.h"

#include "common/singleton.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::LibKernel::TimeDiag {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t SOURCE_COUNT = static_cast<size_t>(Source::Count);
constexpr int    CLOCK_SLOTS  = 32; // PS5 clock ids are small; the last slot collects the rest.

constexpr std::array<const char*, SOURCE_COUNT> SOURCE_NAMES = {
    "tsc",      "ptime",  "ptc",    "tscfreq", "clock",    "gtod",
    "flipstat", "vbstat", "waitvb", "usleep",  "nanosleep"};

struct State {
	std::array<std::atomic<uint64_t>, SOURCE_COUNT> calls {};
	std::array<std::atomic<uint64_t>, CLOCK_SLOTS>  clock_calls {};
	std::atomic<uint64_t>                           sleep_ns {0};
	std::atomic<uint64_t>                           vblanks {0};
	std::atomic<uint64_t>                           flips_cpu {0};
	std::atomic<uint64_t>                           flips_gpu {0};

	std::mutex mutex;
	// (source, caller) -> calls since the last report.
	std::unordered_map<uint64_t, uint64_t> callers;
	std::unordered_map<int, uint64_t>      flip_modes; // (mode << 8 | rate) -> count
	int64_t                                last_flip_arg = 0;
	// Flips that reached the screen since the last report: wall interval and vblank interval.
	std::vector<double>   flip_ms;
	std::vector<uint64_t> flip_vblanks;
	Clock::time_point     last_flip_done {};
	uint64_t              last_flip_vblank = 0;
};

State& GetState() {
	static State state;
	return state;
}

uint64_t CallerKey(Source source, const void* caller) {
	return (reinterpret_cast<uint64_t>(caller) & 0x00ff'ffff'ffff'ffffull) |
	       (static_cast<uint64_t>(source) << 56u);
}

void RecordCaller(Source source, const void* caller) {
	auto&            s = GetState();
	std::scoped_lock lock(s.mutex);
	s.callers[CallerKey(source, caller)]++;
}

std::string DescribeAddress(uint64_t addr) {
	auto* linker  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = linker->FindProgramByAddr(addr);
	if (program == nullptr) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "0x%" PRIx64, addr);
		return buf;
	}
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%s+0x%" PRIx64, program->file_name.filename().string().c_str(),
	              addr - program->base_vaddr);
	return buf;
}

// KYTY_TIME_DIAG=dump: writes the guest code around a call site once, to
// /tmp/timediag-<module offset>.bin (1 KiB before the return address and 1 KiB after), for
// offline disassembly.
void DumpCode(uint64_t addr) {
	static const bool enabled = [] {
		const char* mode = std::getenv("KYTY_TIME_DIAG");
		return mode != nullptr && std::string_view(mode) == "dump";
	}();
	static std::unordered_map<uint64_t, bool> dumped;
	if (!enabled || dumped[addr]) {
		return;
	}
	dumped[addr]  = true;
	auto* linker  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = linker->FindProgramByAddr(addr);
	if (program == nullptr || addr - program->base_vaddr < 1024 ||
	    addr + 1024 > program->base_vaddr + program->base_size) {
		return;
	}
	char path[96];
	std::snprintf(path, sizeof(path), "/tmp/timediag-%" PRIx64 ".bin", addr - program->base_vaddr);
	if (FILE* f = std::fopen(path, "wb"); f != nullptr) {
		std::fwrite(reinterpret_cast<const void*>(addr - 1024), 1, 2048, f);
		std::fclose(f);
	}
}

template <typename T>
T Percentile(std::vector<T> v, int pct) {
	if (v.empty()) {
		return T {};
	}
	const auto at = v.begin() + static_cast<ptrdiff_t>((v.size() - 1) * pct / 100);
	std::nth_element(v.begin(), at, v.end());
	return *at;
}

void Report(double seconds) {
	auto& s = GetState();

	std::array<uint64_t, SOURCE_COUNT> calls {};
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		calls[i] = s.calls[i].exchange(0, std::memory_order_relaxed);
	}
	std::string clocks;
	for (int i = 0; i < CLOCK_SLOTS; i++) {
		if (auto n = s.clock_calls[i].exchange(0, std::memory_order_relaxed); n != 0) {
			clocks += (clocks.empty() ? "" : ",") + std::to_string(i) + ":" +
			          std::to_string(static_cast<uint64_t>(static_cast<double>(n) / seconds));
		}
	}
	const auto sleep_ns  = s.sleep_ns.exchange(0, std::memory_order_relaxed);
	const auto vblanks   = s.vblanks.exchange(0, std::memory_order_relaxed);
	const auto flips_cpu = s.flips_cpu.exchange(0, std::memory_order_relaxed);
	const auto flips_gpu = s.flips_gpu.exchange(0, std::memory_order_relaxed);

	std::vector<std::pair<uint64_t, uint64_t>> callers;
	std::unordered_map<int, uint64_t>          modes;
	std::vector<double>                        flip_ms;
	std::vector<uint64_t>                      flip_vblanks;
	int64_t                                    flip_arg = 0;
	{
		std::scoped_lock lock(s.mutex);
		callers.assign(s.callers.begin(), s.callers.end());
		s.callers.clear();
		modes.swap(s.flip_modes);
		flip_ms.swap(s.flip_ms);
		flip_vblanks.swap(s.flip_vblanks);
		flip_arg = s.last_flip_arg;
	}

	std::string line = "TIMEDIAG";
	auto        add  = [&](const char* name, double value) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), " %s=%.0f", name, value);
		line += buf;
	};
	add("vblank_s", static_cast<double>(vblanks) / seconds);
	add("flips_s", static_cast<double>(flip_ms.size()) / seconds);
	add("submit_cpu_s", static_cast<double>(flips_cpu) / seconds);
	add("submit_gpu_s", static_cast<double>(flips_gpu) / seconds);
	if (!flip_ms.empty()) {
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              " flip_ms=p50:%.1f,p99:%.1f,max:%.1f flip_vbl=p50:%" PRIu64 ",p99:%" PRIu64
		              ",max:%" PRIu64,
		              Percentile(flip_ms, 50), Percentile(flip_ms, 99),
		              *std::max_element(flip_ms.begin(), flip_ms.end()),
		              Percentile(flip_vblanks, 50), Percentile(flip_vblanks, 99),
		              *std::max_element(flip_vblanks.begin(), flip_vblanks.end()));
		line += buf;
	}
	for (const auto& [key, count]: modes) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), " mode%d_rate%d=%" PRIu64, key >> 8, key & 0xff, count);
		line += buf;
	}
	line += " flip_arg=" + std::to_string(flip_arg);
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		if (calls[i] != 0) {
			add(SOURCE_NAMES[i], static_cast<double>(calls[i]) / seconds);
		}
	}
	if (!clocks.empty()) {
		line += " clock_ids=" + clocks;
	}
	add("sleep_ms_s", static_cast<double>(sleep_ns) / 1e6 / seconds);

	std::sort(callers.begin(), callers.end(),
	          [](const auto& a, const auto& b) { return a.second > b.second; });
	line += " callers:";
	for (size_t i = 0; i < std::min<size_t>(callers.size(), 8); i++) {
		const auto [key, count] = callers[i];
		line += " " + std::string(SOURCE_NAMES[key >> 56u]) + "@" +
		        DescribeAddress(key & 0x00ff'ffff'ffff'ffffull) + "=" +
		        std::to_string(static_cast<uint64_t>(static_cast<double>(count) / seconds));
	}
	// Sites read about once or a few times per flip: the candidates for the game's frame delta.
	const double flips = static_cast<double>(flip_ms.size());
	if (flips > 0) {
		line += " per_flip:";
		for (const auto& [key, count]: callers) {
			const double per_flip = static_cast<double>(count) / flips;
			if (per_flip >= 0.5 && per_flip <= 4.5) {
				char buf[24];
				std::snprintf(buf, sizeof(buf), "=%.2f", per_flip);
				line += " " + std::string(SOURCE_NAMES[key >> 56u]) + "@" +
				        DescribeAddress(key & 0x00ff'ffff'ffff'ffffull) + buf;
				DumpCode(key & 0x00ff'ffff'ffff'ffffull);
			}
		}
	}
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

void ReporterThread() {
	auto last = Clock::now();
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto now = Clock::now();
		Report(std::chrono::duration<double>(now - last).count());
		last = now;
	}
}

bool StartIfEnabled() {
	if (std::getenv("KYTY_TIME_DIAG") == nullptr) {
		return false;
	}
	std::thread(ReporterThread).detach();
	return true;
}

} // namespace

bool Enabled() {
	static const bool enabled = StartIfEnabled();
	return enabled;
}

void NoteCall(Source source, const void* caller) {
	if (!Enabled()) {
		return;
	}
	GetState().calls[static_cast<size_t>(source)].fetch_add(1, std::memory_order_relaxed);
	RecordCaller(source, caller);
}

void NoteClock(int clock_id, const void* caller) {
	if (!Enabled()) {
		return;
	}
	auto& s = GetState();
	s.calls[static_cast<size_t>(Source::ClockGettime)].fetch_add(1, std::memory_order_relaxed);
	const int slot = (clock_id >= 0 && clock_id < CLOCK_SLOTS - 1) ? clock_id : CLOCK_SLOTS - 1;
	s.clock_calls[slot].fetch_add(1, std::memory_order_relaxed);
	RecordCaller(Source::ClockGettime, caller);
}

void NoteSleep(Source source, uint64_t ns, const void* caller) {
	if (!Enabled()) {
		return;
	}
	GetState().sleep_ns.fetch_add(ns, std::memory_order_relaxed);
	NoteCall(source, caller);
}

void NoteFlipSubmit(bool from_gpu, int flip_mode, int64_t flip_arg, int flip_rate) {
	if (!Enabled()) {
		return;
	}
	auto& s = GetState();
	(from_gpu ? s.flips_gpu : s.flips_cpu).fetch_add(1, std::memory_order_relaxed);
	std::scoped_lock lock(s.mutex);
	s.flip_modes[(flip_mode << 8) | (flip_rate & 0xff)]++;
	s.last_flip_arg = flip_arg;
}

void NoteFlipDone(uint64_t vblank_count) {
	if (!Enabled()) {
		return;
	}
	auto&            s   = GetState();
	const auto       now = Clock::now();
	std::scoped_lock lock(s.mutex);
	if (s.last_flip_done != Clock::time_point {}) {
		s.flip_ms.push_back(
		    std::chrono::duration<double, std::milli>(now - s.last_flip_done).count());
		s.flip_vblanks.push_back(vblank_count - s.last_flip_vblank);
	}
	s.last_flip_done   = now;
	s.last_flip_vblank = vblank_count;
}

void NoteVblank() {
	if (!Enabled()) {
		return;
	}
	GetState().vblanks.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Libs::LibKernel::TimeDiag
