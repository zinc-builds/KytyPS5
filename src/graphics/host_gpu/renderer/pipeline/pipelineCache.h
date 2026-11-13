#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
struct ShaderParams;

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW
class CommandBuffer;

namespace ShaderRecompiler {
struct CompileOptions;
struct ShaderSource;
struct TranslateResult;
} // namespace ShaderRecompiler

namespace ShaderRecompiler::IR {
class ImageHeapSource;
} // namespace ShaderRecompiler::IR

namespace ShaderRecompiler::Spirv {
struct Profile;
} // namespace ShaderRecompiler::Spirv

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	// 0 without tessellation, 3 for a rectangle list, else the input control points.
	uint32_t                   patch_control_points     = 0;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       alpha_to_coverage_enable = false;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       provoking_vtx_last                                 = false;
	vk::PolygonMode            polygon_mode                                       = vk::PolygonMode::eFill;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
};

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride   = 0;
		uint8_t  instance = 0;
	};
	struct Attribute {
		uint32_t offset  = 0;
		uint8_t  binding = 0;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;
};

// Everything a graphics pipeline is made of besides the stage modules its ids name. Aligned so
// that rendering.color_formats can be passed to Vulkan.
struct alignas(4) GraphicsPipelineKey {
	// VS, LS or mesh; HS; TES; PS. 0 marks an absent stage.
	std::array<uint64_t, 4>  stage_ids {};
	PipelineRenderingState   rendering;
	PipelineVertexInputState vertex_input;
	PipelineStaticParameters static_params;

	bool operator==(const GraphicsPipelineKey& other) const;
};

#pragma pack(pop)

// Keys are compared and hashed as bytes.
static_assert(std::has_unique_object_representations_v<GraphicsPipelineKey>);

// What a graphics pipeline key takes from a draw besides its registers and programs: the
// attachments it renders to (DrawRenderState's color_info up to color_count, and depth_info)
// and its primitive assembly. Built from a final state, or by the prepare worker from its
// proposed targets; compared to check the worker's assumption.
struct GraphicsPipelineTargets {
	struct Color {
		vk::Format format  = vk::Format::eUndefined;
		uint32_t   samples = 0;
		uint8_t    slot    = 0;
		// ColorComponentMapping::packed.
		uint8_t    export_mapping = 0;
		// The guest format is k11_11_10UNorm.
		uint8_t    unorm_11_11_10 = 0;
		uint8_t    reserved       = 0;
	};
	std::array<Color, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	uint32_t                                        color_count = 0;
	// Undefined: no depth or stencil attachment.
	vk::Format                                      depth_format  = vk::Format::eUndefined;
	uint32_t                                        depth_samples = 0;
	vk::PrimitiveTopology                           topology = vk::PrimitiveTopology::ePointList;
	uint32_t                                        primitive_restart_enable = 0;

	// Equal for the key: the colors past color_count are not compared.
	[[nodiscard]] bool Same(const GraphicsPipelineTargets& other) const;
};

// A graphics pipeline key the prepare worker built for a queued draw, valid for the draw if its
// programs are adopted and its final targets equal `targets`.
struct alignas(64) WorkerPipelineKey {
	uint64_t                hash  = 0;
	bool                    ready = false;
	GraphicsPipelineTargets targets;
	GraphicsPipelineKey     key;
};

// A compiled stage with what pipeline creation needs from it besides the key.
struct ShaderModule {
	uint64_t                                     id     = 0;
	vk::ShaderModule                             module = nullptr;
	ShaderRecompiler::IR::CompiledShaderInfo     info;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	std::vector<uint32_t>                        function_code;
	// VS or LS: the host format of each vertex input attribute.
	std::vector<vk::Format>                      vertex_formats;
	// Rectangle-list PS: the parameters it reads.
	std::vector<RectListParameter>               rect_parameters;
	// Indirect mesh draw: a task shader reads the records and launches the groups.
	bool                                         mesh_task = false;
	// A program with GPU-selected calls loaded from the shader store: the serialized resource
	// plan it was compiled with (the plan depends on the callee code in function_code).
	std::vector<uint8_t>                         stored_call_plan;
};

