#ifndef KYTY_LOADER_TRAPDIAG_H_
#define KYTY_LOADER_TRAPDIAG_H_

#include <atomic>
#include <cstdint>

// KYTY_TRAP_DIAG=1 (off by default): where emulated instruction traps (SIGILL) come from. Per
// thread: traps, the emulator's handler time (rdtsc, from the exception handler's entry to its
// return, so without the kernel's signal delivery and sigreturn) and the thread's user and system
// CPU time from /proc; per guest instruction address: traps and the instruction bytes. A reporter
// thread prints TRAPDIAG lines every KYTY_TRAP_DIAG_PERIOD seconds (default 5).
namespace Loader::TrapDiag {

namespace Detail {
extern std::atomic<bool> g_on;
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_on.load(std::memory_order_relaxed);
}

void Init();

// The exception handler, after it emulated the instruction at `rip` (bytes read before the
// emulation), with its rdtsc at entry. Async-signal-safe: no locks, no allocation.
void Note(uint64_t rip, uint32_t bytes, uint64_t start_tsc) noexcept;

} // namespace Loader::TrapDiag

#endif /* KYTY_LOADER_TRAPDIAG_H_ */
