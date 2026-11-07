#ifndef KYTY_COMMON_HYBRID_AFFINITY_H_
#define KYTY_COMMON_HYBRID_AFFINITY_H_

// Local tuning for hybrid Intel CPUs on Linux: keep the emulator on performance cores and give
// the GPU command thread a physical core of its own. On Astro Bot's Sky Garden (i5-14600K) this
// lifted 14 FPS to 18 FPS: guest threads on efficiency cores and an SMT sibling sharing the
// command thread's core both slowed the critical path. KYTY_NO_PIN=1 disables it.

#if defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Common::HybridAffinity {

inline std::set<int> ParseCpuList(const std::string& text) {
	std::set<int>     cpus;
	std::stringstream stream(text);
	std::string       part;
	while (std::getline(stream, part, ',')) {
		char*      end   = nullptr;
		const long first = std::strtol(part.c_str(), &end, 10);
		if (end == part.c_str()) {
			return {};
		}
		long last = first;
		if (*end == '-') {
			const char* rest = end + 1;
			last             = std::strtol(rest, &end, 10);
			if (end == rest) {
				return {};
			}
		}
		for (long cpu = first; cpu <= last && cpu < CPU_SETSIZE; cpu++) {
			cpus.insert(static_cast<int>(cpu));
		}
	}
	return cpus;
}

inline std::set<int> ReadCpuList(const std::string& path) {
	std::ifstream file(path);
	std::string   text;
	return std::getline(file, text) ? ParseCpuList(text) : std::set<int> {};
}

struct Plan {
	bool      active = false;
	cpu_set_t process {};
	int       gpu_cpu = -1;
	// The draw prepare thread's CPU, or -1 for none chosen.
	int       prepare_cpu = -1;
};

inline int EnvCpu(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr ? std::atoi(value) : -1;
}

inline Plan& GetPlan() {
	static Plan plan = [] {
		Plan result;
		if (std::getenv("KYTY_NO_PIN") != nullptr) {
			return result;
		}
		const auto performance = ReadCpuList("/sys/devices/cpu_core/cpus");
		const auto efficiency  = ReadCpuList("/sys/devices/cpu_atom/cpus");
		if (performance.size() < 4 || efficiency.empty()) {
			return result; // not a hybrid CPU, or too few performance cores to spare one
		}
		cpu_set_t allowed;
		CPU_ZERO(&allowed);
		if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
			return result;
		}
		// The last performance core (with its SMT siblings) is reserved for the GPU thread.
		const int  gpu      = *performance.rbegin();
		const auto siblings = ReadCpuList("/sys/devices/system/cpu/cpu" + std::to_string(gpu) +
		                                  "/topology/thread_siblings_list");
		result.gpu_cpu = siblings.empty() ? gpu : *siblings.begin();
		// The draw prepare thread (KYTY_PREPARE_THREAD) gets the first thread of the next
		// performance core to itself; its SMT sibling stays with the emulator.
		// KYTY_PREPARE_CPU=<n> picks another CPU, which the emulator keeps if it is not a
		// performance core.
		const char* prepare_env = std::getenv("KYTY_PREPARE_THREAD");
		const bool  prepare_on  = prepare_env == nullptr || prepare_env[0] != '0';
		int         prepare     = prepare_on ? EnvCpu("KYTY_PREPARE_CPU") : -1;
		if (prepare_on && prepare < 0) {
			for (auto it = performance.rbegin(); it != performance.rend(); ++it) {
				if (siblings.count(*it) == 0 && *it != gpu) {
					const auto core = ReadCpuList("/sys/devices/system/cpu/cpu" +
					                              std::to_string(*it) +
					                              "/topology/thread_siblings_list");
					prepare = core.empty() ? *it : *core.begin();
					break;
				}
			}
		}
		result.prepare_cpu = prepare;
		// KYTY_RESERVE_CPUS=<list>: also keep the emulator off these (for a thread pinned there).
		const char* reserve_env = std::getenv("KYTY_RESERVE_CPUS");
		const auto  reserved    = reserve_env != nullptr ? ParseCpuList(reserve_env) : std::set<int> {};
		CPU_ZERO(&result.process);
		for (const int cpu: performance) {
			if (siblings.count(cpu) == 0 && cpu != gpu && cpu != prepare &&
			    reserved.count(cpu) == 0 && CPU_ISSET(cpu, &allowed)) {
				CPU_SET(cpu, &result.process);
			}
		}
		result.active = CPU_COUNT(&result.process) >= 2 && CPU_ISSET(result.gpu_cpu, &allowed);
		return result;
	}();
	return plan;
}