// N trivially copyable elements, left unconstructed until Construct: value-initializing a draw
// state's eight color attachments and three vertex stages wrote ~16 KB for every draw.
template <typename T, size_t N>
class UninitializedArray {
	static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

public:
	UninitializedArray() {} // NOLINT(modernize-use-equals-default): leaves the elements unset.
	UninitializedArray(std::initializer_list<T> init) {
		EXIT_IF(init.size() > N);
		size_t i = 0;
		for (const auto& value: init) {
			Construct(i++, value);
		}
	}

	// Replaces element i, constructed or not.
	template <typename... Args>
	T& Construct(size_t i, Args&&... args) {
		return *std::construct_at(&m_items[i], std::forward<Args>(args)...);
	}
	[[nodiscard]] T&       operator[](size_t i) { return m_items[i]; }
	[[nodiscard]] const T& operator[](size_t i) const { return m_items[i]; }
	[[nodiscard]] T*       data() { return m_items; }
	[[nodiscard]] const T* data() const { return m_items; }

private:
	union {
		T m_items[N];
	};
};

// A draw's topology, shader stages (from PipelineCache::GetGraphicsPrograms) and targets.
struct DrawRenderState {
	RenderDepthInfo depth_info;
	// Constructed up to the slots PrepareDrawRenderState resolves, and always element 0.
	UninitializedArray<RenderColorInfo, RENDER_COLOR_ATTACHMENTS_MAX> color_info;
	uint32_t                                                          color_count = 0;
	// Color targets the pixel program writes.
	uint32_t              mrt_mask                 = 0;
	vk::PrimitiveTopology topology                 = vk::PrimitiveTopology::ePointList;
	bool                  primitive_restart_enable = false;
	bool                  ps_active                = false;
	// The first vertex_stage_count are constructed: LS, HS and ES with tessellation, else VS.
	UninitializedArray<ShaderVertexInputInfo, 3> vertex_info;
	uint32_t                                     vertex_stage_count = 1;
	ShaderPixelInputInfo                         ps_input_info;
	// Input: the user-data slots a GPU-read indirect draw patches.
	ShaderIndirectOffsets indirect_offsets;
	bool                  indirect_draw = false;

	[[nodiscard]] std::span<const ShaderVertexInputInfo> VertexStages() const {
		return {vertex_info.data(), vertex_stage_count};
	}
};

// Writes the shader store on its own thread, running jobs in the order they were posted.
// An exclusive lock on <path>.lock (flock, or LockFileEx on Windows), tried once without waiting
// and held until destruction: one emulator writes a store or driver cache at a time, and pruning
// removes only files nobody holds. The lock file itself is never removed.
class StoreLock {
public:
	StoreLock() = default;
	explicit StoreLock(const std::filesystem::path& path);
	StoreLock(StoreLock&& other) noexcept: m_handle(other.m_handle) { other.m_handle = kNone; }
	StoreLock& operator=(StoreLock&& other) noexcept;
	StoreLock(const StoreLock&)            = delete;
	StoreLock& operator=(const StoreLock&) = delete;
	~StoreLock() { Release(); }
	[[nodiscard]] bool Held() const { return m_handle != kNone; }

private:
	static constexpr intptr_t kNone = -1; // also INVALID_HANDLE_VALUE
	void                      Release();
	intptr_t                  m_handle = kNone;
};

class ShaderStoreWriter {
public:
	ShaderStoreWriter();
	// Keeps the first `kept` bytes of the store at `path`, or starts it over with `header`.
	void Open(const std::filesystem::path& path, std::span<const uint8_t> header, uint64_t kept);
	// Whether the store is open.
	explicit operator bool() const { return !m_file.IsInvalid(); }
	// Takes the store's writer lock (<path>.lock, held until destruction): one emulator appends
	// to a store at a time. False when another process holds it, or on failure; such a store is
	// still read, just not written. Call before reading the store, so no other writer can append
	// between the read and Open.
	bool Lock(const std::filesystem::path& path);
	// Compresses `record` and appends it on the store thread, in call order.
	void Append(std::vector<uint8_t>&& record);
	void Post(Common::UniqueFunction<void>&& job);
	// Runs the queued jobs and stops the thread; jobs posted later never run. Safe to call again.
	void Finish() { m_thread = {}; }

private:
	friend struct PipelineCacheTestAccess;

