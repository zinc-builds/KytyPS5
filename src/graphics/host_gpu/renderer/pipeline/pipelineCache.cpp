#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/drawPrepare.h"

#include "common/assert.h"
#include "common/frameDiag.h"
#include "common/playtest.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/kernelStats.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/host_gpu/vulkanDiagnostics.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/Tessellation.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <spirv-tools/libspirv.hpp>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr auto DriverCacheSaveInterval = std::chrono::minutes {1};

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// PA_SU_SC_MODE_CNTL.POLY_MODE disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	// Driver cache entries already distinguish shader code and pipeline state across builds.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n",
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	if (values.empty()) {
		return false;
	}
	if (DrawPrepare::OnWorker()) {
		// GPU-written bytes would need a download on the command thread; they are checked where
		// the draw is adopted.
		DrawPrepare::NoteRead(address, values.size_bytes());
		return Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes());
	}
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

// The image heap of lookups on the prepare worker: acquiring host texture state is the command
// thread's, so a draw that would use one is prepared there.
class PrepareWorkerImageHeap final: public ShaderRecompiler::IR::ImageHeapSource {
public:
	bool Acquire(const ShaderRecompiler::IR::ImageHeapRequest&, ShaderRecompiler::IR::ImageHeapView&) override {
		DrawPrepare::Fail();
		return false;
	}
};
PrepareWorkerImageHeap g_prepare_worker_image_heap;

void DumpShader(std::span<const uint32_t> words, const std::string& name) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	const auto path = Config::GetShaderLogFolder() / name;
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(words.data(), words.size_bytes());
}

void ValidateShaderSpirv(const char* stage, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv, bool maintenance8_enabled) {
	if (!Config::ShaderValidationEnabled()) {
		return;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	spvtools::ValidatorOptions validator_options;
	validator_options.SetAllowOffsetTextureOperand(maintenance8_enabled);
	if (tools.Validate(spirv.data(), spirv.size(), validator_options)) {
		return;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF("%s\n", text.c_str());
	EXIT("ShaderRecompiler %s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s", stage,
	     shader_hash, messages.c_str());
}

// Formats with a narrower host variant drop the components the shader does not read.
vk::Format NarrowInputFormat(vk::Format format, uint32_t used_components) {
	switch (format) {
		case vk::Format::eR32G32B32A32Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR32Sfloat;
				case 2: return vk::Format::eR32G32Sfloat;
				case 3: return vk::Format::eR32G32B32Sfloat;
				default: return format;
			}
		case vk::Format::eR32G32B32Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR32Sfloat;
				case 2: return vk::Format::eR32G32Sfloat;
				default: return format;
			}
		case vk::Format::eR16G16B16A16Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR16Sfloat;
				case 2: return vk::Format::eR16G16Sfloat;
				default: return format;
			}
		case vk::Format::eR8G8B8A8Unorm:
			switch (used_components) {
				case 1: return vk::Format::eR8Unorm;
				case 2: return vk::Format::eR8G8Unorm;
				default: return format;
			}
		case vk::Format::eR8G8B8A8Snorm:
			return used_components == 2 ? vk::Format::eR8G8Snorm : format;
		case vk::Format::eR8G8B8A8Uint:
			switch (used_components) {
				case 1: return vk::Format::eR8Uint;
				case 2: return vk::Format::eR8G8Uint;
				default: return format;
			}
		default: return format;
	}
}

vk::Format InputFormat(const ShaderBufferResource& resource, uint32_t used_components) {
	switch (resource.RawFormat()) {
		// Temporary: invalid PS5 buffer format 113 (VertexAttribFormat::k16_16SInt).
		case 113: return NarrowInputFormat(vk::Format::eR32G32B32A32Sfloat, used_components);
		case 121: return vk::Format::eR16G16Sfloat;
		default: break;
	}
	const auto format = VulkanFormat(resource.Format());
	if (format == vk::Format::eUndefined ||
	    ShaderRecompiler::Format::GetFormatInfo(resource.Format()).component_count == 0) {
		EXIT("unknown vertex format: fmt = %u\n", resource.RawFormat());
	}
	return NarrowInputFormat(format, used_components);
}

// The host format of each vertex input attribute, narrowed to the components the stage reads.
std::vector<vk::Format> VertexInputFormats(const ShaderVertexInputInfo&                    input_info,
                                           const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	std::vector<vk::Format> formats(input_info.resources_num);
	for (int index = 0; index < input_info.resources_num; index++) {
		const auto registers_num = input_info.resources_dst[index].registers_num;
		EXIT_IF(registers_num < 1 || registers_num > 4);
		const auto fetched = program.info.vertex_fetch_components[index];
		formats[index] = InputFormat(input_info.resources[index],
		                             fetched > 0 ? fetched : static_cast<uint32_t>(registers_num));
	}
	return formats;
}

} // namespace

ShaderRecompiler::Spirv::Profile SpirvProfile(const GraphicContext& graphics) {
	// AMD GPUs (vendor 0x1002) run NMin/NMax as v_min/v_max, which give the guest's NaN and
	// signed-zero results; GLSL.std.450 alone does not order -0 below +0.
	return {.maintenance8           = graphics.maintenance8_enabled,
	        .native_float_min_max   = graphics.physical_device_properties.vendorID == 0x1002u,
	        .device_clock_period_ps = graphics.device_clock_period_ps,
	        .robust_buffer_access   = graphics.robust_buffer_access_enabled,
	        .r8_srgb_views          = graphics.r8_srgb_views,
	        .r8g8_srgb_views        = graphics.r8g8_srgb_views,
	        .resident_textures      = graphics.resident_textures};
}

std::filesystem::path ShaderStorePath() {
	if (KYTY_BUILD != KYTY_BUILD_RELEASE || !Config::ShaderCacheEnabled() ||
	    Config::GraphicsDebugDumpEnabled()) {
		return {};
	}
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return {};
	}
	return std::filesystem::path("_PipelineCache") / (title_id + ".shaders");
}

