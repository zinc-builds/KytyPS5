#if defined(__linux__)
#include "common/hybridAffinity.h"
#include <pthread.h>
#include <sys/resource.h>
#endif
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/frameSkip.h"
#include "graphics/host_gpu/guestSyncEpoch.h"
#include "graphics/host_gpu/memDiag.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/gpuWaitDiag.h"
#include "common/frameDiag.h"
#include "common/benchMetrics.h"
#include "common/kernelStats.h"
#include "common/stallDiag.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/drawPrepare.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <deque>
#include <string_view>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace Libs::Graphics {

static thread_local Pm4Execution* g_current_execution = nullptr;
static thread_local GuestGpu*     g_gpu_state         = nullptr;

GuestGpu::GuestGpu(RenderContext& renderer): m_renderer(renderer) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_gfx_cp = std::make_unique<CommandProcessor>(renderer, 0);
	m_thread = std::jthread(ThreadRun, this);
}

GuestGpu::~GuestGpu() {
	Shutdown();
}

void GuestGpu::Shutdown() {
	std::lock_guard shutdown_lock(m_shutdown_mutex);
	if (m_shutdown_complete) {
		return;
	}
	{
		Common::LockGuard lock(m_queue_mutex);
		m_accepting = false;
		m_stopping  = true;
		m_work_available.SignalAll();
	}
	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_shutdown_complete = true;
}

bool GuestGpu::IsStopping() {
	Common::LockGuard lock(m_queue_mutex);
	return m_stopping;
}

void GuestGpu::SendCommand(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_commands.push_back(std::move(command));
	m_pending_commands.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
	if (auto* wake = m_command_wake.load(std::memory_order_acquire); wake != nullptr) {
		wake->Notify();
	}
}

void GuestGpu::ProcessCommands() {
	while (m_pending_commands.load(std::memory_order_acquire) != 0) {
		Common::UniqueFunction<void> command;
		{
			Common::LockGuard lock(m_queue_mutex);
			EXIT_IF(m_commands.empty());
			command = std::move(m_commands.front());
			m_commands.pop_front();
			EXIT_IF(m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
		}
		Common::StallDiag::CommandScope stall;
		Common::KernelStats::SiteScope  kernel(Common::KernelStats::Site::Command);
		command();
	}
}

void GuestGpu::SendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		command();
		return;
	}
	std::binary_semaphore done {0};
	SendCommand([operation = std::move(command), &done]() mutable {
		operation();
		done.release();
	});
	done.acquire();
}

void GuestGpu::Submit(std::span<const uint32_t> commands) {
	if (commands.empty()) {
		return;
	}
	Submission submission;
	submission.type     = SubmissionType::Commands;
	submission.queue_id = 0;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitCompute(uint32_t queue, std::span<const uint32_t> commands) {
	EXIT_IF(commands.empty());

	EXIT_NOT_IMPLEMENTED(queue < ComputeQueueBase || queue >= ComputeQueueBase + ComputeQueueCount);

	const auto compute_queue = queue - ComputeQueueBase;
	Submission submission;
	submission.type     = SubmissionType::Commands;
	submission.queue_id = 1 + compute_queue;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitFlipPreparation(uint64_t request_id) {
	Submission submission;
	submission.type            = SubmissionType::FlipPreparation;
	submission.queue_id        = 0;
	submission.flip_request_id = request_id;
	Enqueue(std::move(submission));
}

void GuestGpu::SuspendPoint() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	// Do not hold a queue lock while waiting: asynchronous work may be needed to
	// finish the preceding graphics frame. The first point returns immediately.
	m_suspend_point_ready->acquire();
	Submission submission;
	submission.type = SubmissionType::SuspendPoint;
	Enqueue(std::move(submission));
	m_done_num++;
}

int GuestGpu::GetFrameNum() const {
	return m_done_num;
}

CommandProcessor& GuestGpu::GetProcessor(uint32_t queue_id) {
	if (queue_id == 0) {
		return *m_gfx_cp;
	}
	auto& processor = m_compute_cp[queue_id - 1];
	if (processor == nullptr) {
		processor = std::make_unique<CommandProcessor>(m_renderer, ComputeQueueBase + queue_id - 1);
	}
	return *processor;
}

void CommandProcessor::Reset() {
	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_saved_ctx.reset();
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances                    = 1;
	m_num_instances_addr               = 0;
	m_predicate_skip                   = false;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
	switch (operation) {
		case ContextStateOperation::Clear: m_ctx.Reset(); break;
		case ContextStateOperation::Push:
			EXIT_IF(m_saved_ctx.has_value());
			m_saved_ctx.emplace(m_ctx);
			break;
		case ContextStateOperation::Pop:
			EXIT_IF(!m_saved_ctx.has_value());
			m_ctx = *m_saved_ctx;
			m_ctx.MarkAllWritten();
			m_saved_ctx.reset();
			break;
		case ContextStateOperation::PushClear:
			EXIT_IF(m_saved_ctx.has_value());
			m_saved_ctx.emplace(m_ctx);
			m_ctx.Reset();
			break;
	}
}

void CommandProcessor::BufferInit() {
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

void CommandProcessor::BufferFlush() {
	GetScheduler().Flush();
	// The occlusion counters reach a guest that polls them without waiting for a later fence.
	m_renderer.GetOcclusionQueries().Publish();
}

void CommandProcessor::BufferWait() {
	BufferInit();
	GetScheduler().Finish();
}

void CommandProcessor::WaitForRewind(bool valid) {
	if (!valid) {
		SuspendPm4(true);
		return;
	}
	// The guest may have written data before marking the rewind valid.
	m_renderer.RequestAsyncCollect();
}

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: EXIT("unknown wait compare function: %" PRIu32 "\n", func);
	}

	return false;
}


template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	const T value = *addr;
	if (!TestWaitRegMemValue(value, ref, mask, func)) {
		SuspendPm4(true);
		return;
	}
	// The value may publish data the guest wrote after submitting; collect those writes before
	// the next packet reads guest data. Not if the value is this processor's own label store
	// and the guest has not stored to its page since: then the guest published nothing by it.
	// (A suspended wait resumes through Process, which collects too.)
	if (m_renderer.IsCpLabel(reinterpret_cast<uint64_t>(addr), value)) {
		MemDiag::Add(MemDiag::WaitOnCpLabel);
		m_renderer.ObserveSkip(RenderContext::ObserveSkipKind::CpLabel);
		return;
	}
	MemDiag::Add(MemDiag::WaitCollections);
	m_renderer.RequestAsyncCollect();
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t);

void CommandProcessor::MemSemaphore(uint64_t* counter, bool signal, bool write) {
	if (signal) {
		*counter = write ? 1u : *counter + 1u;
		m_renderer.NoteGpuWrite(reinterpret_cast<uint64_t>(counter), sizeof(*counter));
	} else if (*counter == 0) {
		SuspendPm4(true);
	} else {
		(*counter)--;
		m_renderer.NoteGpuWrite(reinterpret_cast<uint64_t>(counter), sizeof(*counter));
		m_renderer.RequestAsyncCollect();
	}
}

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	const uint32_t dst_sel = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const bool     write_one_address = ((write_control >> 16u) & 0x1u) != 0;

	switch (dst_sel) {
		case 0:
		case 2:
		case 4:
		case 5:
		case 6: break;
		default: EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	if (dw_num == 0) {
		return;
	}

	MemDiag::NoteLabel();
	const uint64_t diag_start = Common::FrameDiag::Enabled() ? Common::FrameDiag::NowNs() : 0;
	// Completion labels can be reused immediately; memcpy may write a destination twice.
	auto& buffers = m_renderer.GetBufferCache();
	// Dwords [from, to) one at a time, each its own label store.
	const auto per_dword = [&](uint32_t from, uint32_t to) {
		auto* output = static_cast<volatile uint32_t*>(dst) + (write_one_address ? 0 : from);
		for (uint32_t i = from; i < to; i++) {
			const auto vaddr = reinterpret_cast<uint64_t>(output);
			if (!GuestRange {vaddr, sizeof(uint32_t)}.Valid() ||
			    buffers.HasGpuDirtyBytes(vaddr, sizeof(uint32_t)) ||
			    !m_renderer.StoreCpLabel(vaddr, &src[i], sizeof(uint32_t), true, dw_num)) {
				*output = src[i];
				m_renderer.NoteGpuWrite(vaddr, sizeof(uint32_t));
			}
			output += !write_one_address;
		}
	};
	const auto base = reinterpret_cast<uint64_t>(dst);
	// A destination overlapping the packet's own data: the per-dword loop reads each value after
	// the stores before it, and labels what it stored; keep that.
	const auto src_begin = reinterpret_cast<uint64_t>(src);
	const bool overlaps  = src_begin < base + uint64_t {dw_num} * 4 && base < src_begin + uint64_t {dw_num} * 4;
	if (write_one_address || overlaps || (base & 3u) != 0 || !RenderContext::WriteDataRanged()) {
		per_dword(0, dw_num);
	} else {
		// Each page's dwords as one label store: the same stores in the same order as the
		// per-dword loop, under one protection and one invalidation (StoreCpLabels). A page
		// holding any byte the GPU dirtied, or one the ranged store refuses, goes per dword.
		for (uint32_t i = 0; i < dw_num;) {
			const uint64_t vaddr = base + uint64_t {i} * sizeof(uint32_t);
			const auto     n     = static_cast<uint32_t>(std::min<uint64_t>(dw_num - i, (0x1000 - (vaddr & 0xfff)) / 4));
			const uint64_t size  = uint64_t {n} * sizeof(uint32_t);
			if (!GuestRange {vaddr, size}.Valid() || buffers.HasGpuDirtyBytes(vaddr, size) ||
			    !m_renderer.StoreCpLabels(vaddr, &src[i], sizeof(uint32_t), n, sizeof(uint32_t), true,
			                              dw_num)) {
				per_dword(i, i + n);
			}
			i += n;
		}
	}
	if (diag_start != 0) {
		Common::FrameDiag::Add(Common::FrameDiag::Counter::WriteDatas);
		Common::FrameDiag::Add(Common::FrameDiag::Counter::WriteDataDwords, dw_num);
		Common::FrameDiag::Add(Common::FrameDiag::Counter::WriteDataNs,
		                       Common::FrameDiag::NowNs() - diag_start);
	}
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	m_renderer.NoteGpuWrite(dst_address, num_bytes);
	static std::atomic<uint32_t> clock_log_count {0};
	if (clock_log_count.fetch_add(1) < 64) {
		LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
		     " size=%u\n",
		     dst_address, value, num_bytes);
	}
}

void CommandProcessor::NoteGuestWrite(const volatile void* address, uint64_t size) {
	m_renderer.NoteGpuWrite(reinterpret_cast<uint64_t>(address), size);
}

void CommandProcessor::GetLodStats(volatile uint32_t* dst, uint32_t size, bool reset) {
	auto& counters = m_renderer.GetRenderExecutor().m_mip_stats;
	// Each of up to 256 2-dword slots after the 16-dword header holds the clamped sample count
	// (dword 0 bits 0-23), the slot id (bits 24-31), the unclamped sample count (dword 1 bits 0-23)
	// and the finest mip sampled (bits 24-27, 15 for none). A non-zero header dword 0 then marks
	// the report complete.
	if (dst != nullptr && size >= 64) {
		const auto slots = std::min((size - 64) / 8, 256u);
		for (uint32_t slot = 0; slot < slots; slot++) {
			const auto& counter = counters[slot];
			dst[16 + slot * 2]  = (slot << 24u) | uint32_t {counter.clamped};
			dst[16 + slot * 2 + 1] =
			    (uint32_t {counter.finest_level} << 24u) | uint32_t {counter.unclamped};
		}
		dst[0] = 1;
	}
	if (dst != nullptr && size >= 64) {
		m_renderer.NoteGpuWrite(reinterpret_cast<uint64_t>(dst), size);
	}
	if (reset) {
		counters = {};
	}
}

