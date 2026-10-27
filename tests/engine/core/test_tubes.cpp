// SPDX-License-Identifier: Apache-2.0
// Tube guides (tube.h): the shapes and the section mapping, children following a tube's width,
// aspect, twist and shape, the stored form.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "groomlab/brush.h"
#include "groomlab/display.h"
#include "groomlab/graph.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/tube.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

std::vector<Vec3f> straightGuide(const Vec3f& root, float length = 1.0f, int segments = 16) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= segments; ++i) pts.push_back(root + Vec3f(0.0f, length * static_cast<float>(i) / segments, 0.0f));
    return pts;
}

TubeShape shapeAt(std::size_t k) { return static_cast<TubeShape>(k); }

// ---------------------------------------------------------------------------
// Shapes

TEST(TubeShapes, OutlinesFillTheUnitDomainCounterClockwise) {
    ASSERT_EQ(tubeShapeNames().size(), 4u);
    for (std::size_t k = 0; k < 4; ++k) {
        const TubeShape shape = shapeAt(k);
        const std::vector<SectionPoint>& outline = tubeOutline(shape);
        ASSERT_GE(outline.size(), 32u) << tubeShapeName(shape);
        float lu = 9, hu = -9, lv = 9, hv = -9, area = 0;
        for (std::size_t i = 0; i < outline.size(); ++i) {
            const SectionPoint& a = outline[i];
            const SectionPoint& b = outline[(i + 1) % outline.size()];
            lu = std::min(lu, a.u), hu = std::max(hu, a.u), lv = std::min(lv, a.v), hv = std::max(hv, a.v);
            area += a.u * b.v - b.u * a.v;
        }
        EXPECT_NEAR(lu, -1.0f, 0.02f) << tubeShapeName(shape);
        EXPECT_NEAR(hu, 1.0f, 0.02f) << tubeShapeName(shape);
        EXPECT_NEAR(lv, -1.0f, 0.02f) << tubeShapeName(shape);
        EXPECT_NEAR(hv, 1.0f, 0.02f) << tubeShapeName(shape);
        EXPECT_GT(area, 0.0f) << tubeShapeName(shape) << " runs counter-clockwise";
        TubeShape parsed = TubeShape::Round;
        EXPECT_TRUE(parseTubeShape(tubeShapeName(shape), &parsed));
        EXPECT_EQ(parsed, shape);
        EXPECT_FALSE(insideTubeShape(shape, 1.2f, 0.0f));
        EXPECT_FALSE(insideTubeShape(shape, 0.0f, -1.2f));
        EXPECT_FALSE(insideTubeShape(shape, 5.0f, 5.0f));
        EXPECT_LT(tubeShapeDepth(shape, 3.0f, 0.0f), 0.0f);
    }
    TubeShape untouched = TubeShape::Blade;
    EXPECT_FALSE(parseTubeShape("square", &untouched));
    EXPECT_EQ(untouched, TubeShape::Blade);
}

