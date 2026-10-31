// SPDX-License-Identifier: Apache-2.0
// Flow fields: arrows on the scalp, blended, and the Flow node laying strands along them.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/flow.h"
#include "groomlab/groom.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

FlowSet pins(const ScalpMesh& scalp, std::initializer_list<std::pair<Vec3f, Vec3f>> list) {
    FlowSet set;
    for (const auto& [at, dir] : list) set.pins.push_back(FlowSet::makePin(scalp, scalp.closestPoint(at), dir));
    return set;
}

TEST(Flow, BlendsArrowsOnTheSurface) {
    const auto plane = test::makePlane(8);  // y = 0, x and z in [0, 1]
    const FlowSet set = pins(*plane, {{{0.1f, 0, 0.5f}, {1, 0.5f, 0}}, {{0.9f, 0, 0.5f}, {0, 0, 1}}});
    const FlowField field(set, *plane);
    ASSERT_FALSE(field.empty());
    const Vec3f up{0, 1, 0};
    const Vec3f nearA = field.at({0.15f, 0, 0.5f}, up), nearB = field.at({0.85f, 0, 0.5f}, up);
    EXPECT_GT(nearA.x, 0.95f);   // the first arrow, flattened onto the surface
    EXPECT_NEAR(nearA.y, 0.0f, 1e-5f);
    EXPECT_GT(nearB.z, 0.95f);
    const Vec3f middle = field.at({0.5f, 0, 0.5f}, up);
    EXPECT_NEAR(middle.length(), 1.0f, 1e-4f);
    EXPECT_GT(middle.x, 0.3f);   // in between: both
    EXPECT_GT(middle.z, 0.3f);
    EXPECT_EQ(FlowField(FlowSet{}, *plane).at({0.5f, 0, 0.5f}, up).length(), 0.0f);
    EXPECT_THROW(FlowSet::makePin(*plane, plane->closestPoint({0.5f, 0, 0.5f}), up), std::invalid_argument);

    const FlowSet again = FlowSet::fromJson(set.toJson());
    ASSERT_EQ(again.pins.size(), 2u);
    EXPECT_EQ(again.contentHash(), set.contentHash());
    EXPECT_TRUE(FlowSet::fromJson("").empty());
}

TEST(Flow, TheFlowNodeLaysGuidelessFurAlongTheArrows) {
    const auto plane = test::makePlane(8);
    GroomData d;
    d.scalp = plane;
    d.flowJson = pins(*plane, {{{0.5f, 0, 0.5f}, {1, 0, 0}}}).toJson();
    // No guides anywhere: Scatter grows short fur, Flow lays it down.
    d.graphJson = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 300.0, "length": 0.2}},
        {"name": "flow", "type": "Flow", "params": {"lift": 30.0, "bend": 0.0, "rollRandom": 0.0}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 5}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "resolution", "input": "strands"},
                      {"from": "resolution", "to": "flow", "input": "strands"},
                      {"from": "flow", "to": "out", "input": "strands"}]})";
    Groom groom(d);
    const StrandsPtr s = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(s && s->strandCount() > 50);
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f* p = s->strandPoints(i);
        const std::size_t n = s->strandPointCount(i);
        const Vec3f heading = (p[n - 1] - p[0]).normalized();
        EXPECT_NEAR(std::asin(heading.y) * 57.2958f, 30.0f, 0.5f) << i;  // lift off the skin
        EXPECT_NEAR(heading.z, 0.0f, 1e-3f);                              // along the arrow
        EXPECT_GT(heading.x, 0.8f);
        EXPECT_NEAR(arcLength(p, n), 0.2f, 1e-3f);                        // lengths kept
    }
}

