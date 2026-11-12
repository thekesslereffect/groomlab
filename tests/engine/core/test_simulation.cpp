// SPDX-License-Identifier: Apache-2.0
// The guide simulation (simulation.h): the solver keeps lengths, hangs, stays out of the scalp,
// is the same on every run and any thread count; the session's cache (scrubbing back reads it, a
// jump steps on from the last frame, a param or groom change drops it, Simulate Range fills it);
// bypass gives the groom exactly; and the cost per frame at thousands of guides.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "groomlab/simulation.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr double kFps = 24.0;

// Unit-scale settings (the scenes here are a unit sphere's): gravity in units per second squared.
SimSettings unitSettings() {
    SimSettings s;
    s.gravity = 9.8f;
    s.collisionOffset = 0.02f;
    s.groomedUnderGravity = false;  // gravity on the groomed shape itself: what these tests measure
    return s;
}

// Guides standing on the sphere's upper half along its normals, `points` points over `length`.
GuidesPtr guidesOnSphere(const ScalpMesh& sphere, std::size_t count, int points, float length) {
    auto guides = std::make_shared<GuideSet>();
    std::size_t made = 0;
    for (std::size_t f = 0; f < sphere.triangleCount() && made < count; f += 7) {
        const StrandRoot root{static_cast<std::int32_t>(f), 0.3f, 0.3f};
        const Vec3f at = sphere.position(root), n = sphere.normal(root);
        if (n.y < 0.2f) continue;
        std::vector<Vec3f> pts;
        for (int k = 0; k < points; ++k) pts.push_back(at + n * (length * static_cast<float>(k) / static_cast<float>(points - 1)));
        Guide& g = guides->add(static_cast<std::uint64_t>(made + 1), std::move(pts));
        g.root = root;
        ++made;
    }
    return guides;
}

float segment(const std::vector<Vec3f>& p, std::size_t j) { return (p[j] - p[j - 1]).length(); }

TEST(Simulation, SegmentsKeepTheirLengthUnderGravityAndMotion) {
    // A row of horizontal guides with no scalp: they swing down under gravity.
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < 12; ++i) {
        guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve({0.0f, 0.0f, 0.1f * i}, {1.0f, 0.0f, 0.0f}, 20));
    }
    SimSettings s = unitSettings();
    s.bend = 0.2f;
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, nullptr, {}}, 1);
    for (int f = 0; f < 48; ++f) ASSERT_TRUE(cache.step({guides, nullptr, {}}, 1));
    const GuidesPtr frame = cache.frame(cache.lastFrame());
    ASSERT_TRUE(frame);
    float worst = 0.0f;
    bool swung = false;
    for (std::size_t g = 0; g < frame->size(); ++g) {
        const std::vector<Vec3f>& p = frame->guides()[g].points;
        EXPECT_EQ(p.front(), guides->guides()[g].points.front()) << "the root moved";
        for (std::size_t j = 1; j < p.size(); ++j) worst = std::max(worst, std::abs(segment(p, j) - 0.05f) / 0.05f);
        swung = swung || p.back().y < -0.5f;
    }
    EXPECT_LT(worst, 1e-3f) << "segments stretch by up to " << worst * 100.0f << "%";
    EXPECT_TRUE(swung) << "gravity did not pull the guides down";
}

TEST(Simulation, APinnedStrandHangsStraightDown) {
    // No bending stiffness, nothing holding it to its groomed shape: it hangs below its root.
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 20));
    SimSettings s = unitSettings();
    s.bend = 0.0f;
    s.twist = 0.0f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    s.damping = 0.3f;
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, nullptr, {}}, 1);
    for (int f = 0; f < 240; ++f) ASSERT_TRUE(cache.step({guides, nullptr, {}}, 1));
    const std::vector<Vec3f>& p = cache.frame(cache.lastFrame())->guides()[0].points;
    for (std::size_t j = 0; j < p.size(); ++j) {
        const float below = 0.05f * static_cast<float>(j);
        EXPECT_NEAR(p[j].x, 0.0f, 0.01f) << "point " << j;
        EXPECT_NEAR(p[j].y, -below, 0.01f) << "point " << j;
        EXPECT_NEAR(p[j].z, 0.0f, 0.01f) << "point " << j;
    }
}

TEST(Simulation, BendStiffnessHoldsTheGroomedShape) {
    // The same strand with full bend stiffness and its root held: it sags far less.
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}, 10));
    auto tipAfter = [&](float bend) {
        SimSettings s = unitSettings();
        s.bend = bend;
        s.rootStiffnessProfile = Ramp::constant(0.0f);
        SimCache cache;
        cache.configure(s, kFps);
        cache.start({guides, nullptr, {}}, 1);
        for (int f = 0; f < 72; ++f) cache.step({guides, nullptr, {}}, 1);
        return cache.frame(cache.lastFrame())->guides()[0].points.back();
    };
    const Vec3f stiff = tipAfter(1.0f), soft = tipAfter(0.0f);
    EXPECT_GT(stiff.y, -0.05f) << "a stiff short strand keeps (nearly) its shape";
    EXPECT_LT(soft.y, -0.15f) << "a soft one hangs";
}

TEST(Simulation, CollisionKeepsPointsOutOfTheScalpByTheOffset) {
    const auto sphere = test::makeSphere(24, 48);
    const GuidesPtr guides = guidesOnSphere(*sphere, 60, 16, 1.2f);
    SimSettings s = unitSettings();
    s.bend = 0.3f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, sphere, {}}, 1);
    for (int f = 0; f < 60; ++f) ASSERT_TRUE(cache.step({guides, sphere, {}}, 1));
    const GuidesPtr frame = cache.frame(cache.lastFrame());
    float lowest = 1e9f;
    bool resting = false;
    for (const Guide& g : frame->guides()) {
        for (std::size_t j = 1; j < g.points.size(); ++j) {
            const SurfacePoint sp = sphere->closestPoint(g.points[j]);
            const float height = (g.points[j] - sp.position).dot(sp.normal);
            lowest = std::min(lowest, height);
            resting = resting || height < 0.05f;
        }
    }
    EXPECT_GE(lowest, s.collisionOffset * 0.75f) << "a point sank to " << lowest << " above the scalp";
    EXPECT_TRUE(resting) << "nothing came to rest on the scalp: the test shows nothing";
}

TEST(Simulation, ColliderMeshesPushTheGuidesAside) {
    // A ball under the hanging guides: they drape over it.
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < 6; ++i) guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve({0.05f * i, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 12));
    const auto unit = test::makeSphere(16, 32, 0.5f);
    std::vector<Vec3f> moved = unit->points();
    for (Vec3f& p : moved) p += Vec3f{0.1f, 1.2f, 0.0f};
    const MeshPtr ball = std::make_shared<const ScalpMesh>(std::move(moved), unit->triangles());
    SimSettings s = unitSettings();
    s.bend = 0.0f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, nullptr, {ball}}, 1);
    for (int f = 0; f < 48; ++f) cache.step({guides, nullptr, {ball}}, 1);
    for (const Guide& g : cache.frame(cache.lastFrame())->guides()) {
        for (const Vec3f& p : g.points) EXPECT_GE((p - Vec3f{0.1f, 1.2f, 0.0f}).length(), 0.5f) << "inside the ball";
    }
}

std::vector<Vec3f> simulatePoints(const SimSettings& s, const GuidesPtr& guides, const MeshPtr& scalp, int frames) {
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, scalp, {}}, 1);
    for (int f = 0; f < frames; ++f) cache.step({guides, scalp, {}}, 1);
    std::vector<Vec3f> out;
    for (const Guide& g : cache.frame(cache.lastFrame())->guides()) out.insert(out.end(), g.points.begin(), g.points.end());
    return out;
}

TEST(Simulation, TwoRunsAreBitIdenticalOnAnyThreadCount) {
    const auto sphere = test::makeSphere(16, 32);
    const GuidesPtr guides = guidesOnSphere(*sphere, 200, 12, 0.8f);
    SimSettings s = unitSettings();
    s.windSpeed = 2.0f;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.05f;
    s.volume = 0.5f;
    s.volumeCellSize = 0.1f;
    const std::vector<Vec3f> a = simulatePoints(s, guides, sphere, 20);
    const std::vector<Vec3f> b = simulatePoints(s, guides, sphere, 20);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3f))) << "two runs differ";
#if GROOMLAB_HAS_TBB
    std::vector<Vec3f> serial;
    tbb::task_arena single(1);
    single.execute([&] { serial = simulatePoints(s, guides, sphere, 20); });
    EXPECT_EQ(0, std::memcmp(a.data(), serial.data(), a.size() * sizeof(Vec3f))) << "one thread and many differ";
#endif
}

TEST(Simulation, HairVolumeIsTheWorldsGrid) {
    // Hair volume's grid is the world's: the guides on a sphere move the same with a far copy of
    // them beside it (out of reach of each other, the two together spanning millions of cells, of
    // which only those by the points are held), to the bit, as alone.
    const auto sphere = test::makeSphere(16, 32);
    const GuidesPtr guides = guidesOnSphere(*sphere, 120, 12, 0.8f);
    auto both = std::make_shared<GuideSet>(*guides);
    for (const Guide& g : guides->guides()) {
        std::vector<Vec3f> pts = g.points;
        for (Vec3f& p : pts) p += Vec3f{1000.0f, 0.0f, 0.0f};
        both->add(g.id + 100000, std::move(pts));  // (no root on the scalp: its own first segment's frame)
    }
    SimSettings s = unitSettings();
    s.collideScalp = false;
    s.volume = 0.8f;
    s.volumeCellSize = 0.05f;
    const std::vector<Vec3f> alone = simulatePoints(s, guides, sphere, 12);
    const std::vector<Vec3f> beside = simulatePoints(s, both, sphere, 12);
    ASSERT_EQ(beside.size(), 2 * alone.size());
    EXPECT_EQ(0, std::memcmp(alone.data(), beside.data(), alone.size() * sizeof(Vec3f)));
    s.volume = 0.0f;
    EXPECT_NE(simulatePoints(s, guides, sphere, 12), alone);  // (the volume did push them)
}

