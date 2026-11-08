#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTARGETS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTARGETS_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// Color targets proposed by the draw prepare worker. The worker decodes each target the pixel
// program writes from the queued draw's registers (or reuses a record for the same registers),
// and writes it into the queued draw's DrawRenderState::color_info; the command thread keeps the
// texture cache's side: the registration check, binding, and finding the image of a target it has
// not resolved yet.
//
// A record: one slot's registers, and its target as decoded, or as the command thread last
// resolved it. Immutable once a job proposes it, but for `generation`, which only the command
// thread reads and refreshes; a replaced record lives until no queued draw can refer to it.
struct WorkerColorTargetRecord {
	HW::RenderTarget rt {}; // compared as bytes
	uint32_t         mask = 0;
	RenderColorInfo  info;
	// The command thread found info.image_id for info.desc (remapped by the texture cache).
	bool             resolved            = false;
	uint32_t         metadata_base_layer = 0;
	// TextureCache registration generation the image was last found at (command thread only).
	mutable uint64_t generation = 0;
};

// A queued draw's color targets, in the order PrepareDrawRenderState resolves them (the slots of
// the pixel program's mrt_mask, ascending); color_info holds the same number of entries.
struct WorkerTargets {
	bool     ready = false;
	uint32_t count = 0;
	// Null: the slot has no color output (color_info holds it unbound).
	std::array<const WorkerColorTargetRecord*, RENDER_COLOR_ATTACHMENTS_MAX> color {};
};

// One prepare worker's records: a few per slot, replaced round robin.
struct WorkerTargetMemo {
	static constexpr uint32_t Ways = 4;
	std::array<std::array<std::unique_ptr<WorkerColorTargetRecord>, Ways>,
	           RENDER_COLOR_ATTACHMENTS_MAX>
	                                                                   slots;
	std::array<uint32_t, RENDER_COLOR_ATTACHMENTS_MAX>                 next {};
	std::vector<std::vector<std::unique_ptr<WorkerColorTargetRecord>>> retired;
	uint64_t                                                           jobs = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTARGETS_H_