	StoreLock                                m_lock;
	Common::File                             m_file;
	std::mutex                               m_mutex;
	std::condition_variable_any              m_ready;
	std::queue<Common::UniqueFunction<void>> m_jobs;
	// Declared last, so it drains the queue and joins before the rest is destroyed.
	std::jthread m_thread;
};

// The owning renderer serializes access, including saves while the GPU is running.
class PipelineCache {
public:
	// Without a shader store path nothing is kept across runs.
	PipelineCache(GraphicContext& graphics, ShaderRecompiler::IR::ImageHeapSource* image_heap,
	              const std::filesystem::path& shader_store = {});
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	// Finishes the queued writes and stops the store thread, then writes the driver cache if it
	// changed.
	void Save();

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		// The push constant range's stages, which every update names.
		vk::ShaderStageFlags    push_stages;
		bool                    uses_push_descriptors = false;
		bool                    uses_image_heap       = false;
	};

	// What one lookup materializes for a stage. Lookups write it, not the shared Program, so a
	// stage's resources belong to the draw or dispatch that looked it up.
	struct StageResources {
		ShaderRecompiler::IR::ResourceSnapshot       snapshot;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
	};
	// A draw's stages, indexed like GraphicsPipelineKey::stage_ids.
	using GraphicsResources = std::array<StageResources, 4>;

	// The color slots the pixel shader exports to, and whether it runs at all (DrawRenderState::
	// ps_active). Also used by frame skipping to name a draw's active shaders.
	struct PixelOutput {
		uint32_t color_output_mask = 0;
		bool     active            = false;
	};
	static PixelOutput PixelOutputs(const HW::Context& context, const HW::Shader& shaders);

	// Reads the shaders and registers bound in `command` into the state's shader stages.
	void GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state);
	// The same from given registers, materializing into `resources`, which must outlive the
	// draw. On the draw prepare worker (DrawPrepare::OnWorker) it only finds what exists: a
	// program or module miss, a call-target program, a new tessellation layout or an image heap
	// marks the preparation failed (DrawPrepare::Fail) and leaves the state incomplete. The
	// command thread must not look up programs while the worker does.
	void GetGraphicsPrograms(const HW::Context& context, const HW::UserConfig& user_config,
	                         const HW::Shader& shaders, DrawRenderState& state,
	                         GraphicsResources& resources);
	// Null skips the dispatch.
	const ShaderModule* GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                      ShaderComputeInputInfo&      input_info);

	Pipeline& GetGraphicsPipeline(const CommandBuffer& command, const DrawRenderState& state);
	// The same with the prepare worker's key for the draw (null: none). The key is used if the
	// state's final targets are those it assumed (KYTY_WORKER_PIPELINE_KEY_VERIFY=1 also builds
	// the key here and exits on a difference); else the key is built here.
	Pipeline& GetGraphicsPipeline(const CommandBuffer& command, const DrawRenderState& state,
	                              const WorkerPipelineKey* proposal);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info);

	// Prepare worker: builds `proposal` for a queued draw whose programs (`state`) it looked up,
	// with the targets it proposed (`targets`, see GraphicsPipelineTargets) and its registers.
	// Leaves proposal.ready false where the command thread would stop or exit instead.
	// KYTY_WORKER_PIPELINE_KEY=0 turns worker keys off, =stats prints WORKERPIPELINEKEY lines.
	void ProposeGraphicsPipelineKey(const HW::Context& registers, const HW::UserConfig& user_config,
	                                const DrawRenderState&         state,
	                                const GraphicsPipelineTargets& targets,
	                                WorkerPipelineKey&             proposal) const;
	[[nodiscard]] static bool WorkerPipelineKeysEnabled();
	// The targets of a draw's final state.
	[[nodiscard]] static GraphicsPipelineTargets FinalTargets(const DrawRenderState& state);

