#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTTEXTURES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTTEXTURES_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <x86intrin.h>

namespace Libs::Graphics {

class Image;
class RenderContext;
struct WorkerTextureRecord;

// KYTY_RESIDENT_TEXTURES=shadow: stage 1a of resident textures (RESEARCH-thin-commit.md). Every
// texture a worker record resolves gets an element of the image heap's array (set 1), and every
// sampler a stable index, but nothing reads them: the classic per-draw bindings still do all
// the work. Per draw the commit predicts, from the binding epoch, the GC tick and two global
// counters (Image::unsettle_generation, Image::read_layout_epoch), which images a resident
// binding could skip, and then checks that the classic path did nothing for them: no refresh, no
// barrier, no state change, the element's view and layout. The first difference exits. It also
// prints the skip rates, the element and sampler counts and the time spent in the code a
// resident binding deletes (RESIDENT_SHADOW lines, cumulative). Command thread only.
[[nodiscard]] bool ResidentTexturesShadow();

class ResidentTextures final {
public:
	explicit ResidentTextures(RenderContext& context);
	~ResidentTextures();
	KYTY_CLASS_NO_COPY(ResidentTextures);

	[[nodiscard]] static uint64_t Now() { return __rdtsc(); }
	// Cycles since `start`, less the cost of the two Now() calls.
	[[nodiscard]] uint64_t Since(uint64_t start) const {
		const auto elapsed = Now() - start;
		return elapsed > m_overhead ? elapsed - m_overhead : 0;
	}

	// A draw's commit, before its transitions: marks the images a resident binding would skip.
	// `bound`: the draw's bound images, its targets among them.
	void Predict(std::span<PreparedBindings* const> stages, std::span<const ImageId> bound);
	// Around one image's transition in the commit, for images Predict marked.
	void SaveState(const Image& image);
	[[nodiscard]] bool StateChanged(const Image& image) const;
	// After the commit: checks the predictions, updates the elements and counts.
	void Check(std::span<PreparedBindings* const> stages);

private:
	struct Entry {
		uint64_t              record_serial = 0; // 0: free
		uint64_t              image_serial  = 0;
		ImageId               image_id;
		std::vector<uint32_t> elements; // one per view (dynamic-mip slots: one per mip)
		// The state at the last check: whether the next draw could sample the elements without
		// any work, and the counters then.
		bool     ok            = false;
		uint64_t unsettle      = 0;
		uint64_t layout_epoch  = 0;
		uint64_t binding_epoch = 0;
		uint64_t tick          = 0;
		uint32_t version       = 0;
		uint64_t used_tick     = 0; // ageing
	};

	struct ViewKey {
		uint64_t      image_serial = 0;
		vk::ImageView view         = nullptr;
		bool operator==(const ViewKey&) const = default;
	};
	struct ViewKeyHash {
		size_t operator()(const ViewKey& key) const noexcept;
	};
	struct Element {
		ViewKey  key;
		uint32_t refs = 0;
	};

	struct Stats {
		uint64_t draws = 0, draws_skip_global = 0, draws_skip_image = 0;
		uint64_t sets = 0, sets_records = 0, sets_images = 0, sets_eligible = 0,
		         sets_skip_global = 0, sets_skip_image = 0; // sets_skip_*: of sets with images
		uint64_t images = 0, images_eligible = 0, skip_global = 0, skip_image = 0, null_images = 0,
		         storage_images = 0, classic_images = 0, feedback_images = 0,
		         unsettled_images = 0, clear_images = 0, dynamic_mip_images = 0,
		         conflict_images = 0;
		uint64_t allocations = 0, retirements = 0, view_changes = 0, gc_ticks = 0;
		uint64_t cycles_total = 0, cycles_eligible = 0, cycles_skip_global = 0,
		         cycles_skip_image = 0;
		size_t   views_peak = 0, entries_peak = 0, entries = 0;
	};

	[[nodiscard]] Entry* FindEntry(const WorkerTextureRecord& record);
	Entry&               NewEntry(const WorkerTextureRecord& record);
	void                 FreeEntry(uint32_t index);
	void                 Bind(Entry& entry, const Image& image, std::span<const vk::ImageView> views);
	void                 Release(Entry& entry);
	void                 Age(uint64_t tick);
	void                 Print();

	RenderContext&                                    m_context;
	uint64_t                                          m_overhead = 0;
	std::vector<Entry>                                m_entries; // [0] unused
	std::vector<uint32_t>                             m_free_entries;
	std::unordered_map<ViewKey, uint32_t, ViewKeyHash> m_views; // to its element
	std::unordered_map<uint32_t, Element>             m_elements;
	std::unordered_map<VkSampler, uint32_t>           m_samplers; // stable sampler indices
	std::unordered_map<uint64_t, uint32_t>            m_image_refs; // image serial: elements
	std::vector<ImageId>                              m_conflicts;  // per draw
	std::vector<vk::ImageView>                        m_scratch_views;
	VulkanImageState                                  m_saved_state;
	std::vector<VulkanImageState>                     m_saved_subresources;
	uint64_t                                          m_last_tick  = UINT64_MAX;
	uint64_t                                          m_start_tsc  = 0;
	uint64_t                                          m_start_ns   = 0;
	uint64_t                                          m_print_tsc  = 0;
	Stats                                             m_stats;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTTEXTURES_H_
