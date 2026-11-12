#include "graphics/host_gpu/renderer/dynamicState.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"

#include <cstdlib>
#include <cstring>

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
		m_heap_layout     = nullptr;
		m_constants_known = 0;
		m_writes_valid    = false;
	}
}

static size_t DescriptorBytes(const vk::WriteDescriptorSet& write) {
	return write.descriptorCount * (IsBufferDescriptor(write.descriptorType)
	                                    ? sizeof(vk::DescriptorBufferInfo)
	                                    : sizeof(vk::DescriptorImageInfo));
}

size_t DescriptorWritesSize(std::span<const vk::WriteDescriptorSet> writes) {
	size_t size = writes.size_bytes();
	for (const auto& write: writes) {
		size += DescriptorBytes(write);
	}
	return size;
}

void CopyDescriptorWrites(std::span<const vk::WriteDescriptorSet> writes, std::byte* data) {
	auto* copies = reinterpret_cast<vk::WriteDescriptorSet*>(data);
	auto* infos  = data + writes.size_bytes();
	for (size_t i = 0; i < writes.size(); i++) {
		copies[i]        = writes[i];
		const auto bytes = DescriptorBytes(writes[i]);
		if (IsBufferDescriptor(writes[i].descriptorType)) {
			std::memcpy(infos, writes[i].pBufferInfo, bytes);
			copies[i].pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(infos);
		} else {
			std::memcpy(infos, writes[i].pImageInfo, bytes);
			copies[i].pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(infos);
		}
		infos += bytes;
	}
}

// Records vkCmdPushDescriptorSetKHR with copies of the writes and their descriptors.
static void RecordPushDescriptors(const CommandBuffer& buffer, vk::PipelineLayout layout,
                                  std::span<const vk::WriteDescriptorSet> writes) {
	const auto count = static_cast<uint32_t>(writes.size());
	buffer.Record(
	    DescriptorWritesSize(writes),
	    [writes](std::byte* data) { CopyDescriptorWrites(writes, data); },
	    [layout, count](vk::CommandBuffer cb, const std::byte* data) {
		    cb.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0, count,
		                            reinterpret_cast<const vk::WriteDescriptorSet*>(data));
	    });
}

void GraphicsBindings::BindPipeline(const CommandBuffer& buffer, vk::Pipeline pipeline,
                                    vk::PipelineLayout layout) {
	if (pipeline == m_pipeline && BindDedupeEnabled()) {
		return;
	}
	buffer.Record([pipeline](vk::CommandBuffer cb) {
		cb.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	});
	m_pipeline = pipeline;
	UseLayout(layout);
}

void GraphicsBindings::PushConstants(const CommandBuffer& buffer, vk::PipelineLayout layout,
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
	const uint32_t push_offset = (first + changed) * 4u;
	const uint32_t push_size   = (last - changed + 1u) * 4u;
	buffer.Record(
	    push_size, [&](std::byte* out) { std::memcpy(out, bytes + changed * 4u, push_size); },
	    [layout, stages, push_offset, push_size](vk::CommandBuffer cb, const std::byte* values) {
		    cb.pushConstants(layout, stages, push_offset, push_size, values);
	    });
}

void GraphicsBindings::PushDescriptors(const CommandBuffer& buffer, vk::PipelineLayout layout,
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
		RecordPushDescriptors(buffer, layout, writes);
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
		RecordPushDescriptors(buffer, layout, m_pending);
	}
}

