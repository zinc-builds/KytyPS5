#include "graphics/host_gpu/vulkanDiagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <execinfo.h>
extern "C" char __ehdr_start; // NOLINT: the ELF header, so offsets match the lld map
#endif

namespace Libs::Graphics::VulkanDiagnostics {

namespace {

enum Kind {
	Barrier,
	Barrier2,
	BeginRendering,
	EndRendering,
	Draw,
	DrawIndexed,
	DrawIndirect,
	DrawMesh,
	Dispatch,
	DispatchIndirect,
	BindPipeline,
	BindPipelineRepeat,
	PushDescriptor,
	PushDescriptorElements,
	PushConstants,
	CopyBuffer,
	CopyImage,
	Clear,
	BeginQuery,
	KindCount
};

constexpr std::array<const char*, KindCount> KindNames = {
    "barrier",  "barrier2",          "begin_rendering", "end_rendering", "draw",           "draw_indexed",
    "draw_indirect", "draw_mesh",    "dispatch",        "dispatch_indirect", "bind_pipeline", "bind_pipeline_repeat",
    "push_descriptor", "push_descriptor_elements", "push_constants", "copy_buffer", "copy_image",     "clear", "begin_query"};

std::array<std::atomic<uint64_t>, KindCount> g_counts {};
bool                                         g_sites = false;
std::mutex                                   g_mutex; // g_layouts, g_site_counts
std::map<std::pair<int, int>, uint64_t>      g_layouts;
std::map<std::pair<int, uintptr_t>, uint64_t> g_site_counts;

void Count(Kind kind) {
	g_counts[kind].fetch_add(1, std::memory_order_relaxed);
}

[[gnu::noinline]] void Site(Kind kind) {
#if defined(__linux__)
	if (!g_sites) {
		return;
	}
	// Site, the wrapper, then the caller of the vulkan.hpp method (usually inlined) and its caller.
	// Three callers: kind, kind + 100 and kind + 200.
	std::array<void*, 5> frames {};
	const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
	const auto base = reinterpret_cast<uintptr_t>(&__ehdr_start);
	std::lock_guard lock(g_mutex);
	for (int depth = 2; depth < count; depth++) {
		g_site_counts[{kind + (depth - 2) * 100, reinterpret_cast<uintptr_t>(frames[depth]) - base}]++;
	}
#else
	(void)kind;
#endif
}

void Report() {
	for (uint32_t seconds = 1;; seconds++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::string line = "VKCOUNT";
		for (int kind = 0; kind < KindCount; kind++) {
			line += " " + std::string(KindNames[kind]) + "=" +
			        std::to_string(g_counts[kind].exchange(0, std::memory_order_relaxed));
		}
		std::string layouts = "VKLAYOUT";
		{
			std::lock_guard lock(g_mutex);
			for (const auto& [key, count]: g_layouts) {
				layouts += " " + std::to_string(key.first) + "->" + std::to_string(key.second) +
				           "=" + std::to_string(count);
			}
			g_layouts.clear();
			if (g_sites && seconds % 20 == 0) {
				for (const auto& [key, count]: g_site_counts) {
					std::printf("VKSITE kind=%d addr=%lx count=%llu\n", key.first,
					            static_cast<unsigned long>(key.second),
					            static_cast<unsigned long long>(count));
				}
				g_site_counts.clear();
			}
		}
		std::printf("%s\n%s\n", line.c_str(), layouts.c_str());
		std::fflush(stdout);
	}
}

// Replaces a dispatcher entry with one that counts calls of the given kind.
#define KYTY_COUNT_COMMAND(name, kind)                                                             \
	do {                                                                                           \
		static PFN_##name original = nullptr;                                                      \
		if (dispatcher.name != nullptr) {                                                          \
			original        = dispatcher.name;                                                     \
			dispatcher.name = [](auto... args) {                                                   \
				Count(kind);                                                                       \
				original(args...);                                                                 \
			};                                                                                     \
		}                                                                                          \
	} while (false)

// Like KYTY_COUNT_COMMAND, also counting draws into the open render pass, or timing dispatches.
#define KYTY_COUNT_DRAW(name, kind)                                                                \
	do {                                                                                           \
		static PFN_##name original = nullptr;                                                      \
		if (dispatcher.name != nullptr) {                                                          \
			original        = dispatcher.name;                                                     \
			dispatcher.name = [](VkCommandBuffer buffer, auto... args) {                           \
				Count(kind);                                                                       \
				PassDraw(buffer);                                                                  \
				original(buffer, args...);                                                         \
			};                                                                                     \
		}                                                                                          \
	} while (false)
#define KYTY_COUNT_DISPATCH(name, kind)                                                            \
	do {                                                                                           \
		static PFN_##name original = nullptr;                                                      \
		if (dispatcher.name != nullptr) {                                                          \
			original        = dispatcher.name;                                                     \
			dispatcher.name = [](VkCommandBuffer buffer, auto... args) {                           \
				Count(kind);                                                                       \
				const auto segment = DispatchBegin(buffer);                                        \
				original(buffer, args...);                                                         \
				DispatchEnd(buffer, segment);                                                      \
			};                                                                                     \
		}                                                                                          \
	} while (false)

// Like KYTY_COUNT_COMMAND, also timing the command as a transfer or clear segment.
#define KYTY_TIME_COMMAND(name, kind, segment_kind, command)                                      \
	do {                                                                                           \
		static PFN_##name original = nullptr;                                                      \
		if (dispatcher.name != nullptr) {                                                          \
			original        = dispatcher.name;                                                     \
			dispatcher.name = [](VkCommandBuffer buffer, auto... args) {                           \
				Count(kind);                                                                       \
				const auto segment = DispatchBegin(buffer, segment_kind, command);                 \
				original(buffer, args...);                                                         \
				DispatchEnd(buffer, segment);                                                      \
			};                                                                                     \
		}                                                                                          \
	} while (false)

// GPU timer state. Each command scheduler (rendering, presentation) records on its own thread.
struct GpuTimer {
	std::mutex                            mutex;
	std::unordered_map<VkCommandBuffer, uint32_t> active; // recording buffer -> its query pair
	bool                                  initialized = false;
	bool                                  enabled     = false;
	vk::Device                            device;
	vk::QueryPool                         pool;
	double                                period = 1.0;
	uint32_t                              next = 0;
	std::array<bool, 512>                 pending {};
	uint64_t                              busy_ns = 0, measured = 0;
	std::chrono::steady_clock::time_point last;
};
GpuTimer g_timer;

// The last pipeline bound per recording buffer and bind point (bind_pipeline_repeat).
std::mutex                                                   g_bind_mutex;
std::map<std::pair<VkCommandBuffer, int>, VkPipeline> g_last_bind;

// KYTY_GPU_TIME=passes: timestamps around each render pass and dispatch, in a chunk of queries
// per scheduler command buffer, to split GPU time by kind and find the costliest passes.
struct PassTimer {
	static constexpr uint32_t ChunkQueries = 16384;
	static constexpr uint32_t Chunks       = 64;
	struct Segment {
		uint32_t begin = 0, end = 0; // query indices
		bool     pass  = false;
		uint32_t draws = 0, width = 0, height = 0, colors = 0;
		uint8_t  kind     = 0; // SegmentKind
		uint8_t  command  = 0; // transfers and clears: index into TransferNames
		uint64_t module   = 0; // dispatches: compute module id (KYTY_GPU_TIME=draws)
		uint32_t barriers = 0; // barriers recorded since the previous segment ended
	};
	enum SegmentKind : uint8_t { Pass, Dispatch, Transfer, Clear };
	struct Chunk {
		bool                 owned   = false; // a recording buffer writes it
		bool                 pending = false;
		uint32_t             used    = 0;
		uint32_t             start = UINT32_MAX, finish = UINT32_MAX; // command buffer bounds
		uint32_t             barriers = 0; // since the last segment ended
		std::vector<Segment> segments;
		struct Draw {
			int32_t  segment = -1;
			uint32_t query   = UINT32_MAX;
			uint64_t vs = 0, ps = 0, vertices = 0;
			bool     changed = false;
		};
		std::vector<Draw> draws;
	};
	struct DrawTotal {
		double   ns = 0;
		uint64_t n = 0, vertices = 0, changed = 0;
		double   changed_ns = 0;
	};
	bool                                          enabled = false;
	bool                                          draws_enabled = false;
	std::unordered_map<uint64_t, DrawTotal>       by_vs, by_ps, by_cs;
	std::unordered_map<VkCommandBuffer, uint64_t> next_dispatch_module;
	DrawTotal                                     draw_total;
	vk::QueryPool                                 pool;
	std::array<Chunk, Chunks>                     chunks;
	uint32_t                                      next = 0;
	std::unordered_map<VkCommandBuffer, uint32_t> active; // buffer -> chunk
	std::unordered_map<VkCommandBuffer, int32_t>  open_pass; // buffer -> segment index
	double pass_ns = 0, dispatch_ns = 0;
	// Outside passes and dispatches: transfers, clears, and the gaps between segments, split by
	// whether barriers were recorded in the gap; lead and tail are the buffer's start and end.
	double   transfer_ns = 0, clear_ns = 0, gap_barrier_ns = 0, gap_plain_ns = 0, lead_ns = 0,
	         tail_ns = 0;
	uint64_t transfers = 0, clears = 0, gaps_barrier = 0, gaps_plain = 0;
	std::array<std::pair<double, uint64_t>, 9> by_command {}; // ns, n
	uint64_t passes = 0, dispatches = 0, pass_draws = 0;
	std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::pair<double, uint64_t>> by_shape; // wxh,colors -> ns, n
};
PassTimer g_passes;

// Writes a timestamp into the buffer's chunk; UINT32_MAX when the buffer has none or it is full.
uint32_t PassStamp(VkCommandBuffer buffer) {
	const auto found = g_passes.active.find(buffer);
	if (found == g_passes.active.end()) return UINT32_MAX;
	auto& chunk = g_passes.chunks[found->second];
	if (chunk.used == PassTimer::ChunkQueries) return UINT32_MAX;
	const auto query = found->second * PassTimer::ChunkQueries + chunk.used++;
	vk::CommandBuffer(buffer).writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, g_passes.pool,
	                                         query);
	return query;
}

void PassCollect(uint32_t index) {
	auto& chunk = g_passes.chunks[index];
	if (!chunk.pending) return;
	chunk.pending = false;
	if (chunk.used != 0) {
		std::vector<uint64_t> stamps(chunk.used);
		if (g_timer.device.getQueryPoolResults(g_passes.pool, index * PassTimer::ChunkQueries,
		                                       chunk.used, stamps.size() * sizeof(uint64_t),
		                                       stamps.data(), sizeof(uint64_t),
		                                       vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
			const auto base = index * PassTimer::ChunkQueries;
			// Each draw gets the time from the previous stamp in its pass (or the pass begin).
			int32_t  segment_index = -1;
			uint64_t previous      = 0;
			for (const auto& draw: chunk.draws) {
				if (draw.segment < 0 || draw.query == UINT32_MAX) continue;
				const auto& segment = chunk.segments[draw.segment];
				if (segment.begin == UINT32_MAX) continue;
				if (draw.segment != segment_index) {
					segment_index = draw.segment;
					previous      = stamps[segment.begin - base];
				}
				const auto stamp = stamps[draw.query - base];
				if (stamp < previous) continue;
				const double ns = static_cast<double>(stamp - previous) * g_timer.period;
				previous        = stamp;
				for (auto* total: {&g_passes.by_vs[draw.vs], &g_passes.by_ps[draw.ps], &g_passes.draw_total}) {
					total->ns += ns;
					total->n++;
					total->vertices += draw.vertices;
					if (draw.changed) {
						total->changed++;
						total->changed_ns += ns;
					}
				}
			}
			const auto at = [&](uint32_t query) { return stamps[query - base]; };
			bool     bounded  = chunk.start != UINT32_MAX && chunk.finish != UINT32_MAX;
			uint64_t last_end = bounded ? at(chunk.start) : 0;
			bool     first    = true;
			for (const auto& segment: chunk.segments) {
				if (segment.end == UINT32_MAX || segment.begin == UINT32_MAX) {
					bounded = false;
					continue;
				}
				const auto b = at(segment.begin), e = at(segment.end);
				if (e < b) continue;
				if (bounded && b >= last_end) {
					const double gap = static_cast<double>(b - last_end) * g_timer.period;
					if (first) {
						g_passes.lead_ns += gap;
					} else if (segment.barriers != 0) {
						g_passes.gap_barrier_ns += gap;
						g_passes.gaps_barrier++;
					} else {
						g_passes.gap_plain_ns += gap;
						g_passes.gaps_plain++;
					}
				}
				first    = false;
				last_end = std::max(last_end, e);
				const double ns = static_cast<double>(e - b) * g_timer.period;
				if (segment.kind == PassTimer::Transfer || segment.kind == PassTimer::Clear) {
					g_passes.by_command[segment.command].first += ns;
					g_passes.by_command[segment.command].second++;
				}
				if (segment.kind == PassTimer::Transfer) {
					g_passes.transfer_ns += ns;
					g_passes.transfers++;
					continue;
				}
				if (segment.kind == PassTimer::Clear) {
					g_passes.clear_ns += ns;
					g_passes.clears++;
					continue;
				}
				if (segment.pass) {
					g_passes.pass_ns += ns;
					g_passes.passes++;
					g_passes.pass_draws += segment.draws;
					auto& shape = g_passes.by_shape[{segment.width, segment.height, segment.colors}];
					shape.first += ns;
					shape.second++;
				} else {
					g_passes.dispatch_ns += ns;
					g_passes.dispatches++;
					if (g_passes.draws_enabled) {
						auto& total = g_passes.by_cs[segment.module];
						total.ns += ns;
						total.n++;
					}
				}
			}
			if (bounded && at(chunk.finish) >= last_end) {
				g_passes.tail_ns += static_cast<double>(at(chunk.finish) - last_end) * g_timer.period;
			}
		}
	}
	chunk.used = 0;
	chunk.start = chunk.finish = UINT32_MAX;
	chunk.barriers = 0;
	chunk.segments.clear();
	chunk.draws.clear();
}

std::string fmt_command(const char* name, double ms, double n) {
	char buffer[96];
	std::snprintf(buffer, sizeof(buffer), " %s=%.2fms/%.0f", name, ms, n);
	return buffer;
}

void PrintDrawTotals(const char* what, std::unordered_map<uint64_t, PassTimer::DrawTotal>& totals,
                     double s, size_t limit) {
	std::vector<std::pair<uint64_t, PassTimer::DrawTotal>> top(totals.begin(), totals.end());
	std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
	for (size_t i = 0; i < top.size() && i < limit; i++) {
		const auto& [id, t] = top[i];
		std::printf("GPUDRAW %s=%016llx ms_s=%.3f draws_s=%.0f us=%.2f verts=%.0f ns_vert=%.2f "
		            "switch=%.0f%% switch_us=%.2f\n",
		            what, static_cast<unsigned long long>(id), t.ns / 1e6 / s, t.n / s,
		            t.ns / 1e3 / static_cast<double>(t.n),
		            static_cast<double>(t.vertices) / static_cast<double>(t.n),
		            t.vertices != 0 ? t.ns / static_cast<double>(t.vertices) : 0.0,
		            100.0 * static_cast<double>(t.changed) / static_cast<double>(t.n),
		            t.changed != 0 ? t.changed_ns / 1e3 / static_cast<double>(t.changed) : 0.0);
	}
	totals.clear();
}

// Adds a finished command buffer's GPU time; skips one whose results are not ready, since it
// may wait on work this thread has yet to record.
void Collect(uint32_t slot) {
	auto& timer = g_timer;
	if (!timer.pending[slot]) {
		return;
	}
	timer.pending[slot] = false;
	std::array<uint64_t, 2> stamps {};
	if (timer.device.getQueryPoolResults(timer.pool, slot * 2, 2, sizeof(stamps), stamps.data(),
	                                     sizeof(uint64_t), vk::QueryResultFlagBits::e64) ==
	        vk::Result::eSuccess &&
	    stamps[1] >= stamps[0]) {
		timer.busy_ns += static_cast<uint64_t>(static_cast<double>(stamps[1] - stamps[0]) * timer.period);
		timer.measured++;
	}
	const auto now     = std::chrono::steady_clock::now();
	const auto elapsed = std::chrono::duration<double, std::milli>(now - timer.last).count();
	if (elapsed >= 1000.0) {
		std::printf("GPUTIME busy_ms_s=%.1f measured_submits_s=%.0f\n",
		            static_cast<double>(timer.busy_ns) / 1e6 * 1000.0 / elapsed,
		            static_cast<double>(timer.measured) * 1000.0 / elapsed);
		if (g_passes.enabled) {
			const double s = elapsed / 1000.0;
			std::printf("GPUPASS pass_ms_s=%.1f passes_s=%.0f draws_s=%.0f dispatch_ms_s=%.1f dispatches_s=%.0f\n",
			            g_passes.pass_ns / 1e6 / s, g_passes.passes / s, g_passes.pass_draws / s,
			            g_passes.dispatch_ns / 1e6 / s, g_passes.dispatches / s);
			std::vector<std::pair<double, std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>>> top;
			for (const auto& [shape, value]: g_passes.by_shape) {
				top.push_back({value.first, {std::get<0>(shape), std::get<1>(shape), std::get<2>(shape), value.second}});
			}
			std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (size_t i = 0; i < top.size() && i < 8; i++) {
				const auto& [w, h, c, n] = top[i].second;
				std::printf("GPUPASS shape=%ux%u colors=%u ms_s=%.2f passes_s=%.0f\n", w, h, c,
				            top[i].first / 1e6 / s, n / s);
			}
			if (g_passes.draws_enabled) {
				const auto& t = g_passes.draw_total;
				std::printf("GPUDRAW total ms_s=%.2f draws_s=%.0f verts_s=%.0f switch_ms_s=%.2f "
				            "switches_s=%.0f vs_modules=%zu ps_modules=%zu\n",
				            t.ns / 1e6 / s, t.n / s, static_cast<double>(t.vertices) / s,
				            t.changed_ns / 1e6 / s, t.changed / s, g_passes.by_vs.size(),
				            g_passes.by_ps.size());
				g_passes.draw_total = {};
				PrintDrawTotals("vs", g_passes.by_vs, s, 25);
				PrintDrawTotals("ps", g_passes.by_ps, s, 15);
				PrintDrawTotals("cs", g_passes.by_cs, s, 15);
			}
			std::printf("GPUGAP transfer_ms_s=%.1f transfers_s=%.0f clear_ms_s=%.1f clears_s=%.0f "
			            "gap_barrier_ms_s=%.1f gaps_barrier_s=%.0f gap_plain_ms_s=%.1f gaps_plain_s=%.0f "
			            "lead_ms_s=%.1f tail_ms_s=%.1f\n",
			            g_passes.transfer_ns / 1e6 / s, g_passes.transfers / s, g_passes.clear_ns / 1e6 / s,
			            g_passes.clears / s, g_passes.gap_barrier_ns / 1e6 / s, g_passes.gaps_barrier / s,
			            g_passes.gap_plain_ns / 1e6 / s, g_passes.gaps_plain / s, g_passes.lead_ns / 1e6 / s,
			            g_passes.tail_ns / 1e6 / s);
			{
				static constexpr std::array<const char*, 9> names = {
				    "copy_buffer", "fill_buffer", "update_buffer", "copy_image", "buffer_to_image",
				    "image_to_buffer", "blit_image", "clear_color", "clear_depth"};
				std::string line = "GPUXFER";
				for (size_t i = 0; i < names.size(); i++) {
					const auto& [ns, n] = g_passes.by_command[i];
					line += fmt_command(names[i], ns / 1e6 / s, static_cast<double>(n) / s);
				}
				std::printf("%s\n", line.c_str());
				g_passes.by_command = {};
			}
			g_passes.transfer_ns = g_passes.clear_ns = g_passes.gap_barrier_ns = g_passes.gap_plain_ns = 0;
			g_passes.lead_ns = g_passes.tail_ns = 0;
			g_passes.transfers = g_passes.clears = g_passes.gaps_barrier = g_passes.gaps_plain = 0;
			g_passes.pass_ns = g_passes.dispatch_ns = 0;
			g_passes.passes = g_passes.dispatches = g_passes.pass_draws = 0;
			g_passes.by_shape.clear();
		}
		std::fflush(stdout);
		timer.busy_ns  = 0;
		timer.measured = 0;
		timer.last     = now;
	}
}

void PassBegin(VkCommandBuffer buffer, const VkRenderingInfo* info) {
	std::lock_guard lock(g_timer.mutex);
	if (!g_passes.enabled) return;
	const auto found = g_passes.active.find(buffer);
	if (found == g_passes.active.end()) return;
	auto& chunk = g_passes.chunks[found->second];
	PassTimer::Segment segment;
	segment.begin  = PassStamp(buffer);
	segment.end    = UINT32_MAX;
	segment.pass   = true;
	segment.barriers = chunk.barriers;
	chunk.barriers   = 0;
	segment.width  = info->renderArea.extent.width;
	segment.height = info->renderArea.extent.height;
	segment.colors = info->colorAttachmentCount;
	g_passes.open_pass[buffer] = static_cast<int32_t>(chunk.segments.size());
	chunk.segments.push_back(segment);
}

void PassEnd(VkCommandBuffer buffer) {
	std::lock_guard lock(g_timer.mutex);
	if (!g_passes.enabled) return;
	const auto found = g_passes.active.find(buffer);
	const auto open  = g_passes.open_pass.find(buffer);
	if (found == g_passes.active.end() || open == g_passes.open_pass.end()) return;
	g_passes.chunks[found->second].segments[open->second].end = PassStamp(buffer);
	g_passes.open_pass.erase(open);
}

void PassDraw(VkCommandBuffer buffer) {
	if (!g_passes.enabled) return;
	std::lock_guard lock(g_timer.mutex);
	const auto found = g_passes.active.find(buffer);
	const auto open  = g_passes.open_pass.find(buffer);
	if (found == g_passes.active.end() || open == g_passes.open_pass.end()) return;
	g_passes.chunks[found->second].segments[open->second].draws++;
}

void PassDrawStamp(VkCommandBuffer buffer, uint64_t vs, uint64_t ps, uint64_t vertices, bool changed) {
	std::lock_guard lock(g_timer.mutex);
	const auto found = g_passes.active.find(buffer);
	const auto open  = g_passes.open_pass.find(buffer);
	if (found == g_passes.active.end() || open == g_passes.open_pass.end()) return;
	auto& chunk = g_passes.chunks[found->second];
	chunk.draws.push_back({open->second, PassStamp(buffer), vs, ps, vertices, changed});
}

void PassBarrier(VkCommandBuffer buffer) {
	if (!g_passes.enabled) return;
	std::lock_guard lock(g_timer.mutex);
	if (const auto found = g_passes.active.find(buffer); found != g_passes.active.end()) {
		g_passes.chunks[found->second].barriers++;
	}
}

// Returns the dispatch's segment index, or -1.
int32_t DispatchBegin(VkCommandBuffer buffer, uint8_t kind = PassTimer::Dispatch,
                      uint8_t command = 0) {
	if (!g_passes.enabled) return -1;
	std::lock_guard lock(g_timer.mutex);
	const auto found = g_passes.active.find(buffer);
	if (found == g_passes.active.end()) return -1;
	auto&              chunk = g_passes.chunks[found->second];
	PassTimer::Segment segment;
	segment.kind     = kind;
	segment.command  = command;
	if (kind == PassTimer::Dispatch) {
		if (const auto tag = g_passes.next_dispatch_module.find(buffer);
		    tag != g_passes.next_dispatch_module.end()) {
			segment.module = tag->second;
			g_passes.next_dispatch_module.erase(tag);
		}
	}
	segment.barriers = chunk.barriers;
	chunk.barriers   = 0;
	segment.begin = PassStamp(buffer);
	segment.end   = UINT32_MAX;
	chunk.segments.push_back(segment);
	return static_cast<int32_t>(chunk.segments.size() - 1);
}

void DispatchEnd(VkCommandBuffer buffer, int32_t index) {
	if (index < 0) return;
	std::lock_guard lock(g_timer.mutex);
	const auto found = g_passes.active.find(buffer);
	if (found == g_passes.active.end()) return;
	g_passes.chunks[found->second].segments[index].end = PassStamp(buffer);
}

} // namespace

void InstallCommandCounters() {
	const char* mode = std::getenv("KYTY_VK_COUNT");
	const char* gpu_time = std::getenv("KYTY_GPU_TIME");
	if (mode == nullptr && (gpu_time == nullptr || (std::strcmp(gpu_time, "passes") != 0 &&
	                                                 std::strcmp(gpu_time, "draws") != 0))) {
		return;
	}
	g_sites          = mode != nullptr && std::strcmp(mode, "sites") == 0;
	auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
	if (dispatcher.vkCmdPipelineBarrier != nullptr) {
		static PFN_vkCmdPipelineBarrier original = dispatcher.vkCmdPipelineBarrier;
		dispatcher.vkCmdPipelineBarrier          = [](auto... args) {
            Count(Barrier);
            Site(Barrier);
            PassBarrier(std::get<0>(std::tuple {args...}));
            original(args...);
		};
	}
	if (dispatcher.vkCmdPipelineBarrier2 != nullptr) {
		static PFN_vkCmdPipelineBarrier2 original = dispatcher.vkCmdPipelineBarrier2;
		dispatcher.vkCmdPipelineBarrier2 = [](VkCommandBuffer buffer, const VkDependencyInfo* info) {
			Count(Barrier2);
			Site(Barrier2);
			PassBarrier(buffer);
			{
				std::lock_guard lock(g_mutex);
				for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++) {
					const auto& barrier = info->pImageMemoryBarriers[i];
					g_layouts[{static_cast<int>(barrier.oldLayout),
					           static_cast<int>(barrier.newLayout)}]++;
				}
			}
			original(buffer, info);
		};
	}
	if (dispatcher.vkCmdBeginRendering != nullptr) {
		static PFN_vkCmdBeginRendering original = dispatcher.vkCmdBeginRendering;
		dispatcher.vkCmdBeginRendering          = [](VkCommandBuffer buffer, const VkRenderingInfo* info) {
            Count(BeginRendering);
            Site(BeginRendering);
            PassBegin(buffer, info);
            original(buffer, info);
		};
	}
	if (dispatcher.vkCmdEndRendering != nullptr) {
		static PFN_vkCmdEndRendering original = dispatcher.vkCmdEndRendering;
		dispatcher.vkCmdEndRendering          = [](VkCommandBuffer buffer) {
			Count(EndRendering);
			Site(EndRendering);
			original(buffer);
			PassEnd(buffer);
		};
	}
	KYTY_COUNT_DRAW(vkCmdDraw, Draw);
	KYTY_COUNT_DRAW(vkCmdDrawIndexed, DrawIndexed);
	KYTY_COUNT_DRAW(vkCmdDrawIndirect, DrawIndirect);
	KYTY_COUNT_DRAW(vkCmdDrawIndexedIndirect, DrawIndirect);
	KYTY_COUNT_DRAW(vkCmdDrawMeshTasksEXT, DrawMesh);
	KYTY_COUNT_DISPATCH(vkCmdDispatch, Dispatch);
	KYTY_COUNT_DISPATCH(vkCmdDispatchBase, Dispatch);
	KYTY_COUNT_DISPATCH(vkCmdDispatchIndirect, DispatchIndirect);
	if (dispatcher.vkCmdBindPipeline != nullptr) {
		static PFN_vkCmdBindPipeline original = dispatcher.vkCmdBindPipeline;
		dispatcher.vkCmdBindPipeline          = [](VkCommandBuffer buffer, VkPipelineBindPoint point,
                                              VkPipeline pipeline) {
			Count(BindPipeline);
			{
				// Same pipeline as the last bind on this buffer and bind point.
				std::lock_guard lock(g_bind_mutex);
				auto& previous = g_last_bind[{buffer, static_cast<int>(point)}];
				if (previous == pipeline) Count(BindPipelineRepeat);
				previous = pipeline;
			}
			original(buffer, point, pipeline);
		};
	}
	if (dispatcher.vkCmdPushDescriptorSetKHR != nullptr) {
		static PFN_vkCmdPushDescriptorSetKHR original = dispatcher.vkCmdPushDescriptorSetKHR;
		dispatcher.vkCmdPushDescriptorSetKHR          = [](VkCommandBuffer buffer, VkPipelineBindPoint point,
                                                      VkPipelineLayout layout, uint32_t set, uint32_t count,
                                                      const VkWriteDescriptorSet* writes) {
			Count(PushDescriptor);
			uint64_t elements = 0;
			for (uint32_t i = 0; i < count; i++) {
				elements += writes[i].descriptorCount;
			}
			g_counts[PushDescriptorElements].fetch_add(elements, std::memory_order_relaxed);
			original(buffer, point, layout, set, count, writes);
		};
	}
	KYTY_COUNT_COMMAND(vkCmdPushConstants, PushConstants);
	KYTY_TIME_COMMAND(vkCmdCopyBuffer, CopyBuffer, PassTimer::Transfer, 0);
	KYTY_TIME_COMMAND(vkCmdFillBuffer, CopyBuffer, PassTimer::Transfer, 1);
	KYTY_TIME_COMMAND(vkCmdUpdateBuffer, CopyBuffer, PassTimer::Transfer, 2);
	KYTY_TIME_COMMAND(vkCmdCopyImage, CopyImage, PassTimer::Transfer, 3);
	KYTY_TIME_COMMAND(vkCmdCopyBufferToImage, CopyImage, PassTimer::Transfer, 4);
	KYTY_TIME_COMMAND(vkCmdCopyImageToBuffer, CopyImage, PassTimer::Transfer, 5);
	KYTY_TIME_COMMAND(vkCmdBlitImage, CopyImage, PassTimer::Transfer, 6);
	KYTY_TIME_COMMAND(vkCmdClearColorImage, Clear, PassTimer::Clear, 7);
	KYTY_TIME_COMMAND(vkCmdClearDepthStencilImage, Clear, PassTimer::Clear, 8);
	KYTY_COUNT_COMMAND(vkCmdClearAttachments, Clear);
	KYTY_COUNT_COMMAND(vkCmdBeginQuery, BeginQuery);
	if (mode != nullptr) std::thread(Report).detach();
}

