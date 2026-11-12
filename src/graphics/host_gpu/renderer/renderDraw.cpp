#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/benchMetrics.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/hybridAffinity.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/frameSkip.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/guestSyncEpoch.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/vulkanDiagnostics.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <type_traits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.has_dynamic_vertex_fetch) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = vs_input_info.stage.module->info;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.module->info.info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void UpdateGraphicsDynamicState(const CommandBuffer& buffer, DynamicState& dynamic_state,
                                       const ShaderVertexInputInfo& vs_input_info,
                                       const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.module->info.info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	dynamic_state.SetViewports(std::span {viewports}.first(viewport_count));
	dynamic_state.SetScissors(std::span {scissors}.first(viewport_count));

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	dynamic_state.SetLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	dynamic_state.SetBlendConstants(blend_constants);
	dynamic_state.SetDepthTestEnabled(depth.depth_test_enable);
	dynamic_state.SetDepthWriteEnabled(depth.depth_write_enable);
	dynamic_state.SetDepthCompareOp(depth.depth_compare_op);
#if !defined(__APPLE__)
	dynamic_state.SetDepthBoundsTestEnabled(depth.depth_bounds_test_enable);
	if (depth.depth_bounds_test_enable) {
		dynamic_state.SetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
#endif

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	dynamic_state.SetDepthBiasEnabled(depth_bias_enable);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		dynamic_state.SetDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	// Rect lists are drawn without culling.
	vk::CullModeFlags cull_mode {};
	if (!Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		if (mode.cull_front) {
			cull_mode |= vk::CullModeFlagBits::eFront;
		}
		if (mode.cull_back) {
			cull_mode |= vk::CullModeFlagBits::eBack;
		}
	}
	dynamic_state.SetCullMode(cull_mode);
	dynamic_state.SetFrontFace(mode.face ? vk::FrontFace::eClockwise
	                                     : vk::FrontFace::eCounterClockwise);

	dynamic_state.SetStencilTestEnabled(depth.stencil_test_enable);
	if (depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		dynamic_state.SetStencilOps({front.failOp, front.passOp, front.depthFailOp, front.compareOp},
		                            {back.failOp, back.passOp, back.depthFailOp, back.compareOp});
		dynamic_state.SetStencilCompareMasks(front.compareMask, back.compareMask);
		dynamic_state.SetStencilWriteMasks(front.writeMask, back.writeMask);
		dynamic_state.SetStencilReferences(front.reference, back.reference);
	}
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

struct DrawCallInfo {
	bool     indexed        = true;
	uint32_t index_count    = 0;
	uint32_t instance_count = 0;

	[[nodiscard]] const char* Name() const { return indexed ? "DrawIndex" : "DrawIndexAuto"; }
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count});
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.load_clear_aspects = {};
		if (depth.depth_clear_enable || meta_clear)
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eDepth;
		// Apply pending clears to compressed stencil as well as depth.
		if (depth.stencil_clear_enable || (meta_clear && metadata.stencil_compressed))
			depth.load_clear_aspects |= vk::ImageAspectFlagBits::eStencil;
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("depth attachment feedback loop is not supported by the host\n");
			}
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not, except in a
		// read-only layout, whose aspects store nothing (STORE_OP_NONE).
		const bool read_only =
		    (DepthReadOnly(layout) || !(ImageViewOps::DepthAspectMask(depth.desc.view_info.format) &
		                                vk::ImageAspectFlagBits::eDepth)) &&
		    (StencilReadOnly(layout) || !(ImageViewOps::DepthAspectMask(depth.desc.view_info.format) &
		                                  vk::ImageAspectFlagBits::eStencil));
		const auto access =
		    read_only ? vk::AccessFlags2 {vk::AccessFlagBits2::eDepthStencilAttachmentRead}
		              : vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count});
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear =
		    static_cast<bool>(depth.load_clear_aspects & vk::ImageAspectFlagBits::eStencil);
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	if (mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear)) {
		// FCE can precede any ordinary draw. Discover the target to materialize its native
		// clear metadata before the expanded image is sampled.
		RenderColorInfo target {};
		ResolveRenderColorTarget(buffer, target, 0, true);
		return true;
	}
	// Metadata/decompression shaders must not export colors into the expanded image.
	return mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

// vertex_offset is the first vertex of non-indexed draws.
struct DrawEmitInfo {
	int32_t          vertex_offset  = 0;
	uint32_t         first_instance = 0;
	DrawIndirectArgs indirect;
};

struct DrawIndexBufferSource {
	uint64_t    address            = 0;
	const void* host_data          = nullptr;
	uint64_t    size               = 0;
	uint32_t    guest_element_size = 0;
};

// The task shader's inputs for an indirect mesh draw: Params in gpu_mesh_indirect.task.
struct MeshTaskParams {
	vk::DeviceAddress records        = 0;
	uint64_t          index_address  = 0;
	uint32_t          stride         = 0;
	uint32_t          index_size     = 0;
	uint32_t          index_limit    = 0;
	uint32_t          primitive_size = 0;
	uint32_t          group_step     = 0;
	uint32_t          max_groups     = 0;
	uint32_t          max_instances  = 0;
	uint32_t          max_total      = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

static MergedVertexRanges MergeVertexRanges(const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	MergedVertexRanges merged;
	auto&              sizes = merged.sizes;
	// Collect the non-empty guest vertex ranges.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges;
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size, 0, {}};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	auto&     merged_ranges = merged.ranges;
	uint32_t& merged_count  = merged.count;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end, 0, {}};
	}

	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
	}
	return merged;
}

