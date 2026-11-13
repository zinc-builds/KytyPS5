#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/memDiag.h"
#include "shader_store/shaderStoreKey.h"

#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/vulkanDiagnostics.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"

#include <algorithm>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <ranges>
#include <xxhash.h>
#include <zstd.h>

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Bump to discard every existing store by hand (a change the source hash cannot see).
constexpr uint32_t kShaderStoreFormat = 2; // 2: module records carry their callee code

// KYTY_SHADER_CACHE_STAMP=N (N > 0) replaces the source hash with N: a manual key, to keep a store
// across a source change known not to alter what it records, or to discard one by changing N.
// Change N whenever the shader recompiler changes, or the stored programs no longer match the code.
uint64_t ManualStamp() {
	static const uint64_t stamp = [] {
		const char* env = std::getenv("KYTY_SHADER_CACHE_STAMP");
		if (env == nullptr || env[0] < '1' || env[0] > '9') {
			return uint64_t {0};
		}
		// A whole positive decimal number only; anything else is ignored.
		char*      end   = nullptr;
		errno            = 0;
		const auto value = std::strtoull(env, &end, 10);
		return errno == 0 && end != nullptr && *end == '\0' ? static_cast<uint64_t>(value) : uint64_t {0};
	}();
	return stamp;
}

// What every record depends on besides its own fields. A store is reused only when all of it
// matches, so a rebuild that leaves these alone keeps the store:
//  - KYTY_SHADER_STORE_SOURCE_HASH: the sources that turn guest code into the stored modules and
//    keys (CMakeLists.txt lists them: the recompiler, its IR and serialization, the guest format
//    and register definitions, the pipeline cache and this file), or KYTY_SHADER_CACHE_STAMP;
//  - sizeof(GraphicsPipelineKey): pipeline records are its raw bytes;
//  - the device and the switches that change generated code: the SPIR-V profile, the shared
//    memory limit, the host subgroup sizes, the buffer cache's page table geometry and the DMA
//    address translation constants, masked selects
//    and KYTY_BDA_STATS=access (instrumented DMA shaders).
// The store's file name carries a hash of all of it (KeyedStorePath), so every build and switch
// combination that would record different modules has its own file in the shared directory:
// turning KYTY_RESIDENT_TEXTURES on and off, or alternating two builds, keeps a warm store for
// each instead of one truncating the other's.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	Writer      header;
	const auto& properties = graphics.GetPhysicalDeviceProperties();
	const uint32_t bda_access     = MemDiag::BdaStatsMode() == 2 ? 1u : 0u;
	const uint32_t masked_selects = ShaderRecompiler::MaskedSelectsEnabled() ? 1u : 0u;
	const uint32_t wave64         = graphics.SupportsComputeWave64() ? 1u : 0u;
	const uint32_t manual         = ManualStamp() != 0 ? 1u : 0u;
	const uint64_t source_hash    = manual != 0 ? ManualStamp() : KYTY_SHADER_STORE_SOURCE_HASH;
	const uint64_t key_size       = sizeof(GraphicsPipelineKey);
	const uint32_t page_bits      = BufferCache::CACHING_PAGEBITS;
	const uint64_t page_count     = BufferCache::CACHING_NUMPAGES;
	// The DMA shaders' address translation (spirvEmitterMemory.cpp).
	const uint64_t extended_base  = LibKernel::Memory::kExtendedMemoryBase;
	const uint64_t lower_size     = LOWER_ADDRESS_SIZE;
	IoAll(header, kShaderStoreFormat, manual, source_hash, key_size, properties.vendorID,
	      properties.deviceID, SpirvProfile(graphics), properties.limits.maxComputeSharedMemorySize,
	      graphics.subgroup_size, wave64, page_bits, page_count, extended_base, lower_size,
	      masked_selects, bda_access);
	return std::move(header.bytes);
}

// Record fields, shared by writing and reading.
template <class S, class Key> void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <class S, class Module> void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V. function_code is the callee code a program with
	// GPU-selected calls was linked against (empty without calls).
	auto& [id, handle, info, specialization, function_code, vertex_formats, rect_parameters,
	       mesh_task, stored_call_plan] = stage;
	IoAll(io, id, info, specialization, function_code, vertex_formats, rect_parameters, mesh_task);
}

} // namespace

