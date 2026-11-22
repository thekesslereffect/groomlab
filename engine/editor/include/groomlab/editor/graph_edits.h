// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's graph edits (engine/editor), every host's: what follows the graph when an
// edit renames, copies or removes nodes, and where Add Node puts a node after the selected one.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/graph_edit.h"

namespace groomlab {

// What a graph edit carries along: the texts and names that name the graph's nodes.
struct GraphFollowers {
    std::string sculpts;       // the Sculpt nodes' edits (sculpt.h: by node name, a compound's inner as "compound/node")
    std::string sculptTarget;  // the Sculpt node the brushes shape ("" none)
    std::string braidTubes;    // Show Tubes' nodes (braid_tubes.h: their names)
    std::string viewNode;      // the node viewed instead of the output ("" none)
};

// The followers after an edit that left the graph `graphAfter` (JSON text): renamed nodes (`moved`
// in order, from -> to: a rename, Group's "node" -> "compound/node", Ungroup back) take their
// Sculpt edits, the target, Show Tubes and the view along; copies ({copy: source}) get a copy of
// their source's Sculpt edits; nodes no longer in the graph lose theirs, and a target or a viewed
// node that is gone (or now inside a compound) is cleared. Sculpt edits that do not read are left
// as they are (the evaluation says why), as is Show Tubes when the graph does not read.
GraphFollowers followGraphEdit(const std::string& graphAfter, GraphFollowers followers,
                               const std::vector<std::pair<std::string, std::string>>& moved,
                               const std::map<std::string, std::string>& copied = {});

// Add Node after the selected node (every host's Graph Editor and node list): `node` (in the
// graph already) goes into `anchor`'s first outgoing link it fits in (insertOnLink: the chain goes
// through it), else is fed by `anchor` on its first input (the spec's order) that takes anchor's
// output; else it stays unlinked. Whether it was wired.
bool wireAfterNode(GraphEdit& edit, const std::string& anchor, const std::string& node);

}  // namespace groomlab