void CommandProcessor::DmaData(uint8_t dst_sel, uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes) {
	if (num_bytes == 0 || static_cast<uint32_t>(dst_address_or_offset) == 0x3022cu) {
		return;
	}
	auto decode_gds = [](uint8_t selector, bool& is_gds) {
		switch (selector) {
			case 0:
			case 3: is_gds = false; return true;
			case 1: is_gds = true; return true;
			default: return false;
		}
	};
	if (dst_sel == 2) {
		// kNowhere discards the GL2 prefetch destination without a guest-visible write.
		if (src_sel != 3) {
			EXIT("unsupported dmaData nowhere source selector 0x%02" PRIx8 "\n", src_sel);
		}
		return;
	}
	bool dst_gds = false;
	if (!decode_gds(dst_sel, dst_gds)) {
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	auto& buffer_cache = m_renderer.GetBufferCache();
	if (src_sel == 2) {
		buffer_cache.FillBuffer(
		    dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu), dst_gds);
		return;
	}
	bool src_gds = false;
	if (!decode_gds(src_sel, src_gds)) {
		EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
	}
	if (src_gds && dst_gds) {
		EXIT("unsupported dmaData GDS-to-GDS copy\n");
	}
	buffer_cache.CopyBuffer(dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes,
	                        dst_gds, src_gds);
}

void GuestGpu::Enqueue(Submission submission) {
	// Before the submission is visible: its processing must find this scan's writes kept.
	submission.submit_scanned = m_renderer.ScanAsyncWritesAtSubmit();
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
	m_work_available.Signal();
}

void GuestGpu::WaitForIdle() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	Common::LockGuard lock(m_queue_mutex);
	while (m_processing || !m_commands.empty() || m_submission_count != 0) {
		m_idle.Wait(&m_queue_mutex);
	}
}

void GuestGpu::ThreadRun(void* data) {
	auto* gpu = static_cast<GuestGpu*>(data);
	EXIT_IF(gpu == nullptr);
	KYTY_PROFILER_THREAD("Thread_Gpu");
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyGpuCmd");
	Common::HybridAffinity::ApplyToGpuThread();
#endif
	g_gpu_state = gpu;
	FrameSkip::Init();
	Common::StallDiag::SetCommandThread();
	Common::KernelStats::SetCommandThread();
	Common::GpuWaitDiag::SetCommandThread();
	Common::BenchMetrics::RegisterThread(Common::BenchMetrics::Role::Cmd);

	for (;;) {
		Common::StallDiag::Report();
		Common::KernelStats::Report();
		Common::GpuWaitDiag::Report();
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         should_stop = false;
		uint64_t                     idle_start  = 0;
		uint64_t                     idle_end    = 0;
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			while (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				if (idle_start == 0 && Common::FrameDiag::Enabled()) {
					Common::FrameDiag::OnCmdIdleBegin();
					idle_start = Common::FrameDiag::NowNs();
				}
				gpu->m_processing = false;
				gpu->m_idle.Signal();
				Common::StallDiag::Scope       stall(Common::StallDiag::Cause::CmdNoWork);
				Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::Idle);
				gpu->m_work_available.Wait(&gpu->m_queue_mutex);
			}
			if (idle_start != 0) {
				idle_end = Common::FrameDiag::NowNs();
			}
			if (gpu->m_stopping && gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_processing = false;
				gpu->m_idle.SignalAll();
				should_stop = true;
			} else if (!gpu->m_commands.empty()) {
				command = std::move(gpu->m_commands.front());
				gpu->m_commands.pop_front();
				EXIT_IF(gpu->m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
				gpu->m_processing = true;
			} else {
				int selected_queue = -1;
				for (uint32_t offset = 0; offset < QueueCount; offset++) {
					const auto id = (gpu->m_next_queue + offset) % QueueCount;
					if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
						selected_queue = static_cast<int>(id);
						break;
					}
				}
				if (selected_queue < 0) {
					gpu->m_processing = false;
					{
						Common::FrameDiag::Scope       frame_diag(Common::FrameDiag::Counter::CmdBlocks,
						                                          Common::FrameDiag::Counter::CmdBlockedNs);
						Common::StallDiag::Scope       stall(Common::StallDiag::Cause::CmdBlocked);
						Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::Idle);
						gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
					}
					for (auto& queue: gpu->m_queues) {
						if (!queue.empty()) {
							queue.front().blocked = false;
						}
					}
					continue;
				}
				auto& queue = gpu->m_queues[static_cast<uint32_t>(selected_queue)];
				submission  = std::move(queue.front());
				queue.pop_front();
				gpu->m_submission_count--;
				gpu->m_next_queue = (static_cast<uint32_t>(selected_queue) + 1) % QueueCount;
				gpu->m_processing = true;
			}
		}
		if (idle_start != 0) {
			Common::FrameDiag::OnCmdIdle(idle_start, idle_end); // printed outside the queue lock
		}
		if (should_stop) {
			gpu->m_gfx_cp->BufferWait();
			g_gpu_state = nullptr;
			return;
		}

		if (command) {
			{
				Common::DebugCounters::ScopedTimer busy(
				    Common::DebugCounters::Counter::GpuThreadBusyNs);
				Common::StallDiag::CommandScope stall;
				Common::KernelStats::SiteScope  kernel(Common::KernelStats::Site::Command);
				command();
			}

			Common::LockGuard lock(gpu->m_queue_mutex);
			gpu->m_processing = false;
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_idle.SignalAll();
			}
			continue;
		}

		const bool complete = [&] {
			Common::DebugCounters::ScopedTimer busy(
			    Common::DebugCounters::Counter::GpuThreadBusyNs);
			return gpu->Process(submission);
		}();

		Common::LockGuard lock(gpu->m_queue_mutex);
		if (!complete) {
			submission.blocked = true;
			gpu->m_queues[submission.queue_id].push_front(std::move(submission));
			gpu->m_submission_count++;
		} else {
			for (auto& queue: gpu->m_queues) {
				if (!queue.empty()) {
					queue.front().blocked = false;
				}
			}
		}
		gpu->m_processing = false;
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
	auto& cp = GetProcessor(submission.queue_id);

	cp.BufferInit();
	bool complete = true;
	if (Common::GpuWaitDiag::Enabled()) {
		Common::GpuWaitDiag::NoteGuestSubmission(submission.queue_id);
	}
	GuestSyncEpoch::Advance();
	MemDiag::NoteSubmit();
	// Writes the guest made before submitting, in asynchronously write-protected ranges: the
	// submitting thread scanned for them (kept), or this scans. Stores after the submit scan
	// are collected where the command buffer can first depend on them: a satisfied wait on a
	// guest-written value, a semaphore wait or a rewind (RequestAsyncCollect), or a resumed
	// submission (its later processing scans here). See FINDINGS "Submit-time collection".
	// A resumed submission re-runs the packet it stopped at first. When that packet collects on
	// its own once satisfied (ResumeCollects: WAIT_REG_MEM, MEM_SEMAPHORE wait, REWIND), stores
	// made while it was blocked are collected there, or published nothing (the wait skip for
	// the processor's own labels), so only kept writes are taken here (KYTY_RESUME_SCAN=1 scans).
	const auto resume_mode = GetResumeScanMode();
	const bool resume_skip = !submission.submit_scanned && submission.execution.ResumeCollects() &&
	                         resume_mode != ResumeScanMode::Scan;
	m_renderer.CollectAsyncWrites(
	    submission.submit_scanned || resume_skip,
	    submission.submit_scanned && GetSubmitScanMode() == SubmitScanMode::Verify
	        ? RenderContext::KeptVerify::Submit
	    : resume_skip && resume_mode == ResumeScanMode::Verify ? RenderContext::KeptVerify::Resume
	                                                            : RenderContext::KeptVerify::None,
	    !submission.submit_scanned && resume_skip);
	submission.submit_scanned = false;

	switch (submission.type) {
		case SubmissionType::Commands:
			complete =
			    cp.Process(submission.execution, submission.commands) == Pm4ProcessResult::Complete;
			if (submission.execution.MadeProgress()) {
				cp.BufferFlush();
			}
			break;
		case SubmissionType::FlipPreparation:
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			// Age caches at the frame boundary, not once per command list.
			m_renderer.RunGarbageCollector();
			m_renderer.GetCommandScheduler().DeferPriorityOperation(
			    [ready = m_suspend_point_ready] { ready->release(); });
			cp.BufferFlush();
			cp.Reset();
			break;
	}

	return complete;
}

Pm4ProcessResult CommandProcessor::Process(Pm4Execution&             execution,
                                           std::span<const uint32_t> commands) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Process");
	EXIT_IF(g_current_execution != nullptr);
	EXIT_IF(commands.size() > UINT32_MAX);
	if (execution.m_buffer_stack.empty() && !commands.empty()) {
		execution.m_buffer_stack.push_back({commands});
	}
	execution.m_suspended     = false;
	execution.m_made_progress = false;

	struct ExecutionScope {
		explicit ExecutionScope(Pm4Execution& execution) { g_current_execution = &execution; }
		~ExecutionScope() { g_current_execution = nullptr; }
	} execution_scope(execution);

	RunPacketLoop(execution);
	// Nothing stays queued past the packet loop.
	m_renderer.GetRenderExecutor().FinishQueuedDraws(CurrentBuffer(),
	                                                 RenderExecutor::DrainEndOfLoop);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain) {
	EXIT_IF(g_current_execution == nullptr);
	EXIT_IF(!g_current_execution->m_next_buffer.empty());
	g_current_execution->m_next_buffer = commands;
	g_current_execution->m_chain       = chain;
}

void CommandProcessor::SuspendPm4(bool resume_collects) {
	EXIT_IF(g_current_execution == nullptr);
	g_current_execution->m_suspended       = true;
	g_current_execution->m_resume_collects = resume_collects;
}

// Packets that read no guest data through the caches, so a requested collection of guest writes
// (RenderContext::RequestAsyncCollect) can wait past them.
// The emulator's context-state NOP (push, pop, clear of the context registers) changes only the
// register context the command processor decodes from: a queued draw holds its own copy of the
// registers, and the packet touches no memory and signals nothing, so it is handled like a
// register write. KYTY_CONTEXT_STATE_DRAIN=1: it finishes the queue again.
static bool IsQueuedContextStateNop(uint32_t packet_header) {
	static const bool drains = [] {
		const char* value = std::getenv("KYTY_CONTEXT_STATE_DRAIN");
		return value != nullptr && value[0] == '1';
	}();
	return !drains && ((packet_header >> 8u) & 0xffu) == Pm4::IT_NOP &&
	       KYTY_PM4_R(packet_header) == Pm4::R_CONTEXT_STATE;
}

static bool DefersAsyncCollect(uint32_t packet_header) {
	switch ((packet_header >> 8u) & 0xffu) {
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_CONTEXT_CONTROL:
		case Pm4::IT_WAIT_REG_MEM:
		case Pm4::IT_WAIT_REG_MEM_64:
		case Pm4::IT_REWIND: return true;
		case Pm4::IT_NOP: {
			// Other NOP codes carry emulator operations (flips, custom draws).
			const auto r = KYTY_PM4_R(packet_header);
			return r == Pm4::R_ZERO || r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER ||
			       IsQueuedContextStateNop(packet_header);
		}
		default: return false;
	}
}

