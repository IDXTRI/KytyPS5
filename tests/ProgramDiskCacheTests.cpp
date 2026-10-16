#include "graphics/host_gpu/renderer/pipeline/programDiskCache.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

namespace Disk = Libs::Graphics::ProgramDiskCache;
namespace IR   = Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ProgramDiskCacheTests: failed: %s\n", message);
		std::abort();
	}
}

IR::CompiledShaderInfo SampleInfo() {
	IR::CompiledShaderInfo info;
	info.stage              = Libs::Graphics::ShaderType::Pixel;
	info.shader_hash        = 0x59bdf02950d06cb6ull;
	info.wave_size          = 32;
	info.user_data_base     = 8;
	info.user_data_count    = 17;
	info.scratch_dwords     = 3;
	info.param_export_mask  = 0x5;
	info.has_address_writes = true;
	IR::BufferResource buffer;
	buffer.source          = 2;
	buffer.first_use_pc    = 0x40;
	buffer.max_byte_extent = 4096;
	buffer.packed_stride   = 16;
	buffer.image_alias     = 1;
	buffer.read = buffer.written = buffer.atomic = buffer.formatted = buffer.scalar = true;
	buffer.gpu_records = true;
	info.info.buffers.push_back(buffer);
	IR::ImageResource image;
	image.source             = 3;
	image.first_use_pc       = 0x80;
	image.resource_class     = IR::ImageResourceClass::Sampled;
	image.mip_mode           = IR::ImageMipMode::Dynamic;
	image.mip_count          = 9;
	image.shader_swizzle     = 0x123;
	image.read = image.depth_compare = image.cube = image.r128 = image.bindless = true;
	image.indirect_root      = 0;
	image.indirect_mapping_offset    = 12;
	image.indirect_search_iterations = 4;
	image.indirect_resources = {0, 1, 5};
	info.info.images.push_back(image);
	info.info.images.push_back(IR::ImageResource {});
	IR::SamplerResource sampler;
	sampler.source                  = 4;
	sampler.force_point_filtering   = true;
	sampler.integer_border          = true;
	sampler.bindless                = true;
	sampler.bindless_mapping_offset = 30;
	sampler.gather_lod              = true;
	info.info.samplers.push_back(sampler);
	info.info.sampled_pairs.push_back({1, 0, 0x90});
	IR::StageInput input;
	input.location        = 2;
	input.component_count = 4;
	input.debug_name      = "attr2";
	input.per_vertex      = true;
	info.info.inputs.push_back(input);
	IR::StageOutput output;
	output.index      = 1;
	output.location   = 3;
	output.debug_name = "color1";
	info.info.outputs.push_back(output);
	info.info.vertex_fetch_components[5] = 3;
	info.info.vertex_offset_sgpr         = 7;
	info.info.instance_offset_sgpr       = 9;
	info.info.has_bitwise_xor = info.info.uses_dma = info.info.watchdog_reports = true;
	info.bindings.push_data_start_dword   = 4;
	info.bindings.memory_offset_dword     = 6;
	info.bindings.memory_offset_count     = 2;
	info.bindings.has_dispatch_dimensions = true;
	info.bindings.user_data_registers     = {0, 1, 2};
	info.bindings.descriptors.push_back({IR::DescriptorBindingKind::Samplers, {0, 3}});
	return info;
}

void TestInfoRoundTrip() {
	const auto           info = SampleInfo();
	std::vector<uint8_t> bytes;
	Disk::SerializeInfo(info, bytes);
	std::span<const uint8_t> in(bytes);
	IR::CompiledShaderInfo   back;
	Check(Disk::DeserializeInfo(in, back), "deserialize failed");
	Check(in.empty(), "deserialize left bytes");
	Check(Disk::SameInfo(info, back), "round trip changed the info");
	for (size_t cut = 0; cut < bytes.size(); cut += 7) {
		std::span<const uint8_t> truncated(bytes.data(), cut);
		IR::CompiledShaderInfo   partial;
		Check(!Disk::DeserializeInfo(truncated, partial), "a truncated info was accepted");
	}
}

