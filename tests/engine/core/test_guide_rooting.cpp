// SPDX-License-Identifier: Apache-2.0
// Guides rooted on the scalp (guide_rooting.h) round a synthetic head (a ball of radius 1, its hair
// region the cap above the equator): a chain drawn tip first is turned round, a floating root is
// joined to the scalp, a root far off it is dropped, a pass through the head is cut and a shallow
// dip pushed out, and a guide lying over the head is left as it is.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/curve.h"
#include "groomlab/guide_rooting.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;

ScalpMesh ball(float radius, int rings = 32, int segments = 64, float thetaMax = kPi) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = thetaMax * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            points.push_back(Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)) * radius);
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const int a = r * segments + s, b = r * segments + (s + 1) % segments, c = a + segments, d = b + segments;
            tris.push_back({a, c, d});
            tris.push_back({a, d, b});
        }
    return ScalpMesh(points, tris);
}

// A straight line from a to b in `n` points.
std::vector<Vec3f> line(const Vec3f& a, const Vec3f& b, int n = 12) {
    std::vector<Vec3f> out;
    for (int i = 0; i < n; ++i) out.push_back(a + (b - a) * (static_cast<float>(i) / static_cast<float>(n - 1)));
    return out;
}

TEST(GuideRooting, TheScaleIsTheHeadsRadius) {
    const ScalpMesh head = ball(2.0f), cap = ball(2.0f, 16, 64, 0.5f * kPi);
    const GuideRootingCounts s = guideRootingScale(cap);
    EXPECT_NEAR(s.headRadius, 2.0f, 0.05f);
    EXPECT_NEAR(s.maxGap, 0.4f * s.headRadius, 1e-4f);
    EXPECT_NEAR(s.snapGap, 0.02f * s.headRadius, 1e-4f);
}

TEST(GuideRooting, AGuideDrawnTipFirstIsTurnedRound) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    // Hair hanging down the side: from its tip (far below, off the head) up to just over the scalp.
    const Vec3f nearScalp = Vec3f(0.95f, 0.31f, 0.0f).normalized() * 1.05f;
    std::vector<Vec3f> tipFirst = line(Vec3f(1.6f, -0.9f, 0.0f), nearScalp);
    const GuideRootingCounts scale = guideRootingScale(cap);
    ASSERT_TRUE(guideStartsAtTip(tipFirst, cap, scale));
    const RootedGuide g = rootGuide(tipFirst, cap, &head, {}, scale);
    ASSERT_FALSE(g.points.empty());
    EXPECT_TRUE(g.flipped);
    EXPECT_NEAR(g.points.front().length(), 1.0f, 3e-3f);                         // the root on the scalp
    EXPECT_LT((g.points.back() - Vec3f(1.6f, -0.9f, 0.0f)).length(), 1e-4f);     // the old start is the tip
    EXPECT_FALSE(guideStartsAtTip(g.points, cap, scale));
    // The pass over many: counted.
    std::vector<std::vector<Vec3f>> all{tipFirst, line(nearScalp, Vec3f(1.6f, -0.9f, 0.0f))};
    const GuideRootingCounts c = rootGuides(all, cap, &head);
    EXPECT_EQ(c.flipped, 1);
    EXPECT_EQ(c.dropped, 0);
    ASSERT_EQ(all.size(), 2u);
}

// Hair lying down the side of the head, root first: it starts on the hair's surface over the temple
// and ends at the hair region's edge, near the scalp, lower than it starts: not turned round.
TEST(GuideRooting, HairLyingDownToTheHairlineKeepsItsRoot) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    std::vector<Vec3f> g;
    for (int i = 0; i <= 20; ++i) {
        const float a = (40.0f + 48.0f * static_cast<float>(i) / 20.0f) * kPi / 180.0f;
        const float r = 1.3f - 0.28f * static_cast<float>(i) / 20.0f;  // (thinning toward the hairline)
        g.push_back(Vec3f(std::sin(a), std::cos(a), 0.2f).normalized() * r);
    }
    const GuideRootingCounts scale = guideRootingScale(cap);
    const RootedGuide r = rootGuide(g, cap, &head, {}, scale);
    ASSERT_FALSE(r.points.empty());
    EXPECT_FALSE(r.flipped);
    EXPECT_TRUE(r.connected);
    EXPECT_GT(r.points.front().y, r.points.back().y);
    EXPECT_FALSE(guideStartsAtTip(g, cap, scale));
}