// FNV-1a over the points' bits.
std::uint64_t bitsHash(const std::vector<Vec3f>& points) {
    std::uint64_t h = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(points.data());
    for (std::size_t i = 0; i < points.size() * sizeof(Vec3f); ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

TEST(Simulation, DryDefaultsAreTheSolverBeforeWater) {
    // The settings stamps the solver made before water existed (its params at their defaults:
    // water off, no rain), to the bit: old bakes still match. The frames are the dry solver's to
    // the bit (made again when the orientations stopped keeping a spin and pushed points kept the
    // leader's correction, so hair comes to rest; and again when the bend constraint's correction
    // became the exact turn and the lengths were kept again after collisions; and again for the
    // core's portable math, the same bits on every platform; and again when each guide's rod was
    // solved whole, so hair ends stopped flipping).
    const auto sphere = test::makeSphere(16, 32);
    const GuidesPtr guides = guidesOnSphere(*sphere, 200, 12, 0.8f);
    SimSettings s = unitSettings();
    s.windSpeed = 2.0f;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.05f;
    if constexpr (test::kRecordedMath) {  // (the frames' bits follow sin and exp: portable_math.h)
        EXPECT_EQ(bitsHash(simulatePoints(s, guides, sphere, 20)), 0xcf93dfcbd12007d9ull);
        EXPECT_EQ(bitsHash(simulatePoints(unitSettings(), guides, sphere, 20)), 0x448a7ce0b65a1e96ull);
    }
    EXPECT_EQ(SimSettings{}.hash(), 0x8eacb7b84342c479ull);  // groomedUnderGravity preload by default since 0.39.0
    EXPECT_EQ(s.hash(), 0xa179909d7d9cee95ull);
    // Water params set while water is off change nothing, the stamp included.
    SimSettings off = s;
    off.waterLevel = 3.0f;
    off.buoyancy = 2.0f;
    off.currentSpeed = 1.0f;
    EXPECT_EQ(off.hash(), s.hash());
    EXPECT_TRUE(SimSettings::fromParams(ParamSet{}).hash() == SimSettings{}.hash());
}

// ---- Water ----------------------------------------------------------------------------------

// Water over everything (the surface at y = 10) with `buoyancy`, no root hold nor bend.
SimSettings underwater(float buoyancy) {
    SimSettings s = unitSettings();
    s.water = true;
    s.waterLevel = 10.0f;
    s.buoyancy = buoyancy;
    s.bend = 0.0f;
    s.twist = 0.0f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    return s;
}

// One guide's frames: the guide's points after `frames` frames, and the cache (wetness).
std::vector<Vec3f> stepGuide(SimCache& cache, const SimSettings& s, const GuidesPtr& guides, int frames) {
    cache.configure(s, kFps);
    cache.start({guides, nullptr, {}}, 1);
    for (int f = 0; f < frames; ++f) cache.step({guides, nullptr, {}}, 1);
    return cache.frame(cache.lastFrame())->guides()[0].points;
}

TEST(Water, UnderwaterHairRisesWithBuoyancy) {
    // A guide hanging from its root, a little off the vertical: buoyancy above gravity's pull floats
    // it up past its root (slowly: the water drags), less than that leaves it hanging.
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, Vec3f{0.3f, -1.0f, 0.0f}.normalized(), 10));
    SimCache floating, sinking;
    const std::vector<Vec3f> up = stepGuide(floating, underwater(2.0f), guides, 240);
    const std::vector<Vec3f> down = stepGuide(sinking, underwater(0.5f), guides, 240);
    // (The tip rises to 0.79, 0.84 before each guide was solved whole: the exact solve gives the
    // orientations' resistance to each substep's turn in full, slowing heavily damped limp hair a
    // little. Still clearly floated up, from 0.95 below its root.)
    EXPECT_GT(up.back().y, 0.75f) << "the tip did not float up";
    EXPECT_LT(down.back().y, -0.8f) << "less than gravity's pull: it still hangs";
    for (std::size_t j = 1; j < up.size(); ++j) EXPECT_NEAR(segment(up, j), 0.1f, 1e-3f);  // lengths kept
}

TEST(Water, WaterDragSlowsTheSwingAgainstAir) {
    // A horizontal guide let go: with gravity unchanged under water (no buoyancy), the water's
    // drag slows its fall to far less than the air lets it fall.
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10));
    SimSettings air = underwater(0.0f), water = underwater(0.0f);
    air.water = false;
    SimCache a, w;
    const float inAir = stepGuide(a, air, guides, 6).back().y, inWater = stepGuide(w, water, guides, 6).back().y;
    EXPECT_LT(inAir, -0.2f) << "the air should let it swing down";
    EXPECT_GT(inWater, inAir * 0.5f) << "in water " << inWater << ", in air " << inAir;
    EXPECT_LT(inWater, -1e-3f) << "it still sinks";
}

TEST(Water, TheCurrentCarriesTheHairDownstream) {
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10));
    SimSettings s = underwater(1.0f);  // weightless
    s.currentSpeed = 2.0f;
    s.currentDirection = {0.0f, 0.0f, 1.0f};
    SimCache cache;
    const Vec3f tip = stepGuide(cache, s, guides, 72).back();
    EXPECT_GT(tip.z, 0.6f) << "the tip did not stream along the current";
}

TEST(Water, CrossingTheSurfaceIsSmooth) {
    // Horizontal guides at heights in steps through the surface, let go from rest: how far each
    // tip moves in one frame changes smoothly with its height (the water fades in over a band, 5%
    // of the guides' length), never jumping between neighbours as at a hard surface.
    auto guides = std::make_shared<GuideSet>();
    constexpr int kSteps = 100;
    for (int k = 0; k <= kSteps; ++k) {
        const float y = 0.002f * static_cast<float>(k - kSteps / 2);  // +-0.1 about the surface at 0 (+-2 bands)
        guides->add(static_cast<std::uint64_t>(k + 1), test::straightCurve({0.0f, y, 0.0f}, {1.0f, 0.0f, 0.0f}, 10));
    }
    SimSettings s = underwater(2.0f);  // under: up as hard as gravity pulls down above
    s.waterLevel = 0.0f;
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, nullptr, {}}, 1);
    cache.step({guides, nullptr, {}}, 1);
    const GuidesPtr frame = cache.frame(cache.lastFrame());
    std::vector<float> moved;
    for (std::size_t g = 0; g < frame->size(); ++g) moved.push_back(frame->guides()[g].points.back().y - guides->guides()[g].points.back().y);
    const float range = *std::max_element(moved.begin(), moved.end()) - *std::min_element(moved.begin(), moved.end());
    EXPECT_GT(range, 0.01f) << "above and below should move apart";
    float jump = 0.0f;
    for (std::size_t k = 1; k < moved.size(); ++k) jump = std::max(jump, std::abs(moved[k] - moved[k - 1]));
    EXPECT_LT(jump, 0.1f * range) << "a jump of " << jump << " of " << range << " between neighbouring heights";
}

TEST(Water, HairSoaksUnderWaterAndDriesOverDryTime) {
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10));
    SimSettings s = underwater(0.5f);
    s.dryTime = 1.0f;
    SimCache cache;
    stepGuide(cache, s, guides, 12);  // half a second under water
    auto wetness = [&] {
        const GuidesPtr f = cache.frame(cache.lastFrame());
        const auto it = f->guides()[0].pointAttributes.find(kWetnessChannel);
        EXPECT_TRUE(it != f->guides()[0].pointAttributes.end());
        float mean = 0.0f;
        for (const float w : it->second) mean += w / static_cast<float>(it->second.size());
        return mean;
    };
    const float soaked = wetness();
    EXPECT_GT(soaked, 0.8f);
    // Lifted far out of the water: a second dries it to about 1/e of that, three to nearly dry.
    auto lifted = std::make_shared<GuideSet>();
    lifted->add(1, test::straightCurve({0.0f, 20.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10));
    for (int f = 0; f < 24; ++f) cache.step({lifted, nullptr, {}}, 1);
    EXPECT_NEAR(wetness(), soaked / 2.71828f, 0.08f);
    for (int f = 0; f < 48; ++f) cache.step({lifted, nullptr, {}}, 1);
    EXPECT_LT(wetness(), 0.1f);
    // Rain alone wets it, out of any water.
    SimSettings rain = unitSettings();
    rain.rain = 1.0f;
    SimCache raining;
    stepGuide(raining, rain, lifted, 48);
    const GuidesPtr f = raining.frame(raining.lastFrame());
    EXPECT_GT(f->guides()[0].pointAttributes.at(kWetnessChannel).back(), 0.9f);
    // Dry, the frames carry no wetness at all.
    SimCache dry;
    stepGuide(dry, unitSettings(), guides, 2);
    EXPECT_TRUE(dry.frame(dry.lastFrame())->guides()[0].pointAttributes.empty());
}

TEST(Water, WetHairOutOfTheWaterHangsHeavierAndLimper) {
    // A short stiff guide soaked and lifted out sags more than the same guide dry.
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f}, 10));
    auto sag = [&](bool wet) {
        SimSettings s = unitSettings();
        s.bend = 0.9f;
        s.rootStiffnessProfile = Ramp::constant(0.0f);
        s.rain = wet ? 1.0f : 0.0f;
        s.wetWeight = 1.0f;
        SimCache cache;
        return stepGuide(cache, s, guides, 72).back().y;
    };
    EXPECT_LT(sag(true), sag(false) - 0.005f);
}

