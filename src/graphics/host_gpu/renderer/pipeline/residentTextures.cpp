#include "graphics/host_gpu/renderer/pipeline/residentTextures.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/imageHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/workerTextures.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {
namespace {

using Flag = PreparedBindings::ResidentShadow::Flag;

// An entry unused for this many texture-cache GC ticks gives up its elements.
constexpr uint64_t AgeTicks = 8;

uint64_t SteadyNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

bool SameState(const VulkanImageState& a, const VulkanImageState& b) {
	return a.pl_stage == b.pl_stage && a.access_mask == b.access_mask && a.layout == b.layout &&
	       a.reads_visible == b.reads_visible;
}

} // namespace

bool ResidentTexturesShadow() {
	static const bool shadow = [] {
		const char* value = std::getenv("KYTY_RESIDENT_TEXTURES");
		return value != nullptr && std::strcmp(value, "shadow") == 0;
	}();
	return shadow;
}

size_t ResidentTextures::ViewKeyHash::operator()(const ViewKey& key) const noexcept {
	const auto view = reinterpret_cast<uint64_t>(static_cast<VkImageView>(key.view));
	return static_cast<size_t>((key.image_serial * 0x9e3779b97f4a7c15ull) ^ view);
}

ResidentTextures::ResidentTextures(RenderContext& context): m_context(context) {
	m_entries.emplace_back();
	// The cost of a Now() pair, taken off every timed region.
	std::vector<uint64_t> samples(2001);
	for (auto& sample: samples) {
		const auto start = Now();
		sample           = Now() - start;
	}
	std::ranges::nth_element(samples, samples.begin() + samples.size() / 2);
	m_overhead  = samples[samples.size() / 2];
	m_start_tsc = m_print_tsc = Now();
	m_start_ns  = SteadyNs();
	std::printf("RESIDENT_SHADOW on: timer overhead %" PRIu64 " cycles\n", m_overhead);
}

ResidentTextures::~ResidentTextures() = default;

ResidentTextures::Entry* ResidentTextures::FindEntry(const WorkerTextureRecord& record) {
	const auto index = record.resident;
	if (index == 0 || index >= m_entries.size() || m_entries[index].record_serial != record.serial) {
		return nullptr;
	}
	return &m_entries[index];
}

ResidentTextures::Entry& ResidentTextures::NewEntry(const WorkerTextureRecord& record) {
	uint32_t index = 0;
	if (m_free_entries.empty()) {
		index = static_cast<uint32_t>(m_entries.size());
		m_entries.emplace_back();
	} else {
		index = m_free_entries.back();
		m_free_entries.pop_back();
	}
	record.resident       = index;
	auto& entry           = m_entries[index];
	entry                 = {};
	entry.record_serial   = record.serial;
	m_stats.entries++;
	m_stats.entries_peak = std::max(m_stats.entries_peak, m_stats.entries);
	return entry;
}

void ResidentTextures::FreeEntry(uint32_t index) {
	auto& entry = m_entries[index];
	Release(entry);
	entry.record_serial = 0;
	m_free_entries.push_back(index);
	m_stats.entries--;
}

// Points the entry at elements sampling `views` of `image`, sharing one element per view.
void ResidentTextures::Bind(Entry& entry, const Image& image,
                            std::span<const vk::ImageView> views) {
	Release(entry);
	auto& heap         = m_context.GetImageHeap();
	entry.image_serial = image.serial;
	for (const auto view: views) {
		const ViewKey key {image.serial, view};
		auto [found, inserted] = m_views.try_emplace(key, 0u);
		if (inserted) {
			found->second = heap.AllocateResident(view, image.ReadLayout());
			m_elements[found->second] = {key, 0u};
			m_image_refs[image.serial]++;
			m_stats.allocations++;
		}
		m_elements[found->second].refs++;
		entry.elements.push_back(found->second);
	}
	m_stats.views_peak = std::max(m_stats.views_peak, m_views.size());
}

void ResidentTextures::Release(Entry& entry) {
	auto& heap = m_context.GetImageHeap();
	for (const auto index: entry.elements) {
		auto element = m_elements.find(index);
		EXIT_IF(element == m_elements.end() || element->second.refs == 0);
		if (--element->second.refs != 0) {
			continue;
		}
		const auto serial = element->second.key.image_serial;
		m_views.erase(element->second.key);
		m_elements.erase(element);
		if (auto refs = m_image_refs.find(serial); refs != m_image_refs.end() && --refs->second == 0) {
			m_image_refs.erase(refs);
		}
		heap.RetireResident(index);
		m_stats.retirements++;
	}
	entry.elements.clear();
	entry.ok = false;
}

