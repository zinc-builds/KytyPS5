#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREPARE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREPARE_H_

#include "common/assert.h"
#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace Libs::Graphics::DrawPrepare {

// Threaded draw preparation: a worker looks up a draw's shader programs (SRT walk, resource
// materialization, module selection) from a copy of the registers while the GPU command thread
// commits and records the previous draw. The command thread adopts the result at the draw's
// serial position only if nothing it depends on could have changed since; otherwise it prepares
// the draw itself.

// Whether this thread is the prepare worker. Its guest reads go through the backing view, never
// fault into the caches, and are logged for the command thread to check.
[[nodiscard]] bool OnWorker() noexcept;

// What a preparation on the worker read and whether its result is usable.
struct Log {
	// Guest ranges read on pages a cache watches, or through the buffer backing: GPU-written
	// bytes there would have needed a synchronization on the command thread.
	std::vector<std::pair<uint64_t, uint64_t>> reads;
	// The preparation needed something only the command thread may do (a cache miss, a module
	// compile, an image heap, a call-target refresh); the result is incomplete.
	bool failed = false;

	void Reset() {
		reads.clear();
		failed = false;
	}
};

// The log of the preparation running on this thread; null off the worker.
[[nodiscard]] Log* CurrentLog() noexcept;
// Called by a job on the worker: where its reads and failures go (null: nowhere).
void SetLog(Log* log) noexcept;
inline void NoteRead(uint64_t vaddr, uint64_t size) {
	if (auto* log = CurrentLog(); log != nullptr) {
		log->reads.emplace_back(vaddr, size);
	}
}
inline void Fail() {
	if (auto* log = CurrentLog(); log != nullptr) {
		log->failed = true;
	}
}

// Advanced by every event that can change what a preparation reads without a register write:
// guest bytes the GPU newly writes (BufferCache), and emulator stores into guest memory
// (GuestSyncEpoch, read alongside). A result is kept only if neither moved between its post and
// its adoption.
void               NoteHazard() noexcept;
[[nodiscard]] uint64_t HazardGeneration() noexcept;

// Emulator stores through the backing into pages that stay read-watched until after the store
// (buffer readbacks publish before their pages are released), recorded instead of advancing
// GuestSyncEpoch's unranged epoch. A preparation read such bytes only through a logging reader,
// so a result whose logged reads miss every range stored since its post is still current.
// Called before and after the store.
void                   NoteStoredRange(uint64_t vaddr, uint64_t size);
[[nodiscard]] uint64_t StoredSequence() noexcept;
// Whether a range recorded after `sequence` overlaps one of `reads`; also true when the record
// no longer reaches back that far.
[[nodiscard]] bool StoredSince(uint64_t sequence,
                               const std::vector<std::pair<uint64_t, uint64_t>>& reads);

// One worker thread. The command thread posts sequence numbers in order; the worker calls
// run(context, sequence) for each, in order.
class Worker {
public:
	using Run = void (*)(void* context, uint64_t sequence);

	Worker(Run run, void* context);
	~Worker();
	KYTY_CLASS_NO_COPY(Worker);

	// Hands sequence `posted + 1` to the worker and returns it.
	uint64_t Post();
	// Waits until the worker has finished `sequence`. Returns the nanoseconds spent waiting.
	uint64_t Wait(uint64_t sequence);
	// Waits until every posted sequence is finished.
	uint64_t WaitIdle() { return Wait(m_posted_local); }
	[[nodiscard]] bool Done(uint64_t sequence) const {
		return m_done.load(std::memory_order_acquire) >= sequence;
	}
	// Nanoseconds the worker spent running jobs (relaxed; for statistics).
	[[nodiscard]] uint64_t BusyNs() const { return m_busy_ns.load(std::memory_order_relaxed); }

private:
	void Loop();
	// Wakes the worker if it sleeps.
	void Wake();

	Run   m_run;
	void* m_context;
	uint64_t m_posted_local = 0; // command thread only
	alignas(64) std::atomic<uint64_t> m_posted {0};
	alignas(64) std::atomic<uint64_t> m_done {0};
	alignas(64) std::atomic<bool> m_sleeping {false};
	std::atomic<bool>             m_stop {false};
	std::atomic<uint64_t>         m_busy_ns {0};
	std::thread                   m_thread;
};

} // namespace Libs::Graphics::DrawPrepare

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREPARE_H_
