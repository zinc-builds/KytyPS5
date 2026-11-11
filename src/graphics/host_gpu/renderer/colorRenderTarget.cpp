#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/drawStateCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace Libs::Graphics {

static bool DccAlphaOnMsb(const HW::ColorInfo& info) {
	switch (info.format) {
		case Prospero::ChannelLayout::k10_10_10_2:
		case Prospero::ChannelLayout::k10_10_10_2Float:
		case Prospero::ChannelLayout::k5_5_5_1: return true;
		case Prospero::ChannelLayout::k2_10_10_10:
		case Prospero::ChannelLayout::k1_5_5_5: return false;
		default: break;
	}
	const auto components =
	    Prospero::ResolveRenderTargetFormat(info.format, info.channel_type).components;
	if (components == 1) {
		return info.channel_order != Prospero::ChannelOrder::kStandard;
	}
	return components == 3 || info.channel_order == Prospero::ChannelOrder::kStandard ||
	       info.channel_order == Prospero::ChannelOrder::kAlt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool DecodeColorTarget(const HW::Context& hw, uint32_t rt_slot, uint32_t mask, RenderColorInfo& r) {
	const auto& rt = hw.GetRenderTarget(rt_slot);
	std::construct_at(&r);
	r.target_slot = rt_slot;

	if (rt.base.addr == 0 || mask == 0) {
		if (graphics_debug_dump_enabled()) {
			static std::atomic_uint log_count = 0;
			const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
			if (log_id < 128) {
				LOGF("RenderColorTarget: no color output slot=%" PRIu32 " base=0x%010" PRIx64
				     " slot_mask=0x%01" PRIx32 " target_mask=0x%08" PRIx32 "\n",
				     rt_slot, rt.base.addr, mask, hw.GetRenderTargetMask());
			}
		}

		return false;
	}
	const auto samples = render_sample_count(rt.attrib.num_fragments);
	// TODO(PPSA24156): Replace this 2x MSAA approximation with proper 4-coverage/2-fragment EQAA.
	const bool approximate_eqaa = rt.attrib.num_samples == 2 && rt.attrib.num_fragments == 1;
	if (approximate_eqaa) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(
			    "Warning: approximating 4-coverage/2-fragment EQAA with 2x MSAA.\n");
		}
	}
	if (samples == 0 || (rt.attrib.num_samples != rt.attrib.num_fragments && !approximate_eqaa)) {
		EXIT("unsupported render-target sample configuration: samples=%u fragments=%u\n",
		     rt.attrib.num_samples, rt.attrib.num_fragments);
	}
	const uint32_t levels = rt.attrib2.num_mip_levels + 1u;
	if (rt.attrib3.dimension > 2u) {
		EXIT("unsupported render-target dimension: %u\n", rt.attrib3.dimension);
	}
	// 1D targets are 2D images one texel high.
	const bool is_1d  = rt.attrib3.dimension == 0u;
	const bool volume = rt.attrib3.dimension == 2u;
	if (is_1d && rt.attrib2.height != 0) {
		EXIT("1D render target has nonzero height: %u\n", rt.attrib2.height);
	}
	if (!volume && rt.attrib3.depth != 0) {
		EXIT("non-3D render target has nonzero depth: %u\n", rt.attrib3.depth);
	}
	if (is_1d && samples != 1) {
		EXIT("multisampled 1D render targets are unsupported\n");
	}
	if (volume && samples != 1) {
		EXIT("multisampled 3D render targets are unsupported\n");
	}
	const uint32_t depth = volume ? rt.attrib3.depth + 1u : 1u;
	// For volumes, CB_COLOR_VIEW bounds exported slices; ATTRIB3 defines storage depth.
	// The host attachment contains only the selected slices that exist in this mip.
	const uint32_t last_layer = volume
	                                ? std::min(rt.view.last_array_slice_index,
	                                           std::max(depth >> rt.view.current_mip_level, 1u) - 1u)
	                                : rt.view.last_array_slice_index;
	if (rt.view.base_array_slice_index > last_layer) {
		EXIT("invalid render-target view: base=%u last=%u\n", rt.view.base_array_slice_index,
		     last_layer);
	}
	const auto view = ResolveTargetViewInfo(rt.view.base_array_slice_index, last_layer);
	if (graphics_debug_dump_enabled()) {
		static std::atomic_uint log_count = 0;
		const auto              log_id    = log_count.fetch_add(1, std::memory_order_relaxed);
		if (log_id < 128) {
			LOGF("RenderColorTarget: inspect slot=%" PRIu32 " base=0x%010" PRIx64
			     " mask=0x%01" PRIx32 " attrib2_width=%" PRIu32 " attrib2_height=%" PRIu32
			     " attrib3_tile=0x%08" PRIx32 " attrib3_dim=0x%08" PRIx32 " fmt=0x%08" PRIx32
			     " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32 "\n",
			     rt_slot, rt.base.addr, mask, rt.attrib2.width, rt.attrib2.height,
			     static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
			     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
			     static_cast<uint32_t>(rt.info.channel_order));
		}
	}

	// Color-control state selects the color-buffer operation and logical blend operation.
	// The normal copy operation is a regular color write, not an attachment clear.
	// Nonlinear clear values are still stored as normalized components.
	// Metadata clears are materialized during image discovery; render-pass loads preserve contents.
	uint32_t   width  = 0;
	uint32_t       height       = 0;
	bool       tile   = false;
	const bool     standard4    = rt.attrib3.tile_mode == Prospero::TileMode::kStandard4KB;
	const bool     standard64   = rt.attrib3.tile_mode == Prospero::TileMode::kStandard64KB;
	const bool     depth_tile   = rt.attrib3.tile_mode == Prospero::TileMode::kDepth;
	const bool     texture_tile = standard4 || standard64 || depth_tile;

	switch (rt.attrib3.tile_mode) {
		case Prospero::TileMode::kLinear:
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
		case Prospero::TileMode::kDepth:
		case Prospero::TileMode::kRenderTarget:
			tile = rt.attrib3.tile_mode != Prospero::TileMode::kLinear;
			break;
		default: EXIT("unknown tile mode: %u\n", static_cast<uint32_t>(rt.attrib3.tile_mode));
	}
	if (samples > 1 && (!tile || levels != 1)) {
		EXIT("multisampled render targets require a single-mip tiled surface\n");
	}
	if (texture_tile && samples != 1) {
		EXIT("texture-tiled color render targets do not support multisampling\n");
	}
	if (volume && !tile) {
		EXIT("linear 3D render targets are unsupported\n");
	}

	width  = rt.attrib2.width + 1;
	height = rt.attrib2.height + 1;
	const auto target_format =
	    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	// A surface with one stored level starts every MIP_LEVEL at the base address, so a level with
	// mip 0's padded pitch aliases mip 0's top-left texels.
	uint32_t mip = rt.view.current_mip_level;
	if (mip != 0 && levels == 1 && samples == 1 && !volume &&
	    TileGetTexturePitch(target_format.guest_format, std::max(width >> mip, 1u),
	                        rt.attrib3.tile_mode) ==
	        TileGetTexturePitch(target_format.guest_format, width, rt.attrib3.tile_mode)) {
		mip = 0;
	}
	if (mip >= levels) {
		EXIT("unsupported render-target mip range: current=%u levels=%u\n",
		     rt.view.current_mip_level, levels);
	}
	const auto bytes_per_element = target_format.bytes_per_element;
	if (bytes_per_element == 0) {
		EXIT("render-target format has no valid element size\n");
	}
	const auto transfer_format = ImageOps::RenderTargetTransferFormat(bytes_per_element);
	TileTextureBlockLayout texture_tile_layout {};
	if (texture_tile &&
	    (!TileGetTextureBlockLayout(transfer_format, rt.attrib3.tile_mode, volume,
	                                texture_tile_layout) ||
	     (rt.base.addr & (texture_tile_layout.block.block_size - 1u)) != 0 ||
	     rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable ||
	     rt.info.fmask_one_frag_mode || rt.info.cmask_fast_clear_enable ||
	     rt.info.dcc_compression_enable || rt.cmask.addr != 0 || rt.fmask.addr != 0 ||
	     rt.dcc_addr.addr != 0 || rt.dcc.data_write_on_dcc_clear_to_reg)) {
		EXIT("unsupported texture-tiled render target: addr=0x%016" PRIx64 " tile=%u"
		     " dimension=%u depth=%u levels=%u layer=%u/%u samples=%u fragments=%u bpe=%u"
		     " cmask=0x%016" PRIx64 " fmask=0x%016" PRIx64 " dcc=0x%016" PRIx64 "\n",
		     rt.base.addr, static_cast<uint32_t>(rt.attrib3.tile_mode), rt.attrib3.dimension,
		     rt.attrib3.depth, levels, view.base_layer, view.image_layers, rt.attrib.num_samples,
		     rt.attrib.num_fragments, bytes_per_element, rt.cmask.addr, rt.fmask.addr,
		     rt.dcc_addr.addr);
	}
	if ((standard64 || depth_tile) &&
	    (rt.attrib3.dimension != 1 || rt.attrib3.depth != 0 ||
	     (depth_tile && (view.base_layer != 0 || view.image_layers != 1)))) {
		EXIT("unsupported 64KB texture-tiled render-target view: dimension=%u depth=%u"
		     " layer=%u/%u\n",
		     rt.attrib3.dimension, rt.attrib3.depth, view.base_layer, view.image_layers);
	}
	auto& desc = r.desc;
	desc.type              = TextureCache::BindingType::RenderTarget;
	desc.info.data            = {rt.base.addr, 0};
	desc.info.pixel_format = target_format.format;
	desc.info.guest_format = target_format.guest_format;
	desc.info.type         = volume ? Prospero::ImageType::kColor3D : Prospero::ImageType::kColor2D;
	desc.info.extent       = {width, height, depth};
	desc.info.resources       = {levels, volume ? 1u : view.image_layers};
	desc.info.bytes_per_block = bytes_per_element;
	desc.info.samples         = samples;
	desc.info.tile_mode       = rt.attrib3.tile_mode;
	if (!volume && rt.attrib3.tile_mode == Prospero::TileMode::kRenderTarget &&
	    levels > std::bit_width(std::max(width, height))) {
		EXIT("unsupported render-target mip count\n");
	}
	desc.info.UpdateSize();
	if (!desc.info.data.Valid()) {
		EXIT("render-target backing range is invalid\n");
	}
	const bool has_dcc        = rt.info.dcc_compression_enable && rt.dcc_addr.addr != 0;
	const bool has_cmask      = !rt.info.dcc_compression_enable && rt.info.cmask_fast_clear_enable &&
	                            rt.cmask.addr != 0 &&
	                            !rt.attrib3.write_vrs_rate_hint_to_cmask;
	if (has_dcc || has_cmask) {
		TileSizeAlign metadata_size {};
		const auto layers = volume ? depth : view.image_layers;
		if (has_dcc) {
			(void)TileGetDccSize(width, height, layers, bytes_per_element, levels,
			                     rt.attrib3.tile_mode, metadata_size, rt.attrib.num_fragments);
			desc.info.metadata.dcc_alpha_msb = DccAlphaOnMsb(rt.info);
		} else {
			(void)TileGetCmaskSize(width, height, layers, levels, metadata_size);
		}
		// DCC owns the clear when both planes are enabled.
		desc.info.metadata.kind = has_dcc ? ImageMetadataKind::Dcc : ImageMetadataKind::Cmask;
		desc.info.metadata.range = {has_dcc ? rt.dcc_addr.addr : rt.cmask.addr, metadata_size.size};
		desc.info.metadata.clear_word =
		    rt.clear_word0.word0 | (uint64_t {rt.clear_word1.word1} << 32u);
		desc.info.metadata.clear_register_valid = true;
	}
	desc.view_info.format = target_format.format;
	desc.view_info.type =
	    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
	desc.view_info.base_level  = mip;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eColorAttachment;
	r.guest_mip_level   = mip;
	r.guest_array_layer = view.base_layer;
	r.export_mapping    = target_format.export_mapping;
	return true;
}