// Packets that only set state or draw (the draw path tracks its own accesses) keep a batch of
// atomic shader writes open; any other packet may observe their memory.
static bool KeepsAtomicWrites(uint32_t packet_header) {
	switch ((packet_header >> 8u) & 0xffu) {
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_INDIRECT_BUFFER:
		case Pm4::IT_DRAW_INDIRECT:
		case Pm4::IT_DRAW_INDEX_INDIRECT:
		case Pm4::IT_DRAW_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_MULTI_INST:
		case Pm4::IT_DRAW_INDEX_AUTO: return true;
		case Pm4::IT_NOP: {
			const auto r = KYTY_PM4_R(packet_header);
			return r == Pm4::R_ZERO || r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER ||
			       IsQueuedContextStateNop(packet_header);
		}
		default: return false;
	}
}

// Packets that touch nothing the caches hold and write no guest memory, besides direct draws
// (which queue themselves, see RenderExecutor::QueueDrawIndex), so a queued draw may wait past
// them. The indirect register writes read guest memory, which a queued draw's shaders could
// write only racily: the hardware command processor reads these packets long before earlier
// draws run, so a guest must put a wait or cache operation in between, and those finish the
// queue. Sky Garden uses them for nearly every draw.
static bool KeepsQueuedDraw(uint32_t packet_header) {
	switch ((packet_header >> 8u) & 0xffu) {
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_INDEX_BASE:
		case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_INDIRECT_BUFFER:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_MULTI_INST:
		case Pm4::IT_DRAW_INDEX_AUTO: return true;
		case Pm4::IT_NOP: {
			const auto r = KYTY_PM4_R(packet_header);
			return r == Pm4::R_ZERO || r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER ||
			       IsQueuedContextStateNop(packet_header);
		}
		default: return false;
	}
}

// Packets that neither wait nor touch memory, so consecutive dispatches around them may overlap
// on the guest: register writes, markers and the dispatches themselves. Any other packet may be
// the guest's synchronization between two dispatches (a partial flush, a cache operation, a label
// write or wait), or GPU work of another kind.
static bool KeepsDispatchChain(uint32_t packet_header) {
	switch ((packet_header >> 8u) & 0xffu) {
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
		case Pm4::IT_SET_SH_REG_INDIRECT:
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
		case Pm4::IT_SET_BASE:
		case Pm4::IT_INDIRECT_BUFFER:
		case Pm4::IT_DISPATCH_DIRECT: return true;
		case Pm4::IT_NOP: {
			const auto r = KYTY_PM4_R(packet_header);
			return r == Pm4::R_ZERO || r == Pm4::R_PUSH_MARKER || r == Pm4::R_POP_MARKER;
		}
		default: return false;
	}
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution) {
	const bool debug_dump = graphics_debug_dump_enabled();
	auto&      executor   = m_renderer.GetRenderExecutor();
	// A dispatch chain never spans two calls: another queue may have run in between.
	bool       first_packet = true;
	while (!execution.m_buffer_stack.empty()) {
		if (g_gpu_state != nullptr) {
			// Guest commands (readbacks, unmaps) see every draw before them committed.
			if (executor.HasQueuedDraw() && g_gpu_state->HasPendingCommands()) {
				executor.FinishQueuedDraws(CurrentBuffer(), RenderExecutor::DrainGuestCommands);
			}
			g_gpu_state->ProcessCommands();
		}
		auto& cursor = execution.m_buffer_stack.back();
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;

		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (debug_dump) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     cursor.offset_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			if (opcode == Pm4::IT_DRAW_INDIRECT_MULTI || opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI) {
				Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectPredicated);
			}
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw > remaining_dw);
			cursor.offset_dw += packet_dw;
			execution.m_made_progress = true;
			continue;
		}

		const PacketView view {packet, packet_header, opcode, remaining_dw, total_dw};
		RunPrologue(view, first_packet);
		if (!RunHandler(execution, view)) {
			return;
		}
	}
}

// Inlined into the packet loops: these run for every packet.
[[gnu::always_inline]] void CommandProcessor::RunPrologue(const PacketView& view, bool& first_packet) {
	auto&      executor      = m_renderer.GetRenderExecutor();
	const auto packet_header = view.header;
	const auto opcode        = view.opcode;
	if (PassDiag::Enabled()) {
		PassDiag::OnPacket(opcode);
	}
	// A multi-draw indirect keeps the queue too while it is queued itself (KYTY_INDIRECT_QUEUE);
	// one drawn at once drains in DrawIndirectMulti.
	// So does PFP_SYNC_ME (a no-op here: the queue keeps guest order, and CP stores run at
	// decode), which the guest puts between its indirect draws (one each in burst frames) so
	// the prefetcher sees the micro-engine's stores to their records.
	const bool queue_indirect = executor.IndirectQueueMode() != 0;
	const bool pfp_sync       = opcode == Pm4::IT_PFP_SYNC_ME && queue_indirect;
	const bool keeps          = KeepsQueuedDraw(packet_header) || pfp_sync ||
	                   ((opcode == Pm4::IT_DRAW_INDIRECT_MULTI ||
	                     opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI) &&
	                    queue_indirect);
	if (executor.HasQueuedDraw() &&
	    (!keeps || (!DefersAsyncCollect(packet_header) && m_renderer.AsyncCollectRequested()))) {
		uint32_t reason = RenderExecutor::DrainAsyncCollect;
		if (!keeps) {
			reason = opcode == Pm4::IT_NOP ? 512u + (KYTY_PM4_R(packet_header) & 0xffu) : opcode;
		}
		executor.FinishQueuedDraws(CurrentBuffer(), reason);
	}
	if (!DefersAsyncCollect(packet_header)) {
		m_renderer.CollectAsyncWritesIfRequested();
	}
	if (first_packet || !KeepsDispatchChain(packet_header)) {
		executor.EndDispatchChain(CurrentBuffer());
		first_packet = false;
	}
	// PFP_SYNC_ME (kept above) neither signals the guest nor stores: an epoch advance would only
	// void the queued draws' preparations.
	if (!KeepsAtomicWrites(packet_header) && !pfp_sync) {
		// The same packets may signal the guest or store into its memory.
		GuestSyncEpoch::Advance();
		if (executor.HasAtomicWrites()) {
			executor.FlushAtomicWrites(CurrentBuffer());
		}
		// Nothing is held open across command buffers here: the draw queue was drained and
		// the dispatch chain ended above (both packets' Keeps* are false) and the atomic
		// batch was just closed. If the GPU has run out of work, it gets what was recorded
		// so far now, not when the guest's command buffer ends (KYTY_EARLY_SUBMIT).
		if (!KeepsQueuedDraw(packet_header) && !KeepsDispatchChain(packet_header)) {
			GetScheduler().SubmitIfGpuIdle();
		}
	}
}

[[gnu::always_inline]] bool CommandProcessor::RunHandler(Pm4Execution& execution, const PacketView& view) {
	auto&       cursor        = execution.m_buffer_stack.back();
	const auto* packet        = view.packet;
	const auto  packet_header = view.header;
	const auto  opcode        = view.opcode;
	const auto  total_dw      = view.total_dw;
	const auto  remaining_dw  = view.remaining_dw;
	auto        handler       = g_cp_op_func[opcode];
	if (handler == nullptr) {
		const auto offset = cursor.offset_dw;
		LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
		     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(packet - offset), total_dw, offset,
		     reinterpret_cast<uint64_t>(packet));
		const auto  dump_begin = (offset > 8 ? offset - 8 : 0);
		const auto  dump_end   = std::min<uint32_t>(total_dw, offset + 16);
		auto* const base       = packet - offset;
		for (uint32_t i = dump_begin; i < dump_end; i++) {
			LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), base[i]);
		}
		EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n", offset, packet_header);
	}

	// Only dispatches from the packet loop chain: it ends chains at synchronizing packets.
	m_in_dispatch_packet = opcode == Pm4::IT_DISPATCH_DIRECT;
	const auto packet_dw =
	    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
	m_in_dispatch_packet = false;
	EXIT_IF(packet_dw > remaining_dw);
	if (execution.m_suspended) {
		return false;
	}
	// The handler may have pushed a buffer (a call): advance the packet's own cursor.
	auto& own = execution.m_buffer_stack.back();
	EXIT_IF(&own != &cursor);
	cursor.offset_dw += packet_dw;
	execution.m_made_progress = true;
	if (!execution.m_next_buffer.empty()) {
		// Chains and taken branches reuse the fetcher; only calls retain a return cursor.
		if (execution.m_chain) {
			cursor = {execution.m_next_buffer};
		} else {
			execution.m_buffer_stack.push_back({execution.m_next_buffer});
		}
		execution.m_next_buffer = {};
	}
	return true;
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	if (num_instances == 0) {
		num_instances = 1;
	}

	m_num_instances      = num_instances;
	m_num_instances_addr = 0;
}

uint32_t CommandProcessor::NumInstances() {
	if (m_num_instances_addr == NumInstancesFromCount) {
		// The last record of the multi-draw (none drawn: NUM_INSTANCES unchanged).
		const auto count = std::min(uint32_t {*reinterpret_cast<const volatile uint32_t*>(m_num_instances_count_addr)},
		                            m_num_instances_max);
		m_num_instances_addr = count == 0 ? 0
		                                  : m_num_instances_records +
		                                        static_cast<uint64_t>(count - 1) * m_num_instances_stride +
		                                        offsetof(vk::DrawIndirectCommand, instanceCount);
	}
	if (m_num_instances_addr != 0) {
		m_num_instances      = *reinterpret_cast<const uint32_t*>(m_num_instances_addr);
		m_num_instances_addr = 0;
	}
	return m_num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address) {
	uint64_t value = 0;

	switch (op) {
		case 0x00:
			m_predicate_skip = false;
			return;
		case 0x01: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			m_renderer.GetOcclusionQueries().Publish();
			// One begin/end pair per DB; bit 63 marks each counter ready.
			constexpr uint64_t ready_bit = 1ull << 63u;
			const auto* results = reinterpret_cast<const volatile uint64_t*>(address);
			for (uint32_t db = 0; db < 16u; db++) {
				const auto begin = results[db * 2u];
				const auto end   = results[db * 2u + 1u];
				if ((begin & end & ready_bit) == 0) {
					if (wait_op == 0) {
						SuspendPm4();
					} else {
						m_predicate_skip = false;
					}
					return;
				}
				value += end - begin;
			}
		} break;
		case 0x03:
			// The wait selector applies only to Z-pass query readiness.
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			value = *reinterpret_cast<const volatile uint64_t*>(address);
			break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
	switch (condition) {
		case 0x00: m_predicate_skip = (value != 0); break;
		case 0x01: m_predicate_skip = (value == 0); break;
		default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
	}
}

// Frame skipping (frameSkip.h): classifies the draw, and drops it in a skipped frame. A dropped
// draw inside an open occlusion query counts as visible.
bool CommandProcessor::SkipOcclusionProxy() {
	auto& executor = m_renderer.GetRenderExecutor();
	auto& stat     = executor.m_proxy_stat;
	stat.in_query++;
	if (!RenderExecutor::ProxyRegisters(m_ctx, m_ucfg)) {
		stat.renders++;
		return false;
	}
	switch (executor.ProxyShaderClass(RenderExecutor::ProxyShaderKey(m_ctx, m_sh_ctx))) {
		case RenderExecutor::ProxyShaders::Unknown: stat.refused_unknown++; return false;
		case RenderExecutor::ProxyShaders::Writes: stat.refused_writes++; return false;
		case RenderExecutor::ProxyShaders::Clean: break;
	}
	if (RenderExecutor::ProxySkipMode() == 2) {
		return false; // rendered; the commit checks the shaders again
	}
	// The draw's only effect would be the query's count, which is synthetic (always visible):
	// nothing to render. Its registers stay as the packets left them.
	stat.skipped++;
	return true;
}

