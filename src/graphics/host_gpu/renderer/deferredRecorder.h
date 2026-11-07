#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEFERREDRECORDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEFERREDRECORDER_H_

#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace Libs::Graphics {

// Records Vulkan commands on a second thread. The GPU command thread queues the commands that end
// a draw (binds, pushes, dynamic state, the draw itself) and goes on preparing the next draw while
// this thread records them. Commands become visible to the recording thread at Publish().
// Anything that records into the command buffer directly must Drain() first (CommandBuffer::Handle
// does), so all commands keep their order and only one thread records at a time. Only the GPU
// command thread may queue or drain.
class DeferredRecorder {
public:
	DeferredRecorder();
	~DeferredRecorder();
	KYTY_CLASS_NO_COPY(DeferredRecorder);

	// Queues f(buffer).
	template <typename F>
	void Record(vk::CommandBuffer buffer, F&& f) {
		Record(buffer, 0, [](std::byte*) {},
		       [g = std::forward<F>(f)](vk::CommandBuffer cb, const std::byte*) mutable { g(cb); });
	}

	// Queues f(buffer, data), where fill(data) writes `size` bytes now.
	template <typename Fill, typename F>
	void Record(vk::CommandBuffer buffer, size_t size, Fill&& fill, F&& f) {
		using Fn = std::decay_t<F>;
		static_assert(alignof(Fn) <= Alignment);
		const size_t fn_offset   = AlignUp(sizeof(Header));
		const size_t data_offset = AlignUp(fn_offset + sizeof(Fn));
		const size_t total       = AlignUp(data_offset + size);
		auto*        record      = Reserve(total);
		auto*        header      = new (record) Header {};
		header->size             = total;
		header->buffer           = buffer;
		header->run              = [](Header* h) {
			auto* bytes = reinterpret_cast<std::byte*>(h);
			auto* fn    = std::launder(reinterpret_cast<Fn*>(bytes + AlignUp(sizeof(Header))));
			(*fn)(h->buffer, bytes + AlignUp(AlignUp(sizeof(Header)) + sizeof(Fn)));
			fn->~Fn();
		};
		new (record + fn_offset) Fn(std::forward<F>(f));
		fill(record + data_offset);
	}

	// Makes the queued commands visible to the recording thread.
	void Publish();

	// Publishes, then waits until every queued command is recorded.
	void Drain() {
		if (m_write != m_done.load(std::memory_order_acquire)) {
			DrainSlow();
		}
	}

private:
	// Records are multiples of a header, so a padding header always fits the end of the ring.
	static constexpr size_t Alignment = 32;
	static constexpr size_t Capacity  = size_t {4} << 20u;

	struct alignas(Alignment) Header {
		void (*run)(Header*)     = nullptr; // null: padding up to the end of the ring
		uint64_t          size   = 0;
		vk::CommandBuffer buffer = nullptr;
	};

	static_assert(sizeof(Header) == Alignment);

	static constexpr size_t AlignUp(size_t value) {
		return (value + Alignment - 1) & ~(Alignment - 1);
	}

	std::byte* Reserve(size_t size);
	void       WaitDone(uint64_t position);
	void       DrainSlow();
	void       Run();

	std::unique_ptr<std::byte[]> m_storage;
	std::byte*                   m_ring = nullptr; // m_storage aligned to Alignment
	uint64_t                     m_write = 0; // bytes queued (command thread only)
	uint64_t                     m_last_published = 0;
	alignas(64) std::atomic<uint64_t> m_published {0};
	alignas(64) std::atomic<uint64_t> m_done {0};
	alignas(64) std::atomic<bool> m_sleeping {false};
	std::atomic<bool>             m_stop {false};
	std::thread                   m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEFERREDRECORDER_H_
