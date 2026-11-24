// SPDX-License-Identifier: Apache-2.0
#pragma once

// The graph edits every host's menus make to add a whole feature (Add > Simulate, Load Guide
// Cache, Add > Collider, Add > Braid, Add > Feathers, Add Scalp), on GraphEdit: Maya's sim.py,
// collision.py, braids.py, feathers.py and extra_scalps.py, the Blender add-on's pipeline.py and
// the app's GroomDocument make them through these, so the nodes land in the same place, with the
// same links, in every host. And a node put into the flow (between two nodes, before or after
// one, on a link: a node dropped on a wire), as every host's editor and tools do it: Maya's
// GraphModel.insert_*, the Blender helper's edit_graph ops and the app's GroomDocument.
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

// Auto layout, every host's (a graph with no stored positions as it opens, Tidy Layout): columns
// `column` apart by how far a node is from the sources (the longest path in), rows `row` apart in
// the graph's order. columnsPerBlock above 0 wraps a long chain into blocks of that many columns,
// one under the other (each as tall as its tallest column, `blockGap` between them): the app's
// graph over the viewport; 0: one row of columns.
struct GraphLayout {
    double column = kInsertColumn;
    double row = kInsertRow;
    int columnsPerBlock = 0;
    double blockGap = 60.0;
};
// Each node's place in that layout, by name; nothing is set (setNodePosition). `only`: just these
// nodes are placed, the rows counted among them (the app's nodes with no position yet, beside the
// ones that have one); the columns are still the whole graph's.
std::map<std::string, std::pair<double, double>> layoutPositions(const GraphEdit& edit, const GraphLayout& layout = {},
                                                                 const std::vector<std::string>* only = nullptr);

// `node` between src and dst: every link from src into dst goes through it (src's output those
// links read into node's `nodeInput`, "": its first input; node's first output into dst's
// inputs). GraphError, the graph unchanged, when src does not feed dst or a link does not fit.
void insertBetween(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& node,
                   const std::string& nodeInput = "strands");
// `node` between what feeds anchor's `input` and anchor (before the output, say), placed a row
// below anchor. GraphError when nothing feeds that input.
void insertBefore(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& input = "strands",
                  const std::string& nodeInput = "strands");
// `node` right after anchor in its chain: anchor -> node -> the first node anchor fed
// (insertBetween); anchor feeding nothing: anchor into node's `nodeInput` ("": its first input).
// Placed half a column right of and a row below anchor.
void insertAfter(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& nodeInput = {});
// A node put into a link (a node dropped on a wire, Tab over one): the one link src (its output
// `output`, "": its first) -> dst.input goes through `node`. node reads src on its first input
// (its spec's order) that takes it, dst reads node's first output: each input is tried as connect
// checks it (the types, no cycle). GraphError, the graph unchanged, when that link is not there,
// node is one of its ends, or no input of node takes src's output with node feeding dst.
void insertOnLink(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& input, const std::string& node,
                  const std::string& output = {});
// Several nodes (a chain dropped on a wire as one) put into a link: src feeds the group's entry
// (the first of them, in the graph's order, none of the others feeds; on its first input that takes
// src's output) and dst reads its exit (the one feeding none of the others; the last such, in the
// graph's order). The links among them stay. One node: insertOnLink. GraphError, the graph
// unchanged, as insertOnLink's.
void insertNodesOnLink(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& input,
                       const std::vector<std::string>& nodes, const std::string& output = {});

// Add > Simulate: a Simulate node (its params from `paramsJson`, an object; "" none) between the
// first Guides node feeding an Interpolate and every Interpolate it feeds, so the strands follow
// the simulated guides; bypassed or deleted, the groom is as it was. Placed half a column right of
// and a row below the Guides node. GraphError when no Guides node feeds an Interpolate. Returns the
// node's name.
// (After a DriveGuides or GuideCache node posing the guides, when the Guides node feeds one.)
// A Part node so drawn parts open the hair (every host's part tools: Maya's add_part_node, the
// Blender add-on's and the app's Part brush): the graph's first Part node, or with none one with
// `paramsJson` over its defaults after `after` ("": the first Interpolate, else Flow, else Bend,
// else Scatter). Returns its name; `made` says whether it is new. GraphError for a graph with no
// strands to part.
std::string insertPart(GraphEdit& edit, const std::string& paramsJson = {}, const std::string& after = {},
                       std::optional<double> groomSize = std::nullopt, bool* made = nullptr);