TEST(Flow, StrandsCurveAsTheArrowsTurn) {
    // Arrows along +x on the left, turning to +z from the middle on: long, low strands from
    // the left follow them round, bending smoothly rather than heading straight on.
    const auto plane = test::makePlane(16);
    GroomData d;
    d.scalp = plane;
    d.flowJson = pins(*plane, {{{0.1f, 0, 0.3f}, {1, 0, 0}}, {{0.55f, 0, 0.3f}, {0, 0, 1}},
                               {{0.55f, 0, 0.7f}, {0, 0, 1}}, {{0.9f, 0, 0.5f}, {0, 0, 1}}}).toJson();
    d.graphJson = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0, "length": 0.5}},
        {"name": "flow", "type": "Flow", "params": {"lift": 2.0, "bend": 0.0, "rollRandom": 0.0}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 24}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "resolution", "input": "strands"},
                      {"from": "resolution", "to": "flow", "input": "strands"},
                      {"from": "flow", "to": "out", "input": "strands"}]})";
    const StrandsPtr s = Groom(d).evaluate(1.0f, 0.0);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f* p = s->strandPoints(i);
        const std::size_t n = s->strandPointCount(i);
        if (p[0].x > 0.15f || std::abs(p[0].z - 0.3f) > 0.05f) continue;  // roots by the first arrow
        ++checked;
        const Vec3f first = (p[1] - p[0]).normalized(), last = (p[n - 1] - p[n - 2]).normalized();
        EXPECT_GT(first.x, 0.9f) << i;  // leaves along the first arrow
        EXPECT_GT(last.z, 0.5f) << i;   // and turns with the others
        float sharpest = 0.0f;
        for (std::size_t k = 1; k + 1 < n; ++k) {
            const Vec3f a = (p[k] - p[k - 1]).normalized(), b = (p[k + 1] - p[k]).normalized();
            sharpest = std::max(sharpest, std::acos(std::clamp(a.dot(b), -1.0f, 1.0f)) * 57.2958f);
        }
        EXPECT_LT(sharpest, 20.0f) << i;  // a curve, not a kink
        EXPECT_NEAR(arcLength(p, n), 0.5f, 0.01f);
    }
    EXPECT_GT(checked, 3u);
}

TEST(Flow, StrandsWrapAroundACurvedScalp) {
    // Arrows down the sides of a sphere; long strands laid low follow its curve instead of
    // leaving on a straight tangent.
    const auto sphere = test::makeSphere(24, 48, 1.0f);
    GroomData d;
    d.scalp = sphere;
    FlowSet set;
    for (int k = 0; k < 12; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / 12.0f;
        const Vec3f at{0.7f * std::cos(a), 0.714f, 0.7f * std::sin(a)};
        set.pins.push_back(FlowSet::makePin(*sphere, sphere->closestPoint(at), Vec3f{0, -1, 0}));
    }
    d.flowJson = set.toJson();
    d.graphJson = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 20.0, "length": 0.8}},
        {"name": "flow", "type": "Flow", "params": {"lift": 5.0, "bend": 1.0, "rollRandom": 0.0}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 16}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "resolution", "input": "strands"},
                      {"from": "resolution", "to": "flow", "input": "strands"},
                      {"from": "flow", "to": "out", "input": "strands"}]})";
    const StrandsPtr s = Groom(d).evaluate(1.0f, 0.0);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f* p = s->strandPoints(i);
        const std::size_t n = s->strandPointCount(i);
        if (p[0].y < 0.3f) continue;  // the upper part, under the arrows
        ++checked;
        float farthest = 0.0f;
        for (std::size_t k = 0; k < n; ++k) farthest = std::max(farthest, p[k].length());
        EXPECT_LT(farthest, 1.06f) << i;  // hugs the sphere (a straight tangent 0.8 long reaches 1.28)
        EXPECT_LT(p[n - 1].y, p[0].y - 0.2f) << i;  // down the side, with the arrows
    }
    EXPECT_GT(checked, 10u);
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(Flow, PinsRememberTheirStroke) {
    FlowSet set;
    FlowPin a;
    a.root.face = 0;
    a.local = Vec3f(1, 0, 0);
    a.line = 2;
    a.seq = 5;
    FlowPin lone;
    lone.root.face = 1;
    lone.local = Vec3f(0, 1, 0);
    set.pins = {a, lone};
    const FlowSet back = FlowSet::fromJson(set.toJson());
    ASSERT_EQ(back.pins.size(), 2u);
    EXPECT_EQ(back.pins[0].line, 2);
    EXPECT_EQ(back.pins[0].seq, 5);
    EXPECT_EQ(back.pins[1].line, -1);  // a pin on its own (and older flow data)
    EXPECT_NE(set.contentHash(), [&] { FlowSet other = set; other.pins[0].seq = 6; return other.contentHash(); }());
    const FlowSet old = FlowSet::fromJson(R"({"version": 1, "pins": [{"face": 0, "u": 0.2, "v": 0.2, "dir": [1, 0, 0]}]})");
    EXPECT_EQ(old.pins[0].line, -1);
}

