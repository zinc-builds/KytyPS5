#include "graphics/host_gpu/vulkanDiagnostics.h"

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
#include <unordered_map>
#include <utility>

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
	Draw,
	DrawIndexed,
	DrawIndirect,
	DrawMesh,
	Dispatch,
	DispatchIndirect,
	BindPipeline,
	PushDescriptor,
	PushConstants,
	CopyBuffer,
	CopyImage,
	Clear,
	KindCount
};

constexpr std::array<const char*, KindCount> KindNames = {
    "barrier",  "barrier2",          "begin_rendering", "draw",           "draw_indexed",
    "draw_indirect", "draw_mesh",    "dispatch",        "dispatch_indirect", "bind_pipeline",
    "push_descriptor", "push_constants", "copy_buffer", "copy_image",     "clear"};

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
	std::array<void*, 4> frames {};
	if (backtrace(frames.data(), static_cast<int>(frames.size())) < 4) {
		return;
	}
	const auto base = reinterpret_cast<uintptr_t>(&__ehdr_start);
	std::lock_guard lock(g_mutex);
	g_site_counts[{kind, reinterpret_cast<uintptr_t>(frames[2]) - base}]++;
	g_site_counts[{kind + 100, reinterpret_cast<uintptr_t>(frames[3]) - base}]++;
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
		std::fflush(stdout);
		timer.busy_ns  = 0;
		timer.measured = 0;
		timer.last     = now;
	}
}

} // namespace

void InstallCommandCounters() {
	const char* mode = std::getenv("KYTY_VK_COUNT");
	if (mode == nullptr) {
		return;
	}
	g_sites          = std::strcmp(mode, "sites") == 0;
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
		dispatcher.vkCmdBeginRendering          = [](auto... args) {
            Count(BeginRendering);
            Site(BeginRendering);
            original(args...);
		};
	}
	KYTY_COUNT_COMMAND(vkCmdDraw, Draw);
	KYTY_COUNT_COMMAND(vkCmdDrawIndexed, DrawIndexed);
	KYTY_COUNT_COMMAND(vkCmdDrawIndirect, DrawIndirect);
	KYTY_COUNT_COMMAND(vkCmdDrawIndexedIndirect, DrawIndirect);
	KYTY_COUNT_COMMAND(vkCmdDrawMeshTasksEXT, DrawMesh);
	KYTY_COUNT_COMMAND(vkCmdDispatch, Dispatch);
	KYTY_COUNT_COMMAND(vkCmdDispatchBase, Dispatch);
	KYTY_COUNT_COMMAND(vkCmdDispatchIndirect, DispatchIndirect);
	KYTY_COUNT_COMMAND(vkCmdBindPipeline, BindPipeline);
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
	std::thread(Report).detach();
}

#undef KYTY_COUNT_COMMAND

void BeginCommandBuffer(vk::Device device, float timestamp_period, vk::CommandBuffer buffer) {
	auto&           timer = g_timer;
	std::lock_guard lock(timer.mutex);
	if (!timer.initialized) {
		timer.initialized = true;
		if (std::getenv("KYTY_GPU_TIME") != nullptr) {
			vk::QueryPoolCreateInfo info {};
			info.queryType  = vk::QueryType::eTimestamp;
			info.queryCount = static_cast<uint32_t>(timer.pending.size() * 2);
			timer.enabled   = device.createQueryPool(&info, nullptr, &timer.pool) == vk::Result::eSuccess;
			timer.device    = device;
			timer.period    = timestamp_period;
			timer.last      = std::chrono::steady_clock::now();
		}
	}
	if (!timer.enabled) {
		return;
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
	buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timer.pool, slot * 2 + 1);
	timer.pending[slot] = true;
}

} // namespace Libs::Graphics::VulkanDiagnostics
