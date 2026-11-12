#ifndef KYTY_COMMON_HYBRID_AFFINITY_H_
#define KYTY_COMMON_HYBRID_AFFINITY_H_

// Local tuning for hybrid Intel CPUs on Linux: keep the emulator on performance cores and give
// the GPU command thread a physical core of its own. On Astro Bot's Sky Garden (i5-14600K) this
// lifted 14 FPS to 18 FPS: guest threads on efficiency cores and an SMT sibling sharing the
// command thread's core both slowed the critical path. KYTY_NO_PIN=1 disables it.

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
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
	// Each draw prepare worker's CPU, or -1 for none chosen.
	std::vector<int> prepare_cpus;
	// The overflow prepare worker's CPU (see PrepareOverflowCpu), or -1.
	int overflow_cpu = -1;
};

inline int EnvCpu(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr ? std::atoi(value) : -1;
}

// Draw prepare workers (KYTY_PREPARE_WORKERS, 1 to 4; 0 with KYTY_PREPARE_THREAD=0).
inline uint32_t PrepareWorkerCount() {
	static const uint32_t count = [] {
		const char* thread = std::getenv("KYTY_PREPARE_THREAD");
		if (thread != nullptr && thread[0] == '0') {
			return 0u;
		}
		const char* value = std::getenv("KYTY_PREPARE_WORKERS");
		return value != nullptr ? static_cast<uint32_t>(std::clamp(std::atoi(value), 1, 4)) : 2u;
	}();
	return count;
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
		// Each draw prepare worker gets the first thread of the next performance core down to
		// itself; its SMT sibling stays with the emulator. KYTY_PREPARE_CPU=<n> (the first
		// worker) or KYTY_PREPARE_CPUS=<list> (in worker order) pick other CPUs, which the
		// emulator keeps if they are not performance cores.
		std::vector<int> prepare;
		if (const char* list = std::getenv("KYTY_PREPARE_CPUS"); list != nullptr) {
			std::stringstream stream(list);
			for (std::string cpu; std::getline(stream, cpu, ',');) {
				prepare.push_back(std::atoi(cpu.c_str()));
			}
		} else if (const int cpu = EnvCpu("KYTY_PREPARE_CPU"); cpu >= 0) {
			prepare.push_back(cpu);
		}
		for (auto it = performance.rbegin();
		     it != performance.rend() && prepare.size() < PrepareWorkerCount(); ++it) {
			if (siblings.count(*it) != 0) {
				continue;
			}
			const auto core  = ReadCpuList("/sys/devices/system/cpu/cpu" + std::to_string(*it) +
			                                "/topology/thread_siblings_list");
			const int  first = core.empty() ? *it : *core.begin();
			if (*it == first && std::ranges::find(prepare, first) == prepare.end()) {
				prepare.push_back(first);
			}
		}
		prepare.resize(std::min<size_t>(prepare.size(), PrepareWorkerCount()));
		result.prepare_cpus = prepare;
		// The overflow prepare worker: by default on the SMT sibling of the last prepare worker's
		// core. KYTY_PREPARE_OVERFLOW_CPU=<n> picks another CPU, KYTY_PREPARE_OVERFLOW=0 turns it off.
		const char* overflow_env = std::getenv("KYTY_PREPARE_OVERFLOW");
		if (const int cpu = EnvCpu("KYTY_PREPARE_OVERFLOW_CPU"); cpu >= 0) {
			result.overflow_cpu = cpu;
		} else if ((overflow_env == nullptr || overflow_env[0] != '0') && prepare.size() >= 2) {
			for (const int sibling: ReadCpuList("/sys/devices/system/cpu/cpu" +
			                                    std::to_string(prepare.back()) +
			                                    "/topology/thread_siblings_list")) {
				if (sibling != prepare.back() && performance.count(sibling) != 0) {
					result.overflow_cpu = sibling;
					break;
				}
			}
		}
		if (overflow_env != nullptr && overflow_env[0] == '0') {
			result.overflow_cpu = -1;
		}
		// KYTY_RESERVE_CPUS=<list>: also keep the emulator off these (for a thread pinned there).
		const char* reserve_env = std::getenv("KYTY_RESERVE_CPUS");
		const auto  reserved    = reserve_env != nullptr ? ParseCpuList(reserve_env) : std::set<int> {};
		CPU_ZERO(&result.process);
		// The emulator's other threads (its main thread, driver threads) stay off the SMT siblings
		// of the prepare workers' cores while there is an overflow worker (one of them is its CPU),
		// and off the overflow worker's CPU. KYTY_FREE_PREPARE_SIBLINGS=0|1 overrides the first.
		std::set<int> prepare_siblings;
		if (result.overflow_cpu >= 0) {
			prepare_siblings.insert(result.overflow_cpu);
		}
		const char* free_siblings = std::getenv("KYTY_FREE_PREPARE_SIBLINGS");
		if (free_siblings != nullptr ? free_siblings[0] == '1' : result.overflow_cpu >= 0) {
			for (const int cpu: prepare) {
				for (const int sibling: ReadCpuList("/sys/devices/system/cpu/cpu" +
				                                    std::to_string(cpu) +
				                                    "/topology/thread_siblings_list")) {
					prepare_siblings.insert(sibling);
				}
			}
		}
		for (const int cpu: performance) {
			if (siblings.count(cpu) == 0 && cpu != gpu && prepare_siblings.count(cpu) == 0 &&
			    std::ranges::find(prepare, cpu) == prepare.end() &&
			    reserved.count(cpu) == 0 && CPU_ISSET(cpu, &allowed)) {
				CPU_SET(cpu, &result.process);
			}
		}
		result.active = CPU_COUNT(&result.process) >= 2 && CPU_ISSET(result.gpu_cpu, &allowed);
		return result;
	}();
	return plan;
}

