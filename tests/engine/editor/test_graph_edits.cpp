// SPDX-License-Identifier: Apache-2.0
// The groom editor's graph edits (engine/editor graph_edits.h), every host's: what follows a
// graph edit (Sculpt edits, the brushes' target, Show Tubes, the viewed node) and Add Node after
// the selected node.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/editor/graph_edits.h"
#include "groomlab/graph_io.h"
#include "groomlab/sculpt.h"

using namespace groomlab;
using json = nlohmann::json;

namespace {

// Two Sculpt nodes after Length in the default graph.
std::string sculptedGraph() {
    GraphEdit edit(defaultGraphJson());
    const std::string a = edit.addNode("Sculpt", "sculpt1");
    const std::string b = edit.addNode("Sculpt", "sculpt2");
    (void)a;
    (void)b;
    return edit.toJson();
}

const char* kSculpts = R"({"sculpt1": {"guides": {"7": [0.5, 0.1, 0.0, 1.0]}}, "sculpt2": {"guides": {"9": [1.0, 0.0, 0.2, 1.0]}}})";

std::vector<std::string> feeds(const std::string& graph, const std::string& to) {
    std::vector<std::string> out;
    const json g = json::parse(graph);
    for (const json& c : g["connections"]) {
        if (c["to"] == to) out.push_back(c["from"].get<std::string>() + "->" + c["input"].get<std::string>());
    }
    return out;
}

}  // namespace

TEST(GraphEdits, RenamesCopiesAndRemovalsCarryWhatNamesTheNodes) {
    GraphEdit edit(sculptedGraph());
    edit.renameNode("sculpt1", "shaping");
    GraphFollowers was{SculptSet::fromJson(kSculpts).toJson(), "sculpt1", "", "sculpt1"};
    GraphFollowers now = followGraphEdit(edit.toJson(), was, edit.movedNodes());
    EXPECT_EQ(sculptedNodes(now.sculpts), (std::vector<std::string>{"sculpt2", "shaping"}));
    EXPECT_EQ(now.sculptTarget, "shaping");
    EXPECT_EQ(now.viewNode, "shaping");
    // A copy gets its source's edits; a removed node loses its own, and the target and view let it go.
    edit.addNode("Sculpt", "shaping1");
    edit.removeNode("sculpt2");
    now = followGraphEdit(edit.toJson(), now, {}, {{"shaping1", "shaping"}});
    EXPECT_EQ(sculptedNodes(now.sculpts), (std::vector<std::string>{"shaping", "shaping1"}));
    edit.removeNode("shaping");
    now = followGraphEdit(edit.toJson(), now, {});
    EXPECT_EQ(sculptedNodes(now.sculpts), (std::vector<std::string>{"shaping1"}));
    EXPECT_EQ(now.sculptTarget, "");
    EXPECT_EQ(now.viewNode, "");
    // Unreadable edits stay as they are; so does everything when the graph does not read.
    GraphFollowers odd{"not json", "x", "", "x"};
    const GraphFollowers kept = followGraphEdit("also not json", odd, {});
    EXPECT_EQ(kept.sculpts, "not json");
    EXPECT_EQ(kept.sculptTarget, "x");
}

TEST(GraphEdits, ShowTubesFollowsARenameAndLetsARemovedBraidGo) {
    GraphEdit edit(defaultGraphJson());
    const std::string braid = edit.addNode("Braid", "braid");
    const std::string other = edit.addNode("Braid", "braid1");
    GraphFollowers was;
    was.braidTubes = braid + "\n" + other;
    edit.renameNode(braid, "plait");
    GraphFollowers now = followGraphEdit(edit.toJson(), was, edit.movedNodes());
    EXPECT_EQ(now.braidTubes, "plait\n" + other);
    edit.removeNode("plait");
    now = followGraphEdit(edit.toJson(), now, {});
    EXPECT_EQ(now.braidTubes, other);
}

TEST(GraphEdits, AddNodeAfterTheSelectedNodeGoesIntoItsLinkElseOnItsFirstInput) {
    // Into length's link to the noise after it: the chain goes through the new node.
    GraphEdit edit(defaultGraphJson());
    const std::string noise = edit.addNode("Noise", "noise2");
    ASSERT_TRUE(wireAfterNode(edit, "length", noise));
    EXPECT_EQ(feeds(edit.toJson(), noise), (std::vector<std::string>{"length->strands"}));
    EXPECT_EQ(feeds(edit.toJson(), "noise"), (std::vector<std::string>{noise + "->strands"}));
    // A node with nothing after it: fed by it on the new node's first input that takes it.
    GraphEdit end(defaultGraphJson());
    end.removeNode("output");
    const std::string tail = end.addNode("Noise", "tail");
    ASSERT_TRUE(wireAfterNode(end, "resample", tail));
    EXPECT_EQ(feeds(end.toJson(), tail), (std::vector<std::string>{"resample->strands"}));
    // Nothing that takes it: left unlinked, the graph as it was.
    GraphEdit none(defaultGraphJson());
    const std::string scalp = none.addNode("Scalp", "scalp2");
    const std::string before = none.toJson();
    EXPECT_FALSE(wireAfterNode(none, "length", scalp));
    EXPECT_EQ(none.toJson(), before);
    EXPECT_FALSE(wireAfterNode(none, "nothere", scalp));
}
