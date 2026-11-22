// SPDX-License-Identifier: Apache-2.0
// Arranging a graph by hand and the editors' newer edits (graph_arrange.h, graph_portals.h, and
// GraphEdit's duplicate, output-link moves, pass-through outputs and compound scopes): what every
// host's node editor does through the core.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "groomlab/graph_arrange.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

const json* nodeIn(const json& g, const std::string& name) {
    for (const json& n : g["nodes"]) {
        if (n["name"] == name) return &n;
    }
    return nullptr;
}

std::string sourceIn(const json& g, const std::string& to, const std::string& input, std::string* output = nullptr) {
    for (const json& c : g["connections"]) {
        if (c["to"] == to && c["input"] == input) {
            if (output) *output = c.value("output", std::string());
            return c["from"].get<std::string>();
        }
    }
    return {};
}

TEST(GraphArrange, AlignsAndSpacesBoxes) {
    const std::vector<LayoutBox> boxes = {{"a", 0, 0, 100, 40}, {"b", 50, 100, 100, 60}, {"c", 300, 20, 120, 40}};
    auto left = alignBoxes(boxes, "left");
    EXPECT_EQ(left["a"].first, 0);
    EXPECT_EQ(left["b"].first, 0);
    EXPECT_EQ(left["c"].first, 0);
    EXPECT_EQ(left["b"].second, 100);  // y untouched
    auto right = alignBoxes(boxes, "right");
    EXPECT_EQ(right["a"].first, 320);  // the rightmost edge (420) less its width
    auto top = alignBoxes(boxes, "top");
    EXPECT_EQ(top["b"].second, 0);
    auto middle = alignBoxes(boxes, "centerY");
    EXPECT_DOUBLE_EQ(middle["a"].second + 20, middle["b"].second + 30);  // the centers on one line
    // A row: left to right from the first, 40 apart, the centers on the first's.
    auto row = alignBoxes(boxes, "row", 40);
    EXPECT_EQ(row["a"], std::make_pair(0.0, 0.0));
    EXPECT_EQ(row["b"], std::make_pair(140.0, -10.0));
    EXPECT_EQ(row["c"].first, 280.0);
    // A column: top to bottom.
    auto column = alignBoxes(boxes, "column", 10);
    EXPECT_EQ(column["c"].second, 50.0);  // under a (0..40) and 10 apart
    EXPECT_EQ(column["b"].second, 100.0);
    // Distributed: the outer two stay, the gaps equal.
    auto spread = alignBoxes(boxes, "distributeX");
    EXPECT_EQ(spread["a"].first, 0);
    EXPECT_EQ(spread["c"].first, 300);
    EXPECT_DOUBLE_EQ(spread["b"].first - 100, 300 - (spread["b"].first + 100));
    // Too few to align: as they were.
    EXPECT_EQ(alignBoxes({boxes[0], boxes[1]}, "distributeY")["b"].second, 100);
    EXPECT_THROW(alignBoxes(boxes, "diagonal"), GraphError);
    EXPECT_EQ(alignModes().size(), 10u);
}

TEST(GraphArrange, TidiesTheSelectionWhereItWas) {
    GraphEdit edit(defaultGraphJson());
    const std::vector<LayoutBox> selected = {{"length", 500, 300, 176, 60}, {"noise", 900, 800, 176, 60}, {"color", 700, 320, 176, 60}};
    const auto placed = arrangeSelection(edit, selected, 230, 120);
    ASSERT_EQ(placed.size(), 3u);
    // A chain: one row from the selection's top-left, a column apart.
    EXPECT_EQ(placed.at("length"), std::make_pair(500.0, 300.0));
    EXPECT_EQ(placed.at("noise"), std::make_pair(730.0, 300.0));
    EXPECT_EQ(placed.at("color"), std::make_pair(960.0, 300.0));
    EXPECT_TRUE(arrangeSelection(edit, {{"nope", 0, 0, 1, 1}}, 230, 120).empty());
}

