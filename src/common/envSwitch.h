#ifndef COMMON_ENV_SWITCH_H_
#define COMMON_ENV_SWITCH_H_

#include <cstdlib>
#include <string_view>

namespace Common {

// An on/off environment switch (a KYTY_* kill switch, verify mode or diagnostic): unset, or one of
// "0", "false", "off", "no" (any case) means off; any other value, empty included, means on. Use
// it instead of testing getenv() for null, which turns a switch on for KYTY_X=0 too.
// docs/mt-rewrite/SWITCHES.md lists every switch (tools/switches.py generates it).
inline bool EnvFlag(const char* name) {
	const char* value = std::getenv(name);
	if (value == nullptr) {
		return false;
	}
	std::string_view v(value);
	auto             is = [&](std::string_view word) {
		if (v.size() != word.size()) {
			return false;
		}
		for (size_t i = 0; i < v.size(); i++) {
			const char c = v[i] >= 'A' && v[i] <= 'Z' ? static_cast<char>(v[i] - 'A' + 'a') : v[i];
			if (c != word[i]) {
				return false;
			}
		}
		return true;
	};
	return !(is("0") || is("false") || is("off") || is("no"));
}

} // namespace Common

#endif // COMMON_ENV_SWITCH_H_