bool GraphicsPipelineKey::operator==(const GraphicsPipelineKey& other) const {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

// Hashes the bytes SameBuiltKey compares: equal keys have equal bytes there.
std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	const auto& vertex = key.vertex_input;
	auto        hash   = XXH3_64bits(&key, offsetof(GraphicsPipelineKey, vertex_input));
	hash = XXH3_64bits_withSeed(&key.static_params, sizeof(key.static_params), hash);
	hash = XXH3_64bits_withSeed(vertex.bindings.data(), vertex.binding_count * sizeof(vertex.bindings[0]),
	                            hash ^ vertex.binding_count);
	return XXH3_64bits_withSeed(vertex.attributes.data(),
	                            vertex.attribute_count * sizeof(vertex.attributes[0]),
	                            hash ^ (static_cast<uint64_t>(vertex.attribute_count) << 8u));
}

std::span<const uint32_t> PipelineCache::Program::FunctionCode() const {
	return call_source
	    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
	    : std::span<const uint32_t>{};
}

bool PipelineCache::Program::RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
	const auto revision = call_source->revision;
	ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
	resource_plan.source_reads = call_source->reads;
	return revision != call_source->revision;
}

template <bool HasCalls>
ShaderModule* PipelineCache::Program::FindModule(const ShaderRecompiler::IR::SrtRuntime& runtime,
                                                 uint32_t push_data_cursor,
                                                 StageResources& resources) {
	Common::KernelStats::SiteScope kernel(Common::KernelStats::Site::SrtWalk);
	if (!ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, resources.snapshot,
	                                               resources.specialization)) {
		// On the worker, a heap it may not acquire can be why.
		EXIT_IF(!DrawPrepare::OnWorker());
		DrawPrepare::Fail();
		return nullptr;
	}
	for (auto& candidate: modules) {
		const auto& layout = candidate.info.bindings;
		if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
		        push_data_cursor, layout.ShaderDataDwords()) ||
		    candidate.specialization != resources.specialization) {
			continue;
		}
		if constexpr (HasCalls) {
			if (!std::ranges::equal(candidate.function_code, FunctionCode())) continue;
		}
		return &candidate;
	}
	return nullptr;
}

