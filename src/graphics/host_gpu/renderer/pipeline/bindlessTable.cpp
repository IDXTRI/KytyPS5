#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/addressBindingReport.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <array>
#include <atomic>
#include <fmt/format.h>
#include <string>
#include <cstdlib>
#include <chrono>
#include <map>
#include <tuple>

namespace Libs::Graphics {

BindlessTable::BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	if (!graphics.bindless_enabled) {
		return;
	}
	m_images_per_array =
	    std::min(MaxImagesPerArray, graphics.bindless_max_sampled_images / ImageArrays);
	if (m_images_per_array == 0) {
		LOGF("Bindless table: the device allows no update-after-bind sampled images\n");
		return;
	}

	constexpr auto image_flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
	                             vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	                             vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	// The buffers are written once, here, so they need no update-after-bind.
	constexpr auto buffer_flags = vk::DescriptorBindingFlags {};
	// A device without update-after-bind samplers still gets a one-entry array, holding the
	// default sampler, so every bindless pipeline sees the same layout.
	m_samplers_per_array = std::min(MaxSamplers, graphics.bindless_max_samplers);
	// The per-kind default slots need room for them and at least one heap region besides.
	const bool kind_slots = ShaderRecompiler::IR::BindlessSamplerClassDefaultsRequested() &&
	                        m_samplers_per_array >= ShaderRecompiler::IR::BindlessSamplerKinds + 64u;
	ShaderRecompiler::IR::BindlessSamplerKindSlotsAvailable().store(kind_slots,
	                                                                std::memory_order_release);
	m_sampler_regions.Reset(kind_slots ? ShaderRecompiler::IR::BindlessSamplerKinds : 1u,
	                        m_samplers_per_array);
	const std::array<vk::DescriptorSetLayoutBinding, 7> bindings {{
	    {Images2D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Images2DArray, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {ImagesCube, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images3D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Translation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Feedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Samplers, vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u),
	     vk::ShaderStageFlagBits::eAll, nullptr},
	}};
	const std::array<vk::DescriptorBindingFlags, 7> binding_flags {
	    image_flags, image_flags, image_flags, image_flags, buffer_flags, buffer_flags, image_flags};
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = static_cast<uint32_t>(binding_flags.size());
	flags_info.pBindingFlags = binding_flags.data();
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout),
	                     "create bindless descriptor layout");

	const std::array<vk::DescriptorPoolSize, 3> sizes {{
	    {vk::DescriptorType::eSampledImage, m_images_per_array * ImageArrays},
	    {vk::DescriptorType::eStorageBuffer, 2},
	    {vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u)},
	}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pool_info.pPoolSizes    = sizes.data();
	RequireVulkanSuccess(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool),
	                     "create bindless descriptor pool");

	vk::DescriptorSetAllocateInfo allocate_info {};
	allocate_info.descriptorPool     = m_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_layout;
	RequireVulkanSuccess(graphics.device.allocateDescriptorSets(&allocate_info, &m_set),
	                     "allocate bindless descriptor set");

	// Both live in host-visible device memory: the host writes translations and clears feedback
	// flags directly, while shaders access them at device speed. The host reads the flags from
	// a host-cached snapshot (RecordFeedbackSnapshot).
	m_translation = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback    = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback_snapshot = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                               AllFlags, TranslationEntries * sizeof(uint32_t));
	EXIT_IF(m_translation->Mapped().empty() || m_feedback->Mapped().empty() ||
	        m_feedback_snapshot->Mapped().empty());
	// A region allocated after a snapshot was recorded reads as "no requests" until the next.
	std::fill(m_feedback_snapshot->Mapped().begin(), m_feedback_snapshot->Mapped().end(),
	          uint8_t {0});
	m_feedback_snapshot->Flush(0, m_feedback_snapshot->Size());
	SetVulkanObjectNameF(graphics.device, m_translation->Handle(), "Bindless Translation");
	SetVulkanObjectNameF(graphics.device, m_feedback->Handle(), "Bindless Feedback");

	const std::array<vk::DescriptorBufferInfo, 2> buffer_infos {{
	    {m_translation->Handle(), 0, VK_WHOLE_SIZE},
	    {m_feedback->Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Translation + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_infos[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);
	CreatePlaceholders(scheduler);
	graphics.bindless_layout = m_layout;
	graphics.bindless_set    = m_set;
	LOGF("Bindless table: %u images per array, %u translation entries, %u samplers\n",
	     m_images_per_array, TranslationEntries, m_samplers_per_array);
}

void BindlessTable::DropSamplerHeapsExcept(uint32_t fields) {
	std::erase_if(m_sampler_heaps, [&](const SamplerHeap& heap) {
		if (((heap.flags >> SamplerFieldsShift) & BindlessSamplerRecords::FieldAll) == fields) {
			return false;
		}
		// Command buffers recorded so far may still read its slots.
		m_sampler_regions.Release(heap.region, heap.capacity, m_scheduler.CurrentTick());
		return true;
	});
}

BindlessTable::SamplerHeap* BindlessTable::FindOrCreateSamplerHeap(uint64_t base,
                                                                   uint32_t table_offset,
                                                                   uint32_t flags) {
	for (auto& heap: m_sampler_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.flags == flags) {
			return &heap;
		}
	}
	auto& heap        = m_sampler_heaps.emplace_back();
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.flags        = flags;
	return &heap;
}

void BindlessTable::WriteDefaultSamplers(SamplerCache& cache) {
	if (m_default_sampler_written || m_set == nullptr) {
		return;
	}
	const uint32_t kinds = ShaderRecompiler::IR::BindlessSamplerClassDefaults()
	                           ? std::min(ShaderRecompiler::IR::BindlessSamplerKinds,
	                                      std::max(m_samplers_per_array, 1u))
	                           : 1u;
	std::vector<vk::DescriptorImageInfo> infos;
	for (uint32_t kind = 0; kind < kinds; kind++) {
		ShaderSamplerResource descriptor;
		std::ranges::copy(ShaderRecompiler::IR::FallbackSampler(), descriptor.fields);
		if ((kind & SamplerPointFiltering) != 0u) {
			descriptor.SetPointFiltering();
		}
		if ((kind & SamplerDepthCompare) != 0u) {
			// A depth-compare use needs a compare function; with none Vulkan leaves the result
			// implementation-defined. ALWAYS (7) reads as lit: no false shadow where the key
			// fell outside its heap.
			descriptor.fields[0] = (descriptor.fields[0] & ~(0x7u << 12u)) | (7u << 12u);
		}
		infos.push_back({cache.GetSampler(descriptor, (kind & SamplerIntegerBorder) != 0u), nullptr,
		                 vk::ImageLayout::eUndefined});
	}
	vk::WriteDescriptorSet write {};
	write.dstSet          = m_set;
	write.dstBinding      = Samplers;
	write.dstArrayElement = 0;
	write.descriptorCount = static_cast<uint32_t>(infos.size());
	write.descriptorType  = vk::DescriptorType::eSampler;
	write.pImageInfo      = infos.data();
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	m_default_sampler_written = true;
}

void BindlessTable::ReportSamplerStats() {
	const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch())
	                                           .count());
	if (m_sampler_stats_time == 0) {
		m_sampler_stats_time = now;
		return;
	}
	if (now - m_sampler_stats_time < 5000u) {
		return;
	}
	m_sampler_stats_time = now;
	if (const auto& i = m_image_stats; i.resolved + i.unreadable + i.null_desc + i.non_float +
	                                       i.shape + i.empty + i.array_full + i.deferred !=
	                                   0u) {
		std::printf("Bindless images (5 s): resolved %u (re-resolved before a draw %u, kept old view "
		            "%u), deferred %u; black placeholder: unreadable %u, null %u, non-float %u, shape %u, "
		            "empty %u, array full %u; slots 2D %u 2DArray %u Cube %u 3D %u of %u\n",
		            i.resolved, i.reresolved, i.kept_old, i.deferred, i.unreadable, i.null_desc,
		            i.non_float, i.shape,
		            i.empty, i.array_full, m_next_slot[0], m_next_slot[1], m_next_slot[2],
		            m_next_slot[3], m_images_per_array);
		std::fflush(stdout);
	}
	m_image_stats = {};
	const auto& s = m_sampler_stats;
	if (s.uses != 0u) {
		std::printf("Bindless samplers (5 s): uses=%u default-only=%u mirrors=%u moves=%u recycled=%u "
		            "full=%u unread gpu-dirty=%u read-failed=%u; slots %u used, next %u of %u, "
		            "%zu free regions, %zu heaps\n",
		            s.uses, s.default_draws, s.mirrors, s.moves, s.recycled, s.full, s.gpu_dirty,
		            s.read_failed, m_sampler_regions.Used(), m_sampler_regions.Next(),
		            m_samplers_per_array, m_sampler_regions.FreeRegions(), m_sampler_heaps.size());
		std::fflush(stdout);
	}
	m_sampler_stats = {};
}

