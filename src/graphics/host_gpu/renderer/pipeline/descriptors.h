#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;
struct TextureProposal;

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
	// The WorkerTextureRecord whose desc this holds, 0 if none: assigning that record again skips
	// copying its ~1 KB desc. Every other assignment replaces the whole binding, resetting it.
	uint64_t                   source = 0;
};

// The part of a texture binding decided by its resource and descriptor alone.
struct TextureDescription {
	TextureCache::ImageDesc desc;
	ShaderTextureResource   descriptor;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
	vk::Format              sampled_view_format = vk::Format::eUndefined;
	bool                    shader_conversion = false;
	bool                    null              = false;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
		// The prepare worker's lookup of this range (BufferCache::Speculation), or none.
		uint32_t speculated = UINT32_MAX;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	std::vector<TextureBinding>           images;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
	// Stage binding-set reuse (KYTY_BINDING_REUSE=0 turns it off): the module, worker texture
	// record serials and sampler values images and samplers were last resolved from, at which
	// TextureCache::BindingEpoch and GcTick. A draw with the same ones keeps them.
	struct Reuse {
		bool                                               valid  = false;
		const void*                                        module = nullptr;
		uint64_t                                           epoch  = 0;
		uint64_t                                           tick   = 0;
		std::vector<uint64_t>                              serials;
		std::vector<ShaderRecompiler::IR::DescriptorValue> samplers;
	} reuse;
	// This draw kept the images, views and samplers of the last one (RebindImages keeps views).
	bool reused = false;
	// A full resolve of this draw that RebindImages may make reusable.
	bool reuse_candidate = false;
	// KYTY_RESIDENT_TEXTURES=shadow: what the classic binding of this stage did in this draw
	// (ResidentTextures checks it at commit).
	struct ResidentShadow {
		enum Flag : uint8_t {
			SlowAcquire  = 1u,  // the view lookup refreshed or locked (not the settled path)
			Barrier      = 2u,  // the commit's transition recorded a barrier
			StateChanged = 4u,  // or changed the tracked image state
			SkipGlobal   = 8u,  // predicted skippable by the global counters
			SkipImage    = 16u, // by the per-image versions
		};
		bool                                active   = false; // a draw stage with worker records
		bool                                resolved = false; // RebindImages resolved again
		uint64_t                            cycles   = 0;     // in code resident bindings delete
		const std::vector<TextureProposal>* proposals = nullptr;
		std::vector<uint8_t>                flags; // per image
	} resident;
	// Resident textures (IR::ImageResource::resident slots; ResidentStore): what this draw's
	// slots sample, decided in RebindImages and committed in CommitBindings.
	struct ResidentSlots {
		enum Kind : uint8_t {
			None     = 0, // not a resident slot
			Null     = 1, // no image: element 0
			Skip     = 2, // the record's element, no per-image work
			Acquired = 3, // acquired and transitioned here; a shared element
			Conflict = 4, // also a target or storage image of the draw: a one-draw element
		};
		enum Verify : uint8_t {
			Predicted   = 1, // KYTY_RESIDENT_TEXTURES=verify: a skip, done the classic way
			SlowAcquire = 2, // and the view lookup did not take the settled path
		};
		const std::vector<TextureProposal>* proposals = nullptr;
		std::vector<uint8_t>                kinds;    // per image
		std::vector<uint8_t>                verify;   // per image
		std::vector<uint32_t>               elements; // per image
		std::vector<uint16_t>               words;    // resident images, then samplers
		uint64_t                            layout_epoch   = 0; // read_layout_epoch at RebindImages
		bool                                samplers_valid = false;
	} resident_slots;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);

// The guest ranges of the stages' buffer bindings, by descriptor index: outs[i] for runtimes[i].
// lookup(context, address, size, speculated) names each range's buffer, or null, and may set
// the source's speculated index. Any thread.
using BufferLookup = BufferId (*)(void* context, uint64_t address, uint64_t size,
                                  uint32_t& speculated);
void DecodeBufferSources(const GraphicContext&                                     graphics,
                         std::span<const ShaderStageRuntime* const>                runtimes,
                         std::span<std::vector<PreparedBindings::BufferSource>* const> outs,
                         BufferLookup lookup, void* context);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
