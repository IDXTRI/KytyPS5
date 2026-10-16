#include "graphics/host_gpu/renderer/pipeline/programDiskCache.h"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <xxhash.h>

#if !defined(_WIN32)
extern char** environ; // NOLINT(readability-redundant-declaration)
#endif

namespace Libs::Graphics::ProgramDiskCache {

namespace IR = ShaderRecompiler::IR;

namespace {

constexpr char     Magic[8]      = {'K', 'Y', 'T', 'Y', 'P', 'R', 'G', '1'};
constexpr uint32_t MaxPayload    = 64u << 20u;
constexpr uint32_t MaxSignature  = 1u << 20u;
constexpr size_t   RecordHeader  = sizeof(uint64_t) * 2 + sizeof(uint32_t) + sizeof(uint64_t);

class Writer {
public:
	explicit Writer(std::vector<uint8_t>& out): m_out(out) {}
	void Bytes(const void* data, size_t size) {
		const auto* bytes = static_cast<const uint8_t*>(data);
		m_out.insert(m_out.end(), bytes, bytes + size);
	}
	void U32(uint32_t value) { Bytes(&value, sizeof(value)); }
	void I32(int32_t value) { Bytes(&value, sizeof(value)); }
	void U64(uint64_t value) { Bytes(&value, sizeof(value)); }
	void Bool(bool value) { U32(value ? 1u : 0u); }
	template <typename E>
	void Enum(E value) {
		U32(static_cast<uint32_t>(value));
	}
	void Words(const std::vector<uint32_t>& words) {
		U32(static_cast<uint32_t>(words.size()));
		Bytes(words.data(), words.size() * sizeof(uint32_t));
	}
	void String(const std::string& text) {
		U32(static_cast<uint32_t>(text.size()));
		Bytes(text.data(), text.size());
	}

private:
	std::vector<uint8_t>& m_out;
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t>& in): m_in(in) {}
	bool Bytes(void* data, size_t size) {
		if (m_in.size() < size) {
			m_ok = false;
			return false;
		}
		if (size != 0) {
			std::memcpy(data, m_in.data(), size);
		}
		m_in = m_in.subspan(size);
		return true;
	}
	uint32_t U32() {
		uint32_t value = 0;
		Bytes(&value, sizeof(value));
		return value;
	}
	int32_t I32() {
		int32_t value = 0;
		Bytes(&value, sizeof(value));
		return value;
	}
	uint64_t U64() {
		uint64_t value = 0;
		Bytes(&value, sizeof(value));
		return value;
	}
	bool Bool() { return U32() != 0u; }
	template <typename E>
	E Enum() {
		return static_cast<E>(U32());
	}
	void Words(std::vector<uint32_t>& words) {
		const auto count = U32();
		if (count > m_in.size() / sizeof(uint32_t)) {
			m_ok = false;
			return;
		}
		words.resize(count);
		Bytes(words.data(), count * sizeof(uint32_t));
	}
	void String(std::string& text) {
		const auto size = U32();
		if (size > m_in.size()) {
			m_ok = false;
			return;
		}
		text.assign(reinterpret_cast<const char*>(m_in.data()), size);
		m_in = m_in.subspan(size);
	}
	// A vector element count, bounded by what is left (every element takes >= 4 bytes).
	bool Count(uint32_t& count) {
		count = U32();
		if (count > m_in.size() / 4u) {
			m_ok = false;
		}
		return m_ok;
	}
	[[nodiscard]] bool Ok() const noexcept { return m_ok; }

private:
	std::span<const uint8_t>& m_in;
	bool                      m_ok = true;
};

} // namespace

KeyBuilder::KeyBuilder() {
	auto* state = XXH3_createState();
	XXH3_128bits_reset(state);
	m_state = state;
}

KeyBuilder::~KeyBuilder() {
	XXH3_freeState(static_cast<XXH3_state_t*>(m_state));
}

void KeyBuilder::Bytes(const void* data, size_t size) {
	XXH3_128bits_update(static_cast<XXH3_state_t*>(m_state), data, size);
}