bool BindlessTable::MirrorSamplerHeap(SamplerHeap&                              heap,
                                      std::span<const std::array<uint32_t, 4>> records,
                                      SamplerCache&                             cache) {
	const auto same_prefix =
	    records.size() >= heap.records.size() &&
	    std::equal(heap.records.begin(), heap.records.end(), records.begin());
	if (same_prefix && records.size() == heap.records.size()) {
		return true;
	}
	uint32_t first = static_cast<uint32_t>(heap.records.size());
	if (!same_prefix || records.size() > heap.capacity || heap.region == 0) {
		// KYTY_BINDLESS_SAMPLER_RECYCLE=1 (live, research): the region a heap leaves is reused
		// once the GPU is past it; otherwise every change of a record leaks a region until the
		// array is full, after which heaps keep stale records or select the default sampler.
		static auto& recycle  = Common::LiveSwitches::Get("KYTY_BINDLESS_SAMPLER_RECYCLE", 0);
		const bool   reuse    = recycle.load(std::memory_order_relaxed) != 0;
		const auto   capacity = std::max<uint32_t>(64u, static_cast<uint32_t>(records.size()) * 2u);
		const auto   next_before = m_sampler_regions.Next();
		const auto   region      = m_sampler_regions.Allocate(
            capacity, true, [&](uint64_t tick) { return m_scheduler.IsFree(tick); });
		if (region == 0u) {
			m_sampler_stats.full++;
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 8) {
				std::printf("Bindless samplers: array full (%u of %u slots), heap 0x%016" PRIx64
				            " keeps %zu records\n",
				            m_sampler_regions.Next(), m_samplers_per_array, heap.base, heap.records.size());
				std::fflush(stdout);
			}
			return false;
		}
		if (m_sampler_regions.Next() == next_before) {
			m_sampler_stats.recycled++;
		}
		if (heap.region != 0u) {
			m_sampler_stats.moves++;
			if (reuse) {
				m_sampler_regions.Release(heap.region, heap.capacity, m_scheduler.CurrentTick());
			}
		}
		heap.region   = region;
		heap.capacity = capacity;
		first = 0;
	}
	m_sampler_stats.mirrors++;
	std::vector<vk::DescriptorImageInfo> infos;
	infos.reserve(records.size() - first);
	// KYTY_BINDLESS_SAMPLERS_CLAMP_ONLY=1 (environment): take only the address modes (CLAMP_X/Y/Z)
	// from the game's records and keep the fallback sampler's filtering and LOD range. Run 65: the
	// full records turned Wolverine's terrain into flat colours while the moon needs its record's
	// clamp; this tells the fields apart from the key -> record mapping.
	// (Now the same as KYTY_BINDLESS_SAMPLER_FIELDS=1, which also keeps DISABLE_CUBE_WRAP.)
	// KYTY_BINDLESS_SAMPLER_FIELDS=<mask> (live, PrepareBindlessSamplers): the field groups taken
	// from the game's records; the rest come from the default sampler (bindlessSamplerRecords.h).
	const uint32_t fields = (heap.flags >> SamplerFieldsShift) & BindlessSamplerRecords::FieldAll;
	for (size_t key = first; key < records.size(); key++) {
		ShaderSamplerResource descriptor;
		const auto            merged = BindlessSamplerRecords::Merge(
            records[key], ShaderRecompiler::IR::BindlessDefaultSampler, fields);
		std::copy(merged.begin(), merged.end(), descriptor.fields);
		if ((heap.flags & SamplerDepthCompare) == 0u) {
			descriptor.fields[0] &= ~(0x7u << 12u);
		}
		if ((heap.flags & SamplerPointFiltering) != 0u) {
			descriptor.SetPointFiltering();
		}
		infos.push_back({cache.GetSampler(descriptor, (heap.flags & SamplerIntegerBorder) != 0u),
		                 nullptr, vk::ImageLayout::eUndefined});
	}
	if (!infos.empty()) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = m_set;
		write.dstBinding      = Samplers;
		write.dstArrayElement = heap.region + first;
		write.descriptorCount = static_cast<uint32_t>(infos.size());
		write.descriptorType  = vk::DescriptorType::eSampler;
		write.pImageInfo      = infos.data();
		m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	}
	static std::atomic<uint32_t> logged = 0;
	if (logged.fetch_add(1) < 32) {
		LOGF("Bindless samplers: heap 0x%016" PRIx64 "+0x%x flags=%u region=%u records=%zu"
		     " (%u new)\n",
		     heap.base, heap.table_offset, heap.flags, heap.region, records.size(),
		     static_cast<uint32_t>(infos.size()));
	}
	// KYTY_SAMPLER_DUMP=1 (environment): the first mirrors of each heap print what its S# records
	// ask for, grouped: address modes, LOD range, mip filter, unnormalized coordinates, LOD bias.
	static const bool dump = [] {
		const char* value = std::getenv("KYTY_SAMPLER_DUMP");
		return value != nullptr && value[0] == '1';
	}();
	static std::atomic<uint32_t> dumped = 0;
	if (dump && !infos.empty() && dumped.fetch_add(1) < 24) {
		std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
		         uint32_t>
		    groups;
		for (const auto& record: records) {
			ShaderSamplerResource r;
			std::copy(record.begin(), record.end(), r.fields);
			groups[{static_cast<uint32_t>(r.ClampX() | (r.ClampY() << 3u) | (r.ClampZ() << 6u)),
			        r.MinLod(), r.MaxLod(), static_cast<uint32_t>(r.MipFilter()),
			        r.ForceUnormCoords() ? 1u : 0u, r.LodBias(),
			        static_cast<uint32_t>(r.XyMinFilter() | (r.XyMagFilter() << 2u))}]++;
		}
		::printf("Sampler heap 0x%016" PRIx64 "+0x%x flags=%u: %zu records, %zu distinct\n", heap.base,
		         heap.table_offset, heap.flags, records.size(), groups.size());
		size_t shown = 0;
		for (const auto& [key, count]: groups) {
			if (shown++ == 16) {
				break;
			}
			const auto [clamp, min_lod, max_lod, mip, unnorm, bias, filters] = key;
			::printf("  %4u x clamp=%u/%u/%u min_lod=%.2f max_lod=%.2f mip=%u unnorm=%u "
			         "bias=0x%04x xyfilt=0x%x\n",
			         count, clamp & 7u, (clamp >> 3u) & 7u, (clamp >> 6u) & 7u,
			         static_cast<double>(min_lod) / 256.0, static_cast<double>(max_lod) / 256.0, mip,
			         unnorm, bias, filters);
		}
		for (size_t i = 0; i < std::min<size_t>(records.size(), 4); i++) {
			::printf("  raw[%zu] = %08x %08x %08x %08x\n", i, records[i][0], records[i][1],
			         records[i][2], records[i][3]);
		}
		std::fflush(stdout);
	}
	heap.records.assign(records.begin(), records.end());
	return true;
}

