#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtNative.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	// Accept image atomics on k32Float descriptors: upstream's float atomics, and integer atomics
	// run as uint atomics on the raw bits. On by default, as in the emulator; the emulator passes
	// --no-float-image-atomics through here.
	bool                      float_image_atomics        = true;
	// Compute: the dispatch's workgroup count (zero when the host does not know it, as for an
	// indirect dispatch) and workgroup size bound the invocation IDs in buffer write extents.
	std::array<uint32_t, 3>   workgroup_count            = {};
	std::array<uint32_t, 3>   workgroup_size             = {};
};

enum class RuntimeValueType { Any, Integer };

// A ResourcePlan compiled for evaluation (KYTY_SRT_COMPILED): every value its roots reach,
// resolved once (identity chains, invariant phis, ReadConst's SRT read), with arguments as node
// indices. Evaluation stays lazy and uses SrtWalker's opcode semantics.
struct CompiledSrt {
	static constexpr int32_t None = -1;
	enum class Kind : uint8_t { Fail, Constant, Inst };
	struct Node {
		const Inst*            inst     = nullptr;
		uint64_t               value    = 0;
		Kind                   kind     = Kind::Fail;
		uint8_t                num_args = 0;
		std::array<int32_t, 8> args {};
	};
	std::vector<Node>                        nodes;
	std::unordered_map<const Inst*, int32_t> index;
	std::vector<int32_t>                     srt_reads;
	std::vector<std::array<int32_t, 8>>      descriptors;
	std::vector<int32_t>                     conditions;
};

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, SrtWalker* clean_evaluator = nullptr,
	          Value active_mask = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	friend struct SrtNativeHelpers;

	// Binds this walker to the plan's native code when the configuration is one it was compiled
	// for (SrtNative.h), compiling it once the plan is refreshed often enough.
	void BindNative();
	bool VerifyNative(Value value, bool native_ok, uint64_t native_result);
	// A value from the native code's tables, evaluated with this walker's frame and mode.
	bool EvaluateNative(const SrtNativeValue& value, uint64_t& result);
	[[nodiscard]] bool UseNativeTables() const;

	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	// KYTY_SRT_COMPILED: evaluates a compiled node (the IR value `value` for the comparison mode).
	bool EvaluateRoot(int32_t node, Value value, uint32_t& result);
	bool EvaluateNode(int32_t node, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	// Argument `index` of the instruction that is argument `handle` of inst (a resolved handle).
	bool HandleArg(const Inst& inst, size_t handle, size_t index, uint64_t& result);
	// Argument `index` of inst as a compiled node, if inst is the node being evaluated.
	[[nodiscard]] int32_t CurrentNodeArg(const Inst& inst, size_t index) const;
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool EvaluateBufferRead(const Inst& inst, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
	std::span<const uint8_t>         m_clean_flat_slots;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	const Inst*                     m_failed_value = nullptr;
	ResourcePlan::EvaluationContext& m_context;
	// KYTY_SRT_COMPILED: the plan's compiled form (null: walk the IR), the node being evaluated,
	// and whether every root is evaluated both ways and compared (mode 2).
	const CompiledSrt*       m_compiled = nullptr;
	const CompiledSrt::Node* m_node     = nullptr;
	bool                     m_compare  = false;
	const SrtNativeCode*            m_native      = nullptr;
	SrtNativeMode                   m_native_mode = SrtNativeMode::Self;
	SrtNativeFrame                  m_native_frame;
	// The last raw read that failed, for RefreshFlatBuffer's report.
	const char* m_read_failure         = nullptr;
	uint64_t    m_read_failure_address = 0;
	uint64_t    m_read_failure_offset  = 0;
	uint64_t    m_read_failure_size    = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