void TestPayloadRoundTrip() {
	const auto                  info  = SampleInfo();
	const std::vector<uint32_t> spirv = {0x07230203u, 0x00010600u, 1, 2, 3};
	std::vector<uint8_t>        payload;
	Disk::MakePayload(spirv, info, payload);
	std::vector<uint32_t>  spirv_back;
	IR::CompiledShaderInfo info_back;
	Check(Disk::ReadPayload(payload, spirv_back, info_back), "payload read failed");
	Check(spirv_back == spirv && Disk::SameInfo(info, info_back), "payload round trip");
	payload.push_back(0);
	Check(!Disk::ReadPayload(payload, spirv_back, info_back), "trailing bytes accepted");
}

void TestKeys() {
	const auto make = [](uint32_t push_start, uint32_t stride) {
		Disk::KeyBuilder                   key;
		IR::ResourceSpecialization         specialization;
		specialization.buffers.push_back({.packed_stride = stride});
		key.U32(1);
		key.Words(std::vector<uint32_t> {1, 2, 3});
		key.Specialization(specialization);
		key.U32(push_start);
		return key.Finish();
	};
	Check(make(0, 16) == make(0, 16), "key not deterministic");
	Check(!(make(0, 16) == make(4, 16)), "push start not in the key");
	Check(!(make(0, 16) == make(0, 32)), "specialization not in the key");
}

// A plan with a cycle through a phi, values in every container, and immediates of each kind.
void BuildPlan(IR::ResourcePlan& plan, IR::Inst*& select_out, IR::Inst*& phi_out) {
	plan.stage           = Libs::Graphics::ShaderType::Pixel;
	plan.shader_hash     = 0x1234;
	plan.user_data_base  = 8;
	plan.user_data_count = 12;
	auto& undef = plan.value_storage.emplace_back(IR::ValueOpcode::UndefU1);
	auto& not_  = plan.value_storage.emplace_back(IR::ValueOpcode::LogicalNot);
	not_.SetArg(0, IR::Value(&undef));
	auto& and_ = plan.value_storage.emplace_back(IR::ValueOpcode::LogicalAnd);
	and_.SetArg(0, IR::Value(&undef));
	and_.SetArg(1, IR::Value(&not_));
	auto& phi = plan.value_storage.emplace_back(IR::ValueOpcode::Phi);
	phi.SetFlags(IR::Type::U32);
	phi.AddPhiOperand(nullptr, IR::Value(5u));
	phi.AddPhiOperand(nullptr, IR::Value(&phi));
	auto& select = plan.value_storage.emplace_back(IR::ValueOpcode::SelectU32);
	select.SetArg(0, IR::Value(&and_));
	select.SetArg(1, IR::Value::F32(1.5f));
	select.SetArg(2, IR::Value(&phi));
	auto& shift = plan.value_storage.emplace_back(IR::ValueOpcode::ShiftRightLogical32);
	shift.SetArg(0, IR::Value(&select));
	shift.SetArg(1, IR::Value(uint32_t {16}));

	plan.memory_info.resize(2);
	plan.memory_info[0].kind   = IR::ResourceKind::Buffer;
	plan.memory_info[0].offset = 64;
	plan.memory_info[1].kind   = IR::ResourceKind::Image;
	plan.memory_info[1].planning_only = true;

	IR::DescriptorSource source;
	source.dwords[0]   = IR::Value(&shift);
	source.dwords[1]   = IR::Value(uint64_t {0x1122334455667788ull});
	source.dword_count = 2;
	IR::DescriptorSource::IndirectImage image {};
	image.table_offset  = 32;
	image.bindless      = true;
	image.key_count     = IR::Value(&phi);
	image.sources       = {1, 2};
	source.indirect_image   = image;
	source.bindless_sampler = IR::DescriptorSource::BindlessSampler {.table_offset = 48};
	plan.descriptor_sources.push_back(source);
	plan.descriptor_sources.emplace_back();

	plan.control_flow.push_back({IR::Value(&and_), {1}, {0}, {}});
	plan.srt_reads.push_back({IR::Value(&select), 12});
	plan.buffer_writes_bounded = {1, 0};
	plan.buffer_writes.push_back({0, 4, IR::Value(&select), IR::Value(3u), {}, IR::Value(true)});
	plan.clean_flat_slots              = {0, 1, 1};
	plan.requires_specialization_memory = true;
	plan.resource_tracking_complete     = true;
	plan.info.buffers.push_back({.source = 0, .max_byte_extent = 128, .read = true});
	plan.uniform_fill.fill.kind  = IR::UniformFillKind::Buffer;
	plan.uniform_fill.fill.words = 2;
	plan.uniform_fill.values[0]  = IR::Value(&select);
	select_out                   = &select;
	phi_out                      = &phi;
}

