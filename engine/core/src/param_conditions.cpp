// SPDX-License-Identifier: Apache-2.0
#include "groomlab/param_conditions.h"

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

}  // namespace groomlab
