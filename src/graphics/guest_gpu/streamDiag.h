#ifndef KYTY_GRAPHICS_GUEST_GPU_STREAM_DIAG_H_
#define KYTY_GRAPHICS_GUEST_GPU_STREAM_DIAG_H_

#include <cstdint>

// KYTY_STREAM_DIAG=1 (off by default, shadow only: nothing it computes is used): what the guest's
// command stream looks like to a translator that would not interpret it packet by packet.
//
// Per guest frame, at the flip, on stdout:
//   STRM  the graphics submissions: how many, by submitting thread, the draws by the thread that
//         wrote the draw packet, draws in the largest submissions, registers a submission's
//         first draw inherits from an earlier submission of the frame, synchronizing packets,
//         render targets drawn to from more than one submission.
//   STRA  the draw segments (the packets from the previous draw through this draw, indirect
//         register arrays followed): how many recur from the previous frame, 2, 3 and 33 frames
//         back and from any of the last 34 frames, by key:
//           E   exact: every packet word and every register value
//           P   parameterized: the shader user-data registers, index buffer, counts, offsets
//               and addresses in non-register packets left out
//           S   shape: packet headers and register numbers only
//           DE  the whole register state at the draw plus the draw's arguments, exact
//           DP  the same, parameterized as P
//         plus runs of consecutive segments that follow the previous frame's order, segment
//         sizes, and how many values differ between a segment and the previous frame's segment
//         of the same shape.
//   STRK  draws/dispatches/EVENT_WRITEs/occlusion-counter dumps by the guest's debug-marker path.
//   STRB  one line per graphics submission, for 40 consecutive frames in every 400.
// Every 100 frames: STRH (AGC HLE calls by guest thread), STRF (by function), STRO (packets the
// command thread ran, by opcode), STRI (registers inherited across submissions), STRD (the
// registers whose values differ between same-shape segments), STRL (histograms).
// The model follows the in-order command thread; KYTY_DECODE_THREAD must be off.
// tools/astro-bench/streamdiag.py summarizes the lines.
namespace Libs::Graphics::StreamDiag {

[[nodiscard]] bool ReadEnabled() noexcept;
// Inline so that the disabled path costs a guard check, not a call.
[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = ReadEnabled();
	return enabled;
}

// Guest threads.
[[nodiscard]] uint32_t RegisterHle(const char* name); // once per call site
void OnHle(uint32_t id) noexcept;                     // an AGC HLE function was entered
void OnAllocate(const uint32_t* cmd) noexcept;        // this thread wrote a packet at cmd
void OnSubmit(const uint32_t* commands, uint32_t dw, bool graphics);

// Command thread.
void OnSubmissionBegin(bool graphics, const uint32_t* commands, uint32_t dw);
void OnPacket(bool graphics, const uint32_t* packet, uint32_t packet_dw);
void OnProcessorReset(bool graphics);
void OnGuestFlip(uint64_t frame);

} // namespace Libs::Graphics::StreamDiag

#endif /* KYTY_GRAPHICS_GUEST_GPU_STREAM_DIAG_H_ */
