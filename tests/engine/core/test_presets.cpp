// SPDX-License-Identifier: Apache-2.0
// Graph presets (presets.h): the shipped files are found and described, fitted to a scalp size
// as the Maya host's presets.py did (which now fits through this), their graphs load, and a
// preset made from a graph reads back. PresetLooks: what every shipped preset makes on a
// reference groom, against recorded digests (tests/data/golden/PresetLooks.json).

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/presets.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;

// The first param of any operator with a given unit: (type, param).
std::pair<std::string, std::string> paramWithUnit(const std::string& unit) {
    for (const json& op : json::parse(operatorSpecsJson())) {
        for (const json& p : op.value("params", json::array())) {
            if (p.value("unit", std::string()) == unit) return {op.value("type", std::string()), p.value("name", std::string())};
        }
    }
    return {};
}

// An operator's param spec (operatorSpecsJson), or null.
const json* operatorParamSpecForTest(const std::string& type, const std::string& param) {
    static const json specs = json::parse(operatorSpecsJson());
    for (const json& op : specs) {
        if (op.value("type", std::string()) != type) continue;
        for (const json& p : op["params"]) {
            if (p.value("name", std::string()) == param) return &p;
        }
    }
    return nullptr;
}

const json& nodeNamed(const json& graph, const std::string& name) {
    for (const json& n : graph["nodes"]) {
        if (n.value("name", std::string()) == name) return n;
    }
    throw std::runtime_error("no node " + name);
}

const json& nodeOfType(const json& graph, const std::string& type) {
    for (const json& n : graph["nodes"]) {
        if (n.value("type", std::string()) == type) return n;
    }
    throw std::runtime_error("no node of type " + type);
}

}  // namespace

TEST(Presets, ShippedPresetsAreFoundDescribedAndLoad) {
    const std::vector<std::string> dirs = presetSearchDirs({}, kShipped);
    ASSERT_FALSE(dirs.empty());
    EXPECT_EQ(dirs.back(), kShipped);
    const std::vector<PresetInfo> presets = listPresets(dirs);
    ASSERT_GT(presets.size(), 10u);
    bool basics = false;
    for (const PresetInfo& p : presets) {
        EXPECT_FALSE(p.title.empty()) << p.name;
        EXPECT_FALSE(p.description.empty()) << p.name << ": every shipped preset is described for the menu";
        EXPECT_TRUE(std::find(presetCategories().begin(), presetCategories().end(), p.category) != presetCategories().end())
            << p.name << " in category '" << p.category << "'";
        EXPECT_EQ(presetPath(p.name, dirs), p.path);
        // Its graph loads as a groom stores it (the preset's own fields gone, the backdrops kept).
        std::ifstream in(std::filesystem::u8path(p.path));
        std::stringstream text;
        text << in.rdbuf();
        const std::string graphText = presetGraphJson(text.str(), 20.0);
        EXPECT_NO_THROW(loadGraphJson(graphText)) << p.name;
        const json graph = json::parse(graphText);
        for (const std::string& field : presetFields()) {
            if (field != "backdrops") EXPECT_FALSE(graph.contains(field)) << p.name << " keeps " << field;
        }
        if (p.name == "hair_basics") {
            basics = true;
            EXPECT_EQ(p.title, "Hair basics");
            EXPECT_EQ(p.category, "Hair");
        }
    }
    EXPECT_TRUE(basics);
    EXPECT_EQ(presetPath("no_such_preset", dirs), "");
    EXPECT_EQ(presetTitle("wet_hair"), "Wet hair");
    EXPECT_EQ(presetNameError("mine"), "");
    EXPECT_NE(presetNameError("bad/name"), "");
    EXPECT_NE(presetNameError(""), "");
}

TEST(Presets, AnEarlierDirectoryHidesALaterOne) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_presets_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "hair_basics.json");
        out << R"({"version": 1, "about": {"title": "Mine", "category": "Hair"}, "nodes": [], "connections": []})";
        std::ofstream broken(dir / "broken.json");
        broken << "not json";
    }
    const std::vector<PresetInfo> presets = listPresets({dir.u8string(), kShipped});
    const auto basics = std::find_if(presets.begin(), presets.end(), [](const PresetInfo& p) { return p.name == "hair_basics"; });
    ASSERT_NE(basics, presets.end());
    EXPECT_EQ(basics->title, "Mine") << "the user's hides the shipped one";
    const auto broken = std::find_if(presets.begin(), presets.end(), [](const PresetInfo& p) { return p.name == "broken"; });
    ASSERT_NE(broken, presets.end());
    EXPECT_EQ(broken->description.rfind("Cannot be read", 0), 0u);
    EXPECT_EQ(broken->title, "Broken");
    std::filesystem::remove_all(dir);
}

TEST(Presets, FittingScalesByUnitMapsIncluded) {
    const auto [op, param] = paramWithUnit("length");
    ASSERT_FALSE(op.empty());
    json graph = {{"size", 1.0}, {"nodes", json::array({{{"name", "n"}, {"type", op}, {"params", {{param, {{"texture", "a.png"}}}}}}})}};
    json fitted = json::parse(fitPresetJson(graph.dump(), 2.0));
    EXPECT_FALSE(fitted.contains("size"));
    // A map without a range: its default max (1) scales too.
    EXPECT_EQ(fitted["nodes"][0]["params"][param], json({{"texture", "a.png"}, {"max", 2.0}}));
    graph["nodes"][0]["params"][param] = {{"texture", "a.png"}, {"min", 0.25}, {"max", 1.5}};
    fitted = json::parse(fitPresetJson(graph.dump(), 2.0));
    EXPECT_EQ(fitted["nodes"][0]["params"][param], json({{"texture", "a.png"}, {"min", 0.5}, {"max", 3.0}}));
    // A number scales; the same size, or none, leaves it; "size" goes either way.
    graph["nodes"][0]["params"][param] = 3.0;
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(graph.dump(), 4.0))["nodes"][0]["params"][param].get<double>(), 12.0);
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(graph.dump(), 1.0))["nodes"][0]["params"][param].get<double>(), 3.0);
    const json same = json::parse(fitPresetJson(graph.dump(), std::nullopt));
    EXPECT_DOUBLE_EQ(same["nodes"][0]["params"][param].get<double>(), 3.0);
    EXPECT_FALSE(same.contains("size"));
    // A density scales by the inverse square, a per-length rate by the inverse.
    const auto [dop, dparam] = paramWithUnit("density");
    ASSERT_FALSE(dop.empty());
    json dense = {{"size", 1.0}, {"nodes", json::array({{{"name", "d"}, {"type", dop}, {"params", {{dparam, 100.0}}}}})}};
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(dense.dump(), 10.0))["nodes"][0]["params"][dparam].get<double>(), 1.0);
    const auto [rop, rparam] = paramWithUnit("perLength");
    ASSERT_FALSE(rop.empty());
    json rate = {{"size", 1.0}, {"nodes", json::array({{{"name", "r"}, {"type", rop}, {"params", {{rparam, 2.0}}}}})}};
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(rate.dump(), 4.0))["nodes"][0]["params"][rparam].get<double>(), 0.5);
    EXPECT_THROW(fitPresetJson("[1, 2]", 1.0), std::runtime_error);
}

