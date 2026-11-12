#ifndef KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_
#define KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_

#include <cstdint>

// Optional frame skipping: `--frame-skip auto` (or KYTY_FRAME_SKIP=auto), off by default.
//
// Astro Bot (and most console games) advance the simulation by a fixed step per flip, so a frame
// that misses its vblank slows the game down. In auto mode the GPU command thread drops the draws
// of a frame now and then when it cannot keep up with the vblank rate; everything else in the
// frame still runs (register state, dispatches, DMA, labels, interrupts, waits, the flip), and the
// flip completes on its vblank without presenting a new image. The game still gets one flip per
// vblank; the screen shows fewer distinct images. See docs/frame-skip.md.
//
// A skipped frame keeps the draws whose results outlive the frame (render targets not redrawn
// every frame, targets the CPU reads back, shaders that write buffers or storage images, entries
// of KYTY_FRAME_SKIP_KEEP) and draws with no render target. KYTY_FRAME_SKIP_DIAG=1 reports those
// classes per render target, with the mode on or off.
//
// With the mode and the diagnostic off, the draw path pays one predictable branch.
namespace Libs::Graphics::HW {
class Context;
class Shader;
} // namespace Libs::Graphics::HW

namespace Libs::Graphics::FrameSkip {

namespace Detail {
extern bool g_active;   // mode or diagnostic on: draws are classified
extern bool g_enabled;  // mode on
extern bool g_diag;     // diagnostic on
extern bool g_skipping; // command thread: the current frame's draws are dropped
} // namespace Detail

[[nodiscard]] inline bool Active() noexcept {
	return Detail::g_active;
}
[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled;
}
[[nodiscard]] inline bool DiagEnabled() noexcept {
	return Detail::g_diag;
}
// Command thread: inside a frame whose draws are dropped.
[[nodiscard]] inline bool SkippingDraws() noexcept {
	return Detail::g_skipping;
}

// Command thread, before the first command buffer.
void Init();

// Command thread, for every draw packet when Active(): true drops the draw. `indirect_count`
// marks a multi-draw whose count the GPU wrote.
bool OnDraw(const HW::Context& ctx, const HW::Shader& sh, bool indirect, bool indirect_count);

// Command thread, at a guest flip packet once the flip is queued. `display_buffers` are the
// port's registered buffer addresses (scan-out only, never kept).
void OnFlip(const uint64_t* display_buffers, int display_buffer_count);

// Any thread (the draw commit): a draw whose shaders write buffers or storage images. Skipped
// frames keep the draws of that shader combination. `ranges` are the written buffer ranges when
// `known` (no raw-address or indirect writes); `atomic_only` when every buffer write is atomic.
struct WrittenRange {
	uint64_t address = 0;
	uint64_t size    = 0;
};
void NoteSideEffectDraw(const HW::Shader& sh, bool buffer_writes, bool image_writes, bool atomic_only,
                        bool known, const WrittenRange* ranges, int range_count);

// Presenter thread: a main-port flip reached the screen at `vblank`; `presented` is false for a
// skipped frame (no new image).
void NoteFlipDone(uint64_t vblank, bool presented);

// Diagnostic: a guest read fault served from GPU-written buffer memory.
void NoteCpuReadback(uint64_t vaddr);

// Performance overlay: skipped and shown frames per second over the last report.
struct OverlayStats {
	double   skipped_per_s = 0.0;
	double   shown_per_s   = 0.0;
	uint32_t worst_gap     = 0; // vblanks between shown images
};
OverlayStats GetOverlayStats();

} // namespace Libs::Graphics::FrameSkip

#endif /* KYTY_GRAPHICS_GUEST_GPU_FRAMESKIP_H_ */