TEST(GuideRooting, AFloatingRootIsJoinedToTheScalpAlongItsNormal) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    const Vec3f n = Vec3f(0.3f, 1.0f, 0.2f).normalized();
    const std::vector<Vec3f> floating = line(n * 1.15f, n * 1.15f + Vec3f(0.0f, -0.2f, -0.8f));
    const RootedGuide g = rootGuide(floating, cap, &head, {}, guideRootingScale(cap));
    ASSERT_FALSE(g.points.empty());
    EXPECT_TRUE(g.connected);
    EXPECT_FALSE(g.flipped);
    EXPECT_NEAR(g.rootGap, 0.15f, 2e-3f);
    EXPECT_NEAR(g.points.front().length(), 1.0f, 3e-3f);
    EXPECT_LT((g.points.front().normalized() - n).length(), 0.01f);   // the nearest scalp point
    EXPECT_GT((g.points[1] - g.points[0]).normalized().dot(n), 0.99f);  // leaving along the normal
    EXPECT_GE(g.root.root.face, 0);
    // A root only a little off: moved onto the scalp, nothing added.
    const std::vector<Vec3f> close = line(n * 1.01f, n * 1.6f + Vec3f(0.0f, -0.2f, -0.8f));
    const RootedGuide s = rootGuide(close, cap, &head, {}, guideRootingScale(cap));
    EXPECT_TRUE(s.snapped);
    EXPECT_FALSE(s.connected);
    EXPECT_EQ(s.points.size(), close.size());
}

TEST(GuideRooting, ALockFarOffTheScalpIsDropped) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    // Hanging beside the face, both ends well below the hair region.
    std::vector<std::vector<Vec3f>> guides{line(Vec3f(0.0f, -0.6f, 1.3f), Vec3f(0.0f, -1.4f, 1.2f))};
    const GuideRootingCounts c = rootGuides(guides, cap, &head);
    EXPECT_EQ(c.droppedGap, 1);
    EXPECT_TRUE(guides.empty());
}

TEST(GuideRooting, AGuideThroughTheHeadIsCutWhereItEnters) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    // From the top up and over, then straight down through the head.
    std::vector<Vec3f> g{Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 1.3f, 0.2f), Vec3f(0.0f, 1.3f, 0.5f)};
    for (int i = 1; i <= 20; ++i) g.push_back(Vec3f(0.0f, 1.3f - 2.6f * static_cast<float>(i) / 20.0f, 0.5f));
    const RootedGuide r = rootGuide(g, cap, &head, {}, guideRootingScale(cap));
    ASSERT_FALSE(r.points.empty());
    EXPECT_TRUE(r.cut);
    for (const Vec3f& p : r.points) EXPECT_GE(p.length(), 0.995f);
    // (it enters at y = sqrt(1 - 0.25))
    EXPECT_NEAR(r.points.back().y, std::sqrt(0.75f), 0.02f);
    EXPECT_LT(arcLength(r.points), arcLength(g));
    // Cut so near its root that little is left: dropped.
    std::vector<std::vector<Vec3f>> shortOne{line(Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, -1.0f, 0.02f))};
    const GuideRootingCounts c = rootGuides(shortOne, cap, &head);
    EXPECT_EQ(c.droppedShort, 1);
}

TEST(GuideRooting, AShallowDipIsPushedOutAndHairOverTheHeadKept) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, 16, 64, 0.5f * kPi);
    // Over the top just above the head, dipping 0.01 into it in the middle.
    std::vector<Vec3f> g;
    for (int i = 0; i <= 30; ++i) {
        const float a = (5.0f + 80.0f * static_cast<float>(i) / 30.0f) * kPi / 180.0f;
        const float r = (i == 0) ? 1.0f : (i >= 14 && i <= 16) ? 0.99f : 1.02f;
        g.push_back(Vec3f(0.0f, std::cos(a), -std::sin(a)) * r);
    }
    const RootedGuide r = rootGuide(g, cap, &head, {}, guideRootingScale(cap));
    ASSERT_FALSE(r.points.empty());
    EXPECT_TRUE(r.pushed);
    EXPECT_FALSE(r.cut);
    EXPECT_FALSE(r.flipped);  // (its tip lies near the scalp too: the root is on it already)
    EXPECT_EQ(r.points.size(), g.size());
    for (const Vec3f& p : r.points) EXPECT_GE(p.length(), 0.995f);
}

}  // namespace
}  // namespace groomlab