// speculated: the prepare worker's lookups of the merged ranges, if it made them (count must
// match). precomputed: the worker's MergeVertexRanges of the same stage, if it made it; only
// the clamp to mapped memory is done again (KYTY_WORKER_BUFFERS=verify merges again and compares).
static void AcquireVertexBuffers(CommandBuffer& buffer, const ShaderVertexInputInfo& vs_input_info,
                                 std::span<const uint32_t> speculated,
                                 PreparedVertexBuffers&    prepared,
                                 const MergedVertexRanges* precomputed) {
	std::optional<MergedVertexRanges> own;
	if (precomputed == nullptr) {
		own.emplace(MergeVertexRanges(vs_input_info));
	} else if (BufferCache::WorkerBuffersVerify()) {
		const auto fresh = MergeVertexRanges(vs_input_info);
		bool same = fresh.count == precomputed->count;
		for (int i = 0; same && i < vs_input_info.buffers_num; i++) {
			same = fresh.sizes[i] == precomputed->sizes[i];
		}
		for (uint32_t i = 0; same && i < fresh.count; i++) {
			same = fresh.ranges[i].base_address == precomputed->ranges[i].base_address &&
			       fresh.ranges[i].requested_end == precomputed->ranges[i].requested_end &&
			       fresh.ranges[i].acquired_end == precomputed->ranges[i].acquired_end;
		}
		if (!same) {
			EXIT("KYTY_WORKER_BUFFERS=verify: the worker's merged vertex ranges differ\n");
		}
	}
	const auto& merged       = precomputed != nullptr ? *precomputed : *own;
	const auto& sizes        = merged.sizes;
	const auto  merged_count = merged.count;
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges;
	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		range       = merged.ranges[i];
		if (precomputed != nullptr) {
			// The worker clamped before the commit's PM4 position: mappings may have changed.
			range.acquired_end = range.base_address +
			                     Libs::LibKernel::Memory::ClampRangeSize(range.base_address,
			                                                             range.RequestedSize());
		}
		range.binding = cache.ObtainBuffer(
		    range.base_address, range.acquired_end - range.base_address, false, false, {},
		    speculated.size() == merged_count ? speculated[i] : UINT32_MAX);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			prepared.sizes[i]   = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

// The register side of ResolvePrimitiveRestart.
enum class PrimitiveRestartMode {
	Off,
	On,
	// On only if the index data holds the reset index, which is unsupported: off or an exit.
	Scan,
	Unsupported,
};

static PrimitiveRestartMode PrimitiveRestartFromRegisters(const HW::UserConfig& ucfg,
                                                          const HW::Context&    hw,
                                                          uint32_t              element_size) {
	const auto control = ucfg.GetPrimitiveResetControl();
	if ((control & ~0x3u) != 0) {
		return PrimitiveRestartMode::Unsupported;
	}
	if ((control & 0x1u) == 0) {
		return PrimitiveRestartMode::Off;
	}
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return PrimitiveRestartMode::Off;
	}

	const auto index_mask  = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index = hw.GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return PrimitiveRestartMode::Off;
	}
	// Native restart; the 8-bit path widens its marker to 0xffff.
	return (reset_index & index_mask) == index_mask ? PrimitiveRestartMode::On
	                                                : PrimitiveRestartMode::Scan;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto element_size = source.guest_element_size;
	switch (PrimitiveRestartFromRegisters(buffer.GetUserConfig(), buffer.GetRegisters(),
	                                      element_size)) {
		case PrimitiveRestartMode::Off: return false;
		case PrimitiveRestartMode::On: return true;
		case PrimitiveRestartMode::Scan: break;
		case PrimitiveRestartMode::Unsupported: EXIT_NOT_IMPLEMENTED(true); return false;
	}
	const auto index_mask    = UINT32_MAX >> ((4 - element_size) * 8);
	const auto restart_index = buffer.GetRegisters().GetPrimitiveResetIndex() & index_mask;

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            DrawRenderState& state, QueuedDraw* queued) {
	m_context.GetImageHeap().BeginDraw();
	if (queued != nullptr) {
		AdoptPrograms(*queued, state);
	} else {
		m_context.GetPipelineCache().GetGraphicsPrograms(buffer, state);
	}
	// Vertex programs beyond the host mesh limits or with unsupported call targets are not
	// compiled.
	if (state.vertex_info[0].stage.module == nullptr) {
		return false;
	}
	// Each resolve constructs its color_info entry; the logs and the sample count read entry 0.
	// The worker's proposals are already there, one per resolved slot.
	if (state.mrt_mask == 0) {
		state.color_info.Construct(0);
	}
	const auto* targets  = m_adopted_targets;
	uint32_t    proposal = 0;
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((state.mrt_mask & (1u << slot)) != 0) {
			auto& r = state.color_info[state.color_count];
			if (targets != nullptr) {
				EXIT_IF(proposal >= targets->count);
				if (proposal != state.color_count) {
					state.color_info.Construct(state.color_count, state.color_info[proposal]);
				}
				ResolveProposedColorTarget(buffer, targets->color[proposal++], r, slot);
			} else {
				ResolveRenderColorTarget(buffer, r, slot);
			}
			if (r.image_id) {
				state.color_count++;
			}
		}
	}
	EXIT_IF(targets != nullptr && proposal != targets->count);
	if (targets != nullptr) {
		ResolveProposedDepthTarget(buffer, targets->depth, state.depth_info);
	} else {
		ResolveRenderDepthTarget(buffer, state.depth_info);
	}
	// TODO(PPSA09243): Vulkan passes take one sample count. Where PS5 attachments mix counts,
	// keep those matching the raster count instead of resolving multisampled shadows.
	const auto colors       = std::span {state.color_info.data(), state.color_count};
	const auto pass_samples = state.depth_info.image_id ? state.depth_info.desc.info.samples
	                                                    : state.color_info[0].desc.info.samples;
	if (std::ranges::any_of(colors, [&](const RenderColorInfo& color) {
		    return color.desc.info.samples != pass_samples;
	    })) {
		const auto raster =
		    render_sample_count(buffer.GetRegisters().GetAaConfig().msaa_num_samples);
		const auto dropped = std::ranges::remove_if(colors, [&](const RenderColorInfo& color) {
			return color.desc.info.samples != raster;
		});
		state.color_count -= static_cast<uint32_t>(dropped.size());
		if (state.depth_info.image_id && state.depth_info.desc.info.samples != raster) {
			state.depth_info = {};
		}
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: dropping attachments whose sample count differs from the raster.\n");
		}
	}

	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source,
                                              uint32_t speculated = UINT32_MAX) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type =
	    source.guest_element_size == 4 ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false,
		                                                      false, {}, speculated);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(const CommandBuffer&         buffer,
                                const PreparedVertexBuffers& prepared) {
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		const auto count = prepared.count;
		buffer.Record(
		    count * (sizeof(vk::Buffer) + 2 * sizeof(vk::DeviceSize)),
		    [&](std::byte* data) {
			    std::memcpy(data, prepared.buffers.data(), count * sizeof(vk::Buffer));
			    data += count * sizeof(vk::Buffer);
			    std::memcpy(data, prepared.offsets.data(), count * sizeof(vk::DeviceSize));
			    data += count * sizeof(vk::DeviceSize);
			    std::memcpy(data, prepared.sizes.data(), count * sizeof(vk::DeviceSize));
		    },
		    [count](vk::CommandBuffer cb, const std::byte* data) {
			    const auto* buffers = reinterpret_cast<const vk::Buffer*>(data);
			    const auto* offsets =
			        reinterpret_cast<const vk::DeviceSize*>(data + count * sizeof(vk::Buffer));
			    cb.bindVertexBuffers2(0, count, buffers, offsets, offsets + count, nullptr);
		    });
	}
}

static void CommitIndexBuffer(const CommandBuffer& buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	buffer.Record([handle = prepared.buffer, offset = prepared.offset,
	               type = prepared.type](vk::CommandBuffer cb) {
		cb.bindIndexBuffer(handle, offset, type);
	});
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.indexed && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, const CommandBuffer& buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.indexed) {
				buffer.Record([count = draw.index_count, instances = draw.instance_count,
				               vertex = emit.vertex_offset,
				               first = emit.first_instance](vk::CommandBuffer cb) {
					cb.drawIndexed(count, instances, 0, vertex, first);
				});
			} else {
				buffer.Record([count = draw.index_count, instances = draw.instance_count,
				               vertex = static_cast<uint32_t>(emit.vertex_offset),
				               first = emit.first_instance](vk::CommandBuffer cb) {
					cb.draw(count, instances, vertex, first);
				});
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.indexed) {
					buffer.Record([instances = draw.instance_count, i, vertex = emit.vertex_offset,
					               first = emit.first_instance](vk::CommandBuffer cb) {
						cb.drawIndexed(4, instances, i, vertex, first);
					});
				} else {
					buffer.Record([instances = draw.instance_count,
					               vertex = i + static_cast<uint32_t>(emit.vertex_offset),
					               first = emit.first_instance](vk::CommandBuffer cb) {
						cb.draw(4, instances, vertex, first);
					});
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

bool RenderExecutor::CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
                                       const DrawEmitInfo& emit, DrawRenderState& state) {
	if (!buffer.GetRegisters().GetRenderControl().copy_depth_to_color) {
		return false;
	}
	auto& ucfg = buffer.GetUserConfig();
	const auto& hw = buffer.GetRegisters();
	auto& depth = state.depth_info;
	auto& color = state.color_info[0];
	EXIT_NOT_IMPLEMENTED(
	    !m_context.GetGraphics().maintenance8_enabled ||
	    !Prospero::IsRectList(ucfg.GetPrimType()) || draw.indexed || draw.index_count != 3 ||
	    draw.instance_count != 1 || emit.vertex_offset != 0 || emit.first_instance != 0 ||
	    state.color_count != 1 || color.target_slot != 0 || !depth.image_id ||
	    color.desc.info.samples != 1 || depth.desc.info.extent.width != color.Extent().width ||
	    depth.desc.info.extent.height != color.Extent().height ||
	    depth.desc.view_info.layer_count != 1 || color.desc.view_info.layer_count != 1 ||
	    hw.GetBlendControl(0).enable || state.ps_input_info.ps_pixel_kill_enable ||
	    state.ps_input_info.ps_depth_export_enable ||
	    state.ps_input_info.ps_sample_mask_export_enable);
	vk::ImageAspectFlags feedback;
	const auto rendering = AcquireRenderTargets(buffer, &color, 1, depth, feedback);
	if (depth.load_clear_aspects) {
		buffer.BeginRendering(rendering);
	}
	auto& cache = m_context.GetTextureCache();
	auto& source = cache.GetImage(depth.image_id);
	auto& destination = cache.GetImage(color.image_id);
	EXIT_NOT_IMPLEMENTED((source.backing.format != vk::Format::eD32Sfloat &&
	                      source.backing.format != vk::Format::eD32SfloatS8Uint) ||
	                     destination.backing.format != vk::Format::eR32Sfloat);
	const auto rect = calc_final_scissor(hw.GetScreenViewport(), hw.GetScanModeControl(),
	                                    {rendering.width, rendering.height}, 0);
	if (rect.right > rect.left && rect.bottom > rect.top) {
		const vk::ImageCopy region {
		    {vk::ImageAspectFlagBits::eDepth, depth.desc.view_info.base_level,
		     depth.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {vk::ImageAspectFlagBits::eColor, color.desc.view_info.base_level,
		     color.desc.view_info.base_layer, 1}, {rect.left, rect.top, 0},
		    {static_cast<uint32_t>(rect.right - rect.left),
		     static_cast<uint32_t>(rect.bottom - rect.top), 1}};
		buffer.EndRendering();
		const auto command = buffer.Handle();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		               {}, command);
		destination.Transit(vk::ImageLayout::eTransferDstOptimal,
		                    vk::AccessFlagBits2::eTransferWrite, {}, command);
		command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  destination.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                  1, &region);
	}
	return true;
}

