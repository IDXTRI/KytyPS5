#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit(uint64_t tick) {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search = [this, &gpu_tick, tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = tick;
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = tick;
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

// KYTY_GPU_TIME=1 (a live switch, off by default): every command buffer gets a timestamp at its
// start and end. When its tick completes, the GPU time it covered (overlaps with the previous
// command buffer removed) is added up, and every 5 s the log reports how busy the GPU was.
struct CommandScheduler::GpuTimer {
	static constexpr uint32_t Slots = 4096;

	explicit GpuTimer(GraphicContext& graphics): graphics(graphics) {
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = Slots * 2;
		if (graphics.device.createQueryPool(&info, nullptr, &pool) != vk::Result::eSuccess) {
			pool = nullptr;
		}
		period_ns = graphics.GetPhysicalDeviceProperties().limits.timestampPeriod;
	}
	~GpuTimer() {
		if (pool) {
			graphics.device.destroyQueryPool(pool, nullptr);
		}
	}
	KYTY_CLASS_NO_COPY(GpuTimer);

	static bool Enabled() {
		static auto& enabled = Common::LiveSwitches::Get("KYTY_GPU_TIME", 0);
		return enabled.load(std::memory_order_relaxed) != 0;
	}

	// GPU thread, a new command buffer.
	void Begin(vk::CommandBuffer command) {
		open_slot = UINT32_MAX;
		if (!pool || !Enabled() || pending.size() >= Slots) {
			return;
		}
		open_slot = next_slot++ % Slots;
		command.resetQueryPool(pool, open_slot * 2, 2);
		command.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, pool, open_slot * 2);
	}

	// GPU thread, before the command buffer ends.
	void End(vk::CommandBuffer command) {
		if (open_slot != UINT32_MAX) {
			command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, pool,
			                        open_slot * 2 + 1);
		}
	}

	void Submitted(uint64_t tick) {
		if (open_slot != UINT32_MAX) {
			pending.push_back({tick, open_slot});
			open_slot = UINT32_MAX;
		}
	}

	template <typename IsFree>
	void Collect(IsFree&& is_free) {
		while (!pending.empty() && is_free(pending.front().tick)) {
			const auto              slot = pending.front().slot;
			std::array<uint64_t, 2> stamps {};
			if (graphics.device.getQueryPoolResults(
			        pool, slot * 2, 2, sizeof(stamps), stamps.data(), sizeof(uint64_t),
			        vk::QueryResultFlagBits::e64) == vk::Result::eSuccess &&
			    stamps[1] >= stamps[0]) {
				const auto begin = std::max(stamps[0], last_end);
				if (stamps[1] > begin) {
					busy_ns += static_cast<double>(stamps[1] - begin) * period_ns;
				}
				last_end = std::max(last_end, stamps[1]);
				buffers++;
			}
			pending.pop_front();
		}
		const auto now     = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration<double>(now - report).count();
		if (elapsed >= 5.0) {
			if (buffers != 0) {
				const double busy_ms_per_s = busy_ns / 1e6 / elapsed;
				::printf("GPU time (%.1f s): busy %.1f ms/s (%.0f%%), %" PRIu64
				         " command buffers\n",
				         elapsed, busy_ms_per_s, busy_ms_per_s / 10.0, buffers);
				std::fflush(stdout);
				if (tracy::ProfilerAvailable()) {
					TracyPlot("GPU busy ms/s", busy_ms_per_s);
				}
			}
			busy_ns = 0;
			buffers = 0;
			report  = now;
		}
	}

	struct Pending {
		uint64_t tick = 0;
		uint32_t slot = 0;
	};

	GraphicContext&                       graphics;
	vk::QueryPool                         pool      = nullptr;
	float                                 period_ns = 1.0f;
	uint32_t                              next_slot = 0;
	uint32_t                              open_slot = UINT32_MAX;
	std::deque<Pending>                   pending;
	uint64_t                              last_end = 0;
	double                                busy_ns  = 0;
	uint64_t                              buffers  = 0;
	std::chrono::steady_clock::time_point report   = std::chrono::steady_clock::now();
};

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_gpu_timer(std::make_unique<GpuTimer>(graphics)), m_master(graphics), m_context(context),
      m_graphics(graphics), m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	StopRecordingThread();
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	KYTY_PROFILER_FUNCTION();
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	KYTY_PROFILER_FUNCTION();
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	KYTY_PROFILER_FUNCTION();
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_priority_operations.push({std::move(operation), CurrentTick()});
		lock.unlock();
		m_operation_available.notify_one();
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	static auto& record_thread = Common::LiveSwitches::Get("KYTY_RECORD_THREAD", 0);
	const bool   threaded =
	    m_record_thread.joinable() && record_thread.load(std::memory_order_relaxed) != 0;
	if (!threaded && m_record_thread.joinable()) {
		// The recording thread may still be ending or submitting the previous command buffer, and
		// the pool must not be used by two threads.
		(void)DrainRecording();
	}
	m_threaded           = threaded;
	m_command.m_threaded = threaded;
	m_command.m_open     = true;
	if (threaded) {
		BeginThreadedCommand();
		return m_command;
	}
	m_command.m_buffer = m_command_pool.Commit(m_master.CurrentTick());
	m_command.Begin();
	m_gpu_timer->Collect([this](uint64_t tick) { return IsFree(tick); });
	m_gpu_timer->Begin(m_command.m_buffer);
	return m_command;
}

