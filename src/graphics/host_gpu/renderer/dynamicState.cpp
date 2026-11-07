#include "graphics/host_gpu/renderer/dynamicState.h"

#include "graphics/host_gpu/graphicContext.h"

#include <cstdlib>

namespace Libs::Graphics {

// KYTY_BIND_DEDUPE=0 records every bind and push again (for A/B comparisons).
static bool BindDedupeEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BIND_DEDUPE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

static bool IsBufferDescriptor(vk::DescriptorType type) {
	return type == vk::DescriptorType::eStorageBuffer || type == vk::DescriptorType::eUniformBuffer;
}

static bool SameDescriptor(const vk::DescriptorBufferInfo& a, const vk::DescriptorBufferInfo& b) {
	return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
}

static bool SameDescriptor(const vk::DescriptorImageInfo& a, const vk::DescriptorImageInfo& b) {
	return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
}

void GraphicsBindings::UseLayout(vk::PipelineLayout layout) {
	if (layout != m_layout) {
		m_layout          = layout;
		m_constants_known = 0;
		m_writes_valid    = false;
	}
}

void GraphicsBindings::BindPipeline(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline,
                                    vk::PipelineLayout layout) {
	if (pipeline == m_pipeline && BindDedupeEnabled()) {
		return;
	}
	cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	m_pipeline = pipeline;
	UseLayout(layout);
}

void GraphicsBindings::PushConstants(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                                     vk::ShaderStageFlags stages, uint32_t offset, uint32_t size,
                                     const void* data) {
	const uint32_t first = offset / 4u;
	const uint32_t count = size / 4u;
	EXIT_IF((offset | size) % 4u != 0 || count == 0 || first + count > m_constants.size());
	UseLayout(layout);
	const auto* bytes   = static_cast<const uint8_t*>(data);
	uint32_t    changed = UINT32_MAX;
	uint32_t    last    = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t value = 0;
		std::memcpy(&value, bytes + i * 4u, sizeof(value));
		const uint32_t dword = first + i;
		if ((m_constants_known & (1u << dword)) == 0 || m_constants[dword] != value) {
			m_constants[dword] = value;
			changed            = std::min(changed, i);
			last               = i;
		}
	}
	m_constants_known |= (count == 32u ? ~0u : ((1u << count) - 1u)) << first;
	if (!BindDedupeEnabled()) {
		changed = 0;
		last    = count - 1;
	} else if (changed == UINT32_MAX) {
		return;
	}
	cmdbuf.pushConstants(layout, stages, (first + changed) * 4u, (last - changed + 1u) * 4u,
	                     bytes + changed * 4u);
}

void GraphicsBindings::PushDescriptors(vk::CommandBuffer cmdbuf, vk::PipelineLayout layout,
                                       std::span<const vk::WriteDescriptorSet> writes) {
	UseLayout(layout);
	bool same = m_writes_valid && m_writes.size() == writes.size() && BindDedupeEnabled();
	for (size_t i = 0; same && i < writes.size(); i++) {
		const auto& write = writes[i];
		same = m_writes[i].binding == write.dstBinding && m_writes[i].type == write.descriptorType &&
		       m_writes[i].count == write.descriptorCount && write.dstArrayElement == 0;
	}
	if (!same) {
		m_writes.clear();
		m_buffers.clear();
		m_images.clear();
		for (const auto& write: writes) {
			const bool buffer = IsBufferDescriptor(write.descriptorType);
			EXIT_IF(write.dstArrayElement != 0 ||
			        (buffer ? write.pBufferInfo == nullptr : write.pImageInfo == nullptr));
			m_writes.push_back({write.dstBinding, write.descriptorType, write.descriptorCount,
			                    static_cast<uint32_t>(buffer ? m_buffers.size() : m_images.size())});
			if (buffer) {
				m_buffers.insert(m_buffers.end(), write.pBufferInfo,
				                 write.pBufferInfo + write.descriptorCount);
			} else {
				m_images.insert(m_images.end(), write.pImageInfo,
				                write.pImageInfo + write.descriptorCount);
			}
		}
		m_writes_valid = true;
		cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
		                            static_cast<uint32_t>(writes.size()), writes.data());
		return;
	}
	// Push runs of changed array elements; the others keep their pushed values.
	m_pending.clear();
	for (size_t i = 0; i < writes.size(); i++) {
		const auto& write  = writes[i];
		const auto& record = m_writes[i];
		const bool  buffer = IsBufferDescriptor(write.descriptorType);
		const auto  changed = [&](uint32_t element) {
            return buffer ? !SameDescriptor(m_buffers[record.first + element],
			                                 write.pBufferInfo[element])
			               : !SameDescriptor(m_images[record.first + element],
			                                 write.pImageInfo[element]);
		};
		for (uint32_t element = 0; element < write.descriptorCount;) {
			if (!changed(element)) {
				element++;
				continue;
			}
			const uint32_t start = element;
			for (; element < write.descriptorCount && changed(element); element++) {
				if (buffer) {
					m_buffers[record.first + element] = write.pBufferInfo[element];
				} else {
					m_images[record.first + element] = write.pImageInfo[element];
				}
			}
			auto run            = write;
			run.dstArrayElement = start;
			run.descriptorCount = element - start;
			if (buffer) {
				run.pBufferInfo = write.pBufferInfo + start;
			} else {
				run.pImageInfo = write.pImageInfo + start;
			}
			m_pending.push_back(run);
		}
	}
	if (!m_pending.empty()) {
		cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
		                            static_cast<uint32_t>(m_pending.size()), m_pending.data());
	}
}