void KeyBuilder::Specialization(const IR::ResourceSpecialization& specialization) {
	U64(specialization.buffers.size());
	for (const auto& buffer: specialization.buffers) {
		U32(buffer.packed_stride);
		U32(static_cast<uint32_t>(buffer.descriptor_format));
		U32(buffer.descriptor_swizzle);
		U32(buffer.zero_stride_oob ? 1u : 0u);
	}
	U64(specialization.images.size());
	for (const auto& image: specialization.images) {
		U32(static_cast<uint32_t>(image.numeric_class));
		U32(static_cast<uint32_t>(image.dimension));
		U32(image.mip_count);
		U32(static_cast<uint32_t>(image.conversion_format));
		U32(image.shader_swizzle);
		U32(image.indirect_root);
		U32(image.indirect_mapping_offset);
		U32(image.indirect_search_iterations);
		U32((image.cube ? 1u : 0u) | (image.fmask ? 2u : 0u) | (image.bindless ? 4u : 0u));
	}
	U64(specialization.samplers.size());
	for (const auto& sampler: specialization.samplers) {
		U32(sampler.bindless ? 1u : 0u);
		U32(sampler.bindless_mapping_offset);
	}
}

Key KeyBuilder::Finish() {
	const auto hash = XXH3_128bits_digest(static_cast<XXH3_state_t*>(m_state));
	return {hash.low64, hash.high64};
}

static void WriteShaderInfo(Writer& w, const IR::ShaderInfo& s) {
	w.U32(static_cast<uint32_t>(s.buffers.size()));
	for (const auto& b: s.buffers) {
		w.U32(b.source);
		w.U32(b.first_use_pc);
		w.U32(b.max_byte_extent);
		w.U32(b.packed_stride);
		w.Enum(b.descriptor_format);
		w.U32(b.descriptor_swizzle);
		w.U32(b.image_alias);
		w.Bool(b.read);
		w.Bool(b.written);
		w.Bool(b.atomic);
		w.Bool(b.formatted);
		w.Bool(b.scalar);
		w.Bool(b.gpu_records);
	}
	w.U32(static_cast<uint32_t>(s.images.size()));
	for (const auto& i: s.images) {
		w.U32(i.source);
		w.U32(i.first_use_pc);
		w.Enum(i.resource_class);
		w.Enum(i.numeric_class);
		w.Enum(i.dimension);
		w.Enum(i.mip_mode);
		w.U32(i.mip_count);
		w.Enum(i.conversion_format);
		w.U32(i.shader_swizzle);
		w.Bool(i.read);
		w.Bool(i.written);
		w.Bool(i.atomic);
		w.Bool(i.atomic64);
		w.Bool(i.depth_compare);
		w.Bool(i.cube);
		w.Bool(i.r128);
		w.U32(i.indirect_root);
		w.U32(i.indirect_mapping_offset);
		w.U32(i.indirect_search_iterations);
		w.Bool(i.bindless);
		w.Words(i.indirect_resources);
	}
	w.U32(static_cast<uint32_t>(s.samplers.size()));
	for (const auto& r: s.samplers) {
		w.U32(r.source);
		w.U32(r.first_use_pc);
		w.Bool(r.force_point_filtering);
		w.Bool(r.depth_compare);
		w.Bool(r.integer_border);
		w.Bool(r.bindless);
		w.U32(r.bindless_mapping_offset);
		w.Bool(r.gather_lod);
	}
	w.U32(static_cast<uint32_t>(s.sampled_pairs.size()));
	for (const auto& p: s.sampled_pairs) {
		w.U32(p.image);
		w.U32(p.sampler);
		w.U32(p.first_use_pc);
	}
	w.U32(static_cast<uint32_t>(s.inputs.size()));
	for (const auto& in: s.inputs) {
		w.Enum(in.kind);
		w.U32(in.location);
		w.U32(in.component_count);
		w.String(in.debug_name);
		w.Bool(in.per_vertex);
	}
	w.U32(static_cast<uint32_t>(s.outputs.size()));
	for (const auto& o: s.outputs) {
		w.Enum(o.kind);
		w.U32(o.index);
		w.U32(o.location);
		w.String(o.debug_name);
	}
	w.Bytes(s.vertex_fetch_components.data(), s.vertex_fetch_components.size());
	w.I32(s.vertex_offset_sgpr);
	w.I32(s.instance_offset_sgpr);
	w.Bool(s.has_bitwise_xor);
	w.Bool(s.uses_dma);
	w.Bool(s.watchdog_reports);

}

