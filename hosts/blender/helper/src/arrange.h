// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Graph Editor arranged by hand, as Maya's and the app's (graph_arrange.h, graph_portals.h and
// GraphEdit's pass-through outputs): Align and Tidy, the nodes a Shift or Ctrl+drag carries, the
// A+drag align gestures, portals and dots, a shake's detach of several nodes, Duplicate reading
// what its nodes read, a chain dropped on a wire, a bundle of links moved to another output, and
// pass-through outputs. The edits are edit_graph ops, one undo step each; the rest are commands
// that change nothing.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace groomlab::helper {

struct Session;

// edit_graph's arranging ops (tidy, align, teleport_link, add_dot, duplicate_nodes,
// set_pass_through...): true when `op` is one of them, its answer in `result`. Throws RequestError
// or GraphError (the graph unchanged).
bool runGraphArrange(Session& s, const std::string& op, const nlohmann::json& args, nlohmann::json& result);

// The commands that only answer (graph_carry, align_gesture, wire_route, shake_test...).
bool isArrangeCommand(const std::string& cmd);
nlohmann::json runArrangeCommand(const std::string& cmd, const nlohmann::json& args, const std::function<Session&()>& session);

}  // namespace groomlab::helper
