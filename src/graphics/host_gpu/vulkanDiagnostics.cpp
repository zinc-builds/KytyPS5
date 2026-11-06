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
    "push_descriptor", "push_constants", "copy_buffer", "copy_image",     "clear", "begin_query"};

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
	static constexpr uint32_t ChunkQueries = 4096;
	static constexpr uint32_t Chunks       = 64;
	struct Segment {
		uint32_t begin = 0, end = 0; // query indices
		bool     pass  = false;
		uint32_t draws = 0, width = 0, height = 0, colors = 0;
	};
	struct Chunk {
		bool                 owned   = false; // a recording buffer writes it
		bool                 pending = false;
		uint32_t             used    = 0;
		std::vector<Segment> segments;
	};
	bool                                          enabled = false;
	vk::QueryPool                                 pool;
	std::array<Chunk, Chunks>                     chunks;
	uint32_t                                      next = 0;
	std::unordered_map<VkCommandBuffer, uint32_t> active; // buffer -> chunk
	std::unordered_map<VkCommandBuffer, int32_t>  open_pass; // buffer -> segment index
	double pass_ns = 0, dispatch_ns = 0;
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
			for (const auto& segment: chunk.segments) {
				if (segment.end == UINT32_MAX || segment.begin == UINT32_MAX) continue;
				const auto b = stamps[segment.begin - base], e = stamps[segment.end - base];
				if (e < b) continue;
				const double ns = static_cast<double>(e - b) * g_timer.period;
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
				}
			}
		}
	}
	chunk.used = 0;
	chunk.segments.clear();
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

// Returns the dispatch's segment index, or -1.
int32_t DispatchBegin(VkCommandBuffer buffer) {
	if (!g_passes.enabled) return -1;
	std::lock_guard lock(g_timer.mutex);
	const auto found = g_passes.active.find(buffer);
	if (found == g_passes.active.end()) return -1;
	auto&              chunk = g_passes.chunks[found->second];
	PassTimer::Segment segment;
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
	if (mode == nullptr && (gpu_time == nullptr || std::strcmp(gpu_time, "passes") != 0)) {
		return;
	}
	g_sites          = mode != nullptr && std::strcmp(mode, "sites") == 0;
	auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
	if (dispatcher.vkCmdPipelineBarrier != nullptr) {
		static PFN_vkCmdPipelineBarrier original = dispatcher.vkCmdPipelineBarrier;
		dispatcher.vkCmdPipelineBarrier          = [](auto... args) {
            Count(Barrier);
            Site(Barrier);
            original(args...);
		};
	}
	if (dispatcher.vkCmdPipelineBarrier2 != nullptr) {
		static PFN_vkCmdPipelineBarrier2 original = dispatcher.vkCmdPipelineBarrier2;
		dispatcher.vkCmdPipelineBarrier2 = [](VkCommandBuffer buffer, const VkDependencyInfo* info) {
			Count(Barrier2);
			Site(Barrier2);
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
	KYTY_COUNT_COMMAND(vkCmdPushDescriptorSetKHR, PushDescriptor);
	KYTY_COUNT_COMMAND(vkCmdPushConstants, PushConstants);
	KYTY_COUNT_COMMAND(vkCmdCopyBuffer, CopyBuffer);
	KYTY_COUNT_COMMAND(vkCmdFillBuffer, CopyBuffer);
	KYTY_COUNT_COMMAND(vkCmdUpdateBuffer, CopyBuffer);
	KYTY_COUNT_COMMAND(vkCmdCopyImage, CopyImage);
	KYTY_COUNT_COMMAND(vkCmdCopyBufferToImage, CopyImage);
	KYTY_COUNT_COMMAND(vkCmdCopyImageToBuffer, CopyImage);
	KYTY_COUNT_COMMAND(vkCmdBlitImage, CopyImage);
	KYTY_COUNT_COMMAND(vkCmdClearColorImage, Clear);
	KYTY_COUNT_COMMAND(vkCmdClearDepthStencilImage, Clear);
	KYTY_COUNT_COMMAND(vkCmdClearAttachments, Clear);
	KYTY_COUNT_COMMAND(vkCmdBeginQuery, BeginQuery);
	if (mode != nullptr) std::thread(Report).detach();
}

#undef KYTY_COUNT_COMMAND
#undef KYTY_COUNT_DRAW
#undef KYTY_COUNT_DISPATCH

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
			if (timer.enabled && std::strcmp(std::getenv("KYTY_GPU_TIME"), "passes") == 0) {
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
		g_passes.chunks[pass->second].pending = true;
		g_passes.chunks[pass->second].owned   = false;
		g_passes.active.erase(pass);
	}
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timer.pool, slot * 2 + 1);
	timer.pending[slot] = true;
}

} // namespace Libs::Graphics::VulkanDiagnostics
