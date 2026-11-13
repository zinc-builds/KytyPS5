#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstdint>
#include <cstddef>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Completes a faulting load into EAX as a zero read and resumes after it.
void SkipLoad(void* native_context, size_t length);
void CompleteLoad32(void* native_context, size_t length, uint32_t value);
// One lane of the emulated VRSQRTPS (tests compare native rewrites with it).
[[nodiscard]] uint32_t ReciprocalSquareRootLane(uint32_t bits);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
