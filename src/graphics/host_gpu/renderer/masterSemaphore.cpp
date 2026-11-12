#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/gpuWaitDiag.h"
#include "common/kernelStats.h"
#include "common/stallDiag.h"
#include "graphics/host_gpu/graphicContext.h"

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_wait_diag_poller.joinable()) {
		m_wait_diag_poller.request_stop();
		m_wait_diag_poller.join();
	}
	m_graphics.device.destroySemaphore(m_semaphore, nullptr);
}

void MasterSemaphore::EnableWaitDiag() {
	if (!Common::GpuWaitDiag::Enabled() || m_wait_diag) {
		return;
	}
	m_wait_diag        = true;
	m_wait_diag_poller = std::jthread([this](std::stop_token stop) {
#if defined(__linux__)
		pthread_setname_np(pthread_self(), "KytyGpuWaitDiag");
#endif
		uint64_t seen = 0;
		while (!stop.stop_requested()) {
			const uint64_t      next = seen + 1;
			vk::SemaphoreWaitInfo wait_info {};
			wait_info.semaphoreCount = 1;
			wait_info.pSemaphores    = &m_semaphore;
			wait_info.pValues        = &next;
			const auto result        = m_graphics.device.waitSemaphores(&wait_info, 20'000'000);
			if (result != vk::Result::eSuccess && result != vk::Result::eTimeout) {
				return;
			}
			uint64_t counter = 0;
			if (m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter) !=
			    vk::Result::eSuccess) {
				return;
			}
			if (counter > seen) {
				Common::GpuWaitDiag::NoteComplete(counter, Common::GpuWaitDiag::NowNs());
				seen = counter;
			}
		}
	});
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick, const std::source_location& site) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::GpuWaits,
	                                         Common::DebugCounters::Counter::GpuWaitNs);
	Common::StallDiag::Scope stall(Common::StallDiag::GpuWaitCause(),
	                               Common::StallDiag::OnCommandThread());
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::GpuWait);
	const bool diag  = m_wait_diag && Common::StallDiag::OnCommandThread();
	const auto known = diag ? KnownGpuTick() : 0;
	const auto start = diag ? Common::GpuWaitDiag::NowNs() : 0;
	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	if (diag) {
		Common::GpuWaitDiag::NoteWait(tick, CurrentTick(), known, start,
		                              Common::GpuWaitDiag::NowNs(), site);
	}
	Refresh();
}

} // namespace Libs::Graphics
