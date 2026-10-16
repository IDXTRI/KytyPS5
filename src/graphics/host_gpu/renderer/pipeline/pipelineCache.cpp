#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/pipeline/programDiskCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"
#include "graphics/shader/recompiler/ir/passes/SrtNative.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <tuple>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

bool ShaderFailureNonFatal() {
	return true;
}

// A shader whose draws are skipped says so in release builds too (LOGF is silent there):
// Wolverine's suit and tree trunks vanished with KYTY_BINDLESS_SAMPLERS=1 without a word.
static void SkipReport(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::vprintf(format, args);
	va_end(args);
	std::fflush(stdout);
}

namespace {

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
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
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
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

// KYTY_PIPELINE_CACHE_ANY_REVISION=1 keeps a cache written by another emulator revision, so
// A/B builds of one title don't start cold. The device, driver and UUID must still match, and
// the driver validates every entry itself; entries for pipelines a build no longer creates are
// simply never looked up.
bool DriverCacheSignatureMatches(std::string_view cached, std::string_view current) {
	if (cached == current) {
		return true;
	}
	// On by default in this build (updates and the launcher keep the warm cache);
	// KYTY_PIPELINE_CACHE_ANY_REVISION=0 turns it off.
	static const bool any_revision = [] {
		const char* value = std::getenv("KYTY_PIPELINE_CACHE_ANY_REVISION");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	if (!any_revision) {
		return false;
	}
	// "KytyPC1:<revision>:<vendor>:..." -> ":<vendor>:..."
	const auto without_revision = [](std::string_view signature) -> std::string_view {
		const auto first = signature.find(':');
		const auto second =
		    first == std::string_view::npos ? first : signature.find(':', first + 1);
		return second == std::string_view::npos ? std::string_view {} : signature.substr(second);
	};
	const auto cached_rest = without_revision(cached);
	return !cached_rest.empty() && cached.starts_with("KytyPC1:") &&
	       cached_rest == without_revision(current);
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

bool UsesShaderClock(const ShaderRecompiler::IR::Program& program) {
	for (auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::ReadClockRealtime64) {
				return true;
			}
		}
	}
	return false;
}

// The resource walker reads guest memory one dword per scalar load, and every read pays the
// GPU-ownership checks and the address-space lock. KYTY_SHADER_READ_CHUNKS (a live switch):
// 1 reads each aligned 256-byte chunk once per program lookup and serves its dwords from that
// copy; 2 (default) looks up each 4 KiB page's backing bytes once per lookup (FindGpuCleanBacking,
// after Senaxx 820f72e5) and copies every later read of the page straight from them. A chunk or
// page with any GPU-owned byte, or one that is not all mapped, is refused and its reads take the
// per-read path, so the values are the ones read before. Nothing marks a page GPU-written during
// one synchronous lookup on the GPU thread; the verdicts live for one lookup only.
// The same object can log every read and its result (KYTY_RESOURCE_MEMO, ProgramCache).
class ShaderReadChunks {
public:
	struct LoggedRead {
		uint64_t address = 0;
		uint32_t first   = 0; // index of the first word in ReadLog::words
		uint32_t count   = 0;
		bool     ok      = false;
	};
	struct ReadLog {
		std::vector<LoggedRead> reads;
		std::vector<uint32_t>   words;
	};

	explicit ShaderReadChunks(int64_t mode): m_chunks(mode == 1), m_pages(mode == 2) {}

	// Returns false when the chunk cannot serve the read; the caller reads as before.
	bool Read(uint64_t address, std::span<uint32_t> values) {
		if (m_pages) {
			return ReadPage(address, values);
		}
		if (!m_chunks) {
			return false;
		}
		const auto base = address & ~(ChunkSize - 1);
		if (values.size_bytes() > ChunkSize || address + values.size_bytes() > base + ChunkSize) {
			return false;
		}
		auto* slot = Find(base);
		if (slot == nullptr) {
			slot        = &m_slots[m_next++ % m_slots.size()];
			slot->base  = base;
			slot->clean = Libs::LibKernel::Memory::TryReadGpuCleanBacking(base, slot->words.data(),
			                                                              ChunkSize);
		}
		if (!slot->clean) {
			return false;
		}
		std::memcpy(values.data(), slot->words.data() + (address - base) / sizeof(uint32_t),
		            values.size_bytes());
		return true;
	}

	void SetLog(ReadLog* log) { m_log = log; }

	void Log(uint64_t address, std::span<const uint32_t> values, bool ok) {
		if (m_log == nullptr) {
			return;
		}
		m_log->reads.push_back({.address = address,
		                        .first   = static_cast<uint32_t>(m_log->words.size()),
		                        .count   = static_cast<uint32_t>(values.size()),
		                        .ok      = ok});
		if (ok) {
			m_log->words.insert(m_log->words.end(), values.begin(), values.end());
		}
	}

	static int64_t Mode() {
		// Wolverine run 23: see PROGRESS.md for the 1 vs 2 A/B.
		static auto& mode = Common::LiveSwitches::Get("KYTY_SHADER_READ_CHUNKS", 2);
		return mode.load(std::memory_order_relaxed);
	}

private:
	static constexpr uint64_t ChunkSize = 256;

	struct Slot {
		uint64_t                                           base  = UINT64_MAX;
		bool                                               clean = false;
		std::array<uint32_t, ChunkSize / sizeof(uint32_t)> words;
	};

	struct Page {
		uint64_t       base    = UINT64_MAX;
		const uint8_t* backing = nullptr;
	};

	bool ReadPage(uint64_t address, std::span<uint32_t> values) {
		const auto base = Common::AlignDown(address, TRACKER_PAGE_SIZE);
		if (values.empty() ||
		    Common::AlignDown(address + values.size_bytes() - 1, TRACKER_PAGE_SIZE) != base) {
			return false;
		}
		const Page* page = nullptr;
		for (size_t i = 0; i < m_page_count; i++) {
			if (m_pages_cache[i].base == base) {
				page = &m_pages_cache[i];
				break;
			}
		}
		if (page == nullptr) {
			auto& slot = m_page_count < m_pages_cache.size()
			                 ? m_pages_cache[m_page_count++]
			                 : m_pages_cache[m_next_page++ % m_pages_cache.size()];
			slot       = {.base = base,
			              .backing =
			                  Libs::LibKernel::Memory::FindGpuCleanBacking(base, TRACKER_PAGE_SIZE)};
			page       = &slot;
		}
		if (page->backing == nullptr) {
			return false;
		}
		std::memcpy(values.data(), page->backing + (address - base), values.size_bytes());
		return true;
	}

	Slot* Find(uint64_t base) {
		for (auto& slot: m_slots) {
			if (slot.base == base) {
				return &slot;
			}
		}
		return nullptr;
	}

	bool                 m_chunks = false;
	bool                 m_pages  = false;
	ReadLog*             m_log    = nullptr;
	std::array<Slot, 8>  m_slots;
	size_t               m_next = 0;
	std::array<Page, 16> m_pages_cache;
	size_t               m_page_count = 0;
	size_t               m_next_page  = 0;
};

// Both SRT readers: only memory the guest has committed. Bytes the GPU has not written are
// current in the backing store; reading them there skips the tracked-page fault, which drains
// the GPU to refresh whatever else on the page the GPU wrote (constants that share a page with
// GPU-written arguments cost a drain per dispatch). Bytes that are mapped but GPU-owned are read
// through the guest mapping, so the fault refreshes them and the value is the one the shader
// would see. Without a reader the walker dereferenced whatever address a descriptor chain
// produced, including 0 on a path the shader never takes.
bool ReadShaderGuestMemoryImpl(ShaderReadChunks* chunks, uint64_t address,
                               std::span<uint32_t> values) {
	if (values.empty()) {
		return false;
	}
	if (chunks != nullptr && chunks->Read(address, values)) {
		return true;
	}
	if (Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, values.data(),
	                                                    values.size_bytes())) {
		return true;
	}
	if (!Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes())) {
		return false;
	}
	std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	return true;
}

bool ReadShaderGuestMemoryRaw(void* userdata, uint64_t address, std::span<uint32_t> values) {
	KYTY_PROFILER_BLOCK("ShaderGuestMemoryRead");
	auto*      chunks = static_cast<ShaderReadChunks*>(userdata);
	const bool ok     = ReadShaderGuestMemoryImpl(chunks, address, values);
	if (chunks != nullptr) {
		chunks->Log(address, values, ok);
	}
	return ok;
}

// Research: a subroutine's code for inlining, in 1 KiB steps while the guest has it committed.
std::vector<uint32_t> ReadShaderCode(uint64_t address) {
	constexpr size_t      Chunk = 256;
	constexpr size_t      Limit = 16384;
	std::vector<uint32_t> words;
	while (words.size() < Limit) {
		std::array<uint32_t, Chunk> chunk {};
		if (!ReadShaderGuestMemoryRaw(nullptr, address + words.size() * sizeof(uint32_t), chunk)) {
			break;
		}
		words.insert(words.end(), chunk.begin(), chunk.end());
	}
	return words;
}