bool CommandProcessor::SkipDraw(bool indirect, bool indirect_count) {
	if (!FrameSkip::OnDraw(m_ctx, m_sh_ctx, indirect, indirect_count)) {
		return false;
	}
	m_renderer.GetOcclusionQueries().NoteSkippedDraw();
	return true;
}

void CommandProcessor::DrawIndex(DrawIndexArgs args) {
	auto& executor           = m_renderer.GetRenderExecutor();
	args.index_type_and_size = m_index_type_and_size;
	if (m_shadow) {
		if (args.instance_count == 0) {
			args.instance_count = m_num_instances;
		}
		m_shadow_draw = {.valid = true, .indexed = true, .index = args};
		return;
	}
	if (OnDecodeThread()) {
		// A draw reading its instance count from memory, frame skipping (its tables are the GPU
		// thread's), a possible occlusion proxy (likewise) and one that cannot be queued run on
		// the GPU thread.
		if ((args.instance_count == 0 && m_num_instances_addr != 0) || FrameSkip::Active() ||
		    (m_synthetic_occlusion_open != 0 && RenderExecutor::ProxySkipMode() != 0)) {
			m_decode_bail = true;
			return;
		}
		if (args.instance_count == 0) {
			args.instance_count = m_num_instances;
		}
		m_decode_bail = !executor.TryQueueDrawIndex({m_ctx, m_ucfg, m_sh_ctx}, args);
		return;
	}
	if (args.instance_count == 0) {
		if (m_num_instances_addr != 0) {
			// Reads guest memory a queued draw may write.
			executor.FinishQueuedDraws(CurrentBuffer(), RenderExecutor::DrainNumInstances);
		}
		args.instance_count = NumInstances();
	}
	// After the instance count: a dropped draw leaves the same register state behind.
	if (FrameSkip::Active() && SkipDraw(false, false)) {
		return;
	}
	if (m_synthetic_occlusion_open != 0 && RenderExecutor::ProxySkipMode() != 0 &&
	    SkipOcclusionProxy()) {
		return;
	}
	executor.QueueDrawIndex(CurrentBuffer(), args);
	// After the queue call: a direct fallback drains the previous target's draws first.
	if (PassDiag::Enabled()) {
		PassDiag::OnDraw(CurrentBuffer().GetRegisters(), CurrentBuffer().GetShaders(), 0);
	}
}

const void* CommandProcessor::IndexAddress(uint32_t first_index) const {
	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}
	return reinterpret_cast<const void*>(m_index_base_addr + first_index * index_size);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	DrawIndex({.index_count = index_count, .index_addr = IndexAddress(index_offset)});
}

static void PatchIndirectDrawOffsets(CommandProcessor& cp, IndirectDrawRegisters registers,
                                     uint32_t vertex_offset, uint32_t instance_offset,
                                     uint32_t first_index, uint32_t draw_index) {
	const auto write = [&](uint32_t location, uint32_t value) {
		if (location == Pm4::SH_NOP) {
			return;
		}
		EXIT_NOT_IMPLEMENTED(location >= Pm4::SH_NUM ||
		                     g_hw_sh_indirect_func[location] == nullptr);
		g_hw_sh_indirect_func[location](cp, location, value);
	};
	write(registers.vertex_offset, vertex_offset);
	write(registers.instance_offset, instance_offset);
	write(registers.index_offset, first_index);
	write(registers.draw_index, draw_index);
}