void RenderExecutor::ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
                                         DrawRenderState& state, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source) {
	auto&      ucfg                     = buffer.GetUserConfig();
	const bool primitive_restart_enable = state.primitive_restart_enable;
	if (CopyDepthToColor(buffer, draw, emit, state)) {
		return;
	}
	const auto  vertex_stages = state.VertexStages();
	const bool  mesh_active   = state.vertex_info[0].stage.module->info.stage == ShaderType::Mesh;
	const bool  mesh_task     = state.vertex_info[0].stage.module->mesh_task;
	const auto& mesh          = state.vertex_info[0].mesh;
	const auto& limits        = m_context.GetGraphics().mesh_shader_properties;
	uint32_t    mesh_groups   = 0;
	uint32_t    mesh_instance_slice = 0;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.indexed || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed);
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.indexed, primitive_restart_enable);
		}
	}
	if (mesh_task) {
		EXIT_NOT_IMPLEMENTED(emit.indirect.count > std::min(limits.maxTaskWorkGroupCount[0],
		                                                    limits.maxTaskWorkGroupTotalCount));
	} else if (mesh_active) {
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups = (primitives - 1u) / mesh.primitives_per_group + 1u;
		// Instances beyond the host limits are drawn in slices below.
		mesh_instance_slice = std::min(limits.maxMeshWorkGroupCount[1],
		                               limits.maxMeshWorkGroupTotalCount / mesh_groups);
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] || mesh_instance_slice == 0) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && draw.indexed) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		PrepareBindings(state.ps_input_info.stage, bindings.pixel);
		descriptor_stages[stage_count++] = &bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info.data(), state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		AcquireVertexBuffers(
		    buffer, state.vertex_info[0],
		    m_adopted_buffers != nullptr
		        ? std::span<const uint32_t> {m_adopted_buffers->vertex_speculated.data(),
		                                     m_adopted_buffers->vertex_count}
		        : std::span<const uint32_t> {},
		    vertex_bindings,
		    m_adopted_buffers != nullptr ? &m_adopted_buffers->vertex_ranges : nullptr);
		index_binding   = PrepareIndexBuffer(
		    buffer, index_source,
		    m_adopted_buffers != nullptr ? m_adopted_buffers->index_speculated : UINT32_MAX);
	}
	std::pair<Buffer*, uint64_t> args_buffer {};
	if (emit.indirect.address != 0) {
		const auto size = static_cast<uint64_t>(emit.indirect.count - 1) * emit.indirect.stride +
		                  (draw.indexed ? sizeof(vk::DrawIndexedIndirectCommand)
		                                : sizeof(vk::DrawIndirectCommand));
		args_buffer = m_context.GetBufferCache().ObtainBuffer(emit.indirect.address, size, false);
		EXIT_IF((args_buffer.second & 3u) != 0);
	}
	auto& pipeline =
	    m_context.GetPipelineCache().GetGraphicsPipeline(buffer, state, m_adopted_pipeline_key);
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info.data(), state.color_count, state.depth_info,
	                         feedback_aspects, stages);
	vk::DeviceAddress task_params = 0;
	if (mesh_task) {
		const MeshTaskParams params {
		    .records        = args_buffer.first->BufferDeviceAddress() + args_buffer.second,
		    .index_address  = index_source.address,
		    .stride         = emit.indirect.stride,
		    .index_size     = index_source.guest_element_size,
		    .index_limit    = draw.index_count,
		    .primitive_size = mesh.InputPrimitiveSize(),
		    .group_step     = mesh.primitives_per_group * mesh.InputPrimitiveStep(),
		    .max_groups     = limits.maxMeshWorkGroupCount[0],
		    .max_instances  = limits.maxMeshWorkGroupCount[1],
		    .max_total      = limits.maxMeshWorkGroupTotalCount,
		};
		auto& stream = m_context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		task_params  = stream.BufferDeviceAddress() + stream.Copy(&params, sizeof(params), 8);
	}

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory. CommitBindings records layout transitions directly; the draw's own commands after
	// them are queued for the recording thread (CommandBuffer::Record), so anything recorded
	// directly below must take a fresh buffer.Handle().
	auto& dynamic_state = m_context.GetCommandScheduler().GetDynamicState();
	auto& bound         = dynamic_state.bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	if (!mesh_active) {
		CommitVertexBuffers(buffer, vertex_bindings);
		CommitIndexBuffer(buffer, index_binding);
	}
	using ShaderRecompiler::IR::PushData;
	const auto draw_dwords =
	    PushData::GraphicsDrawDwords(mesh_active, state.ps_input_info.object_primitive_id);
	if (draw_dwords != 0) {
		std::array<uint32_t, PushData::MeshDrawDwordCount> draw_data {};
		if (mesh_task) {
			draw_data[0] = static_cast<uint32_t>(task_params);
			draw_data[1] = static_cast<uint32_t>(task_params >> 32u);
		} else if (mesh_active) {
			draw_data = {draw.index_count,
			             static_cast<uint32_t>(emit.vertex_offset),
			             index_source.guest_element_size,
			             static_cast<uint32_t>(index_source.address),
			             static_cast<uint32_t>(index_source.address >> 32u)};
		} else {
			draw_data[0] = buffer.GetUserConfig().GetObjectId();
		}
		bound.PushConstants(buffer, pipeline.pipeline_layout, pipeline.push_stages, 0,
		                    draw_dwords * sizeof(uint32_t), draw_data.data());
	}
	if (const auto planes = vertex_stages.back().user_clip_planes; planes != 0) {
		bound.PushConstants(buffer, pipeline.pipeline_layout, pipeline.push_stages,
		                    PushData::UserClipPlaneDword * sizeof(uint32_t),
		                    std::bit_width(planes) * 4u * sizeof(uint32_t),
		                    buffer.GetRegisters().GetUserClipPlanes().data());
	}

	UpdateGraphicsDynamicState(buffer, dynamic_state, vertex_stages.back(), state.depth_info,
	                           rendering);
	dynamic_state.SetAttachmentFeedbackLoopAspects(feedback_aspects);
	dynamic_state.Commit(m_context.GetGraphics(), buffer);

	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (FrameSkip::Active()) {
		// Frame skipping keeps draws whose shaders write memory, and drops a shader combination
		// only once it was inspected here.
		bool image_writes  = false;
		bool buffer_writes = false; // from shader metadata, whatever the descriptors hold now
		bool known         = true;
		bool atomic_only   = true;
		std::array<FrameSkip::WrittenRange, 4> ranges {};
		int                                    range_count = 0;
		for (const auto* stage: stages) {
			const auto& program = stage->runtime->module->info;
			known         = known && !program.has_address_writes && !program.info.uses_dma;
			buffer_writes = buffer_writes || program.has_address_writes;
			for (const auto& image: program.info.images) {
				image_writes = image_writes || image.written || image.atomic;
			}
			for (const auto& meta: program.info.buffers) {
				if (!meta.written && !meta.atomic) {
					continue;
				}
				buffer_writes = true;
				atomic_only   = atomic_only && meta.atomic && !meta.plain_access;
				if (meta.descriptor_index >= stage->buffer_sources.size()) {
					known = false;
					continue;
				}
				const auto& source = stage->buffer_sources[meta.descriptor_index];
				if (source.size != 0 && range_count < static_cast<int>(ranges.size())) {
					ranges[range_count++] = {source.address, source.size};
				}
			}
		}
		FrameSkip::NoteCommittedDraw(buffer.GetShaders(), state.ps_active, buffer_writes,
		                             image_writes, atomic_only, known, ranges.data(), range_count);
	}
	if (shader_write_stages || HasAtomicWrites()) {
		// Guest ranges of the draw's buffer accesses: atomics to buffers the stage accesses only
		// atomically, and everything else. Raw address or indirect accesses have no known range.
		bool                                       known       = true;
		bool                                       plain_write = false;
		std::vector<std::pair<uint64_t, uint64_t>> atomic;
		std::vector<std::pair<uint64_t, uint64_t>> plain;
		for (const auto* stage: stages) {
			const auto& program = stage->runtime->module->info;
			known = known && !program.has_address_writes && !program.info.uses_dma;
			for (const auto& meta: program.info.buffers) {
				known = known && meta.indirect_root == ShaderRecompiler::IR::BufferResource::NoIndirectBuffer &&
				        meta.indirect_resources.empty();
				if (meta.descriptor_index == UINT32_MAX ||
				    meta.descriptor_index >= stage->buffer_sources.size()) {
					continue;
				}
				const auto& source = stage->buffer_sources[meta.descriptor_index];
				if (source.size == 0) {
					continue;
				}
				if (meta.atomic && !meta.plain_access) {
					atomic.emplace_back(source.address, source.size);
				} else {
					plain.emplace_back(source.address, source.size);
					plain_write = plain_write || meta.written;
				}
			}
		}
		if (!mesh_active) {
			const auto& vs = state.vertex_info[0];
			for (int i = 0; i < vs.buffers_num; i++) {
				if (const auto size = VertexBufferDescriptorSize(i, vs); size != 0) {
					plain.emplace_back(vs.buffers[i].addr, size);
				}
			}
		}
		if (index_source.address != 0 && index_source.size != 0) {
			plain.emplace_back(index_source.address,
			                   index_source.host_data != nullptr
			                       ? uint64_t {draw.index_count} * index_source.guest_element_size
			                       : index_source.size);
		}
		if (args_buffer.first != nullptr) {
			plain.emplace_back(emit.indirect.address,
			                   static_cast<uint64_t>(emit.indirect.count - 1) * emit.indirect.stride +
			                       (draw.indexed ? sizeof(vk::DrawIndexedIndirectCommand)
			                                     : sizeof(vk::DrawIndirectCommand)));
		}
		const bool atomic_draw = shader_write_stages && known && !plain_write && !atomic.empty();
		auto&      batch       = m_atomic_batch;
		if (HasAtomicWrites()) {
			// Plain accesses must see the batch's writes; new atomics must follow earlier
			// plain accesses (the batch skipped their write-after-read barrier).
			bool conflict = !known || plain_write;
			for (const auto& [address, size]: plain) {
				conflict = conflict || batch.atomic.Intersects(address, size);
			}
			for (const auto& [address, size]: atomic) {
				conflict = conflict || (atomic_draw && batch.plain.Intersects(address, size));
			}
			if (conflict) {
				FlushAtomicWrites(buffer);
			}
		}
		if (atomic_draw) {
			if (!HasAtomicWrites()) {
				buffer.EndRendering();
				ShaderWriteHazardBarrier(buffer.Handle(), shader_write_stages);
			}
			batch.stages |= shader_write_stages;
			for (const auto& [address, size]: atomic) {
				batch.atomic.Add(address, size);
			}
			shader_write_stages = {};
		}
		if (HasAtomicWrites()) {
			for (const auto& [address, size]: plain) {
				batch.plain.Add(address, size);
			}
		}
	}
	if (shader_write_stages) {
		// As for dispatches, buffer writes wait for earlier accesses to the memory.
		buffer.EndRendering();
		ShaderWriteHazardBarrier(buffer.Handle(), shader_write_stages);
	}
	buffer.BeginRendering(rendering);
	const bool pipeline_changed = bound.BoundPipeline() != pipeline.pipeline;
	bound.BindPipeline(buffer, pipeline.pipeline, pipeline.pipeline_layout);
	auto&         occlusion_queries = m_context.GetOcclusionQueries();
	const int32_t occlusion_query =
	    occlusion_queries.Counting() ? occlusion_queries.Begin(buffer.Handle()) : -1;
	if (mesh_task) {
		buffer.Record([count = emit.indirect.count](vk::CommandBuffer cb) {
			cb.drawMeshTasksEXT(count, 1, 1);
		});
	} else if (mesh_active) {
		for (uint32_t done = 0; done < draw.instance_count;) {
			if (done != 0) {
				bound.PushConstants(buffer, pipeline.pipeline_layout, pipeline.push_stages,
				                    PushData::MeshInstanceSliceDword * sizeof(uint32_t),
				                    sizeof(done), &done);
			}
			const auto count = std::min(mesh_instance_slice, draw.instance_count - done);
			buffer.Record([mesh_groups, count](vk::CommandBuffer cb) {
				cb.drawMeshTasksEXT(mesh_groups, count, 1);
			});
			done += count;
		}
	} else if (args_buffer.first == nullptr) {
		EmitDrawPrimitives(ucfg, buffer, draw, emit);
	} else {
		buffer.Record([indexed = draw.indexed, handle = args_buffer.first->Handle(),
		               offset = args_buffer.second, count = emit.indirect.count,
		               stride = emit.indirect.stride](vk::CommandBuffer cb) {
			if (indexed) {
				cb.drawIndexedIndirect(handle, offset, count, stride);
			} else {
				cb.drawIndirect(handle, offset, count, stride);
			}
		});
	}
	if (VulkanDiagnostics::DrawTimingEnabled()) {
		buffer.Record([vs = state.vertex_info[0].stage.module->id,
		               ps = state.ps_active ? state.ps_input_info.stage.module->id : 0,
		               vertices = args_buffer.first != nullptr
		                              ? 0
		                              : uint64_t {draw.index_count} * draw.instance_count,
		               pipeline_changed](vk::CommandBuffer cb) {
			VulkanDiagnostics::DrawStamp(cb, vs, ps, vertices, pipeline_changed);
		});
	}
	if (occlusion_query >= 0) {
		occlusion_queries.End(buffer.Handle(), occlusion_query);
	}
	buffer.Publish();
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Draws);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DrawInstances, draw.instance_count);

	if (shader_write_stages) {
		buffer.EndRendering();
		ShaderWriteBarrier(buffer.Handle(), shader_write_stages);
	}
}

