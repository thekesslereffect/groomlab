// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/random.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"

TEST(Brush, FalloffHardness) {
    using namespace groomlab;
    // Soft: full only at the center, zero at the radius, smooth between.
    EXPECT_FLOAT_EQ(brushFalloff(0.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(brushFalloff(1.0f, 1.0f), 0.0f);
    EXPECT_NEAR(brushFalloff(0.5f, 1.0f), 0.5625f, 1e-6f);
    // Hardness 0.5: full weight out to half the radius, then the same falloff squeezed into the rest.
    EXPECT_FLOAT_EQ(brushFalloff(0.5f, 1.0f, 0.5f), 1.0f);
    EXPECT_NEAR(brushFalloff(0.75f, 1.0f, 0.5f), 0.5625f, 1e-6f);
    EXPECT_FLOAT_EQ(brushFalloff(1.0f, 1.0f, 0.5f), 0.0f);
    // Hard: full weight to the edge.
    EXPECT_FLOAT_EQ(brushFalloff(0.99f, 1.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(brushFalloff(1.0f, 1.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(sphereWeight({0, 0, 0}, 2.0f, 0.5f)({1.0f, 0.0f, 0.0f}), 1.0f);
}

namespace groomlab {
namespace {

float totalLength(const std::vector<Vec3f>& pts) { return arcLength(pts.data(), pts.size()); }

// One straight primary at (x, 0, z), 1 unit tall, on the unit plane.
struct Scene {
    std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    GroupId primary = kNoGroup;

    GuideId add(float x, float z) {
        if (primary == kNoGroup) primary = h.addGroup("primary");
        const GuideId id = h.addGuide(primary, test::straightCurve({x, 0, z}, {0, 1, 0}, 8));
        h.evaluate(scalp.get());
        return id;
    }
    const std::vector<Vec3f>& pts(GuideId id) const { return h.guide(id)->points; }
};

// ---------------------------------------------------------------------------
// Ray casts

TEST(Ray, HitsThePlaneFromEitherSide) {
    const auto plane = test::makePlane(8);
    SurfacePoint hit;
    float t = 0;
    ASSERT_TRUE(plane->intersectRay({0.3f, 2.0f, 0.6f}, {0, -1, 0}, &hit, &t));
    EXPECT_NEAR(t, 2.0f, 1e-5f);
    EXPECT_NEAR(hit.position.x, 0.3f, 1e-5f);
    EXPECT_NEAR(hit.position.z, 0.6f, 1e-5f);
    EXPECT_NEAR((plane->position(hit.root) - hit.position).length(), 0.0f, 1e-5f);

    EXPECT_TRUE(plane->intersectRay({0.3f, -1.0f, 0.6f}, {0, 1, 0}, &hit));   // from below
    EXPECT_FALSE(plane->intersectRay({0.3f, 1.0f, 0.6f}, {0, 1, 0}, &hit));   // pointing away
    EXPECT_FALSE(plane->intersectRay({3.0f, 1.0f, 3.0f}, {0, -1, 0}, &hit));  // outside the square
}

TEST(Ray, TakesTheNearestHitOnASphere) {
    const auto sphere = test::makeSphere(30, 60, 1.0f);
    SurfacePoint hit;
    float t = 0;
    ASSERT_TRUE(sphere->intersectRay({0, 0, 5}, {0, 0, -1}, &hit, &t));
    EXPECT_NEAR(t, 4.0f, 0.01f);
    EXPECT_NEAR(hit.position.z, 1.0f, 0.01f);
    EXPECT_GT(hit.normal.z, 0.99f);
}

// ---------------------------------------------------------------------------
// Brushes

TEST(Brush, SymmetrySeamIsTheLineDownTheScalpsMiddle) {
    // The line hosts draw while symmetry is on: where x = 0 cuts the sphere (a great circle of
    // radius 1, lifted off it), nothing with symmetry off or a plane past the scalp, and the
    // topology's middle loop (vertices that are their own partner) on a sphere whose seam has vertices.
    const auto sphere = test::makeSphere(12, 24);
    BrushSettings b;
    EXPECT_TRUE(symmetrySeam(*sphere, StrokeSymmetry::of(b, sphere.get()), 0.01f).empty());
    b.mirrorAxis = 0;
    const std::vector<Vec3f> seam = symmetrySeam(*sphere, StrokeSymmetry::of(b, sphere.get()), 0.01f);
    ASSERT_GE(seam.size(), 40u);
    ASSERT_EQ(seam.size() % 2, 0u);
    float length = 0.0f;
    for (std::size_t i = 0; i < seam.size(); ++i) {
        EXPECT_NEAR(seam[i].x, 0.0f, 0.011f);
        EXPECT_GT(seam[i].length(), 0.99f);  // lifted off the surface (the faces lie inside the sphere)
        EXPECT_LT(seam[i].length(), 1.011f);
        if (i % 2) length += (seam[i] - seam[i - 1]).length();
    }
    EXPECT_NEAR(length, 2.0f * 3.14159265f, 0.1f);  // the whole great circle, once
    b.mirrorCenter = 5.0f;
    EXPECT_TRUE(symmetrySeam(*sphere, StrokeSymmetry::of(b, sphere.get()), 0.01f).empty());
    BrushSettings topology;
    topology.mirrorTopology = true;
    const std::vector<Vec3f> loop = symmetrySeam(*sphere, StrokeSymmetry::of(topology, sphere.get()), 0.0f);
    ASSERT_GE(loop.size(), 20u);
    for (const Vec3f& p : loop) EXPECT_NEAR(p.x, 0.0f, 1e-4f);
}

TEST(Brush, CombPullsTipsAndKeepsLengthAndRoot) {
    Scene s;
    const GuideId id = s.add(0.5f, 0.5f);
    const Vec3f root = s.pts(id).front();
    const float length = totalLength(s.pts(id));

    BrushSettings b;
    b.radius = 0.6f;
    const std::size_t changed = applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.8f, 0.5f}, {0.9f, 0.8f, 0.5f}});
    EXPECT_EQ(changed, 1u);
    EXPECT_GT(s.pts(id).back().x, 0.7f);  // combed toward +X
    EXPECT_NEAR((s.pts(id).front() - root).length(), 0.0f, 1e-5f);
    EXPECT_NEAR(totalLength(s.pts(id)), length, 1e-4f);
}

TEST(Brush, CombWithStretchChangesLength) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const GuideId id = h.addGuide(g, test::straightCurve({0, 0, 0}, {0, 1, 0}, 10));
    h.evaluate(nullptr);
    BrushSettings s;
    s.mode = BrushMode::Comb;
    s.radius = 0.3f;
    s.strength = 1.0f;
    s.stretch = true;
    applyBrushStroke(h, nullptr, s, {{0, 1.0f, 0}, {0, 1.2f, 0}, {0, 1.4f, 0}});  // combed out past the tip
    const auto& pts = h.guide(id)->points;
    EXPECT_GT(arcLength(pts.data(), pts.size()), 1.1f);
    EXPECT_EQ(pts.size(), 11u);
    const float seg = (pts[1] - pts[0]).length();
    for (std::size_t i = 1; i < pts.size(); ++i) EXPECT_NEAR((pts[i] - pts[i - 1]).length(), seg, 1e-3f);
    s.stretch = false;  // off: the length stays
    const float before = arcLength(pts.data(), pts.size());
    applyBrushStroke(h, nullptr, s, {{0.2f, 1.2f, 0}, {0.5f, 1.2f, 0}});
    EXPECT_NEAR(arcLength(h.guide(id)->points.data(), h.guide(id)->points.size()), before, 1e-3f);
}

// The Shape brush (ShapeTube): each tube section moves by the brush's weight at its guide point,
// the painted list resampled to one value per point first; the other lists stay as they were.
TEST(Brush, ShapePaintsWidthPerPointWhereBrushed) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const GuideId id = h.addGuide(g, test::straightCurve({0, 0, 0}, {0, 2, 0}, 16));
    h.evaluate(nullptr);
    BrushSettings s;
    s.mode = BrushMode::ShapeTube;
    s.tubeChannel = TubeChannel::Width;
    s.radius = 0.4f;
    s.strength = 1.0f;
    for (int i = 0; i < 5; ++i) applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});  // near the tip
    const TubeProfile& tube = h.guide(id)->tube;
    ASSERT_EQ(tube.width.size(), 17u);              // one section per guide point
    EXPECT_GT(tube.width.back(), 0.12f);            // the tip end grew
    EXPECT_FLOAT_EQ(tube.width.front(), 0.1f);      // the root end didn't
    EXPECT_FLOAT_EQ(tube.width[8], 0.1f);           // nor the middle, out of the brush
    EXPECT_EQ(tube.aspect, std::vector<float>{1.0f});  // the other lists untouched
    EXPECT_EQ(tube.twist, std::vector<float>{0.0f});
    // Ctrl (negative strength): it shrinks again where brushed.
    const float grown = tube.width.back();
    s.strength = -1.0f;
    applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});
    EXPECT_LT(h.guide(id)->tube.width.back(), grown);
}