void ResidentTextures::Age(uint64_t tick) {
	if (tick == m_last_tick) {
		return;
	}
	m_last_tick = tick;
	m_stats.gc_ticks++;
	for (uint32_t index = 1; index < m_entries.size(); index++) {
		const auto& entry = m_entries[index];
		if (entry.record_serial != 0 && entry.used_tick + AgeTicks < tick) {
			FreeEntry(index);
		}
	}
}

void ResidentTextures::Predict(std::span<PreparedBindings* const> stages,
                               std::span<const ImageId>           bound) {
	auto&      cache    = m_context.GetTextureCache();
	const auto unsettle = Image::unsettle_generation.load(std::memory_order_relaxed);
	const auto layout   = Image::read_layout_epoch.load(std::memory_order_relaxed);
	const auto epoch    = cache.BindingEpoch();
	const auto tick     = cache.GcTick();
	// This draw's targets and storage images that resident elements sample: their slots keep
	// the classic binding (attachment or general layout), as a resident commit would check per
	// draw.
	m_conflicts.clear();
	for (const auto id: bound) {
		const auto& image = cache.GetImage(id);
		if ((image.binding.is_target || image.binding.shader_write) &&
		    m_image_refs.contains(image.serial)) {
			m_conflicts.push_back(id);
		}
	}
	for (auto* prepared: stages) {
		auto& shadow = prepared->resident;
		if (!shadow.active) {
			continue;
		}
		const auto& program = prepared->runtime->module->info;
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			if (program.info.images[i].heap) {
				continue;
			}
			const auto& binding = prepared->images[i];
			auto&       flags   = shadow.flags[i];
			if (!binding.image_id) {
				flags |= Flag::SkipGlobal | Flag::SkipImage;
				continue;
			}
			if (binding.desc.type == TextureCache::BindingType::Storage ||
			    i >= shadow.proposals->size()) {
				continue;
			}
			const auto* record = (*shadow.proposals)[i].record;
			if (record == nullptr || binding.source != record->serial) {
				continue;
			}
			const auto* entry = FindEntry(*record);
			if (entry == nullptr || !entry->ok || entry->image_id != binding.image_id ||
			    entry->binding_epoch != epoch || entry->tick != tick ||
			    std::ranges::find(m_conflicts, binding.image_id) != m_conflicts.end()) {
				continue;
			}
			if (entry->unsettle == unsettle && entry->layout_epoch == layout) {
				flags |= Flag::SkipGlobal;
			}
			const auto& image = cache.GetImage(binding.image_id);
			if (entry->image_serial == image.serial &&
			    entry->version == image.residency_version.load(std::memory_order_relaxed)) {
				flags |= Flag::SkipImage;
			}
		}
	}
}

void ResidentTextures::SaveState(const Image& image) {
	m_saved_state = image.backing.state;
	m_saved_subresources.assign(image.backing.subresource_states.begin(),
	                            image.backing.subresource_states.end());
}

bool ResidentTextures::StateChanged(const Image& image) const {
	const auto& now = image.backing.subresource_states;
	return !SameState(m_saved_state, image.backing.state) ||
	       now.size() != m_saved_subresources.size() ||
	       !std::ranges::equal(now, m_saved_subresources, SameState);
}