// A Flow node so drawn flow arrows lay the strands (every host's Flow tool, issue #112, as
// insertPart for parts): the graph's first Flow node, or with none one with `paramsJson` over its
// defaults after `after` ("": the first Interpolate, else Scatter). Returns its name; `made` says
// whether it is new. GraphError for a graph with no strands to lay.
std::string insertFlow(GraphEdit& edit, const std::string& paramsJson = {}, const std::string& after = {},
                       std::optional<double> groomSize = std::nullopt, bool* made = nullptr);

// Add > Wind / Jiggle (every host's): a node of `type` (Wind, Jiggle) with `paramsJson` over its
// spec's defaults, just before the output but before the Collide and Cards nodes ending its chain
// (motionAnchor), so the strands collide where they moved to and the cards (every export and render)
// take the motion. Returns its name. GraphError without an output.
std::string insertMotion(GraphEdit& edit, const std::string& type, const std::string& paramsJson = {},
                         std::optional<double> groomSize = std::nullopt);
// The node a motion node goes before: the output, or the first of the Collide and Cards nodes
// ending its strands chain. "" without an output.
std::string motionAnchor(const GraphEdit& edit);
// Add > Cards (every host's: Maya's create_cards, the Blender add-on's add_cards, the app's Add >
// Cards > Hero/Mid/Low): the graph's first Cards node (its own, not a compound's), or with none a
// Cards node named "cards" with `paramsJson` over its defaults just before the output (the cards
// made from the whole groom). Returns its name; `made` says whether it is new. GraphError without
// an output.
std::string insertCards(GraphEdit& edit, const std::string& paramsJson = {}, std::optional<double> groomSize = std::nullopt,
                        bool* made = nullptr);
std::string insertSimulate(GraphEdit& edit, const std::string& paramsJson = {}, std::optional<double> groomSize = std::nullopt);

// Load Guide Cache: a GuideCache node reading `path` (`object`: the Alembic object, "" every one)
// right after the first Guides node feeding the groom, feeding all it fed, so the guides take the
// cache's poses. A GuideCache the Guides node feeds (or one fed by nothing) reads the new file
// instead. GraphError when no Guides node feeds anything. Returns the node's name.
std::string insertGuideCache(GraphEdit& edit, const std::string& path, const std::string& object = {},
                             std::optional<double> groomSize = std::nullopt);

// Add Drivers, its graph: a DriveGuides node right after the first Guides node feeding the groom,
// feeding all it fed (before a Simulate there, so the simulation plays on top of the drivers). A
// DriveGuides node already there is kept (its name returned). GraphError when no Guides node feeds
// anything. Returns the node's name.
std::string insertDriveGuides(GraphEdit& edit, std::optional<double> groomSize = std::nullopt);

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

// The nodes insertScalp made ("" none).
struct ScalpNodes {
    std::string scalp, scatter, interpolate, merge;
};

// Add Scalp, its graph: hair on another mesh of the groom, the graph's mesh input `meshName`. A
// Scalp node reading it (scalp_<name>; its subdivision the groom's own Scalp's), a Scatter on it
// (scatter_<name>) with the params of the groom's own Scatter (the first one its own Scalp, the
// one reading "scalp", feeds) but a seed of its own, and an Interpolate (interpolate_<name>) with
// the params of the own Interpolate (the first that Scatter feeds), reading the guides that one
// reads (never the first Guides node there is: a braid's) and the new Scalp; no own Interpolate:
// none. A Merge (merge_<name>) joins them to the hair: right after the own Interpolate (the own
// Scatter with none; insertAfter), else before the output, the new strands on its "b". The three
// go in a row below and left of the Merge. A Scalp node reading `meshName` already: the graph
// unchanged, that node returned alone. GraphError for the name "scalp" or none.
ScalpNodes insertScalp(GraphEdit& edit, const std::string& meshName, std::optional<double> groomSize = std::nullopt);

// Remove > Scalp's graph, every host's (insertScalp undone): the Scalp nodes reading `meshName`
// and what they alone feed (its Scatter and Interpolate: a node every input of which comes from
// those, Guides nodes aside), their links gone; a Merge left with its `a` alone passes it through
// (removed, `a` feeding what it fed, the output moved to it). Whether the graph changed.
bool removeScalpNodes(GraphEdit& edit, const std::string& meshName);
// Remove > Collider's graph, every host's (insertCollider undone): the ColliderMesh nodes reading
// `meshName`; the Collide nodes they feed taken out with the flow kept (their strands pass
// through), another node they feed reading nothing. Whether the graph changed.
bool removeColliderNodes(GraphEdit& edit, const std::string& meshName);

