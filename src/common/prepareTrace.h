#ifndef EMULATOR_SRC_COMMON_PREPARETRACE_H_
#define EMULATOR_SRC_COMMON_PREPARETRACE_H_

#include <cstdint>
#include <cstdlib>
#include <x86intrin.h>

// KYTY_PREPARE_TRACE=<file>: per-draw timing of the draw preparation (RenderExecutor), written
// to <file> as binary records. Timers here are TSC ticks and cost nothing when it is unset.
namespace Common::PrepareTrace {

[[nodiscard]] inline const char* File() {
	static const char* file = std::getenv("KYTY_PREPARE_TRACE");
	return file;
}
[[nodiscard]] inline bool Enabled() {
	static const bool on = File() != nullptr && File()[0] != '\0';
	return on;
}
[[nodiscard]] inline uint64_t Now() { return __rdtsc(); }

// Per-thread accumulators for stages inside the program lookup, reset by the job.
inline thread_local uint64_t t_module_ticks    = 0; // shader module lookup (GetShaderParams)
inline thread_local uint64_t t_resource_ticks  = 0; // resource materialization (SRT walk)
inline thread_local uint32_t t_resource_misses = 0; // materializations that recompiled/missed
// MaterializeResources, per job: calls, walk ticks (WalkSrt), flattened SRT words, resources
// decoded, and how many calls had inputs (plan, user data, walked values, read ranges) seen
// before on this thread (any earlier call in the table) or equal to this plan's previous call.
inline thread_local uint32_t t_mat_calls       = 0;
inline thread_local uint64_t t_walk_ticks      = 0;
inline thread_local uint32_t t_srt_words       = 0;
inline thread_local uint32_t t_resources       = 0;
inline thread_local uint32_t t_input_seen      = 0;
inline thread_local uint32_t t_input_same_plan = 0;

class Scope {
public:
	explicit Scope(uint64_t& acc): m_acc(acc), m_start(Enabled() ? Now() : 0) {}
	~Scope() {
		if (m_start != 0) {
			m_acc += Now() - m_start;
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	uint64_t& m_acc;
	uint64_t  m_start;
};

} // namespace Common::PrepareTrace

#endif // EMULATOR_SRC_COMMON_PREPARETRACE_H_
