#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {

namespace {

// KYTY_FAULT_STATS=1 (live, default 0): every 5 s, the tracked-page faults by kind, the distinct
// pages and the pages that faulted most, to tell a few hot pages from many cold ones; then the
// faulting code sites with the time spent handling them. A site is an offset in the emulator
// executable (symbolize with the build's PDB), or "guest" for game code. A read site in the
// emulator stalls its thread until the GPU finishes (BufferCache::ReadMemory).
class FaultStats {
public:
	static bool Enabled() {
		static auto& enabled = Common::LiveSwitches::Get("KYTY_FAULT_STATS", 0);
		return enabled.load(std::memory_order_relaxed) != 0;
	}

	// The instruction that faulted, as an offset in the emulator executable; 0 for other code.
	static uint64_t FaultSite() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		static const auto module_base = [](const char* name) {
			return reinterpret_cast<uint64_t>(GetModuleHandleA(name));
		};
		static const uint64_t self  = module_base(nullptr);
		static const uint64_t ntdll = module_base("ntdll.dll");
		const auto            owner = [](void* pc) {
            MEMORY_BASIC_INFORMATION info {};
            return VirtualQuery(pc, &info, sizeof(info)) != 0
			                      ? reinterpret_cast<uint64_t>(info.AllocationBase)
			                      : 0;
		};
		void*      frames[48] {};
		const auto count = CaptureStackBackTrace(0, 48, frames, nullptr);
		// This handler's frames, the exception dispatch in ntdll, then the faulting code.
		bool seen_self = false;
		for (USHORT i = 0; i < count; i++) {
			const auto base = owner(frames[i]);
			if (base == self) {
				seen_self = true;
				continue;
			}
			if (!seen_self || base != ntdll) {
				continue;
			}
			while (i + 1 < count && owner(frames[i + 1]) == ntdll) {
				i++;
			}
			if (i + 1 < count && owner(frames[i + 1]) == self) {
				return reinterpret_cast<uint64_t>(frames[i + 1]) - self;
			}
			return 0;
		}
#endif
		return 0;
	}

