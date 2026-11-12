#include "graphics/guest_gpu/frameSkip.h"

#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/hardwareContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::FrameSkip {

namespace Detail {
bool g_active   = false;
bool g_enabled  = false;
bool g_diag     = false;
bool g_skipping = false;
} // namespace Detail

namespace {

using Clock = std::chrono::steady_clock;

// A target counts as redrawn every frame once its write rate (an average over ~20 drawn frames)
// reaches this; it takes ~45 consecutive drawn frames from first sight.
constexpr double EVERY_FRAME_RATE = 0.9;
constexpr double RATE_DECAY       = 0.95;
// Matches TextureCache's readback rule for small tiled targets.
constexpr uint64_t SMALL_TILED_READBACK_TEXELS = 64 * 64;
// Feed-forward margin below the vblank period, and the most frames ever skipped.
constexpr double MARGIN        = 0.05;
constexpr double MAX_SKIP_RATE = 0.5;
// A frame this many vblank periods long is a stall (loading, shader compiles): not a reason to
// skip, and its missed vblanks are not counted.
constexpr double STALL_PERIODS = 4.0;

enum class Mode { Off, Auto, Every };

struct Target {
	double   rate         = 0.0; // write rate over drawn frames (decaying average)
	uint64_t first_frame  = 0;   // drawn-frame index when first seen
	uint64_t last_frame   = 0;   // drawn-frame index when last written
	uint32_t draws        = 0;   // draws this frame
	uint32_t width        = 0;
	uint32_t height       = 0;
	uint32_t format       = 0;
	bool     depth        = false;
	bool     side_effects = false; // a draw into it writes buffers or storage images
	bool     readback     = false; // the texture cache writes it back for the CPU
	bool     keep_listed  = false; // KYTY_FRAME_SKIP_KEEP
	bool     display      = false; // a video-out buffer (scan-out only)
	bool     keep         = true;  // decision for skipped frames, refreshed at every flip
	// Diagnostic totals over the report window.
	uint64_t diag_frames = 0;
	uint64_t diag_draws  = 0;
	uint64_t diag_kept   = 0;
};

struct State {
	Mode     mode  = Mode::Off;
	int      every = 0;
	bool     log   = false;
	double   period_ms = 1000.0 / 60.0;
	std::unordered_set<uint64_t> keep_list; // target or pixel-shader addresses

	// Command thread.
	std::unordered_map<uint64_t, Target> targets;
	uint64_t                             last_key     = ~uint64_t {0};
	Target*                              last_target  = nullptr;
	uint64_t                             frames       = 0; // flips
	uint64_t                             drawn_frames = 0;
	Clock::time_point                    last_flip {};
	uint64_t                             last_cpu_ns  = 0;
	double                               cost_drawn   = 0.0; // ms of command-thread CPU
	double                               cost_skipped = 0.0;
	double                               credit       = 0.0;
	double                               bias         = 0.0;
	uint64_t                             misses_seen  = 0;
	// Per-frame draw counts (command thread).
	uint32_t frame_draws = 0, frame_dropped = 0, frame_kept = 0;
	uint32_t frame_indirect = 0, frame_indirect_count = 0, frame_untargeted = 0;

	// Side-effect draws reported from the commit side, merged at the next flip.
	std::mutex            side_mutex;
	std::vector<uint64_t> side_pending;
	uint64_t              side_buffer_draws = 0, side_image_draws = 0;

	// Presenter thread.
	uint64_t last_vblank       = 0;
	uint64_t last_shown_vblank = 0;

	// Report counters (any thread).
	std::atomic<uint64_t> flips {0}, skipped {0}, shown {0}, misses {0}, dropped_draws {0},
	    kept_draws {0}, cpu_readbacks {0};
	std::atomic<uint64_t> misses_total {0}; // monotonic, for the feedback
	std::atomic<uint32_t> worst_gap {0};
	std::array<std::atomic<uint64_t>, 5> gap_hist {}; // 1, 2, 3, 4, 5+ vblanks
	// Diagnostic totals (command thread).
	uint64_t diag_draws = 0, diag_indirect = 0, diag_indirect_count = 0, diag_untargeted = 0;
	uint64_t diag_frames = 0;