void RenderExecutor::ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& r,
                                              uint32_t rt_slot, bool ignore_target_mask,
                                              bool exact_format) {
	KYTY_PROFILER_FUNCTION();
	const auto& hw = buffer.GetRegisters();

	const auto& rt      = hw.GetRenderTarget(rt_slot);
	auto        mask    = render_target_mask_slot(hw.GetRenderTargetMask(), rt_slot);
	if (ignore_target_mask && rt.base.addr != 0 && mask == 0) {
		mask = 0x0f;
	}

	auto* memo = GetDrawStateCacheMode() != DrawStateCacheMode::Off &&
	                     rt_slot < m_color_target_memo.size()
	                 ? &m_color_target_memo[rt_slot]
	                 : nullptr;
	bool hit = false;
	if (memo != nullptr && memo->generation != 0 && memo->mask == mask &&
	    memo->ignore_target_mask == ignore_target_mask && memo->exact_format == exact_format &&
	    std::memcmp(&memo->rt, &rt, sizeof(rt)) == 0) {
		const auto& found      = memo->result.desc;
		const bool  color_clear = found.info.metadata.kind == ImageMetadataKind::Dcc ||
		                         found.info.metadata.kind == ImageMetadataKind::Cmask;
		hit = m_context.GetTextureCache().RefindExactImage(
		    memo->result.image_id, found.info.data, color_clear ? &found : nullptr,
		    memo->metadata_base_layer, memo->generation);
		if (DrawStateCacheStats()) {
			m_color_target_stats.lookups++;
			m_color_target_stats.hits += hit ? 1 : 0;
		}
		if (hit && GetDrawStateCacheMode() != DrawStateCacheMode::Verify) {
			// r may be unconstructed (DrawRenderState::color_info).
			std::construct_at(&r, memo->result);
			BindRenderTarget(r.image_id);
			return;
		}
	} else if (memo != nullptr && DrawStateCacheStats()) {
		m_color_target_stats.lookups++;
	}
	if (!DecodeColorTarget(hw, rt_slot, mask, r)) {
		return;
	}
	auto&      texture_cache       = m_context.GetTextureCache();
	const auto metadata_base_layer = r.desc.view_info.base_layer;
	uint64_t   generation          = 0;
	r.image_id = texture_cache.FindImage(r.desc, exact_format, memo != nullptr ? &generation : nullptr);
	BindRenderTarget(r.image_id);
	if (memo == nullptr) {
		return;
	}
	if (hit && (r.image_id != memo->result.image_id ||
	            !(r.desc.view_info == memo->result.desc.view_info) ||
	            r.desc.info.data != memo->result.desc.info.data ||
	            r.guest_mip_level != memo->result.guest_mip_level ||
	            r.guest_array_layer != memo->result.guest_array_layer)) {
		EXIT("KYTY_DTC_VERIFY: color target memo disagrees: slot %u image %u vs %u\n", rt_slot,
		     memo->result.image_id.index, r.image_id.index);
	}
	memo->generation = r.image_id ? generation : 0;
	if (memo->generation != 0) {
		std::memcpy(&memo->rt, &rt, sizeof(rt)); // With padding: compared as bytes.
		memo->mask                = mask;
		memo->ignore_target_mask  = ignore_target_mask;
		memo->exact_format        = exact_format;
		memo->metadata_base_layer = metadata_base_layer;
		memo->result              = r;
	}
}