void BindlessTable::CreatePlaceholders(CommandScheduler& /*scheduler*/) {
	struct Kind {
		vk::ImageType        type;
		vk::ImageViewType    view;
		uint32_t             layers;
		vk::ImageCreateFlags flags;
	};
	const std::array<Kind, ImageArrays> kinds {{
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::eCube, 6, vk::ImageCreateFlagBits::eCubeCompatible},
	    {vk::ImageType::e3D, vk::ImageViewType::e3D, 1, {}},
	}};
	for (uint32_t i = 0; i < Placeholders; i++) {
		vk::ImageCreateInfo info {};
		info.flags         = kinds[i % ImageArrays].flags;
		info.imageType     = kinds[i % ImageArrays].type;
		info.format        = vk::Format::eR8G8B8A8Unorm;
		info.extent        = vk::Extent3D {1, 1, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = kinds[i % ImageArrays].layers;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(info, m_placeholders[i]));
		vk::ImageViewCreateInfo view {};
		view.image            = m_placeholders[i].image;
		view.viewType         = kinds[i % ImageArrays].view;
		view.format           = info.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, kinds[i % ImageArrays].layers};
		RequireVulkanSuccess(
		    m_graphics.device.createImageView(&view, nullptr, &m_placeholder_views[i]),
		    "create bindless placeholder view");
	}

	// One fenced submission, before any rendering: clears and a buffer fill are not allowed
	// inside the render passes draws record into.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create bindless init pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate bindless init command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin bindless init command buffer");

	std::array<vk::ImageMemoryBarrier, Placeholders> to_transfer {};
	std::array<vk::ImageMemoryBarrier, Placeholders> to_read {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
		                                       kinds[i % ImageArrays].layers};
		to_transfer[i].srcAccessMask       = {};
		to_transfer[i].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_transfer[i].oldLayout           = vk::ImageLayout::eUndefined;
		to_transfer[i].newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].image               = m_placeholders[i].image;
		to_transfer[i].subresourceRange    = range;
		to_read[i]                         = to_transfer[i];
		to_read[i].srcAccessMask           = vk::AccessFlagBits::eTransferWrite;
		to_read[i].dstAccessMask           = vk::AccessFlagBits::eShaderRead;
		to_read[i].oldLayout               = vk::ImageLayout::eTransferDstOptimal;
		to_read[i].newLayout               = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        Placeholders, to_transfer.data());
	// Transparent black by default. KYTY_BINDLESS_DEBUG_COLORS=1: slot 0 grey (not resident), slot
	// 1 red (key outside its heap), slot 2 blue (pending), to see which case a surface is in.
	const bool debug_colors = std::getenv("KYTY_BINDLESS_DEBUG_COLORS") != nullptr;
	const auto color        = [debug_colors](float r, float g, float b) {
        return debug_colors ? vk::ClearColorValue {std::array<float, 4> {r, g, b, 1.0f}}
		                           : vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	};
	const std::array<vk::ClearColorValue, PlaceholderColors> colors {
	    color(0.5f, 0.5f, 0.5f), color(0.8f, 0.1f, 0.1f), color(0.1f, 0.2f, 0.8f)};
	for (uint32_t i = 0; i < Placeholders; i++) {
		command.clearColorImage(m_placeholders[i].image, vk::ImageLayout::eTransferDstOptimal,
		                        &colors[i / ImageArrays], 1, &to_transfer[i].subresourceRange);
	}
	// translation[0], the entry of keys outside their heap, is the red slot.
	command.fillBuffer(m_translation->Handle(), 4, VK_WHOLE_SIZE, 0u);
	command.fillBuffer(m_translation->Handle(), 0, 4, 1u);
	command.fillBuffer(m_feedback->Handle(), 0, VK_WHOLE_SIZE, 0u);
	vk::BufferMemoryBarrier translation_ready {};
	translation_ready.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	translation_ready.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	translation_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.buffer              = m_translation->Handle();
	translation_ready.offset              = 0;
	translation_ready.size                = VK_WHOLE_SIZE;
	auto feedback_ready                   = translation_ready;
	feedback_ready.dstAccessMask          = vk::AccessFlagBits::eShaderRead |
	                                        vk::AccessFlagBits::eShaderWrite |
	                                        vk::AccessFlagBits::eHostRead;
	feedback_ready.buffer                 = m_feedback->Handle();
	const std::array<vk::BufferMemoryBarrier, 2> buffers_ready {translation_ready, feedback_ready};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, static_cast<uint32_t>(buffers_ready.size()),
	                        buffers_ready.data(), Placeholders, to_read.data());
	RequireVulkanSuccess(command.end(), "end bindless init command buffer");

	vk::FenceCreateInfo fence_info {};
	vk::Fence           fence = nullptr;
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create bindless init fence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit bindless init");
	}
	RequireVulkanSuccess(m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX),
	                     "wait for bindless init");
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);

	std::array<vk::DescriptorImageInfo, Placeholders> infos {};
	std::array<vk::WriteDescriptorSet, Placeholders> writes {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		infos[i]                  = {nullptr, m_placeholder_views[i],
		                             vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Images2D + i % ImageArrays;
		writes[i].dstArrayElement = i / ImageArrays;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[i].pImageInfo      = &infos[i];
	}
	m_graphics.device.updateDescriptorSets(Placeholders, writes.data(), 0, nullptr);
}