// Vulkan offsets for a prepared draw, or nullopt when its indirect records need the CPU:
// quad-list draws for counts, tessellation draws for the index/SGPR prolog and mesh draws without
// room for the task payload. Indirect records carry their own offsets.
static std::optional<DrawEmitInfo> ResolveDrawEmit(const HW::UserConfig&        ucfg,
                                                   const ShaderVertexInputInfo& vs,
                                                   DrawOffsetSource             source,
                                                   int32_t                      first_vertex,
                                                   uint32_t                     first_instance,
                                                   const DrawIndirectArgs&      indirect) {
	if ((indirect.address != 0 || source == DrawOffsetSource::IndirectArgs) &&
	    ucfg.GetIndexOffset() != 0) {
		static std::once_flag warning_once;
		std::call_once(warning_once, [&] {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GPU: indirect draw ignores GE_INDX_OFFSET=0x{:08x}; geometry may be incorrect.\n",
			    ucfg.GetIndexOffset()));
		});
	}
	if (indirect.address != 0) {
		if (!vs.indirect_draw ||
		    ucfg.GetPrimType() == Prospero::PrimitiveType::kQuadListLegacy ||
		    vs.indirect_offsets != indirect.offsets) {
			return std::nullopt;
		}
		return DrawEmitInfo {.indirect = indirect};
	}
	if (source == DrawOffsetSource::IndirectArgs) {
		// Native fetches consume the patched SGPRs; rewritten fetches need Vulkan offsets.
		return vs.has_dynamic_vertex_fetch ? DrawEmitInfo {first_vertex, first_instance} : DrawEmitInfo {};
	}
	const auto [vertex_offset, instance_offset] = ResolveDrawOffsets(ucfg.GetIndexOffset(), vs);
	return DrawEmitInfo {vertex_offset + first_vertex, instance_offset};
}

void RenderExecutor::FlushAtomicWrites(CommandBuffer& buffer) {
	if (!HasAtomicWrites()) {
		return;
	}
	buffer.EndRendering();
	ShaderWriteBarrier(buffer.Handle(), m_atomic_batch.stages);
	m_atomic_batch.stages = {};
	m_atomic_batch.atomic.Clear(); // keeping their capacity
	m_atomic_batch.plain.Clear();
}

bool RenderExecutor::DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args,
                               QueuedDraw* queued) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().RetirePendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || (args.instance_count == 0 && args.indirect.address == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	// A queued draw's state already holds the worker's program lookup.
	std::optional<DrawRenderState> own_state;
	if (queued == nullptr) {
		own_state.emplace(DrawRenderState {.indirect_offsets = args.indirect.offsets,
		                                   .indirect_draw    = args.indirect.address != 0});
	}
	auto& state = queued != nullptr ? queued->state : *own_state;
	if (!GetDrawTopology(ucfg, state.topology)) {
		return true;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16: index_source.guest_element_size = 2; break;
		case Prospero::IndexType::kIndex32: index_source.guest_element_size = 4; break;
		case Prospero::IndexType::kIndex8: index_source.guest_element_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	index_source.size = static_cast<uint64_t>(args.index_count) * index_source.guest_element_size;
	state.primitive_restart_enable = ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] =
			    state.primitive_restart_enable && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {true, args.index_count, args.instance_count};
	if (!PrepareDrawRenderState(buffer, draw, state, queued)) {
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
	                     args.index_addr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  args.base_vertex, args.first_instance, args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, index_source);
	}
	ResetBindings();
	return emit.has_value();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool RenderExecutor::DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args,
                              QueuedDraw* queued) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().RetirePendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	Common::LockGuard lock(m_context.GetMutex());
	if (args.indirect.address == 0 && (args.vertex_count == 0 || args.instance_count == 0)) {
		return true;
	}

	if (ConsumeMetadataColorOperation(buffer) || DepthStencilCopy(buffer) ||
	    ResolveColorTargets(buffer)) {
		ResetBindings();
		return true;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return true;
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {false, args.vertex_count, args.instance_count};

	std::optional<DrawRenderState> own_state;
	if (queued == nullptr) {
		own_state.emplace(DrawRenderState {.indirect_offsets = args.indirect.offsets,
		                                   .indirect_draw    = args.indirect.address != 0});
	}
	auto& state = queued != nullptr ? queued->state : *own_state;
	if (!GetDrawTopology(ucfg, state.topology)) {
		ResetBindings();
		return true;
	}
	if (!PrepareDrawRenderState(buffer, draw, state, queued)) {
		ResetBindings();
		return true;
	}

	const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
	if (rect_list && state.vertex_info[0].buffers_num == 0 &&
	    state.vertex_info[0].stage.module->info.param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return true;
	}

	LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);
	const auto emit = ResolveDrawEmit(ucfg, state.vertex_info[0], args.offset_source,
	                                  static_cast<int32_t>(args.first_vertex), args.first_instance,
	                                  args.indirect);
	if (emit) {
		ExecutePreparedDraw(buffer, draw, state, *emit, {});
	}
	ResetBindings();
	return emit.has_value();
}

