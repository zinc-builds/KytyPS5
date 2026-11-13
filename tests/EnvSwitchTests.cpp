#include "common/envSwitch.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "EnvSwitchTests: failed: %s\n", message);
		std::abort();
	}
}

bool With(const char* value) {
#if defined(_WIN32)
	_putenv_s("KYTY_ENV_SWITCH_TEST", value == nullptr ? "" : value); // "" unsets on Windows
#else
	if (value == nullptr) {
		unsetenv("KYTY_ENV_SWITCH_TEST");
	} else {
		setenv("KYTY_ENV_SWITCH_TEST", value, 1);
	}
#endif
	return Common::EnvFlag("KYTY_ENV_SWITCH_TEST");
}

} // namespace

int main() {
	Check(!With(nullptr), "unset is off");
	for (const char* off: {"0", "false", "FALSE", "off", "Off", "no", "NO"}) {
		Check(!With(off), off);
	}
	for (const char* on: {"1", "yes", "on", "true", "verify", "2", "00", "nope"}) {
		Check(With(on), on);
	}
#if !defined(_WIN32)
	Check(With(""), "set but empty is on");
#endif
	std::puts("EnvSwitchTests: all cases passed");
	return 0;
}