// Writes the GS user-data slots the packet patches; false when the shaders cannot read the
// offsets from GPU records: a first-index or subdraw-index destination, or another stage's
// user data.
static bool IndirectDrawOffsets(IndirectDrawRegisters registers, ShaderIndirectOffsets& offsets) {
	if (registers.index_offset != Pm4::SH_NOP || registers.draw_index != Pm4::SH_NOP) {
		return false;
	}
	for (const auto [location, slot]: {std::pair {registers.vertex_offset, &offsets.vertex},
	                                   std::pair {registers.instance_offset, &offsets.instance}}) {
		if (location == Pm4::SH_NOP) {
			continue;
		}
		const uint32_t index = location - Pm4::SPI_SHADER_USER_DATA_GS_0;
		if (index >= HW::UserSgprInfo::SGPRS_MAX) {
			return false;
		}
		*slot = static_cast<uint8_t>(index);
	}
	return true;
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, IndirectDrawRegisters registers,
                                    uint32_t draw_initiator, bool indexed) {
	const auto args_size =
	    indexed ? sizeof(vk::DrawIndexedIndirectCommand) : sizeof(vk::DrawIndirectCommand);
	DrawIndirectMulti(data_offset, 1, nullptr, args_size, registers, draw_initiator, indexed);
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, IndirectDrawRegisters registers,
                                         uint32_t draw_initiator, bool indexed) {
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != (indexed ? 0u : 2u));
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);
	// The guest records match Vulkan's indirect commands byte for byte.
	EXIT_NOT_IMPLEMENTED((stride_in_bytes & 3u) != 0 ||
	                     stride_in_bytes < (indexed ? sizeof(vk::DrawIndexedIndirectCommand)
	                                                : sizeof(vk::DrawIndirectCommand)));

	auto&      executor = m_renderer.GetRenderExecutor();
	const auto args_addr = m_draw_indirect_args_base_addr + data_offset;
	Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectDraws);
	// KYTY_INDIRECT_QUEUE: queued and prepared like a direct draw, the record count (if it is in
	// memory) read by the GPU when it draws: by construction the value the GPU work before it
	// wrote. The packet kept the queue (RunPrologue). With the decode thread the packet is a sync
	// on the GPU thread (not KeepsQueuedDraw), drawn at once as before.
	if (const int mode = executor.IndirectQueueMode();
	    mode != 0 && !FrameSkip::Active() && m_decode == nullptr) {
		DrawIndirectArgs records {args_addr, max_count_or_count, stride_in_bytes};
		const auto&      graphics = m_renderer.GetGraphics();
		const bool       offsets  = IndirectDrawOffsets(registers, records.offsets);
		auto&            buffer   = CurrentBuffer();
		const bool       fits     = max_count_or_count <= graphics.max_draw_indirect_count &&
		                     (!indexed || m_index_buffer_size != 0) &&
		                     executor.IndirectRecordsOnGpu(buffer.GetRegisters(), buffer.GetUserConfig(),
		                                                   buffer.GetShaders());
		if (offsets && fits) {
			if (max_count_or_count == 0) {
				return;
			}
			if (count_addr != nullptr) {
				records.count_address = reinterpret_cast<uint64_t>(count_addr);
				if (mode == 2) {
					// What the decode-time read gives (as without the queue), for the commit's check.
					executor.FinishQueuedDraws(CurrentBuffer(), RenderExecutor::DrainIndirectCount);
					records.decoded_count = std::min(uint32_t {*count_addr}, max_count_or_count);
				}
				m_num_instances_addr       = NumInstancesFromCount;
				m_num_instances_count_addr = records.count_address;
				m_num_instances_records    = args_addr;
				m_num_instances_stride     = stride_in_bytes;
				m_num_instances_max        = max_count_or_count;
				Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectGpuCount);
			} else {
				m_num_instances_addr = args_addr +
				                       static_cast<uint64_t>(max_count_or_count - 1) * stride_in_bytes +
				                       offsetof(vk::DrawIndirectCommand, instanceCount);
			}
			Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectQueued);
			if (indexed) {
				executor.QueueDrawIndex(CurrentBuffer(), {.index_count         = m_index_buffer_size,
				                                          .index_addr          = IndexAddress(0),
				                                          .index_type_and_size = m_index_type_and_size,
				                                          .indirect            = records});
			} else {
				executor.QueueDrawAuto(CurrentBuffer(), {.indirect = records});
			}
			return;
		}
		Common::FrameDiag::Add(offsets ? Common::FrameDiag::Counter::IndirectUnqueued
		                               : Common::FrameDiag::Counter::IndirectPatched);
	} else {
		Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectUnqueued);
	}
	// Drawn at once with the count read here: the draws queued before it go first.
	executor.FinishQueuedDraws(CurrentBuffer(), RenderExecutor::DrainIndirectCount);

	const auto draw_count = count_addr == nullptr
	                            ? max_count_or_count
	                            : std::min(uint32_t {*count_addr}, max_count_or_count);
	if (draw_count == 0) {
		return;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::IndirectDraws, draw_count);

	// NUM_INSTANCES keeps the last draw's instance count.
	m_num_instances_addr = args_addr + static_cast<uint64_t>(draw_count - 1) * stride_in_bytes +
	                       offsetof(vk::DrawIndirectCommand, instanceCount);
	if (FrameSkip::Active()) {
		// Classified only: frame skipping never drops an indirect draw.
		(void)SkipDraw(true, count_addr != nullptr);
	}
	// The GPU reads the records if the shaders allow; the VS prolog removes Vulkan's bases
	// from the index VGPRs and copies them only to the user SGPRs the packet patches.
	DrawIndirectArgs records {args_addr, draw_count, stride_in_bytes};
	if (PassDiag::Enabled()) {
		PassDiag::OnDraw(CurrentBuffer().GetRegisters(), CurrentBuffer().GetShaders(), 2);
	}
	if (IndirectDrawOffsets(registers, records.offsets)) {
		if (!indexed && executor.DrawAuto(CurrentBuffer(), {.indirect = records})) {
			return;
		}
		if (indexed && m_index_buffer_size != 0 &&
		    executor.DrawIndex(CurrentBuffer(), {.index_count         = m_index_buffer_size,
		                                         .index_addr          = IndexAddress(0),
		                                         .index_type_and_size = m_index_type_and_size,
		                                         .indirect            = records})) {
			return;
		}
	}

	const uint32_t max_indices = m_index_buffer_size != 0 ? m_index_buffer_size : UINT32_MAX;
	for (uint32_t i = 0; i < draw_count; i++) {
		const auto addr = args_addr + static_cast<uint64_t>(i) * stride_in_bytes;
		if (!indexed) {
			const auto& args = *reinterpret_cast<const vk::DrawIndirectCommand*>(addr);
			PatchIndirectDrawOffsets(*this, registers, args.firstVertex, args.firstInstance, 0, i);
			executor.DrawAuto(CurrentBuffer(), {.vertex_count   = args.vertexCount,
			                                    .instance_count = args.instanceCount,
			                                    .first_vertex   = args.firstVertex,
			                                    .first_instance = args.firstInstance,
			                                    .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		const auto& args = *reinterpret_cast<const vk::DrawIndexedIndirectCommand*>(addr);
		PatchIndirectDrawOffsets(*this, registers, static_cast<uint32_t>(args.vertexOffset),
		                         args.firstInstance, args.firstIndex, i);
		executor.DrawIndex(CurrentBuffer(), {.index_count = std::min(args.indexCount, max_indices),
		                                     .index_addr  = IndexAddress(args.firstIndex),
		                                     .instance_count      = args.instanceCount,
		                                     .index_type_and_size = m_index_type_and_size,
		                                     .base_vertex         = args.vertexOffset,
		                                     .first_instance      = args.firstInstance,
		                                     .offset_source = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchDirect(CurrentBuffer(), thread_group_x, thread_group_y,
	                                              thread_group_z, mode, IsAsyncComputeQueue(),
	                                              m_in_dispatch_packet);
}

void CommandProcessor::ProcessPssr(const Pssr::Request& request) {
	EXIT_NOT_IMPLEMENTED(IsAsyncComputeQueue());
	if (FrameSkip::SkippingDraws()) {
		return;
	}
	m_renderer.GetRenderExecutor().ProcessPssr(CurrentBuffer(), request);
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	if ((mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		const auto* args = reinterpret_cast<const vk::DispatchIndirectCommand*>(args_addr);
		DispatchDirect(args->x, args->y, args->z, mode);
		return;
	}
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchIndirect(CurrentBuffer(), args_addr, mode,
	                                                IsAsyncComputeQueue());
}

void CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	auto& executor = m_renderer.GetRenderExecutor();
	if (m_shadow) {
		if (args.instance_count == 0) {
			args.instance_count = m_num_instances;
		}
		m_shadow_draw = {.valid = true, .indexed = false, .automatic = args};
		return;
	}
	if (OnDecodeThread()) {
		if ((args.instance_count == 0 && m_num_instances_addr != 0) || FrameSkip::Active() ||
		    (m_synthetic_occlusion_open != 0 && RenderExecutor::ProxySkipMode() != 0)) {
			m_decode_bail = true;
			return;
		}
		if (args.instance_count == 0) {
			args.instance_count = m_num_instances;
		}
		m_decode_bail = !executor.TryQueueDrawAuto({m_ctx, m_ucfg, m_sh_ctx}, args);
		return;
	}
	if (args.instance_count == 0) {
		if (m_num_instances_addr != 0) {
			executor.FinishQueuedDraws(CurrentBuffer(), RenderExecutor::DrainNumInstances);
		}
		args.instance_count = NumInstances();
	}
	if (FrameSkip::Active() && SkipDraw(false, false)) {
		return;
	}
	if (m_synthetic_occlusion_open != 0 && RenderExecutor::ProxySkipMode() != 0 &&
	    SkipOcclusionProxy()) {
		return;
	}
	executor.QueueDrawAuto(CurrentBuffer(), args);
	// After the queue call: a direct fallback drains the previous target's draws first.
	if (PassDiag::Enabled()) {
		PassDiag::OnDraw(CurrentBuffer().GetRegisters(), CurrentBuffer().GetShaders(), 1);
	}
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	m_renderer.GetVideoOut().WaitFlipDone(static_cast<int>(video_out_handle),
	                                      static_cast<int>(display_buffer_index));
}

void CommandProcessor::StoreLabel(void* address, const void* value, uint64_t size) {
	// A label beside GPU-written bytes goes to the backing store, so they are not downloaded.
	// Other labels go through the guest mapping: its fault keeps a clean cached copy current and
	// downloads GPU-written bytes under the label before the store.
	// In an asynchronously tracked page a label goes to the backing too (StoreCpLabel), so
	// waits on it can be told apart from guest stores.
	const auto vaddr   = reinterpret_cast<uint64_t>(address);
	auto&      buffers = m_renderer.GetBufferCache();
	if (GuestRange {vaddr, size}.Valid() && !buffers.HasGpuDirtyBytes(vaddr, size)) {
		const bool gpu_modified = buffers.IsRegionGpuModified(vaddr, size);
		if (m_renderer.StoreCpLabel(vaddr, value, size, !gpu_modified) ||
		    (gpu_modified && [&] {
			    uint64_t stored = 0;
			    std::memcpy(&stored, value, size);
			    return LibKernel::Memory::TryStoreLabelBacking(vaddr, stored, size);
		    }())) {
			return;
		}
	}
	std::memcpy(address, value, size);
	m_renderer.NoteGpuWrite(vaddr, size);
}

void CommandProcessor::ReleaseMem(uint32_t data_sel, uint32_t interrupt_selector, void* address,
                                  uint64_t data, uint32_t interrupt_context_id) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::ReleaseMem()\n"
		     "\t data_sel           = %" PRIu32 "\n"
		     "\t interrupt_selector = %" PRIu32 "\n"
		     "\t address            = 0x%016" PRIx64 "\n"
		     "\t data               = 0x%016" PRIx64 "\n"
		     "\t interrupt_context  = 0x%08" PRIx32 "\n",
		     data_sel, interrupt_selector, reinterpret_cast<uint64_t>(address), data,
		     interrupt_context_id);
	}

	// QueuedInterrupt: kNone, kOnly, kAfterWriteConfirm, kDataAfterWriteConfirm, kSendIntCtx.
	EXIT_NOT_IMPLEMENTED(interrupt_selector > 4);
	const bool interrupt = (interrupt_selector != 0 && interrupt_selector != 3);
	// kSendIntCtx has no label, and kOnly writes none on graphics queues.
	if (address == nullptr || interrupt_selector == 4 ||
	    (interrupt_selector == 1 && !IsAsyncComputeQueue())) {
		data_sel = 0;
	}
	if (data_sel == 0 && !interrupt) {
		return;
	}

	// Dumped occlusion counters are written before any end-of-pipe signal reaches the guest.
	m_renderer.GetOcclusionQueries().Publish();

	const auto write_label = [this, address](const void* value, uint64_t size) {
		StoreLabel(address, value, size);
	};
	if (data_sel != 0) {
		MemDiag::NoteLabel();
	}
	// EndOfPipeData: kNone, kImmediate32b, kImmediate64b, kRefClk; 5 reads GDS.
	switch (data_sel) {
		case 0: break;
		case 1: {
			const auto value = static_cast<uint32_t>(data);
			write_label(&value, sizeof(value));
			break;
		}
		case 3: data = Sync::ReadReferenceClock(); [[fallthrough]];
		case 2: write_label(&data, sizeof(data)); break;
		case 5: {
			GetScheduler().Finish();
			const auto gds = static_cast<uint32_t>(data);
			Sync::ReadGds(*m_renderer.GetBufferCache().GetGdsBuffer(),
			              static_cast<uint32_t*>(address), gds & 0xffffu, gds >> 16u);
			m_renderer.NoteGpuWrite(reinterpret_cast<uint64_t>(address),
			                        uint64_t {gds >> 16u} * sizeof(uint32_t));
			break;
		}
		default: EXIT("unknown release_mem data selector: %" PRIu32 "\n", data_sel);
	}

	// GuestGpu::Process's end-of-slice flush submits the work this interrupt waits on.
	if (interrupt) {
		GetScheduler().DeferPriorityOperation(
		    [&renderer = m_renderer, event_id = m_interrupt_event_id, interrupt_context_id] {
			    renderer.TriggerInterrupt(event_id, interrupt_context_id);
		    });
	}
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index,
                                    uint64_t event_address) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n"
		     "\t address     = 0x%016" PRIx64 "\n",
		     event_type, event_index, event_address);
	}

	if (PassDiag::Enabled()) {
		PassDiag::OnEvent(event_type);
	}
	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	// Partial flushes and cache writebacks only order GPU work, which the renderer's resource
	// barriers already order.
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate, DbDataWritebackInvalidate,
		// DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			break;
		case 0x0000000d:
		case 0x0000000e:
		case 0x00000012:
		case 0x00000017:
		case 0x00000018:
		case 0x00000019:
		case 0x0000001a:
		case 0x0000001b:
		case 0x00000038:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		case 0x00000039: {
			if (event_index != 0x00000001 || event_address == 0 || (event_address & 0x7u) != 0) {
				EXIT("invalid occlusion-counter dump: index=0x%08" PRIx32 ", address=0x%016" PRIx64
				     "\n",
				     event_index, event_address);
			}
			if (auto& queries = m_renderer.GetOcclusionQueries(); queries.Enabled()) {
				queries.Dump(event_address);
				break;
			}
			static std::once_flag warning_once;
			std::call_once(warning_once, [] {
				std::printf("Warning: game uses occlusion queries, which are treated as always "
				            "visible without --occlusion-queries; GPU usage may be higher and FPS "
				            "may be lower.\n");
			});

			// Begins dump to the results, ends 8 bytes into them (OcclusionQueries::Dump).
			if ((event_address & 8u) == 0) {
				m_synthetic_occlusion_open++;
			} else if (m_synthetic_occlusion_open != 0) {
				m_synthetic_occlusion_open--;
			}
			// Without host occlusion queries, publish an always-visible result. The PS5 layout
			// contains one interleaved begin/end pair per DB, and bit 63 marks a result ready.
			constexpr uint64_t ready_bit    = 1ull << 63u;
			constexpr uint64_t counter_mask = ready_bit - 1u;
			auto*              results      = reinterpret_cast<volatile uint64_t*>(event_address);
			const auto         value        = ready_bit | m_synthetic_occlusion_counter;
			for (uint32_t db = 0; db < 16u; db++) {
				results[db * 2u] = value;
			}
			m_renderer.NoteGpuWrite(event_address, 16u * 2u * sizeof(uint64_t));
			m_synthetic_occlusion_counter = (m_synthetic_occlusion_counter + 1u) & counter_mask;
			break;
		}
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip(int handle, int index, int flip_mode, int64_t flip_arg) {
	if (graphics_debug_dump_enabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}
	if (m_renderer.GetRenderExecutor().m_proxy_stat.in_query != 0) {
		m_renderer.GetRenderExecutor().PrintProxyStats();
	}
	m_renderer.GetOcclusionQueries().Publish();
	if (MemDiag::BdaStatsMode() != 0) {
		MemDiag::g_flips.fetch_add(1, std::memory_order_relaxed);
	}

	auto& command = CurrentBuffer();
	auto  request = Sync::PrepareVideoOutFlip(command, handle, index, flip_mode, flip_arg);
	if (Common::GpuWaitDiag::Enabled()) {
		const auto tick = GetScheduler().CurrentTick();
		Common::GpuWaitDiag::NoteFlip(tick);
		GetScheduler().DeferPriorityOperation([&renderer = m_renderer, request, tick] {
			Common::GpuWaitDiag::NoteFlipDone(tick);
			renderer.GetVideoOut().CompleteFlip(request);
		});
	} else {
		GetScheduler().DeferPriorityOperation(
		    [&renderer = m_renderer, request] { renderer.GetVideoOut().CompleteFlip(request); });
	}
	GetScheduler().Flush();
	if (FrameSkip::Active()) {
		std::array<uint64_t, 16> buffers {};
		const int count = m_renderer.GetVideoOut().DisplayBufferAddresses(
		    handle, buffers.data(), static_cast<int>(buffers.size()));
		FrameSkip::OnFlip(buffers.data(), count);
	}
	if (Common::FrameDiag::Enabled()) {
		Common::FrameDiag::OnGuestFlip(PerformanceOverlayFlipCount());
	}
	if (PassDiag::Enabled()) {
		PassDiag::OnGuestFlip(PerformanceOverlayFlipCount());
	}
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	auto& command = CurrentBuffer();
	m_renderer.GetVideoOut().PrepareFlip(request_id, command);
	GetScheduler().DeferPriorityOperation(
	    [this, request_id] { m_renderer.GetVideoOut().CompleteFlip(request_id); });
	GetScheduler().Flush();
}

