#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DECODEHANDOFF_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DECODEHANDOFF_H_

// The hand-off between the decode thread and the GPU (commit) thread (KYTY_DECODE_THREAD=1,
// docs/mt-rewrite/decode-thread.md). The decode thread parses PM4 across the packets that keep
// the draw queue and posts queued draws; every other packet is a sync the GPU thread executes
// after committing everything posted before it, with the decode thread stopped.
//
// Each side waits for the other by spinning a short bounded time (KYTY_DECODE_SPIN_US, default
// 20), then sleeping on a futex word the other side bumps. Spinning keeps a sync's round trip at
// cache-line latency; the bound keeps a stalled side off its core.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define KYTY_DECODE_PAUSE() _mm_pause()
#else
#define KYTY_DECODE_PAUSE() ((void)0)
#endif

namespace Libs::Graphics {

// The decode thread sets this for itself.
inline thread_local bool g_on_decode_thread = false;

class DecodeSignal {
public:
	// Bumps the word and wakes the waiter if it sleeps.
	void Notify() noexcept {
		m_word.fetch_add(1, std::memory_order_seq_cst);
		if (m_sleeping.load(std::memory_order_seq_cst)) {
			m_word.notify_one();
		}
	}
	// Waits until `ready()` holds: spins up to `spin_ns`, then sleeps until notified.
	template <typename Ready>
	void Wait(Ready&& ready, uint64_t spin_ns) noexcept {
		if (ready()) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t spins = 1;; spins++) {
			KYTY_DECODE_PAUSE();
			if (ready()) {
				return;
			}
			if ((spins & 63u) == 0 &&
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			                              std::chrono::steady_clock::now() - start)
			                              .count()) > spin_ns) {
				break;
			}
		}
		for (;;) {
			const auto word = m_word.load(std::memory_order_seq_cst);
			m_sleeping.store(true, std::memory_order_seq_cst);
			if (ready()) {
				m_sleeping.store(false, std::memory_order_relaxed);
				return;
			}
			m_word.wait(word, std::memory_order_seq_cst);
			m_sleeping.store(false, std::memory_order_relaxed);
			m_sleeps++;
			if (ready()) {
				return;
			}
		}
	}
	[[nodiscard]] uint64_t Sleeps() const noexcept { return m_sleeps; }

private:
	alignas(64) std::atomic<uint32_t> m_word {0};
	std::atomic<bool> m_sleeping {false};
	uint64_t          m_sleeps = 0; // the waiter's own count
};

// What the render executor needs to know while the decode thread posts.
struct DecodeHandoff {
	// Bumped by the decode thread after each post, a sync request and the end of its loop.
	DecodeSignal to_commit;
	// Bumped by the GPU thread after each commit (ring space) and a sync's completion.
	DecodeSignal to_decode;
	// WaitPrepareIdle on the GPU thread pauses posting (the workers must stay idle while it looks
	// programs up). Dekker pair: the poster sets `posting` then reads `pause`; the GPU thread sets
	// `pause` then waits for `posting` to clear.
	alignas(64) std::atomic<bool> posting {false};
	alignas(64) std::atomic<bool> pause {false};
	uint64_t spin_ns = 20'000;
	// KYTY_DECODE_THREAD=verify: ownership asserts on the posting and committing paths.
	bool verify = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DECODEHANDOFF_H_