TEST(Brush, ShapePaintsFlatnessAndTwist) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const GuideId id = h.addGuide(g, test::straightCurve({0, 0, 0}, {0, 2, 0}, 16));
    h.evaluate(nullptr);
    BrushSettings s;
    s.mode = BrushMode::ShapeTube;
    s.radius = 0.4f;
    s.strength = 1.0f;
    // Flatness: toward a flat ribbon near the tip; Ctrl rounds it back toward 1.
    s.tubeChannel = TubeChannel::Flatness;
    for (int i = 0; i < 10; ++i) applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});
    const TubeProfile flat = h.guide(id)->tube;
    ASSERT_EQ(flat.aspect.size(), 17u);
    EXPECT_LT(flat.aspect.back(), 0.6f);
    EXPECT_GE(flat.aspect.back(), kMinTubeAspect);
    EXPECT_FLOAT_EQ(flat.aspect.front(), 1.0f);
    EXPECT_EQ(flat.width, (std::vector<float>{0.1f, 0.1f}));  // width as it was
    s.strength = -1.0f;
    applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});
    EXPECT_GT(h.guide(id)->tube.aspect.back(), flat.aspect.back());
    EXPECT_LE(h.guide(id)->tube.aspect.back(), 1.0f);
    // Twist: the sections near the cursor turn one way, Ctrl the other; the root's stays.
    s.tubeChannel = TubeChannel::Twist;
    s.strength = 1.0f;
    for (int i = 0; i < 3; ++i) applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});
    const TubeProfile turned = h.guide(id)->tube;
    ASSERT_EQ(turned.twist.size(), 17u);
    EXPECT_GT(turned.twist.back(), 0.1f);
    EXPECT_FLOAT_EQ(turned.twist.front(), 0.0f);
    s.strength = -1.0f;
    applyBrushStroke(h, nullptr, s, {{0, 1.9f, 0}});
    EXPECT_LT(h.guide(id)->tube.twist.back(), turned.twist.back());
    // Names round trip; an unknown one is refused.
    for (const TubeChannel c : {TubeChannel::Width, TubeChannel::Flatness, TubeChannel::Twist}) {
        EXPECT_EQ(tubeChannelFromString(tubeChannelName(c)), c);
    }
    EXPECT_THROW(tubeChannelFromString("radius"), std::invalid_argument);
}

// Shift+drag with the Shape brush smooths the channel it paints along the guides under it (the
// guide's shape and the other channels untouched); every other brush's Shift is Smooth, as ever.
TEST(Brush, ShapeShiftSmoothsItsChannel) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const GuideId id = h.addGuide(g, test::straightCurve({0, 0, 0}, {0, 2, 0}, 16));
    h.evaluate(nullptr);
    TubeProfile jagged;
    jagged.width.resize(17);
    for (std::size_t i = 0; i < 17; ++i) jagged.width[i] = i % 2 ? 0.2f : 0.1f;
    jagged.twist = {0.0f, 1.0f};
    h.setTube(id, jagged);
    const std::vector<Vec3f> shape = h.guide(id)->points;
    BrushSettings s;
    s.mode = BrushMode::ShapeTube;
    s.tubeChannel = TubeChannel::Width;
    s.radius = 0.6f;
    s.strength = 1.0f;
    useShiftSmoothing(s);
    EXPECT_EQ(s.mode, BrushMode::ShapeTube);
    EXPECT_TRUE(s.smoothTube);
    for (int i = 0; i < 4; ++i) applyBrushStroke(h, nullptr, s, {{0, 1.0f, 0}});  // over the middle
    const TubeProfile& smoothed = h.guide(id)->tube;
    ASSERT_EQ(smoothed.width.size(), 17u);
    auto jump = [&](std::size_t i) { return std::abs(smoothed.width[i + 1] - smoothed.width[i]); };
    EXPECT_LT(jump(7) + jump(8), 0.1f);            // evened out under the brush (was 0.2)
    EXPECT_NEAR(jump(0), 0.1f, 1e-6f);              // out of reach: as it was
    EXPECT_EQ(smoothed.twist, jagged.twist);        // the other channels untouched
    EXPECT_EQ(h.guide(id)->points, shape);          // and the guide
    endShiftSmoothing(s, BrushMode::ShapeTube);
    EXPECT_FALSE(s.smoothTube);
    // Comb's Shift: Smooth, as before.
    BrushSettings comb;
    comb.mode = BrushMode::Comb;
    useShiftSmoothing(comb);
    EXPECT_EQ(comb.mode, BrushMode::Smooth);
    EXPECT_FALSE(comb.smoothTube);
    endShiftSmoothing(comb, BrushMode::Comb);
    EXPECT_EQ(comb.mode, BrushMode::Comb);
}