private:
	friend struct PipelineCacheTestAccess;

	struct Program {
		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan              resource_plan;
		// A deque, so a module stays where it is while the program compiles more.
		std::deque<ShaderModule>                        modules;
		bool                                            skip_dispatch = false;
		// Loaded from the shader store with GPU-selected calls: call_source is rebuilt from the
		// guest code at the first lookup on the command thread, before any module is used.
		bool                                            stored_calls  = false;

		[[nodiscard]] std::span<const uint32_t> FunctionCode() const;
		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime);
		template <bool HasCalls>
		ShaderModule* FindModule(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                         uint32_t push_data_cursor, StageResources& resources);
	};

	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	// Variants of one source share a bucket; ProgramKey equality compares the state.
	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const { return key.hash; }
	};

	// A key with its hash, for lookups that have the hash already.
	struct HashedGraphicsPipelineKey {
		const GraphicsPipelineKey& key;
		uint64_t                   hash;
	};
	struct GraphicsPipelineKeyHash {
		using is_transparent = void;
		std::size_t operator()(const GraphicsPipelineKey& key) const;
		std::size_t operator()(const HashedGraphicsPipelineKey& key) const { return key.hash; }
	};
	struct GraphicsPipelineKeyEqual {
		using is_transparent = void;
		bool operator()(const GraphicsPipelineKey& a, const GraphicsPipelineKey& b) const {
			return a == b;
		}
		bool operator()(const HashedGraphicsPipelineKey& a, const GraphicsPipelineKey& b) const {
			return a.key == b;
		}
		bool operator()(const GraphicsPipelineKey& a, const HashedGraphicsPipelineKey& b) const {
			return a == b.key;
		}
	};

	using ModuleMap = std::unordered_map<uint64_t, const ShaderModule*>;

	GraphicContext&                        m_graphics;
	ShaderRecompiler::IR::ImageHeapSource* m_image_heap;
	std::unordered_map<ProgramKey, Program, ProgramKeyHash> m_program_cache;
	// Modules compiled per stage.
	std::array<uint32_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> m_compiled {};
	// The compile in progress is a playtest-injected miss (not stored).
	bool m_injected_miss = false;
	// Graphics stages indexed like GraphicsPipelineKey::stage_ids, and the dispatch's stage. A
	// stage's resources stay valid until the next lookup of that stage kind.
	GraphicsResources                      m_graphics_resources;
	StageResources                         m_compute_resources;
	vk::PipelineCache             m_driver_cache = nullptr;
	std::filesystem::path         m_driver_cache_path;
	// Held while this emulator may write the driver cache; taken again at a save if another
	// emulator held it at start.
	mutable StoreLock             m_driver_lock;
	std::chrono::steady_clock::time_point m_next_driver_cache_save;
	bool                          m_driver_cache_dirty = false;
	std::unordered_map<GraphicsPipelineKey, Pipeline, GraphicsPipelineKeyHash,
	                   GraphicsPipelineKeyEqual>
	    m_graphics_pipelines;
	// The last graphics lookup's map entry and key hash: most draws repeat it, and comparing the
	// key is cheaper than hashing it. Map nodes keep their address, and entries are only erased
	// before the first lookup (warm-up) and in the destructor.
	const GraphicsPipelineKey* m_last_graphics_key      = nullptr;
	Pipeline*                  m_last_graphics_pipeline = nullptr;
	uint64_t                   m_last_graphics_hash     = 0;
	// Command thread: how GetGraphicsPipeline found its key, for KYTY_WORKER_PIPELINE_KEY=stats.
	struct WorkerKeyStats {
		uint64_t draws = 0, adopted = 0, none = 0, mismatched = 0, verified = 0, start_ns = 0;
	} m_worker_key_stats;
	std::unordered_map<uint64_t, Pipeline>                                     m_compute_pipelines;
	std::unordered_map<uint64_t, ShaderTessellationInputInfo>                  m_tess_layouts;
	// Records everything first created if a shader store is open, and writes the driver cache.
	ShaderStoreWriter m_store;

	// The key of a draw with `targets` (FinalTargets, or the worker's proposal). False, on the
	// worker only, where the command thread would exit.
	bool BuildGraphicsPipelineKey(const HW::Context& registers, const HW::UserConfig& user_config,
	                              const DrawRenderState&         state,
	                              const GraphicsPipelineTargets& targets, bool worker,
	                              GraphicsPipelineKey& key) const;
	// The pipeline of a built key, created if new.
	Pipeline& FindGraphicsPipeline(const GraphicsPipelineKey& key, uint64_t hash,
	                               const DrawRenderState& state);
	void      PrintWorkerKeyStats();

	template <typename InputInfo>
	const ShaderModule* GetProgram(const ShaderParams& params, InputInfo& input_info,
	                               uint32_t& push_data_cursor, StageResources& resources);
	ShaderModule& CompileModule(Program& program, const ProgramKey& key,
	                            ShaderRecompiler::CompileOptions  options,
	                            ShaderRecompiler::TranslateResult translated,
	                            uint32_t                          push_data_cursor,
	                            const ShaderRecompiler::IR::ResourceSpecialization& specialization);
	void InitializeDriverCache();
	// Marks the driver cache changed and, once per interval, has the store thread write it.
	void CheckpointDriverCache();
	void WriteDriverCache() const;

	// pipelineSerialization.cpp
	// Loads the store at `path`; when it does not exist yet, first adopts the first of `adopt`
	// whose header matches (a store an install kept in its own directory).
	void WarmUp(const std::filesystem::path& path, std::span<const std::filesystem::path> adopt = {});
	bool LoadRecord(std::span<const uint8_t>& records);
	void StoreModule(const ProgramKey& key, const Program& program, const ShaderModule& module,
	                 const std::vector<uint32_t>& spirv);
	void StorePipeline(const GraphicsPipelineKey& key);
	void StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout);
	// Every module by id, with id 0 naming an absent stage.
	[[nodiscard]] ModuleMap MapModules() const;
	// False when a stage id of the key is not in `modules`.
	bool CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
	                            const ModuleMap& modules);
};

