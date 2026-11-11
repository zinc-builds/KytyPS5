#include "graphics/host_gpu/renderer/deferredRecorder.h"

#include "common/hybridAffinity.h"
#include "common/kernelStats.h"
#include "common/profiler.h"
#include "common/stallDiag.h"

#include <chrono>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define KYTY_RECORDER_PAUSE() _mm_pause()
#else
#define KYTY_RECORDER_PAUSE() std::this_thread::yield()
#endif

namespace Libs::Graphics {

DeferredRecorder::DeferredRecorder()
    : m_storage(new std::byte[Capacity + Alignment]),
      m_ring(reinterpret_cast<std::byte*>(AlignUp(reinterpret_cast<uintptr_t>(m_storage.get())))),
      m_thread([this] { Run(); }) {}

DeferredRecorder::~DeferredRecorder() {
	Drain();
	m_stop.store(true, std::memory_order_seq_cst);
	m_sleeping.store(false, std::memory_order_seq_cst);
	m_sleeping.notify_one();
	m_thread.join();
}

std::byte* DeferredRecorder::Reserve(size_t size) {
	EXIT_IF(size > Capacity / 4);
	auto position = m_write % Capacity;
	if (Capacity - position < size) {
		// Records never wrap: pad up to the end of the ring.
		const auto padding = Capacity - position;
		WaitDone(m_write + padding + size - Capacity);
		auto* pad = new (m_ring + position) Header {};
		pad->size = padding;
		m_write += padding;
		position = 0;
	}
	if (m_write + size > Capacity) {
		WaitDone(m_write + size - Capacity);
	}
	auto* record = m_ring + position;
	m_write += size;
	return record;
}

void DeferredRecorder::Publish() {
	if (m_write == m_last_published) {
		return;
	}
	m_last_published = m_write;
	// No fence per draw: if the recorder starts to sleep just now, this check can miss it. It
	// then sleeps until the next Publish sees it, or until WaitDone wakes it for certain.
	m_published.store(m_write, std::memory_order_release);
	if (m_sleeping.load(std::memory_order_relaxed)) {
		Wake();
	}
}

void DeferredRecorder::Wake() {
	m_sleeping.store(false, std::memory_order_seq_cst);
	Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::Wake);
	m_sleeping.notify_one();
}

void DeferredRecorder::WaitDone(uint64_t position) {
	if (m_done.load(std::memory_order_acquire) >= position) {
		return;
	}
	Common::StallDiag::Scope       stall(Common::StallDiag::Cause::CmdRecorder);
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::RecorderWait);
	Publish();
	// Pairs with the recorder's seq_cst sleeping store and published load: either it sees what
	// was published, or this sees it asleep.
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (m_sleeping.load(std::memory_order_relaxed)) {
		Wake();
	}
	for (uint32_t spins = 0; m_done.load(std::memory_order_acquire) < position; spins++) {
		if (spins < 4096) {
			KYTY_RECORDER_PAUSE();
		} else {
			Common::KernelStats::Syscall yield(Common::KernelStats::Kind::Yield);
			std::this_thread::yield();
		}
	}
}

void DeferredRecorder::DrainSlow() {
	KYTY_PROFILER_FUNCTION();
	WaitDone(m_write);
}

void DeferredRecorder::Run() {
	KYTY_PROFILER_THREAD("Thread_GpuRecorder");
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyGpuRec");
	Common::HybridAffinity::ApplyToRecorderThread();
#endif
	uint64_t done = 0;
	for (;;) {
		auto published = m_published.load(std::memory_order_acquire);
		if (published == done) {
			if (m_stop.load(std::memory_order_acquire)) {
				return;
			}
			// Draws arrive every few microseconds: spin a while before sleeping.
			const auto start = std::chrono::steady_clock::now();
			auto       stall = Common::StallDiag::Now();
			for (uint32_t spins = 1;; spins++) {
				KYTY_RECORDER_PAUSE();
				published = m_published.load(std::memory_order_acquire);
				if (published != done || m_stop.load(std::memory_order_relaxed)) {
					break;
				}
				if ((spins & 255u) == 0 &&
				    std::chrono::steady_clock::now() - start > std::chrono::microseconds(50)) {
					Common::StallDiag::Add(Common::StallDiag::Cause::RecorderSpin, stall);
					stall = Common::StallDiag::Now();
					m_sleeping.store(true, std::memory_order_seq_cst);
					if (m_published.load(std::memory_order_seq_cst) == done &&
					    !m_stop.load(std::memory_order_seq_cst)) {
						m_sleeping.wait(true, std::memory_order_seq_cst);
					}
					m_sleeping.store(false, std::memory_order_relaxed);
					Common::StallDiag::Add(Common::StallDiag::Cause::RecorderSleep, stall);
					stall = 0;
					break;
				}
			}
			Common::StallDiag::Add(Common::StallDiag::Cause::RecorderSpin, stall);
			continue;
		}
		Common::StallDiag::Scope busy(Common::StallDiag::Cause::RecorderBusy);
		while (done != published) {
			auto*      header = reinterpret_cast<Header*>(m_ring + done % Capacity);
			const auto size   = header->size;
			if (header->run != nullptr) {
				header->run(header);
			}
			done += size;
			m_done.store(done, std::memory_order_release);
		}
	}
}

} // namespace Libs::Graphics