void DynamicState::Commit(const GraphicContext& graphics, vk::CommandBuffer cmdbuf) {
	if (dirty_state.viewports) {
		dirty_state.viewports = false;
		cmdbuf.setViewportWithCount(num_viewports, viewports.data());
	}
	if (dirty_state.scissors) {
		dirty_state.scissors = false;
		cmdbuf.setScissorWithCount(num_scissors, scissors.data());
	}
	if (dirty_state.depth_test_enabled) {
		dirty_state.depth_test_enabled = false;
		cmdbuf.setDepthTestEnable(depth_test_enabled);
	}
	if (dirty_state.depth_write_enabled) {
		dirty_state.depth_write_enabled = false;
		// Note that this must be set in a command buffer even if depth test is disabled.
		cmdbuf.setDepthWriteEnable(depth_write_enabled);
	}
	if (depth_test_enabled && dirty_state.depth_compare_op) {
		dirty_state.depth_compare_op = false;
		cmdbuf.setDepthCompareOp(depth_compare_op);
	}
	if (dirty_state.depth_bounds_test_enabled) {
		dirty_state.depth_bounds_test_enabled = false;
#if !defined(__APPLE__)
		cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
#endif
	}
	if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
		dirty_state.depth_bounds = false;
#if !defined(__APPLE__)
		cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
#endif
	}
	if (dirty_state.depth_bias_enabled) {
		dirty_state.depth_bias_enabled = false;
		cmdbuf.setDepthBiasEnable(depth_bias_enabled);
	}
	if (depth_bias_enabled && dirty_state.depth_bias) {
		dirty_state.depth_bias = false;
		cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
	}
	if (dirty_state.stencil_test_enabled) {
		dirty_state.stencil_test_enabled = false;
		cmdbuf.setStencilTestEnable(stencil_test_enabled);
	}
	if (stencil_test_enabled) {
		if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
		    stencil_front_ops == stencil_back_ops) {
			dirty_state.stencil_front_ops = false;
			dirty_state.stencil_back_ops  = false;
			cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
			                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
			                    stencil_front_ops.compare_op);
		} else {
			if (dirty_state.stencil_front_ops) {
				dirty_state.stencil_front_ops = false;
				cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
				                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
				                    stencil_front_ops.compare_op);
			}
			if (dirty_state.stencil_back_ops) {
				dirty_state.stencil_back_ops = false;
				cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
				                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
				                    stencil_back_ops.compare_op);
			}
		}
		if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
		    stencil_front_reference == stencil_back_reference) {
			dirty_state.stencil_front_reference = false;
			dirty_state.stencil_back_reference  = false;
			cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
			                           stencil_front_reference);
		} else {
			if (dirty_state.stencil_front_reference) {
				dirty_state.stencil_front_reference = false;
				cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
				                           stencil_front_reference);
			}
			if (dirty_state.stencil_back_reference) {
				dirty_state.stencil_back_reference = false;
				cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
			}
		}
		if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
		    stencil_front_write_mask == stencil_back_write_mask) {
			dirty_state.stencil_front_write_mask = false;
			dirty_state.stencil_back_write_mask  = false;
			cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
			                           stencil_front_write_mask);
		} else {
			if (dirty_state.stencil_front_write_mask) {
				dirty_state.stencil_front_write_mask = false;
				cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
				                           stencil_front_write_mask);
			}
			if (dirty_state.stencil_back_write_mask) {
				dirty_state.stencil_back_write_mask = false;
				cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
			}
		}
		if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
		    stencil_front_compare_mask == stencil_back_compare_mask) {
			dirty_state.stencil_front_compare_mask = false;
			dirty_state.stencil_back_compare_mask  = false;
			cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
			                             stencil_front_compare_mask);
		} else {
			if (dirty_state.stencil_front_compare_mask) {
				dirty_state.stencil_front_compare_mask = false;
				cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
				                             stencil_front_compare_mask);
			}
			if (dirty_state.stencil_back_compare_mask) {
				dirty_state.stencil_back_compare_mask = false;
				cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
				                             stencil_back_compare_mask);
			}
		}
	}
	if (dirty_state.cull_mode) {
		dirty_state.cull_mode = false;
		cmdbuf.setCullMode(cull_mode);
	}
	if (dirty_state.front_face) {
		dirty_state.front_face = false;
		cmdbuf.setFrontFace(front_face);
	}
	if (dirty_state.blend_constants) {
		dirty_state.blend_constants = false;
		cmdbuf.setBlendConstants(blend_constants.data());
	}
	if (dirty_state.line_width) {
		dirty_state.line_width = false;
		cmdbuf.setLineWidth(line_width);
	}
	if (dirty_state.feedback_loop_aspects && graphics.attachment_feedback_loop_enabled) {
		dirty_state.feedback_loop_aspects = false;
		cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_aspects);
	}
}

} // namespace Libs::Graphics