BindlessTable::~BindlessTable() {
	for (uint32_t i = 0; i < Placeholders; i++) {
		if (m_placeholder_views[i] != nullptr) {
			m_graphics.device.destroyImageView(m_placeholder_views[i], nullptr);
		}
		if (m_placeholders[i].image != nullptr) {
			m_graphics.DeleteImage(m_placeholders[i]);
		}
	}
	m_translation.reset();
	m_feedback.reset();
	m_feedback_snapshot.reset();
	if (m_pool != nullptr) {
		m_graphics.device.destroyDescriptorPool(m_pool, nullptr);
	}
	if (m_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
	}
}

bool BindlessTable::AllocateRegion(Heap& heap, uint32_t entries) {
	// Headroom so a growing heap rarely moves; a move re-writes the keys resolved so far.
	const auto capacity = std::max(entries, std::min<uint32_t>(entries * 2u, 1u << 16u));
	if (capacity > ShaderRecompiler::IR::BindlessTraceBase - m_next_region) {
		return false;
	}
	const auto old_region  = heap.region;
	const auto old_entries = heap.entries;
	heap.region            = m_next_region;
	heap.entries           = capacity;
	m_next_region += capacity;
	heap.slots.resize(capacity, 0u);
	heap.settled.resize(capacity, 0u);
	heap.images.resize(capacity);
	heap.descriptors.resize(capacity);
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	auto* feedback    = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	for (uint32_t key = 0; key < capacity; key++) {
		uint32_t value = ShaderRecompiler::IR::BindlessPending;
		if (key < old_entries && heap.settled[key] != 0) {
			value = heap.slots[key];
		}
		translation[heap.region + key] = value;
		feedback[heap.region + key]    = 0;
	}
	if (!m_translation->IsCoherent()) {
		m_translation->Flush(heap.region * sizeof(uint32_t), capacity * sizeof(uint32_t));
	}
	LOGF("Bindless heap region: base=0x%016" PRIx64 " binding=%u region=%u entries=%u (was %u at "
	     "%u)\n",
	     heap.base, heap.binding, heap.region, capacity, old_entries, old_region);
	return true;
}

