#include "graphics/host_gpu/renderer/dynamicState.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <cstdio>
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

// KYTY_BIND_VERIFY=1: the command thread makes the same comparisons as the recording thread and
// sends its result along; the recording thread exits on a difference.
static bool BindVerifyEnabled() {
	static const bool enabled = std::getenv("KYTY_BIND_VERIFY") != nullptr;
	return enabled;
}

void RecordedBindings::Use(uint64_t epoch, vk::PipelineLayout layout) {
	if (epoch != m_epoch || layout != m_layout) {
		m_epoch           = epoch;
		m_layout          = layout;
		m_constants_known = 0;
		m_writes_valid    = false;
	}
}

std::pair<uint32_t, uint32_t> RecordedBindings::PlanConstants(uint64_t epoch,
                                                              vk::PipelineLayout layout,
                                                              uint32_t offset, uint32_t size,
                                                              const std::byte* data) {
	const uint32_t first = offset / 4u;
	const uint32_t count = size / 4u;
	EXIT_IF((offset | size) % 4u != 0 || count == 0 || first + count > m_constants.size());
	Use(epoch, layout);
	uint32_t changed = UINT32_MAX;
	uint32_t last    = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t value = 0;
		std::memcpy(&value, data + i * 4u, sizeof(value));
		const uint32_t dword = first + i;
		if ((m_constants_known & (1u << dword)) == 0 || m_constants[dword] != value) {
			m_constants[dword] = value;
			changed            = std::min(changed, i);
			last               = i;
		}
	}
	m_constants_known |= (count == 32u ? ~0u : ((1u << count) - 1u)) << first;
	if (!BindDedupeEnabled()) {
		return {offset, size};
	}
	if (changed == UINT32_MAX) {
		return {0, 0};
	}
	return {(first + changed) * 4u, (last - changed + 1u) * 4u};
}

std::span<const vk::WriteDescriptorSet>
RecordedBindings::PlanDescriptors(uint64_t epoch, vk::PipelineLayout layout,
                                  std::span<const vk::WriteDescriptorSet> writes) {
	Use(epoch, layout);
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
		return writes;
	}
	// Runs of changed array elements; the others keep their pushed values.
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
	return m_pending;
}

void GraphicsBindings::BindPipeline(const CommandBuffer& buffer, vk::Pipeline pipeline,
                                    vk::PipelineLayout layout) {
	if (pipeline == m_pipeline && BindDedupeEnabled()) {
		return;
	}
	if (BindVerifyEnabled()) {
		m_verify.Use(m_epoch, layout);
	}
	buffer.Record([pipeline, state = &m_recorded, epoch = m_epoch, layout](vk::CommandBuffer cb) {
		cb.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		state->Use(epoch, layout);
	});
	m_pipeline = pipeline;
}

namespace {
// What the command thread's comparison decided (KYTY_BIND_VERIFY), sent with a push.
struct VerifyRun {
	uint32_t binding;
	uint32_t first;
	uint32_t count;
	uint32_t pad;
};
} // namespace

void GraphicsBindings::PushConstants(const CommandBuffer& buffer, vk::PipelineLayout layout,
                                     vk::ShaderStageFlags stages, uint32_t offset, uint32_t size,
                                     const void* data) {
	EXIT_IF((offset | size) % 4u != 0 || size == 0 || offset + size > 32u * 4u);
	std::pair<uint32_t, uint32_t> expected {UINT32_MAX, 0};
	if (BindVerifyEnabled()) {
		expected = m_verify.PlanConstants(m_epoch, layout, offset, size,
		                                  static_cast<const std::byte*>(data));
	}
	buffer.Record(
	    size, [&](std::byte* out) { std::memcpy(out, data, size); },
	    [state = &m_recorded, epoch = m_epoch, layout, stages, offset, size,
	     expected](vk::CommandBuffer cb, const std::byte* values) {
		    const auto [push_offset, push_size] =
		        state->PlanConstants(epoch, layout, offset, size, values);
		    if (expected.first != UINT32_MAX &&
		        (expected.first != push_offset || expected.second != push_size)) {
			    EXIT("KYTY_BIND_VERIFY: push constants differ: command thread %u+%u, recorder %u+%u\n",
			         expected.first, expected.second, push_offset, push_size);
		    }
		    if (push_size != 0) {
			    cb.pushConstants(layout, stages, push_offset, push_size,
			                     values + (push_offset - offset));
		    }
	    });
}

void GraphicsBindings::PushDescriptors(const CommandBuffer& buffer, vk::PipelineLayout layout,
                                       std::span<const vk::WriteDescriptorSet> writes) {
	thread_local std::vector<VerifyRun> expected;
	expected.clear();
	const bool verify = BindVerifyEnabled();
	if (verify) {
		for (const auto& run: m_verify.PlanDescriptors(m_epoch, layout, writes)) {
			expected.push_back({run.dstBinding, run.dstArrayElement, run.descriptorCount, 0});
		}
	}
	const size_t writes_size = DescriptorWritesSize(writes);
	const size_t verify_size = verify ? (expected.size() + 1) * sizeof(VerifyRun) : 0;
	const auto   count       = static_cast<uint32_t>(writes.size());
	buffer.Record(
	    writes_size + verify_size,
	    [&](std::byte* data) {
		    CopyDescriptorWrites(writes, data);
		    if (verify) {
			    const VerifyRun header {static_cast<uint32_t>(expected.size()), 0, 0, 0};
			    std::memcpy(data + writes_size, &header, sizeof(header));
			    std::memcpy(data + writes_size + sizeof(header), expected.data(),
			                expected.size() * sizeof(VerifyRun));
		    }
	    },
	    [state = &m_recorded, epoch = m_epoch, layout, count, writes_size,
	     verify](vk::CommandBuffer cb, const std::byte* data) {
		    const std::span<const vk::WriteDescriptorSet> copies {
		        reinterpret_cast<const vk::WriteDescriptorSet*>(data), count};
		    const auto runs = state->PlanDescriptors(epoch, layout, copies);
		    if (verify) {
			    VerifyRun header {};
			    std::memcpy(&header, data + writes_size, sizeof(header));
			    bool same = header.binding == runs.size();
			    for (size_t i = 0; same && i < runs.size(); i++) {
				    VerifyRun run {};
				    std::memcpy(&run, data + writes_size + (i + 1) * sizeof(VerifyRun), sizeof(run));
				    same = run.binding == runs[i].dstBinding && run.first == runs[i].dstArrayElement &&
				           run.count == runs[i].descriptorCount;
			    }
			    if (!same) {
				    EXIT("KYTY_BIND_VERIFY: push descriptors differ: command thread %u runs, "
				         "recorder %zu runs\n",
				         header.binding, runs.size());
			    }
			    static std::atomic<uint64_t> verified {0};
			    if ((verified.fetch_add(1, std::memory_order_relaxed) % 1000000u) == 999999u) {
				    std::printf("KYTY_BIND_VERIFY: %llu pushes match\n",
				                static_cast<unsigned long long>(verified.load()));
				    std::fflush(stdout);
			    }
		    }
		    if (!runs.empty()) {
			    cb.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
			                            static_cast<uint32_t>(runs.size()), runs.data());
		    }
	    });
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