TEST(Flow, SinksTurnTheFieldAroundTheirCenter) {
    const auto plane = test::makePlane(8);  // y = 0, normal +y
    FlowSet set = pins(*plane, {{{0.1f, 0, 0.1f}, {0, 0, 1}}, {{0.9f, 0, 0.9f}, {0, 0, 1}}});
    FlowSink sink;
    sink.root = plane->closestPoint({0.5f, 0, 0.5f}).root;
    sink.angle = flowSinkAngle("converge");
    sink.radius = 0.3f;
    set.sinks.push_back(sink);
    const Vec3f up{0, 1, 0};
    {
        const FlowField field(set, *plane);
        EXPECT_LT(field.at({0.55f, 0, 0.5f}, up).x, -0.95f);  // near the center: gathers in
        EXPECT_GT(field.at({0.5f, 0, 0.95f}, up).z, 0.95f);   // past the radius: the pins
    }
    set.sinks[0].angle = 90.0f;  // circles counter-clockwise seen from above (+y)
    {
        const FlowField field(set, *plane);
        EXPECT_LT(field.at({0.55f, 0, 0.5f}, up).z, -0.95f);  // +y x +x = -z
        EXPECT_GT(field.at({0.45f, 0, 0.5f}, up).z, 0.95f);
    }
    // Sinks alone make a field.
    FlowSet only;
    only.sinks = set.sinks;
    EXPECT_FALSE(only.empty());
    EXPECT_FALSE(FlowField(only, *plane).empty());

    const FlowSet back = FlowSet::fromJson(set.toJson());
    ASSERT_EQ(back.sinks.size(), 1u);
    EXPECT_FLOAT_EQ(back.sinks[0].angle, 90.0f);
    EXPECT_FLOAT_EQ(back.sinks[0].radius, 0.3f);
    EXPECT_EQ(back.contentHash(), set.contentHash());
    EXPECT_NE(set.contentHash(), [&] { FlowSet other = set; other.sinks[0].radius = 0.4f; return other.contentHash(); }());
    EXPECT_THROW(flowSinkAngle("spiral"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// The shaping brushes on arrows and sinks (applyBrushStampToFlow / StrokeToFlow)

BrushSettings flowBrush(BrushMode mode, float radius, float strength = 1.0f) {
    BrushSettings b;
    b.mode = mode;
    b.radius = radius;
    b.strength = strength;
    return b;
}

Vec3f pinAt(const FlowSet& set, const ScalpMesh& scalp, std::size_t i) { return scalp.position(set.pins[i].root); }
Vec3f pinDir(const FlowSet& set, const ScalpMesh& scalp, std::size_t i) {
    FlowSet one;
    one.pins.push_back(set.pins[i]);
    return FlowField(one, scalp).directions().at(0);
}

// Three pins of one stroke line along x at z = 0.5, the middle one pointing +z, the others +x.
FlowSet lineOfPins(const ScalpMesh& plane) {
    FlowSet set = pins(plane, {{{0.2f, 0, 0.5f}, {1, 0, 0}}, {{0.3f, 0, 0.5f}, {0, 0, 1}}, {{0.8f, 0, 0.5f}, {1, 0, 0}}});
    for (std::size_t i = 0; i < 3; ++i) {
        set.pins[i].line = 0;
        set.pins[i].seq = static_cast<int>(i);
    }
    return set;
}

TEST(FlowBrush, CombedStrokeLinesPointAlongTheirOwnCurve) {
    // Five pins of one stroke line along +x. A comb stroke pushes the middle ones toward +z:
    // the line bends, and each pin then points along the bent line (into the bend before its
    // apex, out of it after), not along the brush, which went straight +z.
    const auto plane = test::makePlane(8);
    auto laid = [&]() {
        FlowSet set = pins(*plane, {{{0.3f, 0, 0.4f}, {1, 0, 0}}, {{0.4f, 0, 0.4f}, {1, 0, 0}}, {{0.5f, 0, 0.4f}, {1, 0, 0}},
                                    {{0.6f, 0, 0.4f}, {1, 0, 0}}, {{0.7f, 0, 0.4f}, {1, 0, 0}}});
        for (std::size_t i = 0; i < set.pins.size(); ++i) set.pins[i].line = 0, set.pins[i].seq = static_cast<int>(i);
        return set;
    };
    FlowSet set = laid();
    EXPECT_GT(applyBrushStrokeToFlow(set, *plane, flowBrush(BrushMode::Comb, 0.12f), {{0.5f, 0, 0.4f}, {0.5f, 0, 0.55f}}), 0u);
    ASSERT_EQ(set.pins.size(), 5u);
    EXPECT_GT(pinAt(set, *plane, 2).z, pinAt(set, *plane, 0).z + 0.05f);  // the middle moved with the brush
    EXPECT_NEAR(pinAt(set, *plane, 0).z, 0.4f, 1e-3f);                    // the ends did not
    for (std::size_t i = 0; i < 5; ++i) EXPECT_GT(pinDir(set, *plane, i).x, 0.5f) << i;  // still running along +x
    EXPECT_GT(pinDir(set, *plane, 1).z, 0.1f);   // climbing into the bend
    EXPECT_LT(pinDir(set, *plane, 3).z, -0.1f);  // coming back out of it
    EXPECT_NEAR(pinDir(set, *plane, 2).z, 0.0f, 0.2f);  // level at the apex
}

TEST(FlowBrush, CombedStrokeLineSkipsPinsOffTheScalp) {
    // A pin bound to another mesh (a face this one does not have): combing its stroke line
    // realigns the pins on the scalp and leaves it be, instead of reading a face that is not there.
    const auto plane = test::makePlane(8);
    FlowSet set = lineOfPins(*plane);
    set.pins[2].root.face = static_cast<std::int32_t>(plane->triangleCount() + 100);
    const StrandRoot stale = set.pins[2].root;
    std::size_t changed = 0;
    EXPECT_NO_THROW(changed = applyBrushStrokeToFlow(set, *plane, flowBrush(BrushMode::Comb, 0.15f),
                                                     {{0.25f, 0, 0.5f}, {0.25f, 0, 0.6f}}));
    EXPECT_GT(changed, 0u);
    EXPECT_EQ(set.pins[2].root.face, stale.face);
    EXPECT_GT(pinAt(set, *plane, 0).z, 0.5f);  // the pins on the scalp moved
}

TEST(FlowBrush, CombSlidesPinsAndSinksAndLaysArrowsAlongTheStroke) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.3f, 0, 0.5f}, {0, 0, 1}}});
    set.pins[0].line = 3;
    set.pins[0].seq = 2;
    FlowSink sink;
    sink.root = plane->closestPoint({0.3f, 0, 0.5f}).root;
    sink.radius = 0.2f;
    set.sinks.push_back(sink);
    // Stroke toward +x: the pin and the sink ride along with it, the arrow turned along the stroke.
    EXPECT_EQ(applyBrushStrokeToFlow(set, *plane, flowBrush(BrushMode::Comb, 0.3f), {{0.3f, 0, 0.5f}, {0.5f, 0, 0.5f}}), 2u);
    ASSERT_EQ(set.pins.size(), 1u);
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.5f, 1e-3f);
    EXPECT_NEAR(pinAt(set, *plane, 0).z, 0.5f, 1e-3f);
    EXPECT_GT(pinDir(set, *plane, 0).x, 0.99f);
    EXPECT_EQ(set.pins[0].line, 3);  // still its stroke's
    EXPECT_EQ(set.pins[0].seq, 2);
    EXPECT_NEAR(plane->position(set.sinks[0].root).x, 0.5f, 1e-3f);
    EXPECT_FLOAT_EQ(set.sinks[0].radius, 0.2f);

    // A still stamp (no drag) does nothing; a pin out of reach is left alone.
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Comb, 0.3f), {0.5f, 0, 0.5f}, {}), 0u);
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Comb, 0.1f), {0.9f, 0, 0.9f}, {0.1f, 0, 0}), 0u);
}