ShaderModule& PipelineCache::CompileModule(Program& program, const ProgramKey& key,
                                           ShaderRecompiler::CompileOptions  options,
                                           ShaderRecompiler::TranslateResult translated,
                                           uint32_t                          push_data_cursor,
                                           const ShaderRecompiler::IR::ResourceSpecialization& specialization) {
	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::ShadersCompiled,
	                                         Common::DebugCounters::Counter::ShaderCompileNs,
	                                         Common::DebugCounters::Peak::ShaderCompileNs);
	const auto profile = SpirvProfile(m_graphics);
	options.profile    = &profile;
	auto result = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization,
	                                               push_data_cursor);
	auto info = std::move(result.program).TakeCompiledInfo();

	// The same inputs give the same module id across runs.
	const auto function_code = program.FunctionCode();
	ShaderRecompiler::IR::Writer inputs;
	ShaderRecompiler::IR::IoAll(inputs, key.stage, key.hash, key.user_data_count, key.static_state,
	                            info.bindings.push_data_start_dword, specialization);
	inputs.Raw(function_code.data(), function_code.size_bytes()); // Last, so it needs no size.
	const auto id = XXH3_64bits(inputs.bytes.data(), inputs.bytes.size());

	const auto* stage = ShaderRecompiler::StageName(key.stage);
	DumpShader(result.spirv, fmt::format("{}_{:016x}_{:016x}.spv", stage, key.hash, id));
	VulkanDiagnostics::DumpSpirv(stage, key.hash, id, result.spirv);
	ValidateShaderSpirv(stage, key.hash, result.spirv, profile.maintenance8);
	auto& module = program.modules.emplace_back(ShaderModule {
	    .id             = id,
	    .module         = CompileSPV(result.spirv, m_graphics.device),
	    .info           = std::move(info),
	    .specialization = specialization,
	    .function_code  = {function_code.begin(), function_code.end()},
	});
	m_compiled[static_cast<size_t>(key.stage)]++;
	// Guest geometry shaders are compiled through the host mesh stage.
	std::printf("Shaders: VS %u | PS %u | CS %u | GS %u | LS %u | HS %u | TES %u\n",
	            m_compiled[static_cast<size_t>(ShaderType::Vertex)],
	            m_compiled[static_cast<size_t>(ShaderType::Pixel)],
	            m_compiled[static_cast<size_t>(ShaderType::Compute)],
	            m_compiled[static_cast<size_t>(ShaderType::Mesh)],
	            m_compiled[static_cast<size_t>(ShaderType::Local)],
	            m_compiled[static_cast<size_t>(ShaderType::TessellationControl)],
	            m_compiled[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
	// The inputs of these come from the stage's static key.
	if (key.stage == ShaderType::Vertex || key.stage == ShaderType::Local) {
		module.vertex_formats = VertexInputFormats(*options.input_info.vertex, module.info);
	} else if (key.stage == ShaderType::Pixel &&
	           options.input_info.pixel->parameter_mode == ShaderPixelParameterMode::Rectangle) {
		module.rect_parameters = RectListParameters(*options.input_info.pixel, module.info);
	} else if (key.stage == ShaderType::Mesh) {
		module.mesh_task = options.input_info.vertex->indirect_draw;
	}
	// A program with GPU-selected calls is stored with the callee code it was linked against. An
	// injected miss (playtest) compiles a module the store already has.
	if (!m_injected_miss) {
		StoreModule(key, program, module, result.spirv);
	}
	m_injected_miss = false;
	return module;
}

template <typename InputInfo>
const ShaderModule* PipelineCache::GetProgram(const ShaderParams& params, InputInfo& input_info,
                                              uint32_t& push_data_cursor, StageResources& resources) {
	const auto user_data = std::span(params.user_data).first(params.user_data_count);
	const bool worker = DrawPrepare::OnWorker();
	ShaderRecompiler::IR::SrtRuntime runtime {
	    .user_data                  = user_data,
	    .shader_base                = params.Base(),
	    .read_specialization_memory = ReadShaderGuestMemory,
	    .image_heap                 = worker && m_image_heap != nullptr ? &g_prepare_worker_image_heap
	                                                                    : m_image_heap,
	};
	ShaderRecompiler::CompileOptions options {.wave_size = input_info.wave_size};
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		options.stage            = input_info.logical_stage;
		options.input_info.vertex = &input_info;
		options.user_data_base   = 8;
		if (options.stage == ShaderType::Mesh ||
		    options.stage == ShaderType::TessellationControl) {
			options.user_data_base = 0;
			options.wave_size =
			    options.stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
		}
	} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
		options.stage            = ShaderType::Pixel;
		options.input_info.pixel = &input_info;
	} else {
		static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
		options.stage              = ShaderType::Compute;
		options.input_info.compute = &input_info;
		runtime.workgroup_counts   = input_info.workgroup_counts;
		runtime.indirect_args_address = input_info.indirect_args_address;
	}
	// Per thread, so lookups need no shared scratch; it keeps its capacity between lookups.
	thread_local ProgramKey lookup_key;
	lookup_key.stage           = options.stage;
	lookup_key.hash            = params.hash;
	lookup_key.user_data_count = params.user_data_count;
	BuildStageStaticKey(input_info, lookup_key.static_state);
	auto entry = m_program_cache.find(lookup_key);
	const auto bind = [&](const ShaderModule& module) {
		input_info.stage = {.module = &module, .resources = &resources.snapshot};
		module.info.bindings.AdvancePushData(push_data_cursor);
		return &module;
	};
	bool source_changed = false;
	if (worker) {
		// Only finds: a miss, a compile or a call-target refresh is the command thread's.
		if (entry == m_program_cache.end() || entry->second.call_source ||
		    entry->second.stored_calls) {
			DrawPrepare::Fail();
			return nullptr;
		}
		auto& cached = entry->second;
		if (cached.skip_dispatch) return nullptr;
		auto* module = cached.FindModule<false>(runtime, push_data_cursor, resources);
		// Playtest fault injection: hand graphics draws to the command thread, which misses.
		if (module == nullptr || (!std::is_same_v<InputInfo, ShaderComputeInputInfo> &&
		                          Common::Playtest::Enabled() && Common::Playtest::InjectedMissPending())) {
			DrawPrepare::Fail();
			return nullptr;
		}
		return bind(*module);
	}
	if (entry != m_program_cache.end() && entry->second.stored_calls) {
		// A stored program with GPU-selected calls: relink it against the callees' current code,
		// then use a stored module only if it was compiled for that same code.
		auto& cached        = entry->second;
		cached.stored_calls = false;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code   = params.back_code;
		auto relinked = std::make_unique<ShaderRecompiler::ShaderSource>(
		    ShaderRecompiler::PrepareShaderSource(params.code, options));
		EXIT_IF(relinked->calls.empty() || relinked->unsupported_call_pc != UINT32_MAX);
		ShaderRecompiler::RefreshShaderSource(*relinked, runtime);
		cached.call_source = std::move(relinked);
		const auto function_code = cached.FunctionCode();
		bool       planned       = false;
		for (auto& stored: cached.modules) {
			if (!planned && std::ranges::equal(stored.function_code, function_code) &&
			    ShaderRecompiler::IR::LoadResourcePlan(stored.stored_call_plan, cached.resource_plan)) {
				cached.resource_plan.source_reads = cached.call_source->reads;
				planned                           = true;
			}
			stored.stored_call_plan = {};
		}
		if (planned) {
			if (auto* module = cached.FindModule<true>(runtime, push_data_cursor, resources)) {
				return bind(*module);
			}
		} else {
			// No stored module matches the callees: translate and plan again below.
			source_changed = true;
		}
	} else if (entry != m_program_cache.end()) {
		auto& cached = entry->second;
		if (cached.skip_dispatch) return nullptr;
		ShaderModule* module = nullptr;
		if (cached.call_source) {
			source_changed = cached.RefreshCallSource(runtime);
			if (!source_changed) module = cached.FindModule<true>(runtime, push_data_cursor, resources);
		} else {
			module = cached.FindModule<false>(runtime, push_data_cursor, resources);
			// Playtest fault injection (KYTY_PLAYTEST_INJECT=miss:...): compile this graphics shader
			// again.
			if (!std::is_same_v<InputInfo, ShaderComputeInputInfo> && module != nullptr &&
			    Common::Playtest::Enabled() && Common::Playtest::TakeInjectedMiss()) {
				module          = nullptr;
				m_injected_miss = true;
			}
		}
		if (module != nullptr) return bind(*module);
	}

	// A program-cache miss (the shader store warms the cache at start, so this compiles in play):
	// translation and compilation together, timed for the playtest recorder.
	struct PlaytestMiss {
		uint64_t    start    = Common::Playtest::BeginShaderMiss(); // 0 when off
		uint64_t    hash     = 0;
		const char* stage    = nullptr;
		bool        injected = false;
		bool*       clear    = nullptr; // an injected miss ends here even if nothing compiled
		~PlaytestMiss() {
			Common::Playtest::EndShaderMiss(start, hash, stage, injected);
			*clear = false;
		}
	} playtest_miss {.hash     = params.hash,
	                 .stage    = ShaderRecompiler::StageName(options.stage),
	                 .injected = m_injected_miss,
	                 .clear    = &m_injected_miss};
	options.shader_hash     = params.hash;
	options.user_data       = user_data;
	options.back_code       = params.back_code;
	options.dump_ir         = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
	options.emit_debug_info = Config::GraphicsDebugDumpEnabled();
	ShaderRecompiler::ShaderSource source;
	if (entry == m_program_cache.end()) {
		DumpShader(params.code, fmt::format("original/{}_{:016x}.bin",
		                                    ShaderRecompiler::StageName(options.stage), params.hash));
		source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		if (source.unsupported_call_pc != UINT32_MAX) {
			PipelineCacheLog("!!! TEMPORARY COMPUTE DISPATCH SKIP !!! shader=0x{:016x} pc=0x{:08x} "
			                 "reason=unsupported GPU-selected callable target; rendering is incomplete",
			                 params.hash, source.unsupported_call_pc);
			m_program_cache.emplace(lookup_key, Program {.skip_dispatch = true});
			return nullptr;
		}
	}
	auto* current = entry == m_program_cache.end() ? &source : entry->second.call_source.get();
	ShaderRecompiler::TranslateResult translated;
	if (current != nullptr) {
		if (!current->calls.empty()) {
			if (entry == m_program_cache.end()) ShaderRecompiler::RefreshShaderSource(*current, runtime);
			translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
			translated.program.source_reads = current->reads;
		} else {
			translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
		}
	} else {
		translated = ShaderRecompiler::TranslateProgram(params.code, options);
	}
	if (entry == m_program_cache.end()) {
		auto retained = !source.calls.empty()
		                    ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source))
		                    : nullptr;
		entry = m_program_cache.emplace(lookup_key, Program {
		    .call_source   = std::move(retained),
		    .resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program),
		}).first;
		auto& program = entry->second;
		EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
		    program.resource_plan, runtime, resources.snapshot, resources.specialization));
	} else if (source_changed) {
		entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
		if (const auto* module =
		        entry->second.FindModule<true>(runtime, push_data_cursor, resources)) {
			return bind(*module);
		}
	}
	return bind(CompileModule(entry->second, entry->first, options, std::move(translated),
	                          push_data_cursor, resources.specialization));
}

