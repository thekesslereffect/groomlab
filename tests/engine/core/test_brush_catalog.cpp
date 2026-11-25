// SPDX-License-Identifier: Apache-2.0
// The brush catalog (brush_catalog.h): one list of tools both hosts build their palettes from,
// complete (every brush mode the stroke tools take, plus Select and Freeze), grouped in the
// agreed order (Create, Shape, Trim, Paint, Guides), each tool described and its settings named.

#include <gtest/gtest.h>

#include <set>

#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/stroke.h"

namespace groomlab {

TEST(BrushCatalog, EveryToolOnceInCategoryOrder) {
    const std::vector<BrushEntry>& catalog = brushCatalog();
    ASSERT_EQ(catalog.size(), 20u);
    std::set<std::string> ids;
    int lastCategory = -1;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const BrushEntry& e = catalog[i];
        EXPECT_TRUE(ids.insert(e.id).second) << e.id << " twice";
        EXPECT_EQ(e.order, static_cast<int>(i));
        EXPECT_FALSE(e.label.empty()) << e.id;
        EXPECT_FALSE(e.icon.empty()) << e.id;
        EXPECT_GT(e.hint.size(), 20u) << e.id << " is described in a line";
        EXPECT_FALSE(e.settings.empty()) << e.id;
        EXPECT_GE(static_cast<int>(e.category), lastCategory) << e.id << ": grouped by category in order";
        lastCategory = static_cast<int>(e.category);
        for (const std::string& s : e.settings) EXPECT_TRUE(brushSettingSpec(s)) << e.id << " names the setting " << s;
        EXPECT_EQ(brushEntry(e.id), &e);
    }
    // The categories in the agreed order, each with tools.
    const std::vector<BrushCategory>& categories = brushCategories();
    ASSERT_EQ(categories.size(), 5u);
    EXPECT_EQ(categories.front(), BrushCategory::Create);
    EXPECT_EQ(categories.back(), BrushCategory::Guides);
    EXPECT_STREQ(brushCategoryName(BrushCategory::Create), "Create");
    for (BrushCategory c : categories) EXPECT_FALSE(brushesInCategory(c).empty()) << brushCategoryName(c);
    EXPECT_EQ(catalog.front().id, "place");
    EXPECT_EQ(brushEntry("shape")->category, BrushCategory::Shape);
    EXPECT_EQ(brushEntry("shape")->label, "Tube") << "not the Shape category's name again";
    EXPECT_EQ(brushEntry("twist")->label, "Swing") << "the Tube brush and the Twist node twist; this swings guides round their roots";
    EXPECT_EQ(brushEntry("tube"), nullptr) << "the Shape brush paints the tubes now";
    EXPECT_TRUE(brushEntry("shape")->hasSetting("tubeChannel"));
    EXPECT_TRUE(brushEntry("shape")->hasSetting("tube"));
    EXPECT_FALSE(brushEntry("shape")->hasSetting("collide")) << "it moves no guide point";
    // Roots: its own Create tool (it makes tubes), next to Place and Draw; Tube only edits them. Both
    // show the tube hierarchy's levels.
    ASSERT_TRUE(brushEntry("roots"));
    EXPECT_EQ(brushEntry("roots")->label, "Roots");
    EXPECT_EQ(brushEntry("roots")->category, BrushCategory::Create);
    EXPECT_EQ(brushEntry("roots")->order, brushEntry("draw")->order + 1);
    EXPECT_EQ(brushEntry("roots")->icon, "roots");
    EXPECT_TRUE(brushEntry("roots")->hasSetting("rootTubes"));
    EXPECT_FALSE(brushEntry("roots")->hasSetting("radius")) << "it clicks CVs: no brush circle";
    EXPECT_FALSE(brushEntry("roots")->corrective);
    EXPECT_FALSE(brushEntry("shape")->hasSetting("rootTubes"));
    for (const char* tool : {"roots", "shape"}) EXPECT_TRUE(brushEntry(tool)->hasSetting("tubeLevels")) << tool;
    EXPECT_EQ(brushEntry("grab"), nullptr) << "Comb does what Grab did";
    EXPECT_EQ(brushEntry("clump"), nullptr) << "the Paint brush paints the clump attribute";
    EXPECT_EQ(brushEntry("density"), nullptr) << "Paint on a setting's map (+ New Map > for a setting)";
    EXPECT_EQ(brushEntry("straighten"), nullptr) << "Smooth's Straighten chip";
    EXPECT_TRUE(brushEntry("smooth")->hasSetting("straighten"));
    EXPECT_TRUE(brushEntry("flow")->hasSetting("symmetry")) << "issue 105: the Flow tool mirrors";
    EXPECT_TRUE(brushEntry("part")->hasSetting("symmetry"));
    EXPECT_EQ(brushEntry("select")->category, BrushCategory::Guides);
    EXPECT_EQ(brushEntry("no such tool"), nullptr);
}

