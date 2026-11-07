#include "graphics/shader/recompiler/ir/passes/MaskedSelectElimination.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool IsSelect(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64: return true;
		default: return false;
	}
}

// Pure per-lane operations: each lane's result depends only on that lane's operands.
bool IsLaneLocal(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::Identity:
		case ValueOpcode::Phi:
		case ValueOpcode::BitCastU16F16: case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32: case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64: case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32: case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32: case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16: case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32: case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32: case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64: case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32: case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64: case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3: case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4: case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2: case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16: case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16: case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32: case ValueOpcode::FPNeg32: case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert: case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1: case ValueOpcode::SelectU32: case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64:
		case ValueOpcode::IAdd32: case ValueOpcode::IAdd64: case ValueOpcode::ISub32:
		case ValueOpcode::ISub64: case ValueOpcode::IMul32: case ValueOpcode::UDiv32:
		case ValueOpcode::SMulHi: case ValueOpcode::UMulHi: case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32: case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32: case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32: case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32: case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32: case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32: case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32: case ValueOpcode::FindUMsb32: case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32: case ValueOpcode::UMin32: case ValueOpcode::SMax32:
		case ValueOpcode::UMax32: case ValueOpcode::SMinTri32: case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32: case ValueOpcode::UMaxTri32: case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32: case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64: case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64: case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64: case ValueOpcode::IEqual32: case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32: case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32: case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64: case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64: case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr: case ValueOpcode::LogicalAnd: case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32: case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64: case ValueOpcode::FPOrdGreaterThanEqual64:
		case ValueOpcode::FPOrdNotEqual64: case ValueOpcode::FPOrdLessThan64:
		case ValueOpcode::FPOrdGreaterThan64: case ValueOpcode::FPUnordEqual64:
		case ValueOpcode::FPUnordNotEqual64: case ValueOpcode::FPUnordLessThan64:
		case ValueOpcode::FPUnordGreaterThan64: case ValueOpcode::FPUnordLessThanEqual64:
		case ValueOpcode::FPUnordGreaterThanEqual64: case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32: case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32: case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32: case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32: case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32: case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32: case ValueOpcode::FPIsNan64: case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPCmpClass16:
		case ValueOpcode::FPAdd32: case ValueOpcode::FPSub32: case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32: case ValueOpcode::FPMul32: case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64: case ValueOpcode::FPFma64: case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32: case ValueOpcode::FPMax32: case ValueOpcode::FPMin64:
		case ValueOpcode::FPMax64: case ValueOpcode::FPMinTri32: case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32: case ValueOpcode::FPRecip32: case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPRecipSqrt32: case ValueOpcode::FPSqrt: case ValueOpcode::FPSin:
		case ValueOpcode::FPCos: case ValueOpcode::FPExp2: case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp: case ValueOpcode::FPRoundEven32: case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32: case ValueOpcode::FPTrunc32: case ValueOpcode::FPFloor64:
		case ValueOpcode::FPCeil64: case ValueOpcode::FPTrunc64: case ValueOpcode::FPFract32:
		case ValueOpcode::FPFract64: return true;
		default: return false;
	}
}

// Memory accesses and exports the emitter performs only where their last argument (the guest
// EXEC bit) is set; loads give zero elsewhere.
bool IsExecGuarded(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::LoadBufferU8: case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32: case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3: case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::StoreBufferU8: case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32: case ValueOpcode::StoreBufferU32x2:
		case ValueOpcode::StoreBufferU32x3: case ValueOpcode::StoreBufferU32x4:
		case ValueOpcode::LoadSharedU8: case ValueOpcode::LoadSharedU16:
		case ValueOpcode::LoadSharedU32: case ValueOpcode::LoadSharedU32x2:
		case ValueOpcode::LoadSharedU32x3: case ValueOpcode::LoadSharedU32x4:
		case ValueOpcode::WriteSharedU8: case ValueOpcode::WriteSharedU16:
		case ValueOpcode::WriteSharedU32: case ValueOpcode::WriteSharedU32x2:
		case ValueOpcode::WriteSharedU32x3: case ValueOpcode::WriteSharedU32x4:
		case ValueOpcode::SetAttribute: return true;
		default: return false;
	}
}

