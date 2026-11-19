// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace groomlab {

// The add-node menu every host shows (Maya's Graph Editor, the app's node graph, Blender's node
// editor): its categories in the order a groom is built, each node type's category, its display
// label, a line on what it does and the words a search finds it by. operatorSpecsJson writes
// these into each spec ("category", "categoryTitle", "categoryOrder", "order", "label",
// "keywords", "hidden", "description"), so no host keeps its own list.
//
// Each type gives its own (NodeInfo) where it registers (OperatorRegistry::registerType, in the
// operator's source file); the menu's order within a category is builtin_operators.inc's.
//
// The category here is the menu's. An operator's own category() is its role in a chain (where a
// preset's look starts: a source or grow node), written as the spec's "role".
struct NodeCategory {
    std::string id;     // "shape"
    std::string title;  // "Shape"
};

// The categories in menu order.
const std::vector<NodeCategory>& nodeCategories();
// A category id's place in nodeCategories() (its size for an id it does not list).
int nodeCategoryOrder(const std::string& id);

// What the add-node menu and the param panel show of a node type, given with its registration:
//   registry.registerType(op::kCurl, {"look", "Helical curls around each strand.", "coil wave helix"},
//                         [] { return std::make_unique<CurlOp>(); });
// A built-in type must give a category and a description (Graph.NodeCatalogIsEveryHostsAddNodeMenu).
struct NodeInfo {
    std::string category;     // a NodeCategory id ("look")
    std::string description;  // what it does, in a line: the top of its param panel, its search tooltip
    std::string keywords;     // words a search finds it by that its name does not say ("symmetry")
    std::string label;        // what every host shows ("Linked Groom"); empty: the type's words spaced
    bool hidden = false;      // made by grouping or by a compound, not from the add-node menu
};

// The registered type's info, or null for a type the registry does not know.
const NodeInfo* nodeInfo(const std::string& type);

// A type's place in the add-node menu: its line in builtin_operators.inc (a type registered
// elsewhere, a test's: after them all). Hosts sort by category order, then this.
int nodeMenuOrder(const std::string& type);

// The built-in types in builtin_operators.inc's order (the ones registered outside the core too).
const std::vector<std::string>& builtinNodeTypes();

// A type's display label: its info's, else its CamelCase spaced ("BlendNeighbors" -> "Blend
// Neighbors").
std::string nodeLabel(const std::string& type);
// A param's display label, every host's (Maya's and Blender's pretty): its camelCase as words, the
// first capitalized, the rest lower case but for words all in capitals ("guideRadius" -> "Guide
// radius", "rootUV" -> "Root UV"); underscores as spaces.
std::string paramLabel(const std::string& name);

}  // namespace groomlab