void ResidentTextures::Check(std::span<PreparedBindings* const> stages) {
	auto&      cache    = m_context.GetTextureCache();
	const auto unsettle = Image::unsettle_generation.load(std::memory_order_relaxed);
	const auto layout   = Image::read_layout_epoch.load(std::memory_order_relaxed);
	const auto epoch    = cache.BindingEpoch();
	const auto tick     = cache.GcTick();
	auto&      stats    = m_stats;
	stats.draws++;
	bool draw_global = true;
	bool draw_image  = true;
	for (auto* prepared: stages) {
		auto& shadow = prepared->resident;
		stats.sets++;
		stats.cycles_total += shadow.active ? shadow.cycles : 0;
		if (!shadow.active) {
			draw_global = draw_image = false;
			continue;
		}
		stats.sets_records++;
		const auto& program     = prepared->runtime->module->info;
		bool        eligible    = true;
		bool        skip_global = true;
		bool        skip_image  = true;
		bool        has_images  = false;
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& resource = program.info.images[i];
			if (resource.heap) {
				continue;
			}
			stats.images++;
			has_images = true;
			auto&       binding   = prepared->images[i];
			const auto  flags     = shadow.flags[i];
			const bool  predicted = (flags & (Flag::SkipGlobal | Flag::SkipImage)) != 0;
			if (!binding.image_id) {
				stats.null_images++;
				stats.skip_global++;
				stats.skip_image++;
				continue;
			}
			if (binding.desc.type == TextureCache::BindingType::Storage) {
				stats.storage_images++;
				eligible = skip_global = skip_image = false;
				continue;
			}
			const auto* record =
			    i < shadow.proposals->size() ? (*shadow.proposals)[i].record : nullptr;
			if (record == nullptr || binding.source != record->serial) {
				stats.classic_images++;
				eligible = skip_global = skip_image = false;
				continue;
			}
			stats.images_eligible++;
			if ((*shadow.proposals)[i].color_clear) {
				stats.clear_images++;
			}
			const bool dynamic_mip = resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic;
			stats.dynamic_mip_images += dynamic_mip ? 1 : 0;
			auto&      image       = cache.GetImage(binding.image_id);
			const bool read_branch =
			    !image.binding.is_target && !(image.binding.force_general && !image.info.IsDepth());
			m_scratch_views.clear();
			if (dynamic_mip) {
				m_scratch_views.assign(binding.mip_views.begin(), binding.mip_views.end());
			} else {
				m_scratch_views.push_back(binding.image_view);
			}
			auto*      entry      = FindEntry(*record);
			const bool same_image = entry != nullptr && entry->image_id == binding.image_id &&
			                        entry->image_serial == image.serial;
			bool       same_views = same_image && entry->elements.size() == m_scratch_views.size();
			for (size_t v = 0; same_views && v < m_scratch_views.size(); v++) {
				same_views = m_elements.at(entry->elements[v]).key.view == m_scratch_views[v];
			}
			// A view change keeps the binding epoch only if the view cache is wrong.
			if (same_image && !same_views && entry->binding_epoch == epoch) {
				EXIT("KYTY_RESIDENT_TEXTURES=shadow: stage %u slot %u: the view changed without a "
				     "binding epoch change (image 0x%016" PRIx64 ")\n",
				     static_cast<uint32_t>(program.stage), i, image.info.data.address);
			}
			if (predicted) {
				const char* what = nullptr;
				if (shadow.resolved) {
					what = "the stage was resolved again";
				} else if ((flags & Flag::SlowAcquire) != 0) {
					what = "the view lookup refreshed the image (not settled)";
				} else if ((flags & Flag::Barrier) != 0) {
					what = "the transition recorded a barrier";
				} else if ((flags & Flag::StateChanged) != 0) {
					what = "the transition changed the image state";
				} else if (!read_branch) {
					what = image.binding.is_target ? "the image is a target of the draw"
					                               : "the image is bound in general layout";
				} else if (binding.layout != image.ReadLayout()) {
					what = "the binding's layout is not the read layout";
				} else if (!same_views) {
					what = same_image ? "the view differs from the element's" : "the image changed";
				}
				if (what != nullptr) {
					EXIT("KYTY_RESIDENT_TEXTURES=shadow: stage %u slot %u predicted skippable "
					     "(global %d, per-image %d) but %s: image 0x%016" PRIx64 " id %u layout %u "
					     "(read %u) flags 0x%x\n",
					     static_cast<uint32_t>(program.stage), i,
					     (flags & Flag::SkipGlobal) != 0, (flags & Flag::SkipImage) != 0, what,
					     image.info.data.address, static_cast<uint32_t>(binding.image_id.index),
					     static_cast<uint32_t>(binding.layout),
					     static_cast<uint32_t>(image.ReadLayout()), static_cast<unsigned>(flags));
				}
			}
			stats.skip_global += (flags & Flag::SkipGlobal) != 0 ? 1 : 0;
			stats.skip_image += (flags & Flag::SkipImage) != 0 ? 1 : 0;
			skip_global = skip_global && (flags & Flag::SkipGlobal) != 0;
			skip_image  = skip_image && (flags & Flag::SkipImage) != 0;
			if (std::ranges::find(m_conflicts, binding.image_id) != m_conflicts.end()) {
				stats.conflict_images++;
			}
			// What a resident commit would have done: a first-seen record gets its elements, a
			// changed view new ones; then the entry records whether the next draw can sample
			// them without work.
			if (entry == nullptr) {
				entry = &NewEntry(*record);
			}
			entry->image_id = binding.image_id;
			if (!same_views) {
				if (!entry->elements.empty()) {
					stats.view_changes++;
				}
				Bind(*entry, image, m_scratch_views);
			}
			const bool settled =
			    (image.settled.load(std::memory_order_acquire) & Image::SettledRefresh) != 0 &&
			    image.registered && !image.depth_id && !image.binding.needs_rebind;
			if (!read_branch) {
				stats.feedback_images++;
			} else if (!settled) {
				stats.unsettled_images++;
			}
			entry->ok            = read_branch && settled && binding.layout == image.ReadLayout();
			entry->unsettle      = unsettle;
			entry->layout_epoch  = layout;
			entry->binding_epoch = epoch;
			entry->tick          = tick;
			entry->version       = image.residency_version.load(std::memory_order_relaxed);
			entry->used_tick     = tick;
		}
		for (const auto sampler: prepared->samplers) {
			m_samplers.try_emplace(static_cast<VkSampler>(sampler),
			                       static_cast<uint32_t>(m_samplers.size()));
		}
		skip_global = eligible && skip_global;
		skip_image  = eligible && skip_image;
		draw_global = draw_global && skip_global;
		draw_image  = draw_image && skip_image;
		stats.sets_images += has_images ? 1 : 0;
		stats.sets_eligible += eligible && has_images ? 1 : 0;
		stats.cycles_eligible += eligible ? shadow.cycles : 0;
		stats.sets_skip_global += skip_global && has_images ? 1 : 0;
		stats.sets_skip_image += skip_image && has_images ? 1 : 0;
		stats.cycles_skip_global += skip_global ? shadow.cycles : 0;
		stats.cycles_skip_image += skip_image ? shadow.cycles : 0;
	}
	stats.draws_skip_global += draw_global ? 1 : 0;
	stats.draws_skip_image += draw_image ? 1 : 0;
	m_context.GetImageHeap().WriteResidentElements();
	Age(tick);
	Print();
}