PipelineCache::PipelineCache(GraphicContext&                         graphics,
                             ShaderRecompiler::IR::ImageHeapSource* image_heap,
                             const std::filesystem::path&           shader_store)
    : m_graphics(graphics), m_image_heap(image_heap),
      m_next_driver_cache_save(std::chrono::steady_clock::now() + DriverCacheSaveInterval) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	// The title's own store is split by device and switches; a path given explicitly (tests) is
	// used as is.
	const bool title_store = !shader_store.empty() && shader_store == ShaderStorePath();
	WarmUp(title_store ? VariantStorePath(m_graphics, shader_store) : shader_store);
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& pipeline: pipelines | std::views::values) {
			m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			m_graphics.device.destroyShaderModule(module.module, nullptr);
		}
	}
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {:.2f} MB from {}",
		                 static_cast<double>(initial_data.size()) / 1'000'000.0, path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::CheckpointDriverCache() {
	if (m_driver_cache == nullptr) {
		return;
	}
	m_driver_cache_dirty = true;
	if (const auto now = std::chrono::steady_clock::now(); now >= m_next_driver_cache_save) {
		m_driver_cache_dirty     = false;
		m_next_driver_cache_save = now + DriverCacheSaveInterval;
		// The cache object is internally synchronized, so pipelines may be created meanwhile.
		m_store.Post([this] { WriteDriverCache(); });
	}
}

void PipelineCache::Save() {
	m_store.Finish();
	if (m_driver_cache_dirty) {
		m_driver_cache_dirty = false;
		WriteDriverCache();
	}
}

void PipelineCache::WriteDriverCache() const {
	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {:.2f} MB)",
		                 vk::to_string(result), static_cast<double>(size) / 1'000'000.0);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	const bool written = prefix_written == prefix.size() && payload_written == payload.size() && flushed;
	std::error_code error;
	if (written) {
		std::filesystem::rename(temp_path, m_driver_cache_path, error);
	}
	if (!written || error) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {:.2f} MB to {}",
	                 static_cast<double>(payload.size()) / 1'000'000.0,
	                 Common::PathToString(m_driver_cache_path));
}

void PipelineCache::GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state) {
	GetGraphicsPrograms(command.GetRegisters(), command.GetUserConfig(), command.GetShaders(),
	                    state, m_graphics_resources);
}

PipelineCache::PixelOutput PipelineCache::PixelOutputs(const HW::Context& context,
                                                       const HW::Shader&  shaders) {
	const auto& sh                  = context.GetShaderRegisters();
	const bool  object_primitive_id = context.HasObjectPrimitiveIdOutput();
	const auto  write_mask          = context.GetRenderTargetMask() & sh.m_cbShaderMask;
	uint32_t    color_output_mask   = object_primitive_id ? 1u << 7u : 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh.target_output_mode[index] != 0 && render_target_mask_slot(write_mask, slot) != 0) {
			color_output_mask |= 1u << slot;
		}
	}
	const auto& db = sh.db_shader_control;
	// Without color outputs, a pixel shader still runs for its depth or coverage side effects.
	const bool pixel_active =
	    shaders.GetPs().ps_regs.data_addr != 0 &&
	    (color_output_mask != 0 || db.shader_kill_enable || db.shader_z_export_enable ||
	     db.shader_mask_export_enable || db.shader_dual_export_enable ||
	     db.shader_execute_on_noop || sh.AlphaToMask());
	return {color_output_mask, pixel_active};
}