static bool ReadShaderInfo(Reader& r, IR::ShaderInfo& s) {
	uint32_t count = 0;
	if (!r.Count(count)) return false;
	s.buffers.resize(count);
	for (auto& b: s.buffers) {
		b.source             = r.U32();
		b.first_use_pc       = r.U32();
		b.max_byte_extent    = r.U32();
		b.packed_stride      = r.U32();
		b.descriptor_format  = r.Enum<Prospero::BufferFormat>();
		b.descriptor_swizzle = r.U32();
		b.image_alias        = r.U32();
		b.read               = r.Bool();
		b.written            = r.Bool();
		b.atomic             = r.Bool();
		b.formatted          = r.Bool();
		b.scalar             = r.Bool();
		b.gpu_records        = r.Bool();
	}
	if (!r.Count(count)) return false;
	s.images.resize(count);
	for (auto& i: s.images) {
		i.source                     = r.U32();
		i.first_use_pc               = r.U32();
		i.resource_class             = r.Enum<IR::ImageResourceClass>();
		i.numeric_class              = r.Enum<Prospero::TextureNumericClass>();
		i.dimension                  = r.Enum<ShaderRecompiler::Decoder::ImageDimension>();
		i.mip_mode                   = r.Enum<IR::ImageMipMode>();
		i.mip_count                  = r.U32();
		i.conversion_format          = r.Enum<Prospero::BufferFormat>();
		i.shader_swizzle             = r.U32();
		i.read                       = r.Bool();
		i.written                    = r.Bool();
		i.atomic                     = r.Bool();
		i.atomic64                   = r.Bool();
		i.depth_compare              = r.Bool();
		i.cube                       = r.Bool();
		i.r128                       = r.Bool();
		i.indirect_root              = r.U32();
		i.indirect_mapping_offset    = r.U32();
		i.indirect_search_iterations = r.U32();
		i.bindless                   = r.Bool();
		r.Words(i.indirect_resources);
	}
	if (!r.Count(count)) return false;
	s.samplers.resize(count);
	for (auto& sm: s.samplers) {
		sm.source                  = r.U32();
		sm.first_use_pc            = r.U32();
		sm.force_point_filtering   = r.Bool();
		sm.depth_compare           = r.Bool();
		sm.integer_border          = r.Bool();
		sm.bindless                = r.Bool();
		sm.bindless_mapping_offset = r.U32();
		sm.gather_lod              = r.Bool();
	}
	if (!r.Count(count)) return false;
	s.sampled_pairs.resize(count);
	for (auto& p: s.sampled_pairs) {
		p.image        = r.U32();
		p.sampler      = r.U32();
		p.first_use_pc = r.U32();
	}
	if (!r.Count(count)) return false;
	s.inputs.resize(count);
	for (auto& input: s.inputs) {
		input.kind            = r.Enum<IR::StageInputKind>();
		input.location        = r.U32();
		input.component_count = r.U32();
		r.String(input.debug_name);
		input.per_vertex = r.Bool();
	}
	if (!r.Count(count)) return false;
	s.outputs.resize(count);
	for (auto& output: s.outputs) {
		output.kind     = r.Enum<IR::StageOutputKind>();
		output.index    = r.U32();
		output.location = r.U32();
		r.String(output.debug_name);
	}
	r.Bytes(s.vertex_fetch_components.data(), s.vertex_fetch_components.size());
	s.vertex_offset_sgpr   = r.I32();
	s.instance_offset_sgpr = r.I32();
	s.has_bitwise_xor      = r.Bool();
	s.uses_dma             = r.Bool();
	s.watchdog_reports     = r.Bool();

	return r.Ok();
}

void SerializeInfo(const IR::CompiledShaderInfo& info, std::vector<uint8_t>& out) {
	Writer w(out);
	w.Enum(info.stage);
	w.U64(info.shader_hash);
	w.U32(info.wave_size);
	w.U32(info.user_data_base);
	w.U32(info.user_data_count);
	w.U32(info.scratch_dwords);
	w.U32(info.param_export_mask);
	w.Bool(info.has_address_writes);

	WriteShaderInfo(w, info.info);

	const auto& l = info.bindings;
	w.U32(l.push_data_start_dword);
	w.U32(l.memory_offset_dword);
	w.U32(l.memory_offset_count);
	w.Bool(l.has_dispatch_dimensions);
	w.Words(l.user_data_registers);
	w.U32(static_cast<uint32_t>(l.descriptors.size()));
	for (const auto& d: l.descriptors) {
		w.Enum(d.kind);
		w.Words(d.resources);
	}
}

