#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGE_RECYCLER_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGE_RECYCLER_H_

#include "graphics/host_gpu/graphicContext.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {

// Texture-cache images whose GPU work has finished (TextureCache::DeleteImage defers the erase to
// the tick's completion) give their Vulkan image and memory here instead of destroying them.
// - KYTY_IMAGE_RECYCLE (default on): a later image with the same create info takes one back, so
//   neither vkCreateImage, the allocation nor the bind happens. The pool keeps at most
//   kMaxEntries images, kMaxBytes and kMaxAgeNs of age (checked at least once a second); the rest are destroyed.
// - KYTY_IMAGE_DESTROY_THREAD (default on): views and images are destroyed on a helper thread.
// =0 on both restores destruction in ~Image on the calling thread.
// A recycled image's contents are undefined, as a new image's are: its tracked layout is reset to
// UNDEFINED, so its first barrier discards them.
class ImageRecycler final {
public:
	explicit ImageRecycler(GraphicContext& graphics);
	~ImageRecycler();
	ImageRecycler(const ImageRecycler&)            = delete;
	ImageRecycler& operator=(const ImageRecycler&) = delete;

	[[nodiscard]] bool Enabled() const noexcept { return m_recycle || m_thread_destroy; }

	// Fills `image` from a recycled image with this create info (the image fields as
	// GraphicContext::CreateImage sets them). False: nothing matching.
	[[nodiscard]] bool Take(const vk::ImageCreateInfo& create, VulkanImage& image);
	// Takes over the views and the image (both reset); keeps or destroys them.
	void Retire(std::vector<vk::ImageView>&& views, VulkanImage& image);
	// Destroys everything kept and everything queued for the helper thread, on this thread.
	void Flush();

private:
	struct Key {
		vk::Format           format;
		vk::ImageType        type;
		vk::Extent3D         extent;
		uint32_t             layers;
		uint32_t             mip_levels;
		uint32_t             samples;
		vk::ImageUsageFlags  usage;
		vk::ImageCreateFlags flags;
		bool                 operator==(const Key&) const = default;
	};
	struct Entry {
		Key           key;
		vk::Image     image;
		VmaAllocation allocation;
		uint64_t      bytes;
		uint64_t      time_ns;
	};
	struct Doomed {
		std::vector<vk::ImageView> views;
		vk::Image                  image      = nullptr;
		VmaAllocation              allocation = nullptr;
	};

	static Key  KeyOf(const VulkanImage& image);
	void        Destroy(Doomed&& doomed);
	void        DestroyNow(Doomed& doomed);
	void        Evict(uint64_t now_ns); // m_mutex held
	void        Run();

	static constexpr size_t   kMaxEntries = 64;
	static constexpr uint64_t kMaxBytes   = 512ull * 1024 * 1024;
	static constexpr uint64_t kMaxAgeNs   = 2'000'000'000ull;

	GraphicContext&         m_graphics;
	bool                    m_recycle        = false;
	bool                    m_thread_destroy = false;
	std::mutex              m_mutex;
	std::deque<Entry>       m_entries; // oldest first
	uint64_t                m_bytes   = 0;
	uint64_t                m_evicted = 0;
	std::mutex              m_doomed_mutex;
	std::condition_variable m_doomed_cv;
	std::vector<Doomed>     m_doomed;
	bool                    m_stop       = false;
	bool                    m_destroying = false; // the helper is destroying a batch it took
	std::thread             m_thread;
};

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGE_RECYCLER_H_ */
