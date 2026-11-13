#include "common/configReport.h"

#include "common/envSwitch.h"
#include "common/logging/log.h"
#include "kytyBuildMode.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

extern char** environ; // NOLINT

namespace Common::ConfigReport {

namespace {

struct Known {
	const char* name;
	const char* kind;
	const char* def;
};

#define KYTY_SWITCH(name, kind, def) {name, kind, def},
constexpr Known KNOWN[] = {
#include "common/envSwitchList.inc"
};
#undef KYTY_SWITCH

const Known* Find(std::string_view name) {
	for (const auto& k: KNOWN) {
		if (name == k.name) {
			return &k;
		}
	}
	return nullptr;
}

// A verify mode only checks its feature: with the feature turned off it checks nothing.
struct VerifyPair {
	const char* verify;
	const char* feature;
};
constexpr VerifyPair VERIFY_NEEDS[] = {
    {"KYTY_BINDING_REUSE_VERIFY", "KYTY_BINDING_REUSE"},
    {"KYTY_WORKER_TARGETS_VERIFY", "KYTY_WORKER_TARGETS"},
    {"KYTY_WORKER_PIPELINE_KEY_VERIFY", "KYTY_WORKER_PIPELINE_KEY"},
    {"KYTY_WORKER_TEXTURES_VERIFY", "KYTY_WORKER_TEXTURES"},
    {"KYTY_PREPARE_VERIFY", "KYTY_PREPARE_THREAD"},
    {"KYTY_TILER_VERIFY", "KYTY_TILER_DIRECT"},
};

// Pins that a disabled core layout (KYTY_NO_PIN) ignores.
constexpr const char* PIN_SWITCHES[] = {"KYTY_PREPARE_CPU",  "KYTY_PREPARE_CPUS",  "KYTY_PREPARE_OVERFLOW_CPU",
                                         "KYTY_DECODE_CPU",   "KYTY_RECORDER_CPU",  "KYTY_RESERVE_CPUS",
                                         "KYTY_GUEST_THREAD_CPUS", "KYTY_GUEST_PCORE_NAMES"};

bool IsOffValue(const char* value) {
	// The kill switches' "0"; EnvFlag's other off words count too.
	if (value == nullptr) return false;
	const std::string_view v(value);
	return v == "0" || v == "off" || v == "false" || v == "no";
}

std::mutex                         g_mutex;
std::vector<std::string>           g_startup;
std::map<std::string, std::string> g_features; // ordered by name in the summary
std::atomic<uint64_t>              g_flips {0};
bool                               g_summary_done = false;

void Emit(std::vector<std::string>* keep, std::string line) {
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
	if (keep != nullptr) {
		keep->push_back(std::move(line));
	}
}

} // namespace

void PrintStartup(const std::string& version) {
	std::lock_guard lock(g_mutex);
	Emit(&g_startup, "CONFIG build " KYTY_BUILD_MODE_DESC);
	Emit(&g_startup, "CONFIG version " + version);
	std::vector<std::string> set;
	for (char** e = environ; e != nullptr && *e != nullptr; e++) {
		if (std::strncmp(*e, "KYTY_", 5) == 0) {
			set.emplace_back(*e);
		}
	}
	std::sort(set.begin(), set.end());
	int unknown = 0;
	for (const auto& entry: set) {
		const auto        eq    = entry.find('=');
		const std::string name  = entry.substr(0, eq);
		const std::string value = eq == std::string::npos ? "" : entry.substr(eq + 1);
		if (const auto* k = Find(name); k != nullptr) {
			std::string line = "CONFIG switch " + name + "=" + value + " (" + k->kind + ", default " + k->def;
			if (std::string_view(k->kind).find("kill switch") != std::string_view::npos ||
			    std::string_view(k->def) == "on" || std::string_view(k->def) == "off") {
				line += std::string(", now ") + (IsOffValue(value.c_str()) ? "off" : "on");
			}
			Emit(&g_startup, line + ")");
		} else {
			unknown++;
			Emit(&g_startup, "CONFIG warning: unknown switch " + name + "=" + value +
			                     " (not read by this build: a typo, or a switch that was removed; "
			                     "see docs/mt-rewrite/SWITCHES.md)");
		}
	}
	if (set.empty()) {
		Emit(&g_startup, "CONFIG switches: none set (all defaults)");
	}
	for (const auto& [verify, feature]: VERIFY_NEEDS) {
		if (EnvFlag(verify) && IsOffValue(std::getenv(feature))) {
			Emit(&g_startup, std::string("CONFIG warning: ") + verify + " checks nothing with " + feature +
			                     "=0 (the feature it verifies is off)");
		}
	}
	if (EnvFlag("KYTY_ASYNC_WP_VERIFY") && EnvFlag("KYTY_NO_ASYNC_WP")) {
		Emit(&g_startup, "CONFIG warning: KYTY_ASYNC_WP_VERIFY checks nothing with KYTY_NO_ASYNC_WP");
	}
	if (EnvFlag("KYTY_NO_PIN")) {
		for (const char* pin: PIN_SWITCHES) {
			if (std::getenv(pin) != nullptr) {
				Emit(&g_startup, std::string("CONFIG warning: ") + pin + " is ignored with KYTY_NO_PIN");
			}
		}
	}
	if (EnvFlag("KYTY_STALL_DIAG") && EnvFlag("KYTY_PERF_LOG")) {
		Emit(&g_startup, "CONFIG warning: KYTY_STALL_DIAG roughly doubles the measured prepare wait; "
		                 "do not take numbers of record with it");
	}
}

void Feature(const std::string& name, const std::string& state) {
	std::lock_guard lock(g_mutex);
	const auto      it = g_features.find(name);
	if (it != g_features.end() && it->second == state) {
		return;
	}
	g_features[name] = state;
	Emit(nullptr, "CONFIG feature " + name + ": " + state);
}

void OnFlip() {
	if (g_flips.fetch_add(1, std::memory_order_relaxed) + 1 != 120) {
		return;
	}
	std::lock_guard lock(g_mutex);
	if (g_summary_done) {
		return;
	}
	g_summary_done = true;
	std::string block = "CONFIG summary begin\n";
	for (const auto& line: g_startup) {
		block += line + "\n";
	}
	for (const auto& [name, state]: g_features) {
		block += "CONFIG feature " + name + ": " + state + "\n";
	}
	block += "CONFIG summary end\n";
	Log::WriteToConsoleAndLog(block);
}

} // namespace Common::ConfigReport