void PipelineCache::GetGraphicsPrograms(const HW::Context& context,
                                        const HW::UserConfig& user_config,
                                        const HW::Shader& shaders, DrawRenderState& state,
                                        GraphicsResources& resources) {
	const bool  worker            = DrawPrepare::OnWorker();
	const auto& sh                = context.GetShaderRegisters();
	const auto& vertex_regs       = shaders.GetVs();
	const auto& pixel_regs        = shaders.GetPs();
	auto&       vertex_info       = state.vertex_info;
	auto&       pixel_info        = state.ps_input_info;
	const bool object_primitive_id = context.HasObjectPrimitiveIdOutput();
	const auto [color_output_mask, pixel_active] = PixelOutputs(context, shaders);
	state.ps_active = pixel_active;
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	state.vertex_stage_count = tess_active ? 3u : 1u;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context,
		                                            std::span<ShaderVertexInputInfo, 3> {vertex_info.data(), 3});
		// Reflection decodes both programs; the key also covers the VGT fields in tess.
		const uint64_t layout_key[] = {vertex_params[0].hash, vertex_params[1].hash,
		                               uint64_t {sh.m_vgtLsHsConfig & 0xfff00u} << 32u |
		                                   sh.m_vgtTfParam};
		const auto layout_hash = XXH3_64bits(layout_key, sizeof(layout_key));
		if (worker && !m_tess_layouts.contains(layout_hash)) {
			DrawPrepare::Fail();
			return;
		}
		const auto [layout, inserted] =
		    m_tess_layouts.try_emplace(layout_hash, vertex_info[0].tess);
		if (inserted) {
			ShaderRecompiler::AnalyzeTessellationPrograms(vertex_params[0].code,
			                                              vertex_params[1].code, layout->second);
			StoreLayout(layout->first, layout->second);
		}
		for (uint32_t i = 0; i < 3; i++) {
			vertex_info[i].tess = layout->second;
		}
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (!tess_active && !mesh_active) {
		vertex_info[0].indirect_offsets = state.indirect_offsets;
		vertex_info[0].indirect_draw    = state.indirect_draw;
	}
	if (object_primitive_id) {
		// PA_CL_OBJPRIM_ID_CNTL = 2: VGT_OBJECT_ID plus the pipelined 32-bit primitive ID.
		EXIT_NOT_IMPLEMENTED(sh.m_paClObjprimIdCntl != 2u || mesh_active || tess_active ||
		                     !pixel_active || (sh.m_vgtDrawPayloadCntl & 8u) == 0 ||
		                     user_config.GetPrimType() != Prospero::PrimitiveType::kTriList);
		const auto& target = context.GetRenderTarget(7);
		EXIT_NOT_IMPLEMENTED(Prospero::ResolveRenderTargetFormat(target.info.format,
		                                                       target.info.channel_type)
		                         .buffer_format != Prospero::BufferFormat::k32UInt);
	}
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: dropping mesh draws the host does not support (shader 0x{:016x}).\n",
				    vertex_params[0].hash));
			}
			return;
		}
		// A task shader launches each record's groups and passes its draw parameters in a
		// payload beside the LDS, the allocation pair and the Layer array; without room for
		// it, the CPU reads the records.
		if (state.indirect_draw &&
		    (mesh.lds_size_dwords + 2u + mesh.max_vertices +
		     ShaderRecompiler::IR::PushData::MeshTaskPayloadDwordCount) * sizeof(uint32_t) <=
		        limits.maxMeshPayloadAndSharedMemorySize) {
			vertex_info[0].indirect_offsets = state.indirect_offsets;
			vertex_info[0].indirect_draw    = true;
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
		    target_export_mapping {};
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			const auto& rt = context.GetRenderTarget(slot);
			if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
				target_export_mapping[slot] =
				    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
				                                 rt.info.channel_order)
				        .export_mapping;
			}
		}
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		pixel_info.object_primitive_id = object_primitive_id;
		for (uint32_t export_index = 0; export_index < 8u; export_index++) {
			const auto slot = ShaderPixelExportTarget(pixel_info.target_shader_mask, export_index);
			if (slot >= 8u || pixel_info.target_output_mode[export_index] == 0u) continue;
			const auto& rt = context.GetRenderTarget(slot);
			const auto target_mask = render_target_mask_slot(context.GetRenderTargetMask(), slot);
			if (rt.base.addr == 0 || target_mask == 0u) continue;
			if (rt.info.channel_type == Prospero::ChannelType::kUInt) {
				pixel_info.target_uint_mask |= 1u << export_index;
			} else if (rt.info.channel_type == Prospero::ChannelType::kSInt) {
				pixel_info.target_sint_mask |= 1u << export_index;
			}
			const auto format = Prospero::ResolveRenderTargetFormat(rt.info.format, rt.info.channel_type);
			if (format.buffer_format != Prospero::BufferFormat::k11_11_10UNorm) continue;
			const auto mapping = pixel_info.target_export_mapping[export_index];
			const auto shader_mask = render_target_mask_slot(pixel_info.target_shader_mask, slot);
			const bool blend = context.GetBlendControl(slot).enable && !rt.info.blend_bypass;
			if (blend || (mapping.ApplyMask(target_mask) & 7u) != 7u ||
			    (mapping.ApplyMask(shader_mask) & 7u) != 7u ||
			    pixel_info.target_output_mode[export_index] != 9u) {
				EXIT("unsupported packed UNorm render-target export: slot=%u export=%u mode=%u "
				     "target_mask=0x%x shader_mask=0x%x mapping=0x%02x blend=%u\n",
				     slot, export_index, pixel_info.target_output_mode[export_index], target_mask,
				     shader_mask, mapping.packed, static_cast<uint32_t>(blend));
			}
			pixel_info.target_packed_unorm_mask |= 1u << export_index;
			pixel_info.target_round_mode |= static_cast<uint8_t>(rt.info.round_mode) << export_index;
		}
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0; blended targets are not integer.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
			pixel_info.target_uint_mask &= ~2u;
			pixel_info.target_sint_mask &= ~2u;
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	auto&       last_stage   = vertex_info[tess_active ? 2u : 0u];
	const auto& clip_control = context.GetClipControl();
	if (clip_control.clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = last_stage.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	} else if (clip_control.user_clip_planes != 0) {
		// CLIP_DISABLE also disables user planes. How they combine with shader clip and cull
		// distances is unverified; draw push constants only reach VS/mesh and PS, and the
		// generated RECTLIST stages do not forward clip distances.
		EXIT_NOT_IMPLEMENTED((last_stage.pa_cl_vs_out_cntl & 0xffffu) != 0 || tess_active ||
		                     Prospero::IsRectList(user_config.GetPrimType()));
		last_stage.user_clip_planes = clip_control.user_clip_planes;
	}
	uint32_t push_data_cursor = ShaderRecompiler::IR::PushData::GraphicsDrawDwords(
	    mesh_active, object_primitive_id, last_stage.user_clip_planes);
	if (pixel_active) {
		const auto* found = GetProgram(pixel_params, pixel_info, push_data_cursor, resources[3]);
		if (found == nullptr) {
			// A skipped pixel program is unsupported; the worker fails there or earlier.
			EXIT_IF(!worker);
			DrawPrepare::Fail();
			return;
		}
		const auto& pixel_module = *found;
		for (const auto& output: pixel_module.info.info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
			    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
				state.mrt_mask |= 1u << output.location;
			}
		}
		state.mrt_mask &= color_output_mask;
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		GetProgram(vertex_params[i], vertex_info[i], push_data_cursor, resources[i]);
	}
}

