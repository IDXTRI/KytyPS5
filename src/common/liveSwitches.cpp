#include "common/liveSwitches.h"

#include "common/profiler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Common::LiveSwitches {

namespace {

struct Registry {
	std::mutex                                                   mutex;
	std::map<std::string, std::unique_ptr<std::atomic<int64_t>>> switches;
	// Values read from the file for switches that no call site has registered yet.
	std::map<std::string, int64_t> file_values;
};

// Never destroyed: the polling thread may still run while the process exits.
Registry& GetRegistry() {
	static auto* registry = new Registry;
	return *registry;
}

// A set value without a number (for example "KYTY_X=" or "KYTY_X=on") counts as 1.
int64_t ParseValue(const std::string& text) {
	char*      end   = nullptr;
	const auto value = std::strtoll(text.c_str(), &end, 0);
	return end == text.c_str() ? 1 : value;
}

void Announce(const std::string& name, int64_t value) {
	char      text[160];
	const int length = std::snprintf(text, sizeof(text), "Live switch %s=%lld", name.c_str(),
	                                 static_cast<long long>(value));
	std::printf("%s\n", text);
	std::fflush(stdout);
	if (length > 0 && tracy::ProfilerAvailable()) {
		TracyMessage(text, static_cast<size_t>(length));
	}
}

std::string Trim(const std::string& text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return {};
	}
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

void ReadFile(const std::filesystem::path& path) {
	std::ifstream   file(path);
	std::string     line;
	auto&           registry = GetRegistry();
	std::lock_guard lock(registry.mutex);
	while (std::getline(file, line)) {
		line                 = Trim(line);
		const auto separator = line.find('=');
		if (line.empty() || line[0] == '#' || separator == std::string::npos) {
			continue;
		}
		const auto name            = Trim(line.substr(0, separator));
		const auto value           = ParseValue(Trim(line.substr(separator + 1)));
		registry.file_values[name] = value;
		if (const auto found = registry.switches.find(name);
		    found != registry.switches.end() && found->second->exchange(value) != value) {
			Announce(name, value);
		}
	}
}

void PollFile(const std::filesystem::path& path) {
	std::filesystem::file_time_type last_write {};
	for (;;) {
		std::error_code error;
		const auto      write_time = std::filesystem::last_write_time(path, error);
		if (!error && write_time != last_write) {
			last_write = write_time;
			ReadFile(path);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

void StartPolling() {
	static std::once_flag once;
	std::call_once(once, [] {
		const char* path = std::getenv("KYTY_LIVE_FILE");
		if (path == nullptr || *path == '\0') {
			return;
		}
		std::printf("Live switches: watching %s\n", path);
		std::fflush(stdout);
		std::thread(PollFile, std::filesystem::path(path)).detach();
	});
}

} // namespace

namespace {

// KYTY_VRAM_SAVER=1|2 (environment): defaults for a group of memory switches, so one variable
// selects a low-VRAM setup (an explicit variable or live-file line still wins).
// 1 = settings that only change how the caches count, place and keep memory;
// 2 = also smaller VMA blocks, eviction of unsampled bindless textures, and the retire safety net
//     for deleted images and buffers: the large savings that the stale-read device loss
//     (PLAN P3) has blocked so far. Experimental.
struct Preset {
	const char* name;
	int64_t     value;
	int         level;
};
constexpr Preset VramSaverPresets[] = {
    {"KYTY_GC_HOST_SIZES", 1, 1},           {"KYTY_VMA_MIN_MEMORY", 1, 1},
    {"KYTY_IMAGE_RECYCLE_MB", 64, 1},       {"KYTY_IMAGE_POOL_BUDGET", 1, 1},
    {"KYTY_GC_DRIVER_MARGIN_MB", 768, 1},
    {"KYTY_VMA_BLOCK_MB", 64, 2},           {"KYTY_BINDLESS_EVICT", 1, 2},
    {"KYTY_BINDLESS_EVICT_TIGHT_AGE", 120, 2}, {"KYTY_IMAGE_RETIRE_FRAMES", 90, 2},
    {"KYTY_BUFFER_RETIRE_FRAMES", 90, 2},  {"KYTY_BUFFER_POOLED_KB", 1024, 2},
};

int VramSaverLevel() {
	static const int level = [] {
		const char* value = std::getenv("KYTY_VRAM_SAVER");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	return level;
}

} // namespace

int64_t PresetValue(const char* name, int64_t default_value, int level) {
	if (level <= 0) {
		return default_value;
	}
	for (const auto& preset: VramSaverPresets) {
		if (preset.level <= level && std::strcmp(preset.name, name) == 0) {
			return preset.value;
		}
	}
	return default_value;
}

namespace {
int64_t PresetDefault(const char* name, int64_t default_value) {
	return PresetValue(name, default_value, VramSaverLevel());
}
} // namespace

std::atomic<int64_t>& Get(const char* name, int64_t default_value) {
	StartPolling();
	const auto code_default = default_value;
	default_value           = PresetDefault(name, default_value);
	auto&           registry = GetRegistry();
	std::lock_guard lock(registry.mutex);
	auto&           slot = registry.switches[name];
	if (!slot) {
		int64_t value = default_value;
		if (const char* env = std::getenv(name); env != nullptr) {
			value = ParseValue(env);
		}
		if (const auto found = registry.file_values.find(name);
		    found != registry.file_values.end()) {
			value = found->second;
		}
		slot = std::make_unique<std::atomic<int64_t>>(value);
		if (value != code_default) {
			Announce(name, value);
		}
	}
	return *slot;
}

} // namespace Common::LiveSwitches