bool ReadShaderGuestMemory(void* userdata, uint64_t address, std::span<uint32_t> values) {
	KYTY_PROFILER_BLOCK("ShaderGuestMemoryRead");
	auto*      chunks = static_cast<ShaderReadChunks*>(userdata);
	const bool ok     = ReadShaderGuestMemoryImpl(chunks, address, values);
	if (chunks != nullptr) {
		chunks->Log(address, values, ok);
	}
	return ok;
}

// Live shader bisection, for finding which shader draws a glitch while the game runs:
// KYTY_SHADER_LIST=N (live, default 0): write the distinct shaders used in the next N seconds,
// per stage with their draw counts, to <live file dir>/logs/shaders_<time>.txt, then reset.
// KYTY_LIVE_SKIP=1 (live, default 0): skip the draws and dispatches of the shaders listed in
// <live file dir>/skip_live.txt (hex hashes, any separators), re-read when the file changes.
class LiveShaderTool {
public:
	static LiveShaderTool& Instance() {
		static LiveShaderTool tool;
		return tool;
	}

	bool Skip(ShaderType stage, uint64_t hash) {
		static auto& list_seconds = Common::LiveSwitches::Get("KYTY_SHADER_LIST", 0);
		static auto& live_skip    = Common::LiveSwitches::Get("KYTY_LIVE_SKIP", 0);
		const auto   seconds      = list_seconds.load(std::memory_order_relaxed);
		const bool   skipping     = live_skip.load(std::memory_order_relaxed) != 0;
		if (seconds <= 0 && !m_listing && !skipping) {
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const auto       now = std::chrono::steady_clock::now();
		if (seconds > 0 || m_listing) {
			if (!m_listing) {
				m_listing = true;
				m_list_end = now + std::chrono::seconds(seconds);
				m_counts.clear();
			}
			if (now >= m_list_end) {
				WriteList();
				m_listing = false;
				list_seconds.store(0, std::memory_order_relaxed);
			} else {
				m_counts[{static_cast<uint32_t>(stage), hash}]++;
			}
		}
		if (!skipping) {
			return false;
		}
		if (now >= m_next_check) {
			m_next_check = now + std::chrono::milliseconds(500);
			Reload();
		}
		return m_skipped.contains(hash);
	}

private:
	static std::string Directory() {
		std::string dir = ".";
		if (const char* live = std::getenv("KYTY_LIVE_FILE"); live != nullptr) {
			dir = live;
			const auto slash = dir.find_last_of("\/");
			dir = slash == std::string::npos ? "." : dir.substr(0, slash);
		}
		return dir;
	}

	void Reload() {
		const auto path = Directory() + "/skip_live.txt";
		std::error_code error;
		const auto      time = std::filesystem::last_write_time(path, error);
		if (error) {
			if (!m_skipped.empty()) {
				m_skipped.clear();
				std::printf("Live skip: %s gone, nothing skipped\n", path.c_str());
				std::fflush(stdout);
			}
			m_loaded_time = {};
			return;
		}
		if (time == m_loaded_time) {
			return;
		}
		m_loaded_time = time;
		m_skipped.clear();
		if (auto* file = std::fopen(path.c_str(), "rb"); file != nullptr) {
			std::string text;
			char        buffer[4096];
			size_t      read = 0;
			while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
				text.append(buffer, read);
			}
			std::fclose(file);
			size_t i = 0;
			while (i < text.size()) {
				while (i < text.size() && !std::isxdigit(static_cast<unsigned char>(text[i]))) {
					i++;
				}
				size_t end = i;
				while (end < text.size() &&
				       (std::isxdigit(static_cast<unsigned char>(text[end])) || text[end] == 'x' ||
				        text[end] == 'X')) {
					end++;
				}
				if (end > i) {
					m_skipped.insert(std::strtoull(text.substr(i, end - i).c_str(), nullptr, 16));
				}
				i = end;
			}
		}
		std::printf("Live skip: %zu shaders skipped (%s)\n", m_skipped.size(), path.c_str());
		std::fflush(stdout);
	}

	void WriteList() {
		const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
		                       std::chrono::system_clock::now().time_since_epoch())
		                       .count();
		const auto path = Directory() + "/logs/shaders_" + std::to_string(stamp) + ".txt";
		if (auto* file = std::fopen(path.c_str(), "w"); file != nullptr) {
			std::fprintf(file, "# stage hash uses\n");
			for (const auto& [key, count]: m_counts) {
				std::fprintf(file, "%u 0x%016" PRIx64 " %" PRIu64 "\n", key.first, key.second,
				             count);
			}
			std::fclose(file);
		}
		std::printf("Shader list: %zu shaders -> %s\n", m_counts.size(), path.c_str());
		std::fflush(stdout);
	}

	std::mutex                                           m_mutex;
	bool                                                 m_listing = false;
	std::chrono::steady_clock::time_point                m_list_end {};
	std::chrono::steady_clock::time_point                m_next_check {};
	std::map<std::pair<uint32_t, uint64_t>, uint64_t>    m_counts;
	std::unordered_set<uint64_t>                         m_skipped;
	std::filesystem::file_time_type                      m_loaded_time {};
};

// KYTY_RESOURCE_MEMO=1 (a live switch, off by default): see ProgramCache::Materialize.
bool ResourceMemoEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_RESOURCE_MEMO", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// --skip-shaders and KYTY_SKIP_SHADER_HASHES="hash,hash,...": skip the draws and dispatches of
// these guest shaders, the same way a shader that fails to compile is skipped. To see what one
// shader contributes, to step past one that loses the device, or to leave out work nothing can
// use (the ray-tracing BVH updates whose only consumer gives up).
bool SkipShaderRequested(uint64_t shader_hash) {
	static const std::vector<uint64_t> hashes = [] {
		std::vector<uint64_t> result;
		const auto parse = [&result](std::string_view list) {
			size_t start = 0;
			while (start < list.size()) {
				const auto end = std::min(list.find(',', start), list.size());
				if (end > start) {
					result.push_back(
					    std::strtoull(std::string(list.substr(start, end - start)).c_str(), nullptr, 16));
				}
				start = end + 1;
			}
		};
		parse(Config::GetSkipShaderHashes());
		if (const char* value = std::getenv("KYTY_SKIP_SHADER_HASHES"); value != nullptr) {
			parse(value);
		}
		for (const auto hash: result) {
			LOGF("ProgramCache: skipping the draws and dispatches of shader 0x%016" PRIx64 "\n",
			     hash);
		}
		return result;
	}();
	return std::ranges::find(hashes, shader_hash) != hashes.end();
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirvImpl(const char* label, uint64_t shader_hash,
                             const std::vector<uint32_t>& spirv);

// SPIR-V validation (Config shader_validation_enabled; the Qt launcher used to default it on):
// timed for KYTY_SHADER_STATS.
bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv);

