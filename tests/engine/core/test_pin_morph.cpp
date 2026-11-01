// SPDX-License-Identifier: Apache-2.0
// Morphing a mesh to photos with pins: targets from one photo (depth kept) and from two
// (triangulated), the radial basis warp (pins exact, the rest smooth, a rigid move kept rigid).

#include <cmath>

#include <gtest/gtest.h>

#include "groomlab/pin_morph.h"

namespace groomlab {
namespace {

constexpr double kW = 1000.0, kH = 1000.0, kF = 1200.0;

double distance(const CameraVec3& a, const CameraVec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// A camera at `eye` looking at the origin, Y up (camera_solve.h's model: it looks down -Z).
PinView view(const CameraVec3& eye) {
    CameraVec3 back = eye;
    const double len = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    for (double& c : back) c /= len;
    CameraVec3 right{back[2], 0.0, -back[0]};
    const double rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    for (double& c : right) c /= rl;
    const CameraVec3 up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                        back[0] * right[1] - back[1] * right[0]};
    PinView v;
    for (int i = 0; i < 3; ++i) {
        v.pose.rotation[i][0] = right[i];
        v.pose.rotation[i][1] = up[i];
        v.pose.rotation[i][2] = back[i];
    }
    v.pose.position = eye;
    v.focalPx = kF;
    v.width = kW;
    v.height = kH;
    return v;
}

PinSight sightOf(const PinView& v, int index, const CameraVec3& world) {
    double u = 0.0, vv = 0.0;
    EXPECT_TRUE(projectToImage(v.pose, v.focalPx, v.width, v.height, world, u, vv));
    return {index, u, vv};
}

TEST(PinMorph, OnePhotoSlidesAcrossTheViewAtItsDepth) {
    const std::vector<PinView> views{view({0, 0, 10})};
    MorphPin pin;
    pin.mesh = {0.0, 0.0, 1.0};
    pin.sights.push_back(sightOf(views[0], 0, {0.5, 0.2, 1.0}));  // the feature sits up and right, same depth
    CameraVec3 target{};
    ASSERT_TRUE(pinTarget(pin, views, target));
    EXPECT_NEAR(distance(target, {0.5, 0.2, 1.0}), 0.0, 1e-9);
}

TEST(PinMorph, TwoPhotosTriangulateTheDepth) {
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    const CameraVec3 feature{0.3, -0.4, 0.8};
    MorphPin pin;
    pin.mesh = {0.0, 0.0, 0.0};
    pin.sights = {sightOf(views[0], 0, feature), sightOf(views[1], 1, feature)};
    CameraVec3 target{};
    ASSERT_TRUE(pinTarget(pin, views, target));
    EXPECT_NEAR(distance(target, feature), 0.0, 1e-7);
}

TEST(PinMorph, PinsLandAndTheRestFollowsSmoothly) {
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    // A cube's corners as pins, each pushed out by its own amount; a grid of points between.
    std::vector<MorphPin> pins;
    std::vector<CameraVec3> wanted;
    for (int i = 0; i < 8; ++i) {
        const CameraVec3 p{(i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0};
        const double grow = 1.0 + 0.05 * i;
        const CameraVec3 to{p[0] * grow, p[1] * grow, p[2] * grow};
        MorphPin pin;
        pin.mesh = p;
        pin.sights = {sightOf(views[0], 0, to), sightOf(views[1], 1, to)};
        pins.push_back(pin);
        wanted.push_back(to);
    }
    std::vector<CameraVec3> points;
    for (const MorphPin& p : pins) points.push_back(p.mesh);
    points.push_back({0.0, 0.0, 0.0});
    points.push_back({0.5, 0.5, 0.5});
    const PinMorphResult r = morphToPins(points, pins, views);
    ASSERT_TRUE(r.ok) << r.error;
    for (std::size_t i = 0; i < pins.size(); ++i) EXPECT_NEAR(distance(r.points[i], wanted[i]), 0.0, 1e-6) << i;
    EXPECT_GT(r.rmsBeforePx, 10.0);
    EXPECT_LT(r.rmsAfterPx, 1e-3);
    EXPECT_LT(distance(r.points[8], {0, 0, 0}), 0.2);  // the middle barely moves: the pins grow round it
    EXPECT_GT(r.points[9][0], 0.5);  // a point toward a corner goes out with it
}

TEST(PinMorph, ARigidMoveOfEveryPinMovesTheMeshRigidly) {
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    const CameraVec3 shift{0.2, -0.1, 0.3};
    std::vector<MorphPin> pins;
    for (const CameraVec3& p : std::vector<CameraVec3>{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, -1, 0}, {0, -1, -1}}) {
        MorphPin pin;
        pin.mesh = p;
        const CameraVec3 to{p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]};
        pin.sights = {sightOf(views[0], 0, to), sightOf(views[1], 1, to)};
        pins.push_back(pin);
    }
    const CameraVec3 far{3.0, 2.0, -1.0};
    const PinMorphResult r = morphToPins({far}, pins, views);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NEAR(distance(r.points[0], {far[0] + shift[0], far[1] + shift[1], far[2] + shift[2]}), 0.0, 1e-6);
}

TEST(PinMorph, SymmetryMirrorsPinsAcrossTheMiddle) {
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    // Pins on the right side (x > 0) only, pushed outward; one on the middle line pushed sideways.
    std::vector<MorphPin> pins;
    auto pinTo = [&](const CameraVec3& from, const CameraVec3& to) {
        MorphPin pin;
        pin.mesh = from;
        pin.sights = {sightOf(views[0], 0, to), sightOf(views[1], 1, to)};
        pins.push_back(pin);
    };
    pinTo({1.0, 0.0, 0.0}, {1.2, 0.0, 0.0});
    pinTo({0.7, 0.7, 0.0}, {0.8, 0.8, 0.0});
    pinTo({0.7, -0.7, 0.3}, {0.75, -0.75, 0.35});
    pinTo({0.0, 0.0, 1.0}, {0.1, 0.0, 1.1});  // on the middle: it stays there
    PinMorphOptions sym;
    sym.symmetric = true;
    const std::vector<CameraVec3> points{{-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {-0.7, 0.7, 0.0}, {0.7, 0.7, 0.0}};
    const PinMorphResult r = morphToPins(points, pins, views, sym);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NEAR(r.points[0][0], -r.points[1][0], 1e-6);  // the left moves as the right, mirrored
    EXPECT_NEAR(r.points[0][1], r.points[1][1], 1e-6);
    EXPECT_NEAR(r.points[2][0], -r.points[3][0], 1e-6);
    EXPECT_NEAR(r.points[0][0], -1.2, 1e-6);  // the mirror of a pin lands as the pin does
    const PinMorphResult plain = morphToPins(points, pins, views);
    ASSERT_TRUE(plain.ok);
    EXPECT_GT(std::abs(plain.points[0][0] + 1.2), 0.05);  // without it the left side does not follow
}

TEST(PinMorph, HighStiffnessKeepsTheShape) {
    // Pins pulled every which way: a stiff morph moves the mesh nearly as one piece (its points'
    // distances kept), never stretching it to chase them.
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    std::vector<MorphPin> pins;
    const std::vector<std::pair<CameraVec3, CameraVec3>> pulls = {
        {{1, 0, 0}, {1.3, 0.1, 0}}, {{-1, 0, 0}, {-0.8, -0.2, 0.1}}, {{0, 1, 0}, {0.2, 1.4, 0}},
        {{0, -1, 0}, {0.1, -0.7, -0.2}}, {{0, 0, 1}, {0, 0.3, 1.2}}};
    for (const auto& [from, to] : pulls) {
        MorphPin pin;
        pin.mesh = from;
        pin.sights = {sightOf(views[0], 0, to), sightOf(views[1], 1, to)};
        pins.push_back(pin);
    }
    const std::vector<CameraVec3> points{{2, 0, 0}, {-2, 0, 0}, {0, 2, 0}, {0, 0, -2}};
    PinMorphOptions stiff;
    stiff.stiffness = 0.9;
    const PinMorphResult r = morphToPins(points, pins, views, stiff);
    ASSERT_TRUE(r.ok) << r.error;
    for (std::size_t i = 0; i < points.size(); ++i)
        for (std::size_t j = i + 1; j < points.size(); ++j)
            EXPECT_NEAR(distance(r.points[i], r.points[j]), distance(points[i], points[j]), 0.15 * distance(points[i], points[j]))
                << i << " " << j;
}

TEST(PinMorph, EveryStiffnessStaysSmooth) {
    // From 0 to 0.95 the morph never moves a point further than the pins move (the solve stays well
    // posed), and the pins fall ever shorter as it stiffens.
    const std::vector<PinView> views{view({0, 0, 10}), view({10, 0, 0})};
    std::vector<MorphPin> pins;
    double largest = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double a = i * 0.5236, b = (i % 3 - 1) * 0.6;
        const CameraVec3 p{std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)};
        const CameraVec3 to{p[0] * (1.0 + 0.04 * (i % 4)), p[1] + 0.03 * (i % 3), p[2] * (1.0 - 0.03 * (i % 2))};
        largest = std::max(largest, distance(p, to));
        MorphPin pin;
        pin.mesh = p;
        pin.sights = {sightOf(views[0], 0, to), sightOf(views[1], 1, to)};
        pins.push_back(pin);
    }
    std::vector<CameraVec3> points;
    for (int i = 0; i < 200; ++i) {
        const double a = i * 0.7, b = std::sin(i * 1.3) * 1.2;
        points.push_back({std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)});
    }
    double lastError = -1.0;
    for (double s = 0.0; s <= 0.951; s += 0.05) {
        PinMorphOptions o;
        o.stiffness = s;
        const PinMorphResult r = morphToPins(points, pins, views, o);
        ASSERT_TRUE(r.ok) << s;
        for (std::size_t i = 0; i < points.size(); ++i) EXPECT_LT(distance(r.points[i], points[i]), 2.0 * largest) << s << " " << i;
        EXPECT_GE(r.rmsAfterPx, lastError - 1e-6) << s;
        lastError = r.rmsAfterPx;
    }
}

