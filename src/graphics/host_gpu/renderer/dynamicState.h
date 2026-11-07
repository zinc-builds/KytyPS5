#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_

#include "common/assert.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;

struct StencilOps {
	vk::StencilOp fail_op {};
	vk::StencilOp pass_op {};
	vk::StencilOp depth_fail_op {};
	vk::CompareOp compare_op {};

	bool operator==(const StencilOps& other) const {
		return fail_op == other.fail_op && pass_op == other.pass_op &&
		       depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
	}
};

// Bytes CopyDescriptorWrites needs for writes and the descriptors they point to.
size_t DescriptorWritesSize(std::span<const vk::WriteDescriptorSet> writes);
// Copies writes and their buffer or image descriptors to data, pointing the copies at the copies.
void CopyDescriptorWrites(std::span<const vk::WriteDescriptorSet> writes, std::byte* data);

// Push constants and set-0 push descriptors recorded last in the current command buffer, kept by
// the thread that records (the recording thread, or the GPU command thread without one). A push
// reuses them only while the pipeline layout and the command thread's epoch stay the same.
class RecordedBindings {
public:
	// A pipeline with this layout was bound.
	void Use(uint64_t epoch, vk::PipelineLayout layout);
	// The dwords of [offset, offset + size) that differ from the last push, as {offset, size} in
	// bytes; size 0 if none.
	[[nodiscard]] std::pair<uint32_t, uint32_t> PlanConstants(uint64_t epoch, vk::PipelineLayout layout,
	                                                          uint32_t offset, uint32_t size,
	                                                          const std::byte* data);
	// The runs of writes' elements that differ from the last push; valid until the next call.
	[[nodiscard]] std::span<const vk::WriteDescriptorSet>
	PlanDescriptors(uint64_t epoch, vk::PipelineLayout layout,
	                std::span<const vk::WriteDescriptorSet> writes);

private:
	struct Write {
		uint32_t           binding = 0;
		vk::DescriptorType type {};
		uint32_t           count = 0;
		uint32_t           first = 0; // index into m_buffers or m_images
	};

	uint64_t                              m_epoch           = ~uint64_t {0};
	vk::PipelineLayout                    m_layout          = nullptr;
	std::array<uint32_t, 32>              m_constants {};
	uint32_t                              m_constants_known = 0; // bit per dword
	bool                                  m_writes_valid    = false;
	std::vector<Write>                    m_writes;
	std::vector<vk::DescriptorBufferInfo> m_buffers;
	std::vector<vk::DescriptorImageInfo>  m_images;
	std::vector<vk::WriteDescriptorSet>   m_pending;
};

// The graphics pipeline recorded last in the current command buffer, so a draw records only what
// changed. Push constants and push descriptors are compared on the thread that records them
// (RecordedBindings): this side only advances an epoch that voids them. Anything else that binds
// or pushes graphics state must call Reset() or ForgetLayout().
class GraphicsBindings {
public:
	void Reset() {
		m_pipeline = nullptr;
		m_epoch++;
	}

	// Something else bound graphics descriptor sets: forget what was pushed.
	void ForgetLayout() { m_epoch++; }

	[[nodiscard]] vk::Pipeline BoundPipeline() const { return m_pipeline; }
	void BindPipeline(const CommandBuffer& buffer, vk::Pipeline pipeline,
	                  vk::PipelineLayout layout);
	// Queue pushes for the recording thread, which records the parts that differ from what it
	// pushed last with this layout. offset and size in bytes, multiples of 4.
	void PushConstants(const CommandBuffer& buffer, vk::PipelineLayout layout,
	                   vk::ShaderStageFlags stages, uint32_t offset, uint32_t size,
	                   const void* data);
	void PushDescriptors(const CommandBuffer& buffer, vk::PipelineLayout layout,
	                     std::span<const vk::WriteDescriptorSet> writes);

private:
	vk::Pipeline     m_pipeline = nullptr;
	uint64_t         m_epoch    = 0;
	// The recording side's state. Only the thread that records touches it.
	RecordedBindings m_recorded;
	// KYTY_BIND_VERIFY=1: the same comparison made here, checked against the recording side's.
	RecordedBindings m_verify;
};

