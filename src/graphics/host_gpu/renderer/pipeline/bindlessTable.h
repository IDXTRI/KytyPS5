#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessSamplerRecords.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <span>
#include <deque>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class SamplerCache;

// Descriptor set 1 of every pipeline that samples bindless images: typed arrays of sampled
// images a shader indexes with a slot it looks up per pixel, the key -> slot translation the
// host fills per guest descriptor heap, and a bitset where shaders flag keys whose texture is
// not resident yet. Created only when the device supports descriptor indexing.
class BindlessTable {
public:
	enum Binding : uint32_t {
		Images2D      = 0,
		Images2DArray = 1,
		ImagesCube    = 2,
		Images3D      = 3,
		Translation   = 4,
		Feedback      = 5,
		Samplers      = 6,
	};
	static_assert(Images2D == ShaderRecompiler::IR::BindlessImages2D &&
	              Images2DArray == ShaderRecompiler::IR::BindlessImages2DArray &&
	              ImagesCube == ShaderRecompiler::IR::BindlessImagesCube &&
	              Images3D == ShaderRecompiler::IR::BindlessImages3D &&
	              Translation == ShaderRecompiler::IR::BindlessTranslation &&
	              Feedback == ShaderRecompiler::IR::BindlessFeedback &&
	              Samplers == ShaderRecompiler::IR::BindlessSamplers);
	static constexpr uint32_t ImageArrays        = 4;
	static constexpr uint32_t PlaceholderColors  = 3;
	static constexpr uint32_t Placeholders       = ImageArrays * PlaceholderColors;
	static constexpr uint32_t MaxImagesPerArray  = 16384;
	static constexpr uint32_t TranslationEntries = 1u << 20u;
	static constexpr uint32_t MaxSamplers        = 4096;

	BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler);
	~BindlessTable();
	KYTY_CLASS_NO_COPY(BindlessTable);

	[[nodiscard]] bool                    Enabled() const noexcept { return m_set != nullptr; }
	[[nodiscard]] vk::DescriptorSetLayout Layout() const noexcept { return m_layout; }
	[[nodiscard]] vk::DescriptorSet       Set() const noexcept { return m_set; }
	[[nodiscard]] uint32_t                ImagesPerArray() const noexcept { return m_images_per_array; }

	// A guest descriptor heap as one view type samples it: a region of the translation (and
	// feedback) buffer, one entry per key, and the textures resolved so far.
	struct Heap {
		uint64_t                            base         = 0;
		uint32_t                            table_offset = 0;
		uint32_t                            binding      = 0;
		uint32_t                            region       = 0;
		// Keys the region holds. The guest appends textures to a heap as it streams them in and
		// its descriptor grows; each draw patches its own descriptor's entry count.
		uint32_t                            entries      = 0;
		ShaderRecompiler::IR::ImageResource resource;
		std::vector<uint32_t>               slots;    // per key; 0 = not resolved
		std::vector<uint8_t>                settled;  // per key; resolved, or known placeholder
		std::vector<ImageId>                resolved; // images to keep readable for draws
		std::vector<ImageId>                images;   // per key; the image a resolved key samples
		// Per settled key, the T# it was settled from. The guest rewrites entries as it streams
		// textures out and others in; RenderExecutor::RevalidateBindlessKeys settles a key whose
		// T# changed again (Senaxx 0563e10e).
		std::vector<std::array<uint32_t, 8>> descriptors;
		uint32_t                             revalidate_cursor = 0; // next key to compare
		// What RenderExecutor::CommitBindings last checked: g_bindless_state_generation then, and
		// how many resolved images. Unchanged since, only the images resolved after need a look.
		uint64_t checked_generation = 0;
		size_t   checked_count      = 0;
	};

	// The heap for (base, table offset, view binding), created with every entry pending and
	// room to grow, or moved to a larger region when a descriptor covers more keys; null when
	// the translation buffer is full.
	[[nodiscard]] Heap* FindOrCreateHeap(uint64_t base, uint32_t table_offset, uint32_t binding,
	                                     uint32_t entries,
	                                     const ShaderRecompiler::IR::ImageResource& resource);
	[[nodiscard]] std::deque<Heap>& Heaps() noexcept { return m_heaps; }
	// 0 when the array is full. Slots of dropped images are reused once the GPU is past them.
	[[nodiscard]] uint32_t AllocateSlot(uint32_t binding);
	// Queue a slot for reuse once the GPU is past the current tick (at most once).
	void FreeSlot(uint32_t binding, uint32_t slot);
	void WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view, vk::ImageLayout layout);
	// Resolve a key to a slot (0 = placeholder), or back to pending.
	void SetTranslation(const Heap& heap, uint32_t key, uint32_t slot);
	// The host reads the feedback flags from a copy in host-cached memory. The feedback buffer
	// itself is device memory, and reading every key of three heaps from it uncached took a
	// fifth of the GPU thread. Records that copy at the current point of the command stream.
	void RecordFeedbackSnapshot(CommandScheduler& scheduler);
	[[nodiscard]] bool SnapshotRecorded() const noexcept { return m_snapshot_tick != 0; }
	// Whether the GPU has executed the recorded copy; never waits.
	[[nodiscard]] bool SnapshotReady(CommandScheduler& scheduler);
	void               ConsumeSnapshot() noexcept { m_snapshot_tick = 0; }
	// Pending keys flagged in the snapshot, and the images of resolved keys sampled since the last
	// call (shaders mark both); their flags are cleared.
	void TakeRequests(const Heap& heap, std::vector<uint32_t>& keys, std::vector<ImageId>& used);
	// Logs the loop-watchdog trip reports shaders appended since the last call
	// (BindlessBindings.h), then clears the count.
	void DrainWatchdogReports(uint64_t frame);
	void AddImageReference(ImageId id, Heap& heap, uint32_t key);
	// The key no longer samples what it was settled to: it is pending again, and its image loses
	// the key's reference. The slot keeps its view for the frames in flight and is reused once
	// the GPU is past them. True when the image has no reference left (it need not stay pinned).
	// free_slot = false: the caller keeps the slot (to fall back to it, or free it itself).
	[[nodiscard]] bool ReleaseKey(Heap& heap, uint32_t key, bool free_slot = true);

	// A guest sampler heap as one kind of use samples it: a region of the sampler array that
	// mirrors its S# records, key for key. Uses differ in whether the depth-compare function is
	// kept, whether point filtering is forced and whether border colours are integer, so each
	// kind gets its own region.
	static constexpr uint32_t SamplerDepthCompare   = 1u;
	static constexpr uint32_t SamplerPointFiltering = 2u;
	static constexpr uint32_t SamplerIntegerBorder  = 4u;
	static_assert(SamplerDepthCompare == ShaderRecompiler::IR::BindlessSamplerKeepDepthCompare &&
	              SamplerPointFiltering == ShaderRecompiler::IR::BindlessSamplerPointFiltering &&
	              SamplerIntegerBorder == ShaderRecompiler::IR::BindlessSamplerIntegerBorder);
	// Bits 8..15 of a heap's flags: the KYTY_BINDLESS_SAMPLER_FIELDS mask it was mirrored with
	// (BindlessSamplerRecords::FieldGroup), so a changed mask mirrors the heap anew.
	static constexpr uint32_t SamplerFieldsShift = 8u;
	struct SamplerHeap {
		uint64_t                             base          = 0;
		uint32_t                             table_offset  = 0;
		uint32_t                             flags         = 0;
		uint32_t                             region        = 0; // first slot; 0 = none yet
		uint32_t                             capacity      = 0;
		std::vector<std::array<uint32_t, 4>> records;           // mirrored S# records
		uint64_t                             checked_frame = UINT64_MAX;
	};
	[[nodiscard]] SamplerHeap* FindOrCreateSamplerHeap(uint64_t base, uint32_t table_offset,
	                                                   uint32_t flags);
	// KYTY_BINDLESS_SAMPLER_FIELDS changed: heaps mirrored with another mask are dropped and
	// their regions reused once the GPU is past them.
	void DropSamplerHeapsExcept(uint32_t fields);
	// Mirrors the heap's records into its region. New records are appended in place (their slots
	// were never used); a changed record, or more records than the region holds, moves the heap
	// to a new region, because slots the GPU may be reading are never rewritten. False when the
	// sampler array is full; the heap then keeps its previous records.
	bool MirrorSamplerHeap(SamplerHeap& heap, std::span<const std::array<uint32_t, 4>> records,
	                       SamplerCache& cache);
	// Slot 0 of the sampler array, used for keys outside their heap; written once. With
	// KYTY_BINDLESS_SAMPLER_CLASS_DEFAULTS=1 slots 1..7 hold the default sampler of each kind.
	void WriteDefaultSamplers(SamplerCache& cache);
	// Research counters, reported every 5 s by ReportSamplerStats.
	struct SamplerStats {
		uint32_t mirrors       = 0; // heap reads that changed a mirror
		uint32_t moves         = 0; // a changed record or growth moved a heap to a new region
		uint32_t recycled      = 0; // regions reused (KYTY_BINDLESS_SAMPLER_RECYCLE)
		uint32_t full          = 0; // mirrors refused: the sampler array is full
		uint32_t gpu_dirty     = 0; // heap reads skipped: the GPU wrote the heap
		uint32_t read_failed   = 0; // heap reads skipped: no backing
		uint32_t default_draws = 0; // uses patched with no region (every key -> default)
		uint32_t uses          = 0;
	};
	SamplerStats& Stats() noexcept { return m_sampler_stats; }
	// What bindless image keys resolved to, reported every 5 s with the sampler stats (always,
	// when non-zero): a key settled to the placeholder samples transparent black until its T#
	// changes (Wolverine: black faces, objects without texture).
	struct ImageStats {
		uint32_t resolved    = 0;
		uint32_t unreadable  = 0; // the T# could not be read from guest memory
		uint32_t null_desc   = 0; // a null T#
		uint32_t non_float   = 0; // integer formats have no bindless array
		uint32_t shape       = 0; // a type the binding's array does not hold (1D, MSAA, mismatch)
		uint32_t empty       = 0; // the texture cache had no image for it
		uint32_t array_full  = 0; // no free slot in the binding's array
		uint32_t deferred    = 0; // left pending this frame: the per-frame budget was spent
		uint32_t reresolved  = 0; // of resolved: keys of in-use images settled before a draw
		uint32_t kept_old    = 0; // changed T# not resolvable yet: the key kept its old view
	};
	ImageStats& ImageResolveStats() noexcept { return m_image_stats; }
	void          ReportSamplerStats();
	[[nodiscard]] bool SamplersEnabled() const noexcept { return m_samplers_per_array != 0; }
	// The texture cache dropped a resolved image: point its slots back at the placeholder and
	// make its keys pending again, so a draw that still needs it asks for it anew. in_use: the
	// image was deleted while shaders still sampled it; its keys are queued for TakeReresolve.
	void OnImageUnregistered(ImageId id, bool in_use = false);
	// KYTY_BINDLESS_RERESOLVE (live, default 1): keys of in-use images that were just unregistered,
	// for the draw path to settle again before it records. Otherwise the next draw samples the
	// placeholder (zero) until the shader's feedback is read a frame later; a vertex shader that
	// scales positions by a texel then stretches its triangles across the screen for that frame
	// (Wolverine 2026-10-13: orange floods from VS 0x4d2a3a54c74938ad).
	void TakeReresolve(std::vector<std::pair<Heap*, uint32_t>>& out) {
		out.swap(m_reresolve);
		m_reresolve.clear();
	}
	void QueueReresolve(Heap* heap, uint32_t key) { m_reresolve.emplace_back(heap, key); }
	// Safety net for the stale-slot device losts (VRAM saver 2): the texture cache is dropping an
	// image with these views. A slot still holding one of them goes back to the placeholder and
	// its keys back to pending, whatever the references said. Returns how many slots it reset.
	uint32_t DropStaleViews(std::span<const CachedImageView> views);
	// Device-loss triage (addressBindingReport.h): the slots holding a view of the image.
	static void ReportImageUsers(uint64_t image);