TEST(GraphArrange, BackdropsGrowAroundTheirNodesAndFold) {
    const LayoutBox backdrop{"", 0, 0, 300, 200};
    EXPECT_TRUE(onBackdrop(backdrop, {"a", 100, 100, 50, 20}));
    EXPECT_FALSE(onBackdrop(backdrop, {"a", 290, 100, 50, 20}));  // its center is off it
    // A node moved out past the right and bottom edges: the backdrop grows, never shrinks.
    const LayoutBox grown = grownBackdrop(backdrop, {{"a", 280, 190, 100, 40}}, 20, 28);
    EXPECT_EQ(grown.x, 0);
    EXPECT_EQ(grown.right(), 400);
    EXPECT_EQ(grown.bottom(), 250);
    // Above the title bar: it grows up past the node and its title.
    const LayoutBox up = grownBackdrop(backdrop, {{"a", 50, 10, 100, 40}}, 20, 28);
    EXPECT_EQ(up.y, 10 - 20 - 28);
    EXPECT_EQ(up.bottom(), 200);
    EXPECT_EQ(grownBackdrop(backdrop, {{"a", 100, 100, 50, 20}}, 20, 28).w, 300);
    const LayoutBox folded = foldedBackdrop(backdrop, 28);
    EXPECT_EQ(folded.w, 260);
    EXPECT_EQ(folded.h, 44);
    EXPECT_TRUE(backdropFolded(json{{"collapsed", true}}));
    EXPECT_FALSE(backdropFolded(json{{"title", "x"}}));
}

TEST(GraphArrange, ShiftDragCarriesEverythingUpstream) {
    const GraphEdit edit(defaultGraphJson());
    EXPECT_EQ(nodesWithUpstream(edit, {"length"}),
              (std::vector<std::string>{"scalp", "guides", "scatter", "interpolate", "length"}));
    EXPECT_EQ(nodesWithUpstream(edit, {"scalp", "nope"}), std::vector<std::string>{"scalp"});
}

TEST(GraphArrange, AQuickLeftRightShakeIsCaught) {
    ShakeDetector shake;
    double t = 0.0;
    bool caught = false;
    int at = -1;
    // Back and forth 40 px every 50 ms.
    for (int i = 0; i < 12 && !caught; ++i) {
        caught = shake.add(t, (i % 2) ? 40.0 : 0.0, 0.0);
        if (caught) at = i;
        t += 0.05;
    }
    EXPECT_TRUE(caught);
    EXPECT_GE(at, 4);
    // A slow drag back and forth is no shake.
    ShakeDetector slow;
    bool any = false;
    for (int i = 0; i < 12; ++i) any = any || slow.add(i * 0.5, (i % 2) ? 40.0 : 0.0, 0.0);
    EXPECT_FALSE(any);
    // Nor a small jitter, nor a drag one way.
    ShakeDetector jitter;
    for (int i = 0; i < 20; ++i) any = any || jitter.add(i * 0.02, (i % 2) ? 5.0 : 0.0, 0.0);
    ShakeDetector straight;
    for (int i = 0; i < 20; ++i) any = any || straight.add(i * 0.02, i * 30.0, 0.0);
    EXPECT_FALSE(any);
}

TEST(GraphArrange, WireStylesRouteForwardAndBackward) {
    EXPECT_EQ(wireStyles(), (std::vector<std::string>{"smooth", "square", "rounded"}));
    // Smooth: one curve, level at both ends.
    auto smooth = wireRoute("smooth", 0, 0, 200, 100);
    ASSERT_EQ(smooth.size(), 1u);
    EXPECT_TRUE(smooth[0].cubic);
    EXPECT_EQ(smooth[0].c1y, 0);
    EXPECT_EQ(smooth[0].c2y, 100);
    // Square: across to the middle, down, across.
    auto square = wireRoute("square", 0, 0, 200, 100);
    ASSERT_EQ(square.size(), 3u);
    EXPECT_FALSE(square[0].cubic);
    EXPECT_EQ(square[0].x, 100);
    EXPECT_EQ(square[1].y, 100);
    EXPECT_EQ(square[2].x, 200);
    // Rounded: the same with its two corners curved.
    auto rounded = wireRoute("rounded", 0, 0, 200, 100);
    int curves = 0;
    for (const WireStep& s : rounded) curves += s.cubic ? 1 : 0;
    EXPECT_EQ(curves, 2);
    EXPECT_EQ(rounded.back().x, 200);
    EXPECT_EQ(rounded.back().y, 100);
    // Level ends: no corners at all.
    EXPECT_EQ(wireRoute("rounded", 0, 50, 200, 50).size(), 1u);
    // Backwards: under the cards.
    auto back = wireRoute("square", 300, 0, 0, 0, 80.0);
    double lowest = 0;
    for (const WireStep& s : back) lowest = std::max(lowest, s.y);
    EXPECT_EQ(lowest, 104.0);
    EXPECT_EQ(back.back().x, 0);
    // Between pins: no loop.
    EXPECT_EQ(wireRoute("square", 300, 0, 0, 40, 80.0, true).size(), 3u);
}