	void Add(bool write, uint64_t vaddr, uint64_t site, uint64_t handle_us) {
		std::lock_guard lock(m_mutex);
		auto&           page = m_pages[vaddr >> 12u];
		(write ? page.writes : page.reads)++;
		(write ? m_writes : m_reads)++;
		auto& code = m_sites[site * 2u + (write ? 1u : 0u)];
		code.count++;
		code.us += handle_us;
		const auto now = std::chrono::steady_clock::now();
		if (now - m_report < std::chrono::seconds(5)) {
			return;
		}
		std::vector<std::pair<uint64_t, Counts>> top(m_pages.begin(), m_pages.end());
		const auto                               count = std::min<size_t>(top.size(), 8);
		std::partial_sort(top.begin(), top.begin() + static_cast<ptrdiff_t>(count), top.end(),
		                  [](const auto& a, const auto& b) {
			                  return a.second.reads + a.second.writes >
			                         b.second.reads + b.second.writes;
		                  });
		::printf("Faults (5 s): %" PRIu64 " writes, %" PRIu64 " reads, %zu pages; top:", m_writes,
		         m_reads, m_pages.size());
		for (size_t i = 0; i < count; i++) {
			::printf(" 0x%" PRIx64 " w%" PRIu64 "/r%" PRIu64, top[i].first << 12u,
			         top[i].second.writes, top[i].second.reads);
		}
		::printf("\n");
		std::vector<std::pair<uint64_t, Site>> sites(m_sites.begin(), m_sites.end());
		const auto site_count = std::min<size_t>(sites.size(), 8);
		std::partial_sort(sites.begin(), sites.begin() + static_cast<ptrdiff_t>(site_count),
		                  sites.end(),
		                  [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
		::printf("Fault sites (5 s):");
		for (size_t i = 0; i < site_count; i++) {
			const auto site = sites[i].first / 2u;
			const char kind = (sites[i].first & 1u) != 0 ? 'w' : 'r';
			if (site == 0) {
				::printf(" guest/%c n%" PRIu64 " %.1f ms;", kind, sites[i].second.count,
				         static_cast<double>(sites[i].second.us) / 1000.0);
			} else {
				::printf(" +0x%" PRIx64 "/%c n%" PRIu64 " %.1f ms;", site, kind,
				         sites[i].second.count, static_cast<double>(sites[i].second.us) / 1000.0);
			}
		}
		::printf("\n");
		m_sites.clear();
		m_pages.clear();
		m_writes = 0;
		m_reads  = 0;
		m_report = now;
	}

private:
	struct Counts {
		uint64_t writes = 0;
		uint64_t reads  = 0;
	};
	struct Site {
		uint64_t count = 0;
		uint64_t us    = 0;
	};
	std::mutex                            m_mutex;
	std::unordered_map<uint64_t, Counts>  m_pages;
	std::unordered_map<uint64_t, Site>    m_sites; // key: site * 2 + write
	uint64_t                              m_writes = 0;
	uint64_t                              m_reads  = 0;
	std::chrono::steady_clock::time_point m_report = std::chrono::steady_clock::now();
};

FaultStats g_fault_stats;

} // namespace

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_bindless_table(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_texture_cache.on_bindless_unregister = [this](ImageId id, bool in_use) {
		m_bindless_table.OnImageUnregistered(id, in_use);
	};
	m_texture_cache.on_image_views_unregister = [this](std::span<const CachedImageView> views) {
		static_cast<void>(m_bindless_table.DropStaleViews(views));
	};
	// Idle until KYTY_RECORD_THREAD turns on for a command buffer (commandScheduler.cpp).
	m_command_scheduler.EnableRecordingThread();
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	KYTY_PROFILER_FUNCTION();
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	DispatchFault(access, fault_vaddr);
	return true;
}

// The caches' part of a fault: a write makes the page CPU-dirty in both caches (which untrack
// and unprotect it), a read downloads GPU-written bytes.
void RenderContext::DispatchFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	constexpr uint64_t fault_size = 1;
	const bool         stats      = FaultStats::Enabled();
	const auto         site       = stats ? FaultStats::FaultSite() : 0;
	const auto         start      = stats ? std::chrono::steady_clock::now()
	                                      : std::chrono::steady_clock::time_point {};
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	if (stats) {
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		                    std::chrono::steady_clock::now() - start)
		                    .count();
		g_fault_stats.Add(access == PageFaultAccess::Write, fault_vaddr, site,
		                  static_cast<uint64_t>(us));
	}
}

bool RenderContext::HandleWatchedUnmappedFault(PageFaultAccess access,
                                               uint64_t        fault_vaddr) noexcept {
	constexpr uint64_t fault_size = 1;
	// Writes only: a read would go through BufferCache::ReadMemory, which creates a buffer over
	// a range that is not (or no longer) GPU-mapped.
	if (access != PageFaultAccess::Write || !GuestRange {fault_vaddr, fault_size}.Valid() ||
	    IsMapped(fault_vaddr, fault_size) || PageWatchers(fault_vaddr) == 0u) {
		return false;
	}
	static std::atomic<uint32_t> reported = 0;
	if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
		std::printf("GPU fault: write of watched page 0x%016" PRIx64
		            " outside the GPU-mapped ranges, sent to the caches\n",
		            static_cast<uint64_t>(fault_vaddr & ~uint64_t {0xfff}));
		std::fflush(stdout);
	}
	DispatchFault(access, fault_vaddr);
	// Recovered only if a cache let go of the page; stale watcher counts no cache owns go on to
	// the orphan repair.
	return PageWatchers(fault_vaddr) == 0u;
}

uint32_t RenderContext::PageWatchers(uint64_t fault_vaddr) const noexcept {
	uint32_t write  = 0;
	uint32_t access = 0;
	m_page_manager.Watchers(fault_vaddr, write, access);
	return write + access;
}