TEST(Presets, FittingReachesInsideCompoundsAndKeyedControls) {
    // A feather design is a Compound: its inner nodes' lengths and its exposed params scale too.
    std::ifstream in(std::filesystem::u8path(presetPath("feathers_contour", {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    const json design = json::parse(text.str());
    ASSERT_DOUBLE_EQ(design["size"].get<double>(), 2.0);
    const json fitted = json::parse(fitPresetJson(text.str(), 20.0));
    const json& node = nodeOfType(fitted, "Compound");
    const json& original = nodeOfType(design, "Compound");
    EXPECT_NEAR(node["params"]["vaneWidth"].get<double>(), original["params"]["vaneWidth"].get<double>() * 10.0, 1e-9);
    EXPECT_NEAR(node["params"]["barbWidth"].get<double>(), original["params"]["barbWidth"].get<double>() * 10.0, 1e-9);
    EXPECT_EQ(node["params"]["count"], original["params"]["count"]) << "no unit";
    const json& inner = node["params"]["definition"]["graph"];
    const json& was = original["params"]["definition"]["graph"];
    EXPECT_NEAR(nodeNamed(inner, "propagate")["params"]["length"].get<double>(),
                nodeNamed(was, "propagate")["params"]["length"].get<double>() * 10.0, 1e-9);
    EXPECT_NEAR(nodeNamed(inner, "ruffle")["params"]["amplitude"].get<double>(),
                nodeNamed(was, "ruffle")["params"]["amplitude"].get<double>() * 10.0, 1e-9);
    EXPECT_EQ(nodeNamed(inner, "propagate")["params"]["angle"], nodeNamed(was, "propagate")["params"]["angle"]);
    EXPECT_NEAR(nodeOfType(fitted, "Scatter")["params"]["density"].get<double>(), 14.0 / 100.0, 1e-9);

    // A keyed length's control value scales like the length; a keyed count's does not.
    const auto [op, param] = paramWithUnit("length");
    json keyed = {{"size", 1.0},
                  {"controls", {{"ctl_len", 2.0}, {"ctl_other", 5.0}}},
                  {"nodes", json::array({{{"name", "n"}, {"type", op}, {"params", {{param, 1.0}}}, {"paramControls", {{param, "len"}}}}})}};
    const json controls = json::parse(presetControlsJson(keyed.dump(), 3.0));
    EXPECT_DOUBLE_EQ(controls["ctl_len"].get<double>(), 6.0);
    EXPECT_DOUBLE_EQ(controls["ctl_other"].get<double>(), 5.0);
    EXPECT_EQ(presetControlsJson(R"({"nodes": []})", 3.0), "{}");
    // The graph takes the control values it holds itself: a Control node's value, a keyed param.
    json graph = {{"nodes", json::array({{{"name", "c"}, {"type", "Control"}, {"params", {{"name", "amount"}, {"value", 0.1}}}},
                                         {{"name", "n"}, {"type", op}, {"params", {{param, 1.0}}}, {"paramControls", {{param, "len"}}}}})}};
    const json applied = json::parse(applyPresetControlsToGraph(graph.dump(), R"({"ctl_amount": 0.75, "ctl_len": 6.0})"));
    EXPECT_DOUBLE_EQ(nodeNamed(applied, "c")["params"]["value"].get<double>(), 0.75);
    EXPECT_DOUBLE_EQ(nodeNamed(applied, "n")["params"][param].get<double>(), 6.0);
}

TEST(Presets, FittingScalesVectorsConditionalUnitsAndExpressionVariables) {
    // Positions, moves and sizes (vec3 of length) scale on every component, set or at defaults.
    json graph = {{"size", 1.0},
                  {"nodes", json::array({{{"name", "t"}, {"type", "Transform"},
                                          {"params", {{"translate", {1.0, -2.0, 0.5}}, {"rotate", {10.0, 20.0, 30.0}}}}},
                                         {{"name", "g"}, {"type", "Group"}, {"params", {{"by", "box"}, {"center", {0.0, 1.0, 2.0}}}}}})}};
    json fitted = json::parse(fitPresetJson(graph.dump(), 3.0));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["translate"], json({3.0, -6.0, 1.5}));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["rotate"], json({10.0, 20.0, 30.0})) << "angles stay";
    EXPECT_EQ(nodeNamed(fitted, "g")["params"]["center"], json({0.0, 3.0, 6.0}));
    EXPECT_EQ(nodeNamed(fitted, "g")["params"]["size"], json({3.0, 3.0, 3.0})) << "the default box, fitted";
    EXPECT_FALSE(nodeNamed(fitted, "t")["params"].contains("pivot")) << "a default of zero stays zero: left out";
    // A vector taking an expression in its place stays.
    graph["nodes"][0]["params"]["translate"] = {{"expression", "N * 2"}};
    fitted = json::parse(fitPresetJson(graph.dump(), 3.0));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["translate"], json({{"expression", "N * 2"}}));

    // Noise's frequency is per scene unit in world space only: there it scales, per strand length not.
    const json* frequency = operatorParamSpecForTest("Noise", "frequency");
    ASSERT_TRUE(frequency);
    EXPECT_EQ((*frequency)["unit"], "perLength");
    EXPECT_EQ((*frequency)["unitWhen"], json({{"param", "space"}, {"in", {"world"}}}));
    json noise = {{"size", 1.0},
                  {"nodes", json::array({{{"name", "w"}, {"type", "Noise"}, {"params", {{"space", "world"}, {"frequency", 4.0}}}},
                                         {{"name", "s"}, {"type", "Noise"}, {"params", {{"frequency", 4.0}}}}})}};
    fitted = json::parse(fitPresetJson(noise.dump(), 2.0));
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "w")["params"]["frequency"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "s")["params"]["frequency"].get<double>(), 4.0) << "strand space: per strand length";
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "s")["params"]["amplitude"].get<double>(), 0.4) << "the default amplitude, fitted";
    // The default space (strand) leaves the default frequency as it is on a new node too.
    const json added = json::parse(fitNewNodeParamsJson("Noise", "", kDefaultsScalpSize * 10.0));
    EXPECT_FALSE(added.contains("frequency"));
    EXPECT_DOUBLE_EQ(added["amplitude"].get<double>(), 2.0);
    const json addedWorld = json::parse(fitNewNodeParamsJson("Noise", R"({"space": "world"})", kDefaultsScalpSize * 10.0));
    EXPECT_DOUBLE_EQ(addedWorld["frequency"].get<double>(), 0.3);

    // A field plugged into a param is a factor of the param's own number (field.h): its range stays.
    json masked = {{"size", 1.0},
                   {"nodes", json::array({{{"name", "m"}, {"type", "Map"}, {"params", {{"name", "mask"}, {"min", 0.2}, {"max", 0.9}}}},
                                          {{"name", "d"}, {"type", "Displace"}, {"params", {{"distance", 0.5}}}, {"paramInputs", {"distance"}}}})},
                   {"connections", json::array({{{"from", "m"}, {"to", "d"}, {"input", "distance"}}})}};
    fitted = json::parse(fitPresetJson(masked.dump(), 4.0));
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "m")["params"]["max"].get<double>(), 0.9);
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "d")["params"]["distance"].get<double>(), 2.0);

    // An Expression's variables that say their unit fit by it (a keyable one's control value too).
    const json variables = {{"radius", {{"value", 2.0}, {"unit", "length"}}},
                            {"amount", 0.5},
                            {"center", {0.0, 1.0, 0.0}},
                            {"reach", {{"value", 1.0}, {"unit", "length"}, {"control", "reach"}}}};
    json expression = {{"size", 1.0},
                       {"controls", {{"ctl_reach", 1.5}}},
                       {"nodes", json::array({{{"name", "e"}, {"type", "Expression"},
                                               {"params", {{"expression", "$radius"}, {"variables", variables.dump()}}}}})}};
    fitted = json::parse(fitPresetJson(expression.dump(), 10.0));
    const json fittedVariables = json::parse(nodeNamed(fitted, "e")["params"]["variables"].get<std::string>());
    EXPECT_DOUBLE_EQ(fittedVariables["radius"]["value"].get<double>(), 20.0);
    EXPECT_DOUBLE_EQ(fittedVariables["amount"].get<double>(), 0.5);
    EXPECT_EQ(fittedVariables["center"], json({0.0, 1.0, 0.0})) << "no unit said";
    EXPECT_DOUBLE_EQ(json::parse(presetControlsJson(expression.dump(), 10.0))["ctl_reach"].get<double>(), 15.0);
}