TEST(GraphEditByHand, DuplicateKeepsWhatFedTheNodes) {
    GraphEdit edit(defaultGraphJson());
    const auto made = edit.duplicateNodes({"noise", "color"}, 40, 60);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_EQ(made[0].first, "noise1");
    const json g = json::parse(edit.toJson());
    EXPECT_EQ(sourceIn(g, "noise1", "strands"), "length");  // the input kept
    EXPECT_EQ(sourceIn(g, "color1", "strands"), "noise1");  // the link among them
    EXPECT_EQ(sourceIn(g, "width", "strands"), "color");    // what they fed stays theirs
    EXPECT_THROW(edit.duplicateNodes({"output"}), GraphError);
}

TEST(GraphEditByHand, AnOutputsLinksMoveToAnotherOutput) {
    GraphEdit edit(defaultGraphJson());
    // scalp feeds scatter and interpolate: both from guides' output now? Guides give guides: no.
    EXPECT_EQ(edit.outputLinks("scalp"), (std::vector<std::pair<std::string, std::string>>{{"scatter", "scalp"}, {"interpolate", "scalp"}}));
    EXPECT_TRUE(edit.moveOutputLinks("scalp", "", "guides").empty());  // nothing fits: as it was
    edit.addNode("Noise", "frizz");
    edit.connect("noise", "frizz", "strands");
    const auto moved = edit.moveOutputLinks("noise", "", "length");
    EXPECT_EQ(moved, (std::vector<std::pair<std::string, std::string>>{{"color", "strands"}, {"frizz", "strands"}}));
    const json g = json::parse(edit.toJson());
    EXPECT_EQ(sourceIn(g, "frizz", "strands"), "length");
    EXPECT_TRUE(edit.outputLinks("noise").empty());
    // A move that would make a cycle leaves that link.
    EXPECT_TRUE(edit.moveOutputLinks("length", "", "width").size() < 3u);
}

TEST(GraphEditByHand, PassThroughOutputsHandTheirInputOn) {
    GraphEdit edit(defaultGraphJson());
    EXPECT_TRUE(edit.setPassThrough("noise", "strands", true));
    EXPECT_FALSE(edit.setPassThrough("noise", "strands", true));
    EXPECT_EQ(edit.passThrough("noise"), std::vector<std::string>{"strands"});
    const json spec = json::parse(edit.nodeSpecJson("noise"));
    ASSERT_EQ(spec["outputs"].size(), 2u);
    EXPECT_EQ(spec["outputs"][1]["name"], "thru_strands");
    EXPECT_EQ(GraphEdit::passThroughInput("thru_strands"), "strands");
    EXPECT_EQ(GraphEdit::passThroughInput("strands"), "");
    EXPECT_EQ(edit.outputType("noise", "thru_strands"), "Strands");
    // color reads what comes into noise: length's strands, as drawn through noise.
    edit.connect("noise", "color", "strands", "thru_strands");
    EXPECT_EQ(edit.outputLinks("noise", "thru_strands").size(), 1u);
    const std::string drawn = edit.toJson();
    const GraphDocument doc = loadGraphJson(drawn);
    NodeId src = 0;
    ASSERT_TRUE(doc.graph.inputSource(doc.node("color"), 0, &src));
    EXPECT_EQ(doc.nameOf(src), "length");
    // Saved as drawn: the node's passThrough, the link from its output.
    const json saved = json::parse(saveGraphJson(doc));
    std::string output;
    EXPECT_EQ(sourceIn(saved, "color", "strands", &output), "noise");
    EXPECT_EQ(output, "thru_strands");
    EXPECT_EQ((*nodeIn(saved, "noise"))["passThrough"], json::array({"strands"}));
    // Resolved by hand: the same; with nothing coming in, nothing goes out.
    const json resolved = json::parse(resolvePassThroughJson(drawn));
    EXPECT_EQ(sourceIn(resolved, "color", "strands"), "length");
    edit.disconnect("noise", "strands");
    const json empty = json::parse(resolvePassThroughJson(edit.toJson()));
    EXPECT_EQ(sourceIn(empty, "color", "strands"), "");
    // Detached, its pass-through readers read what fed it; turned off, they go.
    GraphEdit again(drawn);
    again.detach("noise");
    EXPECT_EQ(sourceIn(json::parse(again.toJson()), "color", "strands"), "length");
    GraphEdit off(drawn);
    EXPECT_TRUE(off.setPassThrough("noise", "strands", false));
    EXPECT_EQ(sourceIn(json::parse(off.toJson()), "color", "strands"), "");
    EXPECT_FALSE(json::parse(off.toJson())["nodes"][5].contains("passThrough"));
    EXPECT_THROW(off.setPassThrough("noise", "nope", true), GraphError);
}

