#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTEXTURES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTEXTURES_H_

#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

// Texture resolves proposed by the draw prepare worker (Track B owns the content; plan item 3
// in PLAN-60fps.md). The command thread adopts a proposal only while what it depends on holds,
// and resolves as before otherwise.
struct TextureProposal {
	// Track B: kind, image id, binding, the registration generation it holds for, ...
};

// A resolve the command thread made for a queued draw, handed back to the worker's memo
// through the draw's slot (applied when the worker next runs a job on it).
struct TextureMemoRecord {
	// Track B
};

// The texture side of a queued draw, by stage in WorkerBuffers::runtimes order (vertex stages,
// then pixel), then by image index.
struct WorkerTextures {
	bool                                             ready       = false;
	uint32_t                                         stage_count = 0;
	std::array<const ShaderStageRuntime*, 4>         runtimes {};
	std::array<std::vector<TextureProposal>, 4>      stages;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTEXTURES_H_
