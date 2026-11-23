// SPDX-License-Identifier: Apache-2.0
// Arranging a graph by hand and the editors' newer edits (graph_arrange.h, graph_portals.h, and
// GraphEdit's duplicate, output-link moves, pass-through outputs and compound scopes): what every
// host's node editor does through the core.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>

#include <nlohmann/json.hpp>

#include "groomlab/graph_arrange.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"
#include "groomlab/param_conditions.h"

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

TEST(GraphArrange, CtrlDragCarriesEverythingDownstream) {
    const GraphEdit edit(defaultGraphJson());
    const auto down = nodesWithDownstream(edit, {"noise"});
    EXPECT_NE(std::find(down.begin(), down.end(), "noise"), down.end());
    EXPECT_NE(std::find(down.begin(), down.end(), "color"), down.end());
    EXPECT_EQ(std::find(down.begin(), down.end(), "length"), down.end());
    EXPECT_EQ(nodesWithDownstream(edit, {"nope"}), std::vector<std::string>{});
}

TEST(GraphArrange, CarriesStopAtPortalsAndTakeTheirPinsAndDots) {
    json g = json::parse(defaultGraphJson());
    // Every link into noise teleported: what feeds it is no longer carried with it.
    std::vector<std::string> inputs;
    for (const json& c : g["connections"]) {
        if (c["to"] == "noise") inputs.push_back(c["input"].get<std::string>());
    }
    ASSERT_FALSE(inputs.empty());
    for (const std::string& in : inputs) ASSERT_TRUE(teleportLink(g, "noise", in, 0.0, 0.0, 500.0, 500.0));
    const GraphEdit edit(g.dump());
    const auto up = nodesWithUpstream(edit, {"color"});
    EXPECT_NE(std::find(up.begin(), up.end(), "noise"), up.end());
    EXPECT_EQ(std::find(up.begin(), up.end(), "length"), up.end()) << "not through the portal";
    const auto down = nodesWithDownstream(edit, {"length"});
    EXPECT_EQ(std::find(down.begin(), down.end(), "noise"), down.end()) << "not through the portal";
    // The exit by noise goes with it; the entry by length does not.
    const CarriedLayout withNoise = carriedWith(g, {"noise", "color"});
    ASSERT_EQ(withNoise.dots.size(), 1u);
    EXPECT_EQ(withNoise.dots[0].second, 0);
    // A pin between two moved nodes goes with them; with one of them alone it stays.
    std::string colorInput;
    for (const json& c : g["connections"]) {
        if (c["to"] == "color" && c["from"] == "noise") colorInput = c["input"].get<std::string>();
    }
    ASSERT_FALSE(colorInput.empty());
    const std::string pin = addPin(g, "color", colorInput, 5.0, 5.0);
    ASSERT_FALSE(pin.empty());
    EXPECT_EQ(carriedWith(g, {"noise", "color"}).pins, std::vector<std::string>{pin});
    EXPECT_TRUE(carriedWith(g, {"color"}).pins.empty());
    // On a backdrop: the pin and the exit at (0, 0) lie on it, the entry at (500, 500) does not.
    const CarriedLayout on = layoutOnBackdrop(g, {"", -10.0, -10.0, 40.0, 40.0});
    EXPECT_EQ(on.pins, std::vector<std::string>{pin});
    ASSERT_EQ(on.dots.size(), 1u);
    EXPECT_EQ(on.dots[0].second, 0);
}

