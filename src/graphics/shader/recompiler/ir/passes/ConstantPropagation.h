#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// prefix_entry_lanes: the lanes active at entry are the subgroup's lowest ones (vertex stages),
// so lane predicates built only from the entry block's Ballot(true), LaneId and constants fold.
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             bool prefix_entry_lanes = false);

} // namespace Libs::Graphics::ShaderRecompiler::IR