// Braid paths and feather shafts are not hair: every Guides node reading all guides (no `group`
// of its own) gets `group` on its exclude list. Returns whether the graph changed.
bool excludeFromHairGuides(GraphEdit& edit, const std::string& group);

// Import Alembic Curves as Guides, every host's (Maya's import_guides_from_alembic, the Blender
// helper's import_alembic_curves, the app's): the graph's "guides" node replaced by an ImportCurves
// node of that name reading `path` (every curve, or those under `object`, resampled to `samples`
// points: 0 as read), drawn where the Guides node was, fed the scalp when the graph has a "scalp"
// node; what fed the old node goes. The graph's JSON back. std::invalid_argument for samples past
// 0..1024.
std::string withImportedCurves(const std::string& graphJson, const std::string& path, const std::string& object = {}, int samples = 24);

// ---- Guide groups the graph names --------------------------------------------------------------
// What reads guide group `group` in the graph (its JSON), every host's "Read by": the (node, param)
// pairs of the Guides nodes whose group or exclude list names it (words split by spaces or commas,
// as the Guides node reads them). The graph's own nodes: those inside compounds are not listed
// (renameGroupReaders does follow a rename into them).
std::vector<std::pair<std::string, std::string>> groupReaders(const std::string& graphJson, const std::string& group);
// The graph with every Guides node's group and exclude naming group `from` naming `to` instead,
// inside compounds' definitions too (a list keeps its other names, joined by spaces). Every host's
// group rename. `changed` says whether any did (the same text when none did).
std::string renameGroupReaders(const std::string& graphJson, const std::string& from, const std::string& to, bool* changed = nullptr);

// Add > Braid: a Braid node (its params from `paramsJson`, an object; "" none; `feed` left out:
// on for a cornrow) right after `after` (""/default: the node "interpolate" when it is an
// Interpolate, else the first Interpolate feeding something), before what it fed first, half a
// column right of and a row below it. Its "braids" input reads the first Guides node of `group`
// (a new one, braidPaths, two rows below `after`, when there is none), and the hair's Guides
// nodes leave `group` out. The host makes the guide group itself. GraphError for a style the
// Braid does not have, an `after` that is no node, or no Interpolate. Returns the Braid's name.
std::string insertBraid(GraphEdit& edit, const std::string& group = "braids", const std::string& paramsJson = {},
                        const std::string& after = {}, std::optional<double> groomSize = std::nullopt);

// Add > Bun: the paths of buns braided from the hair. A Braid (insertBraid: after the
// Interpolate, `style` one of its styles, rope for a twisted bun) whose braids input is a Bun node
// reading the first Guides node of `group` (each guide a bun: its root where the bun sits, its way
// out the bun's axis). The Braid gathers the hair from all over the scalp (each strand into the
// nearest bun: several are bantu knots), its end bound, as wide as suits the Bun's default size;
// `bunParamsJson` (an object; "" none) over the Bun's defaults. Lengths fitted to `groomSize`
// from the defaults' scalp. Returns both nodes' names. GraphError as insertBraid.
struct BunNodes {
    std::string bun, braid;
};
BunNodes insertBun(GraphEdit& edit, const std::string& group = "buns", const std::string& style = "three",
                   const std::string& bunParamsJson = {}, const std::string& after = {},
                   std::optional<double> groomSize = std::nullopt);

// Sculpt Sections (every host's Braid node button): a braid's sections made guides to brush, its hair
// following them. A Sculpt node reading the Braid's "sections" output (brushed as any Sculpt: its
// strokes are correctives, so they stay when the braid above changes) and a FollowGuides node right
// after the Braid (its strands, before what the Braid's hair fed) carrying each section's hair along
// its section's change: rest the sections, guides the Sculpt's. Placed below the Braid. A Braid
// that has them already keeps them. GraphError for no Braid of that name.
struct BraidSculptNodes {
    std::string sculpt, follow;
    bool made = false;  // whether they are new
};
BraidSculptNodes insertBraidSculpt(GraphEdit& edit, const std::string& braid, std::optional<double> groomSize = std::nullopt);

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
