#include "graphics/host_gpu/renderer/pipeline/bindlessSamplerRecords.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

namespace Records = Libs::Graphics::BindlessSamplerRecords;
namespace IR      = Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "BindlessSamplerTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFieldGroupsPartitionEveryWord() {
	for (uint32_t word = 0; word < 4u; word++) {
		uint32_t all = 0;
		for (uint32_t a = 0; a < Records::FieldGroupCount; a++) {
			for (uint32_t b = a + 1u; b < Records::FieldGroupCount; b++) {
				Check((Records::FieldBits[a][word] & Records::FieldBits[b][word]) == 0u,
				      "two field groups share a bit");
			}
			all |= Records::FieldBits[a][word];
		}
		Check(all == 0xffffffffu, "a bit belongs to no field group");
	}
}

void TestMergeAllAndNone() {
	const Records::Record record {0x12345678u, 0x9abcdef0u, 0x0fedcba9u, 0x87654321u};
	const Records::Record fallback {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
	Check(Records::Merge(record, fallback, Records::FieldAll) == record,
	      "mask 0xff must keep the game's record (the default)");
	Check(Records::Merge(record, fallback, 0u) == fallback, "mask 0 must give the fallback");
}

void TestMergeAddressOnlyMatchesClampOnly() {
	// KYTY_BINDLESS_SAMPLERS_CLAMP_ONLY took CLAMP_X/Y/Z (bits 0..8) and nothing else.
	const Records::Record record {0x000001b6u | (1u << 12u) | (5u << 9u), 0x00fff100u,
	                              (2u << 20u) | (2u << 22u) | 0x40u, 0xc0000000u};
	const auto& fallback = IR::BindlessDefaultSampler;
	const auto  merged   = Records::Merge(record, fallback, Records::FieldAddress);
	Check((merged[0] & 0x1ffu) == 0x1b6u, "address modes not taken from the record");
	Check(((merged[0] >> 9u) & 7u) == 0u, "anisotropy leaked from the record");
	Check(((merged[0] >> 12u) & 7u) == 0u, "depth compare leaked from the record");
	Check(merged[1] == fallback[1] && merged[2] == fallback[2] && merged[3] == fallback[3],
	      "LOD, filters or border leaked from the record");
}

void TestMergeSingleGroups() {
	const Records::Record record {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
	const Records::Record zero {};
	const auto lod = Records::Merge(record, zero, Records::FieldLod);
	Check(lod[0] == 0u && lod[1] == 0xffffffffu && lod[2] == 0u && lod[3] == 0u,
	      "LOD group is not dword 1");
	const auto bias = Records::Merge(record, zero, Records::FieldLodBias);
	Check(bias[2] == 0xfffffu && bias[0] == 0u && bias[1] == 0u, "LOD bias group is wrong");
	const auto filter = Records::Merge(record, zero, Records::FieldFilter);
	Check(filter[2] == (0xffu << 20u), "filter group is wrong");
	const auto border = Records::Merge(record, zero, Records::FieldBorder);
	Check(border[3] == (0xfffu | (3u << 30u)), "border group is wrong");
	const auto depth = Records::Merge(record, zero, Records::FieldDepthCompare);
	Check(depth[0] == (7u << 12u), "depth compare group is wrong");
}

void TestRegionAllocatorBumpsAndFills() {
	Records::RegionAllocator regions;
	regions.Reset(1, 256);
	const auto always = [](uint64_t) { return true; };
	Check(regions.Allocate(128, false, always) == 1u, "first region must start after slot 0");
	Check(regions.Allocate(127, false, always) == 129u, "second region must follow the first");
	Check(regions.Allocate(1, false, always) == 0u, "a full array must refuse");
	Check(regions.Used() == 255u, "used slot count is wrong");
}

void TestRegionAllocatorRecyclesAfterTick() {
	Records::RegionAllocator regions;
	regions.Reset(8, 8 + 256);
	const auto never  = [](uint64_t) { return false; };
	const auto always = [](uint64_t) { return true; };
	const auto first  = regions.Allocate(128, true, always);
	const auto second = regions.Allocate(128, true, always);
	Check(first == 8u && second == 136u, "class-default layout must start at slot 8");
	regions.Release(first, 128, 42);
	Check(regions.Allocate(64, true, never) == 0u, "a region the GPU may read was reused");
	Check(regions.Allocate(64, false, always) == 0u, "reuse switched off must not recycle");
	const auto reused = regions.Allocate(64, true, [](uint64_t tick) { return tick <= 42u; });
	Check(reused == 8u, "a released region was not reused once free");
	const auto rest = regions.Allocate(64, true, always);
	Check(rest == 72u, "the split remainder of a released region was lost");
	Check(regions.FreeRegions() == 0u, "free list not emptied");
}

void TestSamplerKinds() {
	Check(IR::BindlessSamplerKind(false, false, false) == 0u, "float kind must be slot 0");
	Check(IR::BindlessSamplerKind(false, true, true) == 6u, "point-integer kind slot is wrong");
	Check(IR::BindlessSamplerKind(true, true, true) == IR::BindlessSamplerKinds - 1u,
	      "kinds do not fit the reserved slots");
}

void TestHashList() {
	const auto list = IR::ParseHashList("0x59bdf02950d06cb6,8d09da389bd41ee7,,");
	Check(list.size() == 2u && list.contains(0x59bdf02950d06cb6ull) &&
	          list.contains(0x8d09da389bd41ee7ull),
	      "hash list parse");
	Check(IR::ParseHashList(nullptr).empty(), "null hash list");
}

} // namespace

int main() {
	TestFieldGroupsPartitionEveryWord();
	TestMergeAllAndNone();
	TestMergeAddressOnlyMatchesClampOnly();
	TestMergeSingleGroups();
	TestRegionAllocatorBumpsAndFills();
	TestRegionAllocatorRecyclesAfterTick();
	TestSamplerKinds();
	TestHashList();
	std::printf("BindlessSamplerTests: all passed\n");
	return 0;
}