void RenderContext::DescribeFaultPage(uint64_t fault_vaddr) const noexcept {
	uint32_t write  = 0;
	uint32_t access = 0;
	m_page_manager.Watchers(fault_vaddr, write, access);
	const bool mapped = GuestRange {fault_vaddr, 1}.Valid() && IsMapped(fault_vaddr, 1);
	std::printf("tracker page 0x%016" PRIx64 ": write watchers=%u access watchers=%u, %s\n",
	            static_cast<uint64_t>(fault_vaddr & ~uint64_t {0xfff}), write, access,
	            mapped ? "GPU-mapped" : "not in the GPU-mapped ranges");
	const uint64_t guest_page = fault_vaddr & ~uint64_t {0x3fff};
	for (uint64_t page = guest_page; page < guest_page + 0x4000u; page += 0x1000u) {
		m_page_manager.Watchers(page, write, access);
		std::printf("  4 KiB page 0x%016" PRIx64 ": write=%u access=%u\n", page, write, access);
	}
	std::fflush(stdout);
}

bool RenderContext::CanServeCleanRead(uint64_t fault_vaddr, uint64_t vaddr,
                                      uint64_t size) const noexcept {
	return IsMapped(vaddr, size) && m_page_manager.IsReadWatched(fault_vaddr) &&
	       m_buffer_cache.IsCleanForConcurrentRead(vaddr, size);
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	// Every cached buffer is synchronized so a global-memory shader sees the CPU's writes.
	// That walk touched every buffer per dispatch (a quarter of the GPU thread in the
	// world); it only has to repeat once something became CPU-dirty or a buffer appeared.
	// The walk runs at most once per guest submission: what the game wrote before submitting is
	// visible to all its commands, as on the console, and the GPU thread runs behind the game
	// anyway. Per draw, the walk and the re-protection it triggers took ~38 % of the GPU thread
	// on the title menu (21 -> 32 fps).
	const auto epoch = g_cpu_dirty_epoch.load(std::memory_order_acquire);
	if (epoch != m_bda_synced_epoch) {
		const auto submission = g_guest_submission_seq.load(std::memory_order_relaxed);
		if (submission == m_bda_synced_submission) {
			m_fault_process_pending = true;
			return;
		}
		m_bda_synced_submission = submission;
	}
	if (epoch != m_bda_synced_epoch) {
		// The guest writes somewhere nearly all the time, so the epoch moves between most
		// dispatches; walk only the regions holding CPU-dirty pages, not every buffer.
		std::shared_lock lock(m_mapped_ranges_mutex);
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeCpuDirtyBuffersInRange(start, end - start);
		});
		m_bda_synced_epoch = epoch;
	}
	m_fault_process_pending = true;
}

Common::LockGuard RenderContext::LockMutexProfiled() {
	KYTY_PROFILER_BLOCK("RenderContext::WaitMutex");
	return Common::LockGuard(m_mutex);
}