void ResidentTextures::Print() {
	const auto now = Now();
	if (now - m_print_tsc < 3'000'000'000ull) { // about a second
		return;
	}
	m_print_tsc         = now;
	const auto  ns      = SteadyNs() - m_start_ns;
	const auto  per_ns  = static_cast<double>(now - m_start_tsc) / static_cast<double>(ns);
	const auto& s       = m_stats;
	std::printf("RESIDENT_SHADOW draws=%" PRIu64 " draws_skip_global=%" PRIu64
	            " draws_skip_image=%" PRIu64 " sets=%" PRIu64 " sets_records=%" PRIu64
	            " sets_images=%" PRIu64 " sets_eligible=%" PRIu64 " sets_skip_global=%" PRIu64 " sets_skip_image=%" PRIu64
	            " images=%" PRIu64 " images_eligible=%" PRIu64 " skip_global=%" PRIu64
	            " skip_image=%" PRIu64 " null=%" PRIu64 " storage=%" PRIu64 " classic=%" PRIu64
	            " feedback=%" PRIu64 " unsettled=%" PRIu64 " clears=%" PRIu64
	            " dynamic_mip=%" PRIu64 " conflicts=%" PRIu64 " allocs=%" PRIu64
	            " retires=%" PRIu64 " view_changes=%" PRIu64 " gc_ticks=%" PRIu64
	            " views=%zu views_peak=%zu entries=%zu entries_peak=%zu samplers=%zu"
	            " cyc_total=%" PRIu64 " cyc_eligible=%" PRIu64 " cyc_skip_global=%" PRIu64
	            " cyc_skip_image=%" PRIu64 " tsc_per_ns=%.4f\n",
	            s.draws, s.draws_skip_global, s.draws_skip_image, s.sets, s.sets_records,
	            s.sets_images, s.sets_eligible, s.sets_skip_global,
	            s.sets_skip_image, s.images, s.images_eligible, s.skip_global, s.skip_image,
	            s.null_images, s.storage_images, s.classic_images, s.feedback_images,
	            s.unsettled_images, s.clear_images, s.dynamic_mip_images, s.conflict_images,
	            s.allocations, s.retirements, s.view_changes, s.gc_ticks, m_views.size(),
	            s.views_peak, s.entries, s.entries_peak, m_samplers.size(), s.cycles_total,
	            s.cycles_eligible, s.cycles_skip_global, s.cycles_skip_image, per_ns);
	std::fflush(stdout);
}

} // namespace Libs::Graphics
