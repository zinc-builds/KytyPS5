#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWSTATECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWSTATECACHE_H_

#include "common/envSwitch.h"

#include <cstdlib>

namespace Libs::Graphics {

// Draw state caches reuse per-draw preparation whose inputs have not changed since an earlier
// draw. KYTY_DTC=0 turns them off. KYTY_DTC_VERIFY=1 also prepares every hit from scratch and
// exits unless both agree. KYTY_DTC_STATS=1 prints their hit rates once per second.
enum class DrawStateCacheMode { Off, On, Verify };

inline DrawStateCacheMode GetDrawStateCacheMode() {
	static const auto mode = [] {
		if (const char* verify = std::getenv("KYTY_DTC_VERIFY"); verify != nullptr && *verify == '1') {
			return DrawStateCacheMode::Verify;
		}
		const char* enabled = std::getenv("KYTY_DTC");
		return enabled != nullptr && *enabled == '0' ? DrawStateCacheMode::Off : DrawStateCacheMode::On;
	}();
	return mode;
}

inline bool DrawStateCacheStats() {
	static const bool enabled = Common::EnvFlag("KYTY_DTC_STATS");
	return enabled;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWSTATECACHE_H_