TEST(BrushCatalog, EveryStrokeToolModeIsListedWithItsSettings) {
    // Every mode the stroke tools take is in the catalog, and the hosts' own tools too.
    for (const char* mode : {"comb", "smooth", "lift", "length", "shape", "paint", "cut", "twist", "attract", "erase",
                             "slide", "place", "draw", "clone", "part", "flow", "lengths", "roots"}) {
        const BrushEntry* e = brushEntry(mode);
        ASSERT_TRUE(e) << mode;
        BrushToolSettings s;
        s.brush.attribute = "fur";  // not the clump attribute: Paint on it reads back as "clump"
        EXPECT_NO_THROW(s.setMode(mode)) << mode;
        EXPECT_EQ(s.modeName(), mode) << mode;
    }
    EXPECT_TRUE(brushEntry("select"));
    EXPECT_TRUE(brushEntry("freeze"));
    // Straighten: still a mode (scripts), and what Smooth runs with its chip on.
    BrushToolSettings straighten;
    EXPECT_NO_THROW(straighten.setMode("straighten"));
    BrushSettings smooth;
    smooth.mode = BrushMode::Smooth;
    EXPECT_EQ(strokeMode(smooth), BrushMode::Smooth);
    smooth.straighten = true;
    EXPECT_EQ(strokeMode(smooth), BrushMode::Straighten);
    smooth.mode = BrushMode::Comb;
    EXPECT_EQ(strokeMode(smooth), BrushMode::Comb) << "the chip is Smooth's";
    // The signed tools, their default strengths, and the settings the hosts' cards show.
    for (const char* signedTool : {"lift", "twist", "attract", "length", "shape"}) EXPECT_TRUE(brushEntry(signedTool)->signedStrength) << signedTool;
    EXPECT_FALSE(brushEntry("comb")->signedStrength);
    EXPECT_FLOAT_EQ(brushEntry("smooth")->defaultStrength, 0.5f);
    EXPECT_FLOAT_EQ(brushEntry("comb")->defaultStrength, 1.0f);
    EXPECT_TRUE(brushEntry("comb")->hasSetting("stretch"));
    EXPECT_FALSE(brushEntry("smooth")->hasSetting("stretch"));
    EXPECT_TRUE(brushEntry("erase")->hasSetting("erases"));
    EXPECT_TRUE(brushEntry("place")->hasSetting("newGuides"));
    EXPECT_TRUE(brushEntry("place")->hasSetting("spacing"));
    EXPECT_FALSE(brushEntry("draw")->hasSetting("spacing"));
    EXPECT_FALSE(brushEntry("place")->hasSetting("radius"));
    // Clone (issue 91): a Create tool with the circle (no strength), its source block and switch.
    EXPECT_EQ(brushEntry("clone")->category, BrushCategory::Create);
    EXPECT_TRUE(brushEntry("clone")->hasSetting("radius"));
    EXPECT_FALSE(brushEntry("clone")->hasSetting("strength"));
    EXPECT_TRUE(brushEntry("clone")->hasSetting("clone"));
    BrushToolSettings clone;
    clone.setMode("clone");
    EXPECT_TRUE(clone.showsBrushCircle());
    clone.setMode("place");
    EXPECT_FALSE(clone.showsBrushCircle());
    EXPECT_TRUE(brushEntry("paint")->hasSetting("map"));
    EXPECT_FALSE(brushEntry("paint")->hasSetting("collide"));
    EXPECT_TRUE(brushEntry("comb")->hasSetting("frontFacingOnly"));
    // Falloff is one term, 0 hard to 1 soft (the core's hardness is 1 - falloff).
    const BrushSettingSpec* falloff = brushSettingSpec("falloff");
    ASSERT_TRUE(falloff);
    EXPECT_EQ(falloff->label, "Falloff");
    EXPECT_NE(falloff->hint.find("0 not at all"), std::string::npos);
    EXPECT_NE(falloff->hint.find("1 from the center"), std::string::npos);
    EXPECT_EQ(brushSettingSpec("frontFacingOnly")->label, "Front facing only");
    EXPECT_EQ(brushSettingSpec("no such setting"), nullptr);
}