// A guide placed (or drawn) among tube guides takes its tube from them: the nearest's shape, the
// two nearest's sections blended by inverse distance at its own points.
// A new guide's tube: the placement's shape (the host's choice: Groom Tools' Tubes panel), its
// width, flatness and twist blended from the two nearest guides.
TEST(Brush, NewGuidesInheritTheNeighboursTubes) {
    Scene s;
    const GuideId a = s.add(0.3f, 0.5f), b = s.add(0.7f, 0.5f);
    TubeProfile blade;
    blade.shape = TubeShape::Blade;
    blade.width = {0.2f, 0.1f};
    blade.aspect = {0.5f};
    TubeProfile ribbon;
    ribbon.shape = TubeShape::Ribbon;
    ribbon.width = {0.4f};
    ribbon.twist = {0.0f, 1.0f};
    s.h.setTube(a, blade);
    s.h.setTube(b, ribbon);
    s.h.evaluate(s.scalp.get());
    PlaceSettings p;
    p.tube.shape = TubeShape::Teardrop;
    const GuideId placed = placeGuide(s.h, *s.scalp, p, s.scalp->closestPoint({0.4f, 0, 0.5f}));  // 0.1 from a, 0.3 from b
    const TubeProfile& t = s.h.guide(placed)->tube;
    EXPECT_EQ(t.shape, TubeShape::Teardrop);  // the placement's shape, not the nearest's
    const float wa = 1.0f / 0.1f, wb = 1.0f / 0.3f;
    EXPECT_NEAR(t.widthAt(0.0f), (0.2f * wa + 0.4f * wb) / (wa + wb), 1e-3f);
    EXPECT_NEAR(t.widthAt(1.0f), (0.1f * wa + 0.4f * wb) / (wa + wb), 1e-3f);
    EXPECT_NEAR(t.aspectAt(0.5f), (0.5f * wa + 1.0f * wb) / (wa + wb), 1e-3f);
    EXPECT_NEAR(t.twistAt(1.0f), (1.0f * wb) / (wa + wb), 1e-3f);
    EXPECT_LE(t.width.size(), 2u);  // a line root to tip, said as such
    // Drawn guides too; not matching the neighbours, the brush's own tube.
    const GuideId drawn = drawGuide(s.h, *s.scalp, p, {{0.65f, 0, 0.5f}, {0.65f, 0.5f, 0.5f}, {0.65f, 1.0f, 0.5f}});
    EXPECT_EQ(s.h.guide(drawn)->tube.shape, TubeShape::Teardrop);
    EXPECT_NEAR(s.h.guide(drawn)->tube.widthAt(0.5f), 0.4f, 0.05f);  // mostly the ribbon's, the nearer
    p.group = "other";
    p.matchNeighbors = false;
    const GuideId bare = placeGuide(s.h, *s.scalp, p, s.scalp->closestPoint({0.5f, 0, 0.2f}));
    TubeProfile own = p.tube;
    own.roll = s.h.guide(bare)->tube.roll;  // (worked out on the scalp, not compared)
    EXPECT_EQ(s.h.guide(bare)->tube, own);
}

TEST(Brush, ShapeMirrorsAndLeavesFrozenGuidesAlone) {
    // Symmetry paints the mirror guide too; a frozen guide (the freeze mask) is held back, as with
    // every brush.
    Scene s;
    const GuideId left = s.add(0.25f, 0.75f), right = s.add(0.75f, 0.75f), frozen = s.add(0.25f, 0.25f);
    s.h.setAttribute(frozen, "freeze", 1.0f);
    BrushSettings b;
    b.mode = BrushMode::ShapeTube;
    b.tubeChannel = TubeChannel::Width;
    b.radius = 0.2f;
    b.strength = 1.0f;
    b.mirrorAxis = 0;
    b.mirrorCenter = 0.5f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.25f, 0.9f, 0.75f}, {0.25f, 0.9f, 0.25f}});
    EXPECT_GT(s.h.guide(left)->tube.width.back(), 0.1f);
    EXPECT_GT(s.h.guide(right)->tube.width.back(), 0.1f);  // the mirror image's stamp
    EXPECT_NEAR(s.h.guide(left)->tube.width.back(), s.h.guide(right)->tube.width.back(), 1e-5f);
    EXPECT_EQ(s.h.guide(frozen)->tube.width, (std::vector<float>{0.1f, 0.1f}));
}

