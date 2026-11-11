// SPDX-License-Identifier: Apache-2.0
#pragma once

// The graph edits every host's menus make to add a whole feature (Add > Simulate, Load Guide
// Cache, Add > Collider, Add > Braid, Add > Feathers), on GraphEdit: Maya's sim.py, collision.py,
// braids.py and feathers.py, the Blender add-on's pipeline.py and the app's GroomDocument make
// them through these, so the nodes land in the same place, with the same links, in every host.
// The hosts make the guide groups (they hold the guides) and read the feather presets.

#include <map>
#include <optional>
#include <string>
#include <vector>

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

// Braid paths and feather shafts are not hair: every Guides node reading all guides (no `group`
// of its own) gets `group` on its exclude list. Returns whether the graph changed.
bool excludeFromHairGuides(GraphEdit& edit, const std::string& group);

// Add > Braid: a Braid node (its params from `paramsJson`, an object; "" none; `feed` left out:
// on for a cornrow) right after `after` (""/default: the node "interpolate" when it is an
// Interpolate, else the first Interpolate feeding something), before what it fed first, half a
// column right of and a row below it. Its "braids" input reads the first Guides node of `group`
// (a new one, braidPaths, two rows below `after`, when there is none), and the hair's Guides
// nodes leave `group` out. The host makes the guide group itself. GraphError for a style the
// Braid does not have, an `after` that is no node, or no Interpolate. Returns the Braid's name.
std::string insertBraid(GraphEdit& edit, const std::string& group = "braids", const std::string& paramsJson = {},
                        const std::string& after = {}, std::optional<double> groomSize = std::nullopt);

// A feather design: its name (contour, down, ...) and the graph of its preset feathers_<name>,
// fitted to the scalp (presetGraphJson): its Compound node grows the barbs, its Scatter's
// density and length are the shafts'.
struct FeatherDesign {
    std::string name;
    std::string presetJson;
};

// The nodes insertFeathers made ("" none): the shafts' node, the Split sharing them, one
// Compound per design, the Merges joining the designs, and the Merge with the hair.
struct FeatherNodes {
    std::string shafts, split, merge;
    std::vector<std::string> designs, merges;
};

// Add > Feathers: barbs grown on shafts by one to four designs (several share the shafts through
// a Split, `weights` their shares, default equal). `source` "scatter": a new Scatter (`density`
// shafts per unit area, default the first design's) shaped by the hair's Guides through an
// Interpolate; "guides": the first Guides node of guide group `group` (a new one when there is
// none; the host makes the group), left out of the hair's Guides; else a node of the graph
// ("node" or "node.output"). Each design is a groom group of its own (groupId and groupName on
// its Propagates; the same design again joins its group). The feathers are merged with the hair
// before the output, or with `keepHair` false (or no hair) feed it alone. Placed in a row under
// the output. GraphError for no design or more than four, a weight count that differs, a design
// with no Compound, a source that is no node, or no output. Returns the nodes made.
FeatherNodes insertFeathers(GraphEdit& edit, const std::vector<FeatherDesign>& designs, const std::vector<double>& weights = {},
                            const std::string& source = "scatter", const std::string& group = "feathers", bool keepHair = true,
                            std::optional<double> density = std::nullopt, std::optional<double> groomSize = std::nullopt);

// {groupName: groupId} of the feather designs in the graph (their Propagates', inside compounds
// too; groupId 0 is the hair's and left out): the groom groups an Unreal export writes.
std::map<std::string, int> featherGroups(const GraphEdit& edit);

// The guide groups whose guides grow one feather each: the groups of the Guides nodes (their
// `group`) feeding a feather design (a Propagate, or a Compound with one), in the graph's order.
std::vector<std::string> featherGuideGroups(const GraphEdit& edit);

}  // namespace groomlab
