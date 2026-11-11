#include "graphics/guest_gpu/frameSkip.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace Libs::Graphics::FrameSkip {

namespace Detail {
bool              g_enabled = false;
std::atomic<bool> g_skipping {false};
} // namespace Detail

namespace {

using Clock = std::chrono::steady_clock;

constexpr double BUDGET_MS = 1000.0 / 60.0;

int  g_every = 0;     // fixed mode: skip every n-th frame
bool g_auto  = false; // adaptive mode

// Command-thread state (only touched by the command thread).
uint64_t          g_frame = 0;
Clock::time_point g_last_flip {};
double            g_debt_ms = 0.0;

// Reporter counters.
std::atomic<uint64_t> g_flips {0};
std::atomic<uint64_t> g_skipped_frames {0};
std::atomic<uint64_t> g_skipped_draws {0};
std::atomic<uint64_t> g_frame_us_rendered {0};
std::atomic<uint64_t> g_frame_us_skipped {0};
std::atomic<double>   g_debt_report {0.0};
std::atomic<uint64_t> g_pending_sum {0};

void Reporter() {
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto flips   = g_flips.exchange(0);
		const auto skipped = g_skipped_frames.exchange(0);
		const auto draws   = g_skipped_draws.exchange(0);
		const auto us_r    = g_frame_us_rendered.exchange(0);
		const auto us_s    = g_frame_us_skipped.exchange(0);
		const auto pending = g_pending_sum.exchange(0);
		const auto rendered = flips - std::min(flips, skipped);
		std::printf("FRAMESKIP flips_s=%llu rendered_s=%llu skipped_s=%llu skipped_draws_s=%llu "
		            "cmd_ms_rendered=%.2f cmd_ms_skipped=%.2f debt_ms=%.1f pending=%.2f\n",
		            static_cast<unsigned long long>(flips),
		            static_cast<unsigned long long>(rendered),
		            static_cast<unsigned long long>(skipped),
		            static_cast<unsigned long long>(draws),
		            rendered != 0 ? static_cast<double>(us_r) / 1000.0 / static_cast<double>(rendered)
		                          : 0.0,
		            skipped != 0 ? static_cast<double>(us_s) / 1000.0 / static_cast<double>(skipped)
		                         : 0.0,
		            g_debt_report.load(),
		            flips != 0 ? static_cast<double>(pending) / static_cast<double>(flips) : 0.0);
		std::fflush(stdout);
	}
}

} // namespace

void Init() {
	const char* mode = std::getenv("KYTY_FRAME_SKIP");
	if (mode == nullptr || *mode == '\0' || std::strcmp(mode, "0") == 0) {
		return;
	}
	if (std::strcmp(mode, "auto") == 0) {
		g_auto = true;
	} else {
		g_every = std::atoi(mode);
		if (g_every < 2) {
			std::printf("KYTY_FRAME_SKIP=%s ignored (use auto or n >= 2)\n", mode);
			return;
		}
	}
	Detail::g_enabled = true;
	std::printf("Frame skip: %s\n", g_auto ? "auto (when behind the 60 Hz budget)" : mode);
	if (const char* log = std::getenv("KYTY_FRAME_SKIP_LOG"); log != nullptr && *log == '1') {
		std::thread(Reporter).detach();
	}
}

void NoteSkippedDraw() {
	g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
}

void OnFlip(int pending_flips) {
	const auto now        = Clock::now();
	const bool was_skip   = Detail::g_skipping.load(std::memory_order_relaxed);
	const auto elapsed_ms = g_frame == 0 ? BUDGET_MS
	                                     : std::chrono::duration<double, std::milli>(now - g_last_flip)
	                                           .count();
	g_last_flip = now;
	g_frame++;
	g_flips.fetch_add(1, std::memory_order_relaxed);
	(was_skip ? g_frame_us_skipped : g_frame_us_rendered)
	    .fetch_add(static_cast<uint64_t>(elapsed_ms * 1000.0), std::memory_order_relaxed);
	if (was_skip) {
		g_skipped_frames.fetch_add(1, std::memory_order_relaxed);
	}

	bool skip = false;
	if (g_every != 0) {
		skip = (g_frame + 1) % static_cast<uint64_t>(g_every) == 0;
	} else {
		// Reported only: how far the command thread runs over the 60 Hz budget.
		if (elapsed_ms > 4.0 * BUDGET_MS) {
			g_debt_ms = 0.0;
		} else {
			g_debt_ms = std::clamp(g_debt_ms + elapsed_ms - BUDGET_MS, 0.0, 3.0 * BUDGET_MS);
		}
		// Nothing banked behind this flip, and the last frame was drawn. A long stall (loading,
		// shader compiles) is not won back by skipping one frame.
		skip = !was_skip && pending_flips <= 1 && elapsed_ms <= 4.0 * BUDGET_MS;
	}
	g_pending_sum.fetch_add(static_cast<uint64_t>(std::max(pending_flips, 0)),
	                        std::memory_order_relaxed);
	g_debt_report.store(g_debt_ms, std::memory_order_relaxed);
	Detail::g_skipping.store(skip, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::FrameSkip