// KYTY_VRAM_REPORT=N (live, seconds, default 30; 0 = off): one line splitting the device memory
// by owner, to direct memory work at the largest consumer:
//   device = what the driver counts for the process, of its budget;
//   blocks/allocations = VMA's device-local memory blocks and what is allocated inside them (the
//     difference is free space trapped in partly used blocks);
//   images = texture cache (VMA sizes; guest-accounted in brackets), buffers = buffer cache,
//   pool = parked deleted images, retiring = images and buffers held by the retire switches.
void RenderContext::ReportVram() {
	static auto& interval = Common::LiveSwitches::Get("KYTY_VRAM_REPORT", 30);
	const auto   seconds  = interval.load(std::memory_order_relaxed);
	if (seconds <= 0) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (m_vram_report_time != std::chrono::steady_clock::time_point {} &&
	    now - m_vram_report_time < std::chrono::seconds(seconds)) {
		return;
	}
	const bool first   = m_vram_report_time == std::chrono::steady_clock::time_point {};
	m_vram_report_time = now;
	if (first) {
		return;
	}
	auto&          graphics    = m_texture_cache.Graphics();
	uint64_t       allocations = 0;
	uint64_t       blocks      = 0;
	graphics.GetDeviceAllocationStats(allocations, blocks);
	constexpr double MiB = 1024.0 * 1024.0;
	std::printf("VRAM: device %.0f of %.0f MiB | blocks %.0f, allocations %.0f (trapped %.0f) | "
	            "images %zu %.0f [%.0f guest] | buffers %zu %.0f | pool %.0f | retiring %.0f MiB | "
	            "graveyard %.0f MiB (freed early %" PRIu64 ")\n",
	            static_cast<double>(graphics.GetDeviceMemoryUsage()) / MiB,
	            static_cast<double>(graphics.GetDriverMemoryBudget()) / MiB,
	            static_cast<double>(blocks) / MiB, static_cast<double>(allocations) / MiB,
	            static_cast<double>(blocks - std::min(blocks, allocations)) / MiB,
	            m_texture_cache.ImageCount(), static_cast<double>(m_texture_cache.HostMemory()) / MiB,
	            static_cast<double>(m_texture_cache.UsedMemory()) / MiB,
	            m_buffer_cache.BufferCount(), static_cast<double>(m_buffer_cache.UsedMemory()) / MiB,
	            static_cast<double>(graphics.ParkedImageBytes()) / MiB,
	            static_cast<double>(m_texture_cache.RetiringBytes() +
	                                m_buffer_cache.RetiringBytes()) /
	                MiB,
	            static_cast<double>(GraphicContext::GraveyardBytes()) / MiB,
	            GraphicContext::GraveyardFreedEarly());
	std::fflush(stdout);
}

void RenderContext::RunGarbageCollector() {
	KYTY_PROFILER_FUNCTION();
	ReportVram();
	m_pipeline_cache.MaybeCheckpoint();
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.ProcessImageDump();
	// KYTY_GC_INTERVAL_US (live, default 4000; 0 = every call): collect at most once per interval.
	// This runs after every completed guest submission (~1300/s at the Wolverine spot) while both
	// collectors age in frames and budget per frame. Run 31 A/B 0/4000: collectors 16.1 -> 4.0
	// ms/s, mean frame 73.3 -> 72.6 ms; still ~250 collections per second.
	static auto& gc_interval = Common::LiveSwitches::Get("KYTY_GC_INTERVAL_US", 4000);
	const auto   interval    = gc_interval.load(std::memory_order_relaxed);
	const auto   now_us      = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (interval <= 0 || now_us - m_last_gc_us >= interval) {
		m_last_gc_us = now_us;
		m_texture_cache.RunGarbageCollector();
		m_buffer_cache.RunGarbageCollector();
	}

	// KYTY_MEMORY_STATS=1: every 5 s, what each cache holds against the device budget.
	static auto& stats       = Common::LiveSwitches::Get("KYTY_MEMORY_STATS", 0);
	static auto  report_time = std::chrono::steady_clock::now();
	if (stats.load(std::memory_order_relaxed) != 0) {
		const auto now = std::chrono::steady_clock::now();
		if (now - report_time >= std::chrono::seconds(5)) {
			report_time          = now;
			constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
			uint64_t allocation_bytes = 0;
			uint64_t block_bytes      = 0;
			m_graphics.GetDeviceAllocationStats(allocation_bytes, block_bytes);
			::printf("Memory: buffers %.2f GiB, images %.2f GiB, device usage %.2f GiB, "
			         "budget %.2f GiB (driver %.2f GiB), VMA allocations %.2f GiB in blocks %.2f "
			         "GiB\n",
			         static_cast<double>(m_buffer_cache.UsedMemory()) / GiB,
			         static_cast<double>(m_texture_cache.UsedMemory()) / GiB,
			         static_cast<double>(m_graphics.GetDeviceMemoryUsage()) / GiB,
			         static_cast<double>(m_graphics.GetTotalMemoryBudget()) / GiB,
			         static_cast<double>(m_graphics.GetDriverMemoryBudget()) / GiB,
			         static_cast<double>(allocation_bytes) / GiB,
			         static_cast<double>(block_bytes) / GiB);
			std::fflush(stdout);
			m_graphics.LogMemoryBudget();
		}
	}
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