std::filesystem::path ShaderStoreDir() {
	const auto env = [](const char* name) -> const char* {
		const char* value = std::getenv(name);
		return value != nullptr && value[0] != '\0' ? value : nullptr;
	};
	if (const char* dir = env("KYTY_SHADER_STORE_DIR")) {
		return dir;
	}
#if defined(_WIN32)
	if (const char* local = env("LOCALAPPDATA")) {
		return std::filesystem::path(local) / "kyty-shaders";
	}
#elif defined(__APPLE__)
	if (const char* home = env("HOME")) {
		return std::filesystem::path(home) / "Library" / "Caches" / "kyty-shaders";
	}
#else
	if (const char* xdg = env("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] == '/') {
		return std::filesystem::path(xdg) / "kyty-shaders";
	}
	if (const char* home = env("HOME")) {
		return std::filesystem::path(home) / ".cache" / "kyty-shaders";
	}
#endif
	return InstallStoreDir();
}

std::filesystem::path KeyedStorePath(const GraphicContext& graphics, const std::filesystem::path& base) {
	const auto header = StoreHeader(graphics);
	auto       name   = base.stem();
	name += fmt::format(".{:016x}", XXH3_64bits(header.data(), header.size()));
	name += base.extension();
	return base.parent_path() / name;
}

std::filesystem::path AdoptStore(const std::filesystem::path& store, std::span<const uint8_t> header,
                                 std::span<const std::filesystem::path> candidates) {
	std::error_code error;
	if (std::filesystem::exists(store, error)) {
		return {};
	}
	for (const auto& candidate: candidates) {
		if (candidate.empty() || candidate == store) {
			continue;
		}
		std::vector<uint8_t> head(header.size());
		uint32_t             read = 0;
		if (Common::File file;
		    Common::File::IsFileExisting(candidate) && file.Open(candidate, Common::File::Mode::Read)) {
			file.Read(head.data(), static_cast<uint32_t>(head.size()), &read);
		}
		if (read != header.size() || !std::ranges::equal(head, header)) {
			continue;
		}
		// A temporary name unique to this process, then a rename: a store appears whole or not.
		auto temp = store;
		temp += fmt::format(".{:x}.adopt", std::chrono::steady_clock::now().time_since_epoch().count());
		Common::File::CreateDirectories(store.parent_path());
		std::filesystem::copy_file(candidate, temp, std::filesystem::copy_options::overwrite_existing,
		                           error);
		if (!error) {
			std::filesystem::rename(temp, store, error);
		}
		if (error) {
			std::filesystem::remove(temp, error);
			continue;
		}
		return candidate;
	}
	return {};
}

namespace {
#if !defined(_WIN32)
// The writer lock of `store`, or -1 when another process holds it (or it cannot be made).
int TryLockStore(const std::filesystem::path& store) {
	auto lock_path = store;
	lock_path += ".lock";
	const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		return -1;
	}
	if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}
#endif
} // namespace

size_t PruneStores(const std::filesystem::path& current, size_t keep, std::chrono::hours max_age) {
	const auto dir    = current.parent_path().empty() ? std::filesystem::path(".") : current.parent_path();
	const auto name   = current.filename().string();
	const auto prefix = name.substr(0, name.find('.') + 1); // "<title>."
	const auto ext    = current.extension().string();
	if (prefix.size() < 2 || ext.empty()) {
		return 0;
	}
	std::error_code error;
	std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> stores;
	for (const auto& entry: std::filesystem::directory_iterator(dir, error)) {
		const auto file = entry.path().filename().string();
		if (entry.is_regular_file(error) && file.starts_with(prefix) && file.ends_with(ext) &&
		    file != name) {
			stores.emplace_back(entry.last_write_time(error), entry.path());
		}
	}
	std::ranges::sort(stores, std::greater {});
	const auto now     = std::filesystem::file_time_type::clock::now();
	size_t     removed = 0;
	// `current` counts against `keep`.
	for (size_t i = 0; i < stores.size(); i++) {
		const auto& [time, path] = stores[i];
		if (i + 1 < keep && now - time < max_age) {
			continue;
		}
		auto lock_path = path;
		lock_path += ".lock";
#if !defined(_WIN32)
		const int fd = TryLockStore(path);
		if (fd < 0) {
			continue; // in use
		}
#endif
		if (std::filesystem::remove(path, error)) {
			removed++;
		}
		std::filesystem::remove(lock_path, error);
#if !defined(_WIN32)
		::close(fd);
#endif
	}
	return removed;
}