bool ValidateShaderSpirvImpl(const char* label, uint64_t shader_hash,
                             const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

// A/B switch for background pipeline compiles (KYTY_ASYNC_PIPELINES=0 compiles every graphics
// pipeline on the GPU thread, as before) and how long a draw waits for a new one
// (KYTY_PIPELINE_WAIT_MS, default 20) before it is skipped.
static const bool g_async_pipelines = [] {
	const char* value = std::getenv("KYTY_ASYNC_PIPELINES");
	return value == nullptr || value[0] != '0';
}();
static const std::chrono::milliseconds g_pipeline_wait = [] {
	const char* value = std::getenv("KYTY_PIPELINE_WAIT_MS");
	return std::chrono::milliseconds(value != nullptr ? std::strtoul(value, nullptr, 10) : 20u);
}();

struct PendingGraphicsPipeline {
	std::unique_ptr<GraphicsPipelineBuild> build;
	vk::PipelineCache                      driver_cache = nullptr;
	vk::Pipeline                           pipeline     = nullptr;
	vk::Result                             result       = vk::Result::eSuccess;
	std::atomic<bool>                      done {false};
};

// Compiles graphics pipelines on worker threads. The driver compiled on the GPU thread, which
// stopped the whole emulated GPU for seconds per new pipeline (103 slow pipelines, 170 s of a
// 600 s first pass through the prologue).
class PipelineCompiler {
public:
	explicit PipelineCompiler(uint32_t threads) {
		for (uint32_t i = 0; i < threads; i++) {
			m_threads.emplace_back([this] { Run(); });
		}
	}
	~PipelineCompiler() { Stop(); }
	KYTY_CLASS_NO_COPY(PipelineCompiler);

	void Submit(std::shared_ptr<PendingGraphicsPipeline> job) {
		{
			std::lock_guard lock(m_mutex);
			m_queue.push_back(std::move(job));
		}
		m_work.notify_one();
	}

	// Waits up to the budget; true when the job is done.
	bool Wait(const PendingGraphicsPipeline& job, std::chrono::milliseconds budget) {
		std::unique_lock lock(m_mutex);
		return m_done.wait_for(lock, budget,
		                       [&job] { return job.done.load(std::memory_order_acquire); });
	}

	// Finishes the compiles in progress; queued ones are left to their owner.
	void Stop() {
		{
			std::lock_guard lock(m_mutex);
			if (m_stopped) {
				return;
			}
			m_stopped = true;
			m_queue.clear();
		}
		m_work.notify_all();
		for (auto& thread: m_threads) {
			thread.join();
		}
		m_threads.clear();
	}

	[[nodiscard]] bool Stopped() {
		std::lock_guard lock(m_mutex);
		return m_stopped;
	}

private:
	void Run() {
		for (;;) {
			std::shared_ptr<PendingGraphicsPipeline> job;
			{
				std::unique_lock lock(m_mutex);
				m_work.wait(lock, [this] { return m_stopped || !m_queue.empty(); });
				if (m_queue.empty()) {
					return;
				}
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			job->result = CreateGraphicsPipeline(*job->build, job->driver_cache, &job->pipeline);
			{
				std::lock_guard lock(m_mutex);
				job->done.store(true, std::memory_order_release);
			}
			m_done.notify_all();
		}
	}

	std::mutex                                           m_mutex;
	std::condition_variable                              m_work;
	std::condition_variable                              m_done;
	std::deque<std::shared_ptr<PendingGraphicsPipeline>> m_queue;
	std::vector<std::thread>                             m_threads;
	bool                                                 m_stopped = false;
};

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

// KYTY_ASYNC_SHADERS=1 (environment, research, default off): a draw whose new shader
// permutation is not built yet is skipped while a worker thread compiles it (CompileProgram,
// SPIR-V validation, shader module), instead of the GPU thread waiting. Only for graphics
// stages whose program writes no memory (like the background pipeline compiles); compute and
// memory-writing programs still compile on the GPU thread. Translation stays on the GPU thread
// (it reads guest memory and builds the resource plan). The recompiler's entry points are
// serialized by one mutex while the switch is on, since they were written single-threaded.
static const bool g_async_shaders = [] {
	const char* value = std::getenv("KYTY_ASYNC_SHADERS");
	return value != nullptr && value[0] == '1';
}();
static std::mutex g_recompiler_mutex;
// KYTY_RECOMPILER_LOCK=0 (environment, research): no global lock around the recompiler, so the
// async workers (KYTY_ASYNC_SHADER_THREADS) and the GPU thread's translations run in parallel.
// Audit (2026-10-04): the recompiler's shared state is atomics, mutex-guarded sets and
// thread-local flags; each compile works on its own program. Default on until tested in game.
static const bool g_recompiler_lock = [] {
	const char* value = std::getenv("KYTY_RECOMPILER_LOCK");
	return value == nullptr || value[0] != '0';
}();

static std::unique_lock<std::mutex> RecompilerLock() {
	return g_async_shaders && g_recompiler_lock ? std::unique_lock<std::mutex>(g_recompiler_mutex)
	                                            : std::unique_lock<std::mutex>();
}

// KYTY_SHADER_STATS=1 (live): every 5 s, where shader compile time goes, on all threads:
// translations (GPU thread), compiles on the GPU thread and on the async workers, SPIR-V
// validation and shader module creation, disk-cache hits.
struct ShaderCompileStats {
	std::atomic<uint64_t> translations {0}, translate_us {0};
	std::atomic<uint64_t> compiles {0}, compile_us {0};
	std::atomic<uint64_t> async_compiles {0}, async_compile_us {0};
	std::atomic<uint64_t> validate_us {0}, module_us {0};
	std::atomic<uint64_t> disk_hits {0};
	std::atomic<int64_t>  report_ms {0};

	static bool Enabled() {
		static auto& enabled = Common::LiveSwitches::Get("KYTY_SHADER_STATS", 0);
		return enabled.load(std::memory_order_relaxed) != 0;
	}
	static uint64_t Since(std::chrono::steady_clock::time_point start) {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                 std::chrono::steady_clock::now() - start)
		                                 .count());
	}
	void Report() {
		if (!Enabled()) {
			return;
		}
		const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                        std::chrono::steady_clock::now().time_since_epoch())
		                        .count();
		auto last = report_ms.load(std::memory_order_relaxed);
		if (last == 0) {
			report_ms.compare_exchange_strong(last, now_ms);
			return;
		}
		if (now_ms - last < 5000 || !report_ms.compare_exchange_strong(last, now_ms)) {
			return;
		}
		const auto ms = [](std::atomic<uint64_t>& us) {
			return static_cast<double>(us.exchange(0)) / 1000.0;
		};
		const auto translations   = this->translations.exchange(0);
		const auto translate_ms   = ms(translate_us);
		const auto compiles       = this->compiles.exchange(0);
		const auto compile_ms     = ms(compile_us);
		const auto async_compiles = this->async_compiles.exchange(0);
		const auto async_ms       = ms(async_compile_us);
		::printf("Shader compile (5 s): %" PRIu64 " translations %.1f ms | GPU-thread compiles %" PRIu64
		         " %.1f ms | async compiles %" PRIu64 " %.1f ms | validation %.1f ms | modules %.1f "
		         "ms | disk hits %" PRIu64 "\n",
		         translations, translate_ms, compiles, compile_ms, async_compiles, async_ms,
		         ms(validate_us), ms(module_us), disk_hits.exchange(0));
		std::fflush(stdout);
	}
};
static ShaderCompileStats g_shader_stats;

namespace {
bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	const auto start = std::chrono::steady_clock::now();
	const bool valid = ValidateShaderSpirvImpl(label, shader_hash, spirv);
	g_shader_stats.validate_us.fetch_add(ShaderCompileStats::Since(start),
	                                     std::memory_order_relaxed);
	return valid;
}

vk::ShaderModule TimedCompileSPV(std::span<const uint32_t> spirv, vk::Device device) {
	const auto start  = std::chrono::steady_clock::now();
	const auto module = CompileSPV(spirv, device);
	g_shader_stats.module_us.fetch_add(ShaderCompileStats::Since(start), std::memory_order_relaxed);
	return module;
}
} // namespace

struct AsyncPermutationJob {
	const char*                                  stage_name = nullptr;
	ShaderRecompiler::CompileOptions             options;
	std::vector<uint32_t>                        user_data;
	std::vector<uint32_t>                        back_code;
	std::unique_ptr<ShaderVertexInputInfo>       vertex;
	std::unique_ptr<ShaderPixelInputInfo>        pixel;
	ShaderRecompiler::TranslateResult            translated;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	uint32_t                                     push_data_cursor = 0;
	ProgramDiskCache::Store*                     disk             = nullptr;
	ProgramDiskCache::Key                        disk_key;
	vk::Device                                   device = nullptr;
	// Results, valid once done.
	ShaderRecompiler::IR::CompiledShaderInfo     info;
	vk::ShaderModule                             module = nullptr;
	std::atomic<bool>                            done {false};
	bool                                         claimed = false;
};

// KYTY_ASYNC_SHADER_THREADS=N (environment, default 1, at most 8): worker threads. More than one
// only compiles in parallel with KYTY_RECOMPILER_LOCK=0.
class AsyncShaderCompiler {
public:
	AsyncShaderCompiler() {
		const char* value   = std::getenv("KYTY_ASYNC_SHADER_THREADS");
		const int   threads = std::clamp(value != nullptr ? std::atoi(value) : 1, 1, 8);
		for (int i = 0; i < threads; i++) {
			m_threads.emplace_back([this](std::stop_token stop) { Run(stop); });
		}
	}
	~AsyncShaderCompiler() {
		for (auto& thread: m_threads) {
			thread.request_stop();
		}
		m_available.notify_all();
	}
	KYTY_CLASS_NO_COPY(AsyncShaderCompiler);