// KYTY_WORKER_TARGETS=0 turns the worker's color target proposals off.
static bool WorkerTargetsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WORKER_TARGETS");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// KYTY_WORKER_TARGETS_VERIFY=1: every adopted target is resolved again from the registers.
static bool WorkerTargetsVerify() {
	static const bool verify = std::getenv("KYTY_WORKER_TARGETS_VERIFY") != nullptr;
	return verify;
}

void RenderExecutor::SpeculateTargets(QueuedDraw& queued, WorkerTargetMemo& memo) {
	// A record replaced now may be proposed to queued draws until the ring comes round.
	const auto retire_slots = m_queued_draws.size() + 1;
	if (memo.retired.size() != retire_slots) {
		memo.retired.resize(retire_slots);
		memo.depth_retired.resize(retire_slots);
	}
	const auto job           = memo.jobs++ % retire_slots;
	auto&      retired       = memo.retired[job];
	auto&      depth_retired = memo.depth_retired[job];
	retired.clear();
	depth_retired.clear();
	const auto file_depth = [&](std::unique_ptr<WorkerDepthTargetRecord> record) {
		auto* way     = &memo.depth[memo.depth_next];
		bool  matched = false;
		for (auto& candidate: memo.depth) {
			if (candidate != nullptr &&
			    std::memcmp(&candidate->z, &record->z, sizeof(record->z)) == 0) {
				way     = &candidate;
				matched = true;
				break;
			}
		}
		if (!matched) {
			memo.depth_next = (memo.depth_next + 1) % WorkerTargetMemo::Ways;
		}
		if (*way != nullptr) {
			depth_retired.push_back(std::move(*way));
		}
		*way = std::move(record);
		return way->get();
	};
	for (auto& update: queued.depth_updates) {
		file_depth(std::move(update));
	}
	queued.depth_updates.clear();
	const auto file = [&](std::unique_ptr<WorkerColorTargetRecord> record) {
		const auto slot = record->info.target_slot;
		auto&      ways = memo.slots[slot];
		auto*      way  = &ways[memo.next[slot]];
		for (auto& candidate: ways) {
			if (candidate != nullptr && candidate->mask == record->mask &&
			    std::memcmp(&candidate->rt, &record->rt, sizeof(record->rt)) == 0) {
				way = &candidate;
				break;
			}
		}
		if (way == &ways[memo.next[slot]]) {
			memo.next[slot] = (memo.next[slot] + 1) % WorkerTargetMemo::Ways;
		}
		if (*way != nullptr) {
			retired.push_back(std::move(*way));
		}
		*way = std::move(record);
		return way->get();
	};
	for (auto& update: queued.target_updates) {
		file(std::move(update));
	}
	queued.target_updates.clear();
	auto& targets = queued.targets;
	targets.ready = false;
	auto& state   = queued.state;
	if (!WorkerTargetsEnabled() || queued.log.failed ||
	    GetDrawStateCacheMode() != DrawStateCacheMode::On ||
	    state.vertex_info[0].stage.module == nullptr) {
		return;
	}
	const auto& hw    = queued.registers;
	uint32_t    count = 0;
	if (state.mrt_mask == 0) {
		state.color_info.Construct(0);
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if ((state.mrt_mask & (1u << slot)) == 0) {
			continue;
		}
		const auto& rt   = hw.GetRenderTarget(slot);
		const auto  mask = render_target_mask_slot(hw.GetRenderTargetMask(), slot);
		if (rt.base.addr == 0 || mask == 0) {
			state.color_info.Construct(count).target_slot = slot;
			targets.color[count++] = nullptr;
			continue;
		}
		const WorkerColorTargetRecord* found = nullptr;
		for (const auto& candidate: memo.slots[slot]) {
			if (candidate != nullptr && candidate->mask == mask &&
			    std::memcmp(&candidate->rt, &rt, sizeof(rt)) == 0) {
				found = candidate.get();
				break;
			}
		}
		if (found == nullptr) {
			auto record = std::make_unique<WorkerColorTargetRecord>();
			std::memcpy(&record->rt, &rt, sizeof(rt)); // With padding: compared as bytes.
			record->mask = mask;
			EXIT_IF(!DecodeColorTarget(hw, slot, mask, record->info));
			found = file(std::move(record));
		}
		state.color_info.Construct(count, found->info);
		targets.color[count++] = found;
	}
	targets.count = count;
	targets.depth = nullptr;
	if (auto& r = state.depth_info; DecodeDepthTargetState(hw, r)) {
		const auto&                    z     = hw.GetDepthRenderTarget();
		const WorkerDepthTargetRecord* found = nullptr;
		for (const auto& candidate: memo.depth) {
			if (candidate != nullptr && std::memcmp(&candidate->z, &z, sizeof(z)) == 0) {
				found = candidate.get();
				break;
			}
		}
		if (found == nullptr) {
			auto record = std::make_unique<WorkerDepthTargetRecord>();
			std::memcpy(&record->z, &z, sizeof(z)); // With padding: compared as bytes.
			record->desc = MakeDepthTargetDesc(m_context.GetGraphics(), z);
			found        = file_depth(std::move(record));
		}
		r.desc        = found->desc;
		r.image_id    = found->resolved ? found->image_id : ImageId {};
		targets.depth = found;
	}
	targets.ready = true;
}

