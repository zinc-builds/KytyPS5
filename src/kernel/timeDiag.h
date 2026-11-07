#ifndef KYTY_KERNEL_TIME_DIAG_H_
#define KYTY_KERNEL_TIME_DIAG_H_

#include <cstdint>

// Local benchmarking aid (KYTY_TIME_DIAG=1): which clocks the game reads, how often, from where,
// and the flip cadence it sees. One TIMEDIAG line per second on stdout. Off by default; every
// hook is a single predictable branch when it is off. `flip_vbl` (vblanks between flips that
// reached the screen) is the real-speed check: at p99 == 1 the game paces exactly as on a PS5.
namespace Libs::LibKernel::TimeDiag {

enum class Source : uint8_t {
	ReadTsc,
	ProcessTime,
	ProcessTimeCounter,
	TscFrequency,
	ClockGettime,
	Gettimeofday,
	FlipStatus,
	VblankStatus,
	WaitVblank,
	Usleep,
	Nanosleep,
	Count
};

bool Enabled();

// `caller` is the guest return address of the HLE call.
void NoteCall(Source source, const void* caller);
void NoteClock(int clock_id, const void* caller);
void NoteSleep(Source source, uint64_t ns, const void* caller);
// A flip request the guest made (from the CPU or from a GPU end-of-pipe).
void NoteFlipSubmit(bool from_gpu, int flip_mode, int64_t flip_arg, int flip_rate);
// A flip that reached the screen; `vblank_count` is the display's vblank counter at that moment.
void NoteFlipDone(uint64_t vblank_count);
void NoteVblank();

} // namespace Libs::LibKernel::TimeDiag

#endif /* KYTY_KERNEL_TIME_DIAG_H_ */
