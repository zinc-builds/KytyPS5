#pragma once
// KYTY_CHURN_DIAG=1 (diagnostic, GPU thread only): for the watched pages the guest rewrites every
// frame (an asynchronous collection invalidates them in consecutive frames), what each write is
// used for before the next one. Between two invalidations of such a page it counts the PrepareBda
// syncs covering the page (and those that uploaded it), the binds by the path that served them
// (worker clean lookup, worker stream copy, direct stream copy or reuse, sync), and image syncs.
// The question it answers: if such a page stayed unwatched and were treated as CPU-dirty at
// every use ("hot-dirty"), how many copies would each write cost instead of one, and which binds
// would lose the prepare workers' clean path. One CHURN line per second.

#include "graphics/host_gpu/memDiag.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace Libs::Graphics::ChurnDiag {

enum Use : int {
	BdaSync,      // PrepareBda SynchronizeBuffer covering the page
	BdaUpload,    // ... that uploaded the page
	ObtainSync,   // bind through SynchronizeBuffer (ObtainBuffer's slow path)
	ObtainUpload, // ... that uploaded the page
	WorkerClean,  // bind served by a prepare worker's clean lookup (#60), up to 16 KiB: a
	              // worker stream copy would serve it if the page were always dirty
	WorkerCleanLarge, // ... larger: it would fall back to a command-thread sync
	WorkerStream, // bind served by a prepare worker's stream copy (#78)
	DirectStream, // bind stream-copied on the command thread
	StreamReuse,  // bind served by an earlier stream copy
	ImageSync,    // image sync covering the page
	UseCount
};

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_CHURN_DIAG");
		return value != nullptr && value[0] == '1';
	}();
	return on;
}

struct Page {
	uint64_t                       last_frame = ~uint64_t {0}; // frame of the last invalidation
	uint32_t                       streak     = 0; // consecutive frames with an invalidation
	std::array<uint32_t, UseCount> uses {};
};

struct State {
	std::unordered_map<uint64_t, Page> pages;
	// Closed intervals of candidate pages (streak >= 3): totals per use, and a histogram of the
	// command-thread copies hot-dirty would make per write (every sync, and the clean binds a
	// worker stream copy could not serve: 0, 1, 2, 3-4, 5-8, 9+; today it is the uploads).
	uint64_t                       intervals = 0, invalidations = 0, all_intervals = 0;
	std::array<uint64_t, UseCount> totals {};
	std::array<uint64_t, 6>        copies_hist {};
	std::array<uint64_t, 6>        binds_hist {}; // worker-clean binds per write
	uint64_t                       candidate_pages_frame = 0;
	std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
	uint64_t                       last_frames = 0;
};

inline State& Get() {
	static State state;
	return state;
}

inline int Bucket(uint64_t n) {
	return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n <= 4 ? 3 : n <= 8 ? 4 : 5;
}

// A use of [vaddr, vaddr + size) (pages: only those already tracked as rewritten).
inline void NoteUse(uint64_t vaddr, uint64_t size, Use use) {
	if (!Enabled() || size == 0) {
		return;
	}
	auto& state = Get();
	for (auto page = vaddr & ~(TRACKER_PAGE_SIZE - 1); page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		if (auto it = state.pages.find(page); it != state.pages.end()) {
			it->second.uses[use]++;
		}
	}
}

inline void Print(uint64_t frames_now);

// An asynchronous collection found [begin, end) written and invalidated it (watched pages).
inline void NoteInvalidated(uint64_t begin, uint64_t end) {
	if (!Enabled()) {
		return;
	}
	auto&      state = Get();
	const auto frame = MemDiag::g_flips.load(std::memory_order_relaxed);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		auto& p = state.pages[page];
		state.invalidations++;
		if (p.last_frame == frame) {
			continue; // a second write in the same frame: the interval goes on
		}
		const bool consecutive = p.last_frame != ~uint64_t {0} && p.last_frame + 1 == frame;
		if (p.last_frame != ~uint64_t {0}) {
			state.all_intervals++;
		}
		if (consecutive && p.streak >= 2) { // the interval closing now is the 3rd in a row
			state.intervals++;
			uint64_t copies = 0;
			for (int u = 0; u < UseCount; u++) {
				state.totals[u] += p.uses[u];
			}
			// Command-thread copies if always dirty: every sync, and the large clean binds.
			copies = p.uses[BdaSync] + p.uses[ObtainSync] + p.uses[WorkerCleanLarge] + p.uses[ImageSync];
			state.copies_hist[Bucket(copies)]++;
			state.binds_hist[Bucket(p.uses[WorkerClean] + p.uses[WorkerCleanLarge])]++;
		}
		p.streak     = consecutive ? p.streak + 1 : 1;
		p.last_frame = frame;
		p.uses       = {};
	}
	Print(frame);
}

inline void Print(uint64_t frames_now) {
	auto&      state = Get();
	const auto now   = std::chrono::steady_clock::now();
	if (now - state.last < std::chrono::seconds(1)) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - state.last).count();
	const double frames  = static_cast<double>(frames_now - state.last_frames);
	const double n       = state.intervals != 0 ? static_cast<double>(state.intervals) : 1.0;
	const auto&  t       = state.totals;
	std::printf("CHURN frames_s=%.1f invalidated_pages_s=%.0f intervals_s=%.0f candidate_writes_frame=%.0f "
	            "per_write: bda_sync=%.2f bda_upload=%.2f obtain_sync=%.2f obtain_upload=%.2f "
	            "worker_clean=%.2f worker_clean_large=%.2f worker_stream=%.2f direct_stream=%.2f stream_reuse=%.2f "
	            "image_sync=%.2f",
	            frames / seconds, static_cast<double>(state.invalidations) / seconds,
	            static_cast<double>(state.all_intervals) / seconds,
	            frames != 0 ? static_cast<double>(state.intervals) / frames : 0.0, t[BdaSync] / n,
	            t[BdaUpload] / n, t[ObtainSync] / n, t[ObtainUpload] / n, t[WorkerClean] / n,
	            t[WorkerCleanLarge] / n, t[WorkerStream] / n, t[DirectStream] / n, t[StreamReuse] / n, t[ImageSync] / n);
	std::printf(" copies_hist(0,1,2,3-4,5-8,9+)=");
	for (auto v: state.copies_hist) std::printf("%.3f,", static_cast<double>(v) / n);
	std::printf(" clean_binds_hist=");
	for (auto v: state.binds_hist) std::printf("%.3f,", static_cast<double>(v) / n);
	std::printf(" tracked=%zu\n", state.pages.size());
	std::fflush(stdout);
	state.intervals = state.invalidations = state.all_intervals = 0;
	state.totals      = {};
	state.copies_hist = {};
	state.binds_hist  = {};
	state.last        = now;
	state.last_frames = frames_now;
}

} // namespace Libs::Graphics::ChurnDiag
