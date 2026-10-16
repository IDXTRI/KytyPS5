#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSSAMPLERRECORDS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSSAMPLERRECORDS_H_

// Research helpers for the bindless sampler mirror (BindlessTable::MirrorSamplerHeap), kept free
// of Vulkan so tests/BindlessSamplerTests.cpp can check them.

#include <array>
#include <cstdint>
#include <deque>

namespace Libs::Graphics::BindlessSamplerRecords {

using Record = std::array<uint32_t, 4>;

// Groups of S# fields. KYTY_BINDLESS_SAMPLER_FIELDS=<mask> (live) takes the groups in the mask
// from the game's record and the rest from the fallback sampler, to tell which field of the
// game's samplers breaks a draw (Wolverine's trees vanish with KYTY_BINDLESS_SAMPLERS=1).
enum FieldGroup : uint32_t {
	FieldAddress      = 1u << 0u, // CLAMP_X/Y/Z, DISABLE_CUBE_WRAP
	FieldFilter       = 1u << 1u, // XY_MAG/XY_MIN/Z/MIP filters
	FieldLod          = 1u << 2u, // MIN_LOD, MAX_LOD, PERF_MIP, PERF_Z
	FieldLodBias      = 1u << 3u, // LOD_BIAS, LOD_BIAS_SEC
	FieldAniso        = 1u << 4u, // MAX_ANISO_RATIO, ANISO_THRESHOLD/BIAS/OVERRIDE, POINT_PRECLAMP
	FieldBorder       = 1u << 5u, // BORDER_COLOR_PTR, BORDER_COLOR_TYPE
	FieldDepthCompare = 1u << 6u, // DEPTH_COMPARE_FUNC
	FieldMisc         = 1u << 7u, // everything else (unnormalized, filter mode, trunc, srgb, ...)
	FieldAll          = 0xffu,
};

inline constexpr uint32_t FieldGroupCount = 8;

// The bits of each S# dword that belong to a group, indexed by the group's bit number.
inline constexpr std::array<Record, FieldGroupCount> FieldBits = [] {
	std::array<Record, FieldGroupCount> bits {};
	bits[0] = {0x1ffu | (1u << 28u), 0u, 0u, 0u};
	bits[1] = {0u, 0u, 0xffu << 20u, 0u};
	bits[2] = {0u, 0xffffffffu, 0u, 0u};
	bits[3] = {0u, 0u, 0xfffffu, 0u};
	bits[4] = {(7u << 9u) | (7u << 16u) | (0x3fu << 21u), 0u, (1u << 28u) | (1u << 29u), 0u};
	bits[5] = {0u, 0u, 0u, 0xfffu | (3u << 30u)};
	bits[6] = {7u << 12u, 0u, 0u, 0u};
	for (uint32_t word = 0; word < 4u; word++) {
		uint32_t used = 0;
		for (uint32_t group = 0; group + 1u < FieldGroupCount; group++) {
			used |= bits[group][word];
		}
		bits[FieldGroupCount - 1u][word] = ~used;
	}
	return bits;
}();

// The record with the groups in `mask` from `record` and the others from `fallback`.
inline Record Merge(const Record& record, const Record& fallback, uint32_t mask) {
	Record result {};
	for (uint32_t word = 0; word < 4u; word++) {
		uint32_t from_record = 0;
		for (uint32_t group = 0; group < FieldGroupCount; group++) {
			if ((mask & (1u << group)) != 0u) {
				from_record |= FieldBits[group][word];
			}
		}
		result[word] = (record[word] & from_record) | (fallback[word] & ~from_record);
	}
	return result;
}

// Slots of the bindless sampler array. A heap that changes moves to a new region; without
// recycling the old region is never used again and the array fills (then every later heap
// keeps its old records or selects the default sampler). With recycling, a released region is
// reused once the GPU is past the tick it was released at.
class RegionAllocator {
public:
	void Reset(uint32_t first, uint32_t end) {
		m_next = first;
		m_end  = end;
		m_free.clear();
		m_used = 0;
	}

	// The first slot of `count` free slots, or 0 when the array is full.
	template <typename IsFree>
	uint32_t Allocate(uint32_t count, bool reuse, const IsFree& is_free) {
		if (count == 0u) {
			return 0;
		}
		if (reuse) {
			for (auto it = m_free.begin(); it != m_free.end(); ++it) {
				if (it->count < count || !is_free(it->tick)) {
					continue;
				}
				const auto region = it->region;
				if (it->count == count) {
					m_free.erase(it);
				} else {
					it->region += count;
					it->count -= count;
				}
				m_used += count;
				return region;
			}
		}
		if (m_next >= m_end || count > m_end - m_next) {
			return 0;
		}
		const auto region = m_next;
		m_next += count;
		m_used += count;
		return region;
	}

	// Slots the GPU may still read until `tick`.
	void Release(uint32_t region, uint32_t count, uint64_t tick) {
		if (region == 0u || count == 0u) {
			return;
		}
		m_free.push_back({tick, region, count});
		m_used -= count < m_used ? count : m_used;
	}

	[[nodiscard]] uint32_t Used() const noexcept { return m_used; }
	[[nodiscard]] uint32_t Next() const noexcept { return m_next; }
	[[nodiscard]] size_t   FreeRegions() const noexcept { return m_free.size(); }

private:
	struct FreeRegion {
		uint64_t tick;
		uint32_t region;
		uint32_t count;
	};
	std::deque<FreeRegion> m_free;
	uint32_t               m_next = 1;
	uint32_t               m_end  = 0;
	uint32_t               m_used = 0;
};

} // namespace Libs::Graphics::BindlessSamplerRecords

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSSAMPLERRECORDS_H_