// Where the shader stores and the driver's pipeline cache live, shared by every build and install:
// KYTY_SHADER_STORE_DIR if set, else the user's cache directory (Linux: $XDG_CACHE_HOME or
// ~/.cache, then kyty-shaders; macOS: ~/Library/Caches/kyty-shaders; Windows:
// %LOCALAPPDATA%\kyty-shaders), else the install's own _PipelineCache.
std::filesystem::path ShaderStoreDir();
// The install-local directory stores lived in before ShaderStoreDir, adopted from on first use.
inline const std::filesystem::path& InstallStoreDir() {
	static const std::filesystem::path dir("_PipelineCache");
	return dir;
}
// The shader store of the running title (<dir>/<title>.shaders, before KeyedStorePath); empty
// unless --shader-cache is on in a Release build without debug dumps, which change the SPIR-V.
std::filesystem::path ShaderStorePath();
// `base` with a suffix for everything the store's header holds (the codegen source hash, the
// device and the code-changing switches): <stem>.<16 hex digits><ext>. Builds and switch
// combinations that would record different modules use different files, so none truncates
// another's, and a rebuild that keeps the header finds its store warm.
std::filesystem::path KeyedStorePath(const GraphicContext& graphics, const std::filesystem::path& base);
// `path` with a suffix unique to this process and call, for a file written and then renamed.
std::filesystem::path TempPathFor(const std::filesystem::path& path);
// Copies the first of `candidates` whose first bytes equal `header` to `store` (through a temporary
// file and a rename), unless `store` exists. Returns the adopted path, or empty. The caller holds
// the store's StoreLock, so no other emulator creates `store` meanwhile.
std::filesystem::path AdoptStore(const std::filesystem::path& store, std::span<const uint8_t> header,
                                 std::span<const std::filesystem::path> candidates);
// Removes stores of the same title (<title>.*<ext> beside `current`) not written for `max_age`,
// and all but the `keep` most recently written, never `current` nor a store whose StoreLock
// another emulator holds. Returns how many it removed.
size_t PruneStores(const std::filesystem::path& current, size_t keep, std::chrono::hours max_age);
// Host capabilities that change the emitted SPIR-V.
ShaderRecompiler::Spirv::Profile SpirvProfile(const GraphicContext& graphics);

// Stages are indexed like GraphicsPipelineKey::stage_ids.
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const GraphicsPipelineKey&                 key,
                            const std::array<const ShaderModule*, 4>& stages,
                            vk::PipelineCache                          driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderModule& module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