	void Submit(std::shared_ptr<AsyncPermutationJob> job) {
		{
			std::lock_guard lock(m_mutex);
			m_queue.push_back(std::move(job));
		}
		m_available.notify_one();
	}

private:
	void Run(std::stop_token stop) {
		for (;;) {
			std::shared_ptr<AsyncPermutationJob> job;
			{
				std::unique_lock lock(m_mutex);
				m_available.wait(lock, stop, [this] { return !m_queue.empty(); });
				if (m_queue.empty() || stop.stop_requested()) {
					return;
				}
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			Compile(*job);
		}
	}

	static void Compile(AsyncPermutationJob& job);

	std::mutex                                       m_mutex;
	std::condition_variable_any                      m_available;
	std::deque<std::shared_ptr<AsyncPermutationJob>> m_queue;
	std::vector<std::jthread>                        m_threads;
};

void AsyncShaderCompiler::Compile(AsyncPermutationJob& job) {
	job.options.user_data = job.user_data;
	job.options.back_code = job.back_code;
	if (job.vertex != nullptr) {
		job.options.input_info.vertex = job.vertex.get();
	} else if (job.pixel != nullptr) {
		job.options.input_info.pixel = job.pixel.get();
	}
	std::vector<uint32_t> spirv;
	{
		auto       lock   = RecompilerLock();
		const auto start  = std::chrono::steady_clock::now();
		auto       result = ShaderRecompiler::CompileProgram(std::move(job.translated), job.options,
		                                                     job.specialization, job.push_data_cursor);
		lock              = {}; // validation, dumps and the module need no recompiler lock
		g_shader_stats.async_compiles.fetch_add(1, std::memory_order_relaxed);
		g_shader_stats.async_compile_us.fetch_add(ShaderCompileStats::Since(start),
		                                          std::memory_order_relaxed);
		if (!ValidateShaderSpirv(job.options.dump_label, job.options.shader_hash, result.spirv)) {
			DumpShaderSpirv(job.stage_name, job.options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n",
			     job.options.dump_label, job.options.shader_hash);
		}
		DumpShaderSpirv(job.stage_name, job.options.shader_hash, result.spirv);
		job.info = std::move(result.program).TakeCompiledInfo();
		spirv    = std::move(result.spirv);
	}
	if (job.disk != nullptr) {
		std::vector<uint8_t> payload;
		ProgramDiskCache::MakePayload(spirv, job.info, payload);
		job.disk->Put(job.disk_key, payload);
	}
	job.module = TimedCompileSPV(spirv, job.device);
	EXIT_IF(job.module == nullptr);
	job.done.store(true, std::memory_order_release);
	g_shader_stats.Report();
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;
		// Exact expanded code includes callees and their original return-PC constants.
		// A different target/body must not reuse a program compiled for an earlier call.
		std::vector<uint32_t> function_code;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	// KYTY_RESOURCE_MEMO: the inputs and results of a recent MaterializeResources call.
	struct MemoSlot {
		std::vector<uint32_t>                        user_data;
		uint64_t                                     shader_base = 0;
		// Compute dispatch size: bounds the written extent of each written buffer.
		std::array<uint32_t, 3>                      workgroup_count {};
		std::array<uint32_t, 3>                      workgroup_size {};
		ShaderReadChunks::ReadLog                    log;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		uint64_t                                     last_use = 0;
	};
	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
		std::vector<MemoSlot>                        memo;
		// KYTY_ASYNC_SHADERS: permutations a worker is compiling.
		std::vector<std::shared_ptr<AsyncPermutationJob>> pending;
	};

	static constexpr size_t MemoSlots = 8;

	// Materialization reads guest memory only through the runtime's two readers, so the same
	// user data and shader base, with every logged read returning the same result and words,
	// give the same snapshot and specialization. A hit re-reads the logged words and copies the
	// stored result instead of evaluating the resource plan again.
	bool Materialize(SourceEntry& entry, const ShaderRecompiler::IR::SrtRuntime& runtime,
	                 ShaderReadChunks& reads) {
		if (!ResourceMemoEnabled()) {
			return ShaderRecompiler::IR::MaterializeResources(
			    entry.resource_plan, runtime, entry.resources, entry.specialization);
		}
		memo_clock++;
		for (auto& slot: entry.memo) {
			if (slot.shader_base == runtime.shader_base &&
			    slot.workgroup_count == runtime.workgroup_count &&
			    slot.workgroup_size == runtime.workgroup_size &&
			    std::ranges::equal(slot.user_data, runtime.user_data) &&
			    ReadsUnchanged(slot.log, reads)) {
				entry.resources      = slot.resources;
				entry.specialization = slot.specialization;
				slot.last_use = memo_clock;
				memo_hits++;
				ReportMemo();
				return true;
			}
		}
		memo_misses++;
		ShaderReadChunks::ReadLog log;
		reads.SetLog(&log);
		const bool ok = ShaderRecompiler::IR::MaterializeResources(
		    entry.resource_plan, runtime, entry.resources, entry.specialization);
		reads.SetLog(nullptr);
		if (ok) {
			MemoSlot* slot = nullptr;
			if (entry.memo.size() < MemoSlots) {
				slot = &entry.memo.emplace_back();
			} else {
				slot = &*std::ranges::min_element(entry.memo, {}, &MemoSlot::last_use);
			}
			slot->user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
			slot->shader_base    = runtime.shader_base;
			slot->workgroup_count = runtime.workgroup_count;
			slot->workgroup_size  = runtime.workgroup_size;
			slot->log            = std::move(log);
			slot->resources      = entry.resources;
			slot->specialization = entry.specialization;
			slot->last_use = memo_clock;
		}
		ReportMemo();
		return ok;
	}

	bool ReadsUnchanged(const ShaderReadChunks::ReadLog& log, ShaderReadChunks& reads) {
		for (const auto& read: log.reads) {
			memo_words.resize(read.count);
			const bool ok = ReadShaderGuestMemoryImpl(&reads, read.address, memo_words);
			if (ok != read.ok || (ok && !std::equal(memo_words.begin(), memo_words.end(),
			                                        log.words.begin() + read.first))) {
				return false;
			}
		}
		return true;
	}

	void ReportMemo() {
		const auto now = std::chrono::steady_clock::now();
		if (now - memo_report < std::chrono::seconds(5)) {
			return;
		}
		const auto total = memo_hits + memo_misses;
		::printf("Resource memo (5 s): %" PRIu64 " hits, %" PRIu64 " misses (%.1f%% hits)\n",
		         memo_hits, memo_misses, total != 0 ? 100.0 * memo_hits / total : 0.0);
		std::fflush(stdout);
		memo_hits   = 0;
		memo_misses = 0;
		memo_report = now;
	}

	// KYTY_PREDICT_STATS=1 (live, default 0): how often a lookup finds the same program as the
	// previous lookup of its stage (a candidate for evaluating the resources ahead from the
	// upcoming user data), and how often the user data is the same too.
	struct PredictStage {
		const SourceEntry*    last = nullptr;
		std::vector<uint32_t> user_data;
		uint64_t              same_program = 0, same_inputs = 0, other = 0;
	};
	std::array<PredictStage, 3>           predict_stages;
	std::chrono::steady_clock::time_point predict_report = std::chrono::steady_clock::now();

