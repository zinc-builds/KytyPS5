#include "graphics/host_gpu/renderer/cache/asyncUploader.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr uint64_t RingAlign = 256;

} // namespace

AsyncUploader::AsyncUploader(GraphicContext& graphics, CommandScheduler& scheduler,
                             uint64_t ring_size)
    : m_graphics(graphics), m_scheduler(scheduler), m_ring_size(Common::AlignUp(ring_size, RingAlign)) {
	if (graphics.transfer_queue == nullptr) {
		return;
	}
	if (const char* env = std::getenv("KYTY_ASYNC_UPLOAD"); env != nullptr) {
		m_stats = std::strcmp(env, "stats") == 0;
	}
	if (const char* env = std::getenv("KYTY_ASYNC_UPLOAD_MIN"); env != nullptr) {
		m_min_size = std::strtoull(env, nullptr, 0);
	}

	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;
	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.pNext = &type_info;
	EXIT_NOT_IMPLEMENTED(graphics.device.createSemaphore(&semaphore_info, nullptr, &m_semaphore) !=
	                     vk::Result::eSuccess);

	vk::CommandPoolCreateInfo pool_info {};
	pool_info.queueFamilyIndex = graphics.transfer_queue_family;
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                             vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	EXIT_NOT_IMPLEMENTED(graphics.device.createCommandPool(&pool_info, nullptr, &m_pool) !=
	                     vk::Result::eSuccess);

	m_host   = std::make_unique<StreamBuffer>(graphics, scheduler, MemoryUsage::Upload, m_ring_size,
	                                          true);
	m_device = std::make_unique<Buffer>(
	    graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, m_ring_size,
	    true);
	SetVulkanObjectNameF(graphics.device, m_host->Handle(), "Kyty.AsyncUploadHost");
	SetVulkanObjectNameF(graphics.device, m_device->Handle(), "Kyty.AsyncUploadDevice");
	m_enabled = true;
	std::printf("AsyncUploader: transfer queue family %u, uploads >= %llu bytes\n",
	            graphics.transfer_queue_family, static_cast<unsigned long long>(m_min_size));
}

AsyncUploader::~AsyncUploader() {
	if (m_semaphore != nullptr) {
		// The universal queue may still copy from the rings: the owner finishes it first.
		if (m_value != 0) {
			vk::SemaphoreWaitInfo wait {};
			wait.semaphoreCount = 1;
			wait.pSemaphores    = &m_semaphore;
			wait.pValues        = &m_value;
			(void)m_graphics.device.waitSemaphores(&wait, UINT64_MAX);
		}
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
	if (m_pool != nullptr) {
		m_graphics.device.destroyCommandPool(m_pool, nullptr);
	}
}

bool AsyncUploader::AllocateDevice(uint64_t size, uint64_t& offset) {
	if (size > m_ring_size) {
		return false;
	}
	auto begin = Common::AlignUp(m_device_head, RingAlign);
	if (begin % m_ring_size + size > m_ring_size) {
		begin = Common::AlignUp(begin, m_ring_size); // wrap to the ring start
	}
	// Every range of the previous lap that overlaps [begin, begin + size) must be read first.
	const auto reusable_below = begin + size - m_ring_size; // virtual offsets below this are reused
	while (!m_device_regions.empty() && begin + size > m_ring_size &&
	       m_device_regions.front().begin < reusable_below) {
		const auto tick = m_device_regions.front().tick;
		if (!m_scheduler.IsFree(tick)) {
			m_stat_waits++;
			m_scheduler.Wait(tick);
		}
		m_device_regions.pop_front();
	}
	// Retire finished ranges so the queue stays short.
	while (!m_device_regions.empty() && m_scheduler.IsFree(m_device_regions.front().tick)) {
		m_device_regions.pop_front();
	}
	m_device_regions.push_back({begin, m_scheduler.CurrentTick()});
	m_device_head = begin + size;
	offset        = begin % m_ring_size;
	return true;
}

vk::CommandBuffer AsyncUploader::AcquireCommand() {
	uint64_t done = 0;
	EXIT_NOT_IMPLEMENTED(m_graphics.device.getSemaphoreCounterValue(m_semaphore, &done) !=
	                     vk::Result::eSuccess);
	for (auto& command: m_commands) {
		if (command.value <= done) {
			EXIT_NOT_IMPLEMENTED(command.buffer.reset({}) != vk::Result::eSuccess);
			return command.buffer;
		}
	}
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer buffer;
	EXIT_NOT_IMPLEMENTED(m_graphics.device.allocateCommandBuffers(&allocate, &buffer) !=
	                     vk::Result::eSuccess);
	m_commands.push_back({buffer, 0});
	return buffer;
}

std::pair<vk::Buffer, uint64_t> AsyncUploader::Upload(uint64_t                             size,
                                                      const std::function<void(uint8_t*)>& write) {
	EXIT_IF(!m_enabled || size == 0);
	// The host range first: mapping may submit the current command buffer to wait for reuse.
	const auto [mapped, host_offset] = m_host->Map(size, 16);
	if (mapped == nullptr) {
		m_stat_fallbacks++;
		return {nullptr, 0};
	}
	write(mapped);
	uint64_t device_offset = 0;
	if (!AllocateDevice(size, device_offset)) {
		m_stat_fallbacks++;
		return {nullptr, 0};
	}
	// After the device range: waiting for it may submit, and the host range must be watched by
	// the command buffer that waits for this transfer.
	m_host->Commit();

	const auto command = AcquireCommand();
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	EXIT_NOT_IMPLEMENTED(command.begin(&begin) != vk::Result::eSuccess);
	const vk::BufferCopy copy {host_offset, device_offset, size};
	command.copyBuffer(m_host->Handle(), m_device->Handle(), 1, &copy);
	EXIT_NOT_IMPLEMENTED(command.end() != vk::Result::eSuccess);

	const auto value = ++m_value;
	for (auto& item: m_commands) {
		if (item.buffer == command) item.value = value;
	}
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues    = &value;
	vk::SubmitInfo submit {};
	submit.pNext                = &timeline;
	submit.commandBufferCount   = 1;
	submit.pCommandBuffers      = &command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores    = &m_semaphore;
	const auto result           = m_graphics.transfer_queue.submit(1, &submit, nullptr);
	if (result != vk::Result::eSuccess) {
		EXIT("AsyncUploader: vkQueueSubmit failed: %s\n", vk::to_string(result).c_str());
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmits);
	m_scheduler.WaitBeforeNextSubmit(m_semaphore, value);
	Report(size);
	return {m_device->Handle(), device_offset};
}

void AsyncUploader::Report(uint64_t bytes) {
	m_stat_uploads++;
	m_stat_bytes += bytes;
	if (!m_stats) return;
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	if (m_stat_last == 0) m_stat_last = now;
	const double seconds = static_cast<double>(now - m_stat_last) / 1e9;
	if (seconds < 1.0) return;
	std::printf("ASYNCUPLOAD uploads_s=%.0f mb_s=%.1f ring_waits_s=%.1f fallbacks_s=%.1f\n",
	            static_cast<double>(m_stat_uploads) / seconds,
	            static_cast<double>(m_stat_bytes) / 1e6 / seconds,
	            static_cast<double>(m_stat_waits) / seconds,
	            static_cast<double>(m_stat_fallbacks) / seconds);
	std::fflush(stdout);
	m_stat_uploads = m_stat_bytes = m_stat_waits = m_stat_fallbacks = 0;
	m_stat_last                                                     = now;
}

} // namespace Libs::Graphics
