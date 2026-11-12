#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTSTORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTSTORE_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class RenderContext;
class TextureCache;
struct WorkerTextureRecord;

// KYTY_RESIDENT_TEXTURES=verify: resident bindings, with the classic per-image work still done and
// compared (see ResidentStore).
[[nodiscard]] bool ResidentTexturesVerify();

// Resident textures, stage 1b (RESEARCH-thin-commit.md; GraphicContext::resident_textures). Draw
// shaders sample their eligible images (IR::ImageResource::resident) and samplers from the image
// heap's set by 16-bit index words in shader data. This store owns those elements:
// - An element samples one view of one image in its read layout. Elements are shared per (image
//   serial, view) and come from ImageHeap's allocator, so a retired element is reused only once
//   the scheduler tick it was retired in is free. An element unused for AgeTicks texture-cache
//   GC ticks, or whose image is no longer registered, retires at the next GC tick.
// - A conflict element (a sampled image that is also a target or storage image of the draw)
//   lives for one draw, in the layout the classic commit chose.
// - An entry per worker texture record (WorkerTextureRecord::resident) remembers the element its
//   last resolve used and whether a later draw could sample it with no per-image work: the
//   binding epoch, the GC tick, Image::unsettle_generation and Image::read_layout_epoch unchanged
//   (or, per image, Image::residency_version), and the image settled and in its read state then.
// - Samplers get indices that never change (the sampler cache never evicts).
// Command thread only.
class ResidentStore final {
public:
	using Counters = ResidentCounters;
	struct Skip {
		uint32_t      element = 0;
		vk::ImageView view    = nullptr;
		bool          global  = false; // by the global counters, not the per-image version
		uint32_t      version = 0;     // the image's residency_version when it was remembered
	};
	struct Stats {
		// stages_spilled: shader data in a buffer only because of the index words (the push
		// data cursor estimated from the draw's earlier stages).
		uint64_t draws = 0, stages = 0, stages_push = 0, stages_buffer = 0, stages_spilled = 0;
		uint64_t sets = 0, fast_sets = 0;
		uint64_t slots = 0, null_slots = 0, skip_global = 0, skip_image = 0, acquired = 0,
		         conflicts = 0, late_transits = 0, verify_races = 0;
		uint64_t allocations = 0, retirements = 0, conflict_elements = 0, gc_ticks = 0;
		std::array<uint64_t, 6> words {}; // stages by resident words: 0, 1-2, 3-4, 5-8, 9-16, 17+
		size_t   live = 0, live_peak = 0;
	};

	explicit ResidentStore(RenderContext& context);
	~ResidentStore();
	KYTY_CLASS_NO_COPY(ResidentStore);

	[[nodiscard]] Counters Snapshot() const;
	// Whether a record's slot can sample its remembered element with no per-image work.
	[[nodiscard]] bool TrySkip(const WorkerTextureRecord& record, ImageId id, const Counters& now,
	                           TextureCache& cache, Skip& out);
	// The element sampling `view` of `image` in its read layout; `record`'s, if it is that one.
	[[nodiscard]] uint32_t Element(const WorkerTextureRecord* record, Image& image, ImageId id,
	                               vk::ImageView view);
	// A one-draw element for a conflict slot; SetConflict names its view and layout at commit.
	[[nodiscard]] uint32_t ConflictElement();
	void                   SetConflict(uint32_t element, vk::ImageView view, vk::ImageLayout layout);
	[[nodiscard]] uint16_t Sampler(vk::Sampler sampler);
	// After the commit's transitions, with the counters taken before (Snapshot): what a later draw
	// of this record can skip. Returns whether it can.
	bool Remember(const WorkerTextureRecord& record, Image& image, ImageId id, uint32_t element,
	              bool read_state, const Counters& counters);
	// After a draw's commit: writes new descriptors, retires its conflict elements, ages.
	void EndDraw() {
		if (m_dirty || !m_draw_conflicts.empty()) {
			FlushDraw();
		}
		m_stats.draws++;
		if (const auto tick = m_context_tick(); tick != m_last_tick) {
			Age(tick);
		}
		if ((m_stats.draws & 1023u) == 0u) {
			Print();
		}
	}
	// Verify mode: whether `element` samples `view` of `image` (registered) in `layout`.
	[[nodiscard]] const char* CheckElement(uint32_t element, const Image& image, ImageId id,
	                                       vk::ImageView view, vk::ImageLayout layout) const;
	Stats& GetStats() { return m_stats; }
	[[nodiscard]] bool ElementLive(uint32_t element) const {
		return element < m_elements.size() && m_elements[element].live;
	}
	[[nodiscard]] vk::ImageView   ElementView(uint32_t element) const { return m_elements.at(element).key.view; }
	[[nodiscard]] vk::ImageLayout ElementLayout(uint32_t element) const { return m_elements.at(element).layout; }
	// Verify mode, around one transition: whether it changed the tracked image state.
	void               SaveState(const Image& image);
	[[nodiscard]] bool StateChanged(const Image& image) const;
	[[nodiscard]] const VulkanImageState& SavedState() const { return m_saved_state; }
	[[nodiscard]] size_t SavedSubresources() const { return m_saved_subresources.size(); }

private:
	static constexpr uint64_t AgeTicks = 8;

	struct ViewKey {
		uint64_t      image_serial = 0;
		vk::ImageView view         = nullptr;
		bool operator==(const ViewKey&) const = default;
	};
	struct ViewKeyHash {
		size_t operator()(const ViewKey& key) const noexcept;
	};
	struct ElementState {
		ViewKey         key;
		ImageId         image_id;
		vk::ImageLayout layout       = vk::ImageLayout::eUndefined;
		uint32_t        generation   = 0;
		uint64_t        used_tick    = 0;
		uint64_t        retired_tick = 0; // the scheduler tick it was last retired in
		bool            live         = false;
		bool            conflict     = false;
		bool            retired      = false;
	};
	struct Entry {
		uint64_t record_serial = 0; // 0: free
		uint32_t element       = 0;
		uint32_t generation    = 0; // the element's
		ImageId  image_id;
		uint64_t image_serial = 0;
		bool     ok           = false;
		Counters counters;
		uint32_t version   = 0;
		uint64_t used_tick = 0;
	};

	uint32_t Allocate(vk::ImageView view, vk::ImageLayout layout);
	void     FlushDraw();
	uint64_t m_context_tick() const;
	void     Retire(uint32_t index);
	void     Age(uint64_t tick);
	void     Print();

	RenderContext&                                    m_context;
	std::vector<ElementState>                         m_elements; // by heap element index
	std::unordered_map<ViewKey, uint32_t, ViewKeyHash> m_views;
	std::vector<Entry>                                m_entries; // [0] unused
	std::vector<uint32_t>                             m_free_entries;
	std::unordered_map<VkSampler, uint16_t>           m_samplers;
	std::vector<uint32_t>                             m_draw_conflicts;
	VulkanImageState                                  m_saved_state;
	std::vector<VulkanImageState>                     m_saved_subresources;
	uint64_t                                          m_last_tick = UINT64_MAX;
	bool                                              m_dirty     = false; // descriptors to write
	uint64_t                                          m_print_ns  = 0;
	Stats                                             m_stats;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESIDENTSTORE_H_