ShaderStoreWriter::WriterLock::~WriterLock() {
#if !defined(_WIN32)
	if (fd >= 0) {
		::close(fd);
	}
#endif
}

bool ShaderStoreWriter::Lock(const std::filesystem::path& path) {
#if defined(_WIN32)
	(void)path;
	return true; // no lock: one emulator at a time is assumed
#else
	Common::File::CreateDirectories(path.parent_path());
	if (m_lock.fd < 0) {
		m_lock.fd = TryLockStore(path);
	}
	return m_lock.fd >= 0;
#endif
}

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path,
                           std::span<const std::filesystem::path> adopt) {
	if (path.empty()) {
		return;
	}
	const auto header = StoreHeader(m_graphics);
	const auto           start = std::chrono::steady_clock::now();
	// Before reading: no other emulator appends between the read and Open's truncation.
	const bool writer = m_store.Lock(path);
	if (writer) {
		if (const auto adopted = AdoptStore(path, header, adopt); !adopted.empty()) {
			Log::WriteToConsoleAndLog(fmt::format("Shader cache: adopted {} as {}\n",
			                                      Common::PathToString(adopted),
			                                      Common::PathToString(path)));
		}
	} else {
		Log::WriteToConsoleAndLog(fmt::format(
		    "Shader cache: {} is in use by another emulator; reading it, not adding to it\n",
		    Common::PathToString(path)));
	}
	std::vector<uint8_t> bytes;
	if (Common::File file;
	    Common::File::IsFileExisting(path) && file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: compiling shaders and pipelines from {} ({:.1f} MB) with {} threads\n",
	    Common::PathToString(path), static_cast<double>(bytes.size()) / 1'000'000.0,
	    Common::ParallelThreads()));
	auto       records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() && std::ranges::equal(records.first(header.size()), header);
	if (reusable) {
		records = records.subspan(header.size());
		while (LoadRecord(records)) {
		}
	}
	// A damaged record ends the store; the records before it stay.
	if (writer) {
		m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);
	}

	const auto modules = MapModules();
	std::vector<Common::UniqueFunction<void>> jobs;
	jobs.reserve(m_graphics_pipelines.size() + modules.size());
	for (auto& node: m_graphics_pipelines) {
		jobs.emplace_back([this, &modules, &node] {
			CreateGraphicsPipeline(node.second, node.first, modules);
		});
	}
	// A compute pipeline depends only on its module.
	for (const auto* module: modules | std::views::values) {
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			jobs.emplace_back([this, module, &pipeline = m_compute_pipelines[module->id]] {
				CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
			});
		}
	}
	Common::ParallelFor(jobs.size(), [&](size_t i) { jobs[i](); });
	// A key with a stage missing from the store keeps no pipeline.
	std::erase_if(m_graphics_pipelines,
	              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	if (!m_graphics_pipelines.empty() || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: loaded {} programs, {} modules, {} pipelines in {} ms\n",
	    m_program_cache.size(), modules.size() - 1,
	    m_graphics_pipelines.size() + m_compute_pipelines.size(),
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                          start)
	        .count()));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records) {
	Frame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	std::vector<uint8_t> raw(frame.raw_size);
	if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) != raw.size()) {
		return false;
	}
	Reader     reader {raw};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const bool calls = !module.function_code.empty();
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			if (calls != program->second.stored_calls && !inserted) {
				return false; // one program's records disagree on whether it has calls
			}
			if (calls) {
				// Its plan follows the callee code: PipelineCache::GetProgram picks the record that
				// matches the callees' current code after relinking.
				program->second.stored_calls = true;
				module.stored_call_plan      = std::move(plan);
			}
			VulkanDiagnostics::DumpSpirv(ShaderRecompiler::StageName(program->first.stage),
			                             program->first.hash, module.id, spirv);
			module.module = CompileSPV(spirv, m_graphics.device);
			program->second.modules.push_back(std::move(module));
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			// WarmUp creates the pipeline once every record is loaded.
			m_graphics_pipelines.try_emplace(key);
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	m_store.Append(std::move(record.bytes));
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
