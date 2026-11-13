#ifndef KYTY_COMMON_FP_ENV_H_
#define KYTY_COMMON_FP_ENV_H_

#include "common/common.h"

#include <cstdlib>
#include <cstring>

namespace Common {

struct FpEnv {
	uint32_t mxcsr;
	uint16_t control;
};

inline constexpr FpEnv GuestFpEnv {0x9fc0, 0x037f};

inline FpEnv ReadFpEnv() {
	FpEnv env {};
	asm volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(env.mxcsr), "=m"(env.control) : : "memory");
	return env;
}

inline void WriteFpEnv(FpEnv env) {
	asm volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(env.mxcsr), "m"(env.control) : "memory");
}

// KYTY_GUEST_FP_ENV=0: guest threads and fibers keep the host's FP environment, as before
// upstream f399d481 (fibers still carry their own across switches).
[[nodiscard]] inline bool GuestFpEnvEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GUEST_FP_ENV");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

// Puts this thread in the console's FP environment, unless KYTY_GUEST_FP_ENV=0.
inline void ApplyGuestFpEnv() {
	if (GuestFpEnvEnabled()) {
		WriteFpEnv(GuestFpEnv);
	}
}

} // namespace Common

#endif /* KYTY_COMMON_FP_ENV_H_ */
