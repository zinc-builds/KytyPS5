#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/deferredRecorder.h"
#include "graphics/host_gpu/renderer/drawPrepare.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/renderer/workerTextures.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart.
struct DrawIndirectArgs {
	uint64_t              address = 0;
	uint32_t              count   = 0;
	uint32_t              stride  = 0;
	ShaderIndirectOffsets offsets;
};

struct DrawIndexArgs {
	uint32_t         index_count         = 0;
	const void*      index_addr          = nullptr;
	uint32_t         instance_count      = 0;
	uint32_t         index_type_and_size = 0;
	int32_t          base_vertex         = 0;
	uint32_t         first_instance      = 0;
	DrawOffsetSource offset_source       = DrawOffsetSource::DrawState;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
};

struct DrawAutoArgs {
	uint32_t         vertex_count   = 0;
	uint32_t         instance_count = 0;
	uint32_t         first_vertex   = 0;
	uint32_t         first_instance = 0;
	DrawOffsetSource offset_source  = DrawOffsetSource::DrawState;
	DrawIndirectArgs indirect;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept { return m_buffer == nullptr; }

	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;

	// For recording directly: first waits until the recording thread has recorded everything
	// queued with Record().
	[[nodiscard]] vk::CommandBuffer Handle() const noexcept {
		if (m_recorder != nullptr) {
			m_recorder->Drain();
		}
		if (m_dispatch_barrier_pending) {
			RecordDispatchBarrier();
		}
		return m_buffer;
	}
	// Like Handle(), but keeps a deferred dispatch barrier pending: only for a dispatch's own
	// pipeline, descriptor and push-constant binds, which access no memory.
	[[nodiscard]] vk::CommandBuffer BindingHandle() const noexcept {
		if (m_recorder != nullptr) {
			m_recorder->Drain();
		}
		return m_buffer;
	}
	// Dispatch chains (RenderExecutor::DispatchDirect): a dispatch's closing barrier is recorded
	// before whatever is recorded next (Handle(), Record(), End()), unless the next command is a
	// dispatch the guest let overlap with it, which drops it.
	void DeferDispatchBarrier() const noexcept { m_dispatch_barrier_pending = true; }
	[[nodiscard]] bool DispatchBarrierPending() const noexcept { return m_dispatch_barrier_pending; }
	void DropDispatchBarrier() const noexcept { m_dispatch_barrier_pending = false; }
	void FlushDispatchBarrier() const noexcept {
		if (m_dispatch_barrier_pending) {
			(void)Handle();
		}
	}
	// Records f(vk::CommandBuffer) on the recording thread, in order with everything else, or now
	// if this buffer has none. Queued commands start recording at Publish().
	template <typename F>
	void Record(F&& f) const {
		FlushDispatchBarrier();
		if (m_recorder != nullptr) {
			m_recorder->Record(m_buffer, std::forward<F>(f));
		} else {
			f(m_buffer);
		}
	}
	// Records f(vk::CommandBuffer, data) where fill(data) writes `size` bytes now.
	template <typename Fill, typename F>
	void Record(size_t size, Fill&& fill, F&& f) const {
		FlushDispatchBarrier();
		if (m_recorder != nullptr) {
			m_recorder->Record(m_buffer, size, std::forward<Fill>(fill), std::forward<F>(f));
		} else {
			thread_local std::vector<std::byte> data;
			data.resize(size);
			fill(data.data());
			f(m_buffer, data.data());
		}
	}
	void Publish() const {
		if (m_recorder != nullptr) {
			m_recorder->Publish();
		}
	}
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;
	void RecordDispatchBarrier() const noexcept;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer = nullptr;
	DeferredRecorder*   m_recorder = nullptr; // the render scheduler's recording thread
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	mutable bool        m_dispatch_barrier_pending = false;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
	friend class RenderExecutor; // commits queued draws from their register copies
};