TEST(TubeShapes, InsideAndDepthFollowEachShape) {
    // Round: one minus the normalized radius.
    EXPECT_NEAR(tubeShapeDepth(TubeShape::Round, 0.0f, 0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(tubeShapeDepth(TubeShape::Round, 0.6f, 0.0f), 0.4f, 1e-6f);
    EXPECT_TRUE(insideTubeShape(TubeShape::Round, 0.7f, 0.7f));
    EXPECT_FALSE(insideTubeShape(TubeShape::Round, 0.75f, 0.75f));
    // Ribbon: a rounded square, so its corners are cut but its sides reach the domain's edge.
    EXPECT_TRUE(insideTubeShape(TubeShape::Ribbon, 0.95f, 0.0f));
    EXPECT_TRUE(insideTubeShape(TubeShape::Ribbon, 0.8f, 0.8f));
    EXPECT_FALSE(insideTubeShape(TubeShape::Ribbon, 0.97f, 0.97f));
    // Teardrop: round at the bottom (against the scalp), pointed at the top (away from it).
    EXPECT_TRUE(insideTubeShape(TubeShape::Teardrop, 0.0f, 0.9f));
    EXPECT_FALSE(insideTubeShape(TubeShape::Teardrop, 0.5f, 0.85f));
    EXPECT_TRUE(insideTubeShape(TubeShape::Teardrop, 0.5f, -0.4f));
    // Blade: pointed at both ends of u.
    EXPECT_TRUE(insideTubeShape(TubeShape::Blade, 0.0f, 0.9f));
    EXPECT_TRUE(insideTubeShape(TubeShape::Blade, 0.9f, 0.0f));
    EXPECT_FALSE(insideTubeShape(TubeShape::Blade, 0.9f, 0.5f));
    // Corners (what the wire draws lines along the tube through): a blade's points and crests,
    // a ribbon's corners, a teardrop's point, none for round.
    auto corner = [](TubeShape shape, std::size_t k) { return tubeOutline(shape)[tubeOutlineCorners(shape)[k]]; };
    EXPECT_TRUE(tubeOutlineCorners(TubeShape::Round).empty());
    ASSERT_EQ(tubeOutlineCorners(TubeShape::Blade).size(), 4u);
    EXPECT_NEAR(corner(TubeShape::Blade, 0).u, 1.0f, 1e-4f);
    EXPECT_NEAR(corner(TubeShape::Blade, 1).v, 1.0f, 1e-4f);
    EXPECT_NEAR(corner(TubeShape::Blade, 2).u, -1.0f, 1e-4f);
    EXPECT_NEAR(corner(TubeShape::Blade, 3).v, -1.0f, 1e-4f);
    ASSERT_EQ(tubeOutlineCorners(TubeShape::Teardrop).size(), 1u);
    EXPECT_NEAR(corner(TubeShape::Teardrop, 0).v, 1.0f, 1e-4f);
    EXPECT_NEAR(corner(TubeShape::Teardrop, 0).u, 0.0f, 1e-4f);
    ASSERT_EQ(tubeOutlineCorners(TubeShape::Ribbon).size(), 4u);
    EXPECT_GT(corner(TubeShape::Ribbon, 0).u, 0.85f);
    EXPECT_GT(corner(TubeShape::Ribbon, 0).v, 0.85f);
    // Depth: 0 on the outline, highest inside.
    for (std::size_t k = 0; k < 4; ++k) {
        const TubeShape shape = shapeAt(k);
        for (const SectionPoint& p : tubeOutline(shape)) EXPECT_NEAR(tubeShapeDepth(shape, p.u, p.v), 0.0f, 1e-4f);
    }
}

TEST(TubeShapes, RandomPointsLieInsideAndRepeat) {
    for (std::size_t k = 0; k < 4; ++k) {
        const TubeShape shape = shapeAt(k);
        double su = 0, sv = 0;
        for (std::uint64_t seed = 0; seed < 400; ++seed) {
            const SectionPoint p = randomPointInTubeShape(shape, seed);
            EXPECT_TRUE(insideTubeShape(shape, p.u, p.v)) << tubeShapeName(shape) << " " << p.u << " " << p.v;
            const SectionPoint again = randomPointInTubeShape(shape, seed);
            EXPECT_EQ(p.u, again.u);
            EXPECT_EQ(p.v, again.v);
            su += p.u, sv += p.v;
        }
        // Symmetric across v: points spread on both sides of u = 0.
        EXPECT_NEAR(su / 400.0, 0.0, 0.12) << tubeShapeName(shape);
    }
}

// ---------------------------------------------------------------------------
// The section mapping

TEST(TubeSections, OffsetAndCoordsAreInverses) {
    const TubeSection s{0.3f, 0.25f, 0.7f};
    for (const SectionPoint p : {SectionPoint{0.5f, -0.4f}, SectionPoint{-1.0f, 1.0f}, SectionPoint{0.0f, 0.0f}}) {
        const Vec3f o = sectionOffset(s, p.u, p.v, 2.0f);
        const SectionPoint back = sectionCoords(s, o.x, o.y, 2.0f);
        EXPECT_NEAR(back.u, p.u, 1e-5f);
        EXPECT_NEAR(back.v, p.v, 1e-5f);
    }
    // Without twist: u across by the width, v up by width times aspect.
    const Vec3f o = sectionOffset({0.3f, 0.25f, 0.0f}, 1.0f, 1.0f);
    EXPECT_NEAR(o.x, 0.3f, 1e-6f);
    EXPECT_NEAR(o.y, 0.075f, 1e-6f);
    // A quarter turn: u now runs along y.
    const Vec3f q = sectionOffset({0.3f, 1.0f, kPi / 2}, 1.0f, 0.0f);
    EXPECT_NEAR(q.x, 0.0f, 1e-6f);
    EXPECT_NEAR(q.y, 0.3f, 1e-6f);
}

TEST(TubeSections, ProfileListsSampleEvenlyAndClampTheAspect) {
    TubeProfile t;
    EXPECT_FALSE(t.shaped());
    t.width = {0.2f, 0.1f};
    t.aspect = {1.0f, 0.0f};
    t.twist = {0.0f, 1.0f, 3.0f};
    EXPECT_TRUE(t.shaped());
    EXPECT_NEAR(t.widthAt(0.5f), 0.15f, 1e-6f);
    EXPECT_NEAR(t.aspectAt(1.0f), kMinTubeAspect, 1e-7f);
    EXPECT_NEAR(t.twistAt(0.75f), 2.0f, 1e-6f);
    EXPECT_NEAR(t.twistAt(2.0f), 3.0f, 1e-6f);  // clamped to the tip
    TubeProfile ribbon;
    ribbon.shape = TubeShape::Ribbon;
    EXPECT_TRUE(ribbon.shaped());
}

TEST(TubeSections, SpinePlacesPointsThroughTheSections) {
    const std::vector<Vec3f> pts = straightGuide({0, 0, 0}, 2.0f);
    TubeProfile tube;
    tube.width = {0.2f, 0.1f};
    tube.aspect = {0.5f};
    tube.twist = {0.0f, kPi / 2};
    const TubeSpine spine(pts.data(), pts.size(), {1, 0, 0}, tube, 2.0f);  // shown twice the size
    ASSERT_EQ(spine.samples(), pts.size());
    // Root: u along the frame's normal (x), v along its binormal (tangent y cross normal x: -z).
    const Vec3f u0 = spine.at(0.0f, 1.0f, 0.0f), v0 = spine.at(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(u0.x, 0.4f, 1e-5f);
    EXPECT_NEAR((v0 - pts[0]).length(), 0.2f, 1e-5f);  // width 0.4 shown, aspect 0.5
    EXPECT_NEAR(std::abs(v0.z), 0.2f, 1e-5f);
    // Tip: a quarter turn, half as wide; u now along the binormal.
    const Vec3f u1 = spine.at(1.0f, 1.0f, 0.0f) - pts.back();
    EXPECT_NEAR(u1.x, 0.0f, 1e-5f);
    EXPECT_NEAR(std::abs(u1.z), 0.2f, 1e-5f);
    // Between samples: linear.
    const Vec3f mid = spine.at(0.5f, 0.0f, 0.0f);
    EXPECT_NEAR(mid.y, 1.0f, 1e-5f);
    // Back to section coordinates at the root, and how far along.
    float along = 0.0f;
    const SectionPoint back = spine.rootCoords(spine.at(0.0f, 0.3f, -0.6f) + Vec3f(0, 0.05f, 0), &along);
    EXPECT_NEAR(back.u, 0.3f, 1e-5f);
    EXPECT_NEAR(back.v, -0.6f, 1e-5f);
    EXPECT_NEAR(along, 0.05f, 1e-6f);
}

TEST(TubeSections, JsonScalesTheWidthsAsAWhole) {
    TubeProfile tube;
    tube.width = {0.2f, 0.1f};
    tube.aspect = {0.5f};
    const TubeProfile scaled = editedTube(tube, R"({"scaleWidth": 1.5})");
    EXPECT_NEAR(scaled.width[0], 0.3f, 1e-6f);
    EXPECT_NEAR(scaled.width[1], 0.15f, 1e-6f);  // the taper kept
    EXPECT_EQ(scaled.aspect, tube.aspect);
    EXPECT_THROW(editedTube(tube, R"({"scaleWidth": -1})"), std::invalid_argument);
}

TEST(TubeSections, JsonEditsAndRoundTrips) {
    TubeProfile base;
    TubeProfile t = editedTube(base, R"({"width": 0.4, "aspect": [1, 0.2], "shape": "blade"})", 2.0f);
    EXPECT_EQ(t.shape, TubeShape::Blade);
    ASSERT_EQ(t.width.size(), 1u);
    EXPECT_NEAR(t.width[0], 0.2f, 1e-7f);  // divided by the size shown
    EXPECT_EQ(t.aspect, (std::vector<float>{1.0f, 0.2f}));
    EXPECT_EQ(t.twist, base.twist);  // left alone
    t = editedTube(t, R"({"twist": [0, 1], "samples": 3})");
    EXPECT_EQ(t.width.size(), 3u);
    EXPECT_EQ(t.aspect, (std::vector<float>{1.0f, 0.6f, 0.2f}));
    EXPECT_NEAR(t.twist[1], 0.5f, 1e-6f);
    const TubeProfile again = editedTube(TubeProfile{}, tubeToJson(t));
    EXPECT_EQ(again, t);
    for (const char* bad : {R"({"shape": "square"})", R"({"width": -1})", R"({"width": []})", R"({"aspect": 0})",
                            R"({"samples": 1})", R"([1])", "{"}) {
        EXPECT_THROW(editedTube(base, bad), std::invalid_argument) << bad;
    }
}

// ---------------------------------------------------------------------------
// Children follow the tube

// A primary guide standing straight up out of the plane, with `tube`, and its children.
struct TubeHierarchy {
    std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    GroupId primary = kNoGroup, children = kNoGroup;
    GuideId parent = kNoGuide;

    explicit TubeHierarchy(const TubeProfile& tube, ChildGenerationParams params = {}) {
        primary = h.addGroup("primary");
        parent = h.addGuide(primary, straightGuide({0.5f, 0, 0.5f}), tube);
        h.evaluate(scalp.get());
        params.count = 40;
        children = h.generateChildren(primary, "children", params);
        h.evaluate(scalp.get());
    }
    // A child's point i in the parent's frame there (x along its normal, y along its binormal).
    Vec3f local(const HierarchyGuide& child, std::size_t i) const {
        const HierarchyGuide& p = *h.guide(parent);
        return p.frames()[i].toLocal(child.points[i] - p.points[i]);
    }
};

TEST(TubeChildren, FollowTheTubesAspectAndTwist) {
    TubeProfile tube;
    tube.width = {0.2f};
    tube.aspect = {0.2f};
    tube.twist = {0.0f, kPi / 2};
    ChildGenerationParams params;
    params.jitter = 0.0f;
    TubeHierarchy t(tube, params);
    float rootX = 0, rootY = 0, tipX = 0, tipY = 0;
    for (const GuideId id : t.h.guidesInGroup(t.children)) {
        const HierarchyGuide& c = *t.h.guide(id);
        EXPECT_EQ(c.tube.aspect, tube.aspect);  // the parent's tube, narrower
        EXPECT_NEAR(c.tube.widthAt(0.0f), 0.2f * params.childTubeScale, 1e-6f);
        const Vec3f root = t.local(c, 0);  // (the root section lies in the scalp's plane: no snap)
        rootX = std::max(rootX, std::abs(root.x));
        rootY = std::max(rootY, std::abs(root.y));
        const Vec3f tip = t.local(c, c.points.size() - 1);
        tipX = std::max(tipX, std::abs(tip.x));
        tipY = std::max(tipY, std::abs(tip.y));
    }
    // Low on the guide the bundle is flat across y (aspect 0.2); at the tip, a quarter turn
    // later, flat across x.
    EXPECT_GT(rootX, 0.1f);
    EXPECT_LT(rootY, 0.2f * 0.2f * params.rootSpread + 1e-4f);
    EXPECT_LT(tipX, 0.041f);
    EXPECT_GT(tipY, 0.15f);
    // Reshaping the parent's tube moves the children with it.
    const HierarchyGuide child = *t.h.guide(t.h.guidesInGroup(t.children).front());
    TubeProfile wider = tube;
    wider.width = {0.4f};
    t.h.setTube(t.parent, wider);
    t.h.evaluate(t.scalp.get());
    const Vec3f before = t.local(child, 16), after = t.local(*t.h.guide(child.id), 16);
    EXPECT_NEAR(after.x, before.x * 2.0f, 1e-4f);
    EXPECT_NEAR(after.y, before.y * 2.0f, 1e-4f);
}

TEST(TubeChildren, AreMadeInsideTheShape) {
    for (const TubeShape shape : {TubeShape::Ribbon, TubeShape::Teardrop, TubeShape::Blade}) {
        TubeProfile tube;
        tube.shape = shape;
        tube.width = {0.2f};
        ChildGenerationParams params;
        params.rootSpread = 1.0f;
        params.tipSpread = 1.0f;
        params.jitter = 0.0f;
        TubeHierarchy t(tube, params);
        for (const GuideId id : t.h.guidesInGroup(t.children)) {
            const HierarchyGuide& c = *t.h.guide(id);
            EXPECT_EQ(c.tube.shape, shape);
            for (const Vec3f& o : c.offsets) EXPECT_TRUE(insideTubeShape(shape, o.x, o.y)) << tubeShapeName(shape);
            // A shaped tube's children start their frames from the parent's root.
            const Frame parentRoot = t.h.guide(t.parent)->frames().front();
            EXPECT_NEAR(c.frameUp.dot(parentRoot.normal), 1.0f, 1e-4f);
        }
    }
}

TEST(TubeChildren, SectionsSurviveSavingAndLoading) {
    TubeProfile tube;
    tube.shape = TubeShape::Teardrop;
    tube.width = {0.2f, 0.15f, 0.05f};
    tube.aspect = {0.5f};
    tube.twist = {0.0f, 1.0f};
    TubeHierarchy t(tube);
    const GuideHierarchy loaded = GuideHierarchy::fromJson(t.h.toJson());
    EXPECT_EQ(loaded.guide(t.parent)->tube, tube);
    EXPECT_EQ(loaded.toJson(), t.h.toJson());
    // A tube saved before sections (its old radius) loads with the default tube.
    const std::string old = R"({"version": 1, "groups": [{"id": 1, "name": "primary", "parent": 0}],
        "guides": [{"id": 5, "group": 1, "root": [-1, 0, 0], "frameUp": [1, 0, 0],
                    "tube": {"radius": [0.4, 0.2], "twist": 0.0}, "points": [0, 0, 0, 0, 1, 0]}]})";
    const GuideHierarchy before = GuideHierarchy::fromJson(old);
    EXPECT_EQ(before.guide(5)->tube, TubeProfile{});
    // A shape neither built in nor in the groom's library (a missing library shape) loads round,
    // with a warning.
    const GuideHierarchy missing = GuideHierarchy::fromJson(R"({"guides": [{"id": 5, "group": 1, "root": [-1, 0, 0], "frameUp": [1, 0, 0],
        "tube": {"shape": "star"}, "points": [0, 0, 0, 0, 1, 0]}]})");
    EXPECT_TRUE(missing.guide(5)->tube.roundShape());
    ASSERT_EQ(missing.loadWarnings().size(), 1u);
    EXPECT_NE(missing.loadWarnings()[0].find("'star'"), std::string::npos);
}