private:
	[[nodiscard]] bool AllocateRegion(Heap& heap, uint32_t entries);
public:

private:
	// Slot 0 of every image array: a 1x1 grey texture of that view type, sampled for keys
	// outside a heap and while a texture is pending. Created, cleared and made read-only once.
	void CreatePlaceholders(CommandScheduler& scheduler);

	[[nodiscard]] static uint64_t ImageKey(ImageId id) {
		return static_cast<uint64_t>(id.index) | (static_cast<uint64_t>(id.generation) << 32u);
	}

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	std::deque<Heap>        m_heaps;
	std::unordered_map<uint64_t, std::vector<std::pair<Heap*, uint32_t>>> m_image_refs;
	std::vector<std::pair<Heap*, uint32_t>> m_reresolve;
	uint32_t                m_next_region = 1; // translation[0] is the out-of-range entry
	std::array<uint32_t, ImageArrays> m_next_slot {PlaceholderColors, PlaceholderColors,
	                                               PlaceholderColors, PlaceholderColors};
	// Slots of unregistered images per array, with the tick after which no command buffer reads
	// them.
	std::array<std::deque<std::pair<uint64_t, uint32_t>>, ImageArrays> m_free_slots;
	// Per slot: 1 while it waits on m_free_slots (FreeSlot keeps each slot there once).
	std::array<std::vector<uint8_t>, ImageArrays> m_slot_free;
	// What each slot's descriptor holds now (WriteSlot), for device-loss triage.
	std::array<std::vector<vk::ImageView>, ImageArrays> m_slot_views;
	// The image each slot's view belonged to when it was written: a view handle the driver
	// reuses for another image after the first is destroyed would otherwise hide the slot.
	std::array<std::vector<uint64_t>, ImageArrays> m_slot_view_images;
	std::array<VulkanImage, Placeholders>   m_placeholders;
	std::array<vk::ImageView, Placeholders> m_placeholder_views {};
	vk::DescriptorPool      m_pool   = nullptr;
	vk::DescriptorSetLayout m_layout = nullptr;
	vk::DescriptorSet       m_set    = nullptr;
	uint32_t                m_images_per_array = 0;
	uint32_t                m_samplers_per_array = 0;
	// Slot 0 (and 1..7 with class defaults) is the default sampler.
	BindlessSamplerRecords::RegionAllocator m_sampler_regions;
	bool                    m_default_sampler_written = false;
	SamplerStats            m_sampler_stats;
	ImageStats              m_image_stats;
	uint64_t                m_sampler_stats_time = 0;
	std::deque<SamplerHeap> m_sampler_heaps;
	std::unique_ptr<Buffer> m_translation;
	std::unique_ptr<Buffer> m_feedback;
	std::unique_ptr<Buffer> m_feedback_snapshot;
	uint64_t                m_snapshot_tick = 0; // 0 = none recorded
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
