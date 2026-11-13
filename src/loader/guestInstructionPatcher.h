// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_
#define KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_

#include "common/common.h"

#include <span>
#include <vector>

namespace Loader {

struct GuestInstructionHostFeatures {
	bool sse4a = false;
	bool rdpid = false;
	bool clwb  = false;
};

GuestInstructionHostFeatures GetGuestInstructionHostFeatures();

struct InstructionPatchCounts {
	uint64_t found   = 0;
	uint64_t native  = 0;
	uint64_t trapped = 0;

	uint64_t Skipped() const { return found - native - trapped; }

	InstructionPatchCounts& operator+=(const InstructionPatchCounts& other) {
		found += other.found;
		native += other.native;
		trapped += other.trapped;
		return *this;
	}
};

struct GuestInstructionPatchResult {
	uint64_t               function_count                           = 0;
	uint64_t               instruction_count                        = 0;
	uint64_t               red_zone_function_count                  = 0;
	uint64_t               memory_instruction_count                 = 0;
	uint64_t               short_memory_instruction_count           = 0;
	uint64_t               patched_memory_instruction_count         = 0;
	uint64_t               stack_dependent_memory_instruction_count = 0;
	uint64_t               control_flow_memory_instruction_count    = 0;
	uint64_t               unrelocatable_memory_instruction_count   = 0;
	uint64_t               indirect_red_zone_function_count         = 0;
	InstructionPatchCounts reciprocal_sqrt;
	// VRSQRTPS sites made native by a jump over a preceding instruction (in functions with
	// unresolved indirect branches); counted in reciprocal_sqrt.native too.
	uint64_t               reciprocal_sqrt_preceding_jump = 0;
	InstructionPatchCounts extrq;
	InstructionPatchCounts insertq;
	InstructionPatchCounts rdpid;
	InstructionPatchCounts clwb;
	InstructionPatchCounts rdtsc;
};

void RegisterGuestInstructionPatchModule(void* module_ptr, uint64_t module_size,
                                         void* trampoline_area_ptr, uint64_t trampoline_area_size);
void UnregisterGuestInstructionPatchModule(void* module_ptr);

// Apply enabled instruction fixes using native trampolines or safe trap fallbacks. A nonzero
// tsc_multiplier rewrites RDTSC/RDTSCP to return host TSC * tsc_multiplier >> 32.
GuestInstructionPatchResult PatchGuestInstructions(
    uint64_t segment_addr, uint64_t segment_size, std::span<const uintptr_t> function_starts,
    bool protect_memory, bool emulate_amd,
    GuestInstructionHostFeatures host_features  = GetGuestInstructionHostFeatures(),
    uint64_t                     tsc_multiplier = 0);

// Diagnostic (tools/re/patchstats.cpp): called for each instruction left to trap, while patching.
struct TrappedSiteInfo {
	uintptr_t                  site               = 0;
	uintptr_t                  function_start     = 0;
	uintptr_t                  function_end       = 0;
	bool                       indirect_function  = false; // unresolved indirect branches
	bool                       reciprocal_sqrt    = false;
	std::span<const uintptr_t> unresolved_indirect_branches;
};
using TrappedSiteObserver = void (*)(const TrappedSiteInfo& info);
void SetTrappedSiteObserver(TrappedSiteObserver observer);

bool DecodeEhFrameFunctionStarts(uint64_t eh_frame_header_addr, uint64_t eh_frame_header_size,
                                 std::vector<uintptr_t>* function_starts);

} // namespace Loader

#endif /* KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_ */