TEST(TubeDisplay, WireRingsSitAtEveryGuidePoint) {
    // The Shape brush's view of a tube: an outline at each guide point (where the sections it
    // paints sit) and lines along the tube.
    TubeProfile tube;
    tube.shape = TubeShape::Ribbon;
    tube.width = {0.2f, 0.1f};
    tube.aspect = {0.5f};
    TubeHierarchy t(tube);
    const HierarchyGuide& g = *t.h.guide(t.parent);
    ASSERT_EQ(g.points.size(), 17u);
    std::vector<Vec3f> wire;
    appendTubeWire(wire, g, 1.0f, 8);
    EXPECT_EQ(wire.size(), 2u * (17u * 32u + 8u * 16u));
    float rootReach = 0.0f;
    for (std::size_t k = 0; k < 2 * 32; ++k) {  // the root's ring: in the root's plane, as wide as the root section
        EXPECT_NEAR(wire[k].y, g.points[0].y, 1e-5f);
        rootReach = std::max(rootReach, (wire[k] - g.points[0]).length());
    }
    EXPECT_GE(rootReach, 0.2f - 1e-4f);  // past the half-width only at the corners, by the half-height at most
    EXPECT_LE(rootReach, 0.2f * std::sqrt(1.25f));
    std::vector<Vec3f> sparse;
    appendTubeWire(sparse, g, 2.0f, 4, 4);  // every fourth point's ring (the tip's too), twice as wide
    EXPECT_EQ(sparse.size(), 2u * (5u * 32u + 4u * 16u));
    float sparseReach = 0.0f;
    for (std::size_t k = 0; k < 2 * 32; ++k) sparseReach = std::max(sparseReach, (sparse[k] - g.points[0]).length());
    EXPECT_NEAR(sparseReach, 2.0f * rootReach, 1e-4f);
}

// The wire's lines along a tube go through its shape's corners first (a blade's points and crests,
// so its silhouette reads), then fill in evenly; its rings keep the corners.
TEST(TubeDisplay, WireLinesRunAlongTheCorners) {
    TubeProfile tube;
    tube.shape = TubeShape::Blade;
    tube.width = {0.2f};
    TubeHierarchy t(tube);
    const HierarchyGuide& g = *t.h.guide(t.parent);
    const std::size_t n = g.points.size();
    std::vector<Vec3f> wire;
    appendTubeWire(wire, g, 1.0f, 8);
    const std::size_t rings = 2 * n * 32;
    ASSERT_EQ(wire.size(), rings + 2 * 8 * (n - 1));
    const TubeSpine spine(g.points.data(), n, g.frameUp, g.tube);
    // The first four lines start at the blade's two points and two crests.
    for (std::size_t k = 0; k < 4; ++k) {
        const SectionPoint c = tubeOutline(TubeShape::Blade)[tubeOutlineCorners(TubeShape::Blade)[k]];
        const Vec3f start = wire[rings + 2 * k * (n - 1)];
        EXPECT_NEAR((start - spine.atSample(0, c.u, c.v)).length(), 0.0f, 1e-5f) << k;
    }
    // The root's ring reaches the blade's points: as wide as the section.
    float reach = 0.0f;
    for (std::size_t k = 0; k < 2 * 32; ++k) reach = std::max(reach, (wire[k] - g.points[0]).length());
    EXPECT_NEAR(reach, 0.2f, 1e-4f);
}

// Smoothing a tube's lists along the guide (the Shape brush's Shift, the Smooth button), blending
// tubes (a new guide's from its neighbours') and the JSON edit's "smooth".
TEST(TubeSections, SmoothAndBlend) {
    TubeProfile tube;
    tube.width = {0.1f, 0.3f, 0.1f, 0.3f, 0.1f};
    tube.twist = {0.0f, 2.0f};
    const TubeProfile smooth = smoothedTube(tube, kTubeWidthList, 9, 3);
    ASSERT_EQ(smooth.width.size(), 9u);
    EXPECT_FLOAT_EQ(smooth.width.front(), 0.1f);  // the root's and tip's kept
    EXPECT_FLOAT_EQ(smooth.width.back(), 0.1f);
    float spread = 0.0f;
    for (std::size_t i = 1; i + 1 < 9; ++i) spread = std::max(spread, std::abs(smooth.width[i] - smooth.width[i - 1]));
    EXPECT_LT(spread, 0.08f);  // was 0.1 a step
    EXPECT_EQ(smooth.twist, tube.twist);  // not asked for
    // Weighted: only where the weights let it.
    std::vector<float> weights(9, 0.0f);
    weights[4] = 1.0f;
    const TubeProfile local = smoothedTube(tube, kTubeAllLists, 9, 1, 1.0f, &weights);
    EXPECT_FLOAT_EQ(local.width[2], tube.widthAt(0.25f));
    EXPECT_NE(local.width[4], tube.widthAt(0.5f));
    // Blended: the first's shape, each list the weighted mean, said in as few values as it takes.
    TubeProfile a, b;
    a.shape = TubeShape::Teardrop;
    a.width = {0.2f, 0.1f};
    b.width = {0.4f};
    b.aspect = {0.5f};
    const TubeProfile mix = blendedTube({&a, &b}, {3.0f, 1.0f}, 12);
    EXPECT_EQ(mix.shape, TubeShape::Teardrop);
    ASSERT_EQ(mix.width.size(), 2u);
    EXPECT_NEAR(mix.width[0], 0.25f, 1e-5f);
    EXPECT_NEAR(mix.width[1], 0.175f, 1e-5f);
    ASSERT_EQ(mix.aspect.size(), 1u);
    EXPECT_NEAR(mix.aspect[0], 0.875f, 1e-5f);
    EXPECT_EQ(blendedTube({}, {}, 4), TubeProfile{});
    // The JSON edit: "smooth" at the guide's point count.
    const TubeProfile edited = editedTube(tube, R"({"smooth": "width", "passes": 2})", 1.0f, 9);
    EXPECT_EQ(edited.width.size(), 9u);
    EXPECT_THROW(editedTube(tube, R"({"smooth": "radius"})"), std::invalid_argument);
}

// What the Shape brush's wire shows: the guides it can shape, not hidden groups' nor frozen ones.
TEST(TubeDisplay, ActiveGuidesLeaveOutHiddenAndFrozen) {
    GuideHierarchy h;
    const GroupId shown = h.addGroup("shown"), hidden = h.addGroup("hidden");
    const GuideId a = h.addGuide(shown, straightGuide({0.2f, 0, 0.2f}));
    const GuideId frozen = h.addGuide(shown, straightGuide({0.4f, 0, 0.2f}));
    h.addGuide(hidden, straightGuide({0.6f, 0, 0.2f}));
    h.setAttribute(frozen, "freeze", 1.0f);
    const std::vector<const HierarchyGuide*> active = activeTubeGuides(h, "hidden");
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->id, a);
    EXPECT_EQ(activeTubeGuides(h, "").size(), 2u);
}

TEST(TubeChildren, FlattenedGuidesCarryTheirTubes) {
    TubeProfile tube;
    tube.shape = TubeShape::Ribbon;
    tube.width = {0.2f};
    TubeHierarchy t(tube);
    t.h.setScale(2.0f);
    t.h.evaluate(t.scalp.get());
    const GuideSet primaries = t.h.flatten(0);
    ASSERT_EQ(primaries.size(), 1u);
    EXPECT_TRUE(primaries.guides()[0].hasTube);
    EXPECT_EQ(primaries.guides()[0].tube.shape, TubeShape::Ribbon);
    EXPECT_NEAR(primaries.guides()[0].tube.width[0], 0.4f, 1e-6f);  // at the size shown
    // A tube's change changes the set's hash (Interpolate's key).
    const std::uint64_t h0 = primaries.contentHash();
    TubeProfile flatter = tube;
    flatter.aspect = {0.3f};
    t.h.setTube(t.parent, flatter);
    EXPECT_NE(t.h.flatten(0).contentHash(), h0);
}

// ---------------------------------------------------------------------------
// Interpolate's fillTubes and Clump's followTubes

// Strands on the unit plane (normal +y) and two guides: one standing straight up at the middle,
// 1 long, carrying a tube (a ribbon, flat, a quarter turn root to tip, tapering), and a plain one
// off to the side (no tube).
struct TubeGraph {
    Graph g;
    ExternalInputs ext;
    NodeId scatter = 0, interp = 0, clump = 0;
    TubeProfile tube;
    static constexpr float kLength = 1.0f;

    explicit TubeGraph(double density = 6000.0) {
        tube.shape = TubeShape::Ribbon;
        tube.width = {0.15f, 0.08f};
        tube.aspect = {0.25f};
        tube.twist = {0.0f, kPi / 2};
        ext.set("scalp", test::makePlane(8), 1);
        setGuides(tube);
        const NodeId scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        g.connect(scalp, scatter, 0);
        const NodeId resample = g.addNode(op::kResample);  // unconnected: the strands' segments
        g.node(resample).params().set("segments", std::int64_t{15});
        const NodeId guides = g.addNode(op::kGuides);
        interp = g.addNode(op::kInterpolate);
        g.connect(scatter, interp, 0);
        g.connect(guides, interp, 1);
        g.node(interp).params().set("fillTubes", true);
        clump = g.addNode(op::kClump);
        g.connect(interp, clump, 0);
        g.node(clump).params().set("clumpBy", std::string("size"));
        g.node(clump).params().set("size", 0.06);
        g.node(clump).params().set("strength", 1.0);
    }
    void setGuides(const TubeProfile& t, bool withTube = true, std::uint64_t version = 7) {
        auto set = std::make_shared<GuideSet>();
        Guide& a = set->add(1, straightGuide({0.5f, 0, 0.5f}, kLength));
        a.hasTube = withTube;
        a.tube = t;
        a.frameUp = {1, 0, 0};
        set->add(2, straightGuide({0.12f, 0, 0.12f}, 0.6f));
        ext.set("guides", GuidesPtr(set), version);
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
    // Point p of a strand in the tube's section coordinates at t (the guide stands straight up:
    // its frame's normal is x, its binormal -z).
    SectionPoint sectionOf(const Vec3f& p, float t) const {
        return sectionCoords(tube.sectionAt(t), p.x - 0.5f, -(p.z - 0.5f));
    }
};

TEST(FillTubes, StrandsRideTheirTubeToTheTip) {
    TubeGraph f;
    const StrandsPtr out = f.eval(f.interp);
    const std::vector<float>* ids = out->strandChannel("tubeId");
    const std::vector<float>* us = out->strandChannel("tubeU");
    const std::vector<float>* vs = out->strandChannel("tubeV");
    ASSERT_TRUE(ids && us && vs);
    std::size_t inTube = 0;
    float rootX = 0, rootZ = 0, tipX = 0, tipZ = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if ((*ids)[s] < 0.0f) continue;
        ++inTube;
        const float u = (*us)[s], v = (*vs)[s];
        EXPECT_TRUE(insideTubeShape(TubeShape::Ribbon, u, v));
        const Vec3f* p = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        for (std::size_t j = 0; j < n; ++j) {
            const float t = static_cast<float>(j) / static_cast<float>(n - 1);
            EXPECT_NEAR(p[j].y, t * TubeGraph::kLength, 2e-3f);  // as long as the guide, evenly along it
            const SectionPoint at = f.sectionOf(p[j], t);         // the same place in every section
            EXPECT_NEAR(at.u, u, 2e-3f) << s << " " << j;
            EXPECT_NEAR(at.v, v, 2e-3f) << s << " " << j;
        }
        rootX = std::max(rootX, std::abs(p[0].x - 0.5f)), rootZ = std::max(rootZ, std::abs(p[0].z - 0.5f));
        tipX = std::max(tipX, std::abs(p[n - 1].x - 0.5f)), tipZ = std::max(tipZ, std::abs(p[n - 1].z - 0.5f));
    }
    EXPECT_GT(inTube, 100u);  // about the ribbon's area (3.9 w^2 aspect) times the density
    // A flat bundle across x at the root (aspect 0.25), across z at the tip (a quarter turn), and narrower.
    EXPECT_NEAR(rootX, 0.15f, 0.01f);
    EXPECT_LT(rootZ, 0.15f * 0.25f + 1e-3f);
    EXPECT_LT(tipX, 0.08f * 0.25f + 1e-3f);
    EXPECT_NEAR(tipZ, 0.08f, 0.01f);
}