	void NotePredictability(ShaderType stage, const SourceEntry* entry,
	                        std::span<const uint32_t> user_data) {
		static auto& enabled = Common::LiveSwitches::Get("KYTY_PREDICT_STATS", 0);
		if (enabled.load(std::memory_order_relaxed) == 0 || entry == nullptr) {
			return;
		}
		const size_t index = stage == ShaderType::Pixel     ? 1u
		                     : stage == ShaderType::Compute ? 2u
		                                                    : 0u;
		auto&        s     = predict_stages[index];
		if (s.last == entry) {
			const bool same = std::ranges::equal(s.user_data, user_data);
			(same ? s.same_inputs : s.same_program)++;
		} else {
			s.other++;
		}
		s.last = entry;
		s.user_data.assign(user_data.begin(), user_data.end());
		const auto now = std::chrono::steady_clock::now();
		if (now - predict_report < std::chrono::seconds(5)) {
			return;
		}
		predict_report                       = now;
		static constexpr const char* Names[] = {"vertex", "pixel", "compute"};
		::printf("Predictability (5 s):");
		for (size_t i = 0; i < predict_stages.size(); i++) {
			auto&      p     = predict_stages[i];
			const auto total = p.same_program + p.same_inputs + p.other;
			::printf(" %s %" PRIu64 ": same program %.1f%% (same inputs %.1f%%)", Names[i], total,
			         total ? 100.0 * static_cast<double>(p.same_program + p.same_inputs) /
			                     static_cast<double>(total)
			               : 0.0,
			         total ? 100.0 * static_cast<double>(p.same_inputs) / static_cast<double>(total)
			               : 0.0);
			p.same_program = p.same_inputs = p.other = 0;
		}
		::printf("\n");
	}

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			PipelineKeyHash::Mix(hash, key.function_code.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	// KYTY_PROGRAM_DISK_CACHE (programDiskCache.h): the key of a permutation, from the same
	// inputs this cache reuses a permutation for within a session, plus the compiled code.
	ProgramDiskCache::Key DiskKey(const ShaderRecompiler::CompileOptions&             options,
	                              std::span<const uint32_t>                           code,
	                              const ShaderRecompiler::IR::ResourceSpecialization& specialization,
	                              uint32_t push_data_start_dword) const {
		ProgramDiskCache::KeyBuilder key;
		key.U32(1); // key layout version
		key.U32(static_cast<uint32_t>(lookup_key.stage));
		key.U64(lookup_key.hash);
		key.U32(lookup_key.user_data_count);
		key.U32(lookup_key.code_size);
		key.Words(lookup_key.static_state);
		key.Words(lookup_key.function_code);
		key.Words(code);
		key.U32(static_cast<uint32_t>(options.stage));
		key.U32(options.wave_size);
		key.U32(options.user_data_base);
		key.U32((options.bindless_images ? 1u : 0u) | (options.non_fatal ? 2u : 0u));
		key.Specialization(specialization);
		key.U32(push_data_start_dword);
		return key.Finish();
	}

	// A program that stores to memory must not be skipped (others may read what it writes).
	static bool WritesMemory(const ShaderRecompiler::IR::ResourcePlan& plan) {
		return !plan.buffer_writes.empty() ||
		       std::ranges::any_of(plan.info.buffers, [](const auto& buffer) { return buffer.written; }) ||
		       std::ranges::any_of(plan.info.images, [](const auto& image) {
			       return image.written &&
			              image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
		       });
	}

	void ReportAsync() {
		const auto now = std::chrono::steady_clock::now();
		if (now - async_report < std::chrono::seconds(5)) {
			return;
		}
		async_report = now;
		::printf("Async shaders (5 s): %u compiles started, %u finished, %u stage requests "
		         "skipped while compiling\n",
		         async_submitted, async_finished, async_skipped);
		std::fflush(stdout);
		async_submitted = async_finished = async_skipped = 0;
	}

	// The key of a program's resource plan: DiskKey without the specialization and push start.
	ProgramDiskCache::Key PlanKey(const ShaderRecompiler::CompileOptions& options,
	                              std::span<const uint32_t>               code) const {
		ProgramDiskCache::KeyBuilder key;
		key.U32(0x504c414eu); // "PLAN", key layout 1
		key.U32(static_cast<uint32_t>(lookup_key.stage));
		key.U64(lookup_key.hash);
		key.U32(lookup_key.user_data_count);
		key.U32(lookup_key.code_size);
		key.Words(lookup_key.static_state);
		key.Words(lookup_key.function_code);
		key.Words(code);
		key.U32(static_cast<uint32_t>(options.stage));
		key.U32(options.wave_size);
		key.U32(options.user_data_base);
		key.U32((options.bindless_images ? 1u : 0u) | (options.non_fatal ? 2u : 0u));
		return key.Finish();
	}

	// KYTY_PROGRAM_DISK_CACHE_VERIFY=1 (environment): compile on permutation hits and compare.
	static bool DiskVerify() {
		static const bool verify = [] {
			const char* value = std::getenv("KYTY_PROGRAM_DISK_CACHE_VERIFY");
			return value != nullptr && value[0] == '1';
		}();
		return verify;
	}

	void ReportDiskCache() {
		const auto now = std::chrono::steady_clock::now();
		if (now - disk_report < std::chrono::seconds(5) ||
		    disk_hits + disk_misses + disk_plan_hits + disk_plans_stored == 0) {
			return;
		}
		disk_report = now;
		::printf("Program disk cache (5 s): hits %u, misses %u, plans loaded %u stored %u refused %u, "
		         "records %zu, compile %.1f ms on misses%s\n",
		         disk_hits, disk_misses, disk_plan_hits, disk_plans_stored, disk_plans_refused,
		         disk.Size(), disk_compile_us / 1000.0,
		         disk_verify_mismatches != 0 ? ", VERIFY MISMATCHES" : "");
		std::fflush(stdout);
		disk_hits = disk_misses = 0;
		disk_plan_hits = disk_plans_stored = disk_plans_refused = 0;
		disk_compile_us         = 0;
	}

	// known_key: the permutation's disk key, already looked up and missed by the caller (Get's
	// phase-2 lookup); its hash and lookup are not repeated (except in verify mode).
	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               std::span<const uint32_t>                    code,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t                     push_data_start_dword,
	                               const ProgramDiskCache::Key* known_key = nullptr) {
		ProgramDiskCache::Key disk_key;
		std::vector<uint8_t>  payload;
		const bool            use_disk = disk.Enabled();
		if (use_disk) {
			disk_key = known_key != nullptr
			               ? *known_key
			               : DiskKey(options, code, specialization, push_data_start_dword);
			std::vector<uint32_t>                    spirv;
			ShaderRecompiler::IR::CompiledShaderInfo info;
			if ((known_key == nullptr || DiskVerify()) && disk.Get(disk_key, payload) &&
			    ProgramDiskCache::ReadPayload(payload, spirv, info)) {
				if (!DiskVerify()) {
					disk_hits++;
					g_shader_stats.disk_hits.fetch_add(1, std::memory_order_relaxed);
					ReportDiskCache();
					DumpShaderSpirv(stage_name, options.shader_hash, spirv);
					const auto module = TimedCompileSPV(spirv, device);
					EXIT_IF(module == nullptr);
					return {
					    .specialization = std::move(specialization),
					    .program        = std::move(info),
					    .handle         = {.id = ++next_shader_id, .module = module},
					};
				}
				auto check_lock = RecompilerLock();
				auto check = ShaderRecompiler::CompileProgram(std::move(translated), options,
				                                              specialization, push_data_start_dword);
				check_lock = {};
				const auto compiled = std::move(check.program).TakeCompiledInfo();
				if (check.spirv != spirv || !ProgramDiskCache::SameInfo(compiled, info)) {
					disk_verify_mismatches++;
					::printf("Program disk cache: VERIFY MISMATCH %s hash=0x%016" PRIx64
					         " (spirv %s, info %s)\n",
					         stage_name, options.shader_hash, check.spirv == spirv ? "same" : "differs",
					         ProgramDiskCache::SameInfo(compiled, info) ? "same" : "differs");
					std::fflush(stdout);
				}
				disk_hits++;
				ReportDiskCache();
				const auto module = TimedCompileSPV(check.spirv, device);
				EXIT_IF(module == nullptr);
				return {
				    .specialization = std::move(specialization),
				    .program        = compiled,
				    .handle         = {.id = ++next_shader_id, .module = module},
				};
			}
		}
		const auto compile_start = std::chrono::steady_clock::now();
		auto       compile_lock  = RecompilerLock();
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		compile_lock = {};
		g_shader_stats.compiles.fetch_add(1, std::memory_order_relaxed);
		g_shader_stats.compile_us.fetch_add(ShaderCompileStats::Since(compile_start),
		                                    std::memory_order_relaxed);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		if (use_disk) {
			disk_misses++;
			disk_compile_us += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::microseconds>(
			        std::chrono::steady_clock::now() - compile_start)
			        .count());
			auto info = std::move(result.program).TakeCompiledInfo();
			ProgramDiskCache::MakePayload(result.spirv, info, payload);
			disk.Put(disk_key, payload);
			ReportDiskCache();
			const auto module = TimedCompileSPV(result.spirv, device);
			EXIT_IF(module == nullptr);
			if (options.dump_ir) {
				LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
				     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
			}
			return {
			    .specialization = std::move(specialization),
			    .program        = std::move(info),
			    .handle         = {.id = ++next_shader_id, .module = module},
			};
		}

