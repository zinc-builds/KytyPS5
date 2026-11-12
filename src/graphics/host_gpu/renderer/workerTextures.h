#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTEXTURES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WORKERTEXTURES_H_

#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

// Texture resolves proposed by the draw prepare worker (Track B owns the content; plan item 3
// in PLAN-60fps.md). The command thread adopts a proposal only while what it depends on holds,
// and resolves as before otherwise.
//
// A record of the workers' resolve memo: one resource's description of a T#, and the image the
// command thread last resolved it to. Immutable once a job proposes it; a replaced record lives
// until no queued draw can still refer to it.
struct WorkerTextureRecord {
	ShaderRecompiler::IR::ImageResource   inputs; // TextureDescriptionInputs
	ShaderRecompiler::IR::DescriptorValue value;
	TextureDescription                    description;
	// The resolve, if the command thread made one that can be reused (as TextureResolveMemo).
	bool                    resolved = false;
	bool                    null     = false;
	TextureBinding          binding; // without mip views
	ImageId                 found_id;
	GuestRange              data;
	// TextureCache registration generation. Once proposed, only the command thread reads it, and it
	// moves it forward when the registration check finds no image appeared over the record.
	mutable uint64_t        generation          = 0;
	uint64_t                serial              = 0; // unique per record (TextureBinding::source)
	uint64_t                key                 = 0; // TextureDescriptionKey(inputs, value)
	uint32_t                metadata_base_layer = 0;
	// KYTY_RESIDENT_TEXTURES=shadow: the command thread's ResidentTextures entry for this record,
	// 0 if none. Only the command thread reads or writes it, like generation.
	mutable uint32_t        resident            = 0;
	bool                    color_clear         = false;
	TextureCache::ImageDesc found; // with color_clear
	// KYTY_RESIDENT_TEXTURES=1 (ResidentStore::Remember, TrySkip): what a later draw of this record
	// may skip. Only the command thread reads or writes it; its own cache line, so the line the
	// skip check reads is neither shared with the worker's writes of the record nor elsewhere.
	struct alignas(64) ResidentSkip {
		ResidentCounters counters;          // when it was remembered
		uint64_t         record_serial = 0; // the record's serial then; 0: nothing remembered
		vk::ImageView    view          = nullptr;
		uint32_t         element       = 0;
		uint32_t         version       = 0; // the image's residency_version
		ImageId          image_id;
		bool             ok = false; // settled in its read state then
	};
	static_assert(sizeof(ResidentSkip) == 64);
	mutable ResidentSkip resident_skip;
};

struct TextureProposal {
	const WorkerTextureRecord* record = nullptr; // null: resolve on the command thread
	// Copies of the record's serial, and whether a hit runs its color-clear step (resolved, not
	// null, color_clear), so binding-set reuse need not read the record.
	uint64_t serial      = 0;
	bool     color_clear = false;
};
[[nodiscard]] inline TextureProposal ProposeRecord(const WorkerTextureRecord* record) {
	return {record, record->serial, record->resolved && !record->null && record->color_clear};
}

// A resolve the command thread made for a queued draw, handed back to the worker's memo
// through the draw's slot (applied when the worker next runs a job on it).
struct TextureMemoRecord {
	std::unique_ptr<WorkerTextureRecord> record;
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