	// Overlay snapshot.
	std::mutex        overlay_mutex;
	OverlayStats      overlay;
	Clock::time_point report_start {};
	uint64_t          report_flips = 0, report_skipped = 0, report_shown = 0;
};

State& S() {
	static auto* state = new State; // the presenter may report during static destruction
	return *state;
}

uint64_t ThreadCpuNs() {
	timespec ts {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// The render target a draw writes: the lowest enabled color slot, else the depth target when
// depth writes are on; 0 for a draw with neither.
struct TargetInfo {
	uint64_t key    = 0;
	uint32_t width  = 0;
	uint32_t height = 0;
	uint32_t format = 0;
	bool     tiled  = false;
	bool     depth  = false;
};

TargetInfo DrawTarget(const HW::Context& ctx) {
	TargetInfo info;
	const uint32_t mask = ctx.GetRenderTargetMask();
	for (uint32_t slot = 0; slot < 8; slot++) {
		if (((mask >> (slot * 4u)) & 0xfu) == 0) {
			continue;
		}
		const auto& rt = ctx.GetRenderTarget(slot);
		if (rt.base.addr == 0 || rt.info.format == Prospero::ChannelLayout::kInvalid) {
			continue;
		}
		info.key    = rt.base.addr;
		info.width  = rt.attrib2.width + 1;
		info.height = rt.attrib2.height + 1;
		info.format = static_cast<uint32_t>(rt.info.format);
		info.tiled  = rt.attrib3.tile_mode != Prospero::TileMode::kLinear;
		return info;
	}
	const auto& depth = ctx.GetDepthRenderTarget();
	if (ctx.GetDepthControl().z_write_enable && depth.z_write_base_addr != 0) {
		info.key    = depth.z_write_base_addr;
		info.width  = depth.size.x_max + 1;
		info.height = depth.size.y_max + 1;
		info.format = static_cast<uint32_t>(depth.z_info.format);
		info.tiled  = true;
		info.depth  = true;
	}
	return info;
}

Target& FindTarget(State& s, const TargetInfo& info) {
	auto [it, inserted] = s.targets.try_emplace(info.key);
	auto& t             = it->second;
	if (inserted) {
		t.first_frame = s.drawn_frames;
		t.width       = info.width;
		t.height      = info.height;
		t.format      = info.format;
		t.depth       = info.depth;
		t.keep_listed = s.keep_list.contains(info.key);
		// The texture cache writes these back for the CPU after every submit that drew them.
		const uint64_t texels = static_cast<uint64_t>(info.width) * info.height;
		t.readback = !info.depth && (info.tiled ? ::Config::ReadbackTiledImagesEnabled() &&
		                                              texels <= SMALL_TILED_READBACK_TEXELS
		                                        : ::Config::ReadbackLinearImagesEnabled());
	}
	return t;
}

void Decide(Target& t) {
	const bool every_frame = t.rate >= EVERY_FRAME_RATE;
	t.keep = !t.display && (t.keep_listed || t.side_effects || t.readback || !every_frame);
}

const char* ClassOf(const Target& t) {
	if (t.display) {
		return "display";
	}
	if (t.keep_listed) {
		return "keep-list";
	}
	if (t.side_effects) {
		return "side-effects";
	}
	if (t.readback) {
		return "cpu-readback";
	}
	return t.rate >= EVERY_FRAME_RATE ? "every-frame" : "periodic";
}

void ParseKeepList(State& s, const char* list) {
	std::string_view rest(list);
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		const auto item  = std::string(rest.substr(0, comma));
		rest             = comma == std::string_view::npos ? std::string_view {} : rest.substr(comma + 1);
		if (!item.empty()) {
			s.keep_list.insert(std::strtoull(item.c_str(), nullptr, 0));
		}
	}
}

// Command thread, every KYTY_FRAME_SKIP_DIAG report.
void DiagReport(State& s) {
	const double frames = static_cast<double>(std::max<uint64_t>(s.diag_frames, 1));
	std::printf("FSDIAG frames=%" PRIu64 " draws_frame=%.0f indirect_frame=%.1f "
	            "indirect_gpu_count_frame=%.1f untargeted_frame=%.1f side_buffer_draws=%" PRIu64
	            " side_image_draws=%" PRIu64 " cpu_readbacks=%" PRIu64 " targets=%zu\n",
	            s.diag_frames, static_cast<double>(s.diag_draws) / frames,
	            static_cast<double>(s.diag_indirect) / frames,
	            static_cast<double>(s.diag_indirect_count) / frames,
	            static_cast<double>(s.diag_untargeted) / frames, s.side_buffer_draws,
	            s.side_image_draws, s.cpu_readbacks.exchange(0), s.targets.size());
	std::vector<std::pair<uint64_t, const Target*>> rows;
	for (const auto& [key, t]: s.targets) {
		if (t.diag_draws != 0) {
			rows.emplace_back(key, &t);
		}
	}
	std::sort(rows.begin(), rows.end(),
	          [](const auto& a, const auto& b) { return a.second->diag_draws > b.second->diag_draws; });
	for (const auto& [key, t]: rows) {
		std::printf("FSDIAG target=0x%" PRIx64 " %ux%u fmt=%u%s class=%s rate=%.2f "
		            "written_frames=%" PRIu64 "/%" PRIu64 " draws_written_frame=%.1f kept_draws=%" PRIu64
		            "\n",
		            key, t->width, t->height, t->format, t->depth ? " depth" : "", ClassOf(*t),
		            t->rate, t->diag_frames, s.diag_frames,
		            static_cast<double>(t->diag_draws) /
		                static_cast<double>(std::max<uint64_t>(t->diag_frames, 1)),
		            t->diag_kept);
	}
	std::fflush(stdout);
	for (auto& [key, t]: s.targets) {
		t.diag_frames = t.diag_draws = t.diag_kept = 0;
	}
	s.diag_frames = s.diag_draws = s.diag_indirect = s.diag_indirect_count = s.diag_untargeted = 0;
	s.side_buffer_draws = s.side_image_draws = 0;
}

void LogReport(State& s, double seconds) {
	const auto flips   = s.flips.exchange(0);
	const auto skipped = s.skipped.exchange(0);
	const auto shown   = s.shown.exchange(0);
	const auto misses  = s.misses.exchange(0);
	const auto dropped = s.dropped_draws.exchange(0);
	const auto kept    = s.kept_draws.exchange(0);
	const auto gap     = s.worst_gap.exchange(0);
	std::array<uint64_t, 5> hist {};
	for (size_t i = 0; i < hist.size(); i++) {
		hist[i] = s.gap_hist[i].exchange(0);
	}
	if (s.log) {
		std::printf("FRAMESKIP flips_s=%.1f shown_s=%.1f skipped_s=%.1f missed_vblanks_s=%.1f "
		            "dropped_draws_s=%.0f kept_draws_s=%.0f worst_gap=%u gaps=%" PRIu64 "/%" PRIu64
		            "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " cost_drawn_ms=%.2f cost_skipped_ms=%.2f "
		            "bias=%.3f\n",
		            static_cast<double>(flips) / seconds, static_cast<double>(shown) / seconds,
		            static_cast<double>(skipped) / seconds, static_cast<double>(misses) / seconds,
		            static_cast<double>(dropped) / seconds, static_cast<double>(kept) / seconds, gap,
		            hist[0], hist[1], hist[2], hist[3], hist[4], s.cost_drawn, s.cost_skipped,
		            s.bias);
		std::fflush(stdout);
	}
	std::scoped_lock lock(s.overlay_mutex);
	s.overlay.skipped_per_s = static_cast<double>(skipped) / seconds;
	s.overlay.shown_per_s   = static_cast<double>(shown) / seconds;
	s.overlay.worst_gap     = gap;
}

} // namespace

void Init() {
	auto& s = S();
	switch (::Config::GetFrameSkipMode()) {
		case ::Config::FrameSkipMode::Off: s.mode = Mode::Off; break;
		case ::Config::FrameSkipMode::Auto: s.mode = Mode::Auto; break;
	}
	if (const char* env = std::getenv("KYTY_FRAME_SKIP"); env != nullptr && *env != '\0') {
		if (std::strcmp(env, "auto") == 0) {
			s.mode = Mode::Auto;
		} else if (std::strcmp(env, "off") == 0 || std::strcmp(env, "0") == 0) {
			s.mode = Mode::Off;
		} else if (const int n = std::atoi(env); n >= 2) {
			// Testing aid: skip every n-th frame regardless of load.
			s.mode  = Mode::Every;
			s.every = n;
		} else {
			std::printf("KYTY_FRAME_SKIP=%s ignored (auto, off, or n >= 2)\n", env);
		}
	}
	if (const char* list = std::getenv("KYTY_FRAME_SKIP_KEEP"); list != nullptr) {
		ParseKeepList(s, list);
	}
	const char* log  = std::getenv("KYTY_FRAME_SKIP_LOG");
	const char* diag = std::getenv("KYTY_FRAME_SKIP_DIAG");
	s.log            = log != nullptr && *log == '1';
	s.period_ms      = 1000.0 / static_cast<double>(::Config::GetVblankFrequency());

	Detail::g_enabled = s.mode != Mode::Off;
	Detail::g_diag    = diag != nullptr && *diag == '1';
	Detail::g_active  = Detail::g_enabled || Detail::g_diag;
	if (Detail::g_enabled) {
		std::printf("Frame skip: %s\n", s.mode == Mode::Auto ? "auto" : "every n-th frame (test)");
	}
	s.report_start = Clock::now();
}

bool OnDraw(const HW::Context& ctx, const HW::Shader& sh, bool indirect, bool indirect_count) {
	auto& s = S();
	s.frame_draws++;
	s.frame_indirect += indirect ? 1u : 0u;
	s.frame_indirect_count += indirect_count ? 1u : 0u;

	const auto info = DrawTarget(ctx);
	if (info.key == 0) {
		// No render target: it exists for its side effects.
		s.frame_untargeted++;
		if (Detail::g_skipping) {
			s.frame_kept++;
		}
		return false;
	}
	Target* t = s.last_target;
	if (info.key != s.last_key) {
		t             = &FindTarget(s, info);
		s.last_key    = info.key;
		s.last_target = t;
	}
	t->draws++;
	if (!Detail::g_skipping) {
		return false;
	}
	if (t->keep || (!s.keep_list.empty() && s.keep_list.contains(sh.GetPs().ps_regs.data_addr))) {
		s.frame_kept++;
		t->diag_kept++;
		return false;
	}
	s.frame_dropped++;
	return true;
}

void NoteSideEffectDraw(const HW::Context& ctx, bool buffer_writes, bool image_writes) {
	const auto info = DrawTarget(ctx);
	auto&      s    = S();
	std::scoped_lock lock(s.side_mutex);
	s.side_pending.push_back(info.key);
	s.side_buffer_draws += buffer_writes ? 1u : 0u;
	s.side_image_draws += image_writes ? 1u : 0u;
}

void NoteCpuReadback(uint64_t /*vaddr*/) {
	S().cpu_readbacks.fetch_add(1, std::memory_order_relaxed);
}

void NoteFlipDone(uint64_t vblank, bool presented) {
	auto& s = S();
	if (s.last_vblank != 0 && vblank > s.last_vblank) {
		const uint64_t delta = vblank - s.last_vblank;
		// A long gap is a stall (loading); frame skipping cannot win it back.
		if (delta >= 2 && delta <= 8) {
			s.misses.fetch_add(delta - 1, std::memory_order_relaxed);
			s.misses_total.fetch_add(delta - 1, std::memory_order_relaxed);
		}
	}
	s.last_vblank = vblank;
	if (presented) {
		if (s.last_shown_vblank != 0 && vblank > s.last_shown_vblank) {
			const uint64_t gap = vblank - s.last_shown_vblank;
			s.gap_hist[std::min<uint64_t>(gap, 5) - 1].fetch_add(1, std::memory_order_relaxed);
			if (gap <= 8) {
				auto worst = s.worst_gap.load(std::memory_order_relaxed);
				while (gap > worst && !s.worst_gap.compare_exchange_weak(
				                          worst, static_cast<uint32_t>(gap), std::memory_order_relaxed)) {
				}
			}
		}
		s.last_shown_vblank = vblank;
		s.shown.fetch_add(1, std::memory_order_relaxed);
	}
}

void OnFlip(const uint64_t* display_buffers, int display_buffer_count) {
	auto&      s       = S();
	const auto now     = Clock::now();
	const bool skipped = Detail::g_skipping;
	s.frames++;
	s.flips.fetch_add(1, std::memory_order_relaxed);
	s.dropped_draws.fetch_add(s.frame_dropped, std::memory_order_relaxed);
	s.kept_draws.fetch_add(s.frame_kept, std::memory_order_relaxed);
	if (skipped) {
		s.skipped.fetch_add(1, std::memory_order_relaxed);
	}

	// Side-effect draws the commit side saw since the last flip.
	{
		std::scoped_lock lock(s.side_mutex);
		for (const auto key: s.side_pending) {
			if (auto it = s.targets.find(key); it != s.targets.end()) {
				it->second.side_effects = true;
			}
		}
		s.side_pending.clear();
	}
	for (int i = 0; i < display_buffer_count; i++) {
		if (auto it = s.targets.find(display_buffers[i]); it != s.targets.end()) {
			it->second.display = true;
		}
	}

	// Write rates count drawn frames only: a skipped frame says nothing about a target's cadence.
	if (!skipped) {
		s.drawn_frames++;
	}
	for (auto& [key, t]: s.targets) {
		if (!skipped) {
			const bool written = t.draws != 0;
			t.rate             = t.rate * RATE_DECAY + (written ? 1.0 - RATE_DECAY : 0.0);
			if (written) {
				t.last_frame = s.drawn_frames;
			}
		}
		if (Detail::g_diag && t.draws != 0) {
			t.diag_frames++;
			t.diag_draws += t.draws;
		}
		t.draws = 0;
		Decide(t);
	}
	// Forget targets not drawn for a long time (their memory is likely reused).
	if ((s.frames & 1023u) == 0) {
		std::erase_if(s.targets, [&](const auto& kv) {
			return s.drawn_frames - kv.second.last_frame > 4096 && kv.second.first_frame + 4096 < s.drawn_frames;
		});
	}
	s.last_key    = ~uint64_t {0};
	s.last_target = nullptr;

	if (Detail::g_diag) {
		s.diag_frames++;
		s.diag_draws += s.frame_draws;
		s.diag_indirect += s.frame_indirect;
		s.diag_indirect_count += s.frame_indirect_count;
		s.diag_untargeted += s.frame_untargeted;
		if (s.diag_frames == 600) {
			DiagReport(s);
		}
	}
	s.frame_draws = s.frame_dropped = s.frame_kept = 0;
	s.frame_indirect = s.frame_indirect_count = s.frame_untargeted = 0;

	// Reports once a second.
	if (const double seconds = std::chrono::duration<double>(now - s.report_start).count();
	    seconds >= 1.0) {
		LogReport(s, seconds);
		s.report_start = now;
	}

	if (!Detail::g_enabled) {
		return;
	}

	// The command thread's CPU time for the frame just finished: what a drawn and a skipped
	// frame cost it, without the time it waited (for guest work, a flip slot, the vblank).
	const uint64_t cpu_ns  = ThreadCpuNs();
	const double   cost_ms = s.last_cpu_ns == 0 ? 0.0 : static_cast<double>(cpu_ns - s.last_cpu_ns) / 1e6;
	const double   wall_ms = s.last_flip == Clock::time_point {}
	                             ? 0.0
	                             : std::chrono::duration<double, std::milli>(now - s.last_flip).count();
	s.last_cpu_ns = cpu_ns;
	s.last_flip   = now;
	const bool stall = wall_ms > STALL_PERIODS * s.period_ms;
	if (!stall && cost_ms > 0.0) {
		double& avg = skipped ? s.cost_skipped : s.cost_drawn;
		avg         = avg == 0.0 ? cost_ms : avg * 0.9 + cost_ms * 0.1;
	}

	bool skip = false;
	if (s.mode == Mode::Every) {
		skip = (s.frames + 1) % static_cast<uint64_t>(s.every) == 0;
	} else {
		// Feed-forward: the share of frames to skip so the average frame fits the period:
		// (1 - f) * drawn + f * skipped <= period * (1 - MARGIN).
		const double target    = s.period_ms * (1.0 - MARGIN);
		const double skipped_c = s.cost_skipped > 0.0 ? s.cost_skipped : s.cost_drawn * 0.25;
		const double saving    = std::max(s.cost_drawn - skipped_c, 0.1 * s.period_ms);
		const double share     = std::clamp((s.cost_drawn - target) / saving, 0.0, MAX_SKIP_RATE);
		// Feedback: a vblank that passed without a flip is a game tick lost. Each one asks for a
		// skip soon, and raises the share for a while (the CPU cost misses pipeline waits).
		const uint64_t misses_total = s.misses_total.load(std::memory_order_relaxed);
		const uint64_t misses       = misses_total - s.misses_seen;
		s.misses_seen               = misses_total;
		s.bias = std::clamp(s.bias * 0.99 + (stall ? 0.0 : 0.05 * static_cast<double>(std::min<uint64_t>(misses, 2))),
		                    0.0, MAX_SKIP_RATE);
		s.credit = std::min(s.credit + std::min(share + s.bias, MAX_SKIP_RATE), 1.5);
		if (misses != 0 && !stall) {
			s.credit = std::max(s.credit, 1.0);
		}
		skip = !skipped && !stall && s.credit >= 1.0;
		if (skip) {
			s.credit -= 1.0;
		}
	}
	Detail::g_skipping = skip;
}

OverlayStats GetOverlayStats() {
	auto&            s = S();
	std::scoped_lock lock(s.overlay_mutex);
	return s.overlay;
}

} // namespace Libs::Graphics::FrameSkip