#undef KYTY_COUNT_COMMAND
#undef KYTY_COUNT_DRAW
#undef KYTY_COUNT_DISPATCH
#undef KYTY_TIME_COMMAND

void BeginCommandBuffer(vk::Device device, float timestamp_period, vk::CommandBuffer buffer) {
	{
		std::lock_guard bind_lock(g_bind_mutex);
		g_last_bind.erase({buffer, static_cast<int>(VK_PIPELINE_BIND_POINT_GRAPHICS)});
		g_last_bind.erase({buffer, static_cast<int>(VK_PIPELINE_BIND_POINT_COMPUTE)});
	}
	auto&           timer = g_timer;
	std::lock_guard lock(timer.mutex);
	if (!timer.initialized) {
		timer.initialized = true;
		if (std::getenv("KYTY_GPU_TIME") != nullptr) {
			vk::QueryPoolCreateInfo info {};
			info.queryType  = vk::QueryType::eTimestamp;
			info.queryCount = static_cast<uint32_t>(timer.pending.size() * 2);
			timer.enabled   = device.createQueryPool(&info, nullptr, &timer.pool) == vk::Result::eSuccess;
			if (timer.enabled && (std::strcmp(std::getenv("KYTY_GPU_TIME"), "passes") == 0 ||
			                      DrawTimingEnabled())) {
				g_passes.draws_enabled = DrawTimingEnabled();
				info.queryCount  = PassTimer::ChunkQueries * PassTimer::Chunks;
				g_passes.enabled = device.createQueryPool(&info, nullptr, &g_passes.pool) == vk::Result::eSuccess;
			}
			timer.device    = device;
			timer.period    = timestamp_period;
			timer.last      = std::chrono::steady_clock::now();
		}
	}
	if (!timer.enabled) {
		return;
	}
	if (g_passes.enabled) {
		// The next chunk no recording buffer owns; none free: this buffer goes untimed.
		for (uint32_t tried = 0; tried < PassTimer::Chunks; tried++) {
			const auto chunk = g_passes.next;
			g_passes.next    = (g_passes.next + 1) % PassTimer::Chunks;
			if (g_passes.chunks[chunk].owned) continue;
			PassCollect(chunk);
			buffer.resetQueryPool(g_passes.pool, chunk * PassTimer::ChunkQueries,
			                      PassTimer::ChunkQueries);
			g_passes.chunks[chunk].owned = true;
			g_passes.active[buffer]      = chunk;
			g_passes.chunks[chunk].start = PassStamp(buffer);
			break;
		}
	}
	const auto slot = timer.next;
	timer.next      = (timer.next + 1) % static_cast<uint32_t>(timer.pending.size());
	Collect(slot);
	timer.active[buffer] = slot;
	buffer.resetQueryPool(timer.pool, slot * 2, 2);
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, timer.pool, slot * 2);
}

