// SPDX-License-Identifier: Apache-2.0
// The groom editor's tool overlay (engine/editor tool_overlay.h), every host's: one ring (its
// circles in pixels) and one overlay of colored lines (the ring laid in the world, facing the
// camera, as wide as the brush there).

#include <gtest/gtest.h>

#include "groomlab/editor/tool_overlay.h"
#include "../core/test_helpers.h"

using namespace groomlab;

TEST(ToolOverlay, TheRingHasAHaloAndTheFalloffAndADotForTheToolsThatMakeGuides) {
    BrushToolSettings s;
    s.setMode("comb");
    s.setScreenRadius(40.0f);
    s.setHardness(0.5f);
    const std::vector<RingCircle> ring = brushRing(s);
    ASSERT_EQ(ring.size(), 8u) << "the ring and the falloff ring, each with its halo";
    EXPECT_FLOAT_EQ(ring[2].radius, 40.0f);
    EXPECT_TRUE(ring[0].halo);
    EXPECT_FALSE(ring[2].dashed);
    EXPECT_TRUE(ring[6].dashed) << "the falloff ring";
    EXPECT_FLOAT_EQ(ring[6].radius, 20.0f);
    // Ctrl held: dashed and red.
    const std::vector<RingCircle> reverse = brushRing(s, RingHint::Reverse);
    EXPECT_TRUE(reverse[2].dashed);
    EXPECT_GT(reverse[2].color[0], 0.8f);
    s.setHardness(1.0f);
    EXPECT_EQ(brushRing(s).size(), 4u) << "no falloff ring at full hardness";
    s.setMode("place");
    ASSERT_EQ(brushRing(s).size(), 1u);
    EXPECT_FLOAT_EQ(brushRing(s)[0].radius, 6.0f);
}

TEST(ToolOverlay, TheRingLiesWhereTheHostPutsItAsWideAsTheBrush) {
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.setScreenRadius(50.0f);
    tool.settings.setHardness(1.0f);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, 45.0, 1.0), 400.0, 400.0);
    OverlayRequest request;
    request.x = 200.0f;
    request.y = 200.0f;
    request.offScalp = [](float, float, Vec3f& at) {
        at = Vec3f{0.0f, 0.0f, 0.0f};
        return true;
    };
    SeamCache seam;
    const BrushOverlay drawn = brushOverlay(tool, nullptr, view, request, seam);
    ASSERT_TRUE(drawn.ring);
    EXPECT_FALSE(drawn.ringOnScalp);
    EXPECT_NEAR(drawn.ringRadius, worldRadiusAt(view, {0.0f, 0.0f, 0.0f}, 200.0f, 200.0f, 50.0f), 1e-5f);
    EXPECT_FALSE(drawn.lines.positions.empty());
    EXPECT_TRUE(drawn.lines.overlay);
    // Nowhere to put it: no ring.
    request.offScalp = nullptr;
    EXPECT_FALSE(brushOverlay(tool, nullptr, view, request, seam).ring);
}
