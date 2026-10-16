#ifndef KYTY_COMMON_SPINBACKOFF_H_
#define KYTY_COMMON_SPINBACKOFF_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Common {

// Waiting in a spin lock. The tracking locks spun on test_and_set with no pause: every
// iteration asked for the cache line exclusively (slowing the holder, which writes the same
// line) and a waiter burned a whole core while the holder sat in a long section (the texture
// cache lock across a collection, the page manager's lock across a VirtualProtect call).
// Now: test-and-test-and-set, a CPU pause per iteration, and after a short burst the thread
// yields its time slice. KYTY_SPIN_BACKOFF=0 (environment) restores the plain spin.
inline void CpuRelax() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
	__asm__ __volatile__("yield");
#endif
}

inline bool SpinBackoffEnabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SPIN_BACKOFF");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

class SpinBackoff {
public:
	// Pauses ~1-2 us for the first iterations (a lock released soon is taken without a context
	// switch), then yields.
	static constexpr uint32_t SpinsBeforeYield = 128;

	void Wait() noexcept {
		if (m_spins < SpinsBeforeYield) {
			m_spins++;
			CpuRelax();
		} else {
			std::this_thread::yield();
		}
	}
	[[nodiscard]] uint32_t Spins() const noexcept { return m_spins; }

private:
	uint32_t m_spins = 0;
};

// Acquires an atomic_flag spin lock.
inline void SpinAcquire(std::atomic_flag& flag) noexcept {
	if (!flag.test_and_set(std::memory_order_acquire)) {
		return;
	}
	if (!SpinBackoffEnabled()) {
		while (flag.test_and_set(std::memory_order_acquire)) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
		return;
	}
	SpinBackoff backoff;
	do {
		while (flag.test(std::memory_order_relaxed)) {
			backoff.Wait();
		}
	} while (flag.test_and_set(std::memory_order_acquire));
}

} // namespace Common

#endif // KYTY_COMMON_SPINBACKOFF_H_