size_t IndexOf(const IR::ResourcePlan& plan, const IR::Inst* inst) {
	size_t index = 0;
	for (const auto& candidate: plan.value_storage) {
		if (&candidate == inst) {
			return index;
		}
		index++;
	}
	return SIZE_MAX;
}

void TestPlanRoundTrip() {
	IR::ResourcePlan plan;
	IR::Inst*        select = nullptr;
	IR::Inst*        phi    = nullptr;
	BuildPlan(plan, select, phi);
	std::vector<uint8_t> bytes;
	Check(Disk::SerializePlan(plan, bytes), "plan serialization refused");

	IR::ResourcePlan back;
	Check(Disk::DeserializePlan(bytes, back), "plan deserialization failed");
	std::vector<uint8_t> again;
	Check(Disk::SerializePlan(back, again) && again == bytes, "plan round trip changed bytes");

	Check(back.value_storage.size() == plan.value_storage.size(), "instruction count");
	auto it = back.value_storage.begin();
	for (const auto& inst: plan.value_storage) {
		Check(it->GetOpcode() == inst.GetOpcode() && it->Flags<uint64_t>() == inst.Flags<uint64_t>(),
		      "instruction opcode or flags");
		++it;
	}
	const auto& back_phi = *std::next(back.value_storage.begin(), 3);
	Check(back_phi.NumPhiBlocks() == 2 && back_phi.Arg(0) == IR::Value(5u) &&
	          back_phi.Arg(1).TryInstruction() == &back_phi,
	      "phi cycle not restored");
	const auto& source = back.descriptor_sources[0];
	Check(IndexOf(back, source.dwords[0].TryInstruction()) == 5 &&
	          source.dwords[1] == IR::Value(uint64_t {0x1122334455667788ull}) &&
	          source.dword_count == 2,
	      "descriptor dwords");
	Check(source.indirect_image.has_value() && source.indirect_image->bindless &&
	          source.indirect_image->key_count.TryInstruction() == &back_phi &&
	          source.indirect_image->selector_mask.IsEmpty() &&
	          source.indirect_image->sources == std::vector<uint32_t> {1, 2},
	      "indirect image");
	Check(source.bindless_sampler.has_value() && source.bindless_sampler->table_offset == 48,
	      "bindless sampler");
	Check(!back.descriptor_sources[1].indirect_image.has_value(), "empty descriptor source");
	const auto* back_select = &*std::next(back.value_storage.begin(), 4);
	Check(back_select != nullptr && back_select->Arg(1) == IR::Value::F32(1.5f) &&
	          back_select->Arg(2).TryInstruction() == &back_phi,
	      "select operands");
	Check(back.memory_info == plan.memory_info, "memory info");
	Check(back.control_flow.size() == 1 && back.control_flow[0].successors == std::vector<uint32_t> {1},
	      "control flow");
	Check(back.srt_reads.size() == 1 && back.srt_reads[0].flat_offset == 12 &&
	          back.srt_reads[0].value.TryInstruction() == back_select,
	      "srt reads");
	Check(back.buffer_writes.size() == 1 && back.buffer_writes[0].predicate == IR::Value(true) &&
	          back.buffer_writes[0].soffset.IsEmpty(),
	      "buffer writes");
	Check(back.clean_flat_slots == plan.clean_flat_slots &&
	          back.buffer_writes_bounded == plan.buffer_writes_bounded,
	      "byte vectors");
	Check(back.requires_specialization_memory && back.resource_tracking_complete &&
	          !back.capture_specialization_reads,
	      "flags");
	Check(back.info == plan.info, "shader info");
	Check(back.uniform_fill.fill == plan.uniform_fill.fill &&
	          back.uniform_fill.values[0].TryInstruction() == back_select,
	      "uniform fill");
	(void)select;
	(void)phi;

	for (size_t cut = 0; cut + 1 < bytes.size(); cut += 5) {
		IR::ResourcePlan partial;
		Check(!Disk::DeserializePlan(std::span(bytes.data(), cut), partial),
		      "a truncated plan was accepted");
	}
}