TEST(GraphArrange, AlignGesturesLineUpWithTheNodeDraggedFrom) {
    const GraphEdit edit(defaultGraphJson());
    const std::vector<LayoutBox> boxes = {{"length", 0, 0, 100, 40}, {"noise", 200, 100, 100, 60}, {"color", 400, -50, 100, 40}};
    EXPECT_EQ(alignGestureAxis(5.0, 3.0), "");
    EXPECT_EQ(alignGestureAxis(50.0, 5.0), "row");
    EXPECT_EQ(alignGestureAxis(5.0, -50.0), "column");
    // Shift+A: the selection level with the anchor (a row), or under it (a column).
    auto row = alignGesture(edit, boxes, "length", {"noise", "color"}, "selected", 50.0, 5.0);
    EXPECT_DOUBLE_EQ(row["noise"].second, -10.0);
    EXPECT_DOUBLE_EQ(row["color"].second, 0.0);
    EXPECT_DOUBLE_EQ(row["noise"].first, 200.0);
    EXPECT_EQ(row.count("length"), 0u) << "the anchor stays";
    auto column = alignGesture(edit, boxes, "length", {"noise", "color"}, "selected", 0.0, 50.0);
    EXPECT_DOUBLE_EQ(column["noise"].first, 0.0);
    EXPECT_DOUBLE_EQ(column["noise"].second, 100.0);
    // Under the threshold nothing moves.
    EXPECT_TRUE(alignGesture(edit, boxes, "length", {"noise"}, "selected", 2.0, 2.0).empty());
    // Alt+A: spread evenly, the anchor where it is.
    std::vector<LayoutBox> spread = boxes;
    spread[2].x = 600.0;
    auto even = alignGesture(edit, spread, "length", {"noise", "color"}, "distribute", 50.0, 0.0);
    EXPECT_DOUBLE_EQ(even["noise"].first, 300.0);
    EXPECT_DOUBLE_EQ(even["color"].first, 600.0);
    // A: the chain upstream lined up (dragged left: a row level with the anchor; up: stacked above).
    const std::vector<LayoutBox> chain = {{"interpolate", 0, 300, 100, 40}, {"length", 200, 0, 100, 40}, {"noise", 400, 200, 100, 40}, {"color", 600, 500, 100, 40}};
    auto left = alignGesture(edit, chain, "color", {}, "chain", -60.0, 0.0);
    EXPECT_DOUBLE_EQ(left["noise"].second, 500.0);
    EXPECT_DOUBLE_EQ(left["length"].second, 500.0);
    auto above = alignGesture(edit, chain, "noise", {}, "chain", 0.0, -60.0);
    EXPECT_DOUBLE_EQ(above["length"].first, 400.0);
    EXPECT_DOUBLE_EQ(above["length"].second, 200.0 - 40.0 - 40.0);
    auto below = alignGesture(edit, chain, "noise", {}, "chain", 0.0, 60.0);
    EXPECT_DOUBLE_EQ(below["color"].second, 280.0);
    EXPECT_EQ(below.count("length"), 0u);
    EXPECT_THROW(alignGesture(edit, boxes, "length", {}, "nope", 50.0, 0.0), GraphError);
}

TEST(GraphEditByHand, APassThroughOutputCanBeRenamed) {
    GraphEdit edit(defaultGraphJson());
    EXPECT_THROW(edit.setPassThroughName("noise", "strands", "x"), GraphError) << "not passed through";
    ASSERT_TRUE(edit.setPassThrough("noise", "strands", true));
    EXPECT_EQ(edit.passThroughName("noise", "strands"), "");
    EXPECT_TRUE(edit.setPassThroughName("noise", "strands", "Before Noise"));
    EXPECT_FALSE(edit.setPassThroughName("noise", "strands", "Before Noise"));
    const json spec = json::parse(edit.nodeSpecJson("noise"));
    EXPECT_EQ(spec["outputs"][1]["name"], "thru_strands") << "links still read it by name";
    EXPECT_EQ(spec["outputs"][1]["label"], "Before Noise");
    // Saved and loaded with the graph.
    const GraphDocument doc = loadGraphJson(edit.toJson());
    EXPECT_EQ(GraphEdit(saveGraphJson(doc)).passThroughName("noise", "strands"), "Before Noise");
    // Cleared, and gone with the output.
    EXPECT_TRUE(edit.setPassThroughName("noise", "strands", ""));
    EXPECT_FALSE(json::parse(edit.toJson())["nodes"][5].contains("passThroughNames"));
    edit.setPassThroughName("noise", "strands", "Again");
    edit.setPassThrough("noise", "strands", false);
    EXPECT_FALSE(json::parse(edit.toJson())["nodes"][5].contains("passThroughNames"));
}