TEST(Brush, TwistTurnsTheMirrorGuideTheOtherWay) {
    // A turn about the guide is handed: with symmetry, the mirror guide turns the opposite way,
    // so the two stay mirror images (the Shape brush's twist channel too).
    Scene s;
    s.primary = s.h.addGroup("primary");
    const GuideId left = s.h.addGuide(s.primary, test::straightCurve({0.25f, 0, 0.5f}, {-0.4f, 1, 0.3f}, 8));
    const GuideId right = s.h.addGuide(s.primary, test::straightCurve({0.75f, 0, 0.5f}, {0.4f, 1, 0.3f}, 8));
    s.h.evaluate(s.scalp.get());
    BrushSettings b;
    b.mode = BrushMode::Twist;
    b.radius = 0.3f;
    b.strength = 1.0f;
    b.mirrorAxis = 0;
    b.mirrorCenter = 0.5f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.2f, 0.6f, 0.6f}, {0.2f, 0.6f, 0.4f}});
    const Vec3f l = s.pts(left).back(), r = s.pts(right).back();
    EXPECT_GT((l - test::straightCurve({0.25f, 0, 0.5f}, {-0.4f, 1, 0.3f}, 8).back()).length(), 1e-3f);  // it turned
    EXPECT_NEAR(r.x, 1.0f - l.x, 1e-4f);
    EXPECT_NEAR(r.y, l.y, 1e-4f);
    EXPECT_NEAR(r.z, l.z, 1e-4f);

    b.mode = BrushMode::ShapeTube;
    b.tubeChannel = TubeChannel::Twist;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.2f, 0.6f, 0.6f}, {0.2f, 0.6f, 0.4f}});
    const std::vector<float>& lt = s.h.guide(left)->tube.twist;
    const std::vector<float>& rt = s.h.guide(right)->tube.twist;
    ASSERT_EQ(lt.size(), rt.size());
    float most = 0.0f;
    for (std::size_t i = 0; i < lt.size(); ++i) {
        most = std::max(most, std::fabs(lt[i]));
        EXPECT_NEAR(rt[i], -lt[i], 1e-5f) << "section " << i;
    }
    EXPECT_GT(most, 1e-4f);  // it twisted
}

TEST(Brush, NothingOutsideTheRadiusChanges) {
    Scene s;
    const GuideId id = s.add(0.1f, 0.1f);
    const auto before = s.pts(id);
    BrushSettings b;
    b.radius = 0.2f;
    EXPECT_EQ(applyBrushStroke(s.h, s.scalp.get(), b, {{0.9f, 0.5f, 0.9f}, {0.9f, 0.5f, 0.6f}}), 0u);
    EXPECT_EQ(s.pts(id), before);
}

TEST(Brush, LevelsAndGroupsChooseWhatIsEdited) {
    Scene s;
    const GuideId parent = s.add(0.5f, 0.5f);
    const GroupId kids = s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId child = s.h.guidesInGroup(kids)[0];
    const Vec3f childTip = s.pts(child).back();

    // Level 0: the parent is combed and its children follow.
    BrushSettings b;
    b.radius = 1.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.8f, 0.5f}, {0.8f, 0.8f, 0.5f}});
    EXPECT_GT(s.pts(child).back().x, childTip.x + 0.1f);

    // Group "children" only: the parent stays, the children move and still follow it.
    const auto parentBefore = s.pts(parent);
    const Vec3f tipBefore = s.pts(child).back();
    b.group = "children";
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.6f, 0.8f, 0.5f}, {0.6f, 0.8f, 0.9f}});
    EXPECT_EQ(s.pts(parent), parentBefore);
    EXPECT_GT(s.pts(child).back().z, tipBefore.z + 0.05f);
    EXPECT_FALSE(s.h.guide(child)->locked);
    EXPECT_FALSE(s.h.guide(child)->offsets.empty());
}

TEST(Brush, HiddenGroupsAreLeftAlone) {
    GuideHierarchy h;
    const GroupId a = h.addGroup("a"), b = h.addGroup("b");
    const GuideId ga = h.addGuide(a, test::straightCurve({0, 0, 0}, {0, 1, 0}, 4));
    const GuideId gb = h.addGuide(b, test::straightCurve({0.05f, 0, 0}, {0, 1, 0}, 4));
    h.evaluate(nullptr);
    const auto beforeA = h.guide(ga)->points, beforeB = h.guide(gb)->points;
    BrushSettings s;
    s.mode = BrushMode::Comb;
    s.radius = 2.0f;
    s.level = -1;
    s.hiddenGroups = {"b"};
    applyBrushStroke(h, nullptr, s, {{0, 0.8f, 0}, {0.5f, 0.8f, 0}});
    EXPECT_NE(h.guide(ga)->points, beforeA);
    EXPECT_EQ(h.guide(gb)->points, beforeB);  // hidden: untouched
}

TEST(Brush, PropagateReachesLockedDescendants) {
    Scene s;
    s.add(0.5f, 0.5f);
    const GroupId kids = s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId locked = s.h.guidesInGroup(kids)[0];
    s.h.lock(locked);
    s.h.evaluate(s.scalp.get());
    const auto before = s.pts(locked);

    BrushSettings b;
    b.radius = 1.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.8f, 0.5f}, {0.8f, 0.8f, 0.5f}});
    EXPECT_NEAR((s.pts(locked).back() - before.back()).length(), 0.0f, 1e-5f);  // locked: ignores its parent

    b.propagate = true;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.8f, 0.5f}, {0.8f, 0.8f, 0.5f}});
    EXPECT_GT(s.pts(locked).back().x, before.back().x + 0.1f);
}

TEST(Brush, PropagateEditsEachGuideOnceAndLeavesHiddenAndFrozenAlone) {
    Scene s;
    const GuideId parent = s.add(0.5f, 0.5f);
    ChildGenerationParams params;
    params.count = 3;
    const GroupId kids = s.h.generateChildren(s.primary, "children", params);
    s.h.evaluate(s.scalp.get());
    const std::vector<GuideId> children = s.h.guidesInGroup(kids);
    ASSERT_EQ(children.size(), 3u);
    for (const GuideId id : children) s.h.lock(id);
    s.h.setAttribute(children[2], "freeze", 1.0f);  // held back
    s.h.evaluate(s.scalp.get());
    auto lengths = [&] {
        std::vector<float> out;
        for (const GuideId id : children) out.push_back(totalLength(s.pts(id)));
        return out;
    };
    const float parentBefore = totalLength(s.pts(parent));
    const std::vector<float> before = lengths();

    // Every level, and propagate: the locked children are targets already; each grows by one
    // stamp's worth (a hard brush: weight 1), not twice.
    BrushSettings b;
    b.mode = BrushMode::Length;
    b.radius = 5.0f;
    b.hardness = 1.0f;
    b.level = -1;
    b.propagate = true;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    EXPECT_NEAR(totalLength(s.pts(parent)) / parentBefore, 1.05f, 1e-3f);
    std::vector<float> after = lengths();
    EXPECT_NEAR(after[0] / before[0], 1.05f, 1e-3f);
    EXPECT_NEAR(after[1] / before[1], 1.05f, 1e-3f);
    EXPECT_FLOAT_EQ(after[2], before[2]);  // frozen: propagate does not reach it either

    // Primaries only: propagate reaches the locked children, but not hidden ones.
    b.level = 0;
    b.hiddenGroups = {"children"};
    const std::vector<float> shown = lengths();
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    EXPECT_EQ(lengths(), shown);
    b.hiddenGroups.clear();
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    after = lengths();
    EXPECT_NEAR(after[0] / shown[0], 1.05f, 1e-3f);
    EXPECT_FLOAT_EQ(after[2], shown[2]);
}

