// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "groomlab/control_keys.h"
#include "groomlab/groom_file.h"

using namespace groomlab;

// Keys through the values at their frames, smooth between, never past two keys' values (no
// overshoot: a key held between a rise and a fall stays the top), held outside them.
TEST(ControlKeys, SmoothThroughTheKeysWithoutOvershoot) {
    ControlKeys keys;
    EXPECT_DOUBLE_EQ(controlKeyValue(keys, 5.0), 0.0);
    setControlKey(keys, 10.0, 1.0);
    setControlKey(keys, 1.0, 0.0);
    setControlKey(keys, 20.0, 0.0);
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_DOUBLE_EQ(keys.front().first, 1.0) << "sorted by frame";
    EXPECT_DOUBLE_EQ(controlKeyValue(keys, -5.0), 0.0);
    EXPECT_DOUBLE_EQ(controlKeyValue(keys, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(controlKeyValue(keys, 10.0), 1.0);
    EXPECT_DOUBLE_EQ(controlKeyValue(keys, 30.0), 0.0);
    for (double f = 1.0; f <= 20.0; f += 0.25) {
        const double v = controlKeyValue(keys, f);
        EXPECT_GE(v, 0.0) << f;
        EXPECT_LE(v, 1.0) << f;
    }
    EXPECT_GT(controlKeyValue(keys, 5.5), 0.3);
    EXPECT_LT(controlKeyValue(keys, 5.5), 0.8);
    setControlKey(keys, 10.0, 2.0);
    EXPECT_EQ(keys.size(), 3u) << "the key at a frame replaced";
    EXPECT_TRUE(hasControlKey(keys, 10.0));
    EXPECT_TRUE(removeControlKey(keys, 10.0));
    EXPECT_FALSE(removeControlKey(keys, 10.0));
    EXPECT_FALSE(hasControlKey(keys, 10.0));
}

// The groom file keeps them ("controlKeys": name -> [[frame, value], ...]).
TEST(ControlKeys, TheGroomFileKeepsThem) {
    GroomFile file;
    file.data.graphJson = R"({"nodes": [], "connections": [], "output": ""})";
    setControlKey(file.controlKeys["densityControl"], 1.0, 0.25);
    setControlKey(file.controlKeys["densityControl"], 12.0, 0.75);
    const std::string text = groomFileToJson(file);
    EXPECT_NE(text.find("\"controlKeys\""), std::string::npos);
    const GroomFile back = groomFileFromJson(text);
    ASSERT_EQ(back.controlKeys.count("densityControl"), 1u);
    EXPECT_EQ(back.controlKeys.at("densityControl"), file.controlKeys.at("densityControl"));
}