TEST(FlowBrush, CombedPinsStayOnACurvedScalpWithTangentArrows) {
    const auto sphere = test::makeSphere(30, 60, 1.0f);
    const SurfacePoint top = sphere->closestPoint({0.2f, 1.0f, 0.0f});
    FlowSet set;
    set.pins.push_back(FlowSet::makePin(*sphere, top, {1, 0, 0}));
    const Vec3f start = sphere->position(set.pins[0].root);
    // A straight world-space drag would leave the sphere: the pin is re-bound to it.
    EXPECT_EQ(applyBrushStrokeToFlow(set, *sphere, flowBrush(BrushMode::Comb, 0.5f), {start, start + Vec3f{0.4f, 0, 0}}), 1u);
    const Vec3f at = pinAt(set, *sphere, 0);
    EXPECT_NEAR(at.length(), 1.0f, 0.01f);
    EXPECT_GT(at.x, start.x + 0.2f);
    const Vec3f dir = pinDir(set, *sphere, 0);
    EXPECT_NEAR(dir.dot(sphere->normal(set.pins[0].root).normalized()), 0.0f, 1e-4f);  // tangent where it is now
    EXPECT_GT(dir.x, 0.7f);  // along the stroke, as far as the tangent plane allows
}

TEST(FlowBrush, TwistTurnsArrowsAndSinkHeadings) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.5f, 0, 0.5f}, {1, 0, 0}}});
    FlowSink sink;
    sink.root = plane->closestPoint({0.5f, 0, 0.5f}).root;
    set.sinks.push_back(sink);
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Twist, 0.2f), {0.5f, 0, 0.5f}, {}), 2u);
    // 0.1 radians about +y at strength 1: +x turns toward -z; the sink's angle turns the same in degrees.
    const Vec3f dir = pinDir(set, *plane, 0);
    EXPECT_NEAR(dir.x, std::cos(0.1f), 1e-4f);
    EXPECT_NEAR(dir.z, -std::sin(0.1f), 1e-4f);
    EXPECT_NEAR(set.sinks[0].angle, 70.0f + 0.1f * 57.29578f, 1e-3f);
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.5f, 1e-5f);  // in place
    applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Twist, 0.2f, -1.0f), {0.5f, 0, 0.5f}, {});
    EXPECT_NEAR(pinDir(set, *plane, 0).z, 0.0f, 1e-4f);  // and back
}