BindlessTable::Heap* BindlessTable::FindOrCreateHeap(
    uint64_t base, uint32_t table_offset, uint32_t binding, uint32_t entries,
    const ShaderRecompiler::IR::ImageResource& resource) {
	if (entries == 0) {
		return nullptr;
	}
	for (auto& heap: m_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.binding == binding) {
			if (entries > heap.entries && !AllocateRegion(heap, entries)) {
				return nullptr;
			}
			return &heap;
		}
	}
	auto& heap        = m_heaps.emplace_back();
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.binding      = binding;
	heap.resource     = resource;
	if (!AllocateRegion(heap, std::max(entries, 1u << 14u))) {
		m_heaps.pop_back();
		return nullptr;
	}
	return &heap;
}

void BindlessTable::FreeSlot(uint32_t binding, uint32_t slot) {
	if (binding >= ImageArrays || slot == 0) {
		return;
	}
	auto& flags = m_slot_free[binding];
	if (flags.size() <= slot) {
		flags.resize(static_cast<size_t>(slot) + 1u, 0);
	}
	// A slot already waiting on the free list must not be queued twice: two keys would get it.
	if (flags[slot] != 0) {
		static std::atomic<uint32_t> reported {0};
		if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
			std::printf("Bindless: slot %u/%u freed twice; kept once\n", binding, slot);
			std::fflush(stdout);
		}
		return;
	}
	flags[slot] = 1;
	// Command buffers recorded so far may still sample the slot.
	m_free_slots[binding].emplace_back(m_scheduler.CurrentTick(), slot);
}

uint32_t BindlessTable::AllocateSlot(uint32_t binding) {
	if (binding >= ImageArrays) {
		return 0;
	}
	// KYTY_BINDLESS_SLOT_REUSE=0 (live, research): never reuse slots, to tell a stale slot
	// reference from other bugs (Wolverine runs 54-58: wrong textures, then device losts).
	static auto& reuse = Common::LiveSwitches::Get("KYTY_BINDLESS_SLOT_REUSE", 1);
	auto&        free  = m_free_slots[binding];
	if (reuse.load(std::memory_order_relaxed) != 0 && !free.empty() &&
	    m_scheduler.IsFree(free.front().first)) {
		const auto slot = free.front().second;
		free.pop_front();
		m_slot_free[binding][slot] = 0;
		return slot;
	}
	if (m_next_slot[binding] >= m_images_per_array) {
		return 0;
	}
	return m_next_slot[binding]++;
}

