// SPDX-License-Identifier: Apache-2.0
#include "groomlab/profile.h"

#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace groomlab {

namespace {

bool profileFromEnvironment() {
    const char* text = std::getenv("GROOMLAB_PROFILE");  // NOLINT(concurrency-mt-unsafe): read once, before main
    return text && *text && std::string(text) != "0";
}

struct ProfileTable {
    std::mutex mutex;
    std::vector<ProfileEntry> rows;                     // in first-recorded order
    std::unordered_map<std::string, std::size_t> index;  // label -> its row
};

ProfileTable& table() {
    static ProfileTable t;
    return t;
}

}  // namespace

namespace detail {
std::atomic<bool> profileOn{profileFromEnvironment()};
}

void setProfileEnabled(bool on) { detail::profileOn.store(on, std::memory_order_relaxed); }

void profileAdd(const char* label, double milliseconds) {
    if (!profileEnabled() || !label) return;
    ProfileTable& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    const auto [it, added] = t.index.emplace(label, t.rows.size());
    if (added) t.rows.push_back({label, 0.0, 0});
    ProfileEntry& row = t.rows[it->second];
    row.milliseconds += milliseconds;
    ++row.calls;
}

std::vector<ProfileEntry> profileReport() {
    ProfileTable& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    return t.rows;
}

std::string profileReportText(const std::string& indent) {
    std::ostringstream out;
    for (const ProfileEntry& row : profileReport()) {
        out << indent << row.label << ": " << row.milliseconds << " ms (" << row.calls << (row.calls == 1 ? " call)\n" : " calls)\n");
    }
    return out.str();
}

void profileReset() {
    ProfileTable& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    t.rows.clear();
    t.index.clear();
}

}  // namespace groomlab
