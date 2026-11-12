#include "graphics/host_gpu/renderer/pipeline/residentStore.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/workerTextures.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

bool ResidentTexturesVerify() {
	static const bool verify = [] {
		const char* value = std::getenv("KYTY_RESIDENT_TEXTURES");
		return value != nullptr && std::strcmp(value, "verify") == 0;
	}();
	return verify;
}

size_t ResidentStore::ViewKeyHash::operator()(const ViewKey& key) const noexcept {
	const auto view = reinterpret_cast<uint64_t>(static_cast<VkImageView>(key.view));
	return static_cast<size_t>((key.image_serial * 0x9e3779b97f4a7c15ull) ^ view);
}

ResidentStore::ResidentStore(RenderContext& context): m_context(context) {
	m_entries.emplace_back();
}

ResidentStore::~ResidentStore() = default;

ResidentStore::Counters ResidentStore::Snapshot() const {
	auto& cache = m_context.GetTextureCache();
	// The unsettle count before any settled flag the caller reads (Image::StoreSettled).
	return {.binding_epoch = cache.BindingEpoch(),
	        .tick          = cache.GcTick(),
	        .unsettle      = Image::unsettle_generation.load(std::memory_order_seq_cst),
	        .layout        = Image::read_layout_epoch.load(std::memory_order_relaxed)};
}

bool ResidentStore::TrySkip(const WorkerTextureRecord& record, ImageId id, const Counters& now,
                            TextureCache& cache, Skip& out) {
	const auto index = record.resident;
	if (index == 0 || index >= m_entries.size()) {
		return false;
	}
	auto& entry = m_entries[index];
	if (entry.record_serial != record.serial || !entry.ok || entry.image_id != id ||
	    entry.counters.binding_epoch != now.binding_epoch || entry.counters.tick != now.tick) {
		return false;
	}
	auto& element = m_elements[entry.element];
	if (!element.live || element.generation != entry.generation) {
		return false;
	}
	out.global = entry.counters.unsettle == now.unsettle && entry.counters.layout == now.layout;
	if (!out.global) {
		// One image's counters: it was neither unsettled nor taken out of its read state.
		const auto& image = cache.GetImage(id);
		if (image.serial != entry.image_serial ||
		    image.residency_version.load(std::memory_order_seq_cst) != entry.version) {
			return false;
		}
	}
	element.used_tick = now.tick;
	entry.used_tick   = now.tick;
	out.element       = entry.element;
	out.view          = element.key.view;
	return true;
}

uint32_t ResidentStore::Allocate(vk::ImageView view, vk::ImageLayout layout) {
	auto&      heap  = m_context.GetImageHeap();
	const auto index = heap.AllocateResident(view, layout);
	if (index >= m_elements.size()) {
		m_elements.resize(std::max<size_t>(index + 1u, m_elements.size() * 2u));
	}
	auto& element = m_elements[index];
	// ImageHeap reuses an index only once the tick it was retired in is free.
	if (element.retired && !heap.ResidentElementFree(element.retired_tick)) {
		EXIT("KYTY_RESIDENT_TEXTURES: element %u reused before its retirement tick %" PRIu64
		     " is free\n",
		     index, element.retired_tick);
	}
	element.generation++;
	element.live     = true;
	element.retired  = false;
	element.conflict = false;
	element.layout   = layout;
	m_stats.allocations++;
	m_stats.live++;
	m_stats.live_peak = std::max(m_stats.live_peak, m_stats.live);
	return index;
}

uint32_t ResidentStore::Element(Image& image, ImageId id, vk::ImageView view) {
	const ViewKey key {image.serial, view};
	const auto    tick = m_context.GetTextureCache().GcTick();
	if (const auto found = m_views.find(key); found != m_views.end()) {
		m_elements[found->second].used_tick = tick;
		return found->second;
	}
	const auto index = Allocate(view, image.ReadLayout());
	auto&      element = m_elements[index];
	element.key        = key;
	element.image_id   = id;
	element.used_tick  = tick;
	m_views.emplace(key, index);
	image.resident_live++;
	return index;
}

uint32_t ResidentStore::ConflictElement() {
	const auto index = Allocate(nullptr, vk::ImageLayout::eUndefined);
	m_elements[index].conflict = true;
	m_draw_conflicts.push_back(index);
	m_stats.conflict_elements++;
	return index;
}

void ResidentStore::SetConflict(uint32_t element, vk::ImageView view, vk::ImageLayout layout) {
	EXIT_IF(!m_elements[element].conflict);
	m_elements[element].key.view = view;
	m_elements[element].layout   = layout;
	m_context.GetImageHeap().SetResident(element, view, layout);
}

uint16_t ResidentStore::Sampler(vk::Sampler sampler) {
	const auto [found, inserted] =
	    m_samplers.try_emplace(static_cast<VkSampler>(sampler), static_cast<uint16_t>(m_samplers.size()));
	if (inserted) {
		if (found->second >= ShaderRecompiler::IR::ResidentSamplerCapacity) {
			EXIT("KYTY_RESIDENT_TEXTURES: more than %u samplers\n",
			     ShaderRecompiler::IR::ResidentSamplerCapacity);
		}
		m_context.GetImageHeap().WriteResidentSampler(found->second, sampler);
	}
	return found->second;
}

