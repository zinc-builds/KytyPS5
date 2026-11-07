#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>

// Local benchmarking aids, off unless their environment variable is set. Each prints one line
// per second on stdout.
//
// KYTY_VK_COUNT=1: recorded Vulkan commands per second by type ("VKCOUNT ..."; bind_pipeline_repeat
// counts binds of the pipeline already bound, push_descriptor_elements the descriptors pushed), and
// image layout transitions by old and new layout ("VKLAYOUT ..."). KYTY_VK_COUNT=sites also
// prints, every 20 s, three levels of call sites of barriers and render-pass begins and ends
// ("VKSITE kind=... addr=..."; kind + 100 and + 200 are the callers), as offsets from the ELF
// header for the lld map.
//
// KYTY_GPU_TIME=1: GPU execution time of the submitted command buffers ("GPUTIME ...").
// KYTY_GPU_TIME=passes also times each render pass and dispatch ("GPUPASS ..."): GPU time in
// passes and in dispatches, and the costliest pass shapes (extent, color attachments).
// KYTY_GPU_TIME=draws also stamps the end of each draw ("GPUDRAW ..."): GPU time between a draw's
// stamp and the previous one in its pass, grouped by vertex (or mesh) module and by pixel module.
//
// KYTY_PIPELINE_STATS=1: enables VK_KHR_pipeline_executable_properties and prints the driver's
// statistics for every pipeline it creates ("PIPESTATS kind=... vs=<module id> ps=<module id>
// exec=<name> <statistic>=<value> ...": binary size, registers and the like on NVIDIA), with the
// module ids KYTY_GPU_TIME=draws and KYTY_SPV_DUMP use.
//
// KYTY_SPV_DUMP=<dir>: writes each graphics or compute module's SPIR-V as
// <dir>/<stage>_<guest hash>_<module id>.spv when it is compiled or loaded from the shader store.
namespace Libs::Graphics::VulkanDiagnostics {

// Wraps the default dispatcher's command functions; call once it holds the device functions.
void InstallCommandCounters();

// Bracket each command buffer the scheduler submits.
void BeginCommandBuffer(vk::Device device, float timestamp_period, vk::CommandBuffer buffer);
void EndCommandBuffer(vk::CommandBuffer buffer);

// KYTY_GPU_TIME=draws is set (read once).
bool DrawTimingEnabled();
// Recorded right after a draw command, on the thread that records it.
void DrawStamp(vk::CommandBuffer buffer, uint64_t vs_id, uint64_t ps_id, uint64_t vertices,
               bool pipeline_changed);

// KYTY_GPU_TIME=draws: the compute module of the next dispatch recorded into the buffer.
void TagDispatch(vk::CommandBuffer buffer, uint64_t module_id);

// KYTY_PIPELINE_STATS is set (read once).
bool PipelineStatsRequested();
// Prints a pipeline's executable statistics; it was created with eCaptureStatisticsKHR.
void ReportPipelineStatistics(vk::Device device, vk::Pipeline pipeline, const char* kind,
                              uint64_t vs_id, uint64_t ps_id);

void DumpSpirv(const char* stage, uint64_t hash, uint64_t id, std::span<const uint32_t> spirv);

} // namespace Libs::Graphics::VulkanDiagnostics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAGNOSTICS_H_
