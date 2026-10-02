#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class CommandRecorder;

// DISPATCH_INDIRECT with the thread-dimension initiator carries thread counts, not workgroup
// counts. The command processor used to read them on the CPU, but the previous dispatch writes
// them on the GPU, so every read drained the GPU; the title issues about 200 of these a frame.
// This records a one-invocation pass that converts the counts on the GPU instead.
class IndirectDispatchGroups {
public:
	IndirectDispatchGroups(GraphicContext& graphics, CommandScheduler& scheduler);
	~IndirectDispatchGroups();
	KYTY_CLASS_NO_COPY(IndirectDispatchGroups);

	struct Result {
		vk::Buffer        groups_buffer;
		vk::DeviceSize    groups_offset = 0;
		// A copy of the thread counts that stays valid until the dispatch has run, unlike the
		// cache buffer the guest's arguments live in, which a later binding may merge away.
		vk::DeviceAddress threads = 0;
	};
	// Records the conversion of the three thread counts at `threads` and returns the workgroup
	// counts' location, ready for dispatchIndirect. Binds a compute pipeline and push state, so
	// call it before committing the guest dispatch's bindings.
	[[nodiscard]] Result Convert(const CommandRecorder& command, vk::DeviceAddress threads,
	                             const std::array<uint32_t, 3>& local_size);

private:
	// Ring of converted counts. An entry is rewritten only after 4096 later conversions, each
	// ordered behind the indirect reads before it.
	static constexpr uint32_t Entries      = 4096;
	static constexpr uint32_t EntryDwords  = 8; // groups at 0, thread counts at 4

	GraphicContext&         m_graphics;
	Buffer                  m_groups;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	uint32_t                m_next            = 0;
};

// DRAW_INDIRECT and DRAW_INDEX_INDIRECT of a mesh-shader pipeline. The host expands the vertex
// or index count into mesh workgroups and passes the draw parameters to the mesh shader, so it
// read the GPU-written arguments on the CPU: a drain per draw (Wolverine: ~54 a second, ~4 % of
// the GPU thread). This records a one-invocation pass that writes the mesh task command and the
// draw parameters on the GPU instead (mesh_indirect_args.comp).
class MeshIndirectArgs {
public:
	MeshIndirectArgs(GraphicContext& graphics, CommandScheduler& scheduler);
	~MeshIndirectArgs();
	KYTY_CLASS_NO_COPY(MeshIndirectArgs);

	struct Input {
		vk::DeviceAddress args                 = 0; // the guest's arguments
		bool              indexed              = false;
		uint32_t          max_count            = 0;
		uint32_t          primitive_size       = 0;
		uint32_t          primitive_step       = 0;
		uint32_t          primitives_per_group = 0;
		uint32_t          max_groups_x         = 0;
		uint32_t          max_groups_y         = 0;
		uint32_t          max_groups_total     = 0;
		uint32_t          element_size         = 0;
		uint64_t          index_address        = 0;
	};
	struct Result {
		vk::Buffer        command_buffer;
		vk::DeviceSize    command_offset = 0;
		vk::DeviceAddress parameters     = 0; // PushData::MeshDrawDwordCount - 2 dwords
	};
	// Records the conversion; binds a compute pipeline and push state, so call it before the
	// draw's own bindings are committed.
	[[nodiscard]] Result Convert(const CommandRecorder& command, const Input& input);
	// Six readable dwords for direct draws, whose mesh shaders read the push values instead.
	[[nodiscard]] vk::DeviceAddress DummyParameters() const;

private:
	static constexpr uint32_t Entries     = 4096;
	static constexpr uint32_t EntryDwords = 16; // command at 0, parameters at 4

	GraphicContext&         m_graphics;
	Buffer                  m_entries;
	vk::DescriptorSetLayout m_set_layout      = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	uint32_t                m_next            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDISPATCH_H_