TEST(FillTubes, TheCpuEngineRidesThemAsTheCppDoes) {
    // Interpolate filling tubes on the CPU engine (look.slang rideTubeAt) against its own C++ (the
    // engine off), bit for bit: riders, strands in a soft edge, the rest blending; keeping
    // clearance and not, with a radius's fall-off.
    for (const bool keep : {true, false}) {
        TubeGraph f;
        f.g.node(f.interp).params().set("tubeBlend", 0.5);
        f.g.node(f.interp).params().set("keepClearance", keep);
        f.g.node(f.interp).params().set("limitDistance", 0.5);
        const StrandsPtr engine = f.eval(f.interp);
        gpu::setCpuEngineOn(false);
        TubeGraph g;  // (a graph of its own: evaluated afresh)
        g.g.node(g.interp).params().set("tubeBlend", 0.5);
        g.g.node(g.interp).params().set("keepClearance", keep);
        g.g.node(g.interp).params().set("limitDistance", 0.5);
        const StrandsPtr cpp = g.eval(g.interp);
        gpu::setCpuEngineOn(true);
        ASSERT_EQ(engine->pointCount(), cpp->pointCount());
        std::size_t riders = 0;
        const std::vector<float>& ids = *cpp->strandChannel("tubeId");
        for (std::size_t s = 0; s < cpp->strandCount(); ++s) riders += ids[s] >= 0.0f ? 1 : 0;
        for (std::size_t s = 0; s < cpp->strandCount(); ++s) {
            ASSERT_EQ(std::memcmp(engine->strandPoints(s), cpp->strandPoints(s), cpp->strandPointCount(s) * sizeof(Vec3f)), 0)
                << "strand " << s << (keep ? " keeping clearance" : "");
        }
        EXPECT_EQ(engine->contentHash(), cpp->contentHash()) << keep;
        EXPECT_GT(riders, 100u);
    }
}

TEST(FillTubes, LeavesStrandsOutsideTubesAsTheyWere) {
    TubeGraph f;
    f.g.node(f.interp).params().set("tubeBlend", 0.0);  // a hard edge: every strand outside as it was
    const StrandsPtr filled = f.eval(f.interp);
    f.g.node(f.interp).params().set("fillTubes", false);
    const StrandsPtr plain = f.eval(f.interp);
    EXPECT_EQ(plain->strandChannel("tubeId"), nullptr);
    const std::vector<float>& ids = *filled->strandChannel("tubeId");
    std::size_t outside = 0;
    for (std::size_t s = 0; s < plain->strandCount(); ++s) {
        if (ids[s] >= 0.0f) continue;
        ++outside;
        const std::size_t n = plain->strandPointCount(s);
        EXPECT_EQ(std::memcmp(plain->strandPoints(s), filled->strandPoints(s), n * sizeof(Vec3f)), 0) << s;
        EXPECT_EQ((*filled->strandChannel("tubeU"))[s], 0.0f);
    }
    EXPECT_GT(outside, 1000u);
    // Off, a guide's tube changes nothing: the same strands as guides without tubes, to the bit.
    f.setGuides(f.tube, false, 8);
    const StrandsPtr untubed = f.eval(f.interp);
    ASSERT_EQ(untubed->pointCount(), plain->pointCount());
    EXPECT_EQ(std::memcmp(untubed->positions().data(), plain->positions().data(), plain->pointCount() * sizeof(Vec3f)), 0);
    // On, guides without tubes fill nothing.
    f.g.node(f.interp).params().set("fillTubes", true);
    const StrandsPtr none = f.eval(f.interp);
    for (const float id : *none->strandChannel("tubeId")) EXPECT_EQ(id, -1.0f);
    EXPECT_EQ(std::memcmp(none->positions().data(), plain->positions().data(), plain->pointCount() * sizeof(Vec3f)), 0);
}

// tubeBlend: strands rooted just outside a tube's outline lean toward riding it, all of the way at
// the outline (no jump between the strands inside and those just outside), none at the band's
// edge; strands past the band are exactly what fillTubes off makes.
TEST(FillTubes, TubeBlendSoftensTheEdge) {
    TubeGraph f;
    f.g.node(f.interp).params().set("fillTubes", false);
    const StrandsPtr plain = f.eval(f.interp);
    f.g.node(f.interp).params().set("fillTubes", true);
    const double blend = 0.25;
    f.g.node(f.interp).params().set("tubeBlend", blend);
    const StrandsPtr soft = f.eval(f.interp);
    f.g.node(f.interp).params().set("tubeBlend", 0.0);
    const StrandsPtr hard = f.eval(f.interp);
    const float aspect = f.tube.aspectAt(0.0f);
    // How far a strand's tip lies from where riding the tube at its root's section coordinates
    // puts it, in the tip section's coordinates.
    auto offRide = [&](const StrandSet& out, std::size_t s) {
        const Vec3f* p = out.strandPoints(s);
        const SectionPoint root = f.sectionOf(p[0], 0.0f), tip = f.sectionOf(p[out.strandPointCount(s) - 1], 1.0f);
        return std::hypot(tip.u - root.u, tip.v - root.v);
    };
    std::size_t nearOutline = 0, nearEdge = 0, beyond = 0;
    float softJump = 0.0f, hardJump = 0.0f;
    for (std::size_t s = 0; s < plain->strandCount(); ++s) {
        const Vec3f* p = soft->strandPoints(s);
        const std::size_t n = soft->strandPointCount(s);
        const SectionPoint at = f.sectionOf(p[0], 0.0f);
        if (insideTubeShape(TubeShape::Ribbon, at.u, at.v)) continue;
        const float d = tubeOutsideDistance(TubeShape::Ribbon, aspect, at.u, at.v);
        if (d < 0.02f) {  // just outside: riding the tube, as the strands just inside do
            ++nearOutline;
            softJump = std::max(softJump, offRide(*soft, s));
            hardJump = std::max(hardJump, offRide(*hard, s));
        } else if (d >= static_cast<float>(blend)) {  // past the band: as without fillTubes, to the bit
            ++beyond;
            EXPECT_EQ(std::memcmp(plain->strandPoints(s), p, n * sizeof(Vec3f)), 0) << s;
        } else if (d > 0.9f * static_cast<float>(blend)) {  // the band's edge: all but as without fillTubes
            ++nearEdge;
            EXPECT_LT((p[n - 1] - plain->strandPoints(s)[n - 1]).length(), 0.01f) << s;
        }
        EXPECT_EQ((*soft->strandChannel("tubeId"))[s], -1.0f) << "outside the tube: not in it";
    }
    EXPECT_GT(nearOutline, 5u);
    EXPECT_GT(nearEdge, 5u);
    EXPECT_GT(beyond, 1000u);
    EXPECT_LT(softJump, 0.1f);   // soft: the strands just outside ride the tube too
    EXPECT_GT(hardJump, 0.5f);   // hard: they blend the guides, a seam at the outline
}

