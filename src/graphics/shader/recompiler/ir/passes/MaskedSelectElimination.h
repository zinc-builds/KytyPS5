#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct MaskedSelectStats {
	uint32_t selects    = 0; // Selects on a non-constant condition.
	uint32_t eliminated = 0;
};

// A guest VGPR write under EXEC is Select(exec, value, old): lanes outside EXEC keep the old
// value. Replaces Select(c, a, b) with a when no lane where c is false can ever observe the
// result: every path from it goes through lane-local arithmetic and phis to a select on the same
// c that takes it only where c is true, or into a memory access or export whose EXEC argument is
// c (the emitter skips those lanes). Anything else (cross-lane operations, other memory, block
// conditions) keeps the select. Per-lane semantics only; the caller restricts it to stages that
// map one guest lane to one invocation.
MaskedSelectStats EliminateMaskedSelects(const BlockList& blocks);

} // namespace Libs::Graphics::ShaderRecompiler::IR
