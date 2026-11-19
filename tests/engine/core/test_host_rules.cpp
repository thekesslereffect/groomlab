// SPDX-License-Identifier: Apache-2.0
// Rules every host used to keep a copy of (host review, pass 4): the Node panel's sections, the Add
// Node search, Card Atlas names, the Tube card's values and Smooth, guides as curves and back, where
// new root tubes go, Bind Drivers keeping the attached guides.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/card_blocks.h"
#include "groomlab/drive_guides.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/node_catalog.h"
#include "groomlab/param_conditions.h"
#include "groomlab/root_tubes.h"
#include "groomlab/tube.h"

using namespace groomlab;
using nlohmann::json;

TEST(HostRules, TheNodePanelsSectionsAndShownParams) {
    // One group: no headers; two: each its own; advanced (or the Advanced group) last.
    const json one = json::array({{{"name", "a"}, {"group", "Shape"}}, {{"name", "b"}}, {{"name", "c"}, {"advanced", true}}});
    EXPECT_EQ(paramSections(one), (std::vector<std::string>{"", "", "Advanced"}));
    const json two = json::array({{{"name", "a"}, {"group", "Shape"}},
                                  {{"name", "b"}, {"group", "Random"}},
                                  {{"name", "c"}},
                                  {{"name", "d"}, {"group", "Advanced"}}});
    EXPECT_EQ(paramSections(two), (std::vector<std::string>{"Shape", "Random", "", "Advanced"}));
    // showWhen up the chain: count shows with clumpBy "count", and only while clumpBy shows.
    const json params = json::array({{{"name", "mode"}},
                                      {{"name", "clumpBy"}, {"showWhen", {{"param", "mode"}, {"in", {"pick"}}}}},
                                      {{"name", "count"}, {"showWhen", {{"param", "clumpBy"}, {"in", {"count"}}}}}});
    json values = {{"mode", "pick"}, {"clumpBy", "count"}};
    auto valueOf = [&](const std::string& p) { return values.value(p, json()); };
    EXPECT_TRUE(paramShown(params, "count", valueOf));
    values["mode"] = "inputCurves";
    EXPECT_FALSE(paramShown(params, "count", valueOf)) << "clumpBy hidden: count too";
    EXPECT_TRUE(paramShown(params, "mode", valueOf));
    EXPECT_TRUE(paramShown(params, "nope", valueOf));
}

TEST(HostRules, TheAddNodeSearchRanksAsEveryHost) {
    NodeSearchWords curl;
    curl.name = "curl";
    curl.label = "curl";
    curl.keywords = "coil ringlet shape";
    curl.params = "radius radius frequency frequency ";
    curl.about = "shape helical curls around each strand";
    EXPECT_EQ(nodeSearchRank("cu", curl), 0);
    EXPECT_EQ(nodeSearchRank("rl", curl), 1);
    EXPECT_EQ(nodeSearchRank("ringlet", curl), 2);
    EXPECT_EQ(nodeSearchRank("radius", curl), 3);
    EXPECT_EQ(nodeSearchRank("helical", curl), 4);
    EXPECT_EQ(nodeSearchRank("zebra", curl), -1);
    EXPECT_EQ(nodeSearchRank("  ", curl), 5) << "an empty search lists everything";
    const NodeSearchWords words = nodeSearchWords(json{{"type", "Clump"}, {"params", {{{"name", "clumpBy"}}}}}.dump());
    EXPECT_EQ(words.name, "clump");
    EXPECT_NE(words.params.find("clump by"), std::string::npos) << "a param's name as labelled too";
}

TEST(HostRules, CardAtlasNamesAndBandChanges) {
    CardBlockAtlas atlas;
    atlas.blocks.resize(2);
    atlas.blocks[0].name = "block1";
    atlas.blocks[1].name = "block3";
    EXPECT_EQ(newCardBlockName(atlas), "block4") << "from the count on, one no block has";
    atlas.blocks[1].name = "block2";
    EXPECT_EQ(newCardBlockName(atlas), "block3");
    EXPECT_EQ(cardBlockCopyName(atlas, "block1"), "block1_copy");
    atlas.blocks.push_back(atlas.blocks[0]);
    atlas.blocks.back().name = "block1_copy";
    EXPECT_EQ(cardBlockCopyName(atlas, "block1"), "block1_copy2");
    CardBlock a, b;
    b.length = a.length + 3.0f;
    EXPECT_FALSE(cardBlockBandChanged(a, b)) << "a longer card: the same band";
    b.thickness = a.thickness * 2.0f;
    EXPECT_TRUE(cardBlockBandChanged(a, b));
}

TEST(HostRules, TheTubeCardsValuesAndSmooth) {
    const TubeCardValues v = tubeCardValues({0.2f, 0.5f, 0.1f}, {0.5f, 0.3f}, {0.2f, 0.4f});
    EXPECT_FLOAT_EQ(v.width, 0.5f) << "the widest";
    EXPECT_NEAR(v.flatness, 0.6f, 1e-6f) << "1 - the mean aspect";
    EXPECT_NEAR(v.twist, 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(tubeCardValues({}, {}, {}).width, 0.1f);
    EXPECT_FLOAT_EQ(tubeCardValues({}, {-1.0f}, {}).flatness, 0.99f) << "kept to 0.99";
    EXPECT_FLOAT_EQ(tubeAspectOfFlatness(0.995f), 0.01f);
    EXPECT_EQ(json::parse(tubeSmoothEdit("flatness", 400)), (json{{"smooth", "aspect"}, {"passes", 100}}));
    EXPECT_EQ(json::parse(tubeSmoothEdit("aspect", 0)), (json{{"smooth", "aspect"}, {"passes", 1}}));
    EXPECT_THROW(tubeSmoothEdit("radius"), std::invalid_argument);
}

TEST(HostRules, GuidesAsCurvesAndBack) {
    GuideSet guides;
    guides.add(7, {{0, 0, 0}, {0, 1, 0}});
    guides.add(9, {{1, 0, 0}});  // one point: no curve
    const StrandSet curves = guideCurves(guides);
    ASSERT_EQ(curves.strandCount(), 1u);
    EXPECT_EQ(curves.ids()[0], 7u);
    const GuideSet back = guidesOfCurves(curves);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back.guides()[0].id, 7u);
    EXPECT_EQ(back.guides()[0].points.size(), 2u);
    EXPECT_TRUE(primaryGuideCurves(nullptr).strandCount() == 0);
}

TEST(HostRules, BindDriversKeepsTheAttachedGuides) {
    GuideSet drivers;
    drivers.add(1, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    DriverBind before;
    before.attached[42] = {1};
    const DriverBind after = DriverBind::fromJson(reboundDriversJson(drivers, nullptr, 3, before));
    EXPECT_EQ(after.drivers.size(), 1u);
    EXPECT_EQ(after.attached, before.attached);
    EXPECT_EQ(reboundDriversJson(GuideSet{}, nullptr, 0, DriverBind{}), "") << "nothing bound or attached";
}