// Blocks on a CFG cycle. A value carried around a loop passes a phi in one of them, and the
// same SSA condition may hold a different value in the next iteration there.
std::unordered_set<const Block*> CyclicBlocks(const BlockList& blocks) {
	struct Frame {
		Block* block;
		size_t next;
	};
	std::unordered_map<const Block*, uint32_t> index, low;
	std::unordered_set<const Block*>          on_stack, cyclic;
	std::vector<Block*>                       stack;
	uint32_t                                  counter = 0;
	for (auto* root: blocks) {
		if (index.contains(root)) continue;
		std::vector<Frame> frames {{root, 0}};
		index[root] = low[root] = counter++;
		stack.push_back(root);
		on_stack.insert(root);
		while (!frames.empty()) {
			auto&      frame      = frames.back();
			const auto successors = frame.block->ImmSuccessors();
			if (frame.next < successors.size()) {
				auto* next = successors[frame.next++];
				if (next == frame.block) cyclic.insert(next);
				if (!index.contains(next)) {
					index[next] = low[next] = counter++;
					stack.push_back(next);
					on_stack.insert(next);
					frames.push_back({next, 0});
				} else if (on_stack.contains(next)) {
					low[frame.block] = std::min(low[frame.block], index[next]);
				}
				continue;
			}
			auto* block = frame.block;
			frames.pop_back();
			if (!frames.empty()) {
				low[frames.back().block] = std::min(low[frames.back().block], low[block]);
			}
			if (low[block] != index[block]) continue;
			std::vector<Block*> component;
			Block*              member = nullptr;
			do {
				member = stack.back();
				stack.pop_back();
				on_stack.erase(member);
				component.push_back(member);
			} while (member != block);
			if (component.size() > 1) cyclic.insert(component.begin(), component.end());
		}
	}
	return cyclic;
}

class Observer {
public:
	explicit Observer(const BlockList& blocks): m_cyclic(CyclicBlocks(blocks)) {
		for (const auto* block: blocks) {
			if (auto* inst = block->condition.TryInstruction()) m_block_inputs.insert(inst);
			if (auto* inst = block->indirect_target.TryInstruction()) m_block_inputs.insert(inst);
		}
	}

	// Whether lanes where `condition` is false may observe `root`'s value.
	bool Observable(Inst* root, const Value& condition) {
		m_visited.clear();
		m_pending.assign(1, {root, true});
		m_visited[root] = MaskedBit;
		while (!m_pending.empty()) {
			const auto [inst, masking] = m_pending.back();
			m_pending.pop_back();
			if (m_block_inputs.contains(inst)) return true;
			for (const auto& use: inst->Uses()) {
				auto*      user = use.user;
				const auto op   = user->GetOpcode();
				if (user->Parent() == nullptr) return true;
				if (masking && IsSelect(op) && use.operand == 1 &&
				    user->Arg(0).Resolve() == condition) {
					continue; // Taken only where the condition holds.
				}
				if (IsExecGuarded(op)) {
					const auto exec = user->NumArgs() - 1;
					if (!masking || use.operand == exec || user->Arg(exec).Resolve() != condition) {
						return true;
					}
					if (user->GetType() == Type::Void) continue;
					// A guarded load reads zero where the condition is false; follow it anyway.
				} else if (!IsLaneLocal(op)) {
					return true;
				}
				const bool next_masking =
				    masking && !(op == ValueOpcode::Phi && m_cyclic.contains(user->Parent()));
				const uint8_t bit  = next_masking ? MaskedBit : UnmaskedBit;
				auto&         seen = m_visited[user];
				// Visiting unmasked covers a masked visit too.
				if ((seen & (bit | UnmaskedBit)) != 0) continue;
				seen |= bit;
				if (m_visited.size() > MaxVisited) return true;
				m_pending.push_back({user, next_masking});
			}
		}
		return false;
	}

private:
	static constexpr size_t  MaxVisited  = 8192;
	static constexpr uint8_t MaskedBit   = 1;
	static constexpr uint8_t UnmaskedBit = 2;

	std::unordered_set<const Block*>       m_cyclic;
	std::unordered_set<const Inst*>        m_block_inputs;
	std::unordered_map<Inst*, uint8_t>     m_visited;
	std::vector<std::pair<Inst*, bool>>    m_pending;
};

} // namespace

MaskedSelectStats EliminateMaskedSelects(const BlockList& blocks) {
	MaskedSelectStats stats;
	// Repeated writes under one condition: Select(c, a, Select(c, b, d)) is Select(c, a, d).
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			if (!IsSelect(inst.GetOpcode()) || inst.Arg(0).IsImmediate()) continue;
			const auto condition = inst.Arg(0).Resolve();
			for (;;) {
				const auto* inner = inst.Arg(2).Resolve().TryInstruction();
				if (inner == nullptr || inner->GetOpcode() != inst.GetOpcode() ||
				    inner->Arg(0).Resolve() != condition) {
					break;
				}
				inst.SetArg(2, inner->Arg(2));
			}
		}
	}
	Observer observer(blocks);
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			if (!IsSelect(inst.GetOpcode()) || inst.Arg(0).IsImmediate()) continue;
			++stats.selects;
			const auto condition = inst.Arg(0).Resolve();
			if (observer.Observable(&inst, condition)) continue;
			inst.ReplaceUsesWith(inst.Arg(1));
			++stats.eliminated;
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