TEST(FlowBrush, LengthGrowsAndShrinksSinksOnly) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.5f, 0, 0.5f}, {1, 0, 0}}});
    FlowSink sink;
    sink.root = plane->closestPoint({0.5f, 0, 0.5f}).root;
    sink.radius = 1.0f;
    set.sinks.push_back(sink);
    const std::uint64_t pinHash = [&] { FlowSet p; p.pins = set.pins; return p.contentHash(); }();
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Length, 0.2f), {0.5f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(set.sinks[0].radius, 1.05f, 1e-5f);
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Length, 0.2f, -1.0f), {0.5f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(set.sinks[0].radius, 1.05f * 0.95f, 1e-5f);
    EXPECT_EQ([&] { FlowSet p; p.pins = set.pins; return p.contentHash(); }(), pinHash);  // pins untouched
}

TEST(FlowBrush, SmoothEvensALineAndBlendsDirections) {
    const auto plane = test::makePlane(8);
    FlowSet set = lineOfPins(*plane);
    // Only the middle pin is under the brush: it slides toward the midpoint of
    // its neighbours (x 0.5) and turns toward their +x.
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Smooth, 0.08f), {0.3f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(pinAt(set, *plane, 1).x, 0.4f, 1e-3f);  // half way, at strength 1
    const Vec3f dir = pinDir(set, *plane, 1);
    EXPECT_NEAR(dir.x, 0.7071f, 1e-3f);
    EXPECT_NEAR(dir.z, 0.7071f, 1e-3f);
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.2f, 1e-5f);  // the ends stay
    EXPECT_NEAR(pinAt(set, *plane, 2).x, 0.8f, 1e-5f);
}

TEST(FlowBrush, StraightenTurnsArrowsAlongTheirLine) {
    const auto plane = test::makePlane(8);
    FlowSet set = lineOfPins(*plane);
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Straighten, 0.08f), {0.3f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(pinDir(set, *plane, 1).x, 0.7071f, 1e-3f);  // toward the line's +x, half way
    EXPECT_NEAR(pinAt(set, *plane, 1).x, 0.3f, 1e-5f);      // in place
    // A pin on its own has no line to straighten to.
    FlowSet lone = pins(*plane, {{{0.3f, 0, 0.5f}, {0, 0, 1}}});
    EXPECT_EQ(applyBrushStampToFlow(lone, *plane, flowBrush(BrushMode::Straighten, 0.08f), {0.3f, 0, 0.5f}, {}), 0u);
}