// The GPU pipeline threads (command, prepare workers, recorder) run at this nice value
// (KYTY_PIPELINE_NICE, default 0 = unchanged), so other programs' threads on their cores yield to
// them. Lowering nice needs RLIMIT_NICE (ulimit -e) or CAP_SYS_NICE; without it this is a no-op.
// Not SCHED_FIFO/RR: these threads spin, and a spinning real-time thread starves kernel work.
inline void ApplyPipelinePriority() {
	static const int nice = [] {
		const char* value = std::getenv("KYTY_PIPELINE_NICE");
		return value != nullptr ? std::clamp(std::atoi(value), -20, 19) : 0;
	}();
	if (nice != 0) {
		(void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
	}
}

// Call early in main(), before threads start: later threads inherit the mask.
inline void ApplyToProcess() {
	auto& plan = GetPlan();
	if (plan.active && sched_setaffinity(0, sizeof(plan.process), &plan.process) == 0) {
		std::string prepare;
		for (const int cpu: plan.prepare_cpus) {
			prepare += (prepare.empty() ? "" : ",") + std::to_string(cpu);
		}
		std::printf("Hybrid CPU: emulator on %d performance threads, GPU command thread on CPU %d, "
		            "draw prepare workers on CPUs %s, overflow worker on CPU %d\n",
		            CPU_COUNT(&plan.process), plan.gpu_cpu, prepare.empty() ? "-" : prepare.c_str(),
		            plan.overflow_cpu);
	}
}

// Call from the GPU command thread itself.
inline void ApplyToGpuThread() {
	ApplyPipelinePriority();
	const auto& plan = GetPlan();
	if (!plan.active) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(plan.gpu_cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// The CPU of the overflow prepare worker (see DrawPrepare::Pool), or -1 for none: by default the
// SMT sibling of the last prepare worker's core on a hybrid CPU with the plan active (GetPlan).
inline int PrepareOverflowCpu() {
	static const int cpu = [] {
		const char* off = std::getenv("KYTY_PREPARE_OVERFLOW");
		if (off != nullptr && off[0] == '0') {
			return -1;
		}
		const int env = EnvCpu("KYTY_PREPARE_OVERFLOW_CPU");
		return env >= 0 ? env : GetPlan().overflow_cpu;
	}();
	return cpu;
}

// Call from draw prepare worker `worker`: a performance core the emulator leaves to it (see
// GetPlan). The overflow worker (index PrepareWorkerCount()) goes to PrepareOverflowCpu().
inline void ApplyToPrepareThread(uint32_t worker) {
	ApplyPipelinePriority();
	const auto& plan = GetPlan();
	int         cpu  = -1;
	if (worker >= PrepareWorkerCount()) {
		cpu = PrepareOverflowCpu();
	} else if (worker < plan.prepare_cpus.size()) {
		cpu = plan.prepare_cpus[worker];
	} else if (worker == 0) {
		cpu = EnvCpu("KYTY_PREPARE_CPU");
	}
	if (cpu < 0) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Call from the decode thread (KYTY_DECODE_THREAD): KYTY_DECODE_CPU=<n>, else the emulator's
// performance cores (it is created by the GPU thread, whose own CPU it must not inherit).
inline void ApplyToDecodeThread() {
	ApplyPipelinePriority();
	cpu_set_t set;
	CPU_ZERO(&set);
	if (const int cpu = EnvCpu("KYTY_DECODE_CPU"); cpu >= 0) {
		CPU_SET(cpu, &set);
	} else if (const auto& plan = GetPlan(); plan.active) {
		set = plan.process;
	} else {
		return;
	}
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// The CPUs for the game's threads: the efficiency cores but the recording thread's. Leaving the
// performance cores to the emulator's threads, the game's main thread and its render thread
// gained 3% (core layout, #46). KYTY_GUEST_THREAD_CPUS=<list> picks other CPUs, =none keeps every
// game thread with the emulator. Empty when the plan is not active.
inline const std::set<int>& GuestCpus() {
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
	return cpus;
}

// Guest priorities: lower numbers run first; 700 is the default, so a thread below 700 is one the
// game raised. The threads that submit graphics command buffers set a bar: game threads at or
// above the lowest raised priority any submitter has had (the largest number below 700) run with
// the emulator on the performance cores, the others on GuestCpus(). On Sky Garden the submitter
// (DrawThread, 680) waits for render workers at 678-680 that, on the efficiency cores, waited for
// a core 2.7 times longer than they ran; the rule moves them, the physics workers (478) and audio,
// and leaves the job pool and streamers (699-716). It is a heuristic: a guest priority does not
// say what the frame waits for. A submitter at the default priority sets no bar (it would move
// most of the game). KYTY_GUEST_PCORE_PRIORITY=<n> fixes the bar, =off turns the rule off.
inline constexpr int kGuestDefaultPriority = 700;

// Whether game threads are placed at all (a hybrid CPU with the plan active).
inline bool GuestPlacementActive() {
	return !GuestCpus().empty();
}

// The bar: -1 for none yet (or off).
inline std::atomic<int>& GuestPriorityBar() {
	static std::atomic<int> bar = [] {
		const char* value = std::getenv("KYTY_GUEST_PCORE_PRIORITY");
		return value != nullptr && value[0] >= '0' && value[0] <= '9' ? std::atoi(value) : -1;
	}();
	return bar;
}

// Bumped when the bar changes: each game thread re-places itself when it sees a new value.
inline std::atomic<uint32_t>& GuestPlacementEpoch() {
	static std::atomic<uint32_t> epoch {0};
	return epoch;
}

inline bool GuestPriorityAuto() {
	static const bool automatic = std::getenv("KYTY_GUEST_PCORE_PRIORITY") == nullptr;
	return automatic;
}

// Whether a game thread stays on the performance cores: its name starts with one of `names`, or
// its priority is at or above the bar (bar >= 0).
inline bool KeepGuestOnPerformanceCores(const std::string& name, int priority, int bar,
                                        const std::vector<std::string>& names) {
	if (bar >= 0 && priority <= bar) {
		return true;
	}
	for (const auto& prefix: names) {
		if (name.rfind(prefix, 0) == 0) {
			return true;
		}
	}
	return false;
}

// Raises the bar to `priority` (automatic mode, raised priorities only, `active` placement).
// Returns whether it changed; then the placement epoch moves.
inline bool RaiseGuestPriorityBar(int priority, bool active) {
	if (!active || !GuestPriorityAuto() || priority >= kGuestDefaultPriority || priority < 0) {
		return false;
	}
	auto& bar     = GuestPriorityBar();
	int   current = bar.load(std::memory_order_relaxed);
	while (priority > current) {
		if (bar.compare_exchange_weak(current, priority, std::memory_order_relaxed)) {
			GuestPlacementEpoch().fetch_add(1, std::memory_order_release);
			return true;
		}
	}
	return false;
}

// A thread submitted graphics work at `priority`.
inline void NoteGraphicsSubmitter(int priority) {
	if (RaiseGuestPriorityBar(priority, GuestPlacementActive())) {
		std::printf("Hybrid CPU: game threads at guest priority %d or above (a graphics "
		            "submitter's) on the performance cores\n",
		            priority);
	}
}

// Places the calling game thread by its name and guest priority. Threads whose name starts with
// one of KYTY_GUEST_PCORE_NAMES=<name,...> (default DrawThread) also stay on the performance
// cores. A thread only ever places itself, so no other thread's lifetime is involved.
inline void ApplyToGuestThread(const std::string& name, int priority) {
	const auto& cpus = GuestCpus();
	if (cpus.empty()) {
		return;
	}
	static const std::vector<std::string> names = [] {
		const char*              value = std::getenv("KYTY_GUEST_PCORE_NAMES");
		std::stringstream        stream(value != nullptr ? value : "DrawThread");
		std::vector<std::string> result;
		for (std::string prefix; std::getline(stream, prefix, ',');) {
			if (!prefix.empty()) {
				result.push_back(prefix);
			}
		}
		return result;
	}();
	if (KeepGuestOnPerformanceCores(name, priority,
	                                GuestPriorityBar().load(std::memory_order_relaxed), names)) {
		const auto& process = GetPlan().process;
		(void)pthread_setaffinity_np(pthread_self(), sizeof(process), &process);
		return;
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
	ApplyPipelinePriority();
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
