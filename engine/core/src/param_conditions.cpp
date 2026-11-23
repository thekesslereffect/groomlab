// SPDX-License-Identifier: Apache-2.0
#include "groomlab/param_conditions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace groomlab {

std::string conditionText(const nlohmann::json& value) {
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e15) {
            return std::to_string(static_cast<std::int64_t>(d));
        }
    }
    return value.dump();
}

bool conditionHolds(const nlohmann::json& condition, const nlohmann::json& value) {
    if (!condition.is_object()) return true;
    const auto param = condition.find("param");
    if (param == condition.end() || !param->is_string() || param->get<std::string>().empty()) return true;
    const std::string text = conditionText(value);
    const auto in = condition.find("in");
    if (in == condition.end() || !in->is_array()) return false;
    for (const nlohmann::json& v : *in) {
        if (v.is_string() && v.get<std::string>() == text) return true;
    }
    return false;
}

bool paramInVersion(const nlohmann::json& spec, int nodeVersion) {
    if (!spec.is_object()) return true;
    const auto added = spec.find("addedIn");
    return added == spec.end() || !added->is_number() || added->get<int>() <= nodeVersion;
}

std::vector<std::string> paramSections(const nlohmann::json& params) {
    std::vector<std::string> groups;
    auto advanced = [](const nlohmann::json& p) {
        return p.value("advanced", false) || p.value("group", std::string()) == kAdvancedSection;
    };
    if (params.is_array()) {
        for (const nlohmann::json& p : params) {
            if (!p.is_object() || advanced(p)) continue;
            const std::string group = p.value("group", std::string());
            if (!group.empty() && std::find(groups.begin(), groups.end(), group) == groups.end()) groups.push_back(group);
        }
    }
    // A group marked "groupShown" (a compound's params shown from a node inside) is headed alone too.
    bool shown = false;
    if (params.is_array()) {
        for (const nlohmann::json& p : params) shown = shown || (p.is_object() && p.value("groupShown", false) && !p.value("group", std::string()).empty());
    }
    const bool headed = groups.size() >= 2 || shown;
    std::vector<std::string> out;
    if (!params.is_array()) return out;
    for (const nlohmann::json& p : params) {
        if (!p.is_object()) {
            out.emplace_back();
            continue;
        }
        out.push_back(advanced(p) ? std::string(kAdvancedSection) : headed ? p.value("group", std::string()) : std::string());
    }
    return out;
}

bool paramShown(const nlohmann::json& params, const std::string& name, const std::function<nlohmann::json(const std::string&)>& valueOf) {
    auto specOf = [&](const std::string& n) -> const nlohmann::json* {
        if (!params.is_array()) return nullptr;
        for (const nlohmann::json& p : params) {
            if (p.is_object() && p.value("name", std::string()) == n) return &p;
        }
        return nullptr;
    };
    std::vector<std::string> seen;
    const nlohmann::json* spec = specOf(name);
    while (spec) {
        const nlohmann::json condition = spec->value("showWhen", nlohmann::json());
        const std::string on = condition.is_object() ? condition.value("param", std::string()) : std::string();
        if (on.empty()) return true;
        if (!conditionHolds(condition, valueOf(on))) return false;
        seen.push_back(spec->value("name", std::string()));
        spec = specOf(on);
        if (spec && std::find(seen.begin(), seen.end(), spec->value("name", std::string())) != seen.end()) return true;
    }
    return true;
}

}  // namespace groomlab
