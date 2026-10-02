#include "graphics/host_gpu/addressBindingReport.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace {

struct Range {
	uint64_t size        = 0;
	uint64_t handle      = 0;
	uint32_t object_type = 0;
	int64_t  bound_us    = 0;
	int64_t  released_us = 0;
};

struct ObjectInfo {
	std::string              description;
	std::deque<std::string>  events; // newest last
};

constexpr size_t ReleasedHistory = 65536;
constexpr size_t EventsPerObject = 8;
constexpr size_t ObjectHistory   = 65536;

std::mutex                                g_mutex;
std::map<uint64_t, Range>                 g_live;     // by base address
std::deque<std::pair<uint64_t, Range>>    g_released; // oldest first
std::unordered_map<uint64_t, ObjectInfo>  g_objects;
std::deque<uint64_t>                      g_object_order;

int64_t NowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

const char* ObjectTypeName(uint32_t type) {
	// VkObjectType values.
	switch (type) {
		case 8: return "memory";
		case 9: return "buffer";
		case 10: return "image";
		default: return "object";
	}
}

void PrintObject(uint64_t handle, int64_t now) {
	const auto found = g_objects.find(handle);
	if (found == g_objects.end()) {
		return;
	}
	if (!found->second.description.empty()) {
		std::printf("      %s\n", found->second.description.c_str());
	}
	for (const auto& event: found->second.events) {
		std::printf("      event: %s\n", event.c_str());
	}
	(void)now;
}

ObjectInfo& Object(uint64_t handle) {
	auto [it, inserted] = g_objects.try_emplace(handle);
	if (inserted) {
		g_object_order.push_back(handle);
		if (g_object_order.size() > ObjectHistory) {
			g_objects.erase(g_object_order.front());
			g_object_order.pop_front();
		}
	}
	return it->second;
}

} // namespace

bool AddressBindingReportRequested() {
	static const bool requested = [] {
		const char* value = std::getenv("KYTY_ADDRESS_BINDING_REPORT");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return requested;
}

void AddressBindingNote(uint32_t object_type, uint64_t handle, uint64_t base, uint64_t size,
                        bool bind) {
	std::scoped_lock lock {g_mutex};
	const auto       now = NowUs();
	if (bind) {
		g_live[base] = {size, handle, object_type, now, 0};
		return;
	}
	if (auto found = g_live.find(base); found != g_live.end()) {
		found->second.released_us = now;
		g_released.emplace_back(*found);
		g_live.erase(found);
	} else {
		g_released.emplace_back(base, Range {size, handle, object_type, 0, now});
	}
	if (g_released.size() > ReleasedHistory) {
		g_released.pop_front();
	}
}

void AddressBindingDescribeObject(uint64_t handle, std::string description) {
	std::scoped_lock lock {g_mutex};
	auto&            object = Object(handle);
	object.description      = std::move(description);
	object.events.clear();
}

void AddressBindingAnnotate(uint64_t handle, const char* event) {
	std::scoped_lock lock {g_mutex};
	auto&            object = Object(handle);
	char             text[96];
	std::snprintf(text, sizeof(text), "%s at t=%.3f s", event,
	              static_cast<double>(NowUs()) / 1e6);
	object.events.emplace_back(text);
	if (object.events.size() > EventsPerObject) {
		object.events.pop_front();
	}
}

void AddressBindingDescribe(uint64_t address) {
	std::scoped_lock lock {g_mutex};
	const auto       now   = NowUs();
	bool             found = false;
	if (auto it = g_live.upper_bound(address); it != g_live.begin()) {
		--it;
		if (address - it->first < it->second.size) {
			std::printf("    binding report: LIVE %s 0x%" PRIx64 " va=0x%" PRIx64 " size=0x%" PRIx64
			            ", bound %.1f ms ago\n",
			            ObjectTypeName(it->second.object_type), it->second.handle, it->first,
			            it->second.size,
			            static_cast<double>(now - it->second.bound_us) / 1000.0);
			PrintObject(it->second.handle, now);
			found = true;
		}
	}
	// The most recent releases first; an address range is reused, so report the last few owners.
	int reported = 0;
	for (auto it = g_released.rbegin(); it != g_released.rend() && reported < 3; ++it) {
		if (address >= it->first && address - it->first < it->second.size) {
			std::printf("    binding report: RELEASED %s 0x%" PRIx64 " va=0x%" PRIx64
			            " size=0x%" PRIx64 ", released %.1f ms ago (lived %.1f ms)\n",
			            ObjectTypeName(it->second.object_type), it->second.handle, it->first,
			            it->second.size,
			            static_cast<double>(now - it->second.released_us) / 1000.0,
			            it->second.bound_us != 0 ? static_cast<double>(it->second.released_us -
			                                                           it->second.bound_us) /
			                                           1000.0
			                                     : -1.0);
			PrintObject(it->second.handle, now);
			found = true;
			reported++;
		}
	}
	if (!found) {
		std::printf("    binding report: no object ever bound there (%zu live, %zu released kept)\n",
		            g_live.size(), g_released.size());
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