// ---------------------------------------------------------------------------------------------
// Decode thread, stage 1 (KYTY_DECODE_THREAD=1; docs/mt-rewrite/decode-thread.md).
//
// The decode thread runs the packet loop across the packets that keep the draw queue (register
// writes, indirect register loads, index state, indirect buffers, queueable draws, markers) and
// posts the queued draws. Any other packet, a draw that cannot be queued, pending guest commands
// or a pending collection make it stop at a sync: the GPU thread commits everything posted, then
// runs the packet (its prologue and handler) with the decode thread stopped, then lets it go on.
// The GPU thread owns the CommandBuffer and everything that records; the decode thread owns the
// live registers and the posting side of the queue. Ownership changes hands only at a sync.
//
// Deadlock rules: the GPU thread never waits on the decode thread without serving guest commands
// (a decode-time read of a GPU-written page faults on the decode thread and waits for one), and
// the decode thread checks for guest commands at every packet boundary.

bool CommandProcessor::OnDecodeThread() noexcept {
	return g_on_decode_thread;
}

static uint64_t DecodeNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// KYTY_DECODE_THREAD: 0 (default) off, 1 on, verify: on with the shadow decode, the per-draw
// digest, the sync order and the ownership asserts.
static int DecodeThreadMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_DECODE_THREAD");
		if (value == nullptr) return 0;
		if (std::string_view(value) == "verify") return 2;
		return value[0] == '1' ? 1 : 0;
	}();
	return mode;
}

static uint64_t DecodeHash(const void* data, size_t size, uint64_t hash = 1469598103934665603ull) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; i++) {
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	}
	return hash;
}

// The guest bytes an indirect register load reads (its hash tells a guest race from a bug).
static uint64_t IndirectSourceHash(const uint32_t* packet) {
	const auto opcode = (packet[0] >> 8u) & 0xffu;
	if (opcode != Pm4::IT_SET_CONTEXT_REG_INDIRECT && opcode != Pm4::IT_SET_SH_REG_INDIRECT &&
	    opcode != Pm4::IT_SET_UCONFIG_REG_INDIRECT) {
		return 0;
	}
	const auto* source = reinterpret_cast<const uint8_t*>((static_cast<uint64_t>(packet[1]) & 0xfffffffcu) |
	                                                      (static_cast<uint64_t>(packet[2]) << 32u));
	const auto  count  = packet[4] & 0x3fffu;
	return source == nullptr ? 0 : DecodeHash(source, uint64_t {count} * 8u);
}

static std::string UserConfigDiff(const HW::UserConfig& a, const HW::UserConfig& b) {
	std::string diff;
	const auto  note = [&](bool same, const char* what) {
		if (!same) {
			diff += diff.empty() ? what : std::string(" ") + what;
		}
	};
	note(a.GetPrimType() == b.GetPrimType(), "prim_type");
	note(a.GetIndexOffset() == b.GetIndexOffset(), "index_offset");
	note(a.GetObjectId() == b.GetObjectId(), "object_id");
	note(a.GetPrimitiveResetControl() == b.GetPrimitiveResetControl(), "reset_control");
	note(a.GetGeControl().primitive_group_size == b.GetGeControl().primitive_group_size &&
	         a.GetGeControl().vertex_group_size == b.GetGeControl().vertex_group_size,
	     "ge_cntl");
	note(a.GetGeUserVgprEn().vgpr1 == b.GetGeUserVgprEn().vgpr1 &&
	         a.GetGeUserVgprEn().vgpr2 == b.GetGeUserVgprEn().vgpr2 &&
	         a.GetGeUserVgprEn().vgpr3 == b.GetGeUserVgprEn().vgpr3,
	     "ge_user_vgpr_en");
	return diff.empty() ? diff : "user config (" + diff + ")";
}

static std::string BytesDiff(const void* a, const void* b, size_t size, const char* what) {
	const auto* x = static_cast<const uint8_t*>(a);
	const auto* y = static_cast<const uint8_t*>(b);
	for (size_t i = 0; i < size; i++) {
		if (x[i] != y[i]) {
			char text[96];
			std::snprintf(text, sizeof(text), "%s (byte %zu of %zu: 0x%02x vs 0x%02x)", what, i, size,
			              x[i], y[i]);
			return text;
		}
	}
	return {};
}

void CommandProcessor::CopyDecodeState(const CommandProcessor& from) {
	m_ctx                              = from.m_ctx;
	m_saved_ctx                        = from.m_saved_ctx;
	m_ucfg                             = from.m_ucfg;
	m_sh_ctx                           = from.m_sh_ctx;
	m_index_type_and_size              = from.m_index_type_and_size;
	m_index_buffer_size                = from.m_index_buffer_size;
	m_index_base_addr                  = from.m_index_base_addr;
	m_draw_indirect_args_base_addr     = from.m_draw_indirect_args_base_addr;
	m_dispatch_indirect_args_base_addr = from.m_dispatch_indirect_args_base_addr;
	m_num_instances                    = from.m_num_instances;
	m_num_instances_addr               = from.m_num_instances_addr;
	m_predicate_skip                   = from.m_predicate_skip;
}

std::string CommandProcessor::DecodeStateDiff(const CommandProcessor& other) const {
	std::string diff;
	const auto  note = [&](bool same, const char* what) {
		if (!same) {
			diff += diff.empty() ? what : std::string(", ") + what;
		}
	};
	note(m_ctx.SameRegisters(other.m_ctx), "context registers");
	note(m_saved_ctx.has_value() == other.m_saved_ctx.has_value() &&
	         (!m_saved_ctx.has_value() || m_saved_ctx->SameRegisters(*other.m_saved_ctx)),
	     "saved context");
	if (const auto d = UserConfigDiff(m_ucfg, other.m_ucfg); !d.empty()) {
		diff += diff.empty() ? d : ", " + d;
	}
	if (const auto d = BytesDiff(&m_sh_ctx, &other.m_sh_ctx, sizeof(m_sh_ctx), "shader registers");
	    !d.empty()) {
		diff += diff.empty() ? d : ", " + d;
	}
	note(m_index_type_and_size == other.m_index_type_and_size &&
	         m_index_buffer_size == other.m_index_buffer_size &&
	         m_index_base_addr == other.m_index_base_addr,
	     "index state");
	note(m_num_instances == other.m_num_instances &&
	         m_num_instances_addr == other.m_num_instances_addr,
	     "instances");
	note(m_draw_indirect_args_base_addr == other.m_draw_indirect_args_base_addr &&
	         m_dispatch_indirect_args_base_addr == other.m_dispatch_indirect_args_base_addr,
	     "indirect bases");
	note(m_predicate_skip == other.m_predicate_skip, "predication");
	return diff;
}

std::string CommandProcessor::ShadowDrawDiff(const CommandProcessor& shadow, uint64_t sequence) const {
	auto&       executor = m_renderer.GetRenderExecutor();
	const auto& queued   = *executor.m_queued_draws[(sequence - 1) % executor.m_queued_draws.size()];
	const auto& draw     = shadow.m_shadow_draw;
	std::string diff;
	const auto  note = [&](bool same, const char* what) {
		if (!same) {
			diff += diff.empty() ? what : std::string(", ") + what;
		}
	};
	note(queued.sequence == sequence, "slot sequence");
	note(draw.valid, "no draw replayed");
	if (!diff.empty()) {
		return diff;
	}
	note(queued.registers->SameRegisters(shadow.m_ctx), "context registers");
	if (const auto d = UserConfigDiff(queued.user_config, shadow.m_ucfg); !d.empty()) {
		diff += diff.empty() ? d : ", " + d;
	}
	if (const auto d =
	        BytesDiff(&queued.shaders, &shadow.m_sh_ctx, sizeof(queued.shaders), "shader registers");
	    !d.empty()) {
		diff += diff.empty() ? d : ", " + d;
	}
	note(queued.indexed == draw.indexed, "kind");
	if (queued.indexed && draw.indexed) {
		const auto& a = queued.index_args;
		const auto& b = draw.index;
		note(a.index_count == b.index_count && a.index_addr == b.index_addr &&
		         a.instance_count == b.instance_count &&
		         a.index_type_and_size == b.index_type_and_size &&
		         a.base_vertex == b.base_vertex && a.first_instance == b.first_instance &&
		         a.offset_source == b.offset_source,
		     "index draw arguments");
	} else if (!queued.indexed && !draw.indexed) {
		const auto& a = queued.auto_args;
		const auto& b = draw.automatic;
		note(a.vertex_count == b.vertex_count && a.instance_count == b.instance_count &&
		         a.first_vertex == b.first_vertex && a.first_instance == b.first_instance &&
		         a.offset_source == b.offset_source,
		     "auto draw arguments");
	}
	return diff;
}

class DecodeThread {
public:
	enum class State : uint32_t { Idle, Running, Sync, SyncDone, Finished, Exit };

	explicit DecodeThread(CommandProcessor& cp): m_cp(cp) {
		if (const char* spin = std::getenv("KYTY_DECODE_SPIN_US"); spin != nullptr) {
			handoff.spin_ns = std::strtoull(spin, nullptr, 10) * 1000;
		}
		m_stats_enabled = std::getenv("KYTY_DECODE_STATS") != nullptr;
		verify          = DecodeThreadMode() == 2;
		handoff.verify  = verify;
		handoff.asserts = verify || DrawPrepare::Chaos::Enabled();
		if (verify) {
			shadow.reset(new CommandProcessor(cp.m_renderer, cp.m_interrupt_event_id));
			shadow->m_shadow = true;
		}
		// KYTY_DECODE_RUN_UNTIL_DRAW=0: hand every packet back to the decode thread after a sync.
		const char* until = std::getenv("KYTY_DECODE_RUN_UNTIL_DRAW");
		m_run_until_draw  = until == nullptr || until[0] != '0';
		m_thread        = std::thread([this] { Loop(); });
	}
	~DecodeThread() {
		state.store(State::Exit, std::memory_order_release);
		handoff.to_decode.Notify();
		m_thread.join();
	}
	KYTY_CLASS_NO_COPY(DecodeThread);

	// GPU thread: runs `execution`'s packet loop on the decode thread and commits until it ends.
	void Run(Pm4Execution& execution, GuestGpu* gpu);

	DecodeHandoff      handoff;
	std::atomic<State> state {State::Idle};
	// The sync request (decode thread → GPU thread), and its outcome.
	CommandProcessor::PacketView view {};
	bool                         has_view     = false;
	uint64_t                     requested_ns = 0;
	bool                         stop         = false;
	// A dispatch may have left a barrier pending (EndDispatchChain): the next packet that ends a
	// chain runs on the GPU thread.
	bool          chain_open = false;
	GuestGpu*     gpu        = nullptr;
	Pm4Execution* execution  = nullptr;

	// KYTY_DECODE_THREAD=verify: the decode thread's log of the packets it ran (and the draw each
	// posted) and of its syncs, replayed by a shadow processor on the GPU thread.
	struct VerifyEntry {
		const uint32_t* packet;
		uint32_t        remaining_dw;
		uint32_t        total_dw;
		uint64_t        sequence;    // the draw it posted, or 0
		uint64_t        source_hash; // indirect register loads: the bytes read
		bool            sync;        // a sync request (packet: its packet, or null for commands)
	};
	bool                              verify = false;
	std::mutex                        verify_mutex;
	std::deque<VerifyEntry>           verify_log;
	std::unique_ptr<CommandProcessor> shadow;
	bool                              shadow_diverged = false;
	uint64_t verify_draws = 0, verify_packets = 0, verify_syncs = 0, verify_races = 0;
	void LogVerify(const VerifyEntry& entry) {
		std::lock_guard lock(verify_mutex);
		verify_log.push_back(entry);
	}
	// GPU thread: replays the log through draw `through` (0: to the next sync entry, which it
	// returns through `sync`).
	void VerifyReplay(uint64_t through, const VerifyEntry** sync);
	void VerifySeed();