// Call early in main(), before threads start: later threads inherit the mask.
inline void ApplyToProcess() {
	auto& plan = GetPlan();
	if (plan.active && sched_setaffinity(0, sizeof(plan.process), &plan.process) == 0) {
		std::printf("Hybrid CPU: emulator on %d performance threads, GPU command thread on CPU %d, "
		            "draw prepare thread on CPU %d\n",
		            CPU_COUNT(&plan.process), plan.gpu_cpu, plan.prepare_cpu);
	}
}

// Call from the GPU command thread itself.
inline void ApplyToGpuThread() {
	const auto& plan = GetPlan();
	if (!plan.active) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(plan.gpu_cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Call from an emulator helper thread that a pinned thread may have started.
inline void ApplyToEmulatorThread() {
	const auto& plan = GetPlan();
	if (plan.active) {
		(void)pthread_setaffinity_np(pthread_self(), sizeof(plan.process), &plan.process);
	}
}

// Call from the draw prepare thread: a performance core the emulator leaves to it (see GetPlan).
inline void ApplyToPrepareThread() {
	int cpu = EnvCpu("KYTY_PREPARE_CPU");
	if (cpu < 0 && GetPlan().active) {
		cpu = GetPlan().prepare_cpu;
	}
	if (cpu < 0) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Call from a thread the game created, once it has its name. The game's threads run on the
// efficiency cores (all but the recording thread's): since write faults stopped raising signals
// they are light, and leaving the performance cores to the emulator's threads, the game's main
// thread and its render thread gained 3%. Threads whose name starts with one of
// KYTY_GUEST_PCORE_NAMES=<name,...> (default DrawThread) stay on the emulator's cores.
// KYTY_GUEST_THREAD_CPUS=<list> picks other CPUs, =none keeps every game thread with the emulator.
inline void ApplyToGuestThread(const std::string& name) {
	static const std::set<int> cpus = [] {
		if (!GetPlan().active) {
			return std::set<int> {};
		}
		if (const char* value = std::getenv("KYTY_GUEST_THREAD_CPUS"); value != nullptr) {
			return std::string(value) == "none" ? std::set<int> {} : ParseCpuList(value);
		}
		auto efficiency = ReadCpuList("/sys/devices/cpu_atom/cpus");
		// Not the recording thread's CPU (see ApplyToRecorderThread).
		const int recorder = EnvCpu("KYTY_RECORDER_CPU");
		if (recorder >= 0) {
			efficiency.erase(recorder);
		} else if (!efficiency.empty()) {
			efficiency.erase(std::prev(efficiency.end()));
		}
		return efficiency;
	}();
	if (cpus.empty()) {
		return;
	}
	static const std::vector<std::string> keep = [] {
		const char*              value = std::getenv("KYTY_GUEST_PCORE_NAMES");
		std::stringstream        stream(value != nullptr ? value : "DrawThread");
		std::vector<std::string> names;
		for (std::string prefix; std::getline(stream, prefix, ',');) {
			if (!prefix.empty()) {
				names.push_back(prefix);
			}
		}
		return names;
	}();
	for (const auto& prefix: keep) {
		if (name.rfind(prefix, 0) == 0) {
			return;
		}
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	for (const int cpu: cpus) {
		CPU_SET(cpu, &set);
	}
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Call from the GPU recording thread: an efficiency core of its own, which the emulator leaves
// free. KYTY_RECORDER_CPU=<n> picks another CPU.
inline void ApplyToRecorderThread() {
	int cpu = -1;
	if (const char* value = std::getenv("KYTY_RECORDER_CPU"); value != nullptr) {
		cpu = std::atoi(value);
	} else if (GetPlan().active) {
		const auto efficiency = ReadCpuList("/sys/devices/cpu_atom/cpus");
		if (!efficiency.empty()) {
			cpu = *efficiency.rbegin();
		}
	}
	if (cpu < 0) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

} // namespace Common::HybridAffinity
#endif

#endif // KYTY_COMMON_HYBRID_AFFINITY_H_