namespace {

uint64_t PrepareNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

bool EnvFlag(const char* name, bool fallback) {
	const char* value = std::getenv(name);
	return value == nullptr ? fallback : value[0] != '0';
}

// A fresh state for a program lookup, keeping what the draw path set before it.
void ResetProgramState(DrawRenderState& state) {
	const auto topology = state.topology;
	const auto restart  = state.primitive_restart_enable;
	const auto offsets  = state.indirect_offsets;
	const auto indirect = state.indirect_draw;
	// Reconstructed, not assigned: its arrays are left unset until used.
	std::destroy_at(&state);
	std::construct_at(&state, DrawRenderState {.topology                 = topology,
	                                           .primitive_restart_enable = restart,
	                                           .indirect_offsets         = offsets,
	                                           .indirect_draw            = indirect});
}

bool SameSnapshot(const ShaderRecompiler::IR::ResourceSnapshot& a,
                  const ShaderRecompiler::IR::ResourceSnapshot& b) {
	return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data &&
	       a.specialization_reads == b.specialization_reads && a.uniform_fill == b.uniform_fill;
}

template <typename Info>
std::string CompareStage(const char* name, const Info& a, const PipelineCache::StageResources& ar,
                         const Info& b, const PipelineCache::StageResources& br) {
	if (a.stage.module != b.stage.module) {
		return std::string(name) + ": module";
	}
	if (a.stage.module == nullptr) {
		return {};
	}
	if (!SameSnapshot(ar.snapshot, br.snapshot)) {
		return std::string(name) + ": resources";
	}
	if (ar.specialization != br.specialization) {
		return std::string(name) + ": specialization";
	}
	std::vector<uint32_t> ka;
	std::vector<uint32_t> kb;
	BuildStageStaticKey(a, ka);
	BuildStageStaticKey(b, kb);
	if (ka != kb) {
		return std::string(name) + ": static inputs";
	}
	if constexpr (std::is_same_v<Info, ShaderVertexInputInfo>) {
		if (a.buffers_num != b.buffers_num || a.resources_num != b.resources_num ||
		    std::memcmp(a.buffers, b.buffers, sizeof(a.buffers[0]) * std::max(a.buffers_num, 0)) != 0 ||
		    std::memcmp(a.resources, b.resources,
		                sizeof(a.resources[0]) * std::max(a.resources_num, 0)) != 0) {
			return std::string(name) + ": vertex inputs";
		}
	}
	return {};
}

// The first difference between two lookups of one draw, or empty.
std::string ComparePrograms(const DrawRenderState& a, const PipelineCache::GraphicsResources& ar,
                            const DrawRenderState& b, const PipelineCache::GraphicsResources& br) {
	if (a.ps_active != b.ps_active || a.mrt_mask != b.mrt_mask ||
	    a.vertex_stage_count != b.vertex_stage_count) {
		return "pixel activity, targets or vertex stages";
	}
	static constexpr const char* names[] = {"vertex", "hull", "domain"};
	for (uint32_t i = 0; i < a.vertex_stage_count; i++) {
		if (auto diff = CompareStage(names[i], a.vertex_info[i], ar[i], b.vertex_info[i], br[i]);
		    !diff.empty()) {
			return diff;
		}
	}
	if (a.ps_active) {
		return CompareStage("pixel", a.ps_input_info, ar[3], b.ps_input_info, br[3]);
	}
	return {};
}

// What the draw path would do with these registers besides an ordinary draw.
bool QueueableRegisters(const HW::Context& ctx, const HW::UserConfig& ucfg, const HW::Shader& sh) {
	const auto mode = ctx.GetColorControl().mode;
	if (mode != static_cast<uint8_t>(CbColorMode::Normal)) {
		const auto& override = ctx.GetDepthRenderOverride();
		if (mode != static_cast<uint8_t>(CbColorMode::Disable) || override.force_z_dirty ||
		    override.force_stencil_dirty) {
			return false; // metadata operations, resolves, depth and stencil copies
		}
	}
	vk::PrimitiveTopology topology {};
	return DrawHasValidVertexShader(sh) && GetDrawTopology(ucfg, topology);
}

} // namespace

RenderExecutor::QueuedDraw* RenderExecutor::NextQueuedDraw(const CommandBuffer& buffer) {
	static const bool enabled = EnvFlag("KYTY_PREPARE_THREAD", true);
	if (!enabled ||
	    !QueueableRegisters(buffer.GetRegisters(), buffer.GetUserConfig(), buffer.GetShaders())) {
		return nullptr;
	}
	if (m_prepare_lanes.empty()) {
		const auto workers = std::max(Common::HybridAffinity::PrepareWorkerCount(), 1u);
		EXIT_IF(workers > ShaderRecompiler::IR::ResourcePlan::MaxPrepareWorkers);
		const bool free_lanes = workers > 1 && EnvFlag("KYTY_FREE_LANES", true);
		// Deeper with free lanes: a worker past a slow draw can then run further ahead (with strict
		// alternation the extra draws only queue behind it; either alone measured flat).
		const char* depth_text = std::getenv("KYTY_PREPARE_DEPTH");
		const auto  depth = std::clamp(depth_text != nullptr ? std::atoi(depth_text) : free_lanes ? 8 : 4,
		                               1, 16);
		// One more slot than the queue, rounded up to a multiple of the workers.
		m_queued_draws.resize((depth + workers) / workers * workers);
		for (auto& slot: m_queued_draws) {
			slot = std::make_unique<QueuedDraw>();
		}
		m_prepare_verify = EnvFlag("KYTY_PREPARE_VERIFY", false);
		m_prepare_stats  = EnvFlag("KYTY_PREPARE_STATS", false);
		m_drain_stats    = EnvFlag("KYTY_DRAIN_STATS", false);
		// KYTY_SHARED_TEXTURE_MEMO=0: each worker keeps its own texture memo.
		const bool shared_memo = EnvFlag("KYTY_SHARED_TEXTURE_MEMO", true);
		// With free lanes, an overflow worker (HybridAffinity::PrepareOverflowCpu; by default the SMT
		// sibling of the last worker's core) takes only draws at least KYTY_PREPARE_OVERFLOW_K
		// (default 3) past the oldest untaken one: burst capacity that never holds a draw the
		// command thread wants next. KYTY_PREPARE_OVERFLOW=0 turns it off.
		const bool     overflow   = free_lanes && Common::HybridAffinity::PrepareOverflowCpu() >= 0;
		const char*    k_text     = std::getenv("KYTY_PREPARE_OVERFLOW_K");
		const uint32_t overflow_k = overflow ? static_cast<uint32_t>(std::clamp(
		                                           k_text != nullptr ? std::atoi(k_text) : 3, 1, 15))
		                                     : 0u;
		const uint32_t lanes      = workers + (overflow ? 1u : 0u);
		if (free_lanes) {
			m_prepare_pool = std::make_unique<DrawPrepare::Pool>(
			    static_cast<uint32_t>(m_queued_draws.size()), overflow_k);
		}
		for (uint32_t i = 0; i < lanes; i++) {
			auto lane      = std::make_unique<PrepareLane>();
			lane->executor = this;
			lane->index    = i;
			if (i == 0 || !shared_memo) {
				m_texture_memos.push_back(std::make_unique<SharedTextureMemo>());
			}
			lane->textures.memo = m_texture_memos.back().get();
			// Odd serials, distinct among the workers.
			lane->textures.serial      = 1 + 2 * i;
			lane->textures.serial_step = 2 * lanes;
			lane->stream_ring = m_context.GetBufferCache().CreateWorkerStreamRing();
			lane->worker   = std::make_unique<DrawPrepare::Worker>(&PrepareQueuedDraw, lane.get(), i,
			                                                      m_prepare_pool.get(), i >= workers);
			m_prepare_lanes.push_back(std::move(lane));
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "Draw preparation on {} thread(s){}, {} draws ahead{}{}\n", workers,
		    m_prepare_pool ? " (free lanes)" : "", m_queued_draws.size() - 1,
		    overflow ? fmt::format(", overflow worker on CPU {} (k={})",
		                           Common::HybridAffinity::PrepareOverflowCpu(), overflow_k)
		             : std::string(),
		    m_prepare_verify ? " (verifying)" : ""));
	}
	// The ring holds one more slot than the queue, so the next one is free.
	EXIT_IF(m_queued_count >= m_queued_draws.size());
	return m_queued_draws[m_queued_posted % m_queued_draws.size()].get();
}

namespace {
// KYTY_REGISTER_SNAPSHOT: 1 (default) share, 0 copy per draw, 2 verify.
int RegisterSnapshotMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_REGISTER_SNAPSHOT");
		if (value == nullptr) return 1;
		return std::string_view(value) == "0" ? 0 : std::string_view(value) == "verify" ? 2 : 1;
	}();
	return mode;
}
} // namespace