// Each setter is queued with copies of its values (CommandBuffer::Record).
void DynamicState::Commit(const GraphicContext& graphics, const CommandBuffer& buffer) {
	using Face = vk::StencilFaceFlagBits;
	const auto stencil_ops = [&buffer](Face face, const StencilOps& ops) {
		buffer.Record([face, ops](vk::CommandBuffer cb) {
			cb.setStencilOp(face, ops.fail_op, ops.pass_op, ops.depth_fail_op, ops.compare_op);
		});
	};
	enum class StencilValue : uint8_t { Reference, WriteMask, CompareMask };
	const auto stencil_value = [&buffer](StencilValue kind, Face face, uint32_t value) {
		buffer.Record([kind, face, value](vk::CommandBuffer cb) {
			switch (kind) {
				case StencilValue::Reference: cb.setStencilReference(face, value); break;
				case StencilValue::WriteMask: cb.setStencilWriteMask(face, value); break;
				case StencilValue::CompareMask: cb.setStencilCompareMask(face, value); break;
			}
		});
	};
	if (dirty_state.viewports) {
		dirty_state.viewports = false;
		buffer.Record([count = num_viewports, values = viewports](vk::CommandBuffer cb) {
			cb.setViewportWithCount(count, values.data());
		});
	}
	if (dirty_state.scissors) {
		dirty_state.scissors = false;
		buffer.Record([count = num_scissors, values = scissors](vk::CommandBuffer cb) {
			cb.setScissorWithCount(count, values.data());
		});
	}
	if (dirty_state.depth_test_enabled) {
		dirty_state.depth_test_enabled = false;
		buffer.Record([value = depth_test_enabled](vk::CommandBuffer cb) {
			cb.setDepthTestEnable(value);
		});
	}
	if (dirty_state.depth_write_enabled) {
		dirty_state.depth_write_enabled = false;
		// Note that this must be set in a command buffer even if depth test is disabled.
		buffer.Record([value = depth_write_enabled](vk::CommandBuffer cb) {
			cb.setDepthWriteEnable(value);
		});
	}
	if (depth_test_enabled && dirty_state.depth_compare_op) {
		dirty_state.depth_compare_op = false;
		buffer.Record([value = depth_compare_op](vk::CommandBuffer cb) {
			cb.setDepthCompareOp(value);
		});
	}
	if (dirty_state.depth_bounds_test_enabled) {
		dirty_state.depth_bounds_test_enabled = false;
#if !defined(__APPLE__)
		buffer.Record([value = depth_bounds_test_enabled](vk::CommandBuffer cb) {
			cb.setDepthBoundsTestEnable(value);
		});
#endif
	}
	if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
		dirty_state.depth_bounds = false;
#if !defined(__APPLE__)
		buffer.Record([min = depth_bounds_min, max = depth_bounds_max](vk::CommandBuffer cb) {
			cb.setDepthBounds(min, max);
		});
#endif
	}
	if (dirty_state.depth_bias_enabled) {
		dirty_state.depth_bias_enabled = false;
		buffer.Record([value = depth_bias_enabled](vk::CommandBuffer cb) {
			cb.setDepthBiasEnable(value);
		});
	}
	if (depth_bias_enabled && dirty_state.depth_bias) {
		dirty_state.depth_bias = false;
		buffer.Record([constant = depth_bias_constant, clamp = depth_bias_clamp,
		               slope = depth_bias_slope](vk::CommandBuffer cb) {
			cb.setDepthBias(constant, clamp, slope);
		});
	}
	if (dirty_state.stencil_test_enabled) {
		dirty_state.stencil_test_enabled = false;
		buffer.Record([value = stencil_test_enabled](vk::CommandBuffer cb) {
			cb.setStencilTestEnable(value);
		});
	}
	if (stencil_test_enabled) {
		if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
		    stencil_front_ops == stencil_back_ops) {
			dirty_state.stencil_front_ops = false;
			dirty_state.stencil_back_ops  = false;
			stencil_ops(Face::eFrontAndBack, stencil_front_ops);
		} else {
			if (dirty_state.stencil_front_ops) {
				dirty_state.stencil_front_ops = false;
				stencil_ops(Face::eFront, stencil_front_ops);
			}
			if (dirty_state.stencil_back_ops) {
				dirty_state.stencil_back_ops = false;
				stencil_ops(Face::eBack, stencil_back_ops);
			}
		}
		// Bit-field dirty flags are passed by copy and written back.
		const auto stencil_pair = [&](bool& front_dirty, bool& back_dirty, uint32_t front,
		                              uint32_t back, StencilValue setter) {
			if (front_dirty && back_dirty && front == back) {
				front_dirty = back_dirty = false;
				stencil_value(setter, Face::eFrontAndBack, front);
				return;
			}
			if (front_dirty) {
				front_dirty = false;
				stencil_value(setter, Face::eFront, front);
			}
			if (back_dirty) {
				back_dirty = false;
				stencil_value(setter, Face::eBack, back);
			}
		};
		bool front = dirty_state.stencil_front_reference;
		bool back  = dirty_state.stencil_back_reference;
		stencil_pair(front, back, stencil_front_reference, stencil_back_reference,
		             StencilValue::Reference);
		dirty_state.stencil_front_reference = front;
		dirty_state.stencil_back_reference  = back;
		front = dirty_state.stencil_front_write_mask;
		back  = dirty_state.stencil_back_write_mask;
		stencil_pair(front, back, stencil_front_write_mask, stencil_back_write_mask,
		             StencilValue::WriteMask);
		dirty_state.stencil_front_write_mask = front;
		dirty_state.stencil_back_write_mask  = back;
		front = dirty_state.stencil_front_compare_mask;
		back  = dirty_state.stencil_back_compare_mask;
		stencil_pair(front, back, stencil_front_compare_mask, stencil_back_compare_mask,
		             StencilValue::CompareMask);
		dirty_state.stencil_front_compare_mask = front;
		dirty_state.stencil_back_compare_mask  = back;
	}
	if (dirty_state.cull_mode) {
		dirty_state.cull_mode = false;
		buffer.Record([value = cull_mode](vk::CommandBuffer cb) { cb.setCullMode(value); });
	}
	if (dirty_state.front_face) {
		dirty_state.front_face = false;
		buffer.Record([value = front_face](vk::CommandBuffer cb) { cb.setFrontFace(value); });
	}
	if (dirty_state.blend_constants) {
		dirty_state.blend_constants = false;
		buffer.Record([values = blend_constants](vk::CommandBuffer cb) {
			cb.setBlendConstants(values.data());
		});
	}
	if (dirty_state.line_width) {
		dirty_state.line_width = false;
		buffer.Record([value = line_width](vk::CommandBuffer cb) { cb.setLineWidth(value); });
	}
	if (dirty_state.feedback_loop_aspects && graphics.attachment_feedback_loop_enabled) {
		dirty_state.feedback_loop_aspects = false;
		buffer.Record([value = feedback_loop_aspects](vk::CommandBuffer cb) {
			cb.setAttachmentFeedbackLoopEnableEXT(value);
		});
	}
}

} // namespace Libs::Graphics