bool DeserializeInfo(std::span<const uint8_t>& in, IR::CompiledShaderInfo& info) {
	Reader r(in);
	info                    = {};
	info.stage              = r.Enum<ShaderType>();
	info.shader_hash        = r.U64();
	info.wave_size          = r.U32();
	info.user_data_base     = r.U32();
	info.user_data_count    = r.U32();
	info.scratch_dwords     = r.U32();
	info.param_export_mask  = r.U32();
	info.has_address_writes = r.Bool();

	if (!ReadShaderInfo(r, info.info)) return false;
	uint32_t count = 0;

	auto& l                   = info.bindings;
	l.push_data_start_dword   = r.U32();
	l.memory_offset_dword     = r.U32();
	l.memory_offset_count     = r.U32();
	l.has_dispatch_dimensions = r.Bool();
	r.Words(l.user_data_registers);
	if (!r.Count(count)) return false;
	l.descriptors.resize(count);
	for (auto& d: l.descriptors) {
		d.kind = r.Enum<IR::DescriptorBindingKind>();
		r.Words(d.resources);
	}
	return r.Ok();
}

// ResourcePlan <-> bytes. The plan owns its value graph (ExtractResourcePlan clones every value
// into value_storage, phis without blocks), so instructions are stored by their position there:
// first opcode, flags and operand count of each, then the operands, which handles cycles through
// phis. A plan whose values reach outside value_storage, or whose phis name blocks, is not stored.
namespace {

static_assert(std::is_trivially_copyable_v<IR::MemoryInfo>);
static_assert(std::is_trivially_copyable_v<IR::UniformFill>);

constexpr uint32_t PlanFormatVersion = 1;

// Opcodes this build knows; a record naming another is rejected.
constexpr uint32_t ValueOpcodeCount = static_cast<uint32_t>(IR::ValueOpcode::Count);

class PlanWriter {
public:
	PlanWriter(const IR::ResourcePlan& plan, std::vector<uint8_t>& out): m_w(out) {
		uint32_t index = 0;
		for (const auto& inst: plan.value_storage) {
			m_index.emplace(&inst, index++);
		}
	}
	Writer& W() { return m_w; }
	[[nodiscard]] bool Ok() const noexcept { return m_ok; }

	void Value(const IR::Value& value) {
		const auto type = value.RawType();
		m_w.U32(static_cast<uint32_t>(type));
		if (type == IR::Type::Opaque) {
			const auto found = m_index.find(value.TryInstruction());
			if (found == m_index.end()) {
				m_ok = false;
				m_w.U32(0);
				return;
			}
			m_w.U32(found->second);
			return;
		}
		m_w.U64(value.RawBits());
	}
	void Fail() noexcept { m_ok = false; }

private:
	Writer                                        m_w;
	std::unordered_map<const IR::Inst*, uint32_t> m_index;
	bool                                          m_ok = true;
};

class PlanReader {
public:
	explicit PlanReader(std::span<const uint8_t>& in): m_r(in) {}
	Reader& R() { return m_r; }
	std::vector<IR::Inst*>& Insts() { return m_insts; }
	[[nodiscard]] bool Ok() const noexcept { return m_ok && m_r.Ok(); }
	void               Fail() noexcept { m_ok = false; }

	IR::Value Value() {
		const auto type = static_cast<IR::Type>(m_r.U32());
		if (type == IR::Type::Opaque) {
			const auto index = m_r.U32();
			if (index >= m_insts.size()) {
				m_ok = false;
				return {};
			}
			return IR::Value(m_insts[index]);
		}
		return IR::Value::FromRaw(type, m_r.U64());
	}

private:
	Reader                 m_r;
	std::vector<IR::Inst*> m_insts;
	bool                   m_ok = true;
};

} // namespace