TEST(Water, RunsAreBitIdenticalOnAnyThreadCountAndAWaterMeshWorksAsThePlane) {
    const auto sphere = test::makeSphere(16, 32);
    const GuidesPtr guides = guidesOnSphere(*sphere, 200, 12, 0.8f);
    SimSettings s = unitSettings();
    s.water = true;
    s.waterLevel = 0.6f;  // the upper guides' tips poke out
    s.buoyancy = 1.3f;
    s.currentSpeed = 1.0f;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.05f;
    const std::vector<Vec3f> a = simulatePoints(s, guides, sphere, 20);
    EXPECT_EQ(bitsHash(a), bitsHash(simulatePoints(s, guides, sphere, 20))) << "two runs differ";
#if GROOMLAB_HAS_TBB
    std::vector<Vec3f> serial;
    tbb::task_arena single(1);
    single.execute([&] { serial = simulatePoints(s, guides, sphere, 20); });
    EXPECT_EQ(bitsHash(a), bitsHash(serial)) << "one thread and many differ";
#endif
    // A big flat water mesh at the same height floats the guides as the plane does.
    std::vector<Vec3f> points = test::makePlane(4, 40.0f)->points();
    for (Vec3f& p : points) p += Vec3f{-20.0f, 0.6f, -20.0f};
    const MeshPtr plane = std::make_shared<const ScalpMesh>(std::move(points), test::makePlane(4, 40.0f)->triangles());
    ASSERT_GT(plane->normal({0, 0.3f, 0.3f}).y, 0.99f) << "the plane faces up";
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, sphere, {}, plane}, 1);
    for (int f = 0; f < 20; ++f) cache.step({guides, sphere, {}, plane}, 1);
    std::vector<Vec3f> meshed;
    for (const Guide& g : cache.frame(cache.lastFrame())->guides()) meshed.insert(meshed.end(), g.points.begin(), g.points.end());
    float most = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) most = std::max(most, (a[i] - meshed[i]).length());
    EXPECT_LT(most, 1e-3f);
}

TEST(Water, EveryWaterParamChangesTheStamp) {
    SimSettings base;
    base.water = true;
    std::set<std::uint64_t> stamps{base.hash()};
    auto with = [&](const std::function<void(SimSettings&)>& set) {
        SimSettings s = base;
        set(s);
        EXPECT_TRUE(stamps.insert(s.hash()).second) << "a water param left the stamp alone";
    };
    with([](SimSettings& s) { s.water = false; s.rain = 0.5f; });
    with([](SimSettings& s) { s.waterLevel = 1.0f; });
    with([](SimSettings& s) { s.buoyancy = 1.0f; });
    with([](SimSettings& s) { s.waterDrag = 0.2f; });
    with([](SimSettings& s) { s.currentSpeed = 1.0f; });
    with([](SimSettings& s) { s.currentDirection = {0.0f, 0.0f, 1.0f}; });
    with([](SimSettings& s) { s.dryTime = 3.0f; });
    with([](SimSettings& s) { s.wetWeight = 0.1f; });
    with([](SimSettings& s) { s.rain = 0.3f; });
}

// ---- Through the session --------------------------------------------------------------------

const char* kSimGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "startFrame": 1}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 120.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "sim", "input": "guides"},
    {"from": "sim", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// The graph with the Simulate node's params replaced by `params` (JSON object text).
std::string simGraph(const std::string& params) {
    std::string g = kSimGraph;
    const std::string was = R"({"gravity": 9.8, "collisionOffset": 0.02, "startFrame": 1})";
    g.replace(g.find(was), was.size(), params);
    return g;
}

// The same groom without the Simulate node (guides straight into Interpolate).
std::string unsimulatedGraph() {
    std::string g = kSimGraph;
    const std::string node = R"(    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "startFrame": 1}},
)";
    g.erase(g.find(node), node.size());
    const std::string in = R"(    {"from": "guides", "to": "sim", "input": "guides"},
)";
    g.erase(g.find(in), in.size());
    const std::string out = R"({"from": "sim", "to": "interpolate", "input": "guides"})";
    g.replace(g.find(out), out.size(), R"({"from": "guides", "to": "interpolate", "input": "guides"})");
    return g;
}

std::string hierarchyOn(const ScalpMesh& scalp, float lean = 0.0f) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 10; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 10.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{lean, 0.0f, 0.2f}, 8), tube);
    }
    h.evaluate(&scalp);
    return h.toJson();
}

GroomData simData(const std::string& graph = kSimGraph) {
    GroomData d;
    d.graphJson = graph;
    d.scalp = test::makeSphere(12, 24);
    d.hierarchyJson = hierarchyOn(*d.scalp);
    return d;
}

StrandsPtr viewAt(GroomSession& session, int frame) {
    ViewRequest request;
    request.time = frame / kFps;
    const ViewResult& view = session.evaluate(request);
    EXPECT_TRUE(view.errors.empty()) << (view.errors.empty() ? "" : view.errors.front());
    return view.strands;
}

void expectSameStrands(const StrandSet& a, const StrandSet& b, const std::string& what) {
    ASSERT_EQ(a.ids(), b.ids()) << what;
    ASSERT_EQ(a.positions(), b.positions()) << what;
}

TEST(SimulateNode, GuidesSavedOnAnotherTopologyFollowAMovingScalp) {
    // A groom saved with its guides bound to another mesh (the scalp re-meshed or subdivided
    // since): every frame re-rooted them at the points nearest their saved shapes, so on a moving
    // scalp the guides, and the Simulate node's targets with them, stayed where they were groomed
    // (a wolf's 32 guides slid to its tail as it ran off). Bound again once on the scalp's rest
    // pose, they follow it. (strength 0: the Simulate node gives its input guides.)
    GroomData data = simData(simGraph(R"({"gravity": 9.8, "strength": 0.0, "startFrame": 1})"));
    data.hierarchyJson = hierarchyOn(*test::makeSphere(8, 16));  // bound on other triangles
    auto scalp = std::make_shared<ScalpMesh>(*data.scalp);
    scalp->setRestPoints(scalp->points());
    data.scalp = scalp;
    GroomSession session(data);
    viewAt(session, 1);
    const GuidesPtr atRest = session.simulatedFrame("sim", 1);
    ASSERT_TRUE(atRest);
    std::vector<Vec3f> moved = scalp->points();
    for (Vec3f& p : moved) p += Vec3f{3.0f, 0.0f, 0.0f};
    session.updateScalpPoints(moved);
    viewAt(session, 2);
    const GuidesPtr after = session.simulatedFrame("sim", 2);
    ASSERT_TRUE(after);
    ASSERT_EQ(after->size(), atRest->size());
    float worst = 0.0f;
    for (std::size_t g = 0; g < atRest->size(); ++g) {
        const auto& a = atRest->guides()[g].points;
        const auto& b = after->guides()[g].points;
        ASSERT_EQ(a.size(), b.size());
        for (std::size_t j = 0; j < a.size(); ++j) worst = std::max(worst, (b[j] - (a[j] + Vec3f{3.0f, 0.0f, 0.0f})).length());
    }
    EXPECT_LT(worst, 1e-3f) << "the guides did not move with the scalp";
}

TEST(SimulateNode, MovesTheGuidesTheStrandsFollow) {
    GroomSession session(simData());
    GroomSession still(simData(unsimulatedGraph()));
    const StrandsPtr atStart = viewAt(session, 1);
    expectSameStrands(*atStart, *viewAt(still, 1), "the start frame is the groom at rest");
    const StrandsPtr later = viewAt(session, 12);
    EXPECT_NE(later->positions(), viewAt(still, 12)->positions()) << "the simulation moved nothing";
    const std::vector<SimStatus> status = session.simulationStatus();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_EQ(status[0].node, "sim");
    ASSERT_EQ(status[0].ranges.size(), 1u);
    EXPECT_EQ(status[0].ranges[0], std::make_pair(1, 12));
    EXPECT_TRUE(session.readsTime());
}

TEST(SimulateNode, ScrubbingBackGivesTheFirstPassExactly) {
    GroomSession session(simData());
    std::vector<std::vector<Vec3f>> first;
    for (int f = 1; f <= 16; ++f) first.push_back(viewAt(session, f)->positions());
    const std::size_t evaluations = session.evaluationCount();
    for (int f = 16; f >= 1; --f) {
        EXPECT_EQ(viewAt(session, f)->positions(), first[static_cast<std::size_t>(f - 1)]) << "frame " << f;
    }
    // Scrubbing back steps nothing: only the graph after the Simulate node runs again.
    EXPECT_EQ(session.simulationStatus()[0].frames, 16u);
    EXPECT_GT(session.evaluationCount(), evaluations);
}

TEST(SimulateNode, AJumpForwardStepsOnFromTheLastFrameAsPlayingDoes) {
    GroomSession played(simData()), jumped(simData());
    played.setSimulationFeed([](double) { return true; });  // the inputs are the same at every time
    jumped.setSimulationFeed([](double) { return true; });
    for (int f = 1; f <= 6; ++f) viewAt(played, f);
    viewAt(jumped, 6);
    for (int f = 7; f <= 20; ++f) viewAt(played, f);
    const StrandsPtr a = viewAt(played, 20), b = viewAt(jumped, 20);
    expectSameStrands(*a, *b, "a jump from frame 6 to 20");
    EXPECT_EQ(jumped.simulationStatus()[0].ranges[0], std::make_pair(1, 20));
}

TEST(SimulateNode, AParamChangeDropsTheFramesFromTheStartFrame) {
    GroomSession session(simData(simGraph(R"({"gravity": 9.8, "collisionOffset": 0.02, "cacheMode": "cached"})")));
    const SimRun run = session.simulateTo(10);
    EXPECT_TRUE(run.errors.empty());
    EXPECT_EQ(run.frames, 10);
    EXPECT_EQ(session.simulationStatus()[0].frames, 10u);
    const StrandsPtr simulated = viewAt(session, 10);
    session.setGraphJson(simGraph(R"({"gravity": 9.8, "collisionOffset": 0.02, "cacheMode": "cached", "damping": 0.4})"));
    const StrandsPtr after = viewAt(session, 10);
    EXPECT_EQ(session.simulationStatus()[0].frames, 0u) << "the frames of the old settings stayed";
    GroomSession still(simData(unsimulatedGraph()));
    expectSameStrands(*after, *viewAt(still, 10), "cached mode without frames passes the guides through");
    EXPECT_NE(simulated->positions(), after->positions());
}

