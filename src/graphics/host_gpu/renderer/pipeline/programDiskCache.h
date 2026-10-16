#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMDISKCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMDISKCACHE_H_

// KYTY_PROGRAM_DISK_CACHE (notes/cloud/T4_PROGRAM_DISK_CACHE.md): translated shader permutations
// (SPIR-V + CompiledShaderInfo) kept on disk, so a second launch skips CompileProgram.

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ProgramDiskCache {

struct Key {
	uint64_t lo = 0;
	uint64_t hi = 0;
	bool     operator==(const Key&) const = default;
};

struct KeyHash {
	size_t operator()(const Key& key) const noexcept {
		return static_cast<size_t>(key.lo ^ (key.hi * 0x9e3779b97f4a7c15ull));
	}
};

// Accumulates the inputs of one permutation; Finish gives the 128-bit key.
class KeyBuilder {
public:
	KeyBuilder();
	~KeyBuilder();
	KeyBuilder(const KeyBuilder&)            = delete;
	KeyBuilder& operator=(const KeyBuilder&) = delete;

	void Bytes(const void* data, size_t size);
	void U32(uint32_t value) { Bytes(&value, sizeof(value)); }
	void U64(uint64_t value) { Bytes(&value, sizeof(value)); }
	void Words(std::span<const uint32_t> words) {
		U64(words.size());
		Bytes(words.data(), words.size_bytes());
	}
	void Specialization(const ShaderRecompiler::IR::ResourceSpecialization& specialization);
	[[nodiscard]] Key Finish();

private:
	void* m_state = nullptr;
};

// CompiledShaderInfo <-> bytes, field by field.
void SerializeInfo(const ShaderRecompiler::IR::CompiledShaderInfo& info, std::vector<uint8_t>& out);
[[nodiscard]] bool DeserializeInfo(std::span<const uint8_t>& in,
                                   ShaderRecompiler::IR::CompiledShaderInfo& info);

// ResourcePlan <-> bytes (phase 2: a second launch skips TranslateProgram). False when the plan
// cannot be stored (a value outside its own storage) or the bytes do not fit this build.
[[nodiscard]] bool SerializePlan(const ShaderRecompiler::IR::ResourcePlan& plan,
                                 std::vector<uint8_t>&                     out);
[[nodiscard]] bool DeserializePlan(std::span<const uint8_t>           in,
                                   ShaderRecompiler::IR::ResourcePlan& plan);

[[nodiscard]] bool SameInfo(const ShaderRecompiler::IR::CompiledShaderInfo& a,
                            const ShaderRecompiler::IR::CompiledShaderInfo& b);

// A permutation's payload: SPIR-V words then the info.
void MakePayload(std::span<const uint32_t> spirv,
                 const ShaderRecompiler::IR::CompiledShaderInfo& info, std::vector<uint8_t>& out);
[[nodiscard]] bool ReadPayload(std::span<const uint8_t> payload, std::vector<uint32_t>& spirv,
                               ShaderRecompiler::IR::CompiledShaderInfo& info);

// The file: a header with the signature, then appended records. Thread-safe.
class Store {
public:
	// Loads `path` when its signature matches; otherwise the file is started anew on the first
	// Put. Returns the number of records loaded.
	size_t Open(const std::filesystem::path& path, std::string_view signature);
	[[nodiscard]] bool Enabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }
	// The payload stored for the key, copied out; false when there is none.
	[[nodiscard]] bool Get(const Key& key, std::vector<uint8_t>& payload) const;
	void               Put(const Key& key, std::span<const uint8_t> payload);
	[[nodiscard]] bool Contains(const Key& key) const;
	[[nodiscard]] size_t Size() const;

private:
	bool WriteHeaderLocked();

	mutable std::mutex                                 m_mutex;
	std::unordered_map<Key, std::vector<uint8_t>, KeyHash> m_records;
	std::filesystem::path                              m_path;
	std::string                                        m_signature;
	std::ofstream                                      m_out;
	// Read without the mutex by the GPU thread; a worker's failed Put clears it.
	std::atomic<bool>                                  m_enabled {false};
	bool                                               m_header_valid  = false;
	uint64_t                                           m_valid_bytes   = 0;
};

// KYTY_PROGRAM_DISK_CACHE: 0 off, 1 clean Release builds, 2 any build. Read once.
[[nodiscard]] int Mode();
// The KYTY_* environment, sorted, as "NAME=value\n" lines: compile-time switches are read from it.
[[nodiscard]] std::string EnvironmentSignature();
// Whether a KYTY_* variable can change what the translator and compiler produce, so it selects
// the cache file. Runtime-only switches (memory, GC, faults, stats) do not: changing them keeps
// the warm cache. Conservative: names the shader code reads, plus any name that looks related.
[[nodiscard]] bool AffectsTranslation(std::string_view name);

} // namespace Libs::Graphics::ProgramDiskCache

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMDISKCACHE_H_
