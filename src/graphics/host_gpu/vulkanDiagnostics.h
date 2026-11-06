#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_

#include "graphics/host_gpu/vulkanCommon.h"

// Local benchmarking aids, off unless their environment variable is set. Each prints one line
// per second on stdout.
//
// KYTY_VK_COUNT=1: recorded Vulkan commands per second by type ("VKCOUNT ..."), and image
// layout transitions by old and new layout ("VKLAYOUT ..."). KYTY_VK_COUNT=sites also prints,
// every 20 s, the call sites of barriers and render-pass begins ("VKSITE kind=... addr=..."),
// as offsets from the ELF header for the lld map.
//
// KYTY_GPU_TIME=1: GPU execution time of the submitted command buffers ("GPUTIME ...").
namespace Libs::Graphics::VulkanDiagnostics {

// Wraps the default dispatcher's command functions; call once it holds the device functions.
void InstallCommandCounters();

// Bracket each command buffer the scheduler submits.
void BeginCommandBuffer(vk::Device device, float timestamp_period, vk::CommandBuffer buffer);
void EndCommandBuffer(vk::CommandBuffer buffer);

} // namespace Libs::Graphics::VulkanDiagnostics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_
