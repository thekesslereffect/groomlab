// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom's authoring beyond the brushes (docs/BLENDER.md "Authoring"): the graph's own edits
// as Maya's Graph Editor makes them (Group and Ungroup, exposed params, a node put on a link,
// sliders and keyed params, each one undo step on the session's stack), the controls' values,
// the node library (Save as Node, script nodes, Save as Preset), and what brushes edit: the base
// groom, a corrective layer (shot sculpting) or a Sculpt node, with the corrective stack's edits.

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"

namespace groomlab::helper {

struct Session;

// edit_graph's undoable ops (group, ungroup, expose_param...): true when `op` is one of them,
// its answer in `result`. Throws RequestError or GraphError (the graph unchanged).
bool runGraphAuthoring(Session& s, const std::string& op, const nlohmann::json& args, nlohmann::json& result);

// A graph edit that left `edit` (the graph was `before`, its text) as one undo step on the session's
// stack: its text, the Sculpt nodes' edits and the brushes' target following the nodes it renamed,
// grouped, copied ({copy: source}) or removed (the editor's followGraphEdit, every host's). A graph
// that does not load yet (an input left open) is kept without an undo step; one that loads stays
// one that loads. {graphChanged, graphError, undoable, moved, undo} over `r`.
nlohmann::json commitGraph(Session& s, const std::string& label, const std::string& before, const GraphEdit& edit,
                           nlohmann::json r = nlohmann::json::object(), const std::map<std::string, std::string>& copied = {});
// The same for the graph's text after the edit and the nodes it moved (GraphEdit::movedNodes).
nlohmann::json commitGraphText(Session& s, const std::string& label, const std::string& before, const std::string& text,
                               const std::vector<std::pair<std::string, std::string>>& moved, nlohmann::json r = nlohmann::json::object(),
                               const std::map<std::string, std::string>& copied = {});

// The other authoring commands (set_controls, save_node, correctives_list, sculpt_target...).
bool isAuthoringCommand(const std::string& cmd);
// Runs one; `session` gives the request's session (the commands that take one call it).
nlohmann::json runAuthoringCommand(const std::string& cmd, const nlohmann::json& args, const std::function<Session&()>& session);

}  // namespace groomlab::helper