	// KYTY_DECODE_STATS=1: DECODE lines (GPU thread prints).
	struct Stats {
		std::atomic<uint64_t> syncs {0}, sync_commands {0}, sync_bails {0};
		// By reason: 0..255 opcode, 256 + R code of a NOP, 512 collection, 513 dispatch chain.
		std::array<std::atomic<uint64_t>, 520> by_reason {};
		std::atomic<uint64_t> sync_wait_ns {0};  // decode thread: request to release
		std::atomic<uint64_t> ring_wait_ns {0};  // decode thread: waiting for a free slot
		uint64_t              exec_ns       = 0; // GPU thread: running the syncs
		uint64_t              seen_ns       = 0; // GPU thread: request to start (when idle at it)
		uint64_t              seen_count    = 0;
		uint64_t              idle_ns       = 0; // GPU thread: waiting for the decode thread
		uint64_t              idle_sync_ns  = 0; // ... of which right after a sync's release
		uint64_t              idle_count    = 0;
		uint64_t              gpu_packets   = 0; // run on the GPU thread after a sync
		std::atomic<uint64_t> decode_user_us {0}, decode_sys_us {0}; // cumulative
		uint64_t              last_user_us = 0, last_sys_us = 0;
		uint64_t              start_ns      = 0;
		uint64_t              posted_start  = 0;
	} stats;
	[[nodiscard]] bool StatsEnabled() const { return m_stats_enabled; }

private:
	void Loop();
	void ExecuteSync();
	// After a sync the GPU thread goes on with the packets itself, as ProcessPm4 would, up to the
	// next draw packet (or the end): around dispatches and labels the packets that need it come
	// in runs. False if a packet suspended the execution.
	bool RunUntilDraw();
	void NoteChain(const CommandProcessor::PacketView& packet) {
		if (packet.opcode == Pm4::IT_DISPATCH_DIRECT || packet.opcode == Pm4::IT_DISPATCH_INDIRECT) {
			chain_open = true;
		} else if (!KeepsDispatchChain(packet.header)) {
			chain_open = false;
		}
	}
	void PrintStats(uint64_t posted);

	CommandProcessor& m_cp;
	bool              m_stats_enabled  = false;
	bool              m_run_until_draw = true;
	std::thread       m_thread;
};

void DecodeThread::Loop() {
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "KytyGpuDecode");
	Common::HybridAffinity::ApplyToDecodeThread();
#endif
	g_on_decode_thread = true;
	for (;;) {
		handoff.to_decode.Wait(
		    [&] {
			    const auto s = state.load(std::memory_order_acquire);
			    return s == State::Running || s == State::Exit;
		    },
		    handoff.spin_ns);
		if (state.load(std::memory_order_acquire) == State::Exit) {
			return;
		}
		g_current_execution = execution;
		m_cp.ProcessPm4Decode(*execution);
		g_current_execution = nullptr;
		if (m_stats_enabled) {
			// The decode thread's CPU time (its kernel time is not in the command thread's).
			rusage usage {};
			if (getrusage(RUSAGE_THREAD, &usage) == 0) {
				const auto us = [](const timeval& t) {
					return static_cast<uint64_t>(t.tv_sec) * 1'000'000u + static_cast<uint64_t>(t.tv_usec);
				};
				stats.decode_user_us.store(us(usage.ru_utime), std::memory_order_relaxed);
				stats.decode_sys_us.store(us(usage.ru_stime), std::memory_order_relaxed);
			}
		}
		state.store(State::Finished, std::memory_order_release);
		handoff.to_commit.Notify();
		handoff.to_decode.Wait(
		    [&] {
			    const auto s = state.load(std::memory_order_acquire);
			    return s != State::Finished;
		    },
		    handoff.spin_ns);
	}
}

void DecodeThread::VerifySeed() {
	shadow->CopyDecodeState(m_cp);
	shadow_diverged = false;
}

void DecodeThread::VerifyReplay(uint64_t through, const VerifyEntry** sync) {
	static VerifyEntry last_sync {};
	EXIT_IF(g_on_decode_thread);
	for (;;) {
		VerifyEntry entry {};
		{
			// The decode thread logs a draw just after posting it: wait for the entry.
			for (uint32_t spins = 0;; spins++) {
				std::unique_lock lock(verify_mutex);
				if (!verify_log.empty()) {
					entry = verify_log.front();
					verify_log.pop_front();
					break;
				}
				lock.unlock();
				EXIT_IF(spins > 100'000'000u);
				KYTY_DECODE_PAUSE();
			}
		}
		if (entry.sync) {
			EXIT_IF(through != 0);
			last_sync = entry;
			*sync     = &last_sync;
			verify_syncs++;
			return;
		}
		verify_packets++;
		if (IndirectSourceHash(entry.packet) != entry.source_hash) {
			// The guest rewrote the bytes after the decode thread read them: a race on the guest's
			// side, not ours. The shadow is reseeded at the next sync.
			shadow_diverged = true;
			verify_races++;
		}
		const auto header = entry.packet[0];
		const auto opcode = (header >> 8u) & 0xffu;
		shadow->m_shadow_draw.valid = false;
		(void)g_cp_op_func[opcode](*shadow, header & ~1u, entry.packet + 1, entry.remaining_dw,
		                           entry.total_dw);
		if (entry.sequence != 0) {
			if (!shadow_diverged) {
				if (const auto diff = m_cp.ShadowDrawDiff(*shadow, entry.sequence); !diff.empty()) {
					EXIT("KYTY_DECODE_THREAD=verify: posted draw %" PRIu64
					     " differs from the single-thread decode (%s)\n",
					     entry.sequence, diff.c_str());
				}
				verify_draws++;
			}
			if (entry.sequence == through) {
				return;
			}
		}
	}
}

void DecodeThread::ExecuteSync() {
	EXIT_IF(handoff.asserts && (g_on_decode_thread || !GuestGpu::IsGpuThread()));
	const auto start = m_stats_enabled ? DecodeNowNs() : 0;
	if (m_stats_enabled && requested_ns != 0 && start > requested_ns) {
		stats.seen_ns += start - requested_ns;
		stats.seen_count++;
	}
	if (!has_view) {
		// Guest commands at a packet boundary: everything posted is committed.
		if (gpu != nullptr) {
			gpu->ProcessCommands();
		}
		stop = false;
	} else {
		bool first_packet = false;
		m_cp.RunPrologue(view, first_packet);
		stop = !m_cp.RunHandler(*execution, view);
		NoteChain(view);
		if (!stop && m_run_until_draw) {
			stop = !RunUntilDraw();
		}
	}
	if (m_stats_enabled) {
		stats.exec_ns += DecodeNowNs() - start;
	}
}

static bool IsDrawPacket(uint32_t opcode) {
	return opcode == Pm4::IT_DRAW_INDEX_2 || opcode == Pm4::IT_DRAW_INDEX_OFFSET_2 ||
	       opcode == Pm4::IT_DRAW_INDEX_AUTO || opcode == Pm4::IT_DRAW_INDEX_MULTI_INST;
}

bool DecodeThread::RunUntilDraw() {
	auto&      exec       = *execution;
	const bool debug_dump = graphics_debug_dump_enabled();
	while (!exec.m_buffer_stack.empty()) {
		if (gpu != nullptr && gpu->HasPendingCommands()) {
			gpu->ProcessCommands();
		}
		auto& cursor = exec.m_buffer_stack.back();
		if (cursor.offset_dw == cursor.commands.size()) {
			exec.m_buffer_stack.pop_back();
			continue;
		}
		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;
		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
			exec.m_made_progress = true;
			continue;
		}
		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);
		if (debug_dump) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     cursor.offset_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}
		if ((packet_header & 1u) != 0 && m_cp.ShouldSkipPredicatedPackets()) {
			if (opcode == Pm4::IT_DRAW_INDIRECT_MULTI || opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI) {
				Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectPredicated);
			}
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw > remaining_dw);
			cursor.offset_dw += packet_dw;
			exec.m_made_progress = true;
			continue;
		}
		if (IsDrawPacket(opcode)) {
			return true; // the decode thread takes it from here
		}
		const CommandProcessor::PacketView packet_view {packet, packet_header, opcode,
		                                                remaining_dw, total_dw};
		bool first_packet = false;
		m_cp.RunPrologue(packet_view, first_packet);
		if (!m_cp.RunHandler(exec, packet_view)) {
			return false;
		}
		NoteChain(packet_view);
		if (m_stats_enabled) {
			stats.gpu_packets++;
		}
	}
	return true;
}

void DecodeThread::Run(Pm4Execution& exec, GuestGpu* gpu_state) {
	auto& executor = m_cp.m_renderer.GetRenderExecutor();
	execution      = &exec;
	gpu            = gpu_state;
	stop           = false;
	// The loop's first packet ends a dispatch chain (ProcessPm4's first_packet).
	executor.EndDispatchChain(m_cp.CurrentBuffer());
	chain_open = false;
	executor.SetDecodeHandoff(&handoff);
	if (gpu != nullptr) {
		gpu->SetCommandWake(&handoff.to_commit);
	}
	if (m_stats_enabled && stats.start_ns == 0) {
		stats.start_ns     = DecodeNowNs();
		stats.posted_start = executor.QueuedPosted();
	}
	// The shadow and the verified baseline before the decode thread starts.
	if (verify) {
		VerifySeed();
	}
	uint64_t verified = executor.QueuedPosted();
	state.store(State::Running, std::memory_order_release);
	handoff.to_decode.Notify();
	bool after_sync = false;
	for (;;) {
		if (verify) {
			EXIT_IF(g_on_decode_thread || !GuestGpu::IsGpuThread());
			if (const auto posted = executor.QueuedPosted(); posted != verified) {
				VerifyReplay(posted, nullptr);
				verified = posted;
			}
		}
		// With verify, only what the shadow decode has checked.
		if (executor.CommitPosted(m_cp.CurrentBuffer(),
		                          verify ? verified : executor.QueuedPosted())) {
			after_sync = false;
			continue;
		}
		const auto s = state.load(std::memory_order_acquire);
		if (s == State::Sync) {
			if (executor.QueuedCount() != 0) {
				continue; // posted before the request
			}
			DrawPrepare::Chaos::Point(DrawPrepare::Chaos::Site::DecodeSync);
			if (verify) {
				// Every draw before the request is verified and committed; the next log entry is
				// this sync, and the shadow decode reached the same state.
				const VerifyEntry* entry = nullptr;
				VerifyReplay(0, &entry);
				EXIT_IF(entry == nullptr);
				if (entry->packet != (has_view ? view.packet : nullptr)) {
					EXIT("KYTY_DECODE_THREAD=verify: sync out of order: logged %p, requested %p\n",
					     static_cast<const void*>(entry->packet),
					     static_cast<const void*>(has_view ? view.packet : nullptr));
				}
				if (!shadow_diverged) {
					if (const auto diff = shadow->DecodeStateDiff(m_cp); !diff.empty()) {
						EXIT("KYTY_DECODE_THREAD=verify: the decode thread's state differs from the "
						     "single-thread decode at a sync (%s)\n",
						     diff.c_str());
					}
				}
			}
			ExecuteSync();
			if (verify) {
				VerifySeed(); // the sync ran on the GPU thread: from its state on
				verified = executor.QueuedPosted();
			}
			DrawPrepare::Chaos::Point(DrawPrepare::Chaos::Site::DecodeSync);
			state.store(State::SyncDone, std::memory_order_release);
			handoff.to_decode.Notify();
			after_sync = true;
			continue;
		}
		if (s == State::Finished) {
			if (executor.QueuedCount() != 0) {
				continue;
			}
			if (verify) {
				// The packets after the last draw.
				std::lock_guard lock(verify_mutex);
				while (!verify_log.empty()) {
					const auto entry = verify_log.front();
					verify_log.pop_front();
					EXIT_IF(entry.sync || entry.sequence != 0);
					shadow->m_shadow_draw.valid = false;
					const auto header = entry.packet[0];
					shadow_diverged = shadow_diverged ||
					                  IndirectSourceHash(entry.packet) != entry.source_hash;
					(void)g_cp_op_func[(header >> 8u) & 0xffu](*shadow, header & ~1u,
					                                           entry.packet + 1, entry.remaining_dw,
					                                           entry.total_dw);
				}
				if (!shadow_diverged) {
					if (const auto diff = shadow->DecodeStateDiff(m_cp); !diff.empty()) {
						EXIT("KYTY_DECODE_THREAD=verify: the decode thread's state differs from the "
						     "single-thread decode at the end of the loop (%s)\n",
						     diff.c_str());
					}
				}
			}
			break;
		}
		if (gpu != nullptr && gpu->HasPendingCommands()) {
			// Everything posted is committed: a packet boundary for the guest.
			gpu->ProcessCommands();
			continue;
		}
		const auto idle_start = m_stats_enabled ? DecodeNowNs() : 0;
		handoff.to_commit.Wait(
		    [&] {
			    return executor.QueuedCount() != 0 ||
			           state.load(std::memory_order_acquire) != State::Running ||
			           (gpu != nullptr && gpu->HasPendingCommands());
		    },
		    handoff.spin_ns);
		if (m_stats_enabled) {
			const auto idle = DecodeNowNs() - idle_start;
			stats.idle_ns += idle;
			stats.idle_sync_ns += after_sync ? idle : 0;
			stats.idle_count++;
		}
	}
	if (gpu != nullptr) {
		gpu->SetCommandWake(nullptr);
	}
	executor.SetDecodeHandoff(nullptr);
	state.store(State::Idle, std::memory_order_release);
	handoff.to_decode.Notify();
	execution = nullptr;
	if (m_stats_enabled) {
		PrintStats(executor.QueuedPosted());
	}
}