TEST(Brush, PlaceAndDrawIntoAChildGroupKeepTheTube) {
    Scene s;
    const GuideId parent = s.add(0.5f, 0.5f);
    TubeProfile wide;
    wide.width = {0.4f, 0.2f};
    s.h.setTube(parent, wide);
    const GroupId kids = s.h.addGroup("kids", s.primary);
    PlaceSettings p;
    p.group = "kids";
    p.matchNeighbors = false;
    p.tube.width = {0.05f, 0.02f};
    const GuideId placed = placeGuide(s.h, *s.scalp, p, s.scalp->closestPoint({0.55f, 0, 0.5f}));
    EXPECT_EQ(s.h.guide(placed)->parent, parent);
    EXPECT_EQ(s.h.guide(placed)->tube.width, p.tube.width);
    const GuideId drawn = drawGuide(s.h, *s.scalp, p, {{0.45f, 0, 0.5f}, {0.45f, 0.5f, 0.5f}});
    EXPECT_EQ(s.h.guide(drawn)->tube.width, p.tube.width);
    // Placed among children: the nearest child's tube.
    p.matchNeighbors = true;
    p.tube.width = {0.3f};
    const GuideId matched = placeGuide(s.h, *s.scalp, p, s.scalp->closestPoint({0.5f, 0, 0.55f}));
    EXPECT_EQ(s.h.guide(matched)->tube.width, (std::vector<float>{0.05f, 0.02f}));
    // By hand without a tube (or with no radii): the parent's, scaled by 0.35.
    const std::vector<Vec3f> shape = test::straightCurve({0.5f, 0, 0.6f}, {0, 1, 0}, 4);
    const GuideId bare = s.h.addChildGuide(kids, parent, shape);
    EXPECT_NEAR(s.h.guide(bare)->tube.widthAt(0.0f), 0.4f * 0.35f, 1e-6f);
    TubeProfile none;
    none.width.clear();
    const GuideId empty = s.h.addChildGuide(kids, parent, shape, none);
    EXPECT_NEAR(s.h.guide(empty)->tube.widthAt(1.0f), 0.2f * 0.35f, 1e-6f);
}

TEST(Brush, SmoothRelaxesKinks) {
    Scene s;
    const GuideId id = s.add(0.5f, 0.5f);
    std::vector<Vec3f> kinked = s.pts(id);
    for (std::size_t i = 1; i < kinked.size(); ++i) kinked[i].x += (i % 2 ? 0.08f : -0.08f);
    s.h.setPoints(id, kinked);
    s.h.evaluate(s.scalp.get());

    auto wiggle = [&] {
        float sum = 0;
        const auto& p = s.pts(id);
        for (std::size_t i = 1; i + 1 < p.size(); ++i) sum += std::fabs(p[i].x - 0.5f * (p[i - 1].x + p[i + 1].x));
        return sum;
    };
    const float before = wiggle();
    const float length = totalLength(s.pts(id));
    BrushSettings b;
    b.mode = BrushMode::Smooth;
    b.radius = 2.0f;
    b.strength = 0.8f;
    float last = before;
    for (int i = 0; i < 5; ++i) {
        applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
        EXPECT_LT(wiggle(), last);  // every stamp relaxes further
        last = wiggle();
    }
    EXPECT_LT(wiggle(), 0.5f * before);
    EXPECT_NEAR(totalLength(s.pts(id)), length, 1e-4f);
}

TEST(Brush, LiftRaisesAFlatGuide) {
    Scene s;
    const GuideId id = s.add(0.2f, 0.5f);
    std::vector<Vec3f> flat = test::straightCurve({0.2f, 0, 0.5f}, {1, 0.05f, 0}, 8);
    s.h.setPoints(id, flat);
    s.h.evaluate(s.scalp.get());
    const float before = s.pts(id).back().y;

    BrushSettings b;
    b.mode = BrushMode::Lift;
    b.radius = 2.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.0f, 0.5f}});
    EXPECT_GT(s.pts(id).back().y, before + 0.2f);

    b.strength = -1.0f;
    const float lifted = s.pts(id).back().y;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.0f, 0.5f}});
    EXPECT_LT(s.pts(id).back().y, lifted);
}

TEST(Brush, LengthGrowsAndCuts) {
    Scene s;
    const GuideId id = s.add(0.5f, 0.5f);
    const std::size_t n = s.pts(id).size();
    BrushSettings b;
    b.mode = BrushMode::Length;
    b.radius = 2.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    EXPECT_NEAR(totalLength(s.pts(id)), 1.05f, 1e-3f);
    EXPECT_EQ(s.pts(id).size(), n);

    b.strength = -4.0f;  // cut 20%
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    EXPECT_NEAR(totalLength(s.pts(id)), 1.05f * 0.8f, 1e-3f);
    EXPECT_NEAR(s.pts(id).front().y, 0.0f, 1e-5f);
}

TEST(Brush, ShapeGrowsTheTube) {
    Scene s;
    const GuideId id = s.add(0.5f, 0.5f);
    const float r = s.h.guide(id)->tube.widthAt(0.0f);
    BrushSettings b;
    b.mode = BrushMode::ShapeTube;
    b.radius = 2.0f;
    b.strength = 1.0f;
    b.hardness = 1.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.5f, 0.5f, 0.5f}});
    for (const float w : s.h.guide(id)->tube.width) EXPECT_NEAR(w, r * 1.05f, 1e-5f);  // a hard brush: every section alike
}

TEST(Brush, PlaceAddsAStandingGuide) {
    Scene s;
    SurfacePoint hit;
    ASSERT_TRUE(s.scalp->intersectRay({0.25f, 1.0f, 0.75f}, {0, -1, 0}, &hit));
    PlaceSettings p;
    p.group = "placed";
    p.length = 0.7f;
    p.segments = 6;
    p.matchNeighbors = false;
    const GuideId id = placeGuide(s.h, *s.scalp, p, hit);
    const auto& pts = s.pts(id);
    ASSERT_EQ(pts.size(), 7u);
    EXPECT_NEAR(pts.front().y, 0.0f, 1e-5f);
    EXPECT_NEAR(pts.back().y, 0.7f, 1e-5f);
    EXPECT_GE(s.h.guide(id)->root.face, 0);
    EXPECT_EQ(s.h.groupDepth(s.h.findGroup("placed")), 0);
}

