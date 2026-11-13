#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageRecycler.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <array>
#include <atomic>
#include <bitset>
#include <map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
class TextureCache;
struct TextureCacheTestAccess;

namespace BcAlias {
[[nodiscard]] bool Matches(const ImageInfo& requested, const ImageInfo& cached);
[[nodiscard]] bool Publish(TextureCache& cache, Image& image);
void Resolve(TextureCache& cache, ImageId cached, ImageId merged);
} // namespace BcAlias

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
		bool          heap = false; // PS5: resolved for a resident image heap.
	};

	TextureCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	             BufferCache& buffer_cache);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	// exact_generation, when given, receives a generation for RefindExactImage when the result
	// is an exact match FindImage keeps, else 0.
	[[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_format = false,
	                                uint64_t* exact_generation = nullptr);
	// Repeats a FindImage of a description with data range `data` that returned `id` as an exact
	// match at `generation`, when FindImage would return it again: no image was registered or
	// unregistered on those pages since. Advances generation and touches the image like FindImage,
	// including the color-clear step for `found`, the description as FindImage left it (null when
	// it has no color metadata) and its view's base layer before FindImage.
	// Advanced by every image registration and unregistration, so by every image deletion and
	// recreation (an image's views are destroyed only with the image, after it is unregistered;
	// the sampler cache never evicts). Image bindings resolved at one epoch, and touched at one
	// GcTick, are what a resolve would give again while both hold (stage binding-set reuse).
	[[nodiscard]] uint64_t BindingEpoch() const { return m_registration_generation; }
	[[nodiscard]] uint64_t GcTick() const { return m_gc_tick; }
	[[nodiscard]] bool RefindExactImage(ImageId id, GuestRange data, const ImageDesc* found,
	                                    uint32_t metadata_base_layer, uint64_t& generation);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	// FindTexture without the view: refreshes, settles and tracks the image for this draw's use.
	void AcquireTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	struct MetaDataInfo {
		// PS5 DB_DEPTH_VIEW encodes 13-bit base and last array-slice indices.
		std::bitset<8192> clear_mask;
		// The depth image that last acquired this HTile range.
		ImageId owner;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	// The requested fields that decide an exact match: SameBacking and the stencil plane test.
	struct ExactImageKey {
		GuestRange             data;
		vk::Extent3D           extent;
		ImageSubresources      resources;
		uint32_t               samples         = 1;
		uint32_t               bytes_per_block = 0;
		Prospero::TileMode     tile_mode       = Prospero::TileMode::kLinear;
		ColorTransform         transform       = ColorTransform::None;
		vk::Format             pixel_format    = vk::Format::eUndefined;
		Prospero::BufferFormat guest_format    = Prospero::BufferFormat::kInvalid;
		Prospero::ImageType    type            = Prospero::ImageType::kColor2D;
		BindingType            binding         = BindingType::Texture;
		bool                   exact_format    = false;

		bool operator==(const ExactImageKey&) const = default;
	};
	struct ExactImageKeyHash {
		[[nodiscard]] size_t operator()(const ExactImageKey& key) const noexcept;
	};
	struct ExactImage {
		ImageId  id;
		uint64_t generation = 0;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 44, 14>;
	using PageGenerations =
	    MultiLevelPageTable<uint64_t, ImagePageTable::kPageBits, ImagePageTable::kAddressSpaceBits,
	                        ImagePageTable::kFirstLevelBits>;

	// Registered images per page of the image page table. Written under m_lock, read without it.
	class RegisteredPageCounts final {
	public:
		RegisteredPageCounts() = default;
		~RegisteredPageCounts();
		KYTY_CLASS_NO_COPY(RegisteredPageCounts);

		void               Add(size_t page, int32_t delta);
		[[nodiscard]] bool Any(size_t first, size_t last_exclusive) const noexcept;

	private:
		static constexpr size_t kLeafBits = 10;
		static constexpr size_t kLeafSize = size_t {1} << kLeafBits;
		using Leaf                        = std::array<std::atomic<uint32_t>, kLeafSize>;

		std::array<std::atomic<Leaf*>, ImagePageTable::kPageCount / kLeafSize> m_leaves {};
	};

	// Callers pass a nonempty, valid guest range.
	template <typename Func>
	static void ForEachPage(uint64_t address, size_t size, Func&& func) {
		const uint64_t page_end = (address + size - 1) >> ImagePageTable::kPageBits;
		for (uint64_t page = address >> ImagePageTable::kPageBits; page <= page_end; ++page) {
			func(page);
		}
	}

	[[nodiscard]] ImageId     InsertImage(const ImageInfo& info);
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	void                      DeleteImage(ImageId id);
	void                      FreeImage(ImageId id);
	void                      TouchImage(Image& image);
	void                      TrackImage(ImageId id);
	void                      TrackImageHead(ImageId id);
	void                      TrackImageTail(ImageId id);
	void                      UntrackImage(ImageId id);
	void                      UntrackImageHead(ImageId id);
	void                      UntrackImageTail(ImageId id);
	void                      MarkAsMaybeDirty(ImageId id, Image& image);
	void                      TrackImageDownload(ImageId id, Image& image);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);

	// Stamps the pages of a registration change; exact lookups there search again.
	void               TouchRegistrations(uint64_t address, uint64_t size);
	[[nodiscard]] bool RegistrationsSince(uint64_t address, uint64_t size,
	                                      uint64_t generation) const;

	// Caller holds m_lock: what a lock-free lookup of the image may skip.
	[[nodiscard]] Image::Settled SettledState(const Image& image) const;
	void                         Settle(Image& image);
	void                         VerifySettled(const Image& image, uint8_t need);
	// Caller holds m_lock; it also serializes Image::picked.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	void                        RefreshImage(ImageId id);
	void                        MaterializeColorClear(ImageId id, const ImageDesc& desc,
	                                                uint32_t metadata_base_layer);
	[[nodiscard]] bool StencilTargetSettled(const Image& depth, ImageId stencil_id);
	[[nodiscard]] bool MaterializeColorClearSlices(ImageId id, const ImageDesc& desc,
	                                              uint32_t metadata_base_layer, bool& memoizable);
	void                        InitializeImage(ImageId id);
	[[nodiscard]] bool          CanDownload(const Image& image) const;
	void UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                   uint64_t destination_size, uint32_t levels);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	// Caller holds m_lock and ends rendering. Begins rendering `layers` layers of one color mip
	// through a `format` view, clearing them or, when `clear` is null, loading them; returns the
	// render area.
	vk::Rect2D BeginColorRendering(Image& image, vk::Format format, uint32_t level,
	                               uint32_t layer, uint32_t layers, const vk::ClearValue* clear,
	                               vk::CommandBuffer command);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	[[nodiscard]] bool DownloadImageMemory(ImageId id);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	// Before m_slot_images: images retire into it when the slots are destroyed.
	ImageRecycler                                     m_image_recycler;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	RegisteredPageCounts                              m_registered_pages;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	bool             m_readback_linear_images = false;
	bool             m_readback_tiled_images  = false;
	// Exact matches found by FindImage. The match depends on the images registered on the
	// pages of the request and on fields fixed at their creation, so an entry holds until a
	// registration on one of those pages changes.
	std::unordered_map<ExactImageKey, ExactImage, ExactImageKeyHash> m_exact_images;
	PageGenerations                                                  m_page_generations;
	// Page-table searches made by FindImagesInRegion; tests read it to see a lookup skip one.
	mutable uint64_t m_region_queries = 0;
	uint64_t                                                         m_registration_generation = 1;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	friend class ImageHeap;
	friend class ResidentStore;
	friend bool BcAlias::Matches(const ImageInfo&, const ImageInfo&);
	friend bool BcAlias::Publish(TextureCache&, Image&);
	friend void BcAlias::Resolve(TextureCache&, ImageId, ImageId);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