namespace {
// The table whose slots the device-loss report searches (one bindless table per context).
BindlessTable* g_reported_table = nullptr;
} // namespace

void BindlessTable::ReportImageUsers(uint64_t image) {
	if (g_reported_table == nullptr) {
		return;
	}
	auto& table = *g_reported_table;
	int   found = 0;
	for (uint32_t binding = 0; binding < ImageArrays; binding++) {
		const auto& views  = table.m_slot_views[binding];
		const auto& images = table.m_slot_view_images[binding];
		for (uint32_t slot = 0; slot < views.size(); slot++) {
			const auto view = reinterpret_cast<uint64_t>(static_cast<VkImageView>(views[slot]));
			const bool now  = view != 0 && AddressBindingImageOfView(view) == image;
			const bool when_written = slot < images.size() && images[slot] == image;
			if (!now && !when_written) {
				continue;
			}
			found++;
			std::printf("      bindless slot %u/%u still holds a view of it%s", binding, slot,
			            now ? "" : " (its view handle now names another image)");
			for (auto& heap: table.m_heaps) {
				if (heap.binding != binding) {
					continue;
				}
				for (uint32_t key = 0; key < heap.slots.size(); key++) {
					if (heap.slots[key] == slot) {
						std::printf(" (heap base=0x%" PRIx64 " key=%u settled=%u)", heap.base, key,
						            heap.settled[key]);
					}
				}
			}
			std::printf("\n");
		}
	}
	if (found == 0) {
		std::printf("      no bindless slot holds a view of it (stale reference is elsewhere)\n");
	}
}

void BindlessTable::WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view,
                              vk::ImageLayout layout) {
	if (g_reported_table == nullptr && m_graphics.address_binding_report_enabled) {
		g_reported_table = this;
		AddressBindingSetImageUserReporter(&BindlessTable::ReportImageUsers);
	}
	if (binding < ImageArrays) {
		auto& views = m_slot_views[binding];
		if (views.size() <= slot) {
			views.resize(slot + 1);
		}
		views[slot] = view;
		if (m_graphics.address_binding_report_enabled) {
			auto& images = m_slot_view_images[binding];
			if (images.size() <= slot) {
				images.resize(slot + 1);
			}
			if (images[slot] != 0) {
				AddressBindingAnnotate(images[slot], "bindless slot overwritten");
			}
			images[slot] = AddressBindingImageOfView(
			    reinterpret_cast<uint64_t>(static_cast<VkImageView>(view)));
			if (images[slot] != 0) {
				AddressBindingAnnotate(images[slot], "bindless slot written");
			}
		}
	}
	const vk::DescriptorImageInfo info {nullptr, view, layout};
	vk::WriteDescriptorSet        write {};
	write.dstSet          = m_set;
	write.dstBinding      = binding;
	write.dstArrayElement = slot;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampledImage;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
}