TEST(GraphEditByHand, GoingIntoACompoundAndBackOut) {
    GraphEdit edit(defaultGraphJson());
    const std::string compound = edit.collapse({"noise", "color"});
    const std::string whole = edit.toJson();
    json inner = json::parse(compoundGraphJson(whole, compound));
    EXPECT_EQ(inner["output"], "color");
    ASSERT_NE(nodeIn(inner, "in_strands"), nullptr);
    EXPECT_EQ(sourceIn(inner, "noise", "strands"), "in_strands");
    // Edited inside: a node added between them, a second input, an exposed param that follows a rename.
    GraphEdit in(inner.dump());
    in.addNode("Length", "trim");
    in.connect("noise", "trim", "strands");
    in.connect("trim", "color", "strands");
    in.addNode("Input", "in_extra", R"({"name": "extra", "type": "Guides"})");
    json withExposed = json::parse(in.toJson());
    withExposed["exposed"] = {{"amount", "trim.fraction"}};
    GraphEdit renamed(withExposed.dump());
    renamed.renameNode("trim", "cut");
    const json after = json::parse(withCompoundGraph(whole, compound, renamed.toJson()));
    const json& node = *nodeIn(after, compound);
    const json& definition = node["params"]["definition"];
    EXPECT_EQ(definition["expose"]["amount"], "cut.fraction");
    ASSERT_EQ(definition["inputs"].size(), 2u);
    EXPECT_EQ(definition["inputs"][1]["name"], "extra");
    EXPECT_EQ(definition["inputs"][1]["type"], "Guides");
    EXPECT_FALSE(definition["graph"].contains("exposed"));
    // It still loads and its spec has the new input.
    GraphEdit back(after.dump());
    back.checkConnections();
    EXPECT_EQ(json::parse(back.nodeSpecJson(compound))["inputs"].size(), 2u);
    // An inner graph with no output is refused; so is a path that is no compound.
    json noOutput = inner;
    noOutput.erase("output");
    EXPECT_THROW(withCompoundGraph(whole, compound, noOutput.dump()), GraphError);
    EXPECT_THROW(compoundGraphJson(whole, "length"), GraphError);
    EXPECT_THROW(compoundGraphJson(whole, "nope"), GraphError);
    // A compound inside a compound, by its path.
    GraphEdit nested(inner.dump());
    const std::string innerCompound = nested.collapse({"noise"});
    const std::string outer = withCompoundGraph(whole, compound, nested.toJson());
    const json deep = json::parse(compoundGraphJson(outer, compound + "/" + innerCompound));
    EXPECT_NE(nodeIn(deep, "noise"), nullptr);
    // An input it no longer has: the link into it goes.
    json oneInput = json::parse(compoundGraphJson(after.dump(), compound));
    json kept = json::array();
    for (const json& n : oneInput["nodes"]) {
        if (n["name"] != "in_extra") kept.push_back(n);
    }
    oneInput["nodes"] = kept;
    GraphEdit linked(after.dump());
    linked.connect("guides", compound, "extra");
    const json dropped = json::parse(withCompoundGraph(linked.toJson(), compound, oneInput.dump()));
    EXPECT_EQ(sourceIn(dropped, compound, "extra"), "");
    EXPECT_EQ(sourceIn(dropped, compound, "strands"), "length");
}