void TestPlanRefusesForeignValues() {
	IR::ResourcePlan plan;
	IR::Inst         outside(IR::ValueOpcode::UndefU1);
	plan.srt_reads.push_back({IR::Value(&outside), 0});
	std::vector<uint8_t> bytes;
	Check(!Disk::SerializePlan(plan, bytes), "a value outside the plan was stored");
	plan.srt_reads.clear();
}

void TestStore(const std::filesystem::path& dir) {
	const auto path = dir / "programs.bin";
	std::filesystem::remove(path);
	const std::vector<uint8_t> a = {1, 2, 3, 4};
	const std::vector<uint8_t> b(1000, 7);
	{
		Disk::Store store;
		Check(store.Open(path, "sig-1") == 0, "fresh store not empty");
		store.Put({1, 2}, a);
		store.Put({3, 4}, b);
		store.Put({1, 2}, b); // a key is stored once
	}
	{
		Disk::Store store;
		Check(store.Open(path, "sig-1") == 2, "reload lost records");
		std::vector<uint8_t> payload;
		Check(store.Get({1, 2}, payload) && payload == a, "record a");
		Check(store.Get({3, 4}, payload) && payload == b, "record b");
		Check(!store.Get({5, 6}, payload), "unknown key found");
	}
	{
		Disk::Store store;
		Check(store.Open(path, "sig-2") == 0, "another signature loaded records");
	}
	// A torn last record: the valid ones load, the next Put drops the tail and appends.
	const auto size = std::filesystem::file_size(path);
	std::filesystem::resize_file(path, size - 10);
	{
		Disk::Store store;
		Check(store.Open(path, "sig-1") == 1, "torn tail not skipped");
		store.Put({7, 8}, a);
	}
	{
		Disk::Store          store;
		std::vector<uint8_t> payload;
		Check(store.Open(path, "sig-1") == 2, "append after a torn tail");
		Check(store.Get({7, 8}, payload) && payload == a, "record after the tail");
	}
	std::filesystem::remove(path);
}

} // namespace

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"

int main() {
	TestInfoRoundTrip();
	TestPayloadRoundTrip();
	TestKeys();
	Check(Disk::AffectsTranslation("KYTY_FOLD_LANE_MASKS") &&
	          Disk::AffectsTranslation("KYTY_BINDLESS_SAMPLERS") &&
	          !Disk::AffectsTranslation("KYTY_VRAM_REPORT") &&
	          !Disk::AffectsTranslation("KYTY_GC_HOST_SIZES") &&
	          !Disk::AffectsTranslation("KYTY_FAULT_SETTLE_MS") &&
	          !Disk::AffectsTranslation("KYTY_SHADER_STATS") &&
	          !Disk::AffectsTranslation("KYTY_PROGRAM_DISK_CACHE") &&
	          !Disk::AffectsTranslation("KYTY_ASYNC_SHADERS") &&
	          Disk::AffectsTranslation("KYTY_SOME_NEW_SHADER_SWITCH"),
	      "translation-affecting switch classification");
	TestPlanRoundTrip();
	TestPlanRefusesForeignValues();
	TestStore(std::filesystem::temp_directory_path());
	std::printf("ProgramDiskCacheTests: all passed\n");
	return 0;
}