TEST(SimulateNode, AGroomEditDropsTheFrames) {
    GroomData data = simData();
    GroomSession session(data);
    viewAt(session, 8);
    ASSERT_EQ(session.simulationStatus()[0].frames, 8u);
    session.setHierarchyJson(hierarchyOn(*data.scalp, 0.3f));
    viewAt(session, 8);
    // Started again from frame 1 on the new guides: a fresh session's frames.
    GroomData edited = data;
    edited.hierarchyJson = hierarchyOn(*data.scalp, 0.3f);
    GroomSession fresh(edited);
    expectSameStrands(*viewAt(session, 8), *viewAt(fresh, 8), "after a groom edit");
}

TEST(SimulateNode, AGroomEditShownAtASubframeDropsTheFramesToo) {
    // Edited while a subframe shows (a frame it kept has no inputs at that time to compare): the
    // same time again with other inputs starts it again, so frames after are a fresh session's,
    // not stepped on from frames simulated before the edit.
    GroomData data = simData();
    GroomSession session(data);
    auto at = [](GroomSession& s, double frame) {
        ViewRequest request;
        request.time = frame / kFps;
        const ViewResult& view = s.evaluate(request);
        EXPECT_TRUE(view.errors.empty());
        return view.strands;
    };
    at(session, 6.5);
    session.setHierarchyJson(hierarchyOn(*data.scalp, 0.3f));
    at(session, 6.5);
    GroomData edited = data;
    edited.hierarchyJson = hierarchyOn(*data.scalp, 0.3f);
    GroomSession fresh(edited);
    expectSameStrands(*at(session, 6.5), *at(fresh, 6.5), "the subframe after the edit");
    expectSameStrands(*viewAt(session, 10), *viewAt(fresh, 10), "a frame after the edit");
}

TEST(SimulateNode, CachedModeStepsOnlyInSimulateRange) {
    GroomSession session(simData(simGraph(R"({"gravity": 9.8, "cacheMode": "cached"})")));
    viewAt(session, 12);
    EXPECT_EQ(session.simulationStatus()[0].frames, 0u) << "a view stepped a cached-mode node";
    int calls = 0;
    const SimRun run = session.simulateTo(24, [&](int done, int count) {
        ++calls;
        EXPECT_EQ(count, 24);
        return done < 12;  // stop half way
    });
    EXPECT_TRUE(run.cancelled);
    EXPECT_EQ(calls, 12);
    EXPECT_EQ(session.simulationStatus()[0].ranges[0], std::make_pair(1, 12));
    EXPECT_FALSE(session.simulationStatus()[0].live);
    session.invalidateSimulation();
    EXPECT_EQ(session.simulationStatus()[0].frames, 0u);
}

TEST(SimulateNode, AGroomReadsEarlierFramesUntilABakeCoversThem) {
    GroomSession stepped(simData());
    for (int f = 1; f <= 12; ++f) viewAt(stepped, f);
    EXPECT_TRUE(stepped.readsEarlierFrames({}, 1, 12)) << "a live Simulate node without a bake";
    EXPECT_FALSE(stepped.readsEarlierFrames({}, -10, 0)) << "frames before its start frame";
    // A bake of frames 1-12 made with the node's settings.
    GroomSession session(simData());
    auto baked = std::make_shared<BakedSimulation>();
    baked->read = [&stepped](double seconds) { return stepped.simulatedFrame("sim", static_cast<int>(std::lround(seconds * kFps))); };
    baked->firstSeconds = 1 / kFps;
    baked->lastSeconds = 12 / kFps;
    baked->stamp = session.simulationStamp("sim");
    session.setBakedSimulation("sim", baked);
    EXPECT_FALSE(session.readsEarlierFrames({}, 1, 12));
    EXPECT_TRUE(session.readsEarlierFrames({}, 1, 20)) << "frames past the bake step";
    session.setGraphJson(simGraph(R"({"gravity": 9.8, "collisionOffset": 0.02, "startFrame": 1, "damping": 0.4})"));
    EXPECT_TRUE(session.readsEarlierFrames({}, 1, 12)) << "a bake of other settings";
    GroomSession still(simData(unsimulatedGraph()));
    EXPECT_FALSE(still.readsEarlierFrames({}, 1, 100));
}

TEST(SimulateNode, WithSteppingOffAViewShowsTheFramesItHasOnly) {
    GroomSession stepped(simData());
    for (int f = 1; f <= 12; ++f) viewAt(stepped, f);
    GroomSession background(simData());
    background.setSimulationStepping(false);
    GroomSession still(simData(unsimulatedGraph()));
    expectSameStrands(*viewAt(background, 12), *viewAt(still, 12), "nothing stepped: the guides as groomed");
    EXPECT_EQ(background.simulationStatus()[0].frames, 0u);
    // Its bake's frames, in any order: what stepping showed.
    auto baked = std::make_shared<BakedSimulation>();
    baked->read = [&stepped](double seconds) { return stepped.simulatedFrame("sim", static_cast<int>(std::lround(seconds * kFps))); };
    baked->firstSeconds = 1 / kFps;
    baked->lastSeconds = 12 / kFps;
    baked->stamp = background.simulationStamp("sim");
    background.setBakedSimulation("sim", baked);
    for (int f : {9, 4, 12}) expectSameStrands(*viewAt(background, f), *viewAt(stepped, f), "a baked frame " + std::to_string(f));
    EXPECT_EQ(background.simulationStatus()[0].frames, 0u);
}

TEST(SimulateNode, BypassGivesTheGroomExactly) {
    GroomSession session(simData());
    viewAt(session, 10);
    std::string bypassed = kSimGraph;
    const std::string node = R"({"name": "sim", "type": "Simulate", )";
    bypassed.replace(bypassed.find(node), node.size(), R"({"name": "sim", "type": "Simulate", "bypass": true, )");
    session.setGraphJson(bypassed);
    GroomSession still(simData(unsimulatedGraph()));
    expectSameStrands(*viewAt(session, 10), *viewAt(still, 10), "bypassed");
    // Before its start frame it gives the guides through too.
    GroomSession early(simData(simGraph(R"({"gravity": 9.8, "startFrame": 20})")));
    expectSameStrands(*viewAt(early, 10), *viewAt(still, 10), "before the start frame");
}

TEST(SimulateNode, WetnessReachesTheStrandsAndWaterParamsDropTheFrames) {
    // The sphere under water: its guides' wetness, blended by Interpolate into the strands'
    // wetness point channel (what Clump, HairColor and the shaders read).
    GroomSession session(simData(simGraph(R"({"gravity": 9.8, "water": true, "waterLevel": 5.0})")));
    const StrandsPtr wet = viewAt(session, 12);
    const std::vector<float>* channel = wet->pointChannel(kWetnessChannel);
    ASSERT_TRUE(channel) << "no wetness channel on the strands";
    float least = 1.0f;
    for (const float w : *channel) least = std::min(least, w);
    EXPECT_GT(least, 0.8f);
    // Between two frames the wetness blends as the points do.
    ViewRequest between;
    between.time = 11.5 / kFps;
    EXPECT_TRUE(session.evaluate(between).strands->pointChannel(kWetnessChannel));
    // A water param edited: the frames go, and the frames made again differ.
    session.setGraphJson(simGraph(R"({"gravity": 9.8, "water": true, "waterLevel": 5.0, "buoyancy": 2.0})"));
    viewAt(session, 1);
    EXPECT_EQ(session.simulationStatus()[0].frames, 1u) << "the frames of the old settings stayed";
    EXPECT_NE(viewAt(session, 12)->positions(), wet->positions());
    // Before the start frame the hair is dry, but the channel is there for the maps reading it.
    GroomSession early(simData(simGraph(R"({"gravity": 9.8, "water": true, "waterLevel": 5.0, "startFrame": 20})")));
    const std::vector<float>* none = viewAt(early, 10)->pointChannel(kWetnessChannel);
    ASSERT_TRUE(none);
    EXPECT_EQ(*std::max_element(none->begin(), none->end()), 0.0f);
    // Dry, the strands get no wetness channel.
    GroomSession dry(simData());
    EXPECT_FALSE(viewAt(dry, 4)->pointChannel(kWetnessChannel));
}

TEST(SimulateNode, TheHeadsMotionSwingsTheHairWhichSettles) {
    // The scalp slides along +x over frames 1 to 6 and stops; the hosts' feed gives each frame's.
    GroomData data = simData(simGraph(R"({"gravity": 9.8, "damping": 0.3, "rootStiffnessProfile": 0.0})"));
    const MeshPtr rest = data.scalp;
    auto shifted = [&](double seconds) {
        const float frame = static_cast<float>(seconds * kFps);
        const float dx = 0.15f * std::clamp(frame - 1.0f, 0.0f, 5.0f);
        std::vector<Vec3f> points = rest->points();
        for (Vec3f& p : points) p.x += dx;
        return points;
    };
    GroomSession session(data);
    session.setSimulationFeed([&](double seconds) {
        session.updateScalpPoints(shifted(seconds));
        return true;
    });
    auto tipLag = [&](int frame) {
        session.updateScalpPoints(shifted(frame / kFps));
        viewAt(session, frame);
        // The guides' tips against their roots, along x (the hierarchy as the view shows it is
        // the input; the simulated frame is the node's).
        const GuidesPtr sim = session.simulatedFrame("sim", frame);
        float lag = 0.0f;
        for (const Guide& g : sim->guides()) lag += (g.points.back().x - g.points.front().x);
        return lag / static_cast<float>(sim->size());
    };
    const float atRest = tipLag(1);
    const float moving = tipLag(6);
    EXPECT_LT(moving, atRest - 0.02f) << "the tips did not trail the motion";
    const float settled = tipLag(96);
    EXPECT_NEAR(settled, atRest, 0.05f) << "the hair did not settle back";
}

