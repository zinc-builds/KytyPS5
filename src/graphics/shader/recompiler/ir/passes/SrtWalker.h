#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

// Image descriptors at base + first + k * step, k < count, within size bytes that a sampled
// root selects by GPU-computed byte offset. The host keeps them resident instead of
// materializing them.
struct ImageHeapRequest {
	uint64_t             base  = 0;
	uint64_t             size  = 0;
	uint64_t             step  = 0;
	uint64_t             first = 0;
	uint64_t             count = 0;
	const ImageResource* root  = nullptr;
};

struct ImageHeapView {
	uint64_t                         slots      = 0; // Device address of one slot per heap dword.
	uint32_t                         slot_count = 0; // Keys past it select a trailing null slot.
	std::span<const DescriptorValue> kinds; // One representative descriptor per kind.
};

class ImageHeapSource {
public:
	// False when the host does not keep the table resident.
	virtual bool Acquire(const ImageHeapRequest& request, ImageHeapView& view) = 0;

protected:
	~ImageHeapSource() = default;
};

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
	uint64_t                  indirect_args_address = 0;
	ImageHeapSource*          image_heap            = nullptr;
};

class SrtReadCapture {
public:
	SrtReadCapture(SrtRuntime source, std::vector<std::pair<uint64_t, uint64_t>>& ranges):
	    m_source(source), m_ranges(ranges) {}
	SrtRuntime ObservedRuntime();

private:
	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> values);

	SrtRuntime                                  m_source;
	std::vector<std::pair<uint64_t, uint64_t>>& m_ranges;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uniform across a compute wave, including values derived from its workgroup ID.
// This does not imply that the host can evaluate the value.
bool ValidateWaveUniformValue(const ResourcePlan& program, Value value);
// Compiles the plan's x86 walker. Without values it walks the plan's resources; otherwise it
// only evaluates the values, with every block reachable.
void CompileSrtWalker(ResourcePlan& plan, std::span<const Value> values = {});
// The scratch a walk of plan on this thread uses: the plan's own on the command thread, its
// prepare_scratch on the prepare thread. Walks of one plan on one thread must not overlap.
struct SrtWalkScratchRef {
	std::vector<uint64_t>&                   srt;
	std::vector<uint32_t>&                   material_keys;
	std::vector<ResourcePlan::SelectorKeys>& selector_keys;
};
SrtWalkScratchRef SrtWalkScratch(const ResourcePlan& plan);
// Marks the calling thread as the prepare thread, whose walks use separate scratch.
void SetSrtWalkPrepareThread(bool prepare);
// Runs the walker. flat receives the scalar reads of reachable blocks and reads, when non-null,
// one DWORD range per guest load. False when a reachable read is unknown.
bool WalkSrt(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
             std::vector<std::pair<uint64_t, uint64_t>>* reads);
// Results of the last walk; false when unknown.
bool WalkedValue(const ResourcePlan& plan, Value value, uint32_t& result);
bool WalkedDescriptor(const ResourcePlan& plan, uint32_t source, DescriptorValue& result);
bool WalkedActive(const ResourcePlan& plan, uint32_t source);
bool WalkedImageActive(const ResourcePlan& plan, uint32_t image);
// Length of the guest load at rip if it belongs to the walker running on this thread, else 0.
// The exception handler completes such a load as a zero read.
uint32_t SrtWalkerLoadLength(uint64_t rip);
// Reads a DWORD of guest memory without faulting.
enum class SrtWalkerRead : uint8_t {
	Ok,
	GpuWritten, // the bytes hold GPU writes not yet read back: reading them waits for the GPU
	Failed,     // read in place
};
using SrtWalkerReader = SrtWalkerRead (*)(uint64_t address, uint32_t* value);
// Page-tracker layout the walker's loads check (the page manager asserts it matches).
constexpr uint32_t SrtWalkerTrackedAddressBits = 44;
constexpr uint32_t SrtWalkerRegionBits         = 22;
constexpr uint32_t SrtWalkerPageBits           = 12;
constexpr uint8_t  SrtWalkerReadWatchedBit     = 0x80;
// Lets walker loads skip the fault for pages the tracker protects against CPU reads.
// regions[address >> SrtWalkerRegionBits] is null or points to a region whose byte at
// pages_offset + page index has SrtWalkerReadWatchedBit set while reads of that page fault;
// those loads call read first. Set before any walker runs; null regions disables the check.
void SetSrtWalkerGuestAccess(const void* const* regions, uint64_t pages_offset,
                             SrtWalkerReader read);
// While set on this thread, walker loads of GPU-written bytes evaluate as unknown instead of
// waiting for the GPU. Unknown branch conditions keep every successor reachable; a walk that
// needs such a value fails, and MaterializeResources then repeats it with the wait.
class SrtWalkerUnknownGpuWrites {
public:
	SrtWalkerUnknownGpuWrites();
	~SrtWalkerUnknownGpuWrites();
	SrtWalkerUnknownGpuWrites(const SrtWalkerUnknownGpuWrites&)            = delete;
	SrtWalkerUnknownGpuWrites& operator=(const SrtWalkerUnknownGpuWrites&) = delete;

	// Whether a load on this thread evaluated as unknown since the last call; clears it.
	static bool TakeSeen();
	// Turns the mode on or off for this thread; returns whether it was on.
	static bool Exchange(bool enabled);

private:
	bool m_previous;
};

// Nonnegative affine coefficients: element zero is constant, remaining elements are
// caller-selected leaves. Runtime range proofs must additionally exclude U32 wrap.
template <size_t Size>
std::optional<std::array<uint64_t, Size>> AffineIndex(
    Value value, const auto& resolve, const auto& leaf, uint32_t depth = 0) {
	value = resolve(value);
	if (depth > 32u || value.GetType() != Type::U32) return {};
	std::array<uint64_t, Size> result {};
	if (value.IsImmediate()) {
		result[0] = value.U32();
		return result;
	}
	if (const auto index = leaf(value)) {
		result[*index] = 1u;
		return result;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return {};
	const auto op = inst->GetOpcode();
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) return {};
	auto left = AffineIndex<Size>(inst->Arg(0), resolve, leaf, depth + 1u);
	auto right = AffineIndex<Size>(inst->Arg(1), resolve, leaf, depth + 1u);
	if (!left || !right) return {};
	const auto constant = [](const auto& coefficients) {
		return std::ranges::all_of(std::span(coefficients).subspan(1u),
		                          [](uint64_t coefficient) { return coefficient == 0u; });
	};
	if (op == ValueOpcode::IMul32 && !constant(*right)) std::swap(left, right);
	if (op != ValueOpcode::IAdd32 && !constant(*right)) return {};
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32u) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (size_t index = 0; index < Size; ++index) {
		(*left)[index] = op == ValueOpcode::IAdd32
		    ? (*left)[index] + (*right)[index] : (*left)[index] * (*right)[0];
		if ((*left)[index] > UINT32_MAX) return {};
	}
	return left;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
