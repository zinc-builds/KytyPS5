#ifndef KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_
#define KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_

#include <atomic>

// Experimental frame skipping (Track S prototype), off by default.
//
// Astro Bot steps its simulation by a fixed amount per flip, so a frame that misses its vblank
// slows the game down. With KYTY_FRAME_SKIP set, the GPU command thread drops the draws (and PSSR
// passes) of some frames. Everything else in those frames still runs: register state, dispatches,
// DMA, labels, interrupts, waits and the flip itself, which completes on its vblank but presents
// no new image (the window keeps the previous one). The game still sees one flip per vblank.
//
//   KYTY_FRAME_SKIP=<n>   n >= 2: skip the draws of every n-th frame.
//   KYTY_FRAME_SKIP=auto  skip a frame only when the command thread has fallen behind the
//                         60 Hz budget; never two frames in a row.
//   KYTY_FRAME_SKIP_LOG=1 one FRAMESKIP line per second (flips, skipped frames, debt).
//
// Unset, every hook is one predictable branch on a constant.
namespace Libs::Graphics::FrameSkip {

namespace Detail {
extern bool              g_enabled;
extern std::atomic<bool> g_skipping;
} // namespace Detail

inline bool Enabled() noexcept {
	return Detail::g_enabled;
}

// True while the command thread is inside a frame whose draws are dropped.
inline bool SkippingDraws() noexcept {
	return Detail::g_enabled && Detail::g_skipping.load(std::memory_order_relaxed);
}

void Init();

// Called by the command thread at a guest flip packet, after the flip has been queued. Ends the
// current frame and decides whether the next one is skipped.
void OnFlip();

// Called by the command thread for every draw dropped.
void NoteSkippedDraw();

} // namespace Libs::Graphics::FrameSkip

#endif /* KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_ */