const ShaderModule* PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                                     ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	return GetProgram(params, input_info, push_data_cursor, m_compute_resources);
}

bool GraphicsPipelineTargets::Same(const GraphicsPipelineTargets& other) const {
	return color_count == other.color_count && depth_format == other.depth_format &&
	       depth_samples == other.depth_samples && topology == other.topology &&
	       primitive_restart_enable == other.primitive_restart_enable &&
	       std::memcmp(colors.data(), other.colors.data(), color_count * sizeof(colors[0])) == 0;
}

GraphicsPipelineTargets PipelineCache::FinalTargets(const DrawRenderState& state) {
	GraphicsPipelineTargets targets;
	const auto*             colors = state.color_info.data();
	targets.color_count            = state.color_count;
	EXIT_IF(state.color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	for (uint32_t i = 0; i < state.color_count; i++) {
		const auto& color = colors[i];
		EXIT_IF(color.target_slot >= RENDER_COLOR_ATTACHMENTS_MAX || !color.image_id ||
		        color.desc.view_info.format == vk::Format::eUndefined);
		targets.colors[i] = {
		    .format         = color.desc.view_info.format,
		    .samples        = color.desc.info.samples,
		    .slot           = static_cast<uint8_t>(color.target_slot),
		    .export_mapping = color.export_mapping.packed,
		    .unorm_11_11_10 = static_cast<uint8_t>(color.desc.info.guest_format ==
		                                           Prospero::BufferFormat::k11_11_10UNorm),
		};
	}
	const auto& depth = state.depth_info;
	if (depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id)) {
		targets.depth_format  = depth.desc.view_info.format;
		targets.depth_samples = depth.desc.info.samples;
	}
	targets.topology                 = state.topology;
	targets.primitive_restart_enable = state.primitive_restart_enable ? 1u : 0u;
	return targets;
}

bool PipelineCache::WorkerPipelineKeysEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_WORKER_PIPELINE_KEY");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