void RenderExecutor::ResolveProposedDepthTarget(CommandBuffer&                 buffer,
                                                const WorkerDepthTargetRecord* record,
                                                RenderDepthInfo&               r) {
	if (record == nullptr) {
		return; // no depth or stencil attachment
	}
	auto& texture_cache = m_context.GetTextureCache();
	// A run keeps the previous draw's depth target (see ResolveProposedColorTarget).
	const bool run = record->resolved && RunTargetsValid() &&
	                 m_run.targets.depth_id == record->image_id && r.image_id == record->image_id;
	if (run && RunStateMode() != 2) {
		BindRenderTarget(r.image_id);
		return;
	}
	const auto run_id = record->image_id;
	if (record->resolved) {
		const auto& found       = record->desc;
		const bool  color_clear = found.info.metadata.kind == ImageMetadataKind::Dcc ||
		                         found.info.metadata.kind == ImageMetadataKind::Cmask;
		if (texture_cache.RefindExactImage(record->image_id, found.info.data,
		                                   color_clear ? &found : nullptr,
		                                   record->metadata_base_layer, record->generation)) {
			BindRenderTarget(r.image_id);
		} else {
			ResolveRenderDepthTarget(buffer, r);
			const auto& memo = m_depth_target_memo;
			if (r.image_id && memo.generation != 0 && memo.image_id == r.image_id) {
				auto update                 = std::make_unique<WorkerDepthTargetRecord>(*record);
				update->desc                = memo.desc;
				update->image_id            = memo.image_id;
				update->metadata_base_layer = memo.metadata_base_layer;
				update->generation          = memo.generation;
				m_depth_updates.push_back(std::move(update));
			}
		}
	} else {
		const auto metadata_base_layer = r.desc.view_info.base_layer;
		uint64_t   generation          = 0;
		r.image_id = texture_cache.FindImage(r.desc, false, &generation);
		BindRenderTarget(r.image_id);
		if (r.image_id && generation != 0) {
			auto update = std::make_unique<WorkerDepthTargetRecord>();
			std::memcpy(&update->z, &record->z, sizeof(record->z));
			update->desc                = r.desc;
			update->image_id            = r.image_id;
			update->resolved            = true;
			update->metadata_base_layer = metadata_base_layer;
			update->generation          = generation;
			m_depth_updates.push_back(std::move(update));
		}
	}
	if (run && r.image_id != run_id) {
		EXIT("KYTY_RUN_STATE=verify: the depth target of a run is image %u, not %u\n",
		     static_cast<uint32_t>(r.image_id.index), static_cast<uint32_t>(run_id.index));
	}
	if (WorkerTargetsVerify()) {
		const auto&     hw = buffer.GetRegisters();
		RenderDepthInfo check;
		EXIT_IF(!DecodeDepthTargetState(hw, check));
		check.desc     = MakeDepthTargetDesc(m_context.GetGraphics(), hw.GetDepthRenderTarget());
		check.image_id = texture_cache.FindImage(check.desc, false, nullptr);
		if (check.image_id != r.image_id || !(check.desc.view_info == r.desc.view_info) ||
		    check.desc.info.data != r.desc.info.data ||
		    check.desc.info.stencil != r.desc.info.stencil ||
		    check.depth_test_enable != r.depth_test_enable ||
		    check.depth_write_enable != r.depth_write_enable ||
		    check.depth_compare_op != r.depth_compare_op ||
		    check.depth_clear_enable != r.depth_clear_enable ||
		    check.stencil_test_enable != r.stencil_test_enable ||
		    check.stencil_clear_enable != r.stencil_clear_enable) {
			EXIT("KYTY_WORKER_TARGETS_VERIFY: depth proposal disagrees: image %u vs %u\n",
			     static_cast<uint32_t>(r.image_id.index),
			     static_cast<uint32_t>(check.image_id.index));
		}
	}
}