TEST(GraphPortals, LinksTeleportThroughNamedPortals) {
    json g = json::parse(defaultGraphJson());
    // The scalp's link into interpolate drawn through a portal: made, named after the source.
    const auto exit = teleportLink(g, "interpolate", "scalp", 500, 300, 100, 0);
    ASSERT_TRUE(exit);
    EXPECT_EQ(exit->portal, "scalp");
    EXPECT_EQ(exit->exit, 0);
    EXPECT_EQ(portalOf(g, "scalp", ""), "scalp");
    const auto through = portalOfLink(g, "interpolate", "scalp");
    ASSERT_TRUE(through);
    EXPECT_EQ(through->portal, "scalp");
    // A second link of the same output: the same portal, another exit.
    const auto second = teleportLink(g, "scatter", "scalp", 200, 100, 0, 0);
    EXPECT_EQ(second->portal, "scalp");
    EXPECT_EQ(second->exit, 1);
    EXPECT_EQ(portalNames(g), std::vector<std::string>{"scalp"});
    // Nothing feeds it: no portal.
    EXPECT_FALSE(teleportLink(g, "scalp", "mesh", 0, 0, 0, 0));
    // A link of another source cannot come out of it.
    EXPECT_FALSE(addPortalLink(g, "scalp", 0, "noise", "strands"));
    // Moved, renamed (unique among the portals), an exit placed with no link yet.
    EXPECT_TRUE(movePortal(g, "scalp", 7, 8));
    EXPECT_TRUE(movePortalExit(g, "scalp", 1, 9, 10));
    EXPECT_EQ(addPortal(g, "guides", "", 0, 0, "Head"), "Head");
    EXPECT_EQ(renamePortal(g, "scalp", "Head"), "Head1");
    EXPECT_EQ(addPortalExit(g, "Head", 3, 4), 0);
    prunePortals(g);
    ASSERT_EQ(g["portals"].size(), 2u);
    EXPECT_EQ(g["portals"][1]["exits"][0]["links"].size(), 0u);  // an empty exit stays
    // Its link fed from elsewhere now: out of the portal.
    for (json& c : g["connections"]) {
        if (c["to"] == "scatter" && c["input"] == "scalp") c["from"] = "guides";
    }
    prunePortals(g);
    EXPECT_FALSE(portalOfLink(g, "scatter", "scalp"));
    // A renamed node: the portals follow.
    renamePortalNode(g, "scalp", "head");
    EXPECT_EQ(g["portals"][0]["from"], "head");
    EXPECT_TRUE(removePortalLink(g, "interpolate", "scalp"));
    EXPECT_TRUE(removePortalExit(g, "Head1", 0));
    EXPECT_TRUE(removePortal(g, "Head1"));
    EXPECT_TRUE(removePortal(g, "Head"));
    EXPECT_FALSE(g.contains("portals"));
    // Carried by loading and saving, never evaluated.
    json carried = json::parse(defaultGraphJson());
    teleportLink(carried, "interpolate", "guides", 1, 2, 3, 4);
    const json saved = json::parse(saveGraphJson(loadGraphJson(carried.dump())));
    ASSERT_TRUE(saved.contains("portals"));
    EXPECT_EQ(saved["portals"][0]["name"], "guides");
}

TEST(GraphEditByHand, AShakenGroupKeepsItsOwnLinks) {
    GraphEdit edit(defaultGraphJson());
    // length -> noise -> color: noise and color shaken out together.
    const auto done = edit.detachGroup({"noise", "color"});
    EXPECT_TRUE(done.changed);
    const json g = json::parse(edit.toJson());
    EXPECT_EQ(sourceIn(g, "color", "strands"), "noise") << "kept between them";
    EXPECT_EQ(sourceIn(g, "noise", "strands"), "") << "out of the flow";
    EXPECT_EQ(sourceIn(g, "width", "strands"), "length") << "what fed them feeds what they fed";
    EXPECT_FALSE(edit.detachGroup({"nope"}).changed);
}