// Fill Tubes takes the tubes of every level of the hierarchy, whatever level the Guides node
// hands Interpolate: a child's tube inside its parent's takes the strands rooted in it, the
// parent's tube the rest of its footprint, the deepest winning.
TEST(FillTubes, TubesOfEveryLevelFill) {
    auto hierarchy = std::make_shared<GuideHierarchy>();
    const GroupId primary = hierarchy->addGroup("primary");
    TubeProfile wide;
    wide.width = {0.2f};
    const GuideId parent = hierarchy->addGuide(primary, straightGuide({0.5f, 0, 0.5f}), wide);
    const GroupId kids = hierarchy->addGroup("children", primary);
    TubeProfile narrow;
    narrow.width = {0.05f};
    const GuideId child = hierarchy->addChildGuide(kids, parent, straightGuide({0.58f, 0, 0.5f}), narrow);
    const auto scalp = test::makePlane(8);
    hierarchy->evaluate(scalp.get());
    ASSERT_NE(child, kNoGuide);

    Graph g;
    ExternalInputs ext;
    ext.set("scalp", scalp, 1);
    ext.set("hierarchy", HierarchyPtr(hierarchy), hierarchy->contentHash());
    const NodeId scalpNode = g.addNode(op::kScalp);
    const NodeId scatter = g.addNode(op::kScatter);
    g.node(scatter).params().set("density", 6000.0);
    g.connect(scalpNode, scatter, 0);
    const NodeId guides = g.addNode(op::kGuides);  // level -1: the children
    const NodeId interp = g.addNode(op::kInterpolate);
    g.connect(scatter, interp, 0);
    g.connect(guides, interp, 1);
    g.node(interp).params().set("fillTubes", true);
    g.node(interp).params().set("tubeBlend", 0.0);
    auto eval = [&] {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(interp, ctx);
    };
    // Each strand's tube by where its root lies: the child's, the parent's around it, or none.
    auto check = [&](const StrandsPtr& out) {
        const std::vector<float>& ids = *out->strandChannel("tubeId");
        float childId = -1.0f, parentId = -1.0f;
        std::size_t inChild = 0, inParent = 0;
        const Vec3f childRoot = hierarchy->guide(child)->points.front();
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const Vec3f r = out->strandPoints(s)[0];
            const float dc = std::hypot(r.x - childRoot.x, r.z - childRoot.z), dp = std::hypot(r.x - 0.5f, r.z - 0.5f);
            if (dc < 0.049f) {
                ASSERT_GE(ids[s], 0.0f) << s;
                if (childId < 0.0f) childId = ids[s];
                EXPECT_EQ(ids[s], childId) << "the child's tube, the deepest";
                ++inChild;
            } else if (dc > 0.051f && dp < 0.199f) {
                ASSERT_GE(ids[s], 0.0f) << s;
                if (parentId < 0.0f) parentId = ids[s];
                EXPECT_EQ(ids[s], parentId) << "the parent's tube round the child's";
                ++inParent;
            } else if (dp > 0.201f) {
                EXPECT_EQ(ids[s], -1.0f) << s;
            }
        }
        EXPECT_GT(inChild, 10u);
        EXPECT_GT(inParent, 100u);
        EXPECT_NE(childId, parentId);
    };
    check(eval());
    // The primary level handed over instead: the child's tube still fills (no Guides change needed).
    g.node(guides).params().set("level", std::int64_t{0});
    check(eval());
    // A Guides node that excludes the children leaves their tubes out.
    g.node(guides).params().set("exclude", std::string("children"));
    const StrandsPtr without = eval();
    std::set<float> ids;
    for (const float id : *without->strandChannel("tubeId")) {
        if (id >= 0.0f) ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 1u) << "only the parent's tube";
}

TEST(FillTubes, OutsideDistanceIsInHalfWidthsAllRound) {
    // A round tube: the radius past 1. A flattened one: the same band thickness above as beside it.
    EXPECT_FLOAT_EQ(tubeOutsideDistance(TubeShape::Round, 1.0f, 1.5f, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(tubeOutsideDistance(TubeShape::Round, 1.0f, 0.2f, 0.3f), 0.0f);
    EXPECT_NEAR(tubeOutsideDistance(TubeShape::Ribbon, 0.25f, 1.2f, 0.0f), 0.2f, 1e-4f);
    EXPECT_NEAR(tubeOutsideDistance(TubeShape::Ribbon, 0.25f, 0.0f, 1.0f + 0.2f / 0.25f), 0.2f, 1e-4f);
    EXPECT_GT(tubeOutsideDistance(TubeShape::Blade, 1.0f, 3.0f, 3.0f, 0.5f), 0.5f);  // past the limit: above it
}

TEST(FillTubes, OverlappingTubesTakeTheMostCentral) {
    TubeGraph f;
    auto set = std::make_shared<GuideSet>();
    TubeProfile round;
    round.width = {0.2f};
    for (std::uint64_t id : {1u, 2u}) {
        Guide& g = set->add(id, straightGuide({id == 1 ? 0.45f : 0.6f, 0, 0.5f}));
        g.hasTube = true;
        g.tube = round;
    }
    f.ext.set("guides", GuidesPtr(set), 9);
    const StrandsPtr out = f.eval(f.interp);
    const std::vector<float>& ids = *out->strandChannel("tubeId");
    float first = -1, second = -1;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f r = out->strandPoints(s)[0];
        const float d1 = std::hypot(r.x - 0.45f, r.z - 0.5f), d2 = std::hypot(r.x - 0.6f, r.z - 0.5f);
        if (std::min(d1, d2) > 0.199f || std::abs(d1 - d2) < 1e-3f) continue;
        ASSERT_GE(ids[s], 0.0f);
        float& mine = d1 < d2 ? first : second;
        if (mine < 0.0f) mine = ids[s];
        EXPECT_EQ(ids[s], mine) << "the nearer guide's tube";
    }
    EXPECT_GE(first, 0.0f);
    EXPECT_GE(second, 0.0f);
    EXPECT_NE(first, second);
}

TEST(FillTubes, ClumpsStayInsideTheTubeAndFlattenWithIt) {
    TubeGraph f;
    f.tube.twist = {0.0f};  // flat across x all along
    f.setGuides(f.tube);
    const StrandsPtr filled = f.eval(f.interp);
    const StrandsPtr clumped = f.eval(f.clump);
    const std::vector<float>& tubes = *filled->strandChannel("tubeId");
    const std::vector<float>& clumps = *clumped->strandChannel("clumpId");
    std::map<float, float> tubeOfClump;
    std::set<float> tubeClumps;
    float tipZ = 0.0f;
    for (std::size_t s = 0; s < clumped->strandCount(); ++s) {
        if (clumps[s] < 0.0f) continue;
        // No clump holds strands of a tube and strands outside it.
        const auto it = tubeOfClump.emplace(clumps[s], tubes[s]).first;
        EXPECT_EQ(it->second, tubes[s]) << "clump " << clumps[s];
        if (tubes[s] < 0.0f) continue;
        tubeClumps.insert(clumps[s]);
        const std::size_t n = clumped->strandPointCount(s);
        const Vec3f tip = clumped->strandPoints(s)[n - 1];
        // Gathered toward strands of the tube at the same t: still inside its (convex) section.
        const SectionPoint at = f.sectionOf(tip, 1.0f);
        EXPECT_GE(tubeShapeDepth(TubeShape::Ribbon, at.u, at.v), -0.02f) << s;
        tipZ = std::max(tipZ, std::abs(tip.z - 0.5f));
    }
    EXPECT_GE(tubeClumps.size(), 3u);  // the tube's own clumps, about the size apart across it
    EXPECT_LT(tipZ, 0.08f * 0.25f + 2e-3f);  // flat as the tube
    // Each tube's centers are its own strands (inside its shape).
    ClumpState state;
    ASSERT_TRUE(clumpState(f.g.node(f.clump), clumped, &state));
    std::size_t inside = 0;
    for (const std::size_t g : state.centerGuides) {
        const std::size_t s = state.picked[g];
        if (tubes[s] < 0.0f) continue;
        EXPECT_TRUE(insideTubeShape(TubeShape::Ribbon, (*filled->strandChannel("tubeU"))[s], (*filled->strandChannel("tubeV"))[s]));
        ++inside;
    }
    EXPECT_EQ(inside, tubeClumps.size());
    // Off: clumps by root, across the tube's edge.
    f.g.node(f.clump).params().set("followTubes", false);
    const StrandsPtr across = f.eval(f.clump);
    const std::vector<float>& acrossClumps = *across->strandChannel("clumpId");
    tubeOfClump.clear();
    bool mixed = false;
    for (std::size_t s = 0; s < across->strandCount(); ++s) {
        if (acrossClumps[s] < 0.0f) continue;
        const auto it = tubeOfClump.emplace(acrossClumps[s], tubes[s]).first;
        mixed = mixed || it->second != tubes[s];
    }
    EXPECT_TRUE(mixed);
}

TEST(FillTubes, TheSameOnAnyThreadCount) {
    auto run = [](std::size_t threads) {
        setParallelThreads(threads);
        TubeGraph f;
        const StrandsPtr out = f.eval(f.clump);
        setParallelThreads(0);
        return out;
    };
    const StrandsPtr one = run(1), many = run(0);
    ASSERT_EQ(one->pointCount(), many->pointCount());
    EXPECT_EQ(one->contentHash(), many->contentHash());
    EXPECT_EQ(*one->strandChannel("tubeId"), *many->strandChannel("tubeId"));
    EXPECT_EQ(*one->strandChannel("clumpId"), *many->strandChannel("clumpId"));
}

// A straight guide leaving the unit plane (normal +y) `degrees` above it toward +x, 0.6 long, its
// tube's frame started across the plane (twist 0: lying flat on it), filled by strands scattered
// `density` per unit area; no tube blend, no clearance (a guide this low has the lower half of its
// tube under the plane: clearance would lift those strands, which is not what is measured).
struct SlantGraph {
    Graph g;
    ExternalInputs ext;
    NodeId interp = 0;
    TubeProfile tube;
    std::vector<Vec3f> guide;
    double density;

    SlantGraph(float degrees, const TubeProfile& t, double perArea = 20000.0) : tube(t), density(perArea) {
        const std::shared_ptr<const ScalpMesh> scalp = test::makePlane(16);
        ext.set("scalp", scalp, 1);
        const float a = degrees * kPi / 180.0f;
        const Vec3f way(std::cos(a), std::sin(a), 0.0f);
        for (int i = 0; i <= 16; ++i) guide.push_back(Vec3f(0.3f, 0.0f, 0.5f) + way * (0.6f * static_cast<float>(i) / 16.0f));
        auto set = std::make_shared<GuideSet>();
        Guide& gd = set->add(1, guide);
        gd.hasTube = true;
        gd.tube = t;
        gd.frameUp = {0, 0, -1};
        set->rootOnMesh(*scalp);
        ext.set("guides", GuidesPtr(set), 1);
        const NodeId scalpNode = g.addNode(op::kScalp);
        const NodeId scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        g.connect(scalpNode, scatter, 0);
        const NodeId resample = g.addNode(op::kResample);  // unconnected: the strands' segments
        g.node(resample).params().set("segments", std::int64_t{15});
        const NodeId guides = g.addNode(op::kGuides);
        interp = g.addNode(op::kInterpolate);
        g.connect(scatter, interp, 0);
        g.connect(guides, interp, 1);
        g.connect(scalpNode, interp, 2);
        g.node(interp).params().set("fillTubes", true);
        g.node(interp).params().set("tubeBlend", 0.0);
        g.node(interp).params().set("keepClearance", false);
    }
    StrandsPtr eval() {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(interp, ctx);
    }
};

// How the strands filling a tube sit in its section near the guide's middle.
struct MidFill {
    std::size_t strands = 0;   // in the tube
    float footprint = 0.0f;    // their count over the density: the area they were rooted on
    float section = 0.0f;      // the root section's area
    float occupied = 0.0f;     // share of the cells well inside the outline holding a strand
    float outside = 0.0f;      // the farthest any lies outside the outline, in half-widths
    float thickness = 0.0f;    // the bundle's largest distance off the guide along the section's v axis (scene units)
    float spreadV = 0.0f;      // its largest |v|
};

MidFill measureMidFill(const SlantGraph& f, const StrandSet& out) {
    MidFill m;
    const std::vector<float>* ids = out.strandChannel("tubeId");
    if (!ids) return m;
    const TubeSpine spine(f.guide.data(), f.guide.size(), {0, 0, -1}, f.tube);
    constexpr int kCells = 10;
    std::vector<int> hits(kCells * kCells, 0);
    float aspect = f.tube.aspectAt(0.5f);
    for (std::size_t s = 0; s < out.strandCount(); ++s) {
        if ((*ids)[s] < 0.0f) continue;
        ++m.strands;
        const std::size_t n = out.strandPointCount(s);
        const std::size_t j = (n - 1) / 2;
        const float t = static_cast<float>(j) / static_cast<float>(n - 1);
        const Vec3f c = spine.at(t, 0, 0), au = spine.at(t, 1, 0) - c, av = spine.at(t, 0, 1) - c;
        const Vec3f d = out.strandPoints(s)[j] - c;
        const float u = d.dot(au) / au.dot(au), v = d.dot(av) / av.dot(av);
        m.outside = std::max(m.outside, tubeOutsideDistance(f.tube, aspect, u, v));
        m.thickness = std::max(m.thickness, std::abs(d.dot(av.normalized())));
        m.spreadV = std::max(m.spreadV, std::abs(v));
        const int cu = std::clamp(static_cast<int>((u + 1.0f) * 0.5f * kCells), 0, kCells - 1);
        const int cv = std::clamp(static_cast<int>((v + 1.0f) * 0.5f * kCells), 0, kCells - 1);
        ++hits[cv * kCells + cu];
    }
    int inner = 0, held = 0;
    for (int cv = 0; cv < kCells; ++cv) {
        for (int cu = 0; cu < kCells; ++cu) {
            const float u = -1.0f + (cu + 0.5f) * 2.0f / kCells, v = -1.0f + (cv + 0.5f) * 2.0f / kCells;
            if (tubeShapeDepth(f.tube, u, v) < 0.3f) continue;  // well inside only
            ++inner;
            held += hits[cv * kCells + cu] > 0;
        }
    }
    m.occupied = inner > 0 ? static_cast<float>(held) / static_cast<float>(inner) : 0.0f;
    // The section's area: the outline's (shoelace) times the half-width and half-height.
    const std::vector<SectionPoint>& outline = tubeOutline(f.tube);
    float area = 0.0f;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const SectionPoint& a = outline[i];
        const SectionPoint& b = outline[(i + 1) % outline.size()];
        area += 0.5f * (a.u * b.v - b.u * a.v);
    }
    const TubeSection root = f.tube.sectionAt(0.0f);
    m.section = area * root.width * root.width * root.aspect;
    m.footprint = static_cast<float>(static_cast<double>(m.strands) / f.density);
    return m;
}