struct DynamicState {
	struct {
		bool viewports : 1;
		bool scissors  : 1;

		bool depth_test_enabled  : 1;
		bool depth_write_enabled : 1;
		bool depth_compare_op    : 1;

		bool depth_bounds_test_enabled : 1;
		bool depth_bounds              : 1;

		bool depth_bias_enabled : 1;
		bool depth_bias         : 1;

		bool stencil_test_enabled       : 1;
		bool stencil_front_ops          : 1;
		bool stencil_front_reference    : 1;
		bool stencil_front_write_mask   : 1;
		bool stencil_front_compare_mask : 1;
		bool stencil_back_ops           : 1;
		bool stencil_back_reference     : 1;
		bool stencil_back_write_mask    : 1;
		bool stencil_back_compare_mask  : 1;

		bool cull_mode  : 1;
		bool front_face : 1;

		bool blend_constants       : 1;
		bool line_width            : 1;
		bool feedback_loop_aspects : 1;
	} dirty_state {};

	std::array<vk::Viewport, 16> viewports {};
	std::array<vk::Rect2D, 16>   scissors {};
	uint32_t                   num_viewports {};
	uint32_t                   num_scissors {};

	bool          depth_test_enabled {};
	bool          depth_write_enabled {};
	vk::CompareOp depth_compare_op {};

	bool  depth_bounds_test_enabled {};
	float depth_bounds_min {};
	float depth_bounds_max {};

	bool  depth_bias_enabled {};
	float depth_bias_constant {};
	float depth_bias_clamp {};
	float depth_bias_slope {};

	bool       stencil_test_enabled {};
	StencilOps stencil_front_ops {};
	uint32_t   stencil_front_reference {};
	uint32_t   stencil_front_write_mask {};
	uint32_t   stencil_front_compare_mask {};
	StencilOps stencil_back_ops {};
	uint32_t   stencil_back_reference {};
	uint32_t   stencil_back_write_mask {};
	uint32_t   stencil_back_compare_mask {};

	vk::CullModeFlags cull_mode {};
	vk::FrontFace     front_face {};

	std::array<float, 4> blend_constants {};
	float                line_width {};
	vk::ImageAspectFlags feedback_loop_aspects {};

	/// Records the changed dynamic state into the command buffer.
	void Commit(const GraphicContext& graphics, const CommandBuffer& buffer);

	GraphicsBindings bindings;

	/// Invalidates all dynamic state to be flushed into the next command buffer.
	void Invalidate() {
		std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
		bindings.Reset();
	}

	void SetViewports(std::span<const vk::Viewport> viewports_) {
		EXIT_IF(viewports_.size() > viewports.size());
		if (!std::ranges::equal(std::span(viewports).first(num_viewports), viewports_)) {
			std::ranges::copy(viewports_, viewports.begin());
			num_viewports         = static_cast<uint32_t>(viewports_.size());
			dirty_state.viewports = true;
		}
	}

	void SetScissors(std::span<const vk::Rect2D> scissors_) {
		EXIT_IF(scissors_.size() > scissors.size());
		if (!std::ranges::equal(std::span(scissors).first(num_scissors), scissors_)) {
			std::ranges::copy(scissors_, scissors.begin());
			num_scissors         = static_cast<uint32_t>(scissors_.size());
			dirty_state.scissors = true;
		}
	}

	void SetDepthTestEnabled(const bool enabled) {
		if (depth_test_enabled != enabled) {
			depth_test_enabled             = enabled;
			dirty_state.depth_test_enabled = true;
		}
	}

	void SetDepthWriteEnabled(const bool enabled) {
		if (depth_write_enabled != enabled) {
			depth_write_enabled             = enabled;
			dirty_state.depth_write_enabled = true;
		}
	}

	void SetDepthCompareOp(const vk::CompareOp compare_op) {
		if (depth_compare_op != compare_op) {
			depth_compare_op             = compare_op;
			dirty_state.depth_compare_op = true;
		}
	}