TEST(Brush, SlideMovesRootsAlongTheScalpKeepingShape) {
    Scene s;
    const GuideId id = s.add(0.5f, 0.5f);
    BrushSettings comb;
    comb.radius = 0.6f;
    applyBrushStroke(s.h, s.scalp.get(), comb, {{0.5f, 0.8f, 0.5f}, {0.9f, 0.8f, 0.5f}});  // give it a shape
    const std::vector<Vec3f> before = s.pts(id);

    BrushSettings slide;
    slide.mode = BrushMode::Slide;
    slide.radius = 0.6f;
    applyBrushStroke(s.h, s.scalp.get(), slide, {{0.5f, 0.0f, 0.5f}, {0.7f, 0.0f, 0.5f}});
    const std::vector<Vec3f>& after = s.pts(id);
    ASSERT_EQ(after.size(), before.size());
    EXPECT_GT(after.front().x, before.front().x + 0.05f);  // the root moved with the drag
    EXPECT_NEAR(after.front().y, 0.0f, 1e-4f);             // on the scalp
    for (std::size_t i = 0; i < after.size(); ++i) {
        EXPECT_LT(((after[i] - after[0]) - (before[i] - before[0])).length(), 1e-4f) << i;  // same shape
    }
    EXPECT_GE(s.h.guide(id)->root.face, 0);
}

TEST(Brush, PlaceInterpolatesItsNeighbors) {
    Scene s;
    const GuideId a = s.add(0.3f, 0.5f);
    const GuideId b = s.add(0.7f, 0.5f);
    s.h.setAttribute(a, "clump", 0.2f);
    s.h.setAttribute(b, "clump", 0.8f);
    TubeProfile fat;
    fat.width = {0.3f, 0.2f};
    s.h.setTube(a, fat);
    SurfacePoint hit;
    ASSERT_TRUE(s.scalp->intersectRay({0.4f, 1.0f, 0.5f}, {0, -1, 0}, &hit));
    PlaceSettings p;
    p.segments = 3;  // ignored: the neighbors have 8 segments
    p.length = 5.0f;  // ignored: they are 1 long
    const GuideId id = placeGuide(s.h, *s.scalp, p, hit);
    const HierarchyGuide& g = *s.h.guide(id);
    EXPECT_EQ(g.points.size(), 9u);
    EXPECT_NEAR(totalLength(g.points), 1.0f, 1e-3f);
    EXPECT_GT(g.attributes.at("clump"), 0.2f);
    EXPECT_LT(g.attributes.at("clump"), 0.5f);  // nearer a
    // The two nearest guides' tubes blended by inverse distance (0.1 from a, 0.3 from b).
    EXPECT_NEAR(g.tube.widthAt(0.0f), (0.3f * 10.0f + 0.1f * 10.0f / 3.0f) / (40.0f / 3.0f), 1e-4f);
    EXPECT_NEAR(g.tube.widthAt(1.0f), (0.2f * 10.0f + 0.1f * 10.0f / 3.0f) / (40.0f / 3.0f), 1e-4f);
}

TEST(Brush, NewGuidesTakeTheirGroupsSegments) {
    Scene s;
    s.add(0.3f, 0.5f);
    s.add(0.7f, 0.5f);  // 8 segments each
    s.h.setGroupSegments(s.primary, 5);
    SurfacePoint hit;
    ASSERT_TRUE(s.scalp->intersectRay({0.4f, 1.0f, 0.5f}, {0, -1, 0}, &hit));
    PlaceSettings p;
    p.segments = 3;  // the group's own win
    EXPECT_EQ(s.pts(placeGuide(s.h, *s.scalp, p, hit)).size(), 6u);  // the neighbors' shape at the group's count
    ASSERT_TRUE(s.scalp->intersectRay({0.5f, 1.0f, 0.2f}, {0, -1, 0}, &hit));
    p.matchNeighbors = false;
    EXPECT_EQ(s.pts(placeGuide(s.h, *s.scalp, p, hit)).size(), 6u);  // standing
    EXPECT_EQ(s.pts(drawGuide(s.h, *s.scalp, p, {{0.6f, 0, 0.2f}, {0.6f, 0.5f, 0.2f}, {0.6f, 1.0f, 0.2f}})).size(), 6u);
    // A group with none of its own: the tool's.
    p.group = "other";
    EXPECT_EQ(s.pts(drawGuide(s.h, *s.scalp, p, {{0.2f, 0, 0.8f}, {0.2f, 0.5f, 0.8f}, {0.2f, 1.0f, 0.8f}})).size(), 4u);
}

TEST(Brush, IncrementalEvaluationMatchesFull) {
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(21);
    for (int i = 0; i < 80; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f, 10));
    }
    h.evaluate(scalp.get());
    ChildGenerationParams p;
    p.count = 10;
    const GroupId kids = h.generateChildren(primary, "children", p);
    h.evaluate(scalp.get());
    p.count = 5;
    h.generateChildren(kids, "grandchildren", p);
    h.evaluate(scalp.get());  // 80 + 800 + 4000 guides

    BrushSettings b;
    b.radius = 0.4f;
    b.level = 0;
    const auto t0 = std::chrono::steady_clock::now();
    applyBrushStroke(h, scalp.get(), b, {{0, 1.3f, 0}, {0.5f, 1.3f, 0}});
    const double strokeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // A full evaluate rebuilds brushed primaries from their scalp-local copy,
    // which differs from the incremental result only by float rounding.
    GuideHierarchy full = h;
    full.evaluate(scalp.get());
    float worst = 0.0f;
    for (const HierarchyGuide& g : h.guides()) {
        const HierarchyGuide& f = *full.guide(g.id);
        ASSERT_EQ(f.points.size(), g.points.size());
        for (std::size_t i = 0; i < g.points.size(); ++i) worst = std::max(worst, (f.points[i] - g.points[i]).length());
    }
    EXPECT_LT(worst, 1e-5f);

    const auto t1 = std::chrono::steady_clock::now();
    full.evaluate(scalp.get());
    const double fullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("[ brush ] local stroke on %zu guides: %.2f ms (one full evaluate: %.2f ms)\n", h.guides().size(),
                strokeMs, fullMs);
}

