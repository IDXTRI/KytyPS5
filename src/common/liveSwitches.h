#ifndef KYTY_COMMON_LIVESWITCHES_H_
#define KYTY_COMMON_LIVESWITCHES_H_

#include <atomic>
#include <cstdint>

namespace Common::LiveSwitches {

// A named integer switch for A/B measurements in one process. It starts from the environment
// variable of the same name (a variable set without a number counts as 1), else from
// default_value. When KYTY_LIVE_FILE names a text file, a background thread re-reads it after
// each change; every "NAME=value" line updates that switch, prints the change and marks it in
// the profiler timeline. Read switches with relaxed loads, and call Get once per call site:
//   static auto& spin = Common::LiveSwitches::Get("KYTY_EXAMPLE", 0);
std::atomic<int64_t>& Get(const char* name, int64_t default_value);

} // namespace Common::LiveSwitches

#endif /* KYTY_COMMON_LIVESWITCHES_H_ */
