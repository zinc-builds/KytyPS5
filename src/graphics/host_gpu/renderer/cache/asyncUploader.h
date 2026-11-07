#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_ASYNCUPLOADER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_ASYNCUPLOADER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
struct GraphicContext;

// Large uploads over the copy engine. A copy from host memory into a buffer the universal queue
// uses is bound by PCIe (~22 GB/s), and with the barriers around it the universal queue does
// nothing else meanwhile. Here the bytes go from a host ring into a device-local ring on the
// transfer queue as soon as they are written, while the universal queue still runs earlier
// work. The ring range is fresh, so nothing the universal queue has queued can read it. The
// universal queue then copies ring → destination at VRAM speed, in the command buffer it
// would have copied from host memory, and that submission waits for the transfer.
//
// KYTY_ASYNC_UPLOAD=0: off (no transfer queue is created). KYTY_ASYNC_UPLOAD_MIN=<bytes>:
// smallest upload sent this way (default 256 KiB). KYTY_ASYNC_UPLOAD=stats: ASYNCUPLOAD lines.
class AsyncUploader {
public:
	AsyncUploader(GraphicContext& graphics, CommandScheduler& scheduler,
	              uint64_t ring_size = 256ull * 1024 * 1024);
	~AsyncUploader();
	KYTY_CLASS_NO_COPY(AsyncUploader);

	[[nodiscard]] bool     Enabled() const noexcept { return m_enabled; }
	[[nodiscard]] uint64_t MinSize() const noexcept { return m_min_size; }

	// write(mapped) fills size bytes. Returns the device-local buffer and offset holding them,
	// readable by the current command buffer of the scheduler, or a null buffer when the upload
	// does not fit (the caller then uploads directly).
	[[nodiscard]] std::pair<vk::Buffer, uint64_t> Upload(uint64_t size,
	                                                     const std::function<void(uint8_t*)>& write);

private:
	struct Region {
		uint64_t begin = 0; // virtual (monotonic) ring offset
		uint64_t tick  = 0; // command buffer that reads it
	};
	struct Command {
		vk::CommandBuffer buffer;
		uint64_t          value = 0; // transfer semaphore value once it is done
	};

	[[nodiscard]] bool              AllocateDevice(uint64_t size, uint64_t& offset);
	[[nodiscard]] vk::CommandBuffer AcquireCommand();
	void                            Report(uint64_t bytes);

	GraphicContext&              m_graphics;
	CommandScheduler&            m_scheduler;
	bool                         m_enabled  = false;
	bool                         m_stats    = false;
	uint64_t                     m_min_size = 256 * 1024;
	uint64_t                     m_ring_size;
	std::unique_ptr<StreamBuffer> m_host;
	std::unique_ptr<Buffer>      m_device;
	uint64_t                     m_device_head = 0; // virtual offset
	std::deque<Region>           m_device_regions;
	vk::CommandPool              m_pool      = nullptr;
	vk::Semaphore                m_semaphore = nullptr;
	uint64_t                     m_value     = 0;
	std::vector<Command>         m_commands;
	// KYTY_ASYNC_UPLOAD=stats
	uint64_t m_stat_uploads = 0, m_stat_bytes = 0, m_stat_waits = 0, m_stat_fallbacks = 0;
	int64_t  m_stat_last    = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_ASYNCUPLOADER_H_