HW::Context* RenderExecutor::RegisterSnapshotFor(const HW::Context& live, uint64_t sequence) {
	const auto mode    = RegisterSnapshotMode();
	auto*      current = m_register_snapshot;
	// Compare only the lines written since the last post with the previous draw's registers.
	const bool same_source = current != nullptr && m_register_snapshot_source == &live;
	const auto written     = live.WrittenLines();
	live.ClearWritten();
	m_register_lines_changed =
	    same_source ? (written != 0 ? HW::Context::DifferingLines(live, current->registers, written) : 0)
	                : ~uint64_t {0};
	if (mode != 0 && same_source && m_register_lines_changed == 0) {
		if (mode == 2 && !current->registers.SameRegisters(live)) {
			EXIT("KYTY_REGISTER_SNAPSHOT=verify: the registers changed without a written mark\n");
		}
		current->last_sequence = sequence;
		return &current->registers;
	}
	// Queued draws not yet committed: sequences from `oldest` on.
	const auto oldest = m_queued_posted - m_queued_count + 1;
	if (m_register_snapshots.empty()) {
		m_register_snapshots.push_back(std::make_unique<RegisterSnapshot>());
	}
	auto& candidate = m_register_snapshots[m_register_snapshot_next];
	if (candidate.get() == current || candidate->last_sequence >= oldest) {
		// Still in use: one more snapshot (they are taken in order, so the next one is the
		// oldest).
		m_register_snapshots.insert(m_register_snapshots.begin() +
		                                static_cast<ptrdiff_t>(m_register_snapshot_next),
		                            std::make_unique<RegisterSnapshot>());
	}
	auto* snapshot = m_register_snapshots[m_register_snapshot_next].get();
	m_register_snapshot_next = (m_register_snapshot_next + 1) % m_register_snapshots.size();
	if (mode == 2) {
		for (const auto& slot: m_queued_draws) {
			EXIT_IF(slot->registers == &snapshot->registers && slot->sequence >= oldest &&
			        slot->sequence <= m_queued_posted);
		}
	}
	snapshot->registers = live;
	m_prepare_stat.register_snapshots++;
	snapshot->last_sequence    = sequence;
	m_register_snapshot        = snapshot;
	m_register_snapshot_source = &live;
	return &snapshot->registers;
}

void RenderExecutor::PostQueuedDraw(CommandBuffer& buffer, QueuedDraw& queued) {
	queued.registers = RegisterSnapshotFor(buffer.GetRegisters(), m_queued_posted + 1);
	if (RegisterSnapshotMode() == 2) {
		if (!queued.registers_verify) {
			queued.registers_verify = std::make_unique<HW::Context>();
		}
		*queued.registers_verify = buffer.GetRegisters();
	}
	queued.user_config        = buffer.GetUserConfig();
	queued.shaders            = buffer.GetShaders();
	queued.hazard_generation  = DrawPrepare::HazardGeneration();
	queued.sync_epoch         = GuestSyncEpoch::CurrentUnranged();
	queued.stored_sequence    = DrawPrepare::StoredSequence();
	queued.gpu_write_sequence = DrawPrepare::GpuWriteSequence();
	if (m_post_after_drain) {
		// KYTY_DRAIN_STATS: are the workers asleep when the first draw after a drain is posted?
		m_post_after_drain = false;
		if (m_prepare_pool) {
			const auto sleeping = m_prepare_pool->SleepingWorkers();
			m_drain_stat.refill_posts++;
			m_drain_stat.refill_asleep_any += sleeping != 0 ? 1 : 0;
			m_drain_stat.refill_asleep_all += sleeping == m_prepare_pool->Workers() ? 1 : 0;
			m_refill_asleep = sleeping == m_prepare_pool->Workers();
		}
	}
	queued.sequence           = ++m_queued_posted;
	// The worker finds the slot from the sequence.
	EXIT_IF(m_queued_draws[(queued.sequence - 1) % m_queued_draws.size()].get() != &queued ||
	        (m_prepare_pool ? m_prepare_pool->Post() != queued.sequence
	                        : PrepareWorker(queued.sequence).Post() != PrepareJob(queued.sequence)));
	m_queued_count++;
	m_prepare_stat.queued++;
	while (m_queued_count + 1 > m_queued_draws.size()) {
		FinishQueuedDraw(buffer);
	}
}

void RenderExecutor::QueueDrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args) {
	auto* queued = args.indirect.address == 0 && args.index_count != 0 && args.instance_count != 0
	                   ? NextQueuedDraw(buffer)
	                   : nullptr;
	if (queued == nullptr) {
		FinishQueuedDraws(buffer);
		m_prepare_stat.direct++;
		(void)DrawIndex(buffer, args);
		return;
	}
	queued->indexed    = true;
	queued->index_args = args;
	PostQueuedDraw(buffer, *queued);
}

void RenderExecutor::QueueDrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args) {
	auto* queued = args.indirect.address == 0 && args.vertex_count != 0 && args.instance_count != 0
	                   ? NextQueuedDraw(buffer)
	                   : nullptr;
	if (queued == nullptr) {
		FinishQueuedDraws(buffer);
		m_prepare_stat.direct++;
		(void)DrawAuto(buffer, args);
		return;
	}
	queued->indexed   = false;
	queued->auto_args = args;
	PostQueuedDraw(buffer, *queued);
}

void RenderExecutor::WaitPrepareIdle() {
	uint64_t waited = 0;
	for (auto& lane: m_prepare_lanes) {
		waited += lane->worker->WaitIdle();
	}
	if (waited != 0) {
		Common::BenchMetrics::Add(Common::BenchMetrics::Counter::IdleWaitNs, waited);
		Common::BenchMetrics::Add(Common::BenchMetrics::Counter::PrepareWaits);
	}
}

uint64_t RenderExecutor::PrepareBusyNs() const {
	uint64_t busy = 0;
	for (const auto& lane: m_prepare_lanes) {
		busy += lane->worker->BusyNs();
	}
	return busy;
}

void RenderExecutor::PrepareQueuedDraw(void* prepare_lane, uint64_t job) {
	auto& lane     = *static_cast<PrepareLane*>(prepare_lane);
	auto& self     = *lane.executor;
	const auto sequence =
	    self.m_prepare_pool ? job : (job - 1) * self.m_prepare_lanes.size() + lane.index + 1;
	auto& queued = *self.m_queued_draws[(sequence - 1) % self.m_queued_draws.size()];
	queued.log.Reset();
	DrawPrepare::SetLog(&queued.log);
	std::destroy_at(&queued.state);
	std::construct_at(&queued.state);
	self.m_context.GetPipelineCache().GetGraphicsPrograms(
	    *queued.registers, queued.user_config, queued.shaders, queued.state, queued.resources);
	DrawPrepare::SetLog(nullptr);
	self.SpeculateBuffers(queued, lane.stream_ring);
	self.SpeculateTextures(queued, lane.textures);
	self.SpeculateTargets(queued, lane.targets);
	self.ProposePipelineKey(queued);
}

// Prepare worker: the pipeline key GetGraphicsPipeline would build for the queued draw if the
// command thread finds the targets the worker proposed, with the topology and primitive restart
// the draw path will set from the registers.
void RenderExecutor::ProposePipelineKey(QueuedDraw& queued) {
	auto& proposal    = queued.pipeline_key;
	proposal.ready    = false;
	const auto& state = queued.state;
	const auto& proposed = queued.targets;
	if (!PipelineCache::WorkerPipelineKeysEnabled() || !proposed.ready || queued.log.failed ||
	    state.vertex_info[0].stage.module == nullptr) {
		return;
	}
	GraphicsPipelineTargets targets;
	if (!GetDrawTopology(queued.user_config, targets.topology)) {
		return;
	}
	if (queued.indexed) {
		uint32_t element_size = 0;
		switch (static_cast<Prospero::IndexType>(queued.index_args.index_type_and_size)) {
			case Prospero::IndexType::kIndex16: element_size = 2; break;
			case Prospero::IndexType::kIndex32: element_size = 4; break;
			case Prospero::IndexType::kIndex8: element_size = 1; break;
			default: return;
		}
		switch (PrimitiveRestartFromRegisters(queued.user_config, *queued.registers, element_size)) {
			case PrimitiveRestartMode::On: targets.primitive_restart_enable = 1; break;
			case PrimitiveRestartMode::Off:
			case PrimitiveRestartMode::Scan: break;
			case PrimitiveRestartMode::Unsupported: return;
		}
	}
	// Every proposed target is assumed found; GetGraphicsPipeline checks the final ones.
	for (uint32_t i = 0; i < proposed.count; i++) {
		if (proposed.color[i] == nullptr) {
			continue;
		}
		const auto& color = state.color_info[i];
		targets.colors[targets.color_count++] = {
		    .format         = color.desc.view_info.format,
		    .samples        = color.desc.info.samples,
		    .slot           = static_cast<uint8_t>(color.target_slot),
		    .export_mapping = color.export_mapping.packed,
		    .unorm_11_11_10 = static_cast<uint8_t>(color.desc.info.guest_format ==
		                                           Prospero::BufferFormat::k11_11_10UNorm),
		};
	}
	if (proposed.depth != nullptr &&
	    state.depth_info.desc.view_info.format != vk::Format::eUndefined) {
		targets.depth_format  = state.depth_info.desc.view_info.format;
		targets.depth_samples = state.depth_info.desc.info.samples;
	}
	// Attachments with another sample count than the pass are dropped (PrepareDrawRenderState).
	const auto pass_samples = targets.depth_format != vk::Format::eUndefined
	                              ? targets.depth_samples
	                              : targets.colors[0].samples;
	for (uint32_t i = 0; i < targets.color_count; i++) {
		if (targets.colors[i].samples != pass_samples) {
			return;
		}
	}
	m_context.GetPipelineCache().ProposeGraphicsPipelineKey(*queued.registers, queued.user_config,
	                                                        state, targets, proposal);
}