TEST(Brush, ModeNames) {
    for (const auto mode : {BrushMode::Comb, BrushMode::Smooth, BrushMode::Lift, BrushMode::Length, BrushMode::ShapeTube}) {
        EXPECT_EQ(brushModeFromString(brushModeName(mode)), mode);
    }
    EXPECT_EQ(brushModeFromString("cut"), BrushMode::Cut);
    EXPECT_EQ(brushModeFromString("slide"), BrushMode::Slide);
    EXPECT_EQ(brushModeFromString(brushModeName(BrushMode::Slide)), BrushMode::Slide);
    EXPECT_EQ(brushModeFromString("clump"), BrushMode::Attribute);
    EXPECT_EQ(brushModeFromString(brushModeName(BrushMode::Attribute)), BrushMode::Attribute);
    EXPECT_THROW(brushModeFromString("bogus"), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab

// ---- Culling, screen weights, what the scalp hides, hover ------------------------------

namespace groomlab {
namespace {

using test::lookAtProjection;

// Guides on the unit sphere: `primaries` with `children` each (a child group), 12 segments.
GuideHierarchy groomOnSphere(const ScalpMesh& scalp, int primaries, std::size_t children, std::uint64_t seed = 3) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(seed);
    for (int i = 0; i < primaries; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.7f + Vec3f(0.1f, -0.15f, 0.05f), 12));
    }
    h.evaluate(&scalp);
    if (children > 0) {
        ChildGenerationParams p;
        p.count = children;
        h.generateChildren(primary, "children", p);
        h.evaluate(&scalp);
    }
    return h;
}

TEST(ViewProjection, ProjectsAndUnprojectsAsTheMatrixSays) {
    const ViewProjection view(lookAtProjection({0, 1, 5}, {0, 0.5f, 0}, 40.0, 800.0 / 600.0), 800, 600);
    ASSERT_TRUE(view.valid());
    float x = 0, y = 0;
    ASSERT_TRUE(view.project({0, 0.5f, 0}, x, y));  // the target: the port's center
    EXPECT_NEAR(x, 400.0f, 1e-3f);
    EXPECT_NEAR(y, 300.0f, 1e-3f);
    EXPECT_FALSE(view.project({0, 1, 8}, x, y));  // behind the camera
    // A pixel's ray passes through the points projecting there.
    Vec3f a, b;
    ASSERT_TRUE(view.unproject(123.0f, 456.0f, a, b));
    for (const float t : {0.0f, 0.3f, 1.0f}) {
        ASSERT_TRUE(view.project(a + (b - a) * t, x, y));
        EXPECT_NEAR(x, 123.0f, 0.01f);
        EXPECT_NEAR(y, 456.0f, 0.01f);
    }
    // A box's projection lies within its corners' bounds.
    float x0, y0, x1, y1;
    ASSERT_TRUE(view.projectBox({-0.3f, 0.2f, -0.4f}, {0.5f, 0.9f, 0.6f}, x0, y0, x1, y1));
    Rng rng(9);
    for (int i = 0; i < 200; ++i) {
        const Vec3f p{rng.uniform(-0.3f, 0.5f), rng.uniform(0.2f, 0.9f), rng.uniform(-0.4f, 0.6f)};
        ASSERT_TRUE(view.project(p, x, y));
        EXPECT_TRUE(x >= x0 - 1e-3f && x <= x1 + 1e-3f && y >= y0 - 1e-3f && y <= y1 + 1e-3f);
    }
    EXPECT_FALSE(view.projectBox({-1, -1, 4}, {1, 1, 7}, x0, y0, x1, y1));  // crosses the camera
    EXPECT_EQ(view.hash(), ViewProjection(view.matrix(), 800, 600).hash());
    EXPECT_NE(view.hash(), ViewProjection(view.matrix(), 801, 600).hash());
}

TEST(ScalpVisibility, AnswersByCellWhateverAskedFirst) {
    const auto sphere = test::makeSphere(40, 80, 1.0f);
    const Vec3f eye{0, 0.2f, 6};
    const ScalpVisibility vis(sphere, eye, (Vec3f{0, 0, 0} - eye).normalized(), false);
    EXPECT_FALSE(vis.hidden({0, 0.1f, 1.0f}));   // the front surface
    EXPECT_FALSE(vis.hidden({0, 0.1f, 1.4f}));   // in front of it
    EXPECT_TRUE(vis.hidden({0, 0.1f, -1.0f}));   // the back surface
    EXPECT_TRUE(vis.hidden({0.1f, 0.0f, -1.5f}));  // behind the head
    // The front half's surface points are visible, the back half's hidden (away from the rim).
    std::size_t wrong = 0, asked = 0;
    for (const Vec3f& p : sphere->points()) {
        const Vec3f toEye = (eye - p).normalized();
        const float facing = toEye.dot(p.normalized());
        if (std::abs(facing) < 0.3f) continue;
        ++asked;
        wrong += vis.hidden(p) != (facing < 0.0f) ? 1 : 0;
    }
    EXPECT_EQ(wrong, 0u) << "of " << asked;
    // Answers do not depend on the order points are asked in.
    const ScalpVisibility other(sphere, eye, (Vec3f{0, 0, 0} - eye).normalized(), false);
    std::vector<Vec3f> points = sphere->points();
    std::reverse(points.begin(), points.end());
    for (const Vec3f& p : points) ASSERT_EQ(other.hidden(p), vis.hidden(p));
    EXPECT_EQ(other.key(), vis.key());
    // Orthographic: rays along the view.
    const ScalpVisibility ortho(sphere, eye, {0, 0, -1}, true);
    EXPECT_FALSE(ortho.hidden({0.5f, 0.0f, 0.86f}));
    EXPECT_TRUE(ortho.hidden({0.5f, 0.0f, -0.86f}));
    EXPECT_NE(ortho.key(), vis.key());
}

TEST(Brush, CullingAndStrokeCachesChangeTheSameGuidesTheSame) {
    // Every mode, world sphere and screen weight (with the scalp hiding the back), symmetry,
    // masks, propagate, children: stamps that skip guides out of reach and share a stroke cache
    // give the same hierarchy, stamp by stamp, as stamps weighing every guide.
    const auto sphere = test::makeSphere(40, 80, 1.0f);
    const ViewProjection view(lookAtProjection({0.3f, 1.6f, 4.5f}, {0, 0.7f, 0}, 40.0, 1.5), 900, 600);
    const Vec3f eye{0.3f, 1.6f, 4.5f};
    const auto visibility = std::make_shared<ScalpVisibility>(sphere, eye, (Vec3f{0, 0.7f, 0} - eye).normalized(), false);
    const GuideHierarchy start = groomOnSphere(*sphere, 120, 6);
    const BrushMode modes[] = {BrushMode::Comb, BrushMode::Smooth, BrushMode::Lift, BrushMode::Length, BrushMode::ShapeTube,
                               BrushMode::Attribute, BrushMode::Twist, BrushMode::Straighten,
                               BrushMode::Attract, BrushMode::Erase, BrushMode::Cut, BrushMode::Slide};
    std::map<GuideId, float> mask;
    for (const HierarchyGuide& g : start.guides()) {
        if (g.id % 3 != 0) mask[g.id] = g.id % 2 ? 1.0f : 0.5f;
    }
    int variant = 0, changedVariants = 0;
    for (const BrushMode mode : modes) {
        for (const bool screen : {false, true}) {
            ++variant;
            BrushSettings b;
            b.mode = mode;
            b.radius = 0.35f;
            b.strength = mode == BrushMode::Length ? -0.8f : 0.7f;
            b.level = variant % 3 == 0 ? -1 : (variant % 3 == 1 ? 0 : 1);
            b.propagate = variant % 4 == 0;
            b.pointwise = mode == BrushMode::Attribute && variant % 2 == 0;
            b.attribute = "clump";
            b.mirrorAxis = variant % 5 == 0 ? -1 : 0;
            b.mask = variant % 7 == 0 ? &mask : nullptr;
            GuideHierarchy plain = start, culled = start;
            BrushStrokeCache cache;
            for (int i = 0; i < 6; ++i) {
                const float sx = 380.0f + 25.0f * static_cast<float>(i), sy = 380.0f;
                Vec3f nearPoint, farPoint;
                ASSERT_TRUE(view.unproject(sx, sy, nearPoint, farPoint));
                SurfacePoint hit;
                const Vec3f dir = (farPoint - nearPoint).normalized();
                const Vec3f center = sphere->intersectRay(nearPoint, dir, &hit) ? hit.position : Vec3f{0, 1, 0};
                const Vec3f drag{0.03f, 0.01f, 0.0f};
                const BrushWeightFn weight = screen ? screenBrushWeight(view, sx, sy, 90.0f, 0.2f, visibility) : BrushWeightFn{};
                const BrushReachFn reach = screen ? screenBrushReach(view, sx, sy, 90.0f) : BrushReachFn{};
                // The reference: one guide after another, every guide weighed (the sphere given as a
                // custom weight, the same falloff, so nothing is skipped or made in parallel).
                const BrushWeightFn serial = screen ? weight : sphereWeight(center, b.radius, b.hardness);
                const std::size_t a = applyBrushStamp(plain, sphere.get(), b, center, drag, serial);
                const std::size_t c = applyBrushStamp(culled, sphere.get(), b, center, drag, weight, reach, &cache, screen);
                ASSERT_EQ(a, c) << brushModeName(mode) << (screen ? " screen" : " sphere") << " stamp " << i;
                ASSERT_EQ(plain.contentHash(), culled.contentHash()) << brushModeName(mode) << (screen ? " screen" : " sphere")
                                                                      << " stamp " << i;
            }
            if (plain.contentHash() != start.contentHash()) {
                ++changedVariants;
            } else {
                std::printf("unchanged: %s %s level %d\n", brushModeName(mode), screen ? "screen" : "sphere", b.level);
            }
        }
    }
    EXPECT_GE(changedVariants, variant - 3);  // a stroke out of reach at some level can change nothing
    // A scripted stroke (its stamps share a cache) gives what one stamp after another gives.
    BrushSettings comb;
    comb.radius = 0.3f;
    GuideHierarchy stroked = start, stamped = start;
    applyBrushStroke(stroked, sphere.get(), comb, {{0.0f, 1.05f, 0.2f}, {0.5f, 0.9f, 0.3f}});
    const Vec3f from{0.0f, 1.05f, 0.2f}, to{0.5f, 0.9f, 0.3f};
    const int steps = static_cast<int>(std::ceil((to - from).length() / (0.3f * 0.25f)));
    for (int k = 0; k < steps; ++k) {
        applyBrushStamp(stamped, sphere.get(), comb, from + (to - from) * (static_cast<float>(k) / steps), (to - from) / static_cast<float>(steps));
    }
    EXPECT_EQ(stroked.contentHash(), stamped.contentHash());
}

TEST(ScreenPointGrid, FindsEveryPolylineWithAPointInTheCircle) {
    const auto sphere = test::makeSphere(20, 40, 1.0f);
    const GuideHierarchy h = groomOnSphere(*sphere, 300, 0);
    const ViewProjection view(lookAtProjection({0, 1.5f, 4}, {0, 0.6f, 0}, 45.0, 1.0), 700, 700);
    std::vector<const std::vector<Vec3f>*> lines;
    for (const HierarchyGuide& g : h.guides()) lines.push_back(&g.points);
    const ScreenPointGrid grid(view, lines);
    EXPECT_EQ(grid.size(), lines.size());
    for (const auto& [x, y, r] : {std::tuple{350.0f, 350.0f, 40.0f}, std::tuple{200.0f, 500.0f, 80.0f},
                                  std::tuple{10.0f, 10.0f, 30.0f}, std::tuple{350.0f, 300.0f, 0.5f}}) {
        std::vector<std::uint32_t> want;
        for (std::size_t k = 0; k < lines.size(); ++k) {
            for (const Vec3f& p : *lines[k]) {
                float px, py;
                if (view.project(p, px, py) && std::hypot(px - x, py - y) < r) {
                    want.push_back(static_cast<std::uint32_t>(k));
                    break;
                }
            }
        }
        const std::vector<std::uint32_t> got = grid.near(x, y, r);
        EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
        for (const std::uint32_t k : want) EXPECT_TRUE(std::binary_search(got.begin(), got.end(), k)) << k;
        EXPECT_LT(got.size(), lines.size());
    }
}

}  // namespace
}  // namespace groomlab