TEST(PinMorph, StiffnessEasesTheFitAndBadPinsSaySo) {
    const std::vector<PinView> views{view({0, 0, 10})};
    std::vector<MorphPin> pins;
    for (const CameraVec3& p : std::vector<CameraVec3>{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0.5}}) {
        MorphPin pin;
        pin.mesh = p;
        pin.sights = {sightOf(views[0], 0, {p[0] * 1.2, p[1] * 1.2, p[2]})};
        pins.push_back(pin);
    }
    std::vector<CameraVec3> points;
    for (const MorphPin& p : pins) points.push_back(p.mesh);
    PinMorphOptions soft;
    soft.stiffness = 0.6;
    const PinMorphResult exact = morphToPins(points, pins, views), eased = morphToPins(points, pins, views, soft);
    ASSERT_TRUE(exact.ok && eased.ok);
    EXPECT_LT(exact.rmsAfterPx, 1e-3);
    EXPECT_GE(eased.rmsAfterPx, exact.rmsAfterPx);
    MorphPin behind;
    behind.mesh = {0, 0, 20};  // behind the camera
    behind.sights = {{0, 500.0, 500.0}};
    const PinMorphResult bad = morphToPins(points, {behind}, views);
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.error.empty());
    EXPECT_FALSE(morphToPins(points, {}, views).ok);
}

}  // namespace
}  // namespace groomlab
