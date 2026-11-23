// SPDX-License-Identifier: Apache-2.0
#pragma once

// The core's graph helpers on parsed JSON, for its own sources (the public headers take text).

#include <nlohmann/json.hpp>

namespace groomlab {

// resolvePassThroughJson (graph_edit.h) on the graph's JSON in place.
void resolvePassThrough(nlohmann::json& graph);

}  // namespace groomlab