// The collapse the user found: a guide leaving the head at a shallow angle had its root section
// standing up off the scalp, so the roots' footprint (taken along the guide onto that section)
// was a thin line and every strand crowded into a band of the section. The footprint is now the
// section laid flat on the scalp: whatever the angle, the strands fill the outline at mid-length,
// none outside it, rooted on an area the section's.
TEST(FillTubes, StrandsFillTheOutlineAtAnyAngle) {
    TubeProfile blade;
    blade.shape = TubeShape::Blade;
    blade.width = {0.1f};
    TubeProfile ribbon;
    ribbon.shape = TubeShape::Ribbon;
    ribbon.width = {0.1f};
    ribbon.aspect = {0.3f};
    for (const TubeProfile& tube : {blade, ribbon}) {
        for (const float degrees : {90.0f, 30.0f, 5.0f}) {
            SlantGraph f(degrees, tube, 20000.0 / tube.aspectAt(0.0f));  // as many strands across a flat tube
            const MidFill m = measureMidFill(f, *f.eval());
            const std::string what = std::string(tubeShapeName(tube.shape)) + " at " + std::to_string(degrees) + " degrees";
            EXPECT_GT(m.strands, 100u) << what;
            EXPECT_NEAR(m.footprint / m.section, 1.0f, 0.2f) << what;
            EXPECT_GT(m.occupied, 0.95f) << what;
            EXPECT_LT(m.outside, 0.03f) << what;
            EXPECT_GT(m.spreadV, 0.8f) << what << ": no collapse into a band";
            EXPECT_LT(m.thickness, 0.1f * tube.aspectAt(0.5f) * 1.03f) << what << ": as thick as the section";
        }
    }
}

// Every shape, round to flattened, holds its strands at mid-length: they fill its outline (in the
// section's frame) and none lies outside it; a flattened one is a flat bundle.
TEST(FillTubes, EveryShapeIsFilledAndRespected) {
    for (std::size_t k = 0; k < tubeShapeNames().size(); ++k) {
        for (const float aspect : {1.0f, 0.4f}) {
            for (const float degrees : {90.0f, 20.0f}) {
                TubeProfile tube;
                tube.shape = shapeAt(k);
                tube.width = {0.1f};
                tube.aspect = {aspect};
                SlantGraph f(degrees, tube, 20000.0 / aspect);
                const MidFill m = measureMidFill(f, *f.eval());
                const std::string what = std::string(tubeShapeName(tube.shape)) + " aspect " + std::to_string(aspect) + " at " +
                                         std::to_string(degrees);
                EXPECT_GT(m.occupied, 0.95f) << what;
                EXPECT_LT(m.outside, 0.03f) << what;
                EXPECT_NEAR(m.footprint / m.section, 1.0f, 0.2f) << what;
                EXPECT_LT(m.thickness, 0.1f * aspect * 1.03f) << what;
                EXPECT_GT(m.thickness, 0.1f * aspect * 0.8f) << what;
            }
        }
    }
}

// Giving a primary's tube a shape lays its frame across the scalp, so at twist 0 the tube lies
// flat on the head (its width along the scalp, its height off it), however the guide lies.
TEST(TubeChildren, AShapeLiesFlatOnTheScalp) {
    const std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    std::vector<Vec3f> lying;
    for (int i = 0; i <= 8; ++i) lying.push_back(Vec3f(0.2f + 0.07f * i, 0.02f + 0.01f * i, 0.5f));
    const GuideId id = h.addGuide(primary, lying);
    h.evaluate(scalp.get());
    TubeProfile ribbon;
    ribbon.shape = TubeShape::Ribbon;
    ribbon.aspect = {0.2f};
    h.setTube(id, ribbon);
    h.evaluate(scalp.get());
    const Frame root = h.guide(id)->frames().front();
    EXPECT_NEAR(std::abs(root.normal.z), 1.0f, 1e-3f) << "u across the guide, along the scalp";
    EXPECT_GT(root.binormal.y, 0.95f) << "v off the scalp";
    // Kept through saving and loading.
    const GuideHierarchy loaded = GuideHierarchy::fromJson(h.toJson());
    EXPECT_NEAR(loaded.guide(id)->frameUp.dot(h.guide(id)->frameUp), 1.0f, 1e-5f);
}


// ---------------------------------------------------------------------------
// The shape library: custom outlines

// A five-pointed star (concave, round its center): its points 1 out, its notches `inner` out.
std::vector<SectionPoint> starPoints(float inner = 0.45f) {
    std::vector<SectionPoint> pts;
    for (int i = 0; i < 10; ++i) {
        const float a = kPi / 2 + kPi * static_cast<float>(i) / 5.0f;
        const float r = i % 2 == 0 ? 1.0f : inner;
        pts.push_back({r * std::cos(a), r * std::sin(a)});
    }
    return pts;
}

float signedArea(const std::vector<SectionPoint>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const SectionPoint& x = p[i];
        const SectionPoint& y = p[(i + 1) % p.size()];
        a += 0.5f * (x.u * y.v - y.u * x.v);
    }
    return a;
}

