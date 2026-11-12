#include "graphics/host_gpu/renderer/image/imageRecycler.h"

#include "common/gpuWaitDiag.h"
#include "common/hybridAffinity.h"

#include <vk_mem_alloc.h>

#include <cstdlib>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Libs::Graphics {

namespace {

bool EnvOn(const char* name) {
	const char* value = std::getenv(name);
	return value == nullptr || value[0] != '0';
}

} // namespace

ImageRecycler::ImageRecycler(GraphicContext& graphics): m_graphics(graphics) {
	m_recycle        = EnvOn("KYTY_IMAGE_RECYCLE");
	m_thread_destroy = EnvOn("KYTY_IMAGE_DESTROY_THREAD");
	if (m_thread_destroy) {
		m_thread = std::thread([this] { Run(); });
	}
}

ImageRecycler::~ImageRecycler() {
	Flush();
	if (m_thread.joinable()) {
		{
			std::lock_guard lock(m_doomed_mutex);
			m_stop = true;
		}
		m_doomed_cv.notify_one();
		m_thread.join();
	}
}

ImageRecycler::Key ImageRecycler::KeyOf(const VulkanImage& image) {
	return {image.format, image.image_type, image.extent,     image.layers,
	        image.mip_levels, image.samples, image.usage, image.flags};
}

bool ImageRecycler::Take(const vk::ImageCreateInfo& create, VulkanImage& image) {
	if (!m_recycle || create.tiling != vk::ImageTiling::eOptimal || create.pNext != nullptr ||
	    create.sharingMode != vk::SharingMode::eExclusive ||
	    create.initialLayout != vk::ImageLayout::eUndefined) {
		return false;
	}
	const Key key {create.format,      create.imageType, create.extent,
	               create.arrayLayers, create.mipLevels, static_cast<uint32_t>(create.samples),
	               create.usage,       create.flags};
	std::lock_guard lock(m_mutex);
	Evict(Common::GpuWaitDiag::NowNs());
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
		if (it->key == key) {
			Common::GpuWaitDiag::DriverScope driver_scope(Common::GpuWaitDiag::Driver::ImageRecycled);
			image.format     = key.format;
			image.image_type = key.type;
			image.extent     = key.extent;
			image.layers     = key.layers;
			image.mip_levels = key.mip_levels;
			image.samples    = key.samples;
			image.usage      = key.usage;
			image.flags      = key.flags;
			image.image      = it->image;
			image.allocation = it->allocation;
			image.state      = {.layout = vk::ImageLayout::eUndefined};
			image.subresource_states.clear();
			m_bytes -= it->bytes;
			m_entries.erase(std::next(it).base());
			return true;
		}
	}
	return false;
}

void ImageRecycler::Retire(std::vector<vk::ImageView>&& views, VulkanImage& image) {
	Common::GpuWaitDiag::DriverScope driver_scope(Common::GpuWaitDiag::Driver::ImageRetire);
	Doomed doomed;
	doomed.views = std::move(views);
	if (m_recycle && image.image != nullptr) {
		VmaAllocationInfo info {};
		vmaGetAllocationInfo(m_graphics.allocator, image.allocation, &info);
		Entry entry {KeyOf(image), std::exchange(image.image, nullptr),
		             std::exchange(image.allocation, nullptr), info.size,
		             Common::GpuWaitDiag::NowNs()};
		std::lock_guard lock(m_mutex);
		m_bytes += entry.bytes;
		m_entries.push_back(entry);
		Evict(entry.time_ns);
	} else {
		doomed.image      = std::exchange(image.image, nullptr);
		doomed.allocation = std::exchange(image.allocation, nullptr);
	}
	Destroy(std::move(doomed));
}

void ImageRecycler::Flush() {
	// Destroys inline, including what waits for the helper thread, so the memory is free on return.
	std::vector<Doomed> doomed;
	{
		std::lock_guard lock(m_mutex);
		for (auto& entry: m_entries) {
			Doomed d;
			d.image      = entry.image;
			d.allocation = entry.allocation;
			doomed.push_back(std::move(d));
		}
		m_evicted += m_entries.size();
		m_entries.clear();
		m_bytes = 0;
	}
	{
		std::lock_guard lock(m_doomed_mutex);
		for (auto& d: m_doomed) {
			doomed.push_back(std::move(d));
		}
		m_doomed.clear();
	}
	for (auto& d: doomed) {
		DestroyNow(d);
	}
}

void ImageRecycler::DestroyNow(Doomed& doomed) {
	for (const auto view: doomed.views) {
		m_graphics.device.destroyImageView(view, nullptr);
	}
	if (doomed.image != nullptr) {
		VulkanImage image;
		image.image      = doomed.image;
		image.allocation = doomed.allocation;
		m_graphics.DeleteImage(image);
	}
}

void ImageRecycler::Evict(uint64_t now_ns) {
	while (!m_entries.empty() &&
	       (m_entries.size() > kMaxEntries || m_bytes > kMaxBytes ||
	        now_ns - m_entries.front().time_ns > kMaxAgeNs)) {
		auto& front = m_entries.front();
		m_bytes -= front.bytes;
		m_evicted++;
		Doomed doomed;
		doomed.image      = front.image;
		doomed.allocation = front.allocation;
		m_entries.pop_front();
		Destroy(std::move(doomed));
	}
	if (Common::GpuWaitDiag::DriverStats()) {
		Common::GpuWaitDiag::NoteRecyclerState(m_entries.size(), m_bytes, m_evicted, 0);
	}
}

void ImageRecycler::Destroy(Doomed&& doomed) {
	if (doomed.views.empty() && doomed.image == nullptr) {
		return;
	}
	if (m_thread_destroy) {
		{
			std::lock_guard lock(m_doomed_mutex);
			m_doomed.push_back(std::move(doomed));
		}
		m_doomed_cv.notify_one();
		return;
	}
	DestroyNow(doomed);
}

void ImageRecycler::Run() {
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyImageFree");
	if (Common::HybridAffinity::GetPlan().active) {
		const auto efficiency = Common::HybridAffinity::ReadCpuList("/sys/devices/cpu_atom/cpus");
		if (!efficiency.empty()) {
			cpu_set_t set;
			CPU_ZERO(&set);
			for (const int cpu: efficiency) {
				CPU_SET(cpu, &set);
			}
			(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
		}
	}
#endif
	std::vector<Doomed> batch;
	for (;;) {
		{
			std::unique_lock lock(m_doomed_mutex);
			m_doomed_cv.wait(lock, [this] { return m_stop || !m_doomed.empty(); });
			if (m_doomed.empty() && m_stop) {
				return;
			}
			batch.swap(m_doomed);
		}
		for (auto& doomed: batch) {
			DestroyNow(doomed);
		}
		batch.clear();
	}
}

} // namespace Libs::Graphics