void DecodeThread::PrintStats(uint64_t posted) {
	const auto now = DecodeNowNs();
	if (now - stats.start_ns < 1'000'000'000ull) {
		return;
	}
	const double s     = static_cast<double>(now - stats.start_ns) / 1e9;
	const auto   syncs = stats.syncs.load(std::memory_order_relaxed);
	const auto   per   = [&](uint64_t ns, uint64_t n) { return n != 0 ? ns / 1e3 / n : 0.0; };
	std::printf("DECODE posts_s=%.0f syncs_s=%.0f (commands %.0f, bails %.0f) "
	            "sync_wait_us=%.2f exec_us=%.2f seen_us=%.2f (n_s=%.0f) "
	            "gpu_idle_ms_s=%.2f gpu_idle_after_sync_ms_s=%.2f gpu_idle_n_s=%.0f "
	            "ring_wait_ms_s=%.2f gpu_sleeps_s=%.0f decode_sleeps_s=%.0f gpu_packets_s=%.0f\n",
	            (posted - stats.posted_start) / s, syncs / s,
	            stats.sync_commands.load(std::memory_order_relaxed) / s,
	            stats.sync_bails.load(std::memory_order_relaxed) / s,
	            per(stats.sync_wait_ns.load(std::memory_order_relaxed), syncs),
	            per(stats.exec_ns, syncs), per(stats.seen_ns, stats.seen_count),
	            stats.seen_count / s, stats.idle_ns / s / 1e6, stats.idle_sync_ns / s / 1e6,
	            stats.idle_count / s, stats.ring_wait_ns.load(std::memory_order_relaxed) / s / 1e6,
	            handoff.to_commit.Sleeps() / s, handoff.to_decode.Sleeps() / s,
	            stats.gpu_packets / s);
	std::fflush(stdout);
	std::vector<std::pair<uint64_t, uint32_t>> reasons;
	for (uint32_t r = 0; r < stats.by_reason.size(); r++) {
		if (const auto n = stats.by_reason[r].exchange(0, std::memory_order_relaxed); n != 0) {
			reasons.emplace_back(n, r);
		}
	}
	std::ranges::sort(reasons, std::greater {});
	std::string line;
	for (size_t i = 0; i < reasons.size() && i < 10; i++) {
		char item[48];
		std::snprintf(item, sizeof(item), " %s%02x=%.0f",
		              reasons[i].second >= 512 ? (reasons[i].second == 512 ? "collect" : "chain")
		              : reasons[i].second >= 256 ? "nop"
		                                          : "op",
		              reasons[i].second & 0xffu, reasons[i].first / s);
		line += item;
	}
	std::printf("DECODE syncs by reason:%s\n", line.c_str());
	const auto user = stats.decode_user_us.load(std::memory_order_relaxed);
	const auto sys  = stats.decode_sys_us.load(std::memory_order_relaxed);
	std::printf("DECODE thread cpu user_ms_s=%.1f sys_ms_s=%.1f user_ns_per_post=%.0f "
	            "sys_ns_per_post=%.0f\n",
	            (user - stats.last_user_us) / s / 1e3, (sys - stats.last_sys_us) / s / 1e3,
	            (user - stats.last_user_us) * 1e3 / std::max<double>(1.0, posted - stats.posted_start),
	            (sys - stats.last_sys_us) * 1e3 / std::max<double>(1.0, posted - stats.posted_start));
	stats.last_user_us = user;
	stats.last_sys_us  = sys;
	if (verify) {
		std::printf("DECODE verify draws=%" PRIu64 " packets=%" PRIu64 " syncs=%" PRIu64
		            " guest_races=%" PRIu64 " (all clean so far)\n",
		            verify_draws, verify_packets, verify_syncs, verify_races);
	}
	stats.syncs = 0;
	stats.sync_commands = 0;
	stats.sync_bails = 0;
	stats.sync_wait_ns = 0;
	stats.ring_wait_ns = 0;
	stats.exec_ns = stats.seen_ns = stats.seen_count = 0;
	stats.idle_ns = stats.idle_sync_ns = stats.idle_count = stats.gpu_packets = 0;
	stats.start_ns     = now;
	stats.posted_start = posted;
}

CommandProcessor::~CommandProcessor() = default;

void CommandProcessor::DecodeThreadDeleter::operator()(DecodeThread* decode) const {
	delete decode;
}

void CommandProcessor::RunPacketLoop(Pm4Execution& execution) {
	// Not with frame skipping: its draw classification is the GPU thread's, so every draw would be
	// a sync (stage 1).
	if (DecodeThreadMode() != 0 && !IsAsyncComputeQueue() && g_gpu_state != nullptr &&
	    !FrameSkip::Active() && m_renderer.GetRenderExecutor().EnsurePrepareLanes()) {
		if (m_decode == nullptr) {
			m_decode.reset(new DecodeThread(*this));
		}
		m_decode->Run(execution, g_gpu_state);
		return;
	}
	ProcessPm4(execution);
}

bool CommandProcessor::DecodeSync(const PacketView* view) {
	auto&      d     = *m_decode;
	const auto start = d.StatsEnabled() ? DecodeNowNs() : 0;
	d.has_view       = view != nullptr;
	if (view != nullptr) {
		d.view = *view;
	}
	d.requested_ns = start;
	if (d.verify) {
		d.LogVerify({view != nullptr ? view->packet : nullptr, 0, 0, 0, 0, true});
	}
	DrawPrepare::Chaos::Point(DrawPrepare::Chaos::Site::DecodeSync);
	d.state.store(DecodeThread::State::Sync, std::memory_order_release);
	d.handoff.to_commit.Notify();
	d.handoff.to_decode.Wait(
	    [&] { return d.state.load(std::memory_order_acquire) == DecodeThread::State::SyncDone; },
	    d.handoff.spin_ns);
	const bool stop = d.stop;
	d.state.store(DecodeThread::State::Running, std::memory_order_release);
	if (d.StatsEnabled()) {
		d.stats.syncs.fetch_add(1, std::memory_order_relaxed);
		d.stats.sync_commands.fetch_add(view == nullptr ? 1 : 0, std::memory_order_relaxed);
		d.stats.sync_wait_ns.fetch_add(DecodeNowNs() - start, std::memory_order_relaxed);
	}
	return stop;
}

void CommandProcessor::ProcessPm4Decode(Pm4Execution& execution) {
	auto&      d          = *m_decode;
	const bool debug_dump = graphics_debug_dump_enabled();
	while (!execution.m_buffer_stack.empty()) {
		if (d.gpu != nullptr && d.gpu->HasPendingCommands()) {
			if (DecodeSync(nullptr)) {
				return;
			}
			continue;
		}
		auto& cursor = execution.m_buffer_stack.back();
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;

		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (debug_dump) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     cursor.offset_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			if (opcode == Pm4::IT_DRAW_INDIRECT_MULTI || opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI) {
				Common::FrameDiag::Add(Common::FrameDiag::Counter::IndirectPredicated);
			}
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw > remaining_dw);
			cursor.offset_dw += packet_dw;
			execution.m_made_progress = true;
			continue;
		}

		const PacketView view {packet, packet_header, opcode, remaining_dw, total_dw};
		auto             handler = g_cp_op_func[opcode];
		// The packets that keep the queue run here, as long as no collection is pending before a
		// packet that reads guest data and no dispatch chain is open before one that ends it.
		const bool keeps    = handler != nullptr && KeepsQueuedDraw(packet_header);
		const bool collect  = !DefersAsyncCollect(packet_header) && m_renderer.AsyncCollectRequested();
		const bool chain    = d.chain_open && !KeepsDispatchChain(packet_header);
		if (d.StatsEnabled()) {
			const uint32_t reason = !keeps ? (opcode == Pm4::IT_NOP
			                                      ? 256u + (KYTY_PM4_R(packet_header) & 0xffu)
			                                      : opcode)
			                        : collect ? 512u
			                        : chain   ? 513u
			                                  : UINT32_MAX;
			if (reason != UINT32_MAX) {
				d.stats.by_reason[reason].fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (keeps && !collect && !chain) {
			m_decode_bail             = false;
			const auto posted_before  = d.verify ? m_renderer.GetRenderExecutor().QueuedPosted() : 0;
			const auto source_hash    = d.verify ? IndirectSourceHash(packet) : 0;
			const auto packet_dw = handler(*this, packet_header & ~1u, packet + 1, remaining_dw,
			                               total_dw) +
			                       1;
			if (!m_decode_bail) {
				if (d.verify && opcode != Pm4::IT_INDIRECT_BUFFER) {
					const auto posted = m_renderer.GetRenderExecutor().QueuedPosted();
					d.LogVerify({packet, remaining_dw, total_dw,
					             posted != posted_before ? posted : 0, source_hash, false});
				}
				EXIT_IF(packet_dw > remaining_dw || execution.m_suspended);
				cursor.offset_dw += packet_dw;
				execution.m_made_progress = true;
				if (!execution.m_next_buffer.empty()) {
					if (execution.m_chain) {
						cursor = {execution.m_next_buffer};
					} else {
						execution.m_buffer_stack.push_back({execution.m_next_buffer});
					}
					execution.m_next_buffer = {};
				}
				continue;
			}
			if (d.StatsEnabled()) {
				d.stats.sync_bails.fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (DecodeSync(&view)) {
			return;
		}
	}
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_state != nullptr;
}

} // namespace Libs::Graphics