const std::vector<TextureProposal>*
RenderExecutor::AdoptedTextureProposals(const ShaderStageRuntime& runtime) const {
	const auto* adopted = m_adopted_textures;
	if (adopted == nullptr || !adopted->ready) {
		return nullptr;
	}
	for (uint32_t i = 0; i < adopted->stage_count; i++) {
		if (adopted->runtimes[i] == &runtime) {
			return &adopted->stages[i];
		}
	}
	return nullptr;
}

// Prepare worker: the draw's buffer ranges and the clean buffers holding them, as FindBuffers
// and AcquireVertexBuffers will ask for them (BufferCache::Speculation).
void RenderExecutor::SpeculateBuffers(QueuedDraw& queued, WorkerStreamRing* ring) {
	auto& buffers = queued.buffers;
	buffers.ready = false;
	buffers.speculation.ring = nullptr;
	auto& state   = queued.state;
	if (!BufferCache::WorkerBuffersEnabled() || queued.log.failed ||
	    state.vertex_info[0].stage.module == nullptr) {
		return;
	}
	auto& cache = m_context.GetBufferCache();
	cache.BeginSpeculation(buffers.speculation);
	if (ring != nullptr) {
		ring->BeginDraw(queued.sequence);
		buffers.speculation.ring = ring;
	}
	uint32_t count = 0;
	for (uint32_t i = 0; i < state.vertex_stage_count; i++) {
		buffers.runtimes[count++] = &state.vertex_info[i].stage;
	}
	if (state.ps_active) {
		buffers.runtimes[count++] = &state.ps_input_info.stage;
	}
	buffers.stage_count = count;
	std::array<std::vector<PreparedBindings::BufferSource>*, 4> outs {};
	for (uint32_t i = 0; i < count; i++) {
		outs[i] = &buffers.sources[i];
	}
	struct Context {
		BufferCache&              cache;
		BufferCache::Speculation& speculation;
	} context {cache, buffers.speculation};
	DecodeBufferSources(
	    m_context.GetGraphics(), std::span {buffers.runtimes.data(), count},
	    std::span {outs.data(), count},
	    [](void* data, uint64_t address, uint64_t size, uint32_t& speculated) {
		    auto& c    = *static_cast<Context*>(data);
		    speculated = c.cache.SpeculateLookup(c.speculation, address, size);
		    return speculated != UINT32_MAX ? c.speculation.ranges[speculated].id : BufferId {};
	    },
	    &context);
	buffers.vertex_count = 0;
	if (state.vertex_info[0].stage.module->info.stage != ShaderType::Mesh) {
		buffers.vertex_ranges = MergeVertexRanges(state.vertex_info[0]);
		const auto& merged    = buffers.vertex_ranges;
		for (uint32_t i = 0; i < merged.count; i++) {
			const auto& range = merged.ranges[i];
			buffers.vertex_speculated[i] = cache.SpeculateLookup(
			    buffers.speculation, range.base_address, range.acquired_end - range.base_address);
		}
		buffers.vertex_count = merged.count;
	}
	// The index range as DrawIndex reads it (8-bit indices are expanded on the command thread).
	buffers.index_speculated = UINT32_MAX;
	if (queued.indexed && queued.index_args.index_addr != nullptr) {
		const auto type = static_cast<Prospero::IndexType>(queued.index_args.index_type_and_size);
		const uint64_t element = type == Prospero::IndexType::kIndex16   ? 2
		                         : type == Prospero::IndexType::kIndex32 ? 4
		                                                                 : 0;
		if (element != 0 && queued.index_args.index_count != 0) {
			buffers.index_speculated = cache.SpeculateLookup(
			    buffers.speculation, reinterpret_cast<uint64_t>(queued.index_args.index_addr),
			    uint64_t {queued.index_args.index_count} * element);
		}
	}
	// The adoption checks stream copies against GPU writes and emulator stores like the
	// preparation's other reads.
	for (const auto& range: buffers.speculation.ranges) {
		if (range.stream) {
			queued.log.reads.emplace_back(range.vaddr, range.size);
		}
	}
	buffers.ready = true;
}

void RenderExecutor::FinishQueuedDraw(CommandBuffer& buffer) {
	KYTY_PROFILER_FUNCTION();
	const auto sequence = m_queued_posted - m_queued_count + 1;
	auto&      queued   = *m_queued_draws[(sequence - 1) % m_queued_draws.size()];
	EXIT_IF(m_queued_count == 0 || queued.sequence != sequence);
	m_queued_count--;
	if (const auto waited = m_prepare_pool ? m_prepare_pool->Wait(sequence)
	                                       : PrepareWorker(sequence).Wait(PrepareJob(sequence));
	    waited != 0) {
		m_prepare_stat.wait_ns += waited;
		m_prepare_stat.waits++;
		using Common::BenchMetrics::Counter;
		Common::BenchMetrics::Add(m_draining                  ? Counter::DrainWaitNs
		                          : sequence <= m_refill_until ? Counter::RefillWaitNs
		                                                       : Counter::SteadyWaitNs,
		                          waited);
		Common::BenchMetrics::Add(Counter::PrepareWaits);
		if (m_drain_stats) {
			if (m_draining) {
				m_drain_stat.drain_wait[m_drain_reason] += waited;
			} else if (sequence <= m_refill_until) {
				m_drain_stat.refill_wait[m_drain_reason] += waited;
				m_drain_stat.refill_wait_asleep += m_refill_asleep ? waited : 0;
			} else {
				m_drain_stat.steady_wait += waited;
			}
		}
	}
	if (m_drain_stats) {
		PrintDrainStats();
	}
	// The draw reads the registers it was queued with.
	auto& registers   = *buffer.m_registers;
	auto& user_config = *buffer.m_user_config;
	auto& shaders     = *buffer.m_shaders;
	if (queued.registers_verify && !queued.registers->SameRegisters(*queued.registers_verify)) {
		EXIT("KYTY_REGISTER_SNAPSHOT=verify: a queued draw's register snapshot changed before its "
		     "commit\n");
	}
	buffer.Bind(*queued.registers, queued.user_config, queued.shaders);
	if (queued.indexed) {
		(void)DrawIndex(buffer, queued.index_args, &queued);
	} else {
		(void)DrawAuto(buffer, queued.auto_args, &queued);
	}
	m_adopted_buffers  = nullptr;
	m_adopted_textures = nullptr;
	m_adopted_targets  = nullptr;
	m_adopted_pipeline_key = nullptr;
	// The worker files these in its records when it next uses the slot.
	std::swap(queued.target_updates, m_target_updates);
	m_target_updates.clear();
	std::swap(queued.depth_updates, m_depth_updates);
	m_depth_updates.clear();
	// The worker files these in its memo when it next uses the slot.
	m_prepare_stat.texture_updates += m_texture_updates.size();
	std::swap(queued.texture_updates, m_texture_updates);
	m_texture_updates.clear();
	m_context.GetBufferCache().SetSpeculation(nullptr);
	buffer.Bind(registers, user_config, shaders);
	// The worker that prepared this draw (any, with free lanes) may reuse its stream copies once
	// the GPU passes the current tick. Only that worker's ring holds them.
	if (auto* ring = queued.buffers.speculation.ring; ring != nullptr) {
		ring->Retire(sequence, m_context.GetCommandScheduler().CurrentTick());
	}

	if (m_prepare_stats) {
		auto&      stat = m_prepare_stat;
		const auto now  = PrepareNowNs();
		if (stat.start_ns == 0) {
			stat.start_ns       = now;
			stat.worker_busy_ns = PrepareBusyNs();
		} else if (now - stat.start_ns >= 1'000'000'000ull) {
			const auto busy  = PrepareBusyNs();
			const auto span  = static_cast<double>(now - stat.start_ns);
			const auto draws = static_cast<double>(std::max<uint64_t>(stat.queued, 1));
			std::printf("PREPARE queued=%" PRIu64 " direct=%" PRIu64 " adopted=%" PRIu64
			            " failed=%" PRIu64 " hazard=%" PRIu64 " stored=%" PRIu64
			            " gpu_written=%" PRIu64 " verified=%" PRIu64 " wait_ns_per_draw=%.0f (waits=%" PRIu64
			            " drains=%" PRIu64 ") worker_ns_per_draw=%.0f"
			            " worker_busy=%.1f%% (workers=%zu) texture_resolves=%" PRIu64
			            " texture_refreshes=%" PRIu64 " binding_reuse=%.1f%%"
			            " hazard_gpu=%" PRIu64 " hazard_epoch=%" PRIu64
			            " kept_by_ranged=%" PRIu64 " kept_by_gpu_range=%" PRIu64 " walker_pages=%.2f pages_hist=%" PRIu64 "/%" PRIu64
			            "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
			            " register_snapshots=%.1f%%\n",
			            stat.queued, stat.direct, stat.adopted, stat.failed, stat.hazard,
			            stat.stored, stat.gpu_written, stat.verified, static_cast<double>(stat.wait_ns) / draws,
			            stat.waits, stat.drains,
			            static_cast<double>(busy - stat.worker_busy_ns) / draws,
			            100.0 * static_cast<double>(busy - stat.worker_busy_ns) / span /
			                static_cast<double>(m_prepare_lanes.size()),
			            m_prepare_lanes.size(), stat.texture_updates,
			            stat.texture_generation_refreshes,
			            100.0 * static_cast<double>(stat.binding_reuses) /
			                static_cast<double>(std::max<uint64_t>(stat.binding_sets, 1)),
			            stat.hazard_gpu, stat.hazard_epoch, stat.kept_by_ranged,
			            stat.kept_by_gpu_range,
			            static_cast<double>(stat.walker_pages) /
			                static_cast<double>(std::max<uint64_t>(stat.adopted, 1)),
			            stat.page_buckets[0], stat.page_buckets[1], stat.page_buckets[2],
			            stat.page_buckets[3], stat.page_buckets[4], stat.page_buckets[5],
			            stat.page_buckets[6], stat.page_buckets[7],
			            100.0 * static_cast<double>(stat.register_snapshots) / draws);
			stat                = {};
			stat.start_ns       = now;
			stat.worker_busy_ns = busy;
		}
	}
}