namespace {

bool WorkerPipelineKeyVerify() {
	static const bool verify = [] {
		const char* value = std::getenv("KYTY_WORKER_PIPELINE_KEY_VERIFY");
		return value != nullptr && value[0] != '0';
	}();
	return verify;
}

bool WorkerPipelineKeyStats() {
	static const bool stats = [] {
		const char* value = std::getenv("KYTY_WORKER_PIPELINE_KEY");
		return value != nullptr && std::strcmp(value, "stats") == 0;
	}();
	return stats;
}

// ResolvePolygonMode without its exits: nothing where it would exit.
std::optional<vk::PolygonMode> TryResolvePolygonMode(const HW::ModeControl& mode, bool cull_front,
                                                     bool cull_back) {
	if (mode.poly_mode == 0 || (mode.poly_mode == 1 && cull_front && cull_back)) {
		return vk::PolygonMode::eFill;
	}
	if (mode.poly_mode != 1 ||
	    (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype)) {
		return std::nullopt;
	}
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	if (polygon_mode > 2) {
		return std::nullopt;
	}
	return ResolvePolygonMode(mode, cull_front, cull_back);
}

// The first field where two keys differ, by name; empty if they are equal.
std::string FirstKeyDifference(const GraphicsPipelineKey& a, const GraphicsPipelineKey& b) {
	std::string diff;
	const auto  field = [&](const char* name, int index, const auto& x, const auto& y) {
        if (diff.empty() && std::memcmp(&x, &y, sizeof(x)) != 0) {
            diff = index < 0 ? std::string(name) : fmt::format("{}[{}]", name, index);
        }
	};
	for (int i = 0; i < 4; i++) {
		field("stage_ids", i, a.stage_ids[i], b.stage_ids[i]);
	}
	const auto& ra = a.rendering;
	const auto& rb = b.rendering;
	for (int i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		field("rendering.color_formats", i, ra.color_formats[i], rb.color_formats[i]);
	}
	field("rendering.depth_format", -1, ra.depth_format, rb.depth_format);
	field("rendering.stencil_format", -1, ra.stencil_format, rb.stencil_format);
	field("rendering.color_count", -1, ra.color_count, rb.color_count);
	const auto& va = a.vertex_input;
	const auto& vb = b.vertex_input;
	field("vertex_input.binding_count", -1, va.binding_count, vb.binding_count);
	field("vertex_input.attribute_count", -1, va.attribute_count, vb.attribute_count);
	for (int i = 0; i < ShaderVertexInputInfo::RES_MAX; i++) {
		field("vertex_input.bindings.stride", i, va.bindings[i].stride, vb.bindings[i].stride);
		field("vertex_input.bindings.instance", i, va.bindings[i].instance, vb.bindings[i].instance);
		field("vertex_input.attributes.offset", i, va.attributes[i].offset, vb.attributes[i].offset);
		field("vertex_input.attributes.binding", i, va.attributes[i].binding,
		      vb.attributes[i].binding);
	}
	const auto& sa = a.static_params;
	const auto& sb = b.static_params;
#define KYTY_KEY_FIELD(name) field("static_params." #name, -1, sa.name, sb.name)
#define KYTY_KEY_SLOTS(name)                                                                       \
	for (int i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {                                       \
		field("static_params." #name, i, sa.name[i], sb.name[i]);                                  \
	}
	KYTY_KEY_FIELD(negative_one_to_one);
	KYTY_KEY_FIELD(depth_clip_enable);
	KYTY_KEY_FIELD(topology);
	KYTY_KEY_FIELD(patch_control_points);
	KYTY_KEY_FIELD(primitive_restart_enable);
	KYTY_KEY_FIELD(samples);
	KYTY_KEY_FIELD(sample_shading_enable);
	KYTY_KEY_FIELD(alpha_to_coverage_enable);
	KYTY_KEY_SLOTS(color_mask);
	KYTY_KEY_FIELD(provoking_vtx_last);
	KYTY_KEY_FIELD(polygon_mode);
	KYTY_KEY_SLOTS(color_srcblend);
	KYTY_KEY_SLOTS(color_comb_fcn);
	KYTY_KEY_SLOTS(color_destblend);
	KYTY_KEY_SLOTS(alpha_srcblend);
	KYTY_KEY_SLOTS(alpha_comb_fcn);
	KYTY_KEY_SLOTS(alpha_destblend);
	KYTY_KEY_SLOTS(separate_alpha_blend);
	KYTY_KEY_SLOTS(blend_enable);
#undef KYTY_KEY_SLOTS
#undef KYTY_KEY_FIELD
	if (diff.empty() && !(a == b)) {
		diff = "padding";
	}
	return diff;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool PipelineCache::BuildGraphicsPipelineKey(const HW::Context&             ctx,
                                             const HW::UserConfig&          user_config,
                                             const DrawRenderState&         state,
                                             const GraphicsPipelineTargets& targets, bool worker,
                                             GraphicsPipelineKey& key) const {
	const auto  vertex_info   = state.VertexStages();
	const auto& ps_input_info = state.ps_input_info;
	const auto& vs_input_info = vertex_info.front();

	const bool ps_active    = state.ps_active;
	const bool tessellation = vertex_info.size() == 3;
	for (uint32_t i = 0; i < vertex_info.size(); i++) {
		if (const auto* module = vertex_info[i].stage.module; module != nullptr) {
			key.stage_ids[i] = module->id;
		}
	}
	if (ps_active && ps_input_info.stage.module != nullptr) {
		key.stage_ids[3] = ps_input_info.stage.module->id;
	}
	const bool mesh = vertex_info[0].stage.module->info.stage == ShaderType::Mesh;

	const HW::ModeControl& mc = ctx.GetModeControl();

	auto&    static_params      = key.static_params;
	auto&    rendering          = key.rendering;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < targets.color_count; i++) {
		const auto& color = targets.colors[i];
		const auto  slot  = static_cast<uint32_t>(color.slot);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX || color.format == vk::Format::eUndefined) {
			if (worker) {
				return false;
			}
			EXIT("invalid color attachment: slot %u format %u\n", slot,
			     static_cast<uint32_t>(color.format));
		}
		const Prospero::ColorComponentMapping export_mapping {.packed = color.export_mapping};
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		static_params.color_mask[slot] =
		    export_mapping.ApplyMask(render_target_mask_slot(ctx.GetRenderTargetMask(), slot));
		if (color.unorm_11_11_10 != 0) {
			static_params.color_mask[slot] = 1u;
		}
		rendering.color_formats[slot] = color.format;
		if (attachment_samples == 0) {
			attachment_samples = color.samples;
		} else if (attachment_samples != color.samples) {
			if (worker) {
				return false;
			}
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, color.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(slot);
		const auto& bc                        = ctx.GetBlendControl(slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_active) {
			alpha_source = ps_input_info.alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
		}
	}
	const bool with_depth = targets.depth_format != vk::Format::eUndefined;
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(targets.depth_format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? targets.depth_format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? targets.depth_format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = targets.depth_samples;
		} else if (attachment_samples != targets.depth_samples) {
			if (worker) {
				return false;
			}
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     targets.depth_samples);
		}
	}
	if (targets.color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		const bool supported = static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples));
		if (worker && !supported) {
			return false;
		}
		EXIT_IF(!supported);
	}
	if (attachment_samples == 0 ||
	    vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {}) {
		if (worker) {
			return false;
		}
		EXIT("unsupported attachment sample count: %u\n", attachment_samples);
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = targets.topology;
	static_params.primitive_restart_enable = targets.primitive_restart_enable != 0;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info.ps_sample_shading;
	static_params.alpha_to_coverage_enable = ps_active && ctx.GetShaderRegisters().AlphaToMask();
	const bool rect_list = Prospero::IsRectList(user_config.GetPrimType());
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	const bool cull_front            = !rect_list && mc.cull_front;
	const bool cull_back             = !rect_list && mc.cull_back;
	if (worker) {
		const auto polygon_mode = TryResolvePolygonMode(mc, cull_front, cull_back);
		if (!polygon_mode) {
			return false;
		}
		static_params.polygon_mode = *polygon_mode;
	} else {
		static_params.polygon_mode = ResolvePolygonMode(mc, cull_front, cull_back);
	}
	if (tessellation) {
		static_params.patch_control_points = vs_input_info.tess.input_control_points;
	} else if (!mesh && static_params.topology == vk::PrimitiveTopology::ePatchList) {
		// Rectangle lists expand through generated tessellation stages.
		static_params.patch_control_points = 3;
	}

	if (!mesh) {
		const bool counts_valid = vs_input_info.buffers_num >= 0 &&
		                          vs_input_info.buffers_num <= ShaderVertexInputInfo::RES_MAX &&
		                          vs_input_info.resources_num >= 0 &&
		                          vs_input_info.resources_num <= ShaderVertexInputInfo::RES_MAX;
		if (worker && !counts_valid) {
			return false;
		}
		EXIT_IF(!counts_valid);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			const bool valid   = binding >= 0 && binding < vs_input_info.buffers_num;
			if (worker && !valid) {
				return false;
			}
			EXIT_IF(!valid);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}
	return true;
}