// The final barrier and the queue submission of a command buffer, for both the GPU thread and
// the recording thread.
struct CommandScheduler::SubmitDebug {
	uint32_t op        = 0;
	uint64_t submit_id = 0;
	uint32_t arg0 = 0, arg1 = 0, arg2 = 0, arg3 = 0;
	uint64_t arg4 = 0;
};

void CommandScheduler::QueueSubmit(vk::CommandBuffer buffer, SubmitInfo& submit, uint64_t tick,
                                   const SubmitDebug& debug) {
	auto& graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);
	vk::Result result;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}
	if (result == vk::Result::eErrorDeviceLost) {
		DumpDeviceLossDiagnostics(graphics);
	}
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, debug.op, debug.submit_id, debug.arg0,
		                  debug.arg1, debug.arg2, debug.arg3, debug.arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	m_gpu_timer->Submitted(tick);
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	if (AnyMappedDeviceBuffer()) {
		// Buffers in host-visible device memory are read by the host once this submission's
		// tick completes (BufferCache direct readback): make its writes visible to the host.
		m_command.EndRendering();
		vk::MemoryBarrier2 barrier {};
		barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
		barrier.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
		barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
		vk::DependencyInfo dependency {};
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers    = &barrier;
		m_command.Recorder().pipelineBarrier2(dependency);
	}
	m_command.EndRendering();
	const SubmitDebug debug {m_command.m_debug_op,   m_command.m_debug_submit_id,
	                         m_command.m_debug_arg0, m_command.m_debug_arg1,
	                         m_command.m_debug_arg2, m_command.m_debug_arg3,
	                         m_command.m_debug_arg4};
	// The time this thread hands the command buffer over: the gates on it (KYTY_LABEL_FLUSH_US,
	// KYTY_SLICE_FLUSH_US) measure this thread's batching, not when the driver call happens.
	m_last_submit_us.store(std::chrono::duration_cast<std::chrono::microseconds>(
	                           std::chrono::steady_clock::now().time_since_epoch())
	                           .count(),
	                       std::memory_order_relaxed);
	if (m_threaded) {
		return SubmitThreaded(submit, debug);
	}
	m_gpu_timer->End(m_command.m_buffer);
	m_command.End();
	uint64_t tick = 0;
	{
		// The tick is taken under the queue lock in this path, as before.
		Common::LockGuard lock(m_graphics.queue_mutex);
		tick = m_master.NextTick();
	}
	QueueSubmit(m_command.m_buffer, submit, tick, debug);
	m_command.m_buffer = nullptr;
	m_command.m_open   = false;
	return tick;
}

// Research: KYTY_RECORD_THREAD=1 (live, per command buffer; default 0). Thread_Gpu spent ~15-20 %
// of its time inside the Vulkan driver recording and submitting. With the switch on, the
// renderer's scheduler hands the Vulkan calls of the current command buffer to a recording
// thread: CommandRecorder (Recorder()) and Record() queue them in order, with every array they
// point to copied into the command chunk; the recording thread owns the command pool, begins,
// ends and submits the command buffers. Ticks are still assigned here, in order, when a command
// buffer is submitted, so everything keyed by ticks is unchanged; a host wait on a tick the
// recording thread has not submitted yet just waits longer (timeline semaphores allow it).
// Code that still records through the raw Handle() first waits for the recording thread to
// catch up (DrainRecording), so it stays correct, only slower.
void CommandScheduler::EnableRecordingThread() {
	EXIT_IF(m_record_thread.joinable());
	m_record_thread = std::jthread([this](std::stop_token stop) { RecordingThread(stop); });
}

void CommandScheduler::BeginThreadedCommand() {
	static std::atomic_bool announced = false;
	if (!announced.exchange(true, std::memory_order_relaxed)) {
		::printf("Record thread: first threaded command buffer\n");
		std::fflush(stdout);
	}
	m_command.m_buffer = nullptr;
	m_command.InvalidateDynamicState();
	const auto tick = m_master.CurrentTick();
	Record([this, tick](vk::CommandBuffer) {
		m_worker_buffer = m_command_pool.Commit(tick);
		vk::CommandBufferBeginInfo begin_info {};
		begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		EXIT_NOT_IMPLEMENTED(m_worker_buffer.begin(&begin_info) != vk::Result::eSuccess);
		m_gpu_timer->Collect([this](uint64_t done) { return IsFree(done); });
		m_gpu_timer->Begin(m_worker_buffer);
	});
}