// A head turned on its side from its rest pose, still: the groom at the start frame as each
// session gives it, and the guides' mean tip height over their roots there.
float startTipDrop(const std::string& params, bool turned = true) {
    GroomData data = simData(simGraph(params));
    auto scalp = std::make_shared<ScalpMesh>(*data.scalp);
    if (turned) {
        scalp->setRestPoints(scalp->points());
        std::vector<Vec3f> points = scalp->points();
        for (Vec3f& p : points) p = Vec3f{-p.y, p.x, p.z};  // 90 degrees about z: the top to -x
        *scalp = scalp->withPoints(points);
        scalp->setRestPoints(data.scalp->points());
    }
    data.scalp = scalp;
    GroomSession session(data);
    viewAt(session, 1);
    const GuidesPtr sim = session.simulatedFrame("sim", 1);
    EXPECT_TRUE(sim);
    if (!sim) return 0.0f;
    EXPECT_EQ(session.simulationStatus()[0].ranges[0], std::make_pair(1, 1)) << "a preroll frame was kept";
    float drop = 0.0f;
    for (const Guide& g : sim->guides()) drop += g.points.back().y - g.points.front().y;
    return drop / static_cast<float>(sim->size());
}

TEST(SimulateNode, APrerollStartsFromTheRestPoseSoTheHairHangsAtTheStart) {
    // Without a preroll the hair starts at rest in the start frame's pose: groomed on a head turned
    // on its side it sticks out sideways, holding its weight. With one it starts at rest on the
    // scalp's rest pose (its weight preloaded upright) and the head turns over the preroll: at the
    // start frame it hangs down.
    const std::string base = R"("gravity": 980.0, "collisionOffset": 0.02, "startFrame": 1, "damping": 0.3)";
    const float stiff = startTipDrop("{" + base + "}");
    const float hanging = startTipDrop("{" + base + R"(, "preroll": 24})");
    EXPECT_LT(hanging, stiff - 0.1f) << "the hair did not fall as the head turned over the preroll";
}

TEST(SimulateNode, APrerollOnAStillHeadInItsRestPoseKeepsTheGroom) {
    const std::string base = R"("gravity": 980.0, "collisionOffset": 0.02, "startFrame": 1)";
    const float none = startTipDrop("{" + base + "}", false);
    const float settled = startTipDrop("{" + base + R"(, "preroll": 12})", false);
    EXPECT_NEAR(settled, none, 0.01f) << "the groom moved settling where it rests";
}

TEST(SimulateNode, APrerollChangesTheStampOnlyWhenOn) {
    SimSettings off, on;
    on.preroll = 10;
    EXPECT_NE(off.hash(), on.hash());
    SimSettings zero;
    zero.preroll = 0;
    EXPECT_EQ(off.hash(), zero.hash());
}

TEST(SimulateNode, FramesPlaybackSkippedAreSteppedInOneGoAndKept) {
    // Real-time playback skips frames when a frame takes longer than its time, and while it plays
    // the host's feed gives nothing (Maya reads no other time then). The frames skipped were each
    // stepped toward the frame shown: the head's whole motion since the last frame in the first of
    // them (the hair whipped), and each was stamped with the shown frame's inputs, so scrubbing
    // back to one (not playing: the feed gives its own) found other inputs and threw every frame
    // away; what was simulated again, and baked, was not what had played. Now they are stepped in
    // one go toward the frame shown (the roots moving evenly over them) and kept.
    GroomData data = simData(simGraph(R"({"gravity": 9.8, "damping": 0.3, "rootStiffnessProfile": 0.0})"));
    const MeshPtr rest = data.scalp;
    auto shifted = [&](double seconds) {
        const float frame = static_cast<float>(seconds * kFps);
        const float dx = 0.15f * std::clamp(frame - 1.0f, 0.0f, 8.0f);
        std::vector<Vec3f> points = rest->points();
        if (dx != 0.0f) {  // (else the points to the bit: -0 + 0 is +0, another scalp's hash)
            for (Vec3f& p : points) p.x += dx;
        }
        return points;
    };
    bool playing = true;
    GroomSession skipped(data), every(data);
    for (GroomSession* s : {&skipped, &every}) {
        s->setSimulationFeed([s, &playing, &shifted](double seconds) {
            if (playing) return false;  // (as Maya's: no other time is read while it plays)
            s->updateScalpPoints(shifted(seconds));
            return true;
        });
    }
    auto show = [&](GroomSession& s, int frame) {
        s.updateScalpPoints(shifted(frame / kFps));
        return viewAt(s, frame);
    };
    std::map<int, std::vector<Vec3f>> played, everyFrame;
    for (int f = 1; f <= 16; f += 3) played[f] = show(skipped, f)->positions();  // frames 2-3, 5-6... skipped
    for (int f = 1; f <= 16; ++f) everyFrame[f] = show(every, f)->positions();
    // Close to playing every frame: the skipped frames' motion spread evenly, not all in one (the
    // guides 0.8 long: 0.06 apart at most, where stepping each toward the frame shown was 0.42).
    float worst = 0.0f;
    for (const auto& [f, points] : played) {
        ASSERT_EQ(points.size(), everyFrame[f].size());
        for (std::size_t i = 0; i < points.size(); ++i) worst = std::max(worst, (points[i] - everyFrame[f][i]).length());
    }
    EXPECT_LT(worst, 0.1f);
    // Stopped: scrubbing back to a skipped frame keeps the frames that played.
    playing = false;
    show(skipped, 5);
    EXPECT_EQ(skipped.simulationStatus()[0].ranges[0], std::make_pair(1, 16));
    for (const auto& [f, points] : played) EXPECT_TRUE(show(skipped, f)->positions() == points) << "frame " << f << " as it played";
    EXPECT_EQ(skipped.simulationStatus()[0].frames, 16u);
    // Simulate Range steps nothing more (a bake writes the frames that played).
    EXPECT_EQ(skipped.simulateTo(16).frames, 0);
}

// ---- Cost --------------------------------------------------------------------------------------

// Guides over a head-sized sphere (radius 10, centimeters): `count` guides of 20 points, 15 long.
GuidesPtr headGuides(const ScalpMesh& head, std::size_t count) {
    auto guides = std::make_shared<GuideSet>();
    Rng rng(5);
    std::size_t made = 0;
    while (made < count) {
        const StrandRoot root{static_cast<std::int32_t>(rng.nextU64() % head.triangleCount()), 0.3f, 0.3f};
        const Vec3f at = head.position(root), n = head.normal(root);
        if (n.y < 0.0f) continue;
        std::vector<Vec3f> pts;
        for (int k = 0; k < 20; ++k) pts.push_back(at + n * (15.0f * static_cast<float>(k) / 19.0f));
        Guide& g = guides->add(static_cast<std::uint64_t>(made + 1), std::move(pts));
        g.root = root;
        ++made;
    }
    return guides;
}

double msPerFrame(std::size_t count, int frames) {
    const auto head = test::makeSphere(48, 96, 10.0f);
    const GuidesPtr guides = headGuides(*head, count);
    SimSettings s;  // the node's defaults: 4 substeps, 6 iterations, scalp collisions
    SimCache cache;
    cache.configure(s, kFps);
    cache.start({guides, head, {}}, 1);
    cache.step({guides, head, {}}, 1);  // the field's first regions made
    const auto started = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) cache.step({guides, head, {}}, 1);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / frames;
}

TEST(Budget, SimulatingThousandsOfGuidesTakesWellUnderAFramePerSubstep) {
    const double ms1k = msPerFrame(1000, 8), ms5k = msPerFrame(5000, 8), ms10k = msPerFrame(10000, 4);
    std::printf("[ sim cost ] 1k guides: %.1f ms/frame, 5k: %.1f, 10k: %.1f (20 points, 4 substeps, 6 iterations, scalp "
                "collisions)\n", ms1k, ms5k, ms10k);
    // 5k guides: each of the 4 substeps well under a frame's time at 24 fps (41.7 ms).
    EXPECT_LT(ms5k / 4.0, 1000.0 / 24.0 / 2.0) << ms5k << " ms per frame";
}


// The guides' tips after `seconds` of falling and swinging at `fps` with `substeps`.
std::vector<Vec3f> tipsAfter(double fps, int substeps, double seconds, float stiffness, float friction) {
    const auto sphere = test::makeSphere(16, 32, 1.0f);
    const GuidesPtr guides = guidesOnSphere(*sphere, 12, 12, 0.8f);
    SimSettings s = unitSettings();
    s.substeps = substeps;
    s.stiffness = stiffness;
    s.friction = friction;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.1f;
    s.damping = 0.6f;  // settled by the time asked: the look, not where a swing is
    SimCache cache;
    cache.configure(s, fps);
    const SimInputs in{guides, MeshPtr(sphere), {}};
    cache.start(in, 1);
    const int frames = static_cast<int>(std::lround(seconds * fps));
    for (int f = 0; f < frames; ++f) EXPECT_TRUE(cache.step(in, 1));
    std::vector<Vec3f> tips;
    for (const Guide& g : cache.frame(cache.lastFrame())->guides()) tips.push_back(g.points.back());
    return tips;
}

float largestApart(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float most = 0.0f;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) most = std::max(most, (a[i] - b[i]).length());
    return most;
}

// How much longer than groomed (a share) a 25 cm guide (15 segments, centimetres) hangs after
// `seconds` of falling from level at `fps` with `substeps` and `stiffness`.
float hangingStretch(double fps, int substeps, float stiffness, double seconds = 3.0) {
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {25.0f, 0.0f, 0.0f}, 15));
    SimSettings s;
    s.groomedUnderGravity = false;  // a horizontal groom that gravity pulls down and stretches
    s.substeps = substeps;
    s.stiffness = stiffness;
    s.damping = 0.3f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    SimCache cache;
    cache.configure(s, fps);
    cache.start({guides, nullptr, {}}, 1);
    const int frames = static_cast<int>(std::lround(seconds * fps));
    for (int f = 0; f < frames; ++f) cache.step({guides, nullptr, {}}, 1);
    const std::vector<Vec3f>& p = cache.frame(cache.lastFrame())->guides()[0].points;
    float length = 0.0f;
    for (std::size_t j = 1; j < p.size(); ++j) length += segment(p, j);
    return length / 25.0f - 1.0f;
}