void RenderExecutor::PrintDrainStats() {
	auto&      stat = m_drain_stat;
	const auto now  = PrepareNowNs();
	if (stat.start_ns == 0) {
		stat.start_ns = now;
		return;
	}
	if (now - stat.start_ns < 1'000'000'000ull) {
		return;
	}
	const double s = static_cast<double>(now - stat.start_ns) / 1e9;
	std::vector<uint32_t> reasons;
	uint64_t              drain_total = 0, refill_total = 0, count_total = 0;
	for (uint32_t r = 0; r < DrainReasons; r++) {
		if (stat.count[r] != 0 || stat.drain_wait[r] != 0 || stat.refill_wait[r] != 0) {
			reasons.push_back(r);
			drain_total += stat.drain_wait[r];
			refill_total += stat.refill_wait[r];
			count_total += stat.count[r];
		}
	}
	std::ranges::sort(reasons, [&](uint32_t a, uint32_t b) {
		return stat.drain_wait[a] + stat.refill_wait[a] > stat.drain_wait[b] + stat.refill_wait[b];
	});
	std::printf("DRAIN total drains_s=%.0f drain_wait_us_s=%.0f refill_wait_us_s=%.0f "
	            "steady_wait_us_s=%.0f refill_posts_s=%.0f asleep_any_s=%.0f asleep_all_s=%.0f "
	            "refill_wait_asleep_us_s=%.0f\n",
	            count_total / s, drain_total / s / 1e3, refill_total / s / 1e3,
	            stat.steady_wait / s / 1e3, stat.refill_posts / s, stat.refill_asleep_any / s,
	            stat.refill_asleep_all / s, stat.refill_wait_asleep / s / 1e3);
	for (size_t i = 0; i < reasons.size() && i < 12; i++) {
		const auto r = reasons[i];
		std::printf("DRAIN reason=%u (%s0x%02x) drains_s=%.0f drain_wait_us_s=%.0f "
		            "refill_wait_us_s=%.0f\n",
		            r, r >= 512 ? "nop r=" : r >= 256 ? "site " : "op ", r & 0xffu,
		            stat.count[r] / s, stat.drain_wait[r] / s / 1e3, stat.refill_wait[r] / s / 1e3);
	}
	std::fflush(stdout);
	*m_drain_stat_storage = {};
	stat.start_ns         = now;
}

void RenderExecutor::AdoptPrograms(QueuedDraw& queued, DrawRenderState& state) {
	auto& pipelines = m_context.GetPipelineCache();
	auto& buffers   = m_context.GetBufferCache();
	bool  usable    = false;
	static const bool ranged_gpu = DrawPrepare::RangedGpuWritesEnabled();
	if (queued.log.failed) {
		m_prepare_stat.failed++;
	} else if (GuestSyncEpoch::CurrentUnranged() != queued.sync_epoch) {
		// The emulator stored into guest memory, unranged, while the worker read.
		m_prepare_stat.hazard++;
		m_prepare_stat.hazard_epoch++;
	} else if (ranged_gpu ? DrawPrepare::GpuWrittenSince(queued.gpu_write_sequence, queued.log)
	                      : DrawPrepare::HazardGeneration() != queued.hazard_generation) {
		// Bytes the worker read (or, unranged, any bytes) became GPU-written: the lookup here
		// would have synchronized them.
		m_prepare_stat.hazard++;
		m_prepare_stat.hazard_gpu++;
	} else if (DrawPrepare::StoredSince(queued.stored_sequence, queued.log)) {
		// A readback published bytes the worker read.
		m_prepare_stat.stored++;
	} else if (std::ranges::any_of(queued.log.reads, [&](const auto& read) {
		           return buffers.HasGpuDirtyBytes(read.first, read.second);
	           })) {
		// Here those reads would have waited for the GPU.
		m_prepare_stat.gpu_written++;
	} else {
		usable = true;
	}
	if (!usable) {
		Common::BenchMetrics::Add(Common::BenchMetrics::Counter::Rejected);
		// The command thread may not look programs up while the worker does.
		WaitPrepareIdle();
		ResetProgramState(state);
		pipelines.GetGraphicsPrograms(*queued.registers, queued.user_config, queued.shaders, state,
		                              queued.resources);
		return;
	}
	m_prepare_stat.adopted++;
	if (m_prepare_stats) {
		if (DrawPrepare::StoredSequence() != queued.stored_sequence) {
			m_prepare_stat.kept_by_ranged++;
		}
		if (DrawPrepare::HazardGeneration() != queued.hazard_generation) {
			m_prepare_stat.kept_by_gpu_range++;
		}
		auto pages = queued.log.pages;
		std::ranges::sort(pages);
		const auto distinct = static_cast<uint64_t>(std::unique(pages.begin(), pages.end()) - pages.begin());
		m_prepare_stat.walker_pages += distinct;
		m_prepare_stat.page_buckets[distinct == 0 ? 0 : std::min<uint64_t>(std::bit_width(distinct), 7)]++;
	}
	if (queued.buffers.ready) {
		// FindBuffers and ObtainBuffer use the worker's buffer side while it holds.
		m_adopted_buffers = &queued.buffers;
		buffers.SetSpeculation(&queued.buffers.speculation);
	}
	if (queued.textures.ready) {
		m_adopted_textures = &queued.textures;
	}
	if (queued.targets.ready) {
		m_adopted_targets = &queued.targets;
	}
	if (queued.pipeline_key.ready) {
		m_adopted_pipeline_key = &queued.pipeline_key;
		// Worker-written: fetch it while the targets and buffers are resolved.
		const auto* bytes = reinterpret_cast<const char*>(m_adopted_pipeline_key);
		for (size_t offset = 0; offset < sizeof(WorkerPipelineKey); offset += 64) {
			__builtin_prefetch(bytes + offset);
		}
	}
	if (m_prepare_verify) {
		WaitPrepareIdle();
		static auto check = std::make_unique<std::pair<DrawRenderState, PipelineCache::GraphicsResources>>();
		check->first = state;
		ResetProgramState(check->first);
		pipelines.GetGraphicsPrograms(*queued.registers, queued.user_config, queued.shaders,
		                              check->first, check->second);
		if (const auto diff = ComparePrograms(state, queued.resources, check->first, check->second);
		    !diff.empty()) {
			EXIT("KYTY_PREPARE_VERIFY: the worker's lookup differs (%s): vs=0x%016" PRIx64
			     " ps=0x%016" PRIx64 "\n",
			     diff.c_str(), queued.shaders.GetVs().es_regs.data_addr,
			     queued.shaders.GetPs().ps_regs.data_addr);
		}
		m_prepare_stat.verified++;
	}
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, 1, true, true);
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source,
	                    {src.desc.view_info.base_level, 1, src.desc.view_info.base_layer, 1},
	                    {dst.desc.view_info.base_level, 1, dst.desc.view_info.base_layer, 1});
	return true;
}

} // namespace Libs::Graphics