TEST(Presets, NewNodesAndTheDefaultGraphFitTheScalp) {
    // A node added on a groom takes its defaults fitted from the 20-unit head they are authored at.
    const json scatter = json::parse(fitNewNodeParamsJson("Scatter", "", 200.0));  // a 2 m creature, in cm
    EXPECT_DOUBLE_EQ(scatter["density"].get<double>(), 0.8);
    EXPECT_DOUBLE_EQ(scatter["length"].get<double>(), 10.0);
    EXPECT_FALSE(scatter.contains("seed")) << "no unit: left to its default";
    const json given = json::parse(fitNewNodeParamsJson("Scatter", R"({"length": 3})", 200.0));
    EXPECT_DOUBLE_EQ(given["length"].get<double>(), 3.0) << "what is given stays";
    EXPECT_EQ(fitNewNodeParamsJson("Scatter", "", kDefaultsScalpSize), "{}");
    EXPECT_EQ(fitNewNodeParamsJson("Scatter", "", std::nullopt), "{}");
    GraphEdit edit(defaultGraphJson());
    const std::string made = edit.addNode("Width", "", "", std::nullopt, std::nullopt, 2.0);
    EXPECT_NEAR(json::parse(edit.toJson())["nodes"].back()["params"]["width"].get<double>(), 0.0007, 1e-12) << made;

    // The default graph a new groom stores: its nodes' defaults fitted the same way.
    const json small = json::parse(defaultGraphJsonFor(2.0));
    EXPECT_NEAR(nodeOfType(small, "Scatter")["params"]["density"].get<double>(), 8000.0, 1e-6);
    EXPECT_NEAR(nodeOfType(small, "Scatter")["params"]["length"].get<double>(), 0.1, 1e-9);
    EXPECT_NEAR(nodeOfType(small, "Noise")["params"]["amplitude"].get<double>(), 0.02, 1e-9);
    EXPECT_NEAR(nodeOfType(small, "Width")["params"]["width"].get<double>(), 0.0007, 1e-12);
    EXPECT_DOUBLE_EQ(nodeOfType(small, "Length")["params"]["randomness"].get<double>(), 0.2) << "its own params as they are";
    EXPECT_EQ(json::parse(defaultGraphJsonFor(std::nullopt)), json::parse(defaultGraphJson()));
    EXPECT_EQ(json::parse(defaultGraphJsonFor(kDefaultsScalpSize)), json::parse(defaultGraphJson()));
}