TEST(Simulation, TheLookHoldsAtAnyFrameRateAndSubsteps) {
    // Root stiffness, stretch and repulsion are shares per reference step (bend's compliance
    // already was), friction Coulomb's: 24 and 48 fps, 4 and 8 substeps settle the same, within what the
    // solver itself gives at another step (its iterations per substep: an inextensible strand
    // without friction or repulsion).
    const std::vector<Vec3f> base = tipsAfter(24.0, 4, 3.0, 0.6f, 0.5f);
    ASSERT_EQ(base.size(), 12u);
    const float solver = largestApart(tipsAfter(24.0, 4, 3.0, 1.0f, 0.0f), tipsAfter(48.0, 4, 3.0, 1.0f, 0.0f));
    EXPECT_LT(solver, 0.08f);  // of guides 0.8 long
    EXPECT_LT(largestApart(base, tipsAfter(48.0, 4, 3.0, 0.6f, 0.5f)), solver + 0.01f);
    EXPECT_LT(largestApart(base, tipsAfter(24.0, 8, 3.0, 0.6f, 0.5f)), solver + 0.01f);
    // A soft stretch shows, the same at every rate: hanging hair at stretchStiffness 0.6 is
    // measurably longer than at 1. (This once checked that a soft stretch moved the tips of the
    // guides above more than the solver's own spread, which held only because of a bug: the
    // one-sided solver made soft-stretch hair shorter and stand 5 cm taller. Solved whole, the
    // stretch is a compliance and those tips move 0.02.)
    const float soft = hangingStretch(24.0, 4, 0.6f);
    EXPECT_GT(soft, 0.03f);
    EXPECT_LT(std::abs(hangingStretch(24.0, 4, 1.0f)), 0.002f);
    EXPECT_NEAR(hangingStretch(48.0, 4, 0.6f), soft, 0.01f);
    EXPECT_NEAR(hangingStretch(24.0, 8, 0.6f), soft, 0.01f);
}

// The most any point moved in a frame over the last `last` of `frames` frames on still inputs.
float motionAtTheEnd(const SimSettings& s, const SimInputs& in, int frames, int last) {
    GuideSolver solver;
    solver.start(s, kFps, 1, in);
    std::vector<Vec3f> before = solver.points();
    float most = 0.0f;
    for (int f = 2; f <= frames; ++f) {
        solver.step(in, 1);
        const std::vector<Vec3f>& x = solver.points();
        for (std::size_t i = 0; f > frames - last && i < x.size(); ++i) most = std::max(most, (x[i] - before[i]).length());
        before = x;
    }
    return most;
}

TEST(Simulation, HairOnAStillHeadComesToRest) {
    // Guides standing out of a still head (the node's defaults, centimetres) fall, drape over the
    // scalp and come to rest. The orientations' spin once fed the root stiffness's pull back into
    // the motion and points resting on a surface bounced off it: tips still moved centimetres a
    // frame ten seconds on.
    const SimSettings s;
    // Shoulder-length (25 cm, 16 points) on a 10 cm head, at 4 and 16 substeps.
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr guides = guidesOnSphere(*head, 80, 16, 25.0f);
    EXPECT_LT(motionAtTheEnd(s, {guides, head, {}}, 240, 48), 0.02f) << "on the scalp";
    SimSettings fine = s;
    fine.substeps = 16;
    EXPECT_LT(motionAtTheEnd(fine, {guides, head, {}}, 240, 48), 0.02f) << "16 substeps";
    // Long (55 cm, 24 points) on a 9 cm head over a torso's ellipsoid: on the shoulders.
    const auto small = test::makeSphere(32, 64, 9.0f);
    const auto unit = test::makeSphere(40, 80, 1.0f);
    std::vector<Vec3f> body = unit->points();
    for (Vec3f& p : body) p = Vec3f{p.x * 18.0f, p.y * 20.0f - 36.0f, p.z * 10.0f};
    const MeshPtr torso = std::make_shared<const ScalpMesh>(std::move(body), unit->triangles());
    const GuidesPtr hair = guidesOnSphere(*small, 60, 24, 55.0f);
    EXPECT_LT(motionAtTheEnd(s, {hair, small, {torso}}, 240, 48), 0.02f) << "on the shoulders";
}

// A 4 cm ball swept along +z at `cmPerFrame` (24 fps) through the middle of a curtain of hanging
// guides (21 of 20 cm): how many points the ball's middle passed within half its radius of (the
// motion between frames taken as straight), and whether it moved the hair at all.
std::pair<int, float> ballThroughHair(float cmPerFrame, int substeps, bool restPose) {
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 0;
    for (int ix = -3; ix <= 3; ++ix) {
        for (int iz = -1; iz <= 1; ++iz) {
            guides->add(++id, test::straightCurve({static_cast<float>(ix), 0.0f, static_cast<float>(iz)}, {0.0f, -20.0f, 0.0f}, 19));
        }
    }
    const float radius = 4.0f, y = -12.0f;
    const auto ball = test::makeSphere(16, 32, radius);
    auto ballAt = [&](float z) {
        std::vector<Vec3f> points = ball->points();
        for (Vec3f& p : points) p += Vec3f{0.0f, y, z};
        auto mesh = std::make_shared<ScalpMesh>(ball->withPoints(std::move(points)));
        if (restPose) mesh->setRestPoints(ball->points());
        return MeshPtr(mesh);
    };
    SimSettings s;
    s.substeps = substeps;
    GuideSolver solver;
    float z = -15.0f;
    solver.start(s, kFps, 1, {guides, nullptr, {ballAt(z)}});
    const std::vector<Vec3f> rest = solver.points();
    std::vector<Vec3f> before = rest;
    int through = 0;
    float moved = 0.0f;
    while (z < 23.0f) {
        const float was = z;
        z += cmPerFrame;
        solver.step({guides, nullptr, {ballAt(z)}}, 1);
        const std::vector<Vec3f>& x = solver.points();
        for (std::size_t i = 0; i < x.size(); ++i) {
            moved = std::max(moved, (x[i] - rest[i]).length());
            const float r0 = before[i].z - was, r1 = x[i].z - z;
            if (!(r0 > 0.0f && r1 <= 0.0f)) continue;
            const Vec3f at = before[i] + (x[i] - before[i]) * (r0 / (r0 - r1));
            if (std::hypot(at.x, at.y - y) < 0.5f * radius) ++through;
        }
        before = x;
    }
    return {through, moved};
}

TEST(Simulation, AMovingColliderIsSweptOverTheSubsteps) {
    // A collider that moved since the last frame is met by each substep on its way (its points
    // blended), not where the frame ends from the first substep on: at 144 cm/s (6 cm a frame,
    // past its radius in a frame and a half) a hand-sized ball no longer passes through the hair
    // at the default 4 substeps (16 points did), and more substeps hold it at twice that speed
    // (19 did at 16 substeps). Its rest pose's field or its own: the same.
    for (const bool restPose : {false, true}) {
        const auto [through, moved] = ballThroughHair(6.0f, 4, restPose);
        EXPECT_EQ(through, 0) << "rest pose " << restPose;
        EXPECT_GT(moved, 10.0f) << "the ball should carry the hair along";
        EXPECT_EQ(ballThroughHair(12.0f, 16, restPose).first, 0) << "rest pose " << restPose;
    }
}