bool SerializePlan(const IR::ResourcePlan& plan, std::vector<uint8_t>& out) {
	out.clear();
	PlanWriter p(plan, out);
	auto&      w = p.W();
	w.U32(PlanFormatVersion);
	w.Enum(plan.stage);
	w.U64(plan.shader_hash);
	w.U32(plan.user_data_base);
	w.U32(plan.user_data_count);
	// Instructions, then their operands.
	w.U32(static_cast<uint32_t>(plan.value_storage.size()));
	for (const auto& inst: plan.value_storage) {
		w.Enum(inst.GetOpcode());
		w.U64(inst.Flags<uint64_t>());
		const bool phi = inst.GetOpcode() == IR::ValueOpcode::Phi;
		w.U32(static_cast<uint32_t>(phi ? inst.NumPhiBlocks() : inst.NumArgs()));
	}
	for (const auto& inst: plan.value_storage) {
		const bool phi = inst.GetOpcode() == IR::ValueOpcode::Phi;
		const auto count = phi ? inst.NumPhiBlocks() : inst.NumArgs();
		for (size_t i = 0; i < count; i++) {
			if (phi && inst.PhiBlock(i) != nullptr) {
				p.Fail();
			}
			p.Value(inst.Arg(i));
		}
	}
	w.U32(static_cast<uint32_t>(plan.memory_info.size()));
	w.Bytes(plan.memory_info.data(), plan.memory_info.size() * sizeof(IR::MemoryInfo));
	w.U32(static_cast<uint32_t>(plan.descriptor_sources.size()));
	for (const auto& source: plan.descriptor_sources) {
		for (const auto& dword: source.dwords) {
			p.Value(dword);
		}
		w.U32(source.dword_count);
		w.Bool(source.indirect_image.has_value());
		if (source.indirect_image.has_value()) {
			const auto& i = *source.indirect_image;
			w.U32(i.material_source);
			w.U32(i.table_source);
			w.U32(i.selector_stride);
			w.U32(i.selector_offset);
			w.U32(i.table_offset);
			w.U32(i.key_shift);
			w.U32(i.key_mask);
			w.Bool(i.bindless);
			p.Value(i.key_count);
			p.Value(i.selector_mask);
			w.Words(i.sources);
		}
		w.Bool(source.bindless_sampler.has_value());
		if (source.bindless_sampler.has_value()) {
			w.U32(source.bindless_sampler->table_offset);
		}
	}
	w.U32(static_cast<uint32_t>(plan.control_flow.size()));
	for (const auto& block: plan.control_flow) {
		p.Value(block.condition);
		w.Words(block.successors);
		w.Words(block.sources);
		w.Words(block.srt_reads);
	}
	w.U32(static_cast<uint32_t>(plan.srt_reads.size()));
	for (const auto& read: plan.srt_reads) {
		p.Value(read.value);
		w.U32(read.flat_offset);
	}
	w.U32(static_cast<uint32_t>(plan.buffer_writes_bounded.size()));
	w.Bytes(plan.buffer_writes_bounded.data(), plan.buffer_writes_bounded.size());
	w.U32(static_cast<uint32_t>(plan.buffer_writes.size()));
	for (const auto& write: plan.buffer_writes) {
		w.U32(write.buffer);
		w.U32(write.immediate);
		p.Value(write.index);
		p.Value(write.offset);
		p.Value(write.soffset);
		p.Value(write.predicate);
	}
	w.U32(static_cast<uint32_t>(plan.clean_flat_slots.size()));
	w.Bytes(plan.clean_flat_slots.data(), plan.clean_flat_slots.size());
	w.Bool(plan.requires_specialization_memory);
	w.Bool(plan.capture_specialization_reads);
	w.Bool(plan.descriptor_phi_under_writes);
	w.Bool(plan.has_uniform_buffer_reads);
	w.Bool(plan.bindless_images);
	w.Bool(plan.srt_plan_complete);
	w.Bool(plan.resource_tracking_complete);
	WriteShaderInfo(w, plan.info);
	w.Bytes(&plan.uniform_fill.fill, sizeof(plan.uniform_fill.fill));
	for (const auto& value: plan.uniform_fill.values) {
		p.Value(value);
	}
	return p.Ok();
}