namespace Pssr {
struct Request;
}

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	// may_chain: the command processor's packet loop dispatches, and ends chains (below).
	void DispatchDirect(CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y,
	                    uint32_t thread_group_z, uint32_t mode, bool async_compute,
	                    bool may_chain = false);
	void DispatchIndirect(CommandBuffer& buffer, uint64_t args_addr, uint32_t mode,
	                      bool async_compute);
	// The command processor calls this before any packet that may order GPU work (anything
	// but register writes and dispatches): consecutive direct dispatches the guest let overlap
	// share their barriers until then (see DispatchDirect).
	void EndDispatchChain(CommandBuffer& buffer);
	void ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);

	// Draws whose only shader buffer writes are atomics to buffers they access no other way
	// share one write barrier: consecutive ones skip the barriers (and render pass breaks) around
	// each draw. This records the batch's deferred closing barrier.
	void FlushAtomicWrites(CommandBuffer& buffer);
	[[nodiscard]] bool HasAtomicWrites() const { return static_cast<bool>(m_atomic_batch.stages); }

	// Threaded draw preparation (DrawPrepare): a direct draw whose registers describe an ordinary
	// draw is queued. Its programs are looked up on the prepare worker from a copy of the
	// registers while the draw queued before it is committed here, from its own copy. Anything
	// else that uses the caches or records must FinishQueuedDraws first, and nothing stays queued
	// outside the command processor's packet loop. KYTY_PREPARE_THREAD=0 draws at once instead.
	void QueueDrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args);
	void QueueDrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args);
	void FinishQueuedDraws(CommandBuffer& buffer) {
		if (m_queued_count == 0) {
			return;
		}
		m_prepare_stat.drains++;
		while (m_queued_count != 0) {
			FinishQueuedDraw(buffer);
		}
	}
	[[nodiscard]] bool HasQueuedDraw() const { return m_queued_count != 0; }