TEST(Simulation, AnEmptyGuideDoesNotStopTheOthers) {
    // A guide of no points (and one of one) among the rest: started, stepped, handed back.
    const auto sphere = test::makeSphere(16, 32, 1.0f);
    auto guides = std::make_shared<GuideSet>(*guidesOnSphere(*sphere, 4, 6, 0.5f));
    guides->add(100, {});
    guides->add(101, {Vec3f(0.0f, 2.0f, 0.0f)});
    SimCache cache;
    cache.configure(unitSettings(), kFps);
    const SimInputs in{guides, MeshPtr(sphere), {}};
    cache.start(in, 1);
    for (int f = 0; f < 6; ++f) ASSERT_TRUE(cache.step(in, 1));
    const GuidesPtr out = cache.frame(cache.lastFrame());
    ASSERT_TRUE(out);
    ASSERT_EQ(out->size(), guides->size());
    EXPECT_TRUE(out->guides()[4].points.empty());
    EXPECT_EQ(out->guides()[5].points.size(), 1u);
    for (const Guide& g : out->guides()) {
        for (const Vec3f& p : g.points) EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
}

// ---- Stiff guides, and lengths after collisions -------------------------------------------------

// A head turning about y (radians at a frame) as the inputs see it: the scalp's points turned (its
// rest pose the unturned ones) and the groomed guides with it.
struct TurnedRun {
    float endMotion = 0.0f;   // the most a point moved in a frame, in the head's frame, over the last 48 frames
    float endShape = 0.0f;    // the farthest a point is from its groomed place, in the head's frame, at the end
    float endStretch = 0.0f;  // the most a segment is longer than groomed (a share) over the last 48 frames
    float peakStretch = 0.0f; // ... over every frame
};
TurnedRun turnedRun(const SimSettings& s, const GuidesPtr& guides, const MeshPtr& scalp, const std::function<float(int)>& yawAt,
                    int frames) {
    auto turned = [](float yaw, const Vec3f& v) {
        const float c = std::cos(yaw), sn = std::sin(yaw);
        return Vec3f{c * v.x + sn * v.z, v.y, -sn * v.x + c * v.z};
    };
    auto inputsAt = [&](int frame) {
        const float yaw = yawAt(frame);
        auto g = std::make_shared<GuideSet>(*guides);
        for (Guide& guide : g->guides()) {
            for (Vec3f& p : guide.points) p = turned(yaw, p);
        }
        MeshPtr mesh;
        if (scalp) {
            std::vector<Vec3f> points = scalp->points();
            for (Vec3f& p : points) p = turned(yaw, p);
            auto m = std::make_shared<ScalpMesh>(scalp->withPoints(std::move(points)));
            m->setRestPoints(scalp->points());
            mesh = m;
        }
        return SimInputs{GuidesPtr(g), mesh, {}};
    };
    std::vector<Vec3f> groomed;
    for (const Guide& g : guides->guides()) groomed.insert(groomed.end(), g.points.begin(), g.points.end());
    GuideSolver solver;
    solver.start(s, kFps, 1, inputsAt(1));
    std::vector<Vec3f> before = groomed;
    TurnedRun run;
    for (int f = 2; f <= frames; ++f) {
        solver.step(inputsAt(f), 1);
        std::vector<Vec3f> x = solver.points();
        for (Vec3f& p : x) p = turned(-yawAt(f), p);
        const bool end = f > frames - 48;
        float stretch = 0.0f;
        for (std::size_t g = 0, k = 0; g < guides->size(); ++g) {
            const std::size_t n = guides->guides()[g].points.size();
            for (std::size_t j = 0; j < n; ++j, ++k) {
                if (end) run.endMotion = std::max(run.endMotion, (x[k] - before[k]).length());
                if (f == frames) run.endShape = std::max(run.endShape, (x[k] - groomed[k]).length());
                if (j == 0) continue;
                const float l = (groomed[k] - groomed[k - 1]).length();
                if (l > 1e-6f) stretch = std::max(stretch, (x[k] - x[k - 1]).length() / l - 1.0f);
            }
        }
        run.peakStretch = std::max(run.peakStretch, stretch);
        if (end) run.endStretch = std::max(run.endStretch, stretch);
        before = std::move(x);
    }
    return run;
}

TEST(Simulation, StiffStandingGuidesHoldTheirShapeAndComeToRest) {
    // The bend constraint's correction was the quaternions' difference linearized: a turn far from
    // the groomed one was corrected short, XPBD's multipliers drifted from what was done, and a
    // stiff guide standing up never held: a single 25 cm guide (no scalp, leaning a little) at
    // bend 0.9 fell and whipped about 13 cm a frame ten seconds on (17 at bend 1.0, 32 at 16
    // substeps); 25 cm guides standing on a head at bend 1 lay down on it (40 cm off their shape).
    // Now the correction is the exact turn: they stand, a little bowed, and are still.
    auto alone = std::make_shared<GuideSet>();
    alone->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, Vec3f{0.1f, 1.0f, 0.0f}.normalized() * 25.0f, 15));
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr standing = guidesOnSphere(*head, 80, 16, 25.0f);
    auto still = [](int) { return 0.0f; };
    for (const float bend : {0.9f, 1.0f}) {
        for (const int substeps : {4, 16}) {
            SimSettings s;
            s.bend = bend;
            s.rootStiffness = 0.0f;  // nothing but the bend holds it up
            s.substeps = substeps;
            const TurnedRun one = turnedRun(s, alone, nullptr, still, 240);
            EXPECT_LT(one.endMotion, 0.02f) << "bend " << bend << ", " << substeps << " substeps";
            EXPECT_LT(one.endShape, 2.0f) << "bend " << bend << ", " << substeps << " substeps";
        }
        SimSettings s;
        s.bend = bend;
        const TurnedRun many = turnedRun(s, standing, head, still, 240);
        EXPECT_LT(many.endMotion, 0.02f) << "bend " << bend;
        EXPECT_LT(many.endShape, 5.0f) << "bend " << bend;
    }
    // The stiffest settings (bend 1, rootStiffness 2 held along the whole length) through a fast
    // head turn (90 degrees in 4 frames): still at the end, in their groomed shape.
    SimSettings held;
    held.bend = 1.0f;
    held.rootStiffness = 2.0f;
    held.rootStiffnessProfile = Ramp::constant(1.0f);
    auto turn = [](int f) { return 1.5708f * std::clamp((f - 10) / 4.0f, 0.0f, 1.0f); };
    const TurnedRun stiff = turnedRun(held, standing, head, turn, 120);
    EXPECT_LT(stiff.endMotion, 0.02f);
    EXPECT_LT(stiff.endShape, 0.1f);
}

// Bob-like guides on a 10 cm head: off the scalp along its normal, curving over to hang down, 14 cm
// in 17 points, on the head's upper half.
GuidesPtr bobGuides(const ScalpMesh& head) {
    auto guides = std::make_shared<GuideSet>();
    std::size_t made = 0;
    for (std::size_t f = 0; f < head.triangleCount() && made < 120; f += 5) {
        const StrandRoot root{static_cast<std::int32_t>(f), 0.3f, 0.3f};
        const Vec3f at = head.position(root), n = head.normal(root);
        if (n.y < 0.3f) continue;
        std::vector<Vec3f> pts{at};
        Vec3f p = at;
        for (int k = 1; k < 17; ++k) {
            const float t = static_cast<float>(k) / 16.0f;
            const Vec3f d = (n * (1.0f - t) + Vec3f{n.x, 0.0f, n.z} * 0.3f + Vec3f{0.0f, -1.0f, 0.0f} * t).normalized();
            p = p + d * (14.0f / 16.0f);
            if (p.length() < 10.5f) p = p.normalized() * 10.5f;  // out of the head by half a centimetre
            pts.push_back(p);
        }
        Guide& g = guides->add(static_cast<std::uint64_t>(made + 1), std::move(pts));
        g.root = root;
        ++made;
    }
    return guides;
}

// How far each guide's end (its last two segments' direction) turns from one frame to the next,
// in degrees, in the head's frame, through a head's motion (a yaw about y), as a playblast shows
// it: the 99th percentile and the share past 30 degrees from frame `from` on.
struct EndTurns {
    float p99 = 0.0f;
    float over30 = 0.0f;
};
EndTurns endTurns(const SimSettings& s, const GuidesPtr& guides, const MeshPtr& scalp, const std::function<float(int)>& yawAt,
                  int from, int frames) {
    auto turned = [](float yaw, const Vec3f& v) {
        const float c = std::cos(yaw), sn = std::sin(yaw);
        return Vec3f{c * v.x + sn * v.z, v.y, -sn * v.x + c * v.z};
    };
    auto inputsAt = [&](int frame) {
        const float yaw = yawAt(frame);
        auto g = std::make_shared<GuideSet>(*guides);
        for (Guide& guide : g->guides()) {
            for (Vec3f& p : guide.points) p = turned(yaw, p);
        }
        std::vector<Vec3f> points = scalp->points();
        for (Vec3f& p : points) p = turned(yaw, p);
        auto m = std::make_shared<ScalpMesh>(scalp->withPoints(std::move(points)));
        m->setRestPoints(scalp->points());
        return SimInputs{GuidesPtr(g), MeshPtr(m), {}};
    };
    GuideSolver solver;
    solver.start(s, kFps, 1, inputsAt(1));
    std::vector<Vec3f> before;
    std::vector<float> turns;
    for (int f = 2; f <= frames; ++f) {
        solver.step(inputsAt(f), 1);
        std::vector<Vec3f> x = solver.points();
        for (Vec3f& p : x) p = turned(-yawAt(f), p);
        std::vector<Vec3f> ends;
        for (std::size_t g = 0, k = 0; g < guides->size(); ++g) {
            k += guides->guides()[g].points.size();
            ends.push_back((x[k - 1] - x[k - 3]).normalized());
        }
        if (f > from && !before.empty()) {
            for (std::size_t g = 0; g < ends.size(); ++g) {
                turns.push_back(std::acos(std::clamp(ends[g].dot(before[g]), -1.0f, 1.0f)) * 57.29578f);
            }
        }
        before = std::move(ends);
    }
    EndTurns r;
    std::sort(turns.begin(), turns.end());
    r.p99 = turns[static_cast<std::size_t>(0.99 * static_cast<double>(turns.size() - 1))];
    r.over30 = static_cast<float>(std::count_if(turns.begin(), turns.end(), [](float t) { return t > 30.0f; })) /
               static_cast<float>(turns.size());
    return r;
}

TEST(Simulation, HairEndsDoNotFlipInAHeadShake) {
    // The rod constraints were projected one-sided (the bend turning only the child segment, the
    // stretch moving the point before less and less above bend 0.5): a moving root pumped energy
    // into the chain, the tip's speed doubling every substep, and a bob's ends flipped about in a
    // gentle head shake (at bend 0.78, 3.4% of the ends turned over 30 degrees in a frame at 4
    // substeps, 10.6% at 10: more substeps made it worse). Each guide's chain is solved as a whole
    // now: the ends follow the head smoothly at any substeps.
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr bob = bobGuides(*head);
    // A head shake: 14 degrees each way, four swings a second, for 20 frames, then still.
    auto shake = [](int f) { return f < 30 || f > 50 ? 0.0f : 0.245f * std::sin(6.2832f * static_cast<float>(f - 30) / 6.0f); };
    for (const float bend : {0.4f, 0.6f, 0.78f, 0.9f}) {
        EndTurns coarse;
        for (const auto& [substeps, iterations] : std::vector<std::pair<int, int>>{{4, 6}, {10, 12}}) {
            SimSettings s;
            s.groomedUnderGravity = true;
            s.bend = bend;
            s.twist = 0.6f;
            s.damping = 0.35f;
            s.drag = 0.4f;
            s.inertia = 0.75f;
            s.rootStiffnessProfile = Ramp{{0.0f, 1.0f}, {0.25f, 0.4f}, {0.55f, 0.05f}, {1.0f, 0.0f}};
            s.substeps = substeps;
            s.iterations = iterations;
            const EndTurns r = endTurns(s, bob, head, shake, 30, 72);
            char what[96];
            std::snprintf(what, sizeof what, "bend %.2f, %d substeps: p99 %.1f deg, %.2f%% over 30", bend, substeps, r.p99,
                          100.0f * r.over30);
            char key[32];
            std::snprintf(key, sizeof key, "bend%.2f_substeps%d", bend, substeps);
            ::testing::Test::RecordProperty(key, what);
            EXPECT_LT(r.over30, 0.005f) << what;
            EXPECT_LT(r.p99, 20.0f) << what;
            if (substeps == 4) {
                coarse = r;
            } else {
                EXPECT_LE(r.over30, coarse.over30 + 0.001f) << what << ": more substeps flipped more ends";
            }
        }
    }
}

