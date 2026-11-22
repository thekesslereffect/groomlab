// SPDX-License-Identifier: Apache-2.0
// The groom editor's tool settings (engine/editor tool_settings.h), every host's one parser: set by
// id, kept in range, all or nothing, read back as set, in the host's units and axes.

#include <gtest/gtest.h>

#include <stdexcept>

#include "groomlab/editor/tool_settings.h"

using namespace groomlab;
using nlohmann::json;

TEST(ToolSettings, SetByIdKeptInRangeAndReadBack) {
    BrushToolSettings s;
    applyToolSettings(s, json{{"strength", 0.25}, {"falloff", 0.75}, {"segments", 7}, {"collide", true}, {"symmetry", "x"},
                              {"symmetryCenter", 0.5}, {"paintColor", {1.0, 0.0, 0.0}}, {"paintChannels", "rb"}, {"partName", ""}});
    EXPECT_FLOAT_EQ(s.brush.strength, 0.25f);
    EXPECT_FLOAT_EQ(s.brush.hardness, 0.25f);
    EXPECT_EQ(s.place.segments, 7);
    EXPECT_TRUE(s.brush.collide);
    EXPECT_EQ(s.brush.mirrorAxis, 0);
    EXPECT_FLOAT_EQ(s.brush.mirrorCenter, 0.5f);
    EXPECT_FLOAT_EQ(s.paintColor[3], 1.0f);
    EXPECT_EQ(s.partName, "part");
    const json back = toolSettingsJson(s);
    EXPECT_EQ(back["symmetry"], "x");
    EXPECT_EQ(back["paintChannels"], "rb");
    EXPECT_EQ(brushSettingValue(s, "strength").value(), back["strength"].get<double>());
    // Topology kept when only the center is given; the mode set before its settings.
    setToolSetting(s, "symmetry", "topology");
    setToolSetting(s, "symmetryCenter", 1.0);
    EXPECT_TRUE(s.brush.mirrorTopology);
    // All or nothing: a bad value leaves every setting as it was.
    const float strength = s.brush.strength;
    EXPECT_THROW(applyToolSettings(s, json{{"strength", 0.9}, {"segments", "many"}}), std::invalid_argument);
    EXPECT_FLOAT_EQ(s.brush.strength, strength);
    EXPECT_THROW(setToolSetting(s, "nope", 1), std::invalid_argument);
    EXPECT_THROW(setToolSetting(s, "paintKind", "cloud"), std::invalid_argument);
}

TEST(ToolSettings, AHostsUnitsAndAxes) {
    ToolSpace space;  // a host in centimeters with y and z swapped
    space.lengthIn = [](double v) { return static_cast<float>(v / 100.0); };
    space.lengthOut = [](float v) { return static_cast<double>(v) * 100.0; };
    space.symmetryIn = [](const std::string& axis, double center, int& groomAxis, float& groomCenter) {
        groomAxis = axis == "y" ? 2 : axis == "z" ? 1 : symmetryAxisFromName(axis);
        groomCenter = static_cast<float>(center / 100.0);
    };
    space.symmetryOut = [](int groomAxis, float groomCenter, std::string& axis, double& center) {
        axis = groomAxis == 2 ? "y" : groomAxis == 1 ? "z" : symmetryAxisName(groomAxis);
        center = groomCenter * 100.0;
    };
    BrushToolSettings s;
    applyToolSettings(s, json{{"length", 50.0}, {"symmetry", "y"}, {"symmetryCenter", 10.0}}, space);
    EXPECT_FLOAT_EQ(s.place.length, 0.5f);
    EXPECT_EQ(s.brush.mirrorAxis, 2);
    EXPECT_FLOAT_EQ(s.brush.mirrorCenter, 0.1f);
    const json back = toolSettingsJson(s, space);
    EXPECT_DOUBLE_EQ(back["length"].get<double>(), 50.0);
    EXPECT_EQ(back["symmetry"], "y");
}

TEST(ToolSettings, ADragSetsTheRadiusOrTheStrengthAsEveryHostDoes) {
    BrushToolSettings s;
    EXPECT_FLOAT_EQ(dragToolSetting(s, "radius", 40.0f, 25.0f), 65.0f);
    EXPECT_FLOAT_EQ(dragToolSetting(s, "radius", 40.0f, -100.0f), 5.0f);
    EXPECT_FLOAT_EQ(dragToolSetting(s, "radius", 40.0f, -100.0f, 2.0f), 10.0f) << "5 points at 2 pixels a point";
    EXPECT_FLOAT_EQ(dragToolSetting(s, "strength", 0.5f, 20.0f), 0.7f);
    EXPECT_FLOAT_EQ(s.brush.strength, 0.7f);
    EXPECT_FLOAT_EQ(dragToolSetting(s, "strength", 0.5f, -5000.0f), -10.0f);
    EXPECT_THROW(dragToolSetting(s, "length", 1.0f, 1.0f), std::invalid_argument);
}

TEST(ToolSettings, FreezeSetsPaintAsideAndGivesItBack) {
    BrushToolSettings s;
    FreezeKept kept;
    pickTool(s, "paint", kept);
    s.brush.attribute = "density";
    s.paintKind = "vertices";
    pickTool(s, "freeze", kept);
    EXPECT_EQ(pickedTool(s), "freeze");
    EXPECT_EQ(s.brush.attribute, "freeze");
    pickTool(s, "freeze", kept);  // (again: what was kept stays)
    pickTool(s, "paint", kept);
    EXPECT_EQ(s.brush.attribute, "density");
    EXPECT_EQ(s.paintKind, "vertices");
    EXPECT_EQ(pickedTool(s), "paint");
    EXPECT_THROW(pickTool(s, "nope", kept), std::invalid_argument);
    EXPECT_EQ(pickedTool(s), "paint");
}