uint64_t CommandScheduler::SubmitThreaded(SubmitInfo submit, const SubmitDebug& debug) {
	const auto tick = m_master.NextTick();
	Record([this, submit, tick, debug](vk::CommandBuffer command) mutable {
		m_gpu_timer->End(command);
		EXIT_NOT_IMPLEMENTED(command.end() != vk::Result::eSuccess);
		QueueSubmit(command, submit, tick, debug);
		m_worker_buffer = nullptr;
	});
	DispatchChunk();
	m_command.m_buffer = nullptr;
	m_command.m_open   = false;

	// Every 5 s: threaded command buffers, chunks handed over, and drains (raw Handle() uses,
	// which make Thread_Gpu wait for the recording thread: the sites still to convert).
	m_record_stats.buffers++;
	const auto now = std::chrono::steady_clock::now();
	if (now - m_record_stats.report >= std::chrono::seconds(5)) {
		uint64_t chunks = 0;
		{
			std::lock_guard lock(m_record_mutex);
			chunks = m_chunks_dispatched;
		}
		::printf("Record thread (5 s): %" PRIu64 " command buffers, %" PRIu64 " chunks, %" PRIu64
		         " drains\n",
		         m_record_stats.buffers, chunks - m_record_stats.chunks, m_record_stats.drains);
		std::fflush(stdout);
		m_record_stats = {.chunks = chunks, .report = now};
	}
	return tick;
}

void* CommandScheduler::Allocate(size_t size, size_t alignment) {
	EXIT_IF(!m_threaded || size > Chunk::Size);
	const auto take = [this] {
		std::lock_guard lock(m_record_mutex);
		if (m_free_chunks.empty()) {
			return std::make_unique<Chunk>();
		}
		auto chunk = std::move(m_free_chunks.back());
		m_free_chunks.pop_back();
		return chunk;
	};
	if (m_chunk == nullptr) {
		m_chunk = take();
	}
	auto offset = (m_chunk->used + alignment - 1) & ~(alignment - 1);
	if (offset + size > Chunk::Size) {
		DispatchChunk();
		m_chunk = take();
		offset  = 0;
	}
	m_chunk->used = offset + size;
	return m_chunk->storage + offset;
}

void CommandScheduler::DispatchChunk() {
	if (m_chunk == nullptr || m_chunk->used == 0) {
		return;
	}
	{
		std::lock_guard lock(m_record_mutex);
		m_record_queue.push_back(std::move(m_chunk));
		m_chunks_dispatched++;
	}
	m_record_available.notify_one();
}

vk::CommandBuffer CommandScheduler::DrainRecording() {
	if (!m_record_thread.joinable()) {
		return m_command.m_buffer;
	}
	KYTY_PROFILER_FUNCTION();
	if (m_threaded) {
		m_record_stats.drains++;
	}
	DispatchChunk();
	std::unique_lock lock(m_record_mutex);
	m_record_executed.wait(lock, [this] { return m_chunks_executed == m_chunks_dispatched; });
	return m_worker_buffer;
}

void CommandScheduler::RecordingThread(std::stop_token stop) {
	KYTY_PROFILER_THREAD("Thread_GpuRecord");
	// A command's stashed arrays may sit in up to two chunks before its own: executed chunks are
	// recycled only after the next two have run.
	std::deque<std::unique_ptr<Chunk>> retained;
	for (;;) {
		std::unique_ptr<Chunk> chunk;
		{
			std::unique_lock lock(m_record_mutex);
			if (!m_record_available.wait(lock, stop, [this] { return !m_record_queue.empty(); })) {
				return;
			}
			chunk = std::move(m_record_queue.front());
			m_record_queue.pop_front();
		}
		for (auto* command = chunk->first; command != nullptr;) {
			auto* next = command->next;
			command->Execute(m_worker_buffer);
			command->~RecordedCommand();
			command = next;
		}
		chunk->first = nullptr;
		chunk->last  = nullptr;
		chunk->used  = 0;
		retained.push_back(std::move(chunk));
		std::unique_ptr<Chunk> recycled;
		if (retained.size() > 2) {
			recycled = std::move(retained.front());
			retained.pop_front();
		}
		{
			std::lock_guard lock(m_record_mutex);
			if (recycled != nullptr) {
				m_free_chunks.push_back(std::move(recycled));
			}
			m_chunks_executed++;
		}
		m_record_executed.notify_all();
	}
}

void CommandScheduler::StopRecordingThread() {
	if (!m_record_thread.joinable()) {
		return;
	}
	(void)DrainRecording();
	m_record_thread.request_stop();
	m_record_available.notify_all();
	m_record_thread.join();
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