		const auto module = TimedCompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		if (SkipShaderRequested(params.hash) || LiveShaderTool::Instance().Skip(stage, params.hash)) {
			return ShaderProgram {};
		}
		BufferCache::SetReadbackStatsShader(params.hash);
		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		{
			KYTY_PROFILER_BLOCK("ProgramCache::BuildKey");
			lookup_key.stage           = stage;
			lookup_key.hash            = params.hash;
			lookup_key.user_data_count = params.user_data_count;
			lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
			BuildStageStaticKey(input_info, lookup_key.static_state);
			// Research: a program with inlined calls is valid only for the same call targets.
			if (const auto found = call_targets.find(params.hash); found != call_targets.end()) {
				for (const auto index: found->second) {
					lookup_key.static_state.push_back(index < user_data.size() ? user_data[index]
					                                                           : 0u);
				}
			}
			// Research: ShaderFunctions expands calls first; the calls it refuses reach the
			// recompiler's own inliner.
			lookup_key.function_code.clear();
			if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
				ExpandShaderFunctions(params, user_data, input_info.wave_size,
				                      lookup_key.function_code);
			}
		}
		auto entry = programs.end();
		{
			KYTY_PROFILER_BLOCK("ProgramCache::Lookup");
			if (unsupported.contains(lookup_key)) {
				return ShaderProgram {};
			}
			entry = programs.find(lookup_key);
		}
		NotePredictability(stage, entry != programs.end() ? &entry->second : nullptr, user_data);
		ShaderReadChunks                 read_chunks(ShaderReadChunks::Mode());
		ShaderRecompiler::IR::SrtRuntime runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderGuestMemoryRaw,
		    .userdata                   = &read_chunks,
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .float_image_atomics        = Config::FloatImageAtomicsEnabled(),
		};
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			for (uint32_t axis = 0; axis < 3u; axis++) {
				runtime.workgroup_count[axis] = input_info.dispatch_groups[axis];
				runtime.workgroup_size[axis]  = input_info.threads_num[axis];
			}
		}
		if (entry != programs.end()) {
			bool materialized = false;
			{
				KYTY_PROFILER_BLOCK("ProgramCache::MaterializeResources");
				materialized = Materialize(entry->second, runtime, read_chunks);
				if (ShaderRecompiler::IR::SrtNativeStats stats;
				    ShaderRecompiler::IR::TakeSrtNativeReport(stats)) {
					::printf("SRT native: %u plans compiled, %u failed, %" PRIu64 " KB of code, %" PRIu64
					     " instructions, %" PRIu64 " through the interpreter\n",
					     stats.plans, stats.failed, stats.bytes / 1024u, stats.instructions,
					     stats.interpreted);
				}
			}
			if (!materialized) {
				// A descriptor source that cannot be read right now (memory the guest has not
				// mapped or filled yet) skips this draw rather than the session; the next one
				// re-evaluates from scratch.
				if (!ShaderFailureNonFatal()) {
					EXIT("shader resource materialization failed\n");
				}
				static std::atomic<uint32_t> reported = 0;
				if (reported.fetch_add(1) < 16) {
					SkipReport("ProgramCache: skipping stage %u hash=0x%016" PRIx64
					     ": resource materialization failed (materialization line %d)\n",
					     static_cast<uint32_t>(stage), params.hash,
					     ShaderRecompiler::IR::LastIndirectImageFailureLine());
				}
				return ShaderProgram {};
			}
			auto permutation = entry->second.permutations.end();
			{
				KYTY_PROFILER_BLOCK("ProgramCache::FindPermutation");
				permutation = std::ranges::find_if(
				    entry->second.permutations, [&](const Permutation& candidate) {
					    const auto& layout = candidate.program.bindings;
					    return layout.push_data_start_dword ==
					               ShaderRecompiler::IR::PushData::StartFor(
					                   push_data_cursor, layout.ShaderDataDwords()) &&
					           candidate.specialization == entry->second.specialization;
				    });
			}
			if (permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		// KYTY_ASYNC_SHADERS: a permutation a worker compiles is skipped until it is done, then
		// taken like a built one.
		if (entry != programs.end() && !entry->second.pending.empty()) {
			auto& pending = entry->second.pending;
			const auto job = std::ranges::find_if(pending, [&](const auto& candidate) {
				return candidate->push_data_cursor == push_data_cursor &&
				       candidate->specialization == entry->second.specialization;
			});
			if (job != pending.end()) {
				if (!(*job)->done.load(std::memory_order_acquire)) {
					async_skipped++;
					ReportAsync();
					return ShaderProgram {};
				}
				auto finished = *job;
				pending.erase(job);
				finished->claimed = true;
				entry->second.permutations.push_back({
				    .specialization = std::move(finished->specialization),
				    .program        = std::move(finished->info),
				    .handle         = {.id = ++next_shader_id, .module = finished->module},
				});
				const auto& permutation = entry->second.permutations.back();
				input_info.stage = {.program   = &permutation.program,
				                    .resources = &entry->second.resources};
				permutation.program.bindings.AdvancePushData(push_data_cursor);
				async_finished++;
				return permutation.handle;
			}
		}

		// A program (or permutation) this session has not built yet: recompiled on the GPU
		// thread. Wolverine run 54: the 0.3-0.7 s frames while turning were spent here.
		KYTY_PROFILER_BLOCK("ProgramCache::Recompile", profiler::colors::RedA100);
		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		// KYTY_SHADER_TIMELINE=1: each program build the GPU thread waits for, with how long
		// before it the game registered the shader code ("never" for merged NGG pairs).
		struct TimelineScope {
			const char*                           stage;
			uint64_t                              hash;
			uint64_t                              compiles;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			~TimelineScope() {
				const auto ms = std::chrono::duration<double, std::milli>(
				                    std::chrono::steady_clock::now() - start)
				                    .count();
				const auto ago      = ShaderRegisteredSecondsAgo(hash);
				const bool compiled = g_shader_stats.compiles.load(std::memory_order_relaxed) != compiles;
				if (ago < 0.0) {
					std::printf("Shader timeline: %s 0x%016" PRIx64 " built %.1f ms (%s), registered never\n",
					            stage, hash, ms, compiled ? "compiled" : "disk");
				} else {
					std::printf("Shader timeline: %s 0x%016" PRIx64 " built %.1f ms (%s), registered %.1f s before\n",
					            stage, hash, ms, compiled ? "compiled" : "disk", ago);
				}
			}
		};
		std::optional<TimelineScope> timeline;
		if (ShaderTimelineEnabled()) {
			timeline.emplace(TimelineScope {stage_name, params.hash,
			                                g_shader_stats.compiles.load(std::memory_order_relaxed)});
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		options.non_fatal = ShaderFailureNonFatal();
		options.bindless_images = bindless_images;
		options.read_code       = ReadShaderCode;
		const auto compile_code = lookup_key.function_code.empty()
		                              ? params.code
		                              : std::span<const uint32_t>(lookup_key.function_code);
		std::optional<ProgramDiskCache::Key> disk_key; // the permutation's, once computed
		bool disk_key_checked = false;                  // ...and looked up (a miss)
		// KYTY_PROGRAM_DISK_CACHE phase 2: with the resource plan and the permutation on disk the
		// program is not translated at all. Programs with inlined call targets (their key grows
		// after translation) are neither stored nor looked up.
		if (disk.Enabled() && !call_targets.contains(params.hash)) {
			if (entry == programs.end()) {
				std::vector<uint8_t>               payload;
				ShaderRecompiler::IR::ResourcePlan plan;
				if (disk.Get(PlanKey(options, compile_code), payload) &&
				    ProgramDiskCache::DeserializePlan(payload, plan)) {
					entry = programs.try_emplace(lookup_key, std::move(plan)).first;
					disk_plan_hits++;
					if (!ShaderRecompiler::IR::MaterializeResources(
					        entry->second.resource_plan, runtime, entry->second.resources,
					        entry->second.specialization)) {
						if (!ShaderFailureNonFatal()) {
							EXIT("shader resource materialization failed\n");
						}
						return ShaderProgram {};
					}
				}
			}
			if (entry != programs.end()) {
				disk_key = DiskKey(options, compile_code, entry->second.specialization,
				                   push_data_cursor);
			}
			if (disk_key.has_value() && !DiskVerify()) {
				std::vector<uint8_t>                     payload;
				std::vector<uint32_t>                    spirv;
				ShaderRecompiler::IR::CompiledShaderInfo info;
				disk_key_checked = true;
				if (disk.Get(*disk_key, payload) &&
				    ProgramDiskCache::ReadPayload(payload, spirv, info)) {
					disk_hits++;
					g_shader_stats.disk_hits.fetch_add(1, std::memory_order_relaxed);
					ReportDiskCache();
					DumpShaderSpirv(stage_name, options.shader_hash, spirv);
					const auto module = TimedCompileSPV(spirv, device);
					EXIT_IF(module == nullptr);
					entry->second.permutations.push_back({
					    .specialization = entry->second.specialization,
					    .program        = std::move(info),
					    .handle         = {.id = ++next_shader_id, .module = module},
					});
					const auto& permutation = entry->second.permutations.back();
					input_info.stage        = {.program   = &permutation.program,
					                           .resources = &entry->second.resources};
					permutation.program.bindings.AdvancePushData(push_data_cursor);
					return permutation.handle;
				}
			}
		}
		auto       translate_lock  = RecompilerLock();
		const auto translate_start = std::chrono::steady_clock::now();
		auto       translated      = ShaderRecompiler::TranslateProgram(compile_code, options);
		translate_lock             = {};
		g_shader_stats.translations.fetch_add(1, std::memory_order_relaxed);
		g_shader_stats.translate_us.fetch_add(ShaderCompileStats::Since(translate_start),
		                                      std::memory_order_relaxed);
		g_shader_stats.Report();
		if (!translated.unsupported && !translated.call_target_user_data.empty() &&
		    call_targets.try_emplace(params.hash, translated.call_target_user_data).second) {
			for (const auto index: translated.call_target_user_data) {
				lookup_key.static_state.push_back(index < user_data.size() ? user_data[index] : 0u);
			}
		}
		if (translated.unsupported) {
			// Remember the refusal: a skipped shader is dispatched again every frame, and
			// re-deriving the same answer costs as much as a compile each time.
			unsupported.insert(lookup_key);
			static std::atomic<uint32_t> reported_unsupported = 0;
			if (reported_unsupported.fetch_add(1) < 32) {
				SkipReport("ProgramCache: skipping stage %u hash=0x%016" PRIx64
				           ": translation gave up\n",
				           static_cast<uint32_t>(stage), params.hash);
			}
			return ShaderProgram {};
		}
		if (!shader_clock && UsesShaderClock(translated.program)) {
			if (!ShaderFailureNonFatal()) {
				EXIT("S_MEMREALTIME needs shaderDeviceClock\n");
			}
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 16) {
				SkipReport("ProgramCache: skipping stage %u hash=0x%016" PRIx64
				     ": S_MEMREALTIME needs shaderDeviceClock\n",
				     static_cast<uint32_t>(stage), params.hash);
			}
			unsupported.insert(lookup_key);
			return ShaderProgram {};
		}
		if (entry == programs.end()) {
			auto plan_lock = RecompilerLock();
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program)).first;
			plan_lock = {};
			if (disk.Enabled() && translated.call_target_user_data.empty()) {
				std::vector<uint8_t> bytes;
				if (ProgramDiskCache::SerializePlan(entry->second.resource_plan, bytes)) {
					disk.Put(PlanKey(options, compile_code), bytes);
					disk_plans_stored++;
				} else {
					disk_plans_refused++;
				}
			}
			if (!ShaderRecompiler::IR::MaterializeResources(
			        entry->second.resource_plan, runtime, entry->second.resources,
			        entry->second.specialization)) {
				if (!ShaderFailureNonFatal()) {
					EXIT("shader resource materialization failed\n");
				}
				static std::atomic<uint32_t> reported = 0;
				if (reported.fetch_add(1) < 16) {
					SkipReport("ProgramCache: skipping stage %u hash=0x%016" PRIx64
					     ": resource materialization failed on first use (materialization line %d)\n",
					     static_cast<uint32_t>(stage), params.hash,
					     ShaderRecompiler::IR::LastIndirectImageFailureLine());
				}
				return ShaderProgram {};
			}
		}
		if (g_async_shaders && stage != ShaderType::Compute &&
		    !WritesMemory(entry->second.resource_plan)) {
			// A permutation key computed before translation missed already; one computed now (a
			// program first seen this session) is checked here.
			if (disk.Enabled() && !disk_key.has_value()) {
				disk_key = DiskKey(options, compile_code, entry->second.specialization,
				                   push_data_cursor);
			}
			const bool disk_hit = disk_key.has_value() && !disk_key_checked && disk.Contains(*disk_key);
			if (!disk_hit) {
				auto job              = std::make_shared<AsyncPermutationJob>();
				job->stage_name       = stage_name;
				job->options          = options;
				job->user_data.assign(options.user_data.begin(), options.user_data.end());
				job->back_code.assign(options.back_code.begin(), options.back_code.end());
				job->options.read_code = {};
				if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
					job->pixel = std::make_unique<ShaderPixelInputInfo>(input_info);
				} else if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
					job->vertex = std::make_unique<ShaderVertexInputInfo>(input_info);
				}
				job->translated       = std::move(translated);
				job->specialization   = entry->second.specialization;
				job->push_data_cursor = push_data_cursor;
				job->disk             = disk.Enabled() ? &disk : nullptr;
				job->disk_key         = disk_key.value_or(ProgramDiskCache::Key {});
				job->device           = device;
				entry->second.pending.push_back(job);
				if (async_compiler == nullptr) {
					async_compiler = std::make_unique<AsyncShaderCompiler>();
				}
				async_compiler->Submit(std::move(job));
				async_submitted++;
				ReportAsync();
				return ShaderProgram {};
			}
		}
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, compile_code, std::move(translated), entry->second.specialization,
		    push_data_cursor, disk_key_checked ? &*disk_key : nullptr));
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	ProgramCache(vk::Device device, bool shader_clock, bool bindless_images)
	    : device(device), shader_clock(shader_clock), bindless_images(bindless_images) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		async_compiler.reset(); // joins the worker; a job it was compiling finishes first
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& job: entry.pending) {
				if (job->done.load(std::memory_order_acquire) && job->module != nullptr) {
					device.destroyShaderModule(job->module, nullptr);
				}
			}
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	// Shader function expansion decodes the shader and every callee, and a shader with table
	// calls can be dispatched hundreds of times a frame; the expander keeps what it can reuse.
	void ExpandShaderFunctions(const ShaderParams& params, std::span<const uint32_t> user_data,
	                           uint32_t wave_size, std::vector<uint32_t>& expanded) {
		std::string reason;
		if (!function_expander.Expand(
		        params.code, params.Base(), user_data,
		        [](uint64_t address, std::span<uint32_t> words) {
			        return ReadShaderGuestMemoryRaw(nullptr, address, words);
		        },
		        expanded, reason, wave_size)) {
			static std::atomic<uint32_t> reports {0};
			if (reports.fetch_add(1) < 16) {
				::printf("Shader function expansion hash=%016" PRIx64 ": %s\n", params.hash,
				         reason.c_str());
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	std::unordered_set<ProgramKey, ProgramKeyHash>              unsupported;
	ShaderRecompiler::Decoder::ShaderFunctionExpander          function_expander;
	ProgramKey                                                  lookup_key;
	// Research: per shader hash, the user-data dwords holding its inlined call targets.
	std::unordered_map<uint64_t, std::vector<uint32_t>>         call_targets;
	// KYTY_RESOURCE_MEMO bookkeeping (GPU thread).
	uint64_t                              memo_clock  = 0;
	uint64_t                              memo_hits   = 0;
	uint64_t                              memo_misses = 0;
	std::vector<uint32_t>                 memo_words;
	std::chrono::steady_clock::time_point memo_report = std::chrono::steady_clock::now();
	// KYTY_ASYNC_SHADERS (GPU thread; the compiler owns its worker).
	std::unique_ptr<AsyncShaderCompiler>  async_compiler;
	uint32_t                              async_submitted = 0;
	uint32_t                              async_finished  = 0;
	uint32_t                              async_skipped   = 0;
	std::chrono::steady_clock::time_point async_report    = std::chrono::steady_clock::now();
	// KYTY_PROGRAM_DISK_CACHE (GPU thread).
	ProgramDiskCache::Store               disk;
	uint32_t                              disk_hits              = 0;
	uint32_t                              disk_misses            = 0;
	uint32_t                              disk_verify_mismatches = 0;
	uint32_t                              disk_plan_hits         = 0;
	uint32_t                              disk_plans_stored      = 0;
	uint32_t                              disk_plans_refused     = 0;
	uint64_t                              disk_compile_us        = 0;
	std::chrono::steady_clock::time_point disk_report            = std::chrono::steady_clock::now();
	vk::Device                                                  device;
	bool                                                        shader_clock = false;
	bool                                                        bindless_images = false;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics),
      m_program_cache(std::make_unique<ProgramCache>(
          graphics.device, graphics.shader_device_clock_enabled,
          graphics.bindless_enabled && Config::BindlessImagesEnabled())) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	InitializeProgramDiskCache();
	if (g_async_pipelines) {
		const auto threads = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 4u);
		m_compiler         = std::make_unique<PipelineCompiler>(threads);
		PipelineCacheLog("Vulkan pipelines: compiled on {} worker threads (wait {} ms)", threads,
		                 g_pipeline_wait.count());
	}
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			if (pipeline->pending && pipeline->pending->done.load(std::memory_order_acquire)) {
				m_graphics.device.destroyPipeline(pipeline->pending->pipeline, nullptr);
			}
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

// KYTY_PROGRAM_DISK_CACHE=1|2 (environment): translated permutations on disk, one file per
// signature (revision, GPU, driver, KYTY_* environment) so changing a switch keeps the others.
void PipelineCache::InitializeProgramDiskCache() {
	const int mode = ProgramDiskCache::Mode();
	if (mode <= 0) {
		return;
	}
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	// KYTY_PROGRAM_CACHE_GIT_REVISION=<full commit id> (environment, developer use only): key the
	// program disk cache as that build did (its short hash and the revision in the driver
	// signature), to keep its warm cache across rebuilds that do not change shader translation
	// (every test build otherwise starts cold). Wrong use loads programs translated by other code.
	const char*       revision_override = std::getenv("KYTY_PROGRAM_CACHE_GIT_REVISION");
	const bool        overridden = revision_override != nullptr && std::strlen(revision_override) >= 7;
	const std::string short_override = overridden ? std::string(revision_override, 7) : std::string();
	const std::string_view git_hash = overridden ? std::string_view(short_override) : KYTY_GIT_HASH;
	auto driver_signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	if (overridden) {
		if (const auto at = driver_signature.find(KYTY_GIT_REVISION); at != std::string::npos) {
			driver_signature.replace(at, std::strlen(KYTY_GIT_REVISION), revision_override);
		}
		PipelineCacheLog("Program disk cache: keyed as revision {}", revision_override);
	}
	if (mode == 1 && (KYTY_BUILD != KYTY_BUILD_RELEASE || git_hash == "unknown" ||
	                  git_hash.ends_with("-dirty"))) {
		PipelineCacheLog("Program disk cache: disabled (not a clean Release build; "
		                 "KYTY_PROGRAM_DISK_CACHE=2 forces it)");
		return;
	}
	auto signature = fmt::format("KytyPRG1:{}:{}:bindless={}:float_atomics={}\n", git_hash,
	                             driver_signature,
	                             m_graphics.bindless_enabled && Config::BindlessImagesEnabled(),
	                             Config::FloatImageAtomicsEnabled());
	for (const auto& line: std::string_view(ProgramDiskCache::EnvironmentSignature()) |
	                           std::views::split('\n')) {
		const std::string_view entry(line.begin(), line.end());
		const auto             name = entry.substr(0, entry.find('='));
		if (!entry.empty() && ProgramDiskCache::AffectsTranslation(name)) {
			signature += entry;
			signature += '\n';
		}
	}
	const auto name = fmt::format("{}.programs.{:016x}.bin", title_id,
	                              XXH3_64bits(signature.data(), signature.size()));
	const auto path   = std::filesystem::path("_PipelineCache") / name;
	const auto loaded = m_program_cache->disk.Open(path, signature);
	PipelineCacheLog("Program disk cache: {} ({} permutations)", Common::PathToString(path),
	                 loaded);
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
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
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
			    payload_read != initial_data.size() ||
			    !DriverCacheSignatureMatches(cached_signature, signature) ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			} else if (cached_signature != signature) {
				PipelineCacheLog("Vulkan pipeline cache: keeping {} from another emulator revision",
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
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

// Writes the driver cache to its file (temp file + rename). vkGetPipelineCacheData needs no
// external synchronization, so a checkpoint runs while workers keep creating pipelines.
bool PipelineCache::WriteDriverCache(const char* what) {
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
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return false;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
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
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: {} {} bytes to {}", what, payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	return true;
}

// KYTY_PIPELINE_CACHE_CHECKPOINT_S=N (live, default 180; 0 = only at exit): the driver cache was
// written only when the window closed normally, so a session that ended in a crash (a device
// loss, an unhandled fault) left the next launch compiling every pipeline again. Every N
// seconds, if pipelines were created since the last write, a background thread writes it.
void PipelineCache::MaybeCheckpoint() {
	static auto& interval = Common::LiveSwitches::Get("KYTY_PIPELINE_CACHE_CHECKPOINT_S", 180);
	const auto   seconds  = interval.load(std::memory_order_relaxed);
	if (seconds <= 0) {
		return;
	}
	std::unique_lock checkpoint_lock(m_checkpoint_mutex, std::try_to_lock);
	if (!checkpoint_lock.owns_lock() || m_driver_cache == nullptr || m_driver_cache_path.empty()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (m_last_checkpoint == std::chrono::steady_clock::time_point {}) {
		m_last_checkpoint = now;
		return;
	}
	if (now - m_last_checkpoint < std::chrono::seconds(seconds)) {
		return;
	}
	const auto pipelines = m_graphics_pipelines.size() + m_compute_pipelines.size();
	if (pipelines == m_checkpoint_pipelines) {
		m_last_checkpoint = now;
		return;
	}
	if (m_checkpoint_thread.joinable()) {
		if (!m_checkpoint_done.load(std::memory_order_acquire)) {
			return;
		}
		m_checkpoint_thread.join();
	}
	m_last_checkpoint      = now;
	m_checkpoint_pipelines = pipelines;
	m_checkpoint_done.store(false, std::memory_order_relaxed);
	m_checkpoint_thread = std::thread([this] {
		(void)WriteDriverCache("checkpoint saved");
		m_checkpoint_done.store(true, std::memory_order_release);
	});
}

void PipelineCache::JoinCheckpoint() {
	if (m_checkpoint_thread.joinable()) {
		m_checkpoint_thread.join();
	}
}

void PipelineCache::Save() {
	// Held to the end: a checkpoint must not start while the cache is written and destroyed.
	std::lock_guard checkpoint_lock(m_checkpoint_mutex);
	JoinCheckpoint();
	// No worker may use the driver cache once it is destroyed below; later pipelines are
	// compiled on the GPU thread (FinishPending).
	if (m_compiler != nullptr) {
		m_compiler->Stop();
	}
	if (m_driver_cache == nullptr) {
		return;
	}
	(void)WriteDriverCache("saved");
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		if (!PrepareTessellationPrograms(vertex_regs, context, vertex_info, vertex_params)) {
			if (!ShaderFailureNonFatal()) {
				EXIT("unsupported tessellation programs\n");
			}
			return {};
		}
	} else {
		KYTY_PROFILER_BLOCK("PipelineCache::PrepareProgram(VS)");
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		// A subgroup with more threads than a mesh workgroup may have (NVIDIA: 128) runs its
		// waves in passes; see EmitMeshEntryPoint.
		mesh.passes = mesh.PassesFor(
		    std::min(limits.maxMeshWorkGroupInvocations, limits.maxMeshWorkGroupSize[0]));
		if (mesh.passes == 0 || mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			// Skipped like the draws of a shader that gives up, and reported once per shader.
			static std::mutex                   logged_mutex;
			static std::unordered_set<uint64_t> logged;
			std::lock_guard                     lock(logged_mutex);
			if (logged.insert(vertex_params[0].hash).second) {
				LOGF("mesh shader 0x%016" PRIx64 " exceeds host limits, draws skipped: wave%u "
				     "threads=%u vertices=%u primitives=%u LDS=%u\n",
				     vertex_params[0].hash, mesh.wave_size, mesh.HostThreads(), mesh.max_vertices,
				     mesh.max_primitives, mesh.lds_size_dwords);
			}
			return {};
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		KYTY_PROFILER_BLOCK("PipelineCache::PrepareProgram(PS)");
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		// SPI_SHADER_COL_FORMAT describes export packing, not the attachment numeric type.
		// In particular, 32-bit exports can carry raw integer material data.
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			const auto& rt = context.GetRenderTarget(slot);
			if (rt.base.addr == 0 ||
			    render_target_mask_slot(context.GetRenderTargetMask(), slot) == 0) {
				continue;
			}
			if (rt.info.channel_type == Prospero::ChannelType::kUInt) {
				pixel_info.target_uint_mask |= 1u << slot;
			} else if (rt.info.channel_type == Prospero::ChannelType::kSInt) {
				pixel_info.target_sint_mask |= 1u << slot;
			}
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
			pixel_info.target_uint_mask =
			    (pixel_info.target_uint_mask & ~2u) | ((pixel_info.target_uint_mask & 1u) << 1u);
			pixel_info.target_sint_mask =
			    (pixel_info.target_sint_mask & ~2u) | ((pixel_info.target_sint_mask & 1u) << 1u);
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
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
		// The draw is skipped without a pixel program (a skipped or failed shader, or one an
		// async worker is compiling); building the vertex stages now would only give them a
		// push-data start without the pixel stage's data: a permutation never used.
		if (!result.pixel) {
			return {};
		}
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
		if (!result.vertex[i]) {
			return {};
		}
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	// Use one effective size for the cache key, LDS declaration, and access bounds.
	const auto max_lds_dwords =
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize / 4u;
	if (input_info.lds_size_dwords > max_lds_dwords) {
		static std::atomic_bool warned = false;
		if (!warned.exchange(true, std::memory_order_relaxed)) {
			PipelineCacheLog("GPU warning: game compute shader requests {} bytes of LDS, but "
			                 "the Vulkan device limit is {} bytes. Clamping LDS; rendering may "
			                 "be incorrect.",
			                 input_info.lds_size_dwords * 4u, max_lds_dwords * 4u);
		}
	}
	input_info.lds_size_dwords = std::min(input_info.lds_size_dwords, max_lds_dwords);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	auto* pipeline = TryGetGraphicsPipeline(colors, depth, vertex_info, command, ps_input_info,
	                                        topology, primitive_restart_enable, programs, false);
	EXIT_IF(pipeline == nullptr);
	return *pipeline;
}

bool PipelineCache::FinishPending(Pipeline& pipeline) {
	auto& job = *pipeline.pending;
	if (!job.done.load(std::memory_order_acquire)) {
		if (m_compiler != nullptr && !m_compiler->Stopped()) {
			return false;
		}
		// The workers stopped before this job started: compile it here.
		job.result = CreateGraphicsPipeline(*job.build, m_driver_cache, &job.pipeline);
		job.done.store(true, std::memory_order_release);
	}
	EXIT_NOT_IMPLEMENTED(job.result != vk::Result::eSuccess || job.pipeline == nullptr);
	pipeline.pipeline = job.pipeline;
	pipeline.pending.reset();
	return true;
}

PipelineCache::Pipeline* PipelineCache::TryGetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs, bool may_defer) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
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
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	const auto defer = [this] {
		if (++m_deferred_draws % 256 == 1) {
			LOGF("PipelineCache: %" PRIu64 " draws skipped while their pipeline compiled in the "
			     "background\n",
			     m_deferred_draws);
		}
		return nullptr;
	};
	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		auto& found = *iter->second;
		if (found.pending && !FinishPending(found)) {
			return defer();
		}
		return &found;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	auto build = PrepareGraphicsPipeline(m_graphics, *cached, rendering, key.vertex_input,
	                                     vertex_info, ps_input_info, programs, static_params);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);
	if (may_defer && m_compiler != nullptr && !m_compiler->Stopped()) {
		// A pipeline the driver has cached finishes within the wait; a new one is compiled in
		// the background, and its draws are skipped until it is ready.
		auto job          = std::make_shared<PendingGraphicsPipeline>();
		job->build        = std::move(build);
		job->driver_cache = m_driver_cache;
		cached->pending   = job;
		m_compiler->Submit(job);
		(void)m_compiler->Wait(*job, g_pipeline_wait);
		auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
		EXIT_IF(!inserted);
		auto& pipeline = *iter->second;
		if (!FinishPending(pipeline)) {
			return defer();
		}
		LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);
		return &pipeline;
	}
	const auto result = CreateGraphicsPipeline(*build, m_driver_cache, &cached->pipeline);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return iter->second.get();
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}
} // namespace Libs::Graphics