bool DeserializePlan(std::span<const uint8_t> in, IR::ResourcePlan& plan) {
	plan = IR::ResourcePlan {};
	PlanReader p(in);
	auto&      r = p.R();
	if (r.U32() != PlanFormatVersion) {
		return false;
	}
	plan.stage           = r.Enum<ShaderType>();
	plan.shader_hash     = r.U64();
	plan.user_data_base  = r.U32();
	plan.user_data_count = r.U32();
	uint32_t count       = 0;
	if (!r.Count(count)) return false;
	std::vector<uint32_t> operand_counts(count);
	for (uint32_t i = 0; i < count && r.Ok(); i++) {
		const auto opcode_value = r.U32();
		const auto flags        = r.U64();
		operand_counts[i]       = r.U32();
		if (opcode_value >= ValueOpcodeCount || operand_counts[i] > 0xffffu) {
			return false;
		}
		const auto opcode = static_cast<IR::ValueOpcode>(opcode_value);
		auto& inst        = plan.value_storage.emplace_back(opcode, flags);
		if (opcode != IR::ValueOpcode::Phi && inst.NumArgs() != operand_counts[i]) {
			return false; // another opcode table
		}
		p.Insts().push_back(&inst);
	}
	for (uint32_t i = 0; i < count && p.Ok(); i++) {
		auto*      inst = p.Insts()[i];
		const bool phi  = inst->GetOpcode() == IR::ValueOpcode::Phi;
		for (uint32_t arg = 0; arg < operand_counts[i] && p.Ok(); arg++) {
			const auto value = p.Value();
			if (phi) {
				inst->AddPhiOperand(nullptr, value);
			} else {
				inst->SetArg(arg, value);
			}
		}
	}
	if (!p.Ok() || !r.Count(count)) return false;
	plan.memory_info.resize(count);
	if (!r.Bytes(plan.memory_info.data(), count * sizeof(IR::MemoryInfo)) || !r.Count(count)) {
		return false;
	}
	plan.descriptor_sources.resize(count);
	for (auto& source: plan.descriptor_sources) {
		for (auto& dword: source.dwords) {
			dword = p.Value();
		}
		source.dword_count = r.U32();
		if (r.Bool()) {
			IR::DescriptorSource::IndirectImage i {};
			i.material_source = r.U32();
			i.table_source    = r.U32();
			i.selector_stride = r.U32();
			i.selector_offset = r.U32();
			i.table_offset    = r.U32();
			i.key_shift       = r.U32();
			i.key_mask        = r.U32();
			i.bindless        = r.Bool();
			i.key_count       = p.Value();
			i.selector_mask   = p.Value();
			r.Words(i.sources);
			source.indirect_image = std::move(i);
		}
		if (r.Bool()) {
			source.bindless_sampler = IR::DescriptorSource::BindlessSampler {.table_offset = r.U32()};
		}
		if (!p.Ok()) return false;
	}
	if (!r.Count(count)) return false;
	plan.control_flow.resize(count);
	for (auto& block: plan.control_flow) {
		block.condition = p.Value();
		r.Words(block.successors);
		r.Words(block.sources);
		r.Words(block.srt_reads);
	}
	if (!p.Ok() || !r.Count(count)) return false;
	plan.srt_reads.resize(count);
	for (auto& read: plan.srt_reads) {
		read.value       = p.Value();
		read.flat_offset = r.U32();
	}
	count = r.U32();
	if (count > in.size()) return false;
	plan.buffer_writes_bounded.resize(count);
	if (!r.Bytes(plan.buffer_writes_bounded.data(), count) || !r.Count(count)) return false;
	plan.buffer_writes.resize(count);
	for (auto& write: plan.buffer_writes) {
		write.buffer    = r.U32();
		write.immediate = r.U32();
		write.index     = p.Value();
		write.offset    = p.Value();
		write.soffset   = p.Value();
		write.predicate = p.Value();
	}
	count = r.U32();
	if (count > in.size()) return false;
	plan.clean_flat_slots.resize(count);
	if (!r.Bytes(plan.clean_flat_slots.data(), count)) return false;
	plan.requires_specialization_memory = r.Bool();
	plan.capture_specialization_reads   = r.Bool();
	plan.descriptor_phi_under_writes    = r.Bool();
	plan.has_uniform_buffer_reads       = r.Bool();
	plan.bindless_images                = r.Bool();
	plan.srt_plan_complete              = r.Bool();
	plan.resource_tracking_complete     = r.Bool();
	if (!ReadShaderInfo(r, plan.info)) return false;
	r.Bytes(&plan.uniform_fill.fill, sizeof(plan.uniform_fill.fill));
	for (auto& value: plan.uniform_fill.values) {
		value = p.Value();
	}
	return p.Ok() && in.empty();
}

bool SameInfo(const IR::CompiledShaderInfo& a, const IR::CompiledShaderInfo& b) {
	return a.stage == b.stage && a.shader_hash == b.shader_hash && a.wave_size == b.wave_size &&
	       a.user_data_base == b.user_data_base && a.user_data_count == b.user_data_count &&
	       a.scratch_dwords == b.scratch_dwords && a.param_export_mask == b.param_export_mask &&
	       a.has_address_writes == b.has_address_writes && a.info == b.info &&
	       a.bindings == b.bindings;
}