void BindlessTable::SetTranslation(const Heap& heap, uint32_t key, uint32_t slot) {
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	translation[heap.region + key] = slot;
	if (!m_translation->IsCoherent()) {
		m_translation->Flush((heap.region + key) * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::RecordFeedbackSnapshot(CommandScheduler& scheduler) {
	// Regions are allocated contiguously after word 0, so one copy covers every heap.
	const vk::DeviceSize size = vk::DeviceSize {m_next_region} * sizeof(uint32_t);
	scheduler.EndRendering();
	auto                    command = scheduler.Current().Handle();
	vk::BufferMemoryBarrier flags_written {};
	flags_written.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	flags_written.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	flags_written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.buffer              = m_feedback->Handle();
	flags_written.offset              = 0;
	flags_written.size                = size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1,
	                        &flags_written, 0, nullptr);
	const vk::BufferCopy copy {0, 0, size};
	command.copyBuffer(m_feedback->Handle(), m_feedback_snapshot->Handle(), 1, &copy);
	auto snapshot_written          = flags_written;
	snapshot_written.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	snapshot_written.dstAccessMask = vk::AccessFlagBits::eHostRead;
	snapshot_written.buffer        = m_feedback_snapshot->Handle();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
	                        &snapshot_written, 0, nullptr);
	m_snapshot_tick = scheduler.CurrentTick();
}

bool BindlessTable::SnapshotReady(CommandScheduler& scheduler) {
	return m_snapshot_tick != 0 && scheduler.IsFree(m_snapshot_tick);
}

void BindlessTable::TakeRequests(const Heap& heap, std::vector<uint32_t>& keys,
                                 std::vector<ImageId>& used) {
	const auto* snapshot = reinterpret_cast<const uint32_t*>(m_feedback_snapshot->Mapped().data());
	auto*       feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	m_feedback_snapshot->Invalidate(heap.region * sizeof(uint32_t),
	                                heap.entries * sizeof(uint32_t));
	bool cleared = false;
	for (uint32_t key = 0; key < heap.entries; key++) {
		const auto flag = snapshot[heap.region + key];
		if (flag != 0) {
			// A flag the GPU sets again after the snapshot may be lost here; a key that is
			// still pending (or in use) is flagged again by the next draw that samples it.
			feedback[heap.region + key] = 0;
			if (flag == ShaderRecompiler::IR::BindlessFlagPending) {
				keys.push_back(key);
			} else if (flag == ShaderRecompiler::IR::BindlessFlagUsed && heap.images[key]) {
				used.push_back(heap.images[key]);
			}
			cleared = true;
		}
	}
	if (cleared) {
		m_feedback->Flush(heap.region * sizeof(uint32_t), heap.entries * sizeof(uint32_t));
	}
}

void BindlessTable::DrainWatchdogReports(uint64_t frame) {
	using ShaderRecompiler::IR::WatchdogReportBase;
	using ShaderRecompiler::IR::WatchdogReportPhis;
	using ShaderRecompiler::IR::WatchdogReportSlots;
	using ShaderRecompiler::IR::WatchdogReportWords;
	static_assert(WatchdogReportBase + WatchdogReportWords * (WatchdogReportSlots + 1) <=
	              TranslationEntries);
	auto* feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	if (!m_feedback->IsCoherent()) {
		m_feedback->Invalidate(WatchdogReportBase * sizeof(uint32_t),
		                       WatchdogReportWords * (WatchdogReportSlots + 1) * sizeof(uint32_t));
	}
	// KYTY_TRACE_BINDLESS: what the traced shader's last bindless read used, printed when it
	// changes: key, raw translation, heap region and entry count, and the T# and image the key
	// was settled to. The words are cleared each frame, so "none" = no read since the last frame.
	if (static const bool trace = std::getenv("KYTY_TRACE_BINDLESS") != nullptr; trace) {
		using ShaderRecompiler::IR::BindlessTraceBase;
		if (!m_feedback->IsCoherent()) {
			m_feedback->Invalidate(BindlessTraceBase * sizeof(uint32_t), 4 * sizeof(uint32_t));
		}
		static std::array<uint32_t, 4> last {~0u, ~0u, ~0u, ~0u};
		std::array<uint32_t, 4>        now {};
		std::copy_n(feedback + BindlessTraceBase, 4, now.begin());
		if (now != last) {
			last = now;
			std::string info = "none";
			if (now[3] != 0u || now[0] != 0u) {
				info = fmt::format("key {} raw {} region {} count {}", now[0], now[1], now[2], now[3]);
				for (const auto& heap: m_heaps) {
					if (heap.region == now[2] && now[0] < heap.slots.size()) {
						const auto& t = heap.descriptors[now[0]];
						info += fmt::format(" | heap 0x{:x} binding {} slot {} image {} T# {:08x} {:08x} "
						                    "{:08x} {:08x}",
						                    heap.base, heap.binding, heap.slots[now[0]],
						                    heap.images[now[0]].index, t[0], t[1], t[2], t[3]);
					}
				}
			}
			std::printf("BindlessTrace frame %" PRIu64 ": %s\n", frame, info.c_str());
		}
		std::fill_n(feedback + BindlessTraceBase, 4, 0u);
		if (!m_feedback->IsCoherent()) {
			m_feedback->Flush(BindlessTraceBase * sizeof(uint32_t), 4 * sizeof(uint32_t));
		}
	}
	const auto count = feedback[WatchdogReportBase];
	if (count == 0) {
		return;
	}
	const auto stored = std::min(count, WatchdogReportSlots);
	// Per (shader, loop): how many tripped, and the first report in full.
	std::unordered_map<uint64_t, std::pair<uint32_t, const uint32_t*>> loops;
	for (uint32_t n = 0; n < stored; n++) {
		const auto* report = feedback + WatchdogReportBase + WatchdogReportWords * (n + 1);
		if ((report[0] >> 16u) != 0x5744u) {
			continue;
		}
		const auto key = (static_cast<uint64_t>(report[2]) << 32u | report[1]) ^
		                 (static_cast<uint64_t>(report[0] & 0xffffu) << 48u);
		auto& entry = loops[key];
		if (entry.first++ == 0) {
			entry.second = report;
		}
	}
	static std::atomic<uint32_t> logged = 0;
	LOGF("WatchdogTrips: frame=%" PRIu64 " reports=%u loops=%zu\n", frame, count, loops.size());
	for (const auto& [key, entry]: loops) {
		if (logged.fetch_add(1) >= 256) {
			break;
		}
		const auto* r = entry.second;
		std::string phis;
		for (uint32_t half = 0; half < 2; half++) {
			phis += half == 0 ? " lo:" : " hi:";
			for (uint32_t i = 0; i < WatchdogReportPhis; i++) {
				phis += fmt::format(" {:x}", r[16 + half * WatchdogReportPhis + i]);
			}
		}
		LOGF("WatchdogTrip: hash=0x%08x%08x loop_block=%u trips=%u counter=%u group=%u lane=%u "
		     "ud=[%08x %08x %08x %08x %08x %08x %08x %08x]%s\n",
		     r[2], r[1], r[0] & 0xffffu, entry.first, r[3], r[4], r[5], r[6], r[7], r[8], r[9],
		     r[10], r[11], r[12], r[13], phis.c_str());
	}
	feedback[WatchdogReportBase] = 0;
	if (!m_feedback->IsCoherent()) {
		m_feedback->Flush(WatchdogReportBase * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::AddImageReference(ImageId id, Heap& heap, uint32_t key) {
	m_image_refs[ImageKey(id)].emplace_back(&heap, key);
}

bool BindlessTable::ReleaseKey(Heap& heap, uint32_t key, bool free_slot) {
	const auto id      = heap.images[key];
	bool       no_refs = false;
	if (id) {
		const auto found = m_image_refs.find(ImageKey(id));
		if (found != m_image_refs.end()) {
			std::erase(found->second, std::pair<Heap*, uint32_t> {&heap, key});
			const bool in_heap = std::any_of(found->second.begin(), found->second.end(),
			                                 [&](const auto& ref) { return ref.first == &heap; });
			if (!in_heap) {
				std::erase(heap.resolved, id);
				// Erasing shifts the images RenderExecutor::CommitBindings counted as checked.
				heap.checked_generation = 0;
			}
			if (found->second.empty()) {
				m_image_refs.erase(found);
				no_refs = true;
			}
		}
	}
	if (heap.slots[key] != 0 && free_slot) {
		// Command buffers recorded so far may still sample the slot.
		FreeSlot(heap.binding, heap.slots[key]);
	}
	heap.slots[key]   = 0;
	heap.settled[key] = 0;
	heap.images[key]  = {};
	SetTranslation(heap, key, ShaderRecompiler::IR::BindlessPending);
	return no_refs;
}

void BindlessTable::OnImageUnregistered(ImageId id, bool in_use) {
	const auto found = m_image_refs.find(ImageKey(id));
	if (found == m_image_refs.end()) {
		return;
	}
	static auto& reresolve = Common::LiveSwitches::Get("KYTY_BINDLESS_RERESOLVE", 1);
	const bool   queue     = in_use && reresolve.load(std::memory_order_relaxed) != 0;
	for (const auto& [heap, key]: found->second) {
		if (queue) {
			m_reresolve.emplace_back(heap, key);
		}
		if (heap->slots[key] != 0) {
			WriteSlot(heap->binding, heap->slots[key], m_placeholder_views[heap->binding],
			          vk::ImageLayout::eShaderReadOnlyOptimal);
			// Command buffers recorded so far may still sample the slot.
			FreeSlot(heap->binding, heap->slots[key]);
		}
		heap->images[key]  = {};
		heap->slots[key]   = 0;
		heap->settled[key] = 0;
		SetTranslation(*heap, key, ShaderRecompiler::IR::BindlessPending);
		std::erase(heap->resolved, id);
	}
	m_image_refs.erase(found);
}

uint32_t BindlessTable::DropStaleViews(std::span<const CachedImageView> views) {
	uint32_t reset = 0;
	for (uint32_t binding = 0; binding < ImageArrays; binding++) {
		auto&      slot_views = m_slot_views[binding];
		const auto used       = std::min<size_t>(slot_views.size(), m_next_slot[binding]);
		for (uint32_t slot = PlaceholderColors; slot < used; slot++) {
			const auto view = slot_views[slot];
			if (view == nullptr || view == m_placeholder_views[binding] ||
			    std::none_of(views.begin(), views.end(),
			                 [&](const CachedImageView& cached) { return cached.view == view; })) {
				continue;
			}
			uint32_t keys = 0;
			for (auto& heap: m_heaps) {
				if (heap.binding != binding) {
					continue;
				}
				for (uint32_t key = 0; key < heap.slots.size(); key++) {
					if (heap.slots[key] != slot) {
						continue;
					}
					if (const auto found = m_image_refs.find(ImageKey(heap.images[key]));
					    heap.images[key] && found != m_image_refs.end()) {
						std::erase(found->second, std::pair<Heap*, uint32_t> {&heap, key});
						if (found->second.empty()) {
							m_image_refs.erase(found);
						}
					}
					if (heap.images[key]) {
						std::erase(heap.resolved, heap.images[key]);
						heap.checked_generation = 0;
					}
					heap.slots[key]   = 0;
					heap.settled[key] = 0;
					heap.images[key]  = {};
					SetTranslation(heap, key, ShaderRecompiler::IR::BindlessPending);
					keys++;
				}
			}
			WriteSlot(binding, slot, m_placeholder_views[binding],
			          vk::ImageLayout::eShaderReadOnlyOptimal);
			// Only a slot this took from keys goes to the free list. A slot no key uses was freed
			// already (ReleaseKey / OnImageUnregistered keep its view until reuse); freeing it again
			// put it on the list twice, so two keys got the same slot and one of them sampled the
			// other's texture (Wolverine 2026-10-09: pink leaves, rainbow AA gun, device losts).
			if (keys != 0) {
				// Command buffers recorded so far may still sample the slot.
				FreeSlot(binding, slot);
			}
			reset++;
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
				std::printf("Bindless: slot %u/%u still held a view of an image being dropped "
				            "(%u keys); reset to the placeholder\n",
				            binding, slot, keys);
				std::fflush(stdout);
			}
		}
	}
	return reset;
}

} // namespace Libs::Graphics