TEST(Simulation, GroomedUnderGravityKeepsAStillGroomAndStillMoves) {
    // Soft guides held near the root only: gravity bends them down from the groomed shape.
    // groomedUnderGravity takes the groomed shape as how they rest under gravity: still, they
    // keep it; the head's motion still swings them, and they come back to it. Weightless and
    // preloaded alike (these soft guides stand up: preloaded, they take the share of their weight
    // they can hold upright).
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr guides = guidesOnSphere(*head, 60, 12, 8.0f);
    SimSettings s;
    s.groomedUnderGravity = false;  // first gravity on the groomed shape itself
    s.bend = 0.6f;
    auto still = [](int) { return 0.0f; };
    auto turn = [](int f) { return 1.5708f * std::clamp((f - 30) / 4.0f, 0.0f, 1.0f); };
    const TurnedRun pulled = turnedRun(s, guides, head, still, 60);
    EXPECT_GT(pulled.endShape, 1.0f) << "gravity should bend soft guides";
    s.groomedUnderGravity = true;
    for (const bool preload : {false, true}) {
        SCOPED_TRACE(preload ? "preload" : "weightless");
        s.preload = preload;
        const TurnedRun held = turnedRun(s, guides, head, still, 60);
        EXPECT_LT(held.endShape, 0.02f) << "the groomed shape held under gravity";
        EXPECT_LT(held.endMotion, 0.02f);
        // A head turn swings them (in the head's frame), and they settle back into the groom
        // (with their weight they swing on longer at these low damping defaults).
        const TurnedRun turned = turnedRun(s, guides, head, turn, preload ? 168 : 120);
        EXPECT_LT(turned.endShape, 0.05f);
        EXPECT_LT(turned.endMotion, 0.02f);
        EXPECT_GT(turnedRun(s, guides, head, turn, 34).endShape, 1.0f) << "the turn should swing the guides";  // as it ends
    }
    // Preload by default; weightless and off are other stamps.
    EXPECT_TRUE(SimSettings{}.groomedUnderGravity);
    EXPECT_TRUE(SimSettings{}.preload);
    SimSettings off, weightless;
    off.groomedUnderGravity = false;
    weightless.preload = false;
    EXPECT_NE(off.hash(), SimSettings{}.hash());
    EXPECT_NE(weightless.hash(), SimSettings{}.hash());
}

// The guides run with the head pitched about x (forward) by pitchAt(frame) radians, measured in
// the head's frame: the farthest a point is from its groomed place at `frames`, and at `holdEnd`.
struct PitchedRun {
    float shapeAtHold = 0.0f;
    float endShape = 0.0f;
    float endMotion = 0.0f;
};
PitchedRun pitchedRun(const SimSettings& s, const GuidesPtr& guides, const MeshPtr& scalp, const std::function<float(int)>& pitchAt,
                      int holdEnd, int frames) {
    auto pitched = [](float a, const Vec3f& v) {
        const float c = std::cos(a), sn = std::sin(a);
        return Vec3f{v.x, c * v.y - sn * v.z, sn * v.y + c * v.z};
    };
    auto inputsAt = [&](int frame) {
        const float a = pitchAt(frame);
        auto g = std::make_shared<GuideSet>(*guides);
        for (Guide& guide : g->guides()) {
            for (Vec3f& p : guide.points) p = pitched(a, p);
        }
        MeshPtr mesh;
        if (scalp) {
            std::vector<Vec3f> points = scalp->points();
            for (Vec3f& p : points) p = pitched(a, p);
            auto m = std::make_shared<ScalpMesh>(scalp->withPoints(std::move(points)));
            m->setRestPoints(scalp->points());
            mesh = m;
        }
        return SimInputs{GuidesPtr(g), mesh, {}};
    };
    std::vector<Vec3f> groomed;
    for (const Guide& g : guides->guides()) groomed.insert(groomed.end(), g.points.begin(), g.points.end());
    GuideSolver solver;
    solver.start(s, kFps, 1, inputsAt(1));
    std::vector<Vec3f> before = groomed;
    PitchedRun run;
    for (int f = 2; f <= frames; ++f) {
        solver.step(inputsAt(f), 1);
        std::vector<Vec3f> x = solver.points();
        for (Vec3f& p : x) p = pitched(-pitchAt(f), p);
        for (std::size_t k = 0; k < x.size(); ++k) {
            const float off = (x[k] - groomed[k]).length();
            if (f == holdEnd) run.shapeAtHold = std::max(run.shapeAtHold, off);
            if (f == frames) run.endShape = std::max(run.endShape, off);
            if (f > frames - 24) run.endMotion = std::max(run.endMotion, (x[k] - before[k]).length());
        }
        before = std::move(x);
    }
    return run;
}

TEST(Simulation, GravityPreloadHoldsTheGroomWithItsWeight) {
    // Preload (gravity preloading, Liu 2022): the rest turns take the groomed shape's weight, so
    // gravity acts in full and a still groom rests in its groomed shape. A level 25 cm guide (the
    // most a joint carries) and guides on a head, natural-hair settings.
    auto level = std::make_shared<GuideSet>();
    level->add(1, test::straightCurve({0.0f, 0.0f, 0.0f}, {25.0f, 0.0f, 0.0f}, 15));
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr onHead = guidesOnSphere(*head, 60, 12, 8.0f);
    auto still = [](int) { return 0.0f; };
    auto tilt = [](int f) { return 1.5708f * std::clamp((f - 30) / 6.0f, 0.0f, 1.0f) * (1.0f - std::clamp((f - 90) / 6.0f, 0.0f, 1.0f)); };
    for (const float bend : {0.6f, 0.78f, 0.9f}) {
        SimSettings s;
        s.bend = bend;
        s.twist = 0.6f;
        s.damping = 0.35f;
        s.drag = 0.4f;
        s.rootStiffnessProfile = Ramp{{0.0f, 1.0f}, {0.25f, 0.4f}, {0.55f, 0.05f}, {1.0f, 0.0f}};
        for (const auto& [name, guides, scalp] : std::vector<std::tuple<const char*, GuidesPtr, MeshPtr>>{{"level", level, nullptr},
                                                                                                         {"head", onHead, head}}) {
            s.groomedUnderGravity = false;
            const PitchedRun off = pitchedRun(s, guides, scalp, still, 0, 96);
            s.groomedUnderGravity = true;
            s.preload = false;
            const PitchedRun cancel = pitchedRun(s, guides, scalp, still, 0, 96);
            const PitchedRun cancelTilt = pitchedRun(s, guides, scalp, tilt, 90, 200);
            s.preload = true;
            const PitchedRun preload = pitchedRun(s, guides, scalp, still, 0, 96);
            const PitchedRun preloadTilt = pitchedRun(s, guides, scalp, tilt, 90, 200);
            char what[256];
            std::snprintf(what, sizeof what,
                          "%s bend %.2f: still off %.3f cancel %.3f preload %.3f (motion %.4f); tilted cancel %.3f preload %.3f; "
                          "back cancel %.3f preload %.3f",
                          name, bend, off.endShape, cancel.endShape, preload.endShape, preload.endMotion, cancelTilt.shapeAtHold,
                          preloadTilt.shapeAtHold, cancelTilt.endShape, preloadTilt.endShape);
            ::testing::Test::RecordProperty(std::string(name) + "_bend" + std::to_string(static_cast<int>(bend * 100)), what);
            const float scale = guides == level ? 25.0f : 8.0f;
            EXPECT_LT(preload.endShape, 0.02f * scale) << what;
            EXPECT_LT(preload.endMotion, 0.002f * scale) << what;
            // Back in the groom after the tilt (soft guides with their weight may fall over to
            // the other side and stay there, as hair does: within 15% of their length).
            EXPECT_LT(preloadTilt.endShape, (bend < 0.7f ? 0.15f : 0.03f) * scale) << what;
        }
    }
}

TEST(Simulation, SimulatePresetsSetTheSameParamsAndDefaultIsTheNodes) {
    // Default is the node's spec defaults; every preset sets the same params (switching leaves no
    // mix behind), all of them the node's.
    std::map<std::string, ParamValue> specs;
    for (const ParamSpec& spec : OperatorRegistry::instance().create(op::kSimulate)->paramSpecs()) specs[spec.name] = spec.defaultValue;
    ASSERT_GE(simulatePresets().size(), 2u);
    EXPECT_EQ(simulatePresets()[0].name, "default");
    std::set<std::string> keys;
    for (const SimPreset& preset : simulatePresets()) {
        const auto params = nlohmann::json::parse(simulatePresetParamsJson(preset.name));
        std::set<std::string> these;
        for (const auto& [name, value] : params.items()) {
            EXPECT_TRUE(specs.count(name)) << preset.name << ": " << name << " is not a Simulate param";
            these.insert(name);
        }
        if (keys.empty()) keys = these;
        EXPECT_EQ(these, keys) << preset.name;
    }
    ParamSet defaults;
    for (const std::string& name : keys) defaults.set(name, specs.at(name));
    EXPECT_EQ(nlohmann::json::parse(simulatePresetParamsJson("default")), nlohmann::json::parse(paramsJson(defaults)));
    const auto fur = nlohmann::json::parse(simulatePresetParamsJson("fur"));
    EXPECT_EQ(fur["groomedUnderGravity"], "weightless");
    EXPECT_THROW(simulatePresetParamsJson("nope"), std::invalid_argument);
}

TEST(Simulation, HairRestingOnASurfaceKeepsItsLength) {
    // Collisions came after the length pass, so the points pushed out of the scalp stretched
    // their segments: shoulder-length hair resting on a head (the node's defaults) 1.5% longer
    // than groomed, 12% in a fast head turn (90 degrees in 3 frames). The lengths are kept again
    // after the pushes.
    const auto head = test::makeSphere(32, 64, 10.0f);
    const GuidesPtr guides = guidesOnSphere(*head, 80, 16, 25.0f);
    auto turn = [](int f) { return 1.5708f * std::clamp((f - 120) / 3.0f, 0.0f, 1.0f); };
    const TurnedRun lying = turnedRun(SimSettings{}, guides, head, [](int) { return 0.0f; }, 120);
    EXPECT_LT(lying.endStretch, 0.005f);
    const TurnedRun turning = turnedRun(SimSettings{}, guides, head, turn, 160);
    EXPECT_LT(turning.peakStretch, 0.02f);
    EXPECT_LT(turning.endStretch, 0.005f);
}


}  // namespace
}  // namespace groomlab