TEST(GraphEditByHand, PassThroughOutputsCanBeReordered) {
    GraphEdit edit(defaultGraphJson());
    const json inputs = json::parse(edit.nodeSpecJson("interpolate"))["inputs"];
    ASSERT_GE(inputs.size(), 2u);
    const std::string a = inputs[0]["name"], b = inputs[1]["name"];
    ASSERT_TRUE(edit.setPassThrough("interpolate", a, true));
    ASSERT_TRUE(edit.setPassThrough("interpolate", b, true));
    EXPECT_EQ(edit.passThrough("interpolate"), (std::vector<std::string>{a, b}));
    EXPECT_FALSE(edit.movePassThrough("interpolate", a, -1)) << "the first already";
    EXPECT_TRUE(edit.movePassThrough("interpolate", a, 1));
    EXPECT_EQ(edit.passThrough("interpolate"), (std::vector<std::string>{b, a}));
    // Drawn in that order, kept through save and load.
    const json outputs = json::parse(edit.nodeSpecJson("interpolate"))["outputs"];
    EXPECT_EQ(outputs[outputs.size() - 2]["name"], GraphEdit::passThroughOutput(b));
    EXPECT_EQ(outputs[outputs.size() - 1]["name"], GraphEdit::passThroughOutput(a));
    EXPECT_EQ(GraphEdit(saveGraphJson(loadGraphJson(edit.toJson()))).passThrough("interpolate"), (std::vector<std::string>{b, a}));
    EXPECT_FALSE(edit.movePassThrough("interpolate", a, 5));
    EXPECT_TRUE(edit.movePassThrough("interpolate", a, -5));
    EXPECT_EQ(edit.passThrough("interpolate"), (std::vector<std::string>{a, b}));
    EXPECT_THROW(edit.movePassThrough("noise", "strands", 1), GraphError);
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

TEST(GraphArrange, TidyLaysOutTheSelectionOrAllWithBackdropsAsGroups) {
    // Every node of the default graph scattered about, a backdrop round noise and color.
    json g = json::parse(defaultGraphJson());
    std::vector<LayoutBox> boxes;
    double k = 0.0;
    for (json& n : g["nodes"]) {
        const std::string name = n["name"];
        LayoutBox b{name, std::fmod(k * 370.0, 900.0), std::fmod(k * 530.0, 700.0), 180.0, 80.0};
        if (name == "noise") b.x = 2000.0, b.y = 2000.0;
        if (name == "color") b.x = 2300.0, b.y = 1950.0;
        n["ui"] = {{"x", b.x}, {"y", b.y}};
        boxes.push_back(b);
        k += 1.0;
    }
    g["backdrops"] = json::array({{{"title", "Look"}, {"x", 1950.0}, {"y", 1900.0}, {"w", 600.0}, {"h", 300.0}, {"color", {0.3, 0.3, 0.3}}}});
    const json out = tidyGraph(g, boxes, {});
    std::map<std::string, LayoutBox> at;
    for (const json& n : out["nodes"]) {
        const std::string name = n["name"];
        LayoutBox b = *std::find_if(boxes.begin(), boxes.end(), [&](const LayoutBox& x) { return x.name == name; });
        b.x = n["ui"]["x"];
        b.y = n["ui"]["y"];
        at[name] = b;
    }
    // Links run left to right; the backdrop holds noise and color, and no other node.
    for (const json& c : out["connections"]) EXPECT_LT(at[c["from"]].right(), at[c["to"]].x) << c.dump();
    const json& bd = out["backdrops"][0];
    const LayoutBox backdrop{"Look", bd["x"], bd["y"], bd["w"], bd["h"]};
    for (const auto& [name, b] : at) EXPECT_EQ(onBackdrop(backdrop, b), name == "noise" || name == "color") << name;
    EXPECT_EQ(bd["title"], "Look");
    // A chain runs straight: length level with interpolate.
    EXPECT_NEAR(at["length"].centerY(), at["interpolate"].centerY(), 1e-6);
    // Nodes do not overlap.
    for (const auto& [a, ba] : at)
        for (const auto& [b, bb] : at)
            if (a < b) EXPECT_FALSE(ba.x < bb.right() && bb.x < ba.right() && ba.y < bb.bottom() && bb.y < ba.bottom()) << a << " " << b;

    // Selected: only those move, from the selection's top-left.
    const json some = tidyGraph(g, boxes, {"length", "width"});
    for (std::size_t i = 0; i < some["nodes"].size(); ++i) {
        const std::string name = some["nodes"][i]["name"];
        if (name != "length" && name != "width") EXPECT_EQ(some["nodes"][i]["ui"], g["nodes"][i]["ui"]) << name;
    }
    // A backdrop with only some of its nodes selected stays where it is (grown when it must).
    const json part = tidyGraph(g, boxes, {"noise", "length"});
    EXPECT_GE(part["backdrops"][0]["w"].get<double>(), 600.0 - 1e-9);
    EXPECT_LE(part["backdrops"][0]["x"].get<double>(), 1950.0 + 1e-9);
    // Folded: its nodes keep their places on it as it moves.
    g["backdrops"][0]["collapsed"] = true;
    const json folded = tidyGraph(g, boxes, {});
    double nx = 0, cx = 0;
    for (const json& n : folded["nodes"]) {
        if (n["name"] == "noise") nx = n["ui"]["x"];
        if (n["name"] == "color") cx = n["ui"]["x"];
    }
    EXPECT_NEAR(cx - nx, 300.0, 1e-9);
    EXPECT_NEAR(nx - folded["backdrops"][0]["x"].get<double>(), 50.0, 1e-9);
}

TEST(GraphArrange, APinMovesWithWhatItLinks) {
    json g = json::parse(defaultGraphJson());
    std::vector<LayoutBox> boxes;
    double k = 0.0;
    for (const json& n : g["nodes"]) boxes.push_back({n["name"], k * 10.0, k * 300.0, 180.0, 80.0}), k += 1.0;
    const std::string pin = addPin(g, "noise", "strands", 55.0, 66.0);
    ASSERT_FALSE(pin.empty());
    const json out = tidyGraph(g, boxes, {});
    double x = 0, y = 0;
    ASSERT_TRUE(pinPosition(out, pin, &x, &y));
    EXPECT_FALSE(x == 55.0 && y == 66.0);
}

TEST(GraphArrange, PromoteAnInnerParamToTheCompoundFromInside) {
    GraphEdit edit(defaultGraphJson());
    const std::string compound = edit.collapse({"color", "width"}, "look");
    const std::string whole = edit.toJson();
    EXPECT_TRUE(paramPromotable(whole, compound, "color", "melanin"));
    EXPECT_FALSE(paramPromoted(whole, compound, "color", "melanin"));
    EXPECT_FALSE(paramPromotable(whole, compound, "color", "nope"));
    const std::string promoted = promoteParam(whole, compound, "color", "melanin", true);
    EXPECT_TRUE(paramPromoted(promoted, compound, "color", "melanin"));
    // The compound shows it as a param of its own.
    bool shown = false;
    const json spec = json::parse(GraphEdit(promoted).nodeSpecJson(compound));
    for (const json& p : spec["params"]) shown = shown || p["name"] == "melanin";
    EXPECT_TRUE(shown);
    // Under a header named after its node, a group per node (color's, then width's).
    const std::string both = promoteParam(promoted, compound, "width", "width", true);
    const json params = json::parse(GraphEdit(both).nodeSpecJson(compound))["params"];
    const std::vector<std::string> sections = paramSections(params);
    std::map<std::string, std::string> sectionOf;
    for (std::size_t i = 0; i < params.size(); ++i) sectionOf[params[i]["name"]] = sections[i];
    EXPECT_EQ(sectionOf["melanin"], "color");
    EXPECT_EQ(sectionOf["width"], "width");
    EXPECT_EQ(sectionOf["definition"], "");
    // One node's alone still gets its header.
    const json one = json::parse(GraphEdit(promoted).nodeSpecJson(compound))["params"];
    const std::vector<std::string> oneSections = paramSections(one);
    for (std::size_t i = 0; i < one.size(); ++i) {
        if (one[i]["name"] == "melanin") EXPECT_EQ(oneSections[i], "color");
    }
    EXPECT_EQ(promoteParam(promoted, compound, "color", "melanin", true), promoted);
    const std::string back = promoteParam(promoted, compound, "color", "melanin", false);
    EXPECT_FALSE(paramPromoted(back, compound, "color", "melanin"));
    EXPECT_THROW(promoteParam(whole, "", "color", "melanin", true), GraphError);
    EXPECT_THROW(promoteParam(whole, compound, "color", "nope", true), GraphError);
    // A compound inside a compound: promoted to the inner one, written back into the outer.
    GraphEdit nest(defaultGraphJson());
    const std::string outer = nest.collapse({"noise", "color", "width"}, "outer");
    GraphEdit inside(compoundGraphJson(nest.toJson(), outer));
    const std::string inner = inside.collapse({"color"}, "inner");
    const std::string nested = withCompoundGraph(nest.toJson(), outer, inside.toJson());
    const std::string path = outer + "/" + inner;
    const std::string deep = promoteParam(nested, path, "color", "melanin", true);
    EXPECT_TRUE(paramPromoted(deep, path, "color", "melanin"));
    EXPECT_FALSE(paramPromoted(nested, path, "color", "melanin"));
}

TEST(GraphArrange, ABackdropsTitleIsKeptClearOfItsNodes) {
    json bd = {{"title", "Look"}, {"x", 0.0}, {"y", 0.0}, {"w", 400.0}, {"h", 300.0}};
    EXPECT_EQ(backdropHeader(bd), 28.0);
    bd["fontSize"] = 40;
    EXPECT_NEAR(backdropHeader(bd), 76.0, 1e-9);
    EXPECT_NEAR(backdropContentTop(bd), 76.0, 1e-9);
    bd["description"] = "one line";
    EXPECT_NEAR(backdropContentTop(bd), 76.0 + 6.0 + 16.0, 1e-9);
    bd.erase("description");
    EXPECT_GT(backdropTitleWidth(bd), backdropTitleWidth(json{{"title", "Look"}}));

    // A bigger title pushes the nodes on it down, the backdrop as much taller; one off it stays.
    json g = json::parse(defaultGraphJson());
    g["backdrops"] = json::array({bd});
    const std::vector<LayoutBox> boxes{{"noise", 40.0, 50.0, 180.0, 80.0}, {"color", 240.0, 120.0, 180.0, 80.0},
                                       {"width", 900.0, 50.0, 180.0, 80.0}};
    const json out = clearBackdropTitle(g, 0, boxes);
    std::map<std::string, json> ui;
    for (const json& n : out["nodes"]) ui[n["name"]] = n.value("ui", json());
    EXPECT_NEAR(ui["noise"]["y"].get<double>(), 76.0 + 16.0, 1e-9);
    EXPECT_NEAR(ui["color"]["y"].get<double>(), 120.0 + 42.0, 1e-9);
    EXPECT_TRUE(ui["width"].is_null());
    EXPECT_NEAR(out["backdrops"][0]["h"].get<double>(), 342.0, 1e-9);
    // Already clear: nothing moves.
    EXPECT_EQ(clearBackdropTitle(out, 0, {{"noise", 40.0, 92.0, 180.0, 80.0}}), out);
    // A long title widens a narrow backdrop.
    g["backdrops"][0]["w"] = 100.0;
    EXPECT_GT(clearBackdropTitle(g, 0, {})["backdrops"][0]["w"].get<double>(), 100.0);

    // Tidy fits a backdrop under its title, however big: its nodes start under the title bar.
    json t = json::parse(defaultGraphJson());
    std::vector<LayoutBox> all;
    double k = 0.0;
    for (const json& n : t["nodes"]) {
        LayoutBox b{n["name"], k * 250.0, 0.0, 180.0, 80.0};
        if (b.name == "noise") b.x = 2000.0, b.y = 2000.0;
        if (b.name == "color") b.x = 2300.0, b.y = 2000.0;
        all.push_back(b);
        k += 1.0;
    }
    t["backdrops"] = json::array({{{"title", "A long title for the look"}, {"x", 1950.0}, {"y", 1900.0}, {"w", 600.0}, {"h", 300.0}, {"fontSize", 48}}});
    const json tidied = tidyGraph(t, all, {});
    const json& b = tidied["backdrops"][0];
    for (const json& n : tidied["nodes"]) {
        if (n["name"] != "noise" && n["name"] != "color") continue;
        EXPECT_GE(n["ui"]["y"].get<double>(), b["y"].get<double>() + backdropHeader(b) + 1.0) << n["name"];
    }
    EXPECT_GE(b["w"].get<double>(), backdropTitleWidth(b) - 1e-9);
}

}  // namespace
}  // namespace groomlab