	void SetDepthBoundsTestEnabled(const bool enabled) {
		if (depth_bounds_test_enabled != enabled) {
			depth_bounds_test_enabled             = enabled;
			dirty_state.depth_bounds_test_enabled = true;
		}
	}

	void SetDepthBounds(const float min, const float max) {
		if (depth_bounds_min != min || depth_bounds_max != max) {
			depth_bounds_min         = min;
			depth_bounds_max         = max;
			dirty_state.depth_bounds = true;
		}
	}

	void SetDepthBiasEnabled(const bool enabled) {
		if (depth_bias_enabled != enabled) {
			depth_bias_enabled             = enabled;
			dirty_state.depth_bias_enabled = true;
		}
	}

	void SetDepthBias(const float constant, const float clamp, const float slope) {
		if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
		    depth_bias_slope != slope) {
			depth_bias_constant    = constant;
			depth_bias_clamp       = clamp;
			depth_bias_slope       = slope;
			dirty_state.depth_bias = true;
		}
	}

	void SetStencilTestEnabled(const bool enabled) {
		if (stencil_test_enabled != enabled) {
			stencil_test_enabled             = enabled;
			dirty_state.stencil_test_enabled = true;
		}
	}

	void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
		if (stencil_front_ops != front_ops) {
			stencil_front_ops             = front_ops;
			dirty_state.stencil_front_ops = true;
		}
		if (stencil_back_ops != back_ops) {
			stencil_back_ops             = back_ops;
			dirty_state.stencil_back_ops = true;
		}
	}

	void SetStencilReferences(const uint32_t front_reference, const uint32_t back_reference) {
		if (stencil_front_reference != front_reference) {
			stencil_front_reference             = front_reference;
			dirty_state.stencil_front_reference = true;
		}
		if (stencil_back_reference != back_reference) {
			stencil_back_reference             = back_reference;
			dirty_state.stencil_back_reference = true;
		}
	}

	void SetStencilWriteMasks(const uint32_t front_write_mask, const uint32_t back_write_mask) {
		if (stencil_front_write_mask != front_write_mask) {
			stencil_front_write_mask             = front_write_mask;
			dirty_state.stencil_front_write_mask = true;
		}
		if (stencil_back_write_mask != back_write_mask) {
			stencil_back_write_mask             = back_write_mask;
			dirty_state.stencil_back_write_mask = true;
		}
	}

	void SetStencilCompareMasks(const uint32_t front_compare_mask,
	                            const uint32_t back_compare_mask) {
		if (stencil_front_compare_mask != front_compare_mask) {
			stencil_front_compare_mask             = front_compare_mask;
			dirty_state.stencil_front_compare_mask = true;
		}
		if (stencil_back_compare_mask != back_compare_mask) {
			stencil_back_compare_mask             = back_compare_mask;
			dirty_state.stencil_back_compare_mask = true;
		}
	}

	void SetCullMode(const vk::CullModeFlags cull_mode_) {
		if (cull_mode != cull_mode_) {
			cull_mode             = cull_mode_;
			dirty_state.cull_mode = true;
		}
	}

	void SetFrontFace(const vk::FrontFace front_face_) {
		if (front_face != front_face_) {
			front_face             = front_face_;
			dirty_state.front_face = true;
		}
	}

	void SetBlendConstants(const std::array<float, 4> blend_constants_) {
		if (blend_constants != blend_constants_) {
			blend_constants             = blend_constants_;
			dirty_state.blend_constants = true;
		}
	}

	void SetLineWidth(const float width) {
		if (line_width != width) {
			line_width             = width;
			dirty_state.line_width = true;
		}
	}

	void SetAttachmentFeedbackLoopAspects(const vk::ImageAspectFlags aspects) {
		if (feedback_loop_aspects != aspects) {
			feedback_loop_aspects             = aspects;
			dirty_state.feedback_loop_aspects = true;
		}
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DYNAMICSTATE_H_