bool DrawTimingEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_TIME");
		return value != nullptr && std::strcmp(value, "draws") == 0;
	}();
	return enabled;
}

void DrawStamp(vk::CommandBuffer buffer, uint64_t vs_id, uint64_t ps_id, uint64_t vertices,
               bool pipeline_changed) {
	if (!g_passes.draws_enabled) return;
	PassDrawStamp(buffer, vs_id, ps_id, vertices, pipeline_changed);
}

void TagDispatch(vk::CommandBuffer buffer, uint64_t module_id) {
	if (!g_passes.draws_enabled) return;
	std::lock_guard lock(g_timer.mutex);
	g_passes.next_dispatch_module[buffer] = module_id;
}

void DumpSpirv(const char* stage, uint64_t hash, uint64_t id, std::span<const uint32_t> spirv) {
	static const char* dir = std::getenv("KYTY_SPV_DUMP");
	if (dir == nullptr) return;
	char name[512];
	std::snprintf(name, sizeof(name), "%s/%s_%016llx_%016llx.spv", dir, stage,
	              static_cast<unsigned long long>(hash), static_cast<unsigned long long>(id));
	if (FILE* file = std::fopen(name, "wb"); file != nullptr) {
		std::fwrite(spirv.data(), sizeof(uint32_t), spirv.size(), file);
		std::fclose(file);
	}
}

void EndCommandBuffer(vk::CommandBuffer buffer) {
	auto&           timer = g_timer;
	std::lock_guard lock(timer.mutex);
	const auto      found = timer.active.find(buffer);
	if (!timer.enabled || found == timer.active.end()) {
		return;
	}
	const auto slot = found->second;
	timer.active.erase(found);
	if (const auto pass = g_passes.active.find(buffer); pass != g_passes.active.end()) {
		// Still inside a render pass (End() closes it after this hook): stamp its end here.
		if (const auto open = g_passes.open_pass.find(buffer); open != g_passes.open_pass.end()) {
			g_passes.chunks[pass->second].segments[open->second].end = PassStamp(buffer);
			g_passes.open_pass.erase(open);
		}
		g_passes.chunks[pass->second].finish  = PassStamp(buffer);
		g_passes.chunks[pass->second].pending = true;
		g_passes.chunks[pass->second].owned   = false;
		g_passes.active.erase(pass);
	}
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timer.pool, slot * 2 + 1);
	timer.pending[slot] = true;
}

} // namespace Libs::Graphics::VulkanDiagnostics