void MakePayload(std::span<const uint32_t> spirv, const IR::CompiledShaderInfo& info,
                 std::vector<uint8_t>& out) {
	out.clear();
	Writer w(out);
	w.U32(static_cast<uint32_t>(spirv.size()));
	w.Bytes(spirv.data(), spirv.size_bytes());
	SerializeInfo(info, out);
}

bool ReadPayload(std::span<const uint8_t> payload, std::vector<uint32_t>& spirv,
                 IR::CompiledShaderInfo& info) {
	Reader r(payload);
	r.Words(spirv);
	if (!r.Ok() || spirv.empty()) {
		return false;
	}
	return DeserializeInfo(payload, info) && payload.empty();
}

size_t Store::Open(const std::filesystem::path& path, std::string_view signature) {
	std::lock_guard lock(m_mutex);
	m_path      = path;
	m_signature = signature;
	m_enabled.store(true, std::memory_order_release);
	m_records.clear();
	m_header_valid = false;
	m_valid_bytes  = 0;
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return 0;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::span<const uint8_t> rest(data);
	if (rest.size() < sizeof(Magic) + sizeof(uint32_t) ||
	    std::memcmp(rest.data(), Magic, sizeof(Magic)) != 0) {
		return 0;
	}
	rest = rest.subspan(sizeof(Magic));
	uint32_t signature_size = 0;
	std::memcpy(&signature_size, rest.data(), sizeof(signature_size));
	rest = rest.subspan(sizeof(signature_size));
	if (signature_size > MaxSignature || signature_size > rest.size() ||
	    std::string_view(reinterpret_cast<const char*>(rest.data()), signature_size) != signature) {
		return 0;
	}
	rest           = rest.subspan(signature_size);
	m_header_valid = true;
	m_valid_bytes  = data.size() - rest.size();
	while (rest.size() >= RecordHeader) {
		Key      key;
		uint32_t size = 0;
		uint64_t hash = 0;
		std::memcpy(&key.lo, rest.data(), 8);
		std::memcpy(&key.hi, rest.data() + 8, 8);
		std::memcpy(&size, rest.data() + 16, 4);
		std::memcpy(&hash, rest.data() + 20, 8);
		if (size > MaxPayload || size > rest.size() - RecordHeader) {
			break;
		}
		const auto payload = rest.subspan(RecordHeader, size);
		if (XXH3_64bits(payload.data(), payload.size()) != hash) {
			break;
		}
		m_records[key].assign(payload.begin(), payload.end());
		rest = rest.subspan(RecordHeader + size);
		m_valid_bytes += RecordHeader + size;
	}
	return m_records.size();
}

bool Store::Get(const Key& key, std::vector<uint8_t>& payload) const {
	std::lock_guard lock(m_mutex);
	const auto      found = m_records.find(key);
	if (found == m_records.end()) {
		return false;
	}
	payload = found->second;
	return true;
}

bool Store::Contains(const Key& key) const {
	std::lock_guard lock(m_mutex);
	return m_records.contains(key);
}

size_t Store::Size() const {
	std::lock_guard lock(m_mutex);
	return m_records.size();
}

bool Store::WriteHeaderLocked() {
	std::error_code error;
	if (m_path.has_parent_path()) {
		std::filesystem::create_directories(m_path.parent_path(), error);
	}
	if (m_header_valid) {
		// Drop a corrupt or truncated tail before appending after the valid records.
		std::filesystem::resize_file(m_path, m_valid_bytes, error);
		m_out.open(m_path, std::ios::binary | std::ios::app);
		return static_cast<bool>(m_out);
	}
	m_out.open(m_path, std::ios::binary | std::ios::trunc);
	if (!m_out) {
		return false;
	}
	const auto size = static_cast<uint32_t>(m_signature.size());
	m_out.write(Magic, sizeof(Magic));
	m_out.write(reinterpret_cast<const char*>(&size), sizeof(size));
	m_out.write(m_signature.data(), static_cast<std::streamsize>(m_signature.size()));
	m_out.flush();
	m_header_valid = static_cast<bool>(m_out);
	return m_header_valid;
}