private:
	[[nodiscard]] static bool ChainsDispatch(const ShaderComputeInputInfo& input_info,
	                                         const PreparedBindings& bindings, uint32_t mode);
	void               PrintDispatchChainStats();
	struct DispatchChain {
		bool                                  hazard_barrier = false; // the chain has one
		uint64_t                              dispatches     = 0;
		uint64_t                              chained        = 0;
		std::chrono::steady_clock::time_point last {};
	} m_dispatch_chain;

	// The buffer side of a queued draw, prepared by the worker (BufferCache::Speculation): the
	// stages' buffer ranges and the clean buffers holding them and its vertex ranges.
	struct WorkerBuffers {
		bool                                                       ready       = false;
		uint32_t                                                   stage_count = 0;
		std::array<const ShaderStageRuntime*, 4>                   runtimes {};
		std::array<std::vector<PreparedBindings::BufferSource>, 4> sources;
		// Speculation index of each merged vertex range (AcquireVertexBuffers' order).
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> vertex_speculated {};
		uint32_t                                             vertex_count = 0;
		BufferCache::Speculation                             speculation;
	};

	// A queued draw: what it was given, its registers when queued, and the worker's lookup.
	struct QueuedDraw {
		HW::Context                      registers;
		HW::UserConfig                   user_config;
		HW::Shader                       shaders;
		bool                             indexed = true;
		DrawIndexArgs                    index_args;
		DrawAutoArgs                     auto_args;
		DrawRenderState                  state;
		PipelineCache::GraphicsResources resources;
		DrawPrepare::Log                 log;
		uint64_t                         sequence = 0;
		// DrawPrepare::HazardGeneration, GuestSyncEpoch::CurrentUnranged and
		// DrawPrepare::StoredSequence when it was queued.
		uint64_t                         hazard_generation = 0;
		uint64_t                         sync_epoch        = 0;
		uint64_t                         stored_sequence   = 0;
		WorkerBuffers                    buffers;
		WorkerTextures                   textures;
		// Resolves the command thread made for this draw, for the worker's memo (see
		// TextureMemoRecord): appended while the command thread owns the slot, applied and
		// cleared by the worker before its next job on it.
		std::vector<TextureMemoRecord> texture_updates;
	};
	// The worker's buffer side for the queued draw being committed, while it may be used.
	const WorkerBuffers* m_adopted_buffers = nullptr;
	void                 SpeculateBuffers(QueuedDraw& queued);
	// The worker's texture side for the queued draw being committed, while it may be used.
	const WorkerTextures* m_adopted_textures = nullptr;
	// A prepare worker's resolve memo. Open addressing by TextureDescriptionKey (a mixed hash),
	// linear probing, at most half full; records are only replaced or all dropped, so no
	// tombstones.
	struct WorkerTextureMemo {
		struct Entry {
			uint64_t                             key = 0;
			std::unique_ptr<WorkerTextureRecord> record; // null: empty
		};
		std::vector<Entry> entries;
		size_t             size = 0;
		// The entry holding `key`, or the empty one it would go to.
		[[nodiscard]] Entry& Slot(uint64_t key) {
			const auto mask = entries.size() - 1;
			for (auto index = (key ^ (key >> 29u)) & mask;; index = (index + 1) & mask) {
				auto& entry = entries[index];
				if (entry.record == nullptr || entry.key == key) {
					return entry;
				}
			}
		}
	};
	// One prepare worker's texture side: its memo, records it replaced in its last jobs, and
	// the next serial of its records (odd, unique among workers: see PrepareLane).
	struct WorkerTextureState {
		WorkerTextureMemo                                              records;
		std::vector<std::vector<std::unique_ptr<WorkerTextureRecord>>> retired;
		uint64_t                                                       jobs   = 0;
		uint64_t                                                       serial = 1;
		uint64_t                                                       serial_step = 2;
	};
	// Prepare worker, after SpeculateBuffers: proposes the draw's texture resolves from its memo.
	void SpeculateTextures(QueuedDraw& queued, WorkerTextureState& state);
	// Resolves made for the queued draw being committed, handed to its slot after the draw.
	std::vector<TextureMemoRecord> m_texture_updates;
	// WorkerTextureRecord serials: odd from the workers, even from the command thread.
	uint64_t m_command_texture_serial = 0;
	// The worker's proposals for a stage being prepared, if it made them for this runtime.
	[[nodiscard]] const std::vector<TextureProposal>* AdoptedTextureProposals(
	    const ShaderStageRuntime& runtime) const;

	// False, with nothing drawn, when the draw's indirect records need CPU counts. A queued draw
	// brings its registers (bound to `buffer`) and the worker's lookup.
	bool DrawIndex(CommandBuffer& buffer, const DrawIndexArgs& args, QueuedDraw* queued = nullptr);
	bool DrawAuto(CommandBuffer& buffer, const DrawAutoArgs& args, QueuedDraw* queued = nullptr);
	// The slot for a draw to queue, or null when it must be drawn at once: preparation threading
	// is off, or the registers select a special operation (metadata, resolve, depth copy) or no
	// draw at all.
	QueuedDraw* NextQueuedDraw(const CommandBuffer& buffer);
	// Copies the registers into the filled slot, hands it to the worker and commits the oldest
	// queued draws beyond the queue depth.
	void        PostQueuedDraw(CommandBuffer& buffer, QueuedDraw& queued);
	// Commits the oldest queued draw.
	void        FinishQueuedDraw(CommandBuffer& buffer);
	// The queued draw's programs: the worker's if nothing they depend on changed since it was
	// queued, else looked up here.
	void AdoptPrograms(QueuedDraw& queued, DrawRenderState& state);
	static void PrepareQueuedDraw(void* lane, uint64_t job);
	// The worker a queued draw goes to, and its job number there.
	[[nodiscard]] DrawPrepare::Worker& PrepareWorker(uint64_t sequence) const {
		return *m_prepare_lanes[(sequence - 1) % m_prepare_lanes.size()]->worker;
	}
	[[nodiscard]] uint64_t PrepareJob(uint64_t sequence) const {
		return (sequence - 1) / m_prepare_lanes.size() + 1;
	}
	// Waits until every worker has finished its posted jobs.
	void WaitPrepareIdle();
	[[nodiscard]] uint64_t PrepareBusyNs() const;

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		PreparedBindings                pixel;
	};

	// The description of a texture binding, kept for inputs seen before.
	[[nodiscard]] const TextureDescription&
	FindTextureDescription(const ShaderRecompiler::IR::ImageResource&   resource,
	                       const ShaderRecompiler::IR::DescriptorValue& value);

	// A stage image slot's last resolve, reused while its descriptor is unchanged and the texture
	// cache would find the same image.
	struct TextureResolveMemo {
		// The resource fields a resolve reads (TextureDescriptionInputs) and the T#.
		ShaderRecompiler::IR::ImageResource   inputs;
		ShaderRecompiler::IR::DescriptorValue value;
		TextureBinding                        binding; // Without mip views.
		bool                                  reusable = false;
		bool                                  null     = false;
		// Otherwise what FindImage returned, for TextureCache::RefindExactImage.
		ImageId                 found_id;
		GuestRange              data;
		uint64_t                generation          = 0;
		uint32_t                metadata_base_layer = 0;
		bool                    color_clear         = false;
		TextureCache::ImageDesc found; // With color_clear.
	};
	// ResolveTexture that records in `trace` whether and how its result can be reused.
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value,
	                                            TextureResolveMemo*                          trace,
	                                            const TextureDescription* known = nullptr);
	void ResolveProposedTexture(const WorkerTextureRecord&                   record,
	                            const ShaderRecompiler::IR::ImageResource&   resource,
	                            const ShaderRecompiler::IR::DescriptorValue& value,
	                            TextureBinding&                              out);
	void VerifyTextureResolve(const TextureBinding&                        adopted,
	                          const ShaderRecompiler::IR::ImageResource&   resource,
	                          const ShaderRecompiler::IR::DescriptorValue& value);
	// The memo slots of a stage module's images.
	[[nodiscard]] std::vector<TextureResolveMemo>& TextureMemoSlots(const ShaderModule& module);
	// ResolveTexture for image `index` of a stage, through the stage's memo slots, into `out`.
	// out keeps its mip view storage, emptied.
	// proposal: the prepare worker's for this binding, or null.
	void ResolveStageTexture(std::vector<TextureResolveMemo>*             memo, uint32_t index,
	                         const ShaderRecompiler::IR::ImageResource&   resource,
	                         const ShaderRecompiler::IR::DescriptorValue& value, TextureBinding& out,
	                         const TextureProposal*                       proposal = nullptr);

	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slot, bool ignore_target_mask = false,
	                              bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                   const DrawEmitInfo& emit, DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                          DrawRenderState& state, QueuedDraw* queued);
	void                      ExecutePreparedDraw(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                              DrawRenderState& state, const DrawEmitInfo& emit,
	                                              const DrawIndexBufferSource& index_source);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                        CommandBuffer& command, uint32_t group_x,
	                                        uint32_t group_y, uint32_t group_z, uint32_t mode);

	// GET_LOD_STATS counters since their last reset. With --lod-stats a bound T# with counting
	// enabled counts as sampled at its BASE_LEVEL, clamped when its MIN_LOD lies above that level.
	struct MipStatsCounter {
		uint8_t finest_level = 15; // no samples
		bool    clamped      = false;
		bool    unclamped    = false;
	};

	// Guest ranges since the batch's opening barrier: written by its atomics, and accessed in
	// other ways by draws between them.
	struct AtomicWriteBatch {
		vk::PipelineStageFlags stages;
		RangeSet               atomic;
		RangeSet               plain;
	};

	RenderContext&                        m_context;
	AtomicWriteBatch                      m_atomic_batch;
	bool                                  m_lod_stats = Config::LodStatsEnabled();
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	vk::DescriptorSetLayout               m_pssr_descriptor_layout = nullptr;
	vk::PipelineLayout                    m_pssr_pipeline_layout   = nullptr;
	vk::Pipeline                          m_pssr_pipeline          = nullptr;
	std::array<MipStatsCounter, 256>      m_mip_stats {};
	struct TextureDescriptionEntry {
		ShaderRecompiler::IR::ImageResource   inputs;
		ShaderRecompiler::IR::DescriptorValue value;
		TextureDescription                    description;
	};
	// Descriptions by input hash; an entry is used only when its inputs compare equal.
	std::unordered_map<uint64_t, TextureDescriptionEntry> m_texture_descriptions;
	// Image slots of each stage module by its id: a program's modules move as it compiles more.
	std::unordered_map<uint64_t, std::vector<TextureResolveMemo>> m_texture_memo;
	// A color target slot's last resolve, reused while its registers are unchanged and the
	// texture cache would find the same image.
	struct ColorTargetMemo {
		HW::RenderTarget rt {};
		uint32_t         mask                = 0;
		bool             ignore_target_mask  = false;
		bool             exact_format        = false;
		uint64_t         generation          = 0; // 0: not reusable.
		uint32_t         metadata_base_layer = 0;
		RenderColorInfo  result;
	};
	std::array<ColorTargetMemo, RENDER_COLOR_ATTACHMENTS_MAX> m_color_target_memo {};
	// The depth target's last description and image, kept like ColorTargetMemo.
	struct DepthTargetMemo {
		HW::DepthRenderTarget   z {};
		uint64_t                generation          = 0; // 0: not reusable.
		uint32_t                metadata_base_layer = 0;
		TextureCache::ImageDesc desc;
		ImageId                 image_id;
	} m_depth_target_memo;
	struct TextureMemoStats {
		uint64_t lookups = 0, hits = 0, changed = 0, stale = 0, unreusable = 0;
		uint64_t start_ns = 0;
	} m_texture_memo_stats;
	struct {
		uint64_t lookups = 0, hits = 0;
	} m_color_target_stats;

	// Draws queued for the prepare worker, in a ring of depth + 1 slots: up to `depth` queued
	// (prepared or being prepared) and one being filled. Slot i takes sequences i + 1, i + 1 +
	// ring size, ...
	// A prepare worker and what only it uses. Draw `sequence` goes to worker (sequence - 1) % N
	// as its job (sequence - 1) / N + 1; the ring's size is a multiple of N, so a slot always
	// goes to the same worker (its texture_updates are for that worker's memo).
	struct PrepareLane {
		RenderExecutor*                      executor = nullptr;
		uint32_t                             index    = 0;
		WorkerTextureState                   textures;
		std::unique_ptr<DrawPrepare::Worker> worker;
	};
	std::vector<std::unique_ptr<PrepareLane>>        m_prepare_lanes;
	std::vector<std::unique_ptr<QueuedDraw>>         m_queued_draws;
	uint64_t                                         m_queued_posted = 0; // sequences posted
	uint32_t                                         m_queued_count  = 0; // posted, not committed
	bool                                             m_prepare_verify = false;
	bool                                             m_prepare_stats  = false;
	struct PrepareStats {
		uint64_t queued = 0, direct = 0, adopted = 0, failed = 0, hazard = 0, gpu_written = 0;
		uint64_t stored = 0;
		uint64_t wait_ns = 0, verified = 0, worker_busy_ns = 0, start_ns = 0;
		uint64_t drains = 0, waits = 0; // drains: FinishQueuedDraws with draws queued
	} m_prepare_stat;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
