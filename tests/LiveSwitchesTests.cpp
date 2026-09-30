#include "common/liveSwitches.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "LiveSwitchesTests: failed: %s\n", message);
    std::abort();
  }
}

void SetEnv(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void WriteFile(const std::filesystem::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::trunc);
  file << text;
}

// The file is polled every 250 ms; allow a generous margin on a loaded machine.
bool WaitFor(const std::atomic<int64_t> &value, int64_t expected) {
  for (int i = 0; i < 100; i++) {
    if (value.load() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

} // namespace

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "kyty_live_switches_test.txt";
  WriteFile(path, "KYTY_TEST_EARLY=4\n");
  // Both must be set before the first Get(), which starts the file watcher.
  SetEnv("KYTY_LIVE_FILE", path.string().c_str());
  SetEnv("KYTY_TEST_ENV", "7");
  SetEnv("KYTY_TEST_WORD", "on");

  auto &from_env = Common::LiveSwitches::Get("KYTY_TEST_ENV", 0);
  Check(from_env.load() == 7, "the environment value was not used");
  Check(Common::LiveSwitches::Get("KYTY_TEST_WORD", 0).load() == 1,
        "a set variable without a number is not 1");
  Check(Common::LiveSwitches::Get("KYTY_TEST_UNSET", 42).load() == 42,
        "an unset switch did not keep its default");
  Check(&Common::LiveSwitches::Get("KYTY_TEST_ENV", 0) == &from_env,
        "a second Get returned a different switch");

  auto &early = Common::LiveSwitches::Get("KYTY_TEST_EARLY", 0);
  Check(WaitFor(early, 4), "the initial file value was not applied");

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  WriteFile(
      path,
      "# comment\n  KYTY_TEST_EARLY = 5  \nnot a switch\nKYTY_TEST_LATE=9\n"
      "KYTY_TEST_ENV=0x10\n");
  Check(WaitFor(early, 5), "a changed file value was not applied");
  Check(WaitFor(from_env, 16), "a file value did not override the environment");

  // A value read before any call site registered the switch is used when it
  // registers.
  auto &late = Common::LiveSwitches::Get("KYTY_TEST_LATE", 0);
  Check(late.load() == 9,
        "a switch registered after the file was read missed its value");

  std::error_code error;
  std::filesystem::remove(path, error);
  std::printf("LiveSwitchesTests: OK\n");
  return 0;
}