TEST(Presets, RequiredMapsAndMakingOne) {
    std::ifstream in(std::filesystem::u8path(presetPath("painted_maps", {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    const std::vector<PresetMap> maps = presetRequiredMaps(text.str());
    ASSERT_EQ(maps.size(), 1u);
    EXPECT_EQ(maps[0].name, "groomMasks");
    EXPECT_EQ(maps[0].kind, "vertices");
    EXPECT_EQ(maps[0].color.size(), 3u);
    EXPECT_TRUE(presetRequiredMaps(R"({"nodes": []})").empty());

    // The guide groups a preset reads: the hand-placed flight feathers' (made empty, drawn into).
    std::ifstream handPlaced(std::filesystem::u8path(presetPath("hand_placed_feathers", {kShipped})));
    std::stringstream handText;
    handText << handPlaced.rdbuf();
    const std::vector<PresetGroup> groups = presetRequiredGroups(handText.str());
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].name, "feathers");
    EXPECT_TRUE(groups[0].draw);
    EXPECT_TRUE(presetRequiredMaps(handText.str()).empty());
    EXPECT_TRUE(presetRequiredGroups(text.str()).empty());
    const std::vector<PresetGroup> some =
        presetRequiredGroups(R"({"requires": {"groups": [{"name": "tail"}, {"name": ""}, {"name": "tail", "draw": true}, 3]}, "nodes": []})");
    ASSERT_EQ(some.size(), 1u) << "named once, nameless entries skipped";
    EXPECT_EQ(some[0].name, "tail");
    EXPECT_FALSE(some[0].draw);

    // A preset made from the default graph: titled, sized, the layout kept, and it loads again.
    json graph = json::parse(defaultGraphJson());
    graph["backdrops"] = json::array({{{"title", "All"}, {"x", 0}, {"y", 0}, {"w", 100}, {"h", 100}}});
    const std::string made = makePresetJson(graph.dump(), 20.0, "Mine", "  what it is ", "Hair", R"({"ctl_a": 1})");
    const json preset = json::parse(made);
    EXPECT_EQ(made.find("\"version\""), made.find_first_of('"')) << "the version comes first";
    EXPECT_DOUBLE_EQ(preset["size"].get<double>(), 20.0);
    EXPECT_EQ(preset["about"]["title"], "Mine");
    EXPECT_EQ(preset["about"]["description"], "what it is");
    EXPECT_EQ(preset["about"]["category"], "Hair");
    EXPECT_EQ(preset["backdrops"].size(), 1u);
    EXPECT_EQ(preset["controls"]["ctl_a"], 1);
    const json back = json::parse(presetGraphJson(made, 20.0));
    EXPECT_EQ(back["nodes"].size(), graph["nodes"].size());
    EXPECT_EQ(back["backdrops"].size(), 1u);
    EXPECT_FALSE(back.contains("about"));
    EXPECT_NO_THROW(loadGraphJson(back.dump()));
    // No about, no size: neither is written.
    const json bare = json::parse(makePresetJson(defaultGraphJson(), std::nullopt, "", "", ""));
    EXPECT_FALSE(bare.contains("about"));
    EXPECT_FALSE(bare.contains("size"));

    // The scalp size a preset records: the largest extent of the mesh.
    const auto plane = test::makePlane(2, 3.0f);
    ASSERT_TRUE(presetScalpSize(*plane));
    EXPECT_FLOAT_EQ(static_cast<float>(*presetScalpSize(*plane)), 3.0f);
}

std::string readPreset(const std::string& name) {
    std::ifstream in(std::filesystem::u8path(presetPath(name, {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

TEST(Presets, InsertingAPresetWrapsItsLookAsACompoundAfterTheNode) {
    // The groom: hair_basics on a 40-unit scalp (the presets are made at 20: sizes double).
    const std::string basics = readPreset("hair_basics");
    const std::string graph = presetGraphJson(basics, 40.0);
    json before = json::parse(graph);
    const std::string interpolate = nodeOfType(before, "Interpolate")["name"];
    const std::string output = before["output"];
    // Nothing selected: before the Output. The Compound takes what fed the Output and feeds it.
    std::string made;
    const std::string inserted = insertPresetJson(graph, readPreset("wet"), 40.0, "", presetNodeName("wet"), &made);
    EXPECT_EQ(made, "wet");
    EXPECT_NO_THROW(loadGraphJson(inserted));
    GraphEdit edit(inserted);
    EXPECT_NO_THROW(edit.checkConnections());
    const json after = json::parse(inserted);
    const json& compound = nodeNamed(after, "wet");
    EXPECT_EQ(compound["type"], "Compound");
    std::string feedsOutput, fedByCompound;
    for (const json& c : after["connections"]) {
        if (c["to"] == output) feedsOutput = c["from"];
        if (c["to"] == "wet") fedByCompound = c["from"];
    }
    EXPECT_EQ(feedsOutput, "wet");
    std::string lastBefore;
    for (const json& c : before["connections"]) {
        if (c["to"] == output) lastBefore = c["from"];
    }
    EXPECT_EQ(fedByCompound, lastBefore);
    EXPECT_EQ(after["nodes"].size(), before["nodes"].size() + 1);
    // The look inside: the Input, the preset's look nodes (not its Scalp, Guides, Scatter,
    // Interpolate or Output), fitted to the scalp (the Clump's count is not a size; its params as
    // given; the HairColor's melanin unchanged).
    const json& definition = compound["params"]["definition"];
    std::set<std::string> inner;
    for (const json& n : definition["graph"]["nodes"]) inner.insert(n["name"]);
    EXPECT_EQ(inner, (std::set<std::string>{"strands", "wetClump", "wetColor"}));
    EXPECT_EQ(definition["graph"]["output"], "wetColor");
    EXPECT_EQ(definition["inputs"].size(), 1u);
    EXPECT_EQ(definition["inputs"][0]["name"], "strands");
    // The nodes right of the insertion point moved over a column; the Compound sits in the gap.
    const double outputX = nodeNamed(after, output)["ui"]["x"];
    const double outputXBefore = nodeNamed(before, output)["ui"]["x"];
    EXPECT_DOUBLE_EQ(outputX, outputXBefore + 240.0);
    EXPECT_DOUBLE_EQ(compound["ui"]["x"].get<double>(), outputXBefore);

    // After a chosen node: the Compound takes over everything that node's strands fed.
    std::size_t fedBefore = 0;
    for (const json& c : before["connections"]) fedBefore += c["from"] == interpolate ? 1 : 0;
    ASSERT_GE(fedBefore, 1u);
    const std::string mid = insertPresetJson(graph, readPreset("frizz"), 40.0, interpolate, "frizz", &made);
    EXPECT_EQ(made, "frizz");
    EXPECT_NO_THROW(loadGraphJson(mid));
    const json midGraph = json::parse(mid);
    std::size_t fromInterpolate = 0, fromFrizz = 0;
    for (const json& c : midGraph["connections"]) {
        if (c["from"] == interpolate) {
            ++fromInterpolate;
            EXPECT_EQ(c["to"], "frizz");
        }
        if (c["from"] == "frizz") ++fromFrizz;
    }
    EXPECT_EQ(fromInterpolate, 1u);
    EXPECT_EQ(fromFrizz, fedBefore);
    // Fitted: the Noise's amplitude (a length) doubled from the preset's 0.12 at 20 units.
    const json& frizz = nodeNamed(midGraph, "frizz")["params"]["definition"]["graph"];
    for (const json& n : frizz["nodes"]) {
        if (n["name"] == "frizzNoise") EXPECT_NEAR(n["params"]["amplitude"].get<double>(), 0.24, 1e-9);
    }
    // Twice: the second takes a free name.
    const std::string twice = insertPresetJson(mid, readPreset("frizz"), 40.0, "", "frizz", &made);
    EXPECT_EQ(made, "frizz1");
    EXPECT_NO_THROW(loadGraphJson(twice));

    // Refused: a node giving no strands, an unknown node, a preset with no look.
    const std::string scalp = nodeOfType(before, "Scalp")["name"];
    EXPECT_THROW(insertPresetJson(graph, readPreset("wet"), 40.0, scalp), std::runtime_error);
    EXPECT_THROW(insertPresetJson(graph, readPreset("wet"), 40.0, "noSuchNode"), std::runtime_error);
    const json bare = json::parse(R"j({"version": 1, "output": "out",
        "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"}, {"name": "out", "type": "Output"}],
        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "out", "input": "strands"}]})j");
    EXPECT_THROW(insertPresetJson(graph, bare.dump(), 40.0, ""), std::runtime_error);
    EXPECT_EQ(presetNodeName("cards_mid"), "cardsMid");
    EXPECT_EQ(presetNodeName("wet"), "wet");
}

TEST(Presets, EveryAddOnInsertsIntoEveryBasePreset) {
    // The Add-ons category is made for inserting: each one goes into every other shipped preset
    // (before its Output) and the graph loads, every connection's types fitting.
    const std::vector<PresetInfo> presets = listPresets({kShipped});
    std::vector<PresetInfo> addons, bases;
    for (const PresetInfo& p : presets) (p.category == "Add-ons" ? addons : bases).push_back(p);
    ASSERT_EQ(addons.size(), 6u);
    ASSERT_GT(bases.size(), 10u);
    for (const PresetInfo& addon : addons) {
        const std::string addonText = readPreset(addon.name);
        for (const PresetInfo& base : bases) {
            const std::string graph = presetGraphJson(readPreset(base.name), 20.0);
            std::string inserted, made;
            ASSERT_NO_THROW(inserted = insertPresetJson(graph, addonText, 20.0, "", presetNodeName(addon.name), &made))
                << addon.name << " into " << base.name;
            EXPECT_NO_THROW(loadGraphJson(inserted)) << addon.name << " into " << base.name;
            EXPECT_NO_THROW(GraphEdit(inserted).checkConnections()) << addon.name << " into " << base.name;
            EXPECT_EQ(json::parse(inserted)["nodes"].size(), json::parse(graph)["nodes"].size() + 1) << addon.name << " into " << base.name;
        }
    }
    // The Add-ons category is listed last, after Games.
    const std::vector<std::string>& categories = presetCategories();
    EXPECT_EQ(categories.back(), "Add-ons");
}

TEST(Presets, SearchDirsFollowTheEnvironment) {
#ifdef _WIN32
    _putenv_s("GROOMLAB_PRESET_PATH", "C:/a;C:/b");
    _putenv_s("GROOMLAB_USER_PRESETS", "C:/mine");
#else
    setenv("GROOMLAB_PRESET_PATH", "/a:/b", 1);
    setenv("GROOMLAB_USER_PRESETS", "/mine", 1);
#endif
    const std::vector<std::string> dirs = presetSearchDirs(presetUserDir(), "shipped");
    ASSERT_EQ(dirs.size(), 4u);
#ifdef _WIN32
    EXPECT_EQ(dirs[0], "C:/a");
    EXPECT_EQ(dirs[1], "C:/b");
    EXPECT_EQ(dirs[2], "C:/mine");
    _putenv_s("GROOMLAB_PRESET_PATH", "");
    _putenv_s("GROOMLAB_USER_PRESETS", "");
#else
    EXPECT_EQ(dirs[0], "/a");
    EXPECT_EQ(dirs[2], "/mine");
    unsetenv("GROOMLAB_PRESET_PATH");
    unsetenv("GROOMLAB_USER_PRESETS");
#endif
    EXPECT_EQ(dirs[3], "shipped");
    // Without the variables: the user's Maya folder (when a home is known), then the shipped one.
    const std::vector<std::string> plain = presetSearchDirs(presetUserDir(), "shipped");
    EXPECT_EQ(plain.back(), "shipped");
    if (plain.size() == 2) EXPECT_NE(plain[0].find("groomlab"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Preset looks: every shipped preset evaluated on a reference groom (the strokes fixture's scalp
// and guides, the preset fitted to its size, the maps it requires painted) at a low density,
// against digests recorded in tests/data/golden/PresetLooks.json, so a change that alters what a
// preset makes (a default, an operator's behaviour, the preset itself) is flagged. Re-record an intended change with
// `python tools/dev/record_preset_looks.py` (GROOMLAB_UPDATE_GOLDEN=1 on this test) and review the
// diff.

namespace {

const std::filesystem::path kPresetLooks = std::filesystem::path(GROOMLAB_TEST_DATA) / "golden" / "PresetLooks.json";
constexpr float kPresetLookDensity = 0.05f;

bool updatingGoldens() {
    const char* v = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    return v && *v && std::string(v) != "0";
}

// What a preset made, in a form that survives float noise: counts and IDs exactly, the points'
// sums and bounds, the widths' sum and the channels' names (and each channel's sum).
json presetLook(const StrandSet& s) {
    std::uint64_t ids = hash64(s.strandCount());
    for (const StrandId id : s.ids()) ids = hashCombine(ids, id);
    double sum[3] = {0, 0, 0};
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::size_t points = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f* p = s.strandPoints(i);
        for (std::size_t k = 0; k < s.strandPointCount(i); ++k, ++points) {
            const float xyz[3] = {p[k].x, p[k].y, p[k].z};
            for (int a = 0; a < 3; ++a) {
                sum[a] += xyz[a];
                lo[a] = std::min(lo[a], xyz[a]);
                hi[a] = std::max(hi[a], xyz[a]);
            }
        }
    }
    double widths = 0.0;
    for (const float w : s.widths()) widths += w;
    json channels = json::object();
    for (const std::string& name : s.strandChannelNames()) {
        double total = 0.0;
        for (const float v : *s.strandChannel(name)) total += v;
        channels[name] = total;
    }
    return {{"strands", s.strandCount()},
            {"points", points},
            {"ids", std::to_string(ids)},
            {"sum", {sum[0], sum[1], sum[2]}},
            {"lo", {lo[0], lo[1], lo[2]}},
            {"hi", {hi[0], hi[1], hi[2]}},
            {"widths", widths},
            {"channels", channels}};
}

// Equal within a relative tolerance (float sums in another order, a parallel pass).
void expectNear(const json& got, const json& want, const std::string& what) {
    if (got.is_number() && want.is_number()) {
        const double g = got.get<double>(), w = want.get<double>();
        EXPECT_NEAR(g, w, 1e-4 * std::max(1.0, std::abs(w))) << what;
    } else if (got.is_array() && want.is_array() && got.size() == want.size()) {
        for (std::size_t i = 0; i < got.size(); ++i) expectNear(got[i], want[i], what + "[" + std::to_string(i) + "]");
    } else if (got.is_object() && want.is_object()) {
        EXPECT_EQ(got.size(), want.size()) << what << ": " << got.dump() << " vs " << want.dump();
        for (const auto& [key, value] : want.items()) {
            if (got.contains(key)) expectNear(got.at(key), value, what + "." + key);
            else ADD_FAILURE() << what << ": no " << key;
        }
    } else {
        EXPECT_EQ(got, want) << what;
    }
}

}  // namespace

TEST(PresetLooks, EveryShippedPresetMakesWhatItMadeBefore) {
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const std::optional<double> size = presetScalpSize(*reference.data.scalp);
    json looks = json::object();
    for (const PresetInfo& p : listPresets({kShipped})) {
        std::ifstream in(std::filesystem::u8path(p.path));
        std::stringstream text;
        text << in.rdbuf();
        GroomData data = reference.data;
        data.graphJson = presetGraphJson(text.str(), size);
        test::paintRequiredMaps(text.str(), data);  // the maps a host makes on applying it, painted
        try {
            GroomSession groom(data);
            groom.setPathResolver(searchPathResolver({reference.directory, kShipped}));
            const StrandsPtr made = groom.evaluate(kPresetLookDensity, 0.0);
            looks[p.name] = made ? presetLook(*made) : json{{"error", "nothing made"}};
        } catch (const std::exception& e) {
            looks[p.name] = {{"error", e.what()}};
        }
    }
    ASSERT_GT(looks.size(), 10u);
    if (updatingGoldens()) std::ofstream(kPresetLooks, std::ios::binary) << looks.dump(1) << "\n";
    std::ifstream in(kPresetLooks, std::ios::binary);
    ASSERT_TRUE(in) << "no " << kPresetLooks.generic_string() << " (run python tools/dev/record_preset_looks.py)";
    const json recorded = json::parse(in);
    for (const auto& [name, look] : looks.items()) {
        if (!recorded.contains(name)) {
            ADD_FAILURE() << "preset " << name << " has no recorded look (run python tools/dev/record_preset_looks.py)";
            continue;
        }
        expectNear(look, recorded.at(name), name);
    }
    for (const auto& [name, look] : recorded.items()) {
        EXPECT_TRUE(looks.contains(name)) << "preset " << name << " is recorded but no longer shipped: re-record";
    }
}

// ---- A preset's look as one Compound shows the look's key params ("exposed") -------------------

namespace {

// An operator's param spec (operatorSpecsJson), or null.
const json* operatorParamSpec(const std::string& type, const std::string& param) {
    static const json specs = json::parse(operatorSpecsJson());
    for (const json& op : specs) {
        if (op.value("type", std::string()) != type) continue;
        for (const json& p : op["params"]) {
            if (p.value("name", std::string()) == param) return &p;
        }
    }
    return nullptr;
}

json& mutableNode(json& graph, const std::string& name) {
    for (json& n : graph["nodes"]) {
        if (n.value("name", std::string()) == name) return n;
    }
    throw std::runtime_error("no node " + name);
}

// A value of the param's kind other than `value`, within its limits: a number at 0.7 of it, a
// vector's components too, a switch flipped, a choice the next one.
json editedValue(const json& spec, const json& value) {
    const std::string kind = spec.value("kind", std::string());
    if (kind == "bool") return !value.get<bool>();
    if (kind == "vec3") {
        json out = json::array();
        for (const json& c : value) out.push_back(c.get<double>() * 0.7 + 0.05);
        return out;
    }
    if (kind == "string") {
        for (const json& choice : spec.value("choices", json::array())) {
            if (choice != value) return choice;
        }
        return value;
    }
    double v = value.get<double>() != 0.0 ? value.get<double>() * 0.7 : spec.value("softMax", spec.value("max", 1.0)) * 0.1;
    if (spec.contains("min")) v = std::max(v, spec["min"].get<double>());
    if (spec.contains("max")) v = std::min(v, spec["max"].get<double>());
    if (kind == "int") {
        std::int64_t i = std::llround(v);
        if (i == value.get<std::int64_t>()) i += 1;
        return i;
    }
    return v;
}

// The look the reference groom makes from a graph (presetLook), or the error it gives.
json lookOf(const GroomFile& reference, const std::string& graphJson) {
    GroomData data = reference.data;
    data.graphJson = graphJson;
    try {
        GroomSession groom(data);
        groom.setPathResolver(searchPathResolver({reference.directory, kShipped}));
        const StrandsPtr made = groom.evaluate(kPresetLookDensity, 0.0);
        return made ? presetLook(*made) : json{{"error", "nothing made"}};
    } catch (const std::exception& e) {
        return {{"error", e.what()}};
    }
}

}  // namespace

TEST(Presets, AnInsertedPresetsCompoundExposesItsKeyParams) {
    // Every shipped preset with a look lists its key params ("exposed"); inserted, its Compound
    // shows each (and only those, beside its own definition and repeat) with the inner param's
    // spec (range, unit, group, advanced), set to the inner node's fitted value, in the look's
    // order. A preset without the list has no look to insert (the feathers' designs, card LODs).
    const std::string graph = presetGraphJson(readPreset("hair_basics"), 40.0);
    std::size_t withList = 0;
    for (const PresetInfo& p : listPresets({kShipped})) {
        const std::string text = readPreset(p.name);
        const json preset = json::parse(text);
        if (!preset.contains("exposed")) {
            EXPECT_THROW(insertPresetJson(graph, text, 40.0, ""), std::runtime_error) << p.name << " has a look but lists no exposed params";
            continue;
        }
        ++withList;
        const json& listed = preset["exposed"];
        EXPECT_GE(listed.size(), 1u) << p.name;
        EXPECT_LE(listed.size(), 8u) << p.name;
        std::string made;
        std::string inserted;
        ASSERT_NO_THROW(inserted = insertPresetJson(graph, text, 40.0, "", presetNodeName(p.name), &made)) << p.name;
        const json after = json::parse(inserted);
        const json& compound = nodeNamed(after, made);
        const json& definition = compound["params"]["definition"];
        const json spec = json::parse(nodeSpecJson(compound.dump()));
        std::vector<std::string> shown;
        for (const json& s : spec["params"]) {
            const std::string name = s["name"];
            if (name != "definition" && name != "repeat") shown.push_back(name);
        }
        EXPECT_EQ(shown.size(), listed.size()) << p.name << ": " << spec["params"].dump();
        EXPECT_EQ(definition["expose"].size(), listed.size()) << p.name;
        // The look's order: by inner node, then by the operator's own param order.
        std::vector<std::pair<std::size_t, std::size_t>> places;
        for (const json& e : listed) {
            const std::string name = e["name"], node = e["node"], param = e["param"];
            const std::string what = p.name + " " + name;
            EXPECT_EQ(definition["expose"].value(name, std::string()), node + "." + param) << what;
            const json& inner = nodeNamed(definition["graph"], node);
            const json* innerSpec = operatorParamSpec(inner["type"], param);
            ASSERT_NE(innerSpec, nullptr) << what;
            const json innerValue = inner["params"].contains(param) ? inner["params"][param] : (*innerSpec)["default"];
            EXPECT_EQ(compound["params"].value(name, json()), innerValue) << what << ": the Compound holds the inner (fitted) value";
            const auto at = std::find(shown.begin(), shown.end(), name);
            ASSERT_NE(at, shown.end()) << what;
            const json& exposedSpec = spec["params"][static_cast<std::size_t>(at - shown.begin()) + 2];
            ASSERT_EQ(exposedSpec["name"], name) << what;
            for (const char* key : {"kind", "unit", "min", "max", "softMin", "softMax", "group", "advanced", "animatable", "mappable", "choices",
                                    "worldDirection"}) {
                EXPECT_EQ(exposedSpec.value(key, json()), innerSpec->value(key, json())) << what << " " << key;
            }
            EXPECT_EQ(exposedSpec["default"], innerValue) << what;
            if (innerSpec->contains("showWhen")) {
                // Shown by the exposed param it names, or always when that is not exposed.
                const std::string on = (*innerSpec)["showWhen"]["param"];
                std::string onName;
                for (const json& o : listed) {
                    if (o["node"] == node && o["param"] == on) onName = o["name"];
                }
                if (onName.empty()) EXPECT_FALSE(exposedSpec.contains("showWhen")) << what;
                else EXPECT_EQ(exposedSpec["showWhen"]["param"], onName) << what;
            }
            std::size_t nodeAt = 0, paramAt = 0;
            for (std::size_t i = 0; i < definition["graph"]["nodes"].size(); ++i) {
                if (definition["graph"]["nodes"][i]["name"] == node) nodeAt = i;
            }
            const json opParams = json::parse(nodeSpecJson(json({{"type", inner["type"]}}).dump()))["params"];
            for (std::size_t i = 0; i < opParams.size(); ++i) {
                if (opParams[i]["name"] == param) paramAt = i;
            }
            places.emplace_back(nodeAt, paramAt);
        }
        std::vector<std::string> expectedOrder;
        std::vector<std::size_t> byPlace(listed.size());
        for (std::size_t i = 0; i < byPlace.size(); ++i) byPlace[i] = i;
        std::sort(byPlace.begin(), byPlace.end(), [&](std::size_t a, std::size_t b) { return places[a] < places[b]; });
        for (const std::size_t i : byPlace) expectedOrder.push_back(listed[i]["name"]);
        EXPECT_EQ(shown, expectedOrder) << p.name;
        // Saved and loaded, the Compound keeps them; keyed, it loads.
        const json saved = json::parse(saveGraphJson(loadGraphJson(inserted)));
        const json& kept = nodeNamed(saved, made);
        for (const json& e : listed) EXPECT_TRUE(kept["params"].contains(e["name"].get<std::string>())) << p.name;
        json keyed = after;
        const json* firstSpec = operatorParamSpec(nodeNamed(definition["graph"], listed[0]["node"])["type"], listed[0]["param"]);
        if (firstSpec->value("animatable", true) && (firstSpec->value("kind", std::string()) == "float")) {
            mutableNode(keyed, made)["paramControls"] = {{listed[0]["name"].get<std::string>(), "look"}};
            EXPECT_NO_THROW(loadGraphJson(keyed.dump())) << p.name;
        }
    }
    EXPECT_GE(withList, 20u);
    // Fitted with the look: hair_basics' width (0.007 at 20 units) doubled on a 40-unit scalp.
    std::string made;
    const json basics = json::parse(insertPresetJson(graph, readPreset("hair_basics"), 40.0, "", "basics", &made));
    EXPECT_NEAR(nodeNamed(basics, made)["params"]["width"].get<double>(), 0.014, 1e-9);
}

TEST(Presets, APresetWithoutAnExposedListGetsTheDefaultPick) {
    // One param per look node, Clump, Curl, Noise... HairColor, Width first, then the others in
    // the graph's order: each one's first plain number param; shown in the look's order.
    json preset = json::parse(readPreset("hair_basics"));
    preset.erase("exposed");
    std::string made;
    const json after = json::parse(insertPresetJson(presetGraphJson(readPreset("hair_basics"), 20.0), preset.dump(), 20.0, "", "look", &made));
    const json& compound = nodeNamed(after, made);
    std::vector<std::string> shown;
    const json spec = json::parse(nodeSpecJson(compound.dump()));
    for (const json& s : spec["params"]) shown.push_back(s["name"]);
    EXPECT_EQ(shown, (std::vector<std::string>{"definition", "repeat", "strength", "amplitude", "melanin", "width", "segments"}));
    const json& expose = compound["params"]["definition"]["expose"];
    EXPECT_EQ(expose["strength"], "clump.strength");
    EXPECT_EQ(expose["amplitude"], "noise.amplitude");
    EXPECT_EQ(expose["segments"], "resample.segments");
    EXPECT_DOUBLE_EQ(compound["params"]["strength"].get<double>(), 0.8);
    // A name the Compound has already (an input's, another node's param) takes the node's name.
    json listed = json::parse(readPreset("hair_basics"));
    listed["exposed"] = json::array({{{"node", "clump"}, {"param", "count"}, {"name", "strands"}},
                                     {{"node", "clump"}, {"param", "strength"}},
                                     {{"node", "noise"}, {"param", "fraction"}, {"name", "strength"}},
                                     {{"node", "scatter"}, {"param", "density"}},
                                     {{"node", "clump"}, {"param", "noSuchParam"}}});
    const json named = json::parse(insertPresetJson(presetGraphJson(readPreset("hair_basics"), 20.0), listed.dump(), 20.0, "", "look", &made));
    const json& namedExpose = nodeNamed(named, made)["params"]["definition"]["expose"];
    EXPECT_EQ(namedExpose.size(), 3u) << namedExpose.dump() << ": the Scatter is outside the look; no such param";
    EXPECT_EQ(namedExpose["clumpCount"], "clump.count");
    EXPECT_EQ(namedExpose["strength"], "clump.strength");
    EXPECT_EQ(namedExpose["noiseFraction"], "noise.fraction");
}

TEST(Presets, EditingAnExposedParamEqualsEditingTheInnerNode) {
    // Each shipped preset inserted into hair_basics on the reference groom: every exposed param
    // set on the Compound makes the groom that setting the inner nodes' params does.
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const std::optional<double> size = presetScalpSize(*reference.data.scalp);
    const std::string graph = presetGraphJson(readPreset("hair_basics"), size);
    std::size_t compared = 0, changed = 0;
    for (const PresetInfo& p : listPresets({kShipped})) {
        const std::string text = readPreset(p.name);
        if (!json::parse(text).contains("exposed")) continue;
        std::string made;
        const json inserted = json::parse(insertPresetJson(graph, text, size, "", presetNodeName(p.name), &made));
        const json spec = json::parse(nodeSpecJson(nodeNamed(inserted, made).dump()));
        json onCompound = inserted, onInner = inserted;
        json& outer = mutableNode(onCompound, made)["params"];
        json& innerParams = mutableNode(onInner, made)["params"];
        json& definition = innerParams["definition"];
        for (const json& s : spec["params"]) {
            const std::string name = s["name"];
            if (name == "definition" || name == "repeat") continue;
            const json value = editedValue(s, outer[name]);
            outer[name] = value;
            const std::string target = definition["expose"][name];
            const std::size_t dot = target.find('.');
            mutableNode(definition["graph"], target.substr(0, dot))["params"][target.substr(dot + 1)] = value;
            innerParams.erase(name);  // the Compound no longer sets it: the inner node's own value
        }
        definition["expose"] = json::object();
        const json before = lookOf(reference, inserted.dump());
        if (before.contains("error")) {
            // What the look reads that this groom lacks: painted_maps' map.
            EXPECT_EQ(p.name, "painted_maps") << before.dump();
            continue;
        }
        const json edited = lookOf(reference, onCompound.dump());
        EXPECT_FALSE(edited.contains("error")) << p.name << ": " << edited.dump();
        expectNear(edited, lookOf(reference, onInner.dump()), p.name);
        ++compared;
        if (edited != before) ++changed;
    }
    EXPECT_GE(compared, 20u);
    EXPECT_GE(changed, compared / 2) << "most presets' edits change the groom";
}

}  // namespace groomlab
