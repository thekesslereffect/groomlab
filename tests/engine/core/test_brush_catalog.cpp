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
    ASSERT_EQ(catalog.size(), 22u);
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
    EXPECT_EQ(brushEntry("shape")->label, "Shape");
    EXPECT_EQ(brushEntry("tube"), nullptr) << "the Shape brush paints the tubes now";
    EXPECT_TRUE(brushEntry("shape")->hasSetting("tubeChannel"));
    EXPECT_TRUE(brushEntry("shape")->hasSetting("tube"));
    EXPECT_FALSE(brushEntry("shape")->hasSetting("collide")) << "it moves no guide point";
    EXPECT_EQ(brushEntry("grab"), nullptr) << "Comb does what Grab did";
    EXPECT_EQ(brushEntry("clump"), nullptr) << "the Paint brush paints the clump attribute";
    EXPECT_EQ(brushEntry("density"), nullptr) << "Paint on a setting's map (+ New Map > for a setting)";
    EXPECT_EQ(brushEntry("straighten")->label, "Straighten");
    EXPECT_EQ(brushEntry("select")->category, BrushCategory::Guides);
    EXPECT_EQ(brushEntry("no such tool"), nullptr);
}

TEST(BrushCatalog, EveryStrokeToolModeIsListedWithItsSettings) {
    // Every mode the stroke tools take is in the catalog, and the hosts' own tools too.
    for (const char* mode : {"comb", "smooth", "lift", "length", "shape", "paint", "cut", "twist", "curl", "straighten",
                             "attract", "erase", "slide", "direction", "noise", "place", "draw", "part",
                             "flow", "lengths"}) {
        const BrushEntry* e = brushEntry(mode);
        ASSERT_TRUE(e) << mode;
        BrushToolSettings s;
        s.brush.attribute = "fur";  // not the clump attribute: Paint on it reads back as "clump"
        EXPECT_NO_THROW(s.setMode(mode)) << mode;
        EXPECT_EQ(s.modeName(), mode) << mode;
    }
    EXPECT_TRUE(brushEntry("select"));
    EXPECT_TRUE(brushEntry("freeze"));
    // The signed tools, their default strengths, and the settings the hosts' cards show.
    for (const char* signedTool : {"lift", "twist", "curl", "attract", "length", "shape"}) EXPECT_TRUE(brushEntry(signedTool)->signedStrength) << signedTool;
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

}  // namespace groomlab