TEST(TubeShapeLibrary, OutlinesAreCheckedAndFitted) {
    // Given clockwise, off center and small: fitted into the domain, counter-clockwise.
    std::vector<SectionPoint> given = starPoints();
    std::reverse(given.begin(), given.end());
    for (SectionPoint& p : given) p = {0.3f + 0.2f * p.u, -2.0f + 0.2f * p.v};
    const CustomTubeShapePtr star = makeCustomTubeShape("star", given, false);
    ASSERT_TRUE(star);
    EXPECT_EQ(star->points.size(), 10u);
    EXPECT_EQ(star->outline.points.size(), kTubeOutlinePoints);
    EXPECT_GT(signedArea(star->outline.points), 0.0f);
    float lu = 9, hu = -9, lv = 9, hv = -9;
    for (const SectionPoint& q : star->outline.points) lu = std::min(lu, q.u), hu = std::max(hu, q.u), lv = std::min(lv, q.v), hv = std::max(hv, q.v);
    EXPECT_NEAR(lu, -1.0f, 0.02f);
    EXPECT_NEAR(hu, 1.0f, 0.02f);
    EXPECT_NEAR(lv, -1.0f, 0.02f);
    EXPECT_NEAR(hv, 1.0f, 0.02f);
    EXPECT_EQ(star->outline.corners.size(), 10u) << "straight edges: every point a corner";
    TubeProfile tube;
    tube.custom = star;
    EXPECT_TRUE(tube.shaped());
    EXPECT_EQ(tube.shapeName(), "star");
    EXPECT_TRUE(insideTubeShape(tube, 0.0f, 0.0f));
    EXPECT_FALSE(insideTubeShape(tube, 0.75f, 0.75f)) << "between two points of the star";
    for (int k = 0; k < 50; ++k) {
        const SectionPoint q = randomPointInTubeShape(tube, static_cast<std::uint64_t>(k));
        EXPECT_GE(tubeShapeDepth(tube, q.u, q.v), 0.0f);
    }
    // Smooth: a curve through the same points, no corners at a star's gentle notches... but its points.
    const CustomTubeShapePtr round = makeCustomTubeShape("soft", starPoints(0.9f), true);
    EXPECT_TRUE(round->outline.corners.empty());
    // What is refused, saying why.
    auto refused = [](const std::string& name, const std::vector<SectionPoint>& pts, const std::string& says) {
        try {
            makeCustomTubeShape(name, pts, false);
            ADD_FAILURE() << name << ": accepted";
        } catch (const std::invalid_argument& e) {
            EXPECT_NE(std::string(e.what()).find(says), std::string::npos) << e.what();
        }
    };
    refused("bowtie", {{-1, -1}, {1, 1}, {1, -1}, {-1, 1}}, "crosses itself");
    refused("cup", {{-1, 1}, {-1, -1}, {1, -1}, {1, 1}, {0.8f, 1}, {0.8f, -0.8f}, {-0.8f, -0.8f}, {-0.8f, 1}}, "center");
    refused("line", {{-1, 0}, {0, 0}, {1, 0}}, "line");
    refused("two", {{-1, 0}, {1, 0}}, "at least 3");
    refused("round", starPoints(), "built-in");
    refused("bad/name", starPoints(), "letters");
    // The editor's curve: through the points, unfitted.
    const std::vector<SectionPoint> curve = sampleTubeShapeCurve(starPoints(), true);
    EXPECT_GE(curve.size(), 40u);
    EXPECT_NEAR(curve.front().u, starPoints().front().u, 1e-5f);
    // Built-ins as editable points make the same shape closely.
    for (const TubeShape shape : {TubeShape::Round, TubeShape::Ribbon, TubeShape::Teardrop, TubeShape::Blade}) {
        bool smooth = true;
        const CustomTubeShapePtr copy = makeCustomTubeShape("copy", builtinTubeShapePoints(shape, &smooth), smooth);
        TubeProfile own;
        own.custom = copy;
        int agree = 0, total = 0;
        for (int i = 0; i <= 20; ++i) {
            for (int j = 0; j <= 20; ++j) {
                const float u = -0.98f + 0.098f * i, v = -0.98f + 0.098f * j;  // (off the ribbon's flat sides)
                agree += insideTubeShape(shape, u, v) == insideTubeShape(own, u, v);
                ++total;
            }
        }
        EXPECT_GT(static_cast<float>(agree) / total, 0.95f) << tubeShapeName(shape) << ": " << agree << " of " << total;
    }
}

TEST(TubeShapeLibrary, SavedEditedRenamedAndRemoved) {
    TubeHierarchy t(TubeProfile{});  // a parent with 40 children
    GuideHierarchy& h = t.h;
    const GuideId other = h.addGuide(t.primary, straightGuide({0.2f, 0, 0.2f}));
    h.setTubeShape("star", starPoints(), false);
    ASSERT_TRUE(h.tubeShape("star"));
    TubeProfile tube;
    tube.width = {0.2f};
    tube.custom = h.tubeShape("star");
    h.setTube(t.parent, tube);
    h.setTube(other, tube);
    for (const GuideId id : h.guidesInGroup(t.children)) {
        TubeProfile c = h.guide(id)->tube;
        c.custom = h.tubeShape("star");
        h.setTube(id, c);
    }
    h.evaluate(t.scalp.get());
    const std::size_t users = 2 + h.guidesInGroup(t.children).size();
    EXPECT_EQ(h.tubeShapeUsers("star"), users);
    // Saved with the guides and loaded back the same.
    const std::string saved = h.toJson();
    EXPECT_NE(saved.find("\"tubeShapes\""), std::string::npos);
    const GuideHierarchy loaded = GuideHierarchy::fromJson(saved);
    EXPECT_TRUE(loaded.loadWarnings().empty());
    ASSERT_TRUE(loaded.tubeShape("star"));
    EXPECT_EQ(loaded.guide(t.parent)->tube.custom, loaded.tubeShape("star")) << "the library's own entry";
    EXPECT_EQ(loaded.guide(t.parent)->tube, h.guide(t.parent)->tube);
    EXPECT_EQ(loaded.toJson(), saved);
    // A hierarchy without shapes writes no library (its text as before).
    EXPECT_EQ(GuideHierarchy().toJson().find("tubeShapes"), std::string::npos);
    // Editing the shape: every guide taking it takes the new outline, the hash changes.
    const std::uint64_t before = h.contentHash();
    const std::vector<Vec3f> tipBefore = h.guide(other)->points;
    h.setTubeShape("star", starPoints(0.7f), true);
    for (const HierarchyGuide& g : h.guides()) {
        if (g.tube.custom) EXPECT_EQ(g.tube.custom, h.tubeShape("star"));
    }
    EXPECT_NE(h.contentHash(), before);
    EXPECT_TRUE(h.tubeShape("star")->smooth);
    // A bad edit changes nothing.
    EXPECT_THROW(h.setTubeShape("star", {{-1, -1}, {1, 1}, {1, -1}, {-1, 1}}), std::invalid_argument);
    EXPECT_TRUE(h.tubeShape("star")->smooth);
    // Renamed: its guides keep it.
    h.renameTubeShape("star", "spiky");
    EXPECT_FALSE(h.tubeShape("star"));
    EXPECT_EQ(h.tubeShapeUsers("spiky"), users);
    EXPECT_EQ(h.guide(other)->tube.shapeName(), "spiky");
    EXPECT_THROW(h.renameTubeShape("spiky", "blade"), std::invalid_argument);
    // A tube taking a shape the library lacks brings it along (a guide from another groom).
    TubeProfile foreign;
    foreign.custom = makeCustomTubeShape("leaf", builtinTubeShapePoints(TubeShape::Teardrop, nullptr), true);
    h.setTube(other, foreign);
    ASSERT_TRUE(h.tubeShape("leaf"));
    // Removed: its guides go round.
    EXPECT_EQ(h.removeTubeShape("spiky"), users - 1);
    EXPECT_TRUE(h.guide(t.parent)->tube.roundShape());
    EXPECT_THROW(h.removeTubeShape("spiky"), std::invalid_argument);
    // set_tube by name (editedTube with the library).
    const TubeProfile named = editedTube(TubeProfile{}, R"({"shape": "leaf"})", 1.0f, 0, &h.tubeShapes());
    EXPECT_EQ(named.custom, h.tubeShape("leaf"));
    EXPECT_THROW(editedTube(TubeProfile{}, R"({"shape": "nope"})", 1.0f, 0, &h.tubeShapes()), std::invalid_argument);
    EXPECT_NE(tubeToJson(named).find("\"leaf\""), std::string::npos);
}

// A concave custom outline (a star) holds its strands as a built-in does: at mid-length they fill
// it (in the section's frame) and none lies outside it, upright and slanted.
TEST(FillTubes, ACustomConcaveOutlineIsFilled) {
    TubeProfile tube;
    tube.width = {0.12f};
    tube.custom = makeCustomTubeShape("star", starPoints(0.55f), false);
    for (const float degrees : {90.0f, 20.0f}) {
        SlantGraph f(degrees, tube, 30000.0);
        const MidFill m = measureMidFill(f, *f.eval());
        const std::string what = "star at " + std::to_string(degrees) + " degrees";
        EXPECT_GT(m.strands, 100u) << what;
        EXPECT_GT(m.occupied, 0.9f) << what;
        EXPECT_LT(m.outside, 0.03f) << what;
        EXPECT_NEAR(m.footprint / m.section, 1.0f, 0.2f) << what;
    }
}

// The leftover the user found: children of a guide leaving the head at a shallow angle had their
// roots snapped straight down from the root section standing off the scalp, so they bunched on a
// line across the guide. A shaped tube's children now root on its footprint (the section laid on
// the scalp), spread along the guide as across it, the way its strands are; an upright guide's
// are where they were; a lock and unlock leaves a child where it was.
TEST(TubeChildren, ChildrenOfASlantedTubeSpreadOverItsFootprint) {
    auto spreads = [](float degrees, bool shaped, float* along, float* across) {
        const std::shared_ptr<const ScalpMesh> scalp = test::makePlane(16, 2.0f);
        GuideHierarchy h;
        const GroupId primary = h.addGroup("primary");
        const float a = degrees * kPi / 180.0f;
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 16; ++i) pts.push_back(Vec3f(0.6f, 0.0f, 1.0f) + Vec3f(std::cos(a), std::sin(a), 0.0f) * (0.8f * i / 16.0f));
        TubeProfile tube;
        tube.width = {0.15f};
        if (shaped) tube.shape = TubeShape::Blade;
        const GuideId parent = h.addGuide(primary, pts, tube);
        h.evaluate(scalp.get());
        ChildGenerationParams params;
        params.count = 60;
        const GroupId kids = h.generateChildren(primary, "children", params);
        h.evaluate(scalp.get());
        // The roots' spread along the guide (x) and across it (z), as standard deviations.
        double sx = 0, sz = 0, sxx = 0, szz = 0;
        const std::vector<GuideId> ids = h.guidesInGroup(kids);
        for (const GuideId id : ids) {
            const Vec3f r = h.guide(id)->points.front();
            sx += r.x, sz += r.z, sxx += r.x * r.x, szz += r.z * r.z;
        }
        const double n = static_cast<double>(ids.size());
        *along = static_cast<float>(std::sqrt(std::max(sxx / n - (sx / n) * (sx / n), 0.0)));
        *across = static_cast<float>(std::sqrt(std::max(szz / n - (sz / n) * (sz / n), 0.0)));
        // Lock and unlock: no jump (the footprint taken back out of the offsets).
        const GuideId child = ids.front();
        const std::vector<Vec3f> shape = h.guide(child)->points;
        h.lock(child);
        h.unlock(child);
        h.evaluate(scalp.get());
        for (std::size_t i = 0; i < shape.size(); ++i) EXPECT_NEAR((h.guide(child)->points[i] - shape[i]).length(), 0.0f, 2e-3f) << i;
        (void)parent;
    };
    float along = 0, across = 0;
    spreads(10.0f, true, &along, &across);
    std::printf("shaped tube at 10 degrees: children's roots spread %.4f along, %.4f across\n", along, across);
    EXPECT_GT(along, 0.3f * across) << "not on a line";
    float roundAlong = 0, roundAcross = 0;
    spreads(10.0f, false, &roundAlong, &roundAcross);
    std::printf("round tube at 10 degrees (as before): %.4f along, %.4f across\n", roundAlong, roundAcross);
    EXPECT_LT(roundAlong, 0.25f * roundAcross) << "a round tube's children as they always were";
    float upAlong = 0, upAcross = 0;
    spreads(90.0f, true, &upAlong, &upAcross);
    EXPECT_GT(upAlong, 0.2f * upAcross);
}