void PipelineCache::ProposeGraphicsPipelineKey(const HW::Context&             registers,
                                               const HW::UserConfig&          user_config,
                                               const DrawRenderState&         state,
                                               const GraphicsPipelineTargets& targets,
                                               WorkerPipelineKey&             proposal) const {
	proposal.ready   = false;
	proposal.targets = targets;
	// Value-initialized: keys are compared and hashed as bytes.
	std::construct_at(&proposal.key);
	if (!BuildGraphicsPipelineKey(registers, user_config, state, targets, true, proposal.key)) {
		return;
	}
	proposal.hash  = GraphicsPipelineKeyHash {}(proposal.key);
	proposal.ready = true;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(const CommandBuffer&   command,
                                                            const DrawRenderState& state) {
	return GetGraphicsPipeline(command, state, nullptr);
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(const CommandBuffer&     command,
                                                            const DrawRenderState&   state,
                                                            const WorkerPipelineKey* proposal) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);
	const auto targets = FinalTargets(state);
	auto&      stats   = m_worker_key_stats;
	stats.draws++;
	if (proposal != nullptr && proposal->ready && proposal->targets.Same(targets)) {
		stats.adopted++;
		if (WorkerPipelineKeyVerify()) {
			GraphicsPipelineKey check {};
			BuildGraphicsPipelineKey(command.GetRegisters(), command.GetUserConfig(), state,
			                         targets, false, check);
			auto diff = FirstKeyDifference(proposal->key, check);
			if (diff.empty() && proposal->hash != GraphicsPipelineKeyHash {}(check)) {
				diff = "hash";
			}
			if (!diff.empty()) {
				EXIT("KYTY_WORKER_PIPELINE_KEY_VERIFY: the worker's pipeline key differs in %s\n",
				     diff.c_str());
			}
			if (++stats.verified % 1000000 == 0) {
				std::printf("WORKER_PIPELINE_KEY_VERIFY checks=%" PRIu64 "\n", stats.verified);
				std::fflush(stdout);
			}
		}
		PrintWorkerKeyStats();
		return FindGraphicsPipeline(proposal->key, proposal->hash, state);
	}
	if (proposal == nullptr || !proposal->ready) {
		stats.none++;
	} else {
		stats.mismatched++;
	}
	PrintWorkerKeyStats();
	GraphicsPipelineKey key {};
	BuildGraphicsPipelineKey(command.GetRegisters(), command.GetUserConfig(), state, targets, false,
	                         key);
	return FindGraphicsPipeline(key, 0, state);
}

void PipelineCache::PrintWorkerKeyStats() {
	if (!WorkerPipelineKeyStats()) {
		return;
	}
	auto&      stats = m_worker_key_stats;
	const auto now   = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
	if (stats.start_ns == 0) {
		stats.start_ns = now;
	} else if (now - stats.start_ns >= 1'000'000'000ull) {
		std::printf("WORKERPIPELINEKEY draws=%" PRIu64 " adopted=%" PRIu64 " none=%" PRIu64
		            " mismatched=%" PRIu64 "\n",
		            stats.draws, stats.adopted, stats.none, stats.mismatched);
		const auto verified = stats.verified;
		stats               = {};
		stats.start_ns      = now;
		stats.verified      = verified;
	}
}

// Equality of two keys built by BuildGraphicsPipelineKey, whose vertex bindings and attributes
// past their counts stay zero: ~200 of the key's 516 bytes instead of all of them.
static bool SameBuiltKey(const GraphicsPipelineKey& a, const GraphicsPipelineKey& b) {
	const auto& va = a.vertex_input;
	const auto& vb = b.vertex_input;
	return std::memcmp(&a, &b, offsetof(GraphicsPipelineKey, vertex_input)) == 0 &&
	       va.binding_count == vb.binding_count && va.attribute_count == vb.attribute_count &&
	       std::memcmp(va.bindings.data(), vb.bindings.data(),
	                   va.binding_count * sizeof(va.bindings[0])) == 0 &&
	       std::memcmp(va.attributes.data(), vb.attributes.data(),
	                   va.attribute_count * sizeof(va.attributes[0])) == 0 &&
	       std::memcmp(&a.static_params, &b.static_params, sizeof(a.static_params)) == 0;
}

// hash: the key's, or 0 to compute it if needed (a hash of 0 is then computed again).
PipelineCache::Pipeline& PipelineCache::FindGraphicsPipeline(const GraphicsPipelineKey& key,
                                                             uint64_t                   hash,
                                                             const DrawRenderState&     state) {
	if (m_last_graphics_pipeline != nullptr && (hash == 0 || hash == m_last_graphics_hash) &&
	    SameBuiltKey(key, *m_last_graphics_key)) {
		return *m_last_graphics_pipeline;
	}
	if (hash == 0) {
		hash = GraphicsPipelineKeyHash {}(key);
	}
	auto iter = m_graphics_pipelines.find(HashedGraphicsPipelineKey {key, hash});
	if (iter != m_graphics_pipelines.end()) {
		m_last_graphics_key      = &iter->first;
		m_last_graphics_pipeline = &iter->second;
		m_last_graphics_hash     = hash;
		return iter->second;
	}
	iter                     = m_graphics_pipelines.try_emplace(key).first;
	auto& pipeline           = iter->second;
	m_last_graphics_key      = &iter->first;
	m_last_graphics_pipeline = &pipeline;
	m_last_graphics_hash     = hash;

	const auto  vertex_info   = state.VertexStages();
	const auto& ps_input_info = state.ps_input_info;
	const bool  ps_active     = state.ps_active;
	std::array<const ShaderModule*, 4> stages {};
	for (uint32_t i = 0; i < vertex_info.size(); i++) {
		stages[i] = vertex_info[i].stage.module;
	}
	if (ps_active) {
		stages[3] = ps_input_info.stage.module;
	}
	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vertex_info.front());
		if (ps_active) {
			ShaderDbgDumpInputInfo(ps_input_info);
		}
	}

	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		const uint64_t playtest_start = Common::Playtest::Enabled() ? Common::FrameDiag::NowNs() : 0;
		CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
		if (playtest_start != 0) {
			Common::Playtest::Event(Common::Playtest::Kind::PipelineCreate,
			                        Common::FrameDiag::NowNs() - playtest_start, 0);
		}
	}
	StorePipeline(key);
	CheckpointDriverCache();

	return pipeline;
}

PipelineCache::Pipeline& PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	const auto& module = *input_info.stage.module;

	auto [iter, inserted] = m_compute_pipelines.try_emplace(module.id);
	auto& pipeline        = iter->second;
	if (!inserted) {
		return pipeline;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		const uint64_t playtest_start = Common::Playtest::Enabled() ? Common::FrameDiag::NowNs() : 0;
		CreatePipelineInternal(m_graphics, pipeline, module, m_driver_cache);
		if (playtest_start != 0) {
			Common::Playtest::Event(Common::Playtest::Kind::PipelineCreate,
			                        Common::FrameDiag::NowNs() - playtest_start, 1);
		}
	}
	CheckpointDriverCache();

	return pipeline;
}
} // namespace Libs::Graphics