void Store::Put(const Key& key, std::span<const uint8_t> payload) {
	std::lock_guard lock(m_mutex);
	if (!m_enabled.load(std::memory_order_relaxed) || payload.size() > MaxPayload ||
	    !m_records.try_emplace(key, payload.begin(), payload.end()).second) {
		return;
	}
	if (!m_out.is_open() && !WriteHeaderLocked()) {
		m_enabled.store(false, std::memory_order_release);
		return;
	}
	const auto size = static_cast<uint32_t>(payload.size());
	const auto hash = static_cast<uint64_t>(XXH3_64bits(payload.data(), payload.size()));
	m_out.write(reinterpret_cast<const char*>(&key.lo), 8);
	m_out.write(reinterpret_cast<const char*>(&key.hi), 8);
	m_out.write(reinterpret_cast<const char*>(&size), 4);
	m_out.write(reinterpret_cast<const char*>(&hash), 8);
	m_out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(size));
	m_out.flush();
	if (!m_out) {
		m_enabled.store(false, std::memory_order_release);
	}
}

int Mode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_PROGRAM_DISK_CACHE");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	return mode;
}

bool AffectsTranslation(std::string_view name) {
	// Read under src/graphics/shader (2026-10-04); a new compile-time switch there should be
	// added, though the word list below catches most.
	static constexpr std::string_view Known[] = {
	    "KYTY_BINDLESS_SAMPLERS",          "KYTY_BINDLESS_SAMPLERS_HASH_BITS",
	    "KYTY_BINDLESS_SAMPLERS_HASH_VALUE", "KYTY_BINDLESS_SAMPLERS_SKIP",
	    "KYTY_BINDLESS_SAMPLER_CLASS_DEFAULTS", "KYTY_BINDLESS_SAMPLER_COPIES",
	    "KYTY_BITWISE_FLOAT_MINMAX",       "KYTY_CLAMP_SAMPLER_SHADERS",
	    "KYTY_DEFAULT_SAMPLER_CLAMP",      "KYTY_FOLD_LANE_MASKS",
	    "KYTY_KEY_SHIFT_AND14",            "KYTY_KEY_SHIFT_HIGH",
	    "KYTY_MESH_CULL",                  "KYTY_NO_FLOAT_CONTROLS",
	    "KYTY_SKIP_BVH_DISPATCHES",        "KYTY_VALIDATE_IR",
	    "KYTY_WATCH_EXTENTS",
	};
	for (const auto known: Known) {
		if (name == known) {
			return true;
		}
	}
	// Host-side or scheduling switches whose names match the words below.
	static constexpr std::string_view Runtime[] = {
	    "KYTY_ASYNC_SHADERS",            "KYTY_ASYNC_SHADER_THREADS",
	    "KYTY_RECOMPILER_LOCK",          "KYTY_SKIP_SHADER_HASHES",
	    "KYTY_BINDLESS_SAMPLER_FIELDS",  "KYTY_BINDLESS_SAMPLER_RECHECK",
	    "KYTY_BINDLESS_SAMPLER_RECYCLE", "KYTY_BINDLESS_SAMPLER_SLOTS_OFF",
	    "KYTY_BINDLESS_SAMPLER_KEY_SHIFT", "KYTY_BINDLESS_SAMPLER_SHADERS",
	};
	for (const auto runtime: Runtime) {
		if (name == runtime) {
			return false;
		}
	}
	// Logging and the cache's own settings never change the output.
	if (name.starts_with("KYTY_PROGRAM_DISK_CACHE") || name.find("_LOG") != std::string_view::npos ||
	    name.find("_DUMP") != std::string_view::npos || name.find("STATS") != std::string_view::npos ||
	    name == "KYTY_LIVE_FILE") {
		return false;
	}
	static constexpr std::string_view Words[] = {"SHADER", "SAMPLER", "SPIRV", "_IR_", "RECOMPILER",
	                                             "TRANSLAT", "FOLD", "FLOAT", "MESH", "TESS"};
	for (const auto word: Words) {
		if (name.find(word) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

std::string EnvironmentSignature() {
#if defined(_WIN32)
	char** env = _environ;
#else
	char** env = environ;
#endif
	std::vector<std::string> entries;
	for (; env != nullptr && *env != nullptr; ++env) {
		if (std::strncmp(*env, "KYTY_", 5) == 0) {
			entries.emplace_back(*env);
		}
	}
	std::ranges::sort(entries);
	std::string result;
	for (const auto& entry: entries) {
		result += entry;
		result += '\n';
	}
	return result;
}

} // namespace Libs::Graphics::ProgramDiskCache
