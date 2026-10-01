#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <iterator>

namespace Libs::Graphics {

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	TrimImagePool(0, true);
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

// Research: KYTY_IMAGE_RECYCLE_MB=N (live, 0 = off). Wolverine reuses the same memory for
// differently sized and formatted render targets within a frame, so the texture cache deletes
// and recreates ~50 images per frame (~100/s, ~500 MB/s of driver allocations, kernel calls and
// dedicated-memory frees on Thread_Gpu). A deleted image (only after the GPU is done with it:
// the texture cache defers the erase to its tick) is parked, up to N MiB, and an identical
// CreateImage takes it back. It starts in UNDEFINED layout like a new image, so its old
// contents are never observed. Parked images unused for 2 s are destroyed.
static int64_t ImageRecycleMegabytes() {
	static auto& megabytes = Common::LiveSwitches::Get("KYTY_IMAGE_RECYCLE_MB", 0);
	return megabytes.load(std::memory_order_relaxed);
}

decltype(GraphicContext::PooledImage::key)
GraphicContext::PoolKey(const vk::ImageCreateInfo& info) {
	return {info.flags,         info.imageType,    info.format,    info.extent.width,
	        info.extent.height, info.extent.depth, info.mipLevels, info.arrayLayers,
	        info.samples,       info.tiling,       info.usage,     info.sharingMode};
}

void GraphicContext::TrimImagePool(uint64_t cap_bytes, bool all) {
	constexpr auto   MaxAge = std::chrono::seconds(2);
	const auto       now    = std::chrono::steady_clock::now();
	std::scoped_lock lock(m_image_pool_mutex);
	// Oldest first: entries are appended as they are parked.
	size_t keep_from = 0;
	while (keep_from < m_image_pool.size() && (all || m_image_pool_bytes > cap_bytes ||
	                                           now - m_image_pool[keep_from].parked > MaxAge)) {
		auto& entry = m_image_pool[keep_from];
		vmaDestroyImage(allocator, entry.image, entry.allocation);
		m_image_pool_bytes -= entry.bytes;
		keep_from++;
	}
	m_image_pool.erase(m_image_pool.begin(),
	                   m_image_pool.begin() + static_cast<std::ptrdiff_t>(keep_from));
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	vk::Image::CType native_image = VK_NULL_HANDLE;
	auto             result       = vk::Result::eErrorUnknown;
	if (image_info.pNext == nullptr && image_info.initialLayout == vk::ImageLayout::eUndefined &&
	    ImageRecycleMegabytes() > 0) {
		const auto       key = PoolKey(image_info);
		std::scoped_lock lock(m_image_pool_mutex);
		// Newest first, so the entries kept warm are the ones reused.
		for (auto it = m_image_pool.rbegin(); it != m_image_pool.rend(); ++it) {
			if (it->key == key) {
				native_image     = it->image;
				image.allocation = it->allocation;
				m_image_pool_bytes -= it->bytes;
				m_image_pool.erase(std::next(it).base());
				result = vk::Result::eSuccess;
				break;
			}
		}
	}

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	if (ImageRecycleMegabytes() > 0) {
		static std::atomic<uint64_t> reused {0};
		static std::atomic<uint64_t> allocated {0};
		static std::atomic<int64_t>  report_ms {0};
		(result == vk::Result::eSuccess ? reused : allocated)++;
		const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                        std::chrono::steady_clock::now().time_since_epoch())
		                        .count();
		if (auto last = report_ms.load();
		    now_ms - last >= 5000 && report_ms.compare_exchange_strong(last, now_ms)) {
			std::scoped_lock lock(m_image_pool_mutex);
			::printf("Image pool (5 s): reused %" PRIu64 ", allocated %" PRIu64
			         ", parked %zu (%.1f MiB)\n",
			         reused.exchange(0), allocated.exchange(0), m_image_pool.size(),
			         static_cast<double>(m_image_pool_bytes) / 1048576.0);
			std::fflush(stdout);
		}
	}
	if (result != vk::Result::eSuccess) {
		result = static_cast<vk::Result>(vmaCreateImage(
		    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info), &alloc_info,
		    &native_image, &image.allocation, nullptr));
	}
	if (result != vk::Result::eSuccess) {
		alloc_info.requiredFlags  = 0;
		alloc_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		result                    = static_cast<vk::Result>(vmaCreateImage(
            allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
            &alloc_info, &native_image, &image.allocation, nullptr));
		static std::atomic<uint32_t> spill_count {0};
		if (const auto seen = spill_count.fetch_add(1, std::memory_order_relaxed); seen < 32) {
			LOGF("Image spilled to host memory (%u): %ux%ux%u layers=%u levels=%u format=%d -> %s\n",
			     seen + 1, image_info.extent.width, image_info.extent.height,
			     image_info.extent.depth, image_info.arrayLayers, image_info.mipLevels,
			     static_cast<int>(image_info.format), vk::to_string(result).c_str());
		}
	}
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.tiling     = image_info.tiling;
	image.sharing    = image_info.sharingMode;
	image.recyclable =
	    image_info.pNext == nullptr && image_info.initialLayout == vk::ImageLayout::eUndefined;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	const auto megabytes = ImageRecycleMegabytes();
	const auto cap       = static_cast<uint64_t>(std::max<int64_t>(megabytes, 0)) << 20u;
	if (megabytes > 0 && image.recyclable) {
		vk::ImageCreateInfo info {};
		info.flags       = image.flags;
		info.imageType   = image.image_type;
		info.format      = image.format;
		info.extent      = image.extent;
		info.mipLevels   = image.mip_levels;
		info.arrayLayers = image.layers;
		info.samples     = static_cast<vk::SampleCountFlagBits>(image.samples);
		info.tiling      = image.tiling;
		info.usage       = image.usage;
		info.sharingMode = image.sharing;
		VmaAllocationInfo allocation {};
		vmaGetAllocationInfo(allocator, image.allocation, &allocation);
		{
			std::scoped_lock lock(m_image_pool_mutex);
			m_image_pool.push_back({PoolKey(info), image.image, image.allocation, allocation.size,
			                        std::chrono::steady_clock::now()});
			m_image_pool_bytes += allocation.size;
		}
		image.image      = nullptr;
		image.allocation = nullptr;
	} else {
		vmaDestroyImage(allocator, image.image, image.allocation);
		image.image      = nullptr;
		image.allocation = nullptr;
	}
	TrimImagePool(cap, megabytes <= 0);
}

} // namespace Libs::Graphics