TEST(GraphEditByHand, AChainDroppedOnAWireGoesIn) {
    GraphEdit edit(defaultGraphJson());
    edit.addNode("Noise", "a");
    edit.addNode("Curl", "b");
    edit.connect("a", "b", "strands");
    insertNodesOnLink(edit, "length", "noise", "strands", {"b", "a"});
    const json g = json::parse(edit.toJson());
    EXPECT_EQ(sourceIn(g, "a", "strands"), "length");
    EXPECT_EQ(sourceIn(g, "b", "strands"), "a");
    EXPECT_EQ(sourceIn(g, "noise", "strands"), "b");
    EXPECT_THROW(insertNodesOnLink(edit, "a", "b", "strands", {"a", "b"}), GraphError);
}

TEST(GraphArrange, BackdropsKeepSlowNodesAndLetFastOnesGo) {
    EXPECT_TRUE(backdropKeeps(true, 5000.0, false));
    EXPECT_TRUE(backdropKeeps(false, 200.0, false));
    EXPECT_FALSE(backdropKeeps(false, 3000.0, false));
    EXPECT_FALSE(backdropKeeps(true, 0.0, true)) << "once out, out for that drag";
}

TEST(GraphPortals, DotsAreFreeUntilFedOrJoined) {
    json g = json::parse(defaultGraphJson());
    const std::string dot = addDot(g, 10, 20);
    EXPECT_EQ(dot, "Dot");
    EXPECT_FALSE(portalFed(g, dot));
    prunePortals(g);
    EXPECT_EQ(portalNames(g), std::vector<std::string>{"Dot"}) << "a free dot stays";
    // Fed: its source set; a link drawn straight out of it (organizing the graph).
    EXPECT_TRUE(setPortalSource(g, dot, "length", ""));
    EXPECT_TRUE(portalFed(g, dot));
    EXPECT_TRUE(addPortalLink(g, dot, -1, "noise", "strands"));
    const auto own = portalOfLink(g, "noise", "strands");
    ASSERT_TRUE(own);
    EXPECT_EQ(own->exit, -1);
    // Another free dot whose Input Portal is the first: an exit of it, where it stood.
    const std::string other = addDot(g, 300, 40);
    EXPECT_EQ(other, "Dot1");
    EXPECT_EQ(joinPortal(g, other, dot), 0);
    EXPECT_EQ(portalNames(g), std::vector<std::string>{"Dot"});
    EXPECT_EQ(g["portals"][0]["exits"][0]["x"], 300);
    EXPECT_EQ(joinPortal(g, dot, dot), -1);
    // A free dot cannot be joined to (no source); an output with a portal takes no second one.
    const std::string free = addDot(g, 0, 0, "Free");
    EXPECT_EQ(joinPortal(g, dot, free), -1);
    EXPECT_FALSE(setPortalSource(g, free, "length", ""));
    // Its links (entry's and exits'); an exit leaving: a free dot again.
    EXPECT_EQ(portalLinks(g, dot), (std::vector<std::pair<std::string, std::string>>{{"noise", "strands"}}));
    EXPECT_EQ(leavePortal(g, dot, 0), "Dot1");
    EXPECT_FALSE(portalFed(g, "Dot1"));
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(GraphEditByHand, ADuplicateReadsThroughTheSameDotAndPins) {
    json g = json::parse(defaultGraphJson());
    // noise reads length out of a dot's exit; color reads noise through a pin.
    const auto exit = teleportLink(g, "noise", "strands", 300, 0, 100, 0);
    ASSERT_TRUE(exit);
    addPin(g, "color", "strands", 10, 10);
    GraphEdit edit(g.dump());
    const auto made = edit.duplicateNodes({"noise"}, 0, 60);
    edit.duplicateNodes({"color"}, 0, 60);
    const json after = json::parse(edit.toJson());
    const auto dot = portalOfLink(after, made[0].first, "strands");
    ASSERT_TRUE(dot);
    EXPECT_EQ(dot->portal, exit->portal);
    EXPECT_EQ(dot->exit, exit->exit);
    EXPECT_EQ(pinRoute(after, "color1", "strands").size(), 1u);
}

}  // namespace
}  // namespace groomlab