TEST(FlowBrush, AttractPullsPinsToTheCenterAndPushesThemAway) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.3f, 0, 0.5f}, {0, 0, 1}}});
    EXPECT_EQ(applyBrushStampToFlow(set, *plane, flowBrush(BrushMode::Attract, 0.5f), {0.5f, 0, 0.5f}, {}), 1u);
    const float w = (1.0f - 0.16f) * (1.0f - 0.16f);  // falloff at 0.4 of the radius
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.3f + 0.2f * 0.1f * w, 1e-4f);
    EXPECT_GT(pinDir(set, *plane, 0).z, 0.99f);
    FlowSet pushed = pins(*plane, {{{0.3f, 0, 0.5f}, {0, 0, 1}}});
    applyBrushStampToFlow(pushed, *plane, flowBrush(BrushMode::Attract, 0.5f, -1.0f), {0.5f, 0, 0.5f}, {});
    EXPECT_LT(pinAt(pushed, *plane, 0).x, 0.3f - 0.01f);
}

TEST(FlowBrush, EraseRemovesWhatIsUnderTheBrush) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.2f, 0, 0.5f}, {1, 0, 0}}, {{0.8f, 0, 0.5f}, {1, 0, 0}}});
    FlowSink sink;
    sink.root = plane->closestPoint({0.2f, 0, 0.5f}).root;
    set.sinks.push_back(sink);
    EXPECT_EQ(applyBrushStrokeToFlow(set, *plane, flowBrush(BrushMode::Erase, 0.2f), {{0.15f, 0, 0.5f}, {0.25f, 0, 0.5f}}), 2u);
    ASSERT_EQ(set.pins.size(), 1u);
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.8f, 1e-5f);
    EXPECT_TRUE(set.sinks.empty());
}

TEST(FlowBrush, SymmetryMirrorsTheStampAndItsDrag) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.3f, 0, 0.5f}, {0, 0, 1}}, {{0.7f, 0, 0.5f}, {0, 0, 1}}});
    BrushSettings b = flowBrush(BrushMode::Comb, 0.15f);
    b.mirrorAxis = 0;
    b.mirrorCenter = 0.5f;
    EXPECT_EQ(applyBrushStrokeToFlow(set, *plane, b, {{0.3f, 0, 0.5f}, {0.4f, 0, 0.5f}}), 2u);
    EXPECT_NEAR(pinAt(set, *plane, 0).x, 0.4f, 1e-3f);
    EXPECT_NEAR(pinAt(set, *plane, 1).x, 0.6f, 1e-3f);  // the mirror image slid the other way
    EXPECT_GT(pinDir(set, *plane, 0).x, 0.99f);
    EXPECT_LT(pinDir(set, *plane, 1).x, -0.99f);
    // A pin on the seam changes once.
    FlowSet seam = pins(*plane, {{{0.5f, 0, 0.5f}, {0, 0, 1}}});
    EXPECT_EQ(applyBrushStampToFlow(seam, *plane, flowBrush(BrushMode::Twist, 0.2f), {0.5f, 0, 0.5f}, {}), 1u);
    b.mode = BrushMode::Twist;
    FlowSet seam2 = pins(*plane, {{{0.5f, 0, 0.5f}, {0, 0, 1}}});
    EXPECT_EQ(applyBrushStampToFlow(seam2, *plane, b, {0.5f, 0, 0.5f}, {}), 1u);
    EXPECT_EQ(seam.contentHash(), seam2.contentHash());
}

TEST(FlowBrush, OtherBrushesLeaveArrowsAlone) {
    const auto plane = test::makePlane(8);
    FlowSet set = pins(*plane, {{{0.5f, 0, 0.5f}, {1, 0, 0}}});
    FlowSink sink;
    sink.root = plane->closestPoint({0.5f, 0, 0.5f}).root;
    set.sinks.push_back(sink);
    const std::uint64_t before = set.contentHash();
    for (const BrushMode mode : {BrushMode::Lift, BrushMode::ShapeTube, BrushMode::Attribute,
                                 BrushMode::Slide, BrushMode::Cut}) {
        EXPECT_EQ(applyBrushStrokeToFlow(set, *plane, flowBrush(mode, 0.3f), {{0.4f, 0, 0.5f}, {0.6f, 0, 0.5f}}), 0u)
            << brushModeName(mode);
    }
    EXPECT_EQ(set.contentHash(), before);
}

}  // namespace
}  // namespace groomlab