void ResidentStore::Remember(const WorkerTextureRecord& record, Image& image, ImageId id,
                             uint32_t element, bool read_state) {
	auto index = record.resident;
	if (index == 0 || index >= m_entries.size() || m_entries[index].record_serial != record.serial) {
		if (m_free_entries.empty()) {
			index = static_cast<uint32_t>(m_entries.size());
			m_entries.emplace_back();
		} else {
			index = m_free_entries.back();
			m_free_entries.pop_back();
		}
		record.resident = index;
	}
	auto& entry         = m_entries[index];
	entry.record_serial = record.serial;
	entry.element       = element;
	entry.generation    = m_elements[element].generation;
	entry.image_id      = id;
	entry.image_serial  = image.serial;
	// The counters before the settled flag (Image::StoreSettled).
	entry.counters = Snapshot();
	entry.version  = image.residency_version.load(std::memory_order_seq_cst);
	entry.ok = read_state &&
	           (image.settled.load(std::memory_order_seq_cst) & Image::SettledRefresh) != 0 &&
	           image.registered && !image.depth_id && !image.binding.needs_rebind;
	entry.used_tick = entry.counters.tick;
}

void ResidentStore::Retire(uint32_t index) {
	auto& element = m_elements[index];
	EXIT_IF(!element.live);
	if (!element.conflict) {
		m_views.erase(element.key);
		auto& images = m_context.GetTextureCache();
		if (auto* image = images.m_slot_images.try_get(element.image_id);
		    image != nullptr && image->serial == element.key.image_serial && image->resident_live != 0) {
			image->resident_live--;
		}
	}
	m_context.GetImageHeap().RetireResident(index);
	element.live         = false;
	element.retired      = true;
	element.retired_tick = m_context.GetCommandScheduler().CurrentTick();
	element.generation++;
	m_stats.retirements++;
	m_stats.live--;
}

void ResidentStore::Age(uint64_t tick) {
	if (tick == m_last_tick) {
		return;
	}
	m_last_tick = tick;
	m_stats.gc_ticks++;
	auto& images = m_context.GetTextureCache();
	for (uint32_t index = 0; index < m_elements.size(); index++) {
		const auto& element = m_elements[index];
		if (!element.live || element.conflict) {
			continue;
		}
		// An image that left the cache has no draw naming its elements: the binding epoch moved.
		const auto* image = images.m_slot_images.try_get(element.image_id);
		if (element.used_tick + AgeTicks < tick || image == nullptr ||
		    image->serial != element.key.image_serial || !image->registered) {
			Retire(index);
		}
	}
	for (uint32_t index = 1; index < m_entries.size(); index++) {
		auto& entry = m_entries[index];
		if (entry.record_serial != 0 && entry.used_tick + AgeTicks < tick) {
			entry.record_serial = 0;
			m_free_entries.push_back(index);
		}
	}
}

void ResidentStore::EndDraw() {
	m_context.GetImageHeap().WriteResidentElements();
	for (const auto index: m_draw_conflicts) {
		Retire(index);
	}
	m_draw_conflicts.clear();
	m_stats.draws++;
	Age(m_context.GetTextureCache().GcTick());
	Print();
}

const char* ResidentStore::CheckElement(uint32_t element, const Image& image, ImageId id,
                                        vk::ImageView view, vk::ImageLayout layout) const {
	if (element >= m_elements.size() || !m_elements[element].live) {
		return "the element is not live";
	}
	const auto& entry = m_elements[element];
	if (!image.registered) {
		return "the image is not registered";
	}
	if (!entry.conflict && (entry.image_id != id || entry.key.image_serial != image.serial)) {
		return "the element samples another image";
	}
	if (entry.key.view != view) {
		return "the element samples another view";
	}
	if (entry.layout != layout) {
		return "the element's layout differs from the image's";
	}
	return nullptr;
}

void ResidentStore::SaveState(const Image& image) {
	m_saved_state = image.backing.state;
	m_saved_subresources.assign(image.backing.subresource_states.begin(),
	                            image.backing.subresource_states.end());
}

bool ResidentStore::StateChanged(const Image& image) const {
	const auto same = [](const VulkanImageState& a, const VulkanImageState& b) {
		return a.pl_stage == b.pl_stage && a.access_mask == b.access_mask && a.layout == b.layout &&
		       a.reads_visible == b.reads_visible;
	};
	return !same(m_saved_state, image.backing.state) ||
	       !std::ranges::equal(image.backing.subresource_states, m_saved_subresources, same);
}

void ResidentStore::Print() {
	const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch())
	                                           .count());
	if (m_print_ns == 0) {
		m_print_ns = now;
		return;
	}
	if (now - m_print_ns < 1'000'000'000ull) {
		return;
	}
	m_print_ns    = now;
	const auto& s = m_stats;
	std::printf("RESIDENT draws=%" PRIu64 " stages=%" PRIu64 " stages_push=%" PRIu64
	            " stages_buffer=%" PRIu64 " stages_spilled=%" PRIu64 " slots=%" PRIu64 " null=%" PRIu64
	            " skip_global=%" PRIu64 " skip_image=%" PRIu64 " acquired=%" PRIu64
	            " conflicts=%" PRIu64 " late_transits=%" PRIu64 " allocs=%" PRIu64
	            " retires=%" PRIu64 " conflict_elements=%" PRIu64 " gc_ticks=%" PRIu64
	            " live=%zu live_peak=%zu samplers=%zu words=%" PRIu64 ",%" PRIu64 ",%" PRIu64
	            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
	            s.draws, s.stages, s.stages_push, s.stages_buffer, s.stages_spilled, s.slots, s.null_slots,
	            s.skip_global, s.skip_image, s.acquired, s.conflicts, s.late_transits,
	            s.allocations, s.retirements, s.conflict_elements, s.gc_ticks, s.live, s.live_peak,
	            m_samplers.size(), s.words[0], s.words[1], s.words[2], s.words[3], s.words[4],
	            s.words[5]);
	std::fflush(stdout);
}

} // namespace Libs::Graphics
