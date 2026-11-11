// SPDX-License-Identifier: Apache-2.0
#pragma once

// The graph edits every host's menus make to add a whole feature (Add > Simulate, Load Guide
// Cache, Add > Collider), on GraphEdit: Maya's sim.py and collision.py, the Blender add-on's
// pipeline.py and the app's GroomDocument make them through these, so the node lands in the same
// place, with the same links, in every host.

#include <optional>
#include <string>

#include "groomlab/graph_edit.h"

namespace groomlab {

// Where an inserted node goes: a column right of (or a row below) the node it sits by, as the
// core lays out new nodes (graph_edit.cpp's column).
constexpr double kInsertColumn = 240.0;
constexpr double kInsertRow = 140.0;

// Sets a node's position ("ui"). GraphError for no such node.
void setNodePosition(GraphEdit& edit, const std::string& node, double x, double y);
// A node's position, nullopt when it has none.
std::optional<std::pair<double, double>> nodePosition(const GraphEdit& edit, const std::string& node);

// `node` between src and dst: every link from src into dst goes through it (src's output those
// links read into node's `nodeInput`, node's first output into dst's inputs).
void insertBetween(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& node,
                   const std::string& nodeInput = "strands");
// `node` between what feeds anchor's `input` and anchor (before the output, say), placed a row
// below anchor. GraphError when nothing feeds that input.
void insertBefore(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& input = "strands",
                  const std::string& nodeInput = "strands");

// Add > Simulate: a Simulate node (its params from `paramsJson`, an object; "" none) between the
// first Guides node feeding an Interpolate and every Interpolate it feeds, so the strands follow
// the simulated guides; bypassed or deleted, the groom is as it was. Placed half a column right of
// and a row below the Guides node. GraphError when no Guides node feeds an Interpolate. Returns the
// node's name.
std::string insertSimulate(GraphEdit& edit, const std::string& paramsJson = {}, std::optional<double> groomSize = std::nullopt);

// Load Guide Cache: a GuideCache node reading `path` (`object`: the Alembic object, "" every one)
// right after the first Guides node feeding the groom, feeding all it fed, so the guides take the
// cache's poses. A GuideCache the Guides node feeds (or one fed by nothing) reads the new file
// instead. GraphError when no Guides node feeds anything. Returns the node's name.
std::string insertGuideCache(GraphEdit& edit, const std::string& path, const std::string& object = {},
                             std::optional<double> groomSize = std::nullopt);

// Add > Collider, its graph: a ColliderMesh node reading the graph's mesh input `meshName` (the
// one reading it already, else a new one named after it, letters, digits and _). With `wire`, a
// Collide node (collide_<name>; `offset` its offset) goes just before the output, the ColliderMesh
// on its collider input and the groom's own Scalp on its scalp input (off the scalp too), the
// ColliderMesh placed left of it. A ColliderMesh there already with no `wire` (and no `friction`)
// leaves the graph as it is. `friction` (-1 to 1) sets the ColliderMesh's. GraphError for the
// name "scalp" or none, or a graph with no output to wire before. Returns the ColliderMesh node.
std::string insertCollider(GraphEdit& edit, const std::string& meshName, bool wire = true,
                           std::optional<double> offset = std::nullopt, std::optional<double> friction = std::nullopt,
                           std::optional<double> groomSize = std::nullopt);

}  // namespace groomlab