void RenderExecutor::ResolveProposedColorTarget(CommandBuffer&                 buffer,
                                                const WorkerColorTargetRecord* record,
                                                RenderColorInfo& r, uint32_t slot) {
	if (record == nullptr) {
		return; // no color output: r is unbound
	}
	auto& texture_cache = m_context.GetTextureCache();
	// A run keeps the previous draw's target: the same registers, so the same record contents,
	// and no image registered, unregistered or collected since (RunEpochs), so RefindExactImage
	// would find it again; its color clear would find nothing to do either (the memo's epochs).
	const bool run = record->resolved && RunTargetsValid() && slot < RENDER_COLOR_ATTACHMENTS_MAX &&
	                 m_run.targets.slot_ids[slot] == record->info.image_id &&
	                 r.image_id == record->info.image_id;
	if (run && RunStateMode() != 2) {
		BindRenderTarget(r.image_id);
		return;
	}
	const auto run_id = record->info.image_id;
	if (record->resolved) {
		const auto& found       = record->info.desc;
		const bool  color_clear = found.info.metadata.kind == ImageMetadataKind::Dcc ||
		                         found.info.metadata.kind == ImageMetadataKind::Cmask;
		if (texture_cache.RefindExactImage(record->info.image_id, found.info.data,
		                                   color_clear ? &found : nullptr,
		                                   record->metadata_base_layer, record->generation)) {
			BindRenderTarget(r.image_id);
		} else {
			// An image appeared over it, or it was removed: resolve from the registers, and hand
			// the result back.
			ResolveRenderColorTarget(buffer, r, slot);
			const auto& memo = m_color_target_memo[slot];
			if (r.image_id && memo.generation != 0 && memo.result.image_id == r.image_id) {
				auto update                 = std::make_unique<WorkerColorTargetRecord>(*record);
				update->info                = memo.result;
				update->metadata_base_layer = memo.metadata_base_layer;
				update->generation          = memo.generation;
				m_target_updates.push_back(std::move(update));
			}
		}
	} else {
		// r holds the worker's decode: find its image, and hand the result back.
		const auto metadata_base_layer = r.desc.view_info.base_layer;
		uint64_t   generation          = 0;
		r.image_id = texture_cache.FindImage(r.desc, false, &generation);
		BindRenderTarget(r.image_id);
		if (r.image_id && generation != 0) {
			auto update                 = std::make_unique<WorkerColorTargetRecord>();
			std::memcpy(&update->rt, &record->rt, sizeof(record->rt));
			update->mask                = record->mask;
			update->info                = r;
			update->resolved            = true;
			update->metadata_base_layer = metadata_base_layer;
			update->generation          = generation;
			m_target_updates.push_back(std::move(update));
		}
	}
	// (A stale worker record resolves again to the same image and is handed back refreshed; the
	// run path keeps the image and leaves the record as it is.)
	if (run && r.image_id != run_id) {
		EXIT("KYTY_RUN_STATE=verify: color target slot %u of a run is image %u, not %u\n", slot,
		     static_cast<uint32_t>(r.image_id.index), static_cast<uint32_t>(run_id.index));
	}
	if (WorkerTargetsVerify()) {
		const auto& hw   = buffer.GetRegisters();
		const auto  mask = render_target_mask_slot(hw.GetRenderTargetMask(), slot);
		RenderColorInfo check;
		EXIT_IF(!DecodeColorTarget(hw, slot, mask, check));
		check.image_id = texture_cache.FindImage(check.desc, false, nullptr);
		if (check.image_id != r.image_id || !(check.desc.view_info == r.desc.view_info) ||
		    check.desc.info.data != r.desc.info.data || check.target_slot != r.target_slot ||
		    check.guest_mip_level != r.guest_mip_level ||
		    check.guest_array_layer != r.guest_array_layer ||
		    check.export_mapping.packed != r.export_mapping.packed ||
		    check.desc.info.pixel_format != r.desc.info.pixel_format ||
		    check.desc.info.samples != r.desc.info.samples) {
			EXIT("KYTY_WORKER_TARGETS_VERIFY: proposal disagrees: slot %u image %u vs %u\n", slot,
			     static_cast<uint32_t>(r.image_id.index),
			     static_cast<uint32_t>(check.image_id.index));
		}
		static uint64_t checks = 0;
		if (++checks % 1000000 == 0) {
			std::printf("WORKER_TARGETS_VERIFY checks=%" PRIu64 "\n", checks);
			std::fflush(stdout);
		}
	}
}

} // namespace Libs::Graphics