TEST(BrushCatalog, EveryValuesDefaultIsTheSettingsOwn) {
    // A value's default is written once, in BrushToolSettings: its spec says the same, so a host
    // building a slider from the spec starts where a fresh tool does.
    const BrushToolSettings fresh;
    std::set<std::string> values;
    for (const BrushSettingSpec& s : brushSettingSpecs()) {
        const std::optional<double> own = brushSettingValue(fresh, s.id);
        EXPECT_EQ(own.has_value(), s.isValue()) << s.id << ": a value has a field in the settings, a group none";
        if (!own) continue;
        values.insert(s.id);
        EXPECT_NEAR(*own, s.defaultValue, 1e-6) << s.id;
    }
    EXPECT_EQ(values, (std::set<std::string>{"radius", "strength", "falloff", "smoothStrength", "selectedOnly", "pressure", "collide",
                                             "frontFacingOnly", "stretch", "straighten", "length", "segments", "matchNeighbors",
                                             "spacing", "soloNewGuides", "cloneReplace", "alphaAngle", "alphaSpacing",
                                             "alphaInvert"}));
    EXPECT_EQ(brushSettingValue(fresh, "newGuides"), std::nullopt);
    EXPECT_EQ(brushSettingValue(fresh, "no such setting"), std::nullopt);
}

TEST(BrushCatalog, EveryValuesDefaultIsInsideItsSpanAndLimits) {
    for (const BrushSettingSpec& s : brushSettingSpecs()) {
        if (!s.isValue()) {
            EXPECT_STREQ(brushSettingKindName(s.kind), "") << s.id;
            continue;
        }
        EXPECT_STRNE(brushSettingKindName(s.kind), "") << s.id;
        EXPECT_LE(s.min, s.softMin) << s.id;
        EXPECT_LE(s.softMin, s.defaultValue) << s.id;
        EXPECT_LE(s.defaultValue, s.softMax) << s.id;
        EXPECT_LE(s.softMax, s.max) << s.id;
        EXPECT_LT(s.softMin, s.softMax) << s.id;
        EXPECT_GE(s.decimals, 0) << s.id;
        if (s.kind == BrushSettingKind::Bool) EXPECT_TRUE(s.defaultValue == 0.0 || s.defaultValue == 1.0) << s.id;
        if (s.kind != BrushSettingKind::Float) EXPECT_EQ(s.decimals, 0) << s.id;
        // A length is written for a scalp of kDefaultsScalpSize; no other unit scales with the groom.
        EXPECT_TRUE(s.unit.empty() || s.unit == "pixels" || s.unit == "angle" || s.unit == "fraction" || s.unit == "length") << s.id;
    }
    EXPECT_EQ(brushSettingSpec("length")->unit, "length");
    EXPECT_EQ(brushSettingSpec("spacing")->unit, "length");
    // The limits are what the settings' setters keep: a value at a limit stays, one past it is brought back.
    BrushToolSettings s;
    const auto limits = [](const char* id) { return std::pair<double, double>(brushSettingSpec(id)->min, brushSettingSpec(id)->max); };
    s.setScreenRadius(static_cast<float>(limits("radius").first - 1.0));
    EXPECT_DOUBLE_EQ(*brushSettingValue(s, "radius"), limits("radius").first);
    s.setSegments(static_cast<int>(limits("segments").first) - 1);
    EXPECT_DOUBLE_EQ(*brushSettingValue(s, "segments"), limits("segments").first);
    s.setSmoothStrength(static_cast<float>(limits("smoothStrength").first - 1.0));
    EXPECT_DOUBLE_EQ(*brushSettingValue(s, "smoothStrength"), limits("smoothStrength").first);
    for (const double falloff : {-1.0, 2.0}) {
        s.setHardness(static_cast<float>(1.0 - falloff));
        EXPECT_DOUBLE_EQ(*brushSettingValue(s, "falloff"), falloff < 0.0 ? limits("falloff").first : limits("falloff").second);
    }
    EXPECT_FLOAT_EQ(static_cast<float>(limits("alphaSpacing").first), BrushToolSettings::kMinAlphaSpacing);
    EXPECT_FLOAT_EQ(static_cast<float>(limits("alphaSpacing").second), BrushToolSettings::kMaxAlphaSpacing);
}

TEST(BrushCatalog, TheCorrectivesTabShowsTheToolsThatRecordIntoACorrective) {
    // A corrective keeps point offsets: the shaping brushes and Select work on one; the tools that
    // add or remove things, paint attributes (Paint, Freeze) or paint tubes do not (BrushTool
    // refuses them while a corrective is picked).
    std::set<std::string> on;
    for (const BrushEntry& e : brushCatalog()) {
        if (e.corrective) on.insert(e.id);
    }
    EXPECT_EQ(on, (std::set<std::string>{"comb", "slide", "smooth", "lift", "twist", "attract", "cut", "length", "select"}));
}

}  // namespace groomlab