// The sections roll to lie as the scalp does: a ribbon's height points off the head all along a
// guide curving round it (not only at its root, where its frame starts), and twist turns it from
// there; a guide's tube far off the scalp carries on as it left it.
TEST(TubeSections, ShapedSectionsFollowTheScalp) {
    const std::shared_ptr<const ScalpMesh> scalp = test::makeSphere(48, 96, 1.0f);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    // Spiralling up round it (a curve that twists out of any plane: its rotation-minimizing frames
    // drift off the sphere's normal), just above the surface.
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 48; ++i) {
        const float lon = 5.0f * static_cast<float>(i) / 48.0f;
        const float lat = -1.0f + 2.2f * static_cast<float>(i) / 48.0f;
        pts.push_back(Vec3f(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)) * 1.02f);
    }
    TubeProfile ribbon;
    ribbon.shape = TubeShape::Ribbon;
    ribbon.width = {0.05f};
    ribbon.aspect = {0.3f};
    const GuideId id = h.addGuide(primary, pts, ribbon);
    h.evaluate(scalp.get());
    const HierarchyGuide& g = *h.guide(id);
    ASSERT_FALSE(g.tube.roll.empty());
    auto worst = [&](const TubeProfile& tube) {
        const TubeSpine spine(g.points.data(), g.points.size(), g.frameUp, tube);
        float least = 1.0f;
        for (std::size_t i = 0; i < g.points.size(); ++i) {
            const Vec3f up = (spine.atSample(i, 0, 1) - spine.atSample(i, 0, 0)).normalized();
            least = std::min(least, up.dot(g.points[i].normalized()));
        }
        return least;
    };
    TubeProfile unrolled = g.tube;
    unrolled.roll.clear();
    const float with = worst(g.tube), without = worst(unrolled);
    std::printf("section height against the scalp's normal: worst %.3f rolled, %.3f unrolled\n", with, without);
    EXPECT_GT(with, 0.97f);
    EXPECT_LT(without, 0.9f) << "the frames alone drift (else this measures nothing)";
    // A twist turns it from there.
    TubeProfile twisted = g.tube;
    twisted.twist = {kPi / 2};
    EXPECT_LT(std::abs(worst(twisted)), 0.2f);
    // Not saved, not compared.
    EXPECT_EQ(GuideHierarchy::fromJson(h.toJson()).guide(id)->tube, g.tube);
    EXPECT_EQ(h.toJson().find("roll"), std::string::npos);
    // A round tube does not roll (its children and strands as they always were).
    h.setTube(id, TubeProfile{});
    h.evaluate(scalp.get());
    EXPECT_TRUE(h.guide(id)->tube.roll.empty());
}

// Scatter's guideRadius round a guide with a shaped tube fills the tube's footprint (its outline
// laid on the scalp, plus tubeMargin), not a disc round its root: a wide flat blade, upright and
// lying low, gets roots all over its footprint at the scatter's density, and none far outside. So
// does a sized round tube (it once got the small disc: only shaped tubes counted).
TEST(FillTubes, ScatterGuideRadiusFillsTheFootprint) {
    for (const bool round : {false, true})
    for (const float degrees : {90.0f, 15.0f}) {
        SCOPED_TRACE(std::string(round ? "round" : "blade") + " at " + std::to_string(degrees));
        Graph g;
        ExternalInputs ext;
        const std::shared_ptr<const ScalpMesh> scalp = test::makePlane(16, 2.0f);
        ext.set("scalp", scalp, 1);
        const float a = degrees * kPi / 180.0f;
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 16; ++i) pts.push_back(Vec3f(1.0f, 0.0f, 1.0f) + Vec3f(std::cos(a), std::sin(a), 0.0f) * (0.6f * i / 16.0f));
        TubeProfile blade;
        blade.shape = round ? TubeShape::Round : TubeShape::Blade;
        blade.width = {0.4f};
        blade.aspect = {round ? 1.0f : 0.6f};
        auto set = std::make_shared<GuideSet>();
        Guide& gd = set->add(1, pts);
        gd.hasTube = true;
        gd.tube = blade;
        gd.frameUp = {0, 0, -1};
        set->rootOnMesh(*scalp);
        ext.set("guides", GuidesPtr(set), 1);
        const NodeId scalpNode = g.addNode(op::kScalp);
        const NodeId guides = g.addNode(op::kGuides);
        const NodeId scatter = g.addNode(op::kScatter);
        const double density = 20000.0;
        g.node(scatter).params().set("density", density);
        g.node(scatter).params().set("guideRadius", 0.05);  // a disc this small would hold almost nothing
        g.node(scatter).params().set("tubeMargin", 0.0);
        g.connect(scalpNode, scatter, 0);
        g.connect(guides, scatter, 1);
        EvalContext ctx;
        ctx.externals = &ext;
        const StrandsPtr out = g.evaluateStrands(scatter, ctx);
        TubeSpine spine(pts.data(), pts.size(), gd.frameUp, blade);
        spine.setRootSurface({0, 1, 0});
        const float aspect = spine.rootHeight() / spine.rootWidth();
        constexpr int kCells = 10;
        std::vector<int> hits(kCells * kCells, 0);
        float outside = 0.0f;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const SectionPoint at = spine.rootCoords(out->strandPoints(s)[0]);
            outside = std::max(outside, tubeOutsideDistance(blade, aspect, at.u, at.v));
            const int cu = std::clamp(static_cast<int>((at.u + 1.0f) * 0.5f * kCells), 0, kCells - 1);
            const int cv = std::clamp(static_cast<int>((at.v + 1.0f) * 0.5f * kCells), 0, kCells - 1);
            ++hits[cv * kCells + cu];
        }
        int inner = 0, held = 0;
        for (int cv = 0; cv < kCells; ++cv) {
            for (int cu = 0; cu < kCells; ++cu) {
                const float u = -1.0f + (cu + 0.5f) * 2.0f / kCells, v = -1.0f + (cv + 0.5f) * 2.0f / kCells;
                if (tubeShapeDepth(blade.shape, u, v) < 0.3f) continue;
                ++inner;
                held += hits[cv * kCells + cu] > 0;
            }
        }
        float area = 0.0f;
        const std::vector<SectionPoint>& outline = tubeOutline(blade.shape);
        for (std::size_t i = 0; i < outline.size(); ++i) {
            area += 0.5f * (outline[i].u * outline[(i + 1) % outline.size()].v - outline[(i + 1) % outline.size()].u * outline[i].v);
        }
        const double expected = area * spine.rootWidth() * spine.rootHeight() * density;
        const std::string what = "blade at " + std::to_string(degrees) + " degrees";
        std::printf("%s: %zu roots for %.0f expected, %.0f%% of the footprint covered, farthest %.3f out\n", what.c_str(),
                    out->strandCount(), expected, 100.0f * held / std::max(inner, 1), outside);
        EXPECT_NEAR(static_cast<double>(out->strandCount()) / expected, 1.0, 0.15) << what;
        EXPECT_GT(static_cast<float>(held) / inner, 0.95f) << what;
        EXPECT_LT(outside, 0.03f) << what;
    }
}

// Place and Draw take the placement tube's shape (Groom Tools' Tubes panel: a built-in, or a
// library shape by name), and blend the width, flatness and twist from the two nearest guides;
// with no guides near, the placement's own.
TEST(TubeChildren, PlacedGuidesTakeThePlacementShape) {
    const std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    h.setTubeShape("star", starPoints(), false);
    PlaceSettings s;
    setPlacementTube(s, R"({"shape": "ribbon", "width": 0.05, "aspect": 0.5})");
    EXPECT_EQ(s.tube.shape, TubeShape::Ribbon);
    EXPECT_NE(placementTubeJson(s).find("\"ribbon\""), std::string::npos);
    const GuideId first = placeGuide(h, *scalp, s, scalp->closestPoint({0.3f, 0, 0.3f}));
    EXPECT_EQ(h.guide(first)->tube.shape, TubeShape::Ribbon);
    EXPECT_NEAR(h.guide(first)->tube.widthAt(0.0f), 0.05f, 1e-5f) << "no guides near: the placement's width";
    TubeProfile wide = h.guide(first)->tube;
    wide.shape = TubeShape::Blade;
    wide.width = {0.2f};
    wide.aspect = {0.3f};
    h.setTube(first, wide);
    setPlacementTube(s, R"({"shape": "star"})");
    EXPECT_EQ(s.tubeShape, "star");
    const GuideId second = placeGuide(h, *scalp, s, scalp->closestPoint({0.35f, 0, 0.3f}));
    const TubeProfile& placed = h.guide(second)->tube;
    EXPECT_EQ(placed.custom, h.tubeShape("star")) << "the placement's shape, not the neighbour's blade";
    EXPECT_NEAR(placed.widthAt(0.5f), 0.2f, 1e-4f) << "the neighbour's width";
    EXPECT_NEAR(placed.aspectAt(0.5f), 0.3f, 1e-4f);
    EXPECT_THROW(setPlacementTube(s, R"({"shape": "no/such"})"), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
