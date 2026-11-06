// SPDX-License-Identifier: Apache-2.0
// Batch N4, the simulation's parity with Yeti: per guide and per point sim attributes (the
// mappable stiffnesses, damping, drag, rootStiffness, mass and simulate, painted on the
// guides), hair volume (Yeti's density gradient) and friction per collider. Each: what it does,
// the solver as it was at the defaults (to the bit), and the same frames on any thread count.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "groomlab/simulation.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr double kFps = 24.0;

SimSettings unitSettings() {
    SimSettings s;
    s.gravity = 9.8f;
    s.collisionOffset = 0.02f;
    s.groomedUnderGravity = false;  // gravity on the groomed shape itself: what these tests measure
    return s;
}

MapRef guideMap(const std::string& layer) {
    MapRef m;
    m.source = MapRef::Source::Guide;
    m.name = layer;
    return m;
}

// The guides' points after `frames` frames toward the same inputs (the maps read on them once).
std::vector<Vec3f> run(const SimSettings& s, const SimInputs& in, int frames) {
    SimCache cache;
    cache.configure(s, kFps);
    cache.start(in, 1);
    for (int f = 0; f < frames; ++f) EXPECT_TRUE(cache.step(in, 1));
    std::vector<Vec3f> out;
    for (const Guide& g : cache.frame(cache.lastFrame())->guides()) out.insert(out.end(), g.points.begin(), g.points.end());
    return out;
}

// The same on one thread (TBB) as on all.
std::vector<Vec3f> runOnOneThread(const SimSettings& s, const SimInputs& in, int frames) {
#if GROOMLAB_HAS_TBB
    std::vector<Vec3f> out;
    tbb::task_arena single(1);
    single.execute([&] { out = run(s, in, frames); });
    return out;
#else
    return run(s, in, frames);
#endif
}

bool sameBits(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3f)) == 0);
}

// A row of horizontal guides (no scalp), every other one marked 1 in the guide layer "mask" (the
// rest 0), with a per point layer "along" (0 at the root to 1 at the tip).
GuidesPtr rowOfGuides(int count, int segments = 16) {
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < count; ++i) {
        Guide& g = guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve({0.0f, 0.0f, 0.1f * i}, {1.0f, 0.0f, 0.0f}, segments));
        g.attributes["mask"] = i % 2 == 0 ? 1.0f : 0.0f;
        std::vector<float>& along = g.pointAttributes["along"];
        for (int k = 0; k <= segments; ++k) along.push_back(static_cast<float>(k) / static_cast<float>(segments));
    }
    return guides;
}

// Guides standing on the sphere's upper half along its normals.
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
        g.attributes["mask"] = made % 3 == 0 ? 1.0f : 0.0f;
        ++made;
    }
    return guides;
}

// ---- Per guide and per point settings ----------------------------------------------------------

TEST(SimAttributes, StrengthZeroHoldsPaintedGuidesAtTheirInputShape) {
    // strength painted 0 on every other guide: those stay as groomed, the rest fall.
    const GuidesPtr guides = rowOfGuides(8);
    SimSettings s = unitSettings();
    s.bend = 0.2f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    ParamSet params;
    params.set("strength", guideMap("mask"));
    SimInputs in{guides, nullptr, {}};
    in.maps = simulationMaps(params, *guides, nullptr);
    ASSERT_TRUE(in.maps);
    ASSERT_EQ(in.maps->strength.size(), 8u * 17u);
    const std::vector<Vec3f> p = run(s, in, 24);
    for (std::size_t g = 0; g < guides->size(); ++g) {
        const Guide& input = guides->guides()[g];
        const bool simulated = input.attributes.at("mask") > 0.5f;
        float apart = 0.0f;
        for (std::size_t j = 0; j < input.points.size(); ++j) apart = std::max(apart, (p[g * 17 + j] - input.points[j]).length());
        if (simulated) {
            EXPECT_GT(apart, 0.3f) << "guide " << g << " should fall";
        } else {
            EXPECT_LT(apart, 1e-5f) << "guide " << g << " painted 0 moved";
        }
    }
    EXPECT_TRUE(sameBits(p, runOnOneThread(s, in, 24))) << "one thread and many differ";
}

TEST(SimAttributes, StrengthHeldAlongTheLengthAndBlendedInBetween) {
    const GuidesPtr guides = rowOfGuides(2);
    SimSettings s = unitSettings();
    s.bend = 0.2f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    const std::vector<Vec3f> free = run(s, {guides, nullptr, {}}, 24);
    // Per point: held over the first half of each guide (an expression of the layer `along`), the
    // tip half hangs from the held part.
    {
        ParamSet params;
        MapRef m;
        m.source = MapRef::Source::Attribute;
        m.name = "along";
        m.min = -1.0f;  // along 0-0.5 maps to -1-0 (held), the tip half rises to 1
        m.max = 1.0f;
        params.set("strength", m);
        SimInputs in{guides, nullptr, {}};
        in.maps = simulationMaps(params, *guides, nullptr);
        ASSERT_TRUE(in.maps);
        const std::vector<Vec3f> p = run(s, in, 24);
        const std::vector<Vec3f>& input = guides->guides()[0].points;
        for (std::size_t j = 0; j <= 8; ++j) EXPECT_LT((p[j] - input[j]).length(), 1e-5f) << "point " << j << " is held";
        EXPECT_LT(p[16].y, input[16].y - 0.1f) << "the tip half hangs";
    }
    // strength 0.5 everywhere: half the simulated offset from the input shape.
    SimSettings half = s;
    half.strength = 0.5f;
    const std::vector<Vec3f> p = run(half, {guides, nullptr, {}}, 24);
    for (std::size_t g = 0; g < guides->size(); ++g) {
        const std::vector<Vec3f>& input = guides->guides()[g].points;
        for (std::size_t j = 0; j < input.size(); ++j) {
            const Vec3f expected = input[j] + (free[g * 17 + j] - input[j]) * 0.5f;
            EXPECT_LT((p[g * 17 + j] - expected).length(), 1e-4f) << "guide " << g << " point " << j;
        }
    }
    EXPECT_NE(half.hash(), s.hash());
}

TEST(SimAttributes, MapsAtTheDefaultsAreTheSolverAsItWas) {
    // Every mappable param mapped to its default value (an expression of a constant), with wind,
    // repulsion and the scalp: the frames of the solver without maps, to the bit.
    const auto sphere = test::makeSphere(16, 32);
    const GuidesPtr guides = guidesOnSphere(*sphere, 120, 12, 0.8f);
    SimSettings s = unitSettings();
    s.windSpeed = 2.0f;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.05f;
    const std::vector<Vec3f> plain = run(s, {guides, sphere, {}}, 16);
    ParamSet params;
    const std::pair<const char*, float> defaults[] = {{"stretchStiffness", 1.0f}, {"bend", 0.5f}, {"twist", 0.5f},
                                                      {"damping", 0.1f},          {"drag", 0.05f},       {"rootStiffness", 1.0f},
                                                      {"mass", 1.0f},             {"strength", 1.0f}};
    for (const auto& [name, value] : defaults) {
        MapRef m;
        m.source = MapRef::Source::Expression;
        m.name = "1";
        m.min = 0.0f;
        m.max = value;
        params.set(name, m);
    }
    SimInputs in{guides, sphere, {}};
    in.maps = simulationMaps(params, *guides, sphere);
    ASSERT_TRUE(in.maps);
    EXPECT_EQ(in.maps->mass.size(), in.maps->bend.size());
    EXPECT_TRUE(sameBits(run(s, in, 16), plain)) << "default maps changed the frames";
    // Their settings stamp says they are mapped (a bake made before is not taken).
    EXPECT_NE(SimSettings::fromParams(params).hash(), SimSettings::fromParams(ParamSet{}).hash());
    EXPECT_EQ(SimSettings::fromParams(ParamSet{}).hash(), SimSettings{}.hash());
}

TEST(SimAttributes, MassAndStiffnessMapsChangeTheMotionAsPainted) {
    // Wind across a row of guides: the ones painted heavy (mask 1: mass 8) blow less far; the ones
    // painted stiff (bend 1 against 0) sag less.
    const GuidesPtr guides = rowOfGuides(8);
    SimSettings s = unitSettings();
    s.gravity = 0.0f;
    s.windSpeed = 3.0f;
    s.windDirection = {0.0f, 1.0f, 0.0f};
    s.windGust = 0.0f;
    s.drag = 0.3f;
    s.bend = 0.3f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    ParamSet params;
    MapRef heavy = guideMap("mask");
    heavy.min = 1.0f;
    heavy.max = 8.0f;
    params.set("mass", heavy);
    SimInputs in{guides, nullptr, {}};
    in.maps = simulationMaps(params, *guides, nullptr);
    const std::vector<Vec3f> p = run(s, in, 12);
    float heavyRise = 0.0f, lightRise = 0.0f;
    for (std::size_t g = 0; g < guides->size(); ++g) {
        const float rise = p[g * 17 + 16].y;
        (g % 2 == 0 ? heavyRise : lightRise) += rise / 4.0f;
    }
    EXPECT_GT(lightRise, 0.05f) << "the wind did not lift the light guides";
    EXPECT_LT(heavyRise, lightRise * 0.6f) << "heavy guides blew as far as light ones";
    EXPECT_TRUE(sameBits(p, runOnOneThread(s, in, 12))) << "mass map: one thread and many differ";

    SimSettings sag = unitSettings();
    sag.rootStiffnessProfile = Ramp::constant(0.0f);
    ParamSet stiff;
    stiff.set("bend", guideMap("mask"));  // mask 1: bend 1, mask 0: bend 0
    SimInputs bent{guides, nullptr, {}};
    bent.maps = simulationMaps(stiff, *guides, nullptr);
    const std::vector<Vec3f> q = run(sag, bent, 24);
    for (std::size_t g = 0; g + 1 < guides->size(); g += 2) {
        EXPECT_GT(q[g * 17 + 16].y, q[(g + 1) * 17 + 16].y + 0.1f) << "the stiff guide " << g << " sagged as far";
    }
    EXPECT_TRUE(sameBits(q, runOnOneThread(sag, bent, 24))) << "bend map: one thread and many differ";

    // rootStiffness painted 0 frees what rootStiffnessProfile holds.
    SimSettings held = unitSettings();
    held.rootStiffnessProfile = Ramp::constant(0.5f);
    ParamSet freed;
    freed.set("rootStiffness", guideMap("mask"));  // mask 0: scale 0, free
    SimInputs part{guides, nullptr, {}};
    part.maps = simulationMaps(freed, *guides, nullptr);
    const std::vector<Vec3f> r = run(held, part, 24);
    EXPECT_GT(r[0 * 17 + 16].y, r[1 * 17 + 16].y + 0.1f) << "the guide whose hold was painted away hangs lower";
}

// ---- Hair volume -------------------------------------------------------------------------------

// A tuft: 25 guides rooted 0.02 apart fanning up and out, with nothing holding them: they fall
// together into a tight bundle below their roots.
GuidesPtr tuft() {
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < 5; ++k) {
            const Vec3f root{0.02f * (i - 2), 0.0f, 0.02f * (k - 2)};
            const Vec3f dir = Vec3f{0.3f * (i - 2), 1.0f, 0.3f * (k - 2)}.normalized();
            guides->add(static_cast<std::uint64_t>(i * 5 + k + 1), test::straightCurve(root, dir, 12));
        }
    }
    return guides;
}

float meanTipDistance(const std::vector<Vec3f>& p, std::size_t guides, std::size_t points) {
    double sum = 0.0;
    std::size_t pairs = 0;
    for (std::size_t a = 0; a < guides; ++a) {
        for (std::size_t b = a + 1; b < guides; ++b) {
            sum += (p[a * points + points - 1] - p[b * points + points - 1]).length();
            ++pairs;
        }
    }
    return static_cast<float>(sum / static_cast<double>(pairs));
}

TEST(SimVolume, KeepsACollapsingTuftApartAndIsOffByDefault) {
    const GuidesPtr guides = tuft();
    SimSettings s = unitSettings();
    s.bend = 0.0f;
    s.twist = 0.0f;
    s.rootStiffnessProfile = Ramp::constant(0.0f);
    s.damping = 0.3f;
    const std::vector<Vec3f> collapsed = run(s, {guides, nullptr, {}}, 48);
    SimSettings off = s;
    off.volumeCellSize = 0.3f;  // no volume: the cell size changes nothing, the stamp included
    EXPECT_TRUE(sameBits(run(off, {guides, nullptr, {}}, 48), collapsed));
    EXPECT_EQ(off.hash(), s.hash());
    SimSettings full = s;
    full.volume = 1.0f;
    full.volumeCellSize = 0.1f;
    const std::vector<Vec3f> kept = run(full, {guides, nullptr, {}}, 48);
    const float before = meanTipDistance(collapsed, 25, 13), after = meanTipDistance(kept, 25, 13);
    // 1.13x (0.76 against 0.67) since each guide is solved whole, 1.54x before (0.81 against 0.53):
    // the exact solve gives the orientations' resistance to each substep's turn in full, so the
    // heavily damped tuft falls together more slowly (0.27 apart at frame 96, 0.15 before) and the
    // volume's pushes move it a little less. With volume the tips hold 0.75 apart throughout.
    EXPECT_GT(after, before * 1.1f) << "volume kept the tips " << after << " apart against " << before;
    EXPECT_NE(full.hash(), s.hash());
    EXPECT_TRUE(sameBits(kept, runOnOneThread(full, {guides, nullptr, {}}, 48))) << "volume: one thread and many differ";
    EXPECT_TRUE(sameBits(kept, run(full, {guides, nullptr, {}}, 48))) << "volume: two runs differ";
}

// ---- Friction per collider ---------------------------------------------------------------------

// Guides on a head (the scalp) and, far off, a row of guides falling onto a ball (the collider).
struct TwoSurfaces {
    MeshPtr head = test::makeSphere(16, 32);
    MeshPtr ball;
    GuidesPtr guides;
    std::size_t headGuides = 0;
    TwoSurfaces() {
        const auto unit = test::makeSphere(16, 32, 0.5f);
        std::vector<Vec3f> moved = unit->points();
        for (Vec3f& p : moved) p += Vec3f{10.1f, 1.2f, 0.0f};
        ball = std::make_shared<const ScalpMesh>(std::move(moved), unit->triangles());
        auto set = std::make_shared<GuideSet>(*guidesOnSphere(*head, 40, 12, 0.8f));
        headGuides = set->size();
        for (int i = 0; i < 6; ++i) {
            set->add(static_cast<std::uint64_t>(1000 + i), test::straightCurve({10.0f + 0.05f * i, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 11));
        }
        guides = set;
    }
    std::vector<Vec3f> frames(SimSettings s, std::vector<float> friction, int count = 36) const {
        s.bend = 0.0f;
        s.rootStiffnessProfile = Ramp::constant(0.0f);
        SimInputs in{guides, head, {ball}};
        in.colliderFriction = std::move(friction);
        return run(s, in, count);
    }
};

TEST(SimFriction, EachColliderSlidesByItsOwnFriction) {
    const TwoSurfaces t;
    const SimSettings s = unitSettings();  // friction 0.3
    const std::vector<Vec3f> global = t.frames(s, {});
    EXPECT_TRUE(sameBits(t.frames(s, {-1.0f}), global)) << "a negative friction is not the global one";
    EXPECT_TRUE(sameBits(t.frames(s, {0.3f}), global)) << "the global friction set on the collider differs";
    const std::vector<Vec3f> sticky = t.frames(s, {1.0f}), slick = t.frames(s, {0.0f});
    const std::size_t onHead = t.headGuides * 12;
    // The head's guides never touch the ball: the same to the bit; the ball's slide by its friction.
    EXPECT_EQ(0, std::memcmp(sticky.data(), global.data(), onHead * sizeof(Vec3f))) << "the ball's friction moved the head's hair";
    EXPECT_EQ(0, std::memcmp(slick.data(), global.data(), onHead * sizeof(Vec3f))) << "the ball's friction moved the head's hair";
    EXPECT_FALSE(sameBits(sticky, global));
    EXPECT_FALSE(sameBits(slick, global));
    // Sliding along the ball: how far the points resting on it (in both frames) moved along it
    // between frames 24 and 36.
    auto sliding = [&](float friction) {
        const std::vector<Vec3f> a = t.frames(s, {friction}, 24), b = t.frames(s, {friction}, 36);
        const Vec3f center{10.1f, 1.2f, 0.0f};
        double along = 0.0;
        int resting = 0;
        for (std::size_t i = onHead; i < a.size(); ++i) {
            if ((a[i] - center).length() > 0.56f || (b[i] - center).length() > 0.56f) continue;
            const Vec3f n = (b[i] - center).normalized(), d = b[i] - a[i];
            along += (d - n * d.dot(n)).length();
            ++resting;
        }
        EXPECT_GT(resting, 5) << "nothing rests on the ball: the test shows nothing";
        return along / std::max(1, resting);
    };
    const double stuck = sliding(1.0f), slid = sliding(0.0f);
    EXPECT_LT(stuck, slid * 0.5) << "hair slid along the sticky ball (" << stuck << ") as along the slick one (" << slid << ")";
}

// Coulomb friction: a limp guide lying across a 20 degree slope from its root rests there when
// `friction` holds slopes steeper than that (0.3: 27 degrees) and swings down it when not (0.15:
// 13.5 degrees), at any substeps.
TEST(SimFriction, HairRestsOnSlopesUpToItsFrictionsAngle) {
    const float slope = 20.0f * 3.14159265f / 180.0f;
    const Vec3f up{std::cos(slope), std::sin(slope), 0.0f}, normal{-std::sin(slope), std::cos(slope), 0.0f};
    const auto flat = test::makePlane(8, 4.0f);
    std::vector<Vec3f> points = flat->points();
    for (Vec3f& p : points) {
        const Vec3f c = p - Vec3f{2.0f, 0.0f, 2.0f};
        p = up * c.x + normal * c.y + Vec3f{0.0f, 0.0f, c.z};
    }
    const MeshPtr plane = std::make_shared<const ScalpMesh>(std::move(points), flat->triangles());  // facing `normal`
    auto guides = std::make_shared<GuideSet>();
    const Vec3f root = Vec3f{0.0f, 0.0f, -0.5f} + normal * 0.03f, across{0.0f, 0.0f, 1.0f};
    guides->add(1, test::straightCurve(root, across, 10));
    auto slid = [&](float friction, int substeps) {
        SimSettings s = unitSettings();
        s.bend = 0.0f;
        s.twist = 0.0f;
        s.rootStiffnessProfile = Ramp::constant(0.0f);
        s.friction = friction;
        s.substeps = substeps;
        const std::vector<Vec3f> p = run(s, {guides, nullptr, {plane}}, 48);
        return (root + across - p.back()).dot(up);  // how far down the slope the tip went
    };
    EXPECT_LT(slid(0.3f, 4), 0.03f) << "hair slid on a slope its friction holds";
    EXPECT_LT(slid(0.3f, 8), 0.03f) << "hair slid on a slope its friction holds (8 substeps)";
    EXPECT_LT(slid(1.0f, 4), 0.03f) << "friction 1 let hair slide";
    const float slow = slid(0.15f, 4), free = slid(0.0f, 4);
    EXPECT_GT(slow, 0.2f) << "hair held on a slope steeper than its friction's";
    EXPECT_GT(free, slow) << "friction did not slow the slide";
    // Either side of the slope's angle: 22.5 degrees holds, 18 slides.
    EXPECT_LT(slid(0.25f, 4), 0.03f) << "hair slid on a slope just under its friction's angle";
    EXPECT_GT(slid(0.2f, 4), 0.1f) << "hair held on a slope just over its friction's angle";
}

// ---- Cost ----------------------------------------------------------------------------------------

// 5k guides of 20 points over a head-sized sphere (centimeters), the node's defaults: ms per frame
// as they are, with every param mapped (per guide) and with volume on.
// Volume and repulsion push light hair more, but never past their cap: the mass scales the push
// before it is capped, so a guide mapped nearly weightless is not thrown (it was, hundreds of units).
TEST(Simulation, LightHairIsPushedNoFartherThanTheCap) {
    const auto head = test::makeSphere(24, 48, 10.0f);
    auto set = std::make_shared<GuideSet>();
    Rng rng(9);
    while (set->size() < 400) {
        const StrandRoot root{static_cast<std::int32_t>(rng.nextU64() % head->triangleCount()), 0.3f, 0.3f};
        const Vec3f at = head->position(root), n = head->normal(root);
        if (n.y < 0.8f) continue;  // packed on the crown, so the hair crowds
        std::vector<Vec3f> pts;
        for (int k = 0; k < 12; ++k) pts.push_back(at + n * (15.0f * static_cast<float>(k) / 11.0f));
        Guide& g = set->add(static_cast<std::uint64_t>(set->size() + 1), std::move(pts));
        g.root = root;
        g.attributes["mask"] = rng.nextFloat();
    }
    const GuidesPtr guides = set;
    SimSettings s;
    s.volume = 0.5f;
    s.volumeCellSize = 1.0f;
    s.repulsion = 0.5f;
    s.repulsionRadius = 0.5f;
    ParamSet params;
    MapRef light = guideMap("mask");
    light.min = 0.001f;
    light.max = 0.01f;
    params.set("mass", light);
    SimInputs in{guides, head, {}};
    in.maps = simulationMaps(params, *guides, head);
    float farthest = 0.0f;
    for (const Vec3f& p : run(s, in, 10)) farthest = std::max(farthest, p.length());
    EXPECT_LT(farthest, 10.0f + 15.0f + 2.0f) << "a light guide was thrown";
}

TEST(Budget, SimAttributesAndVolumeStayWithinTheSimulationsBudget) {
    const auto head = test::makeSphere(48, 96, 10.0f);
    auto set = std::make_shared<GuideSet>();
    Rng rng(5);
    while (set->size() < 5000) {
        const StrandRoot root{static_cast<std::int32_t>(rng.nextU64() % head->triangleCount()), 0.3f, 0.3f};
        const Vec3f at = head->position(root), n = head->normal(root);
        if (n.y < 0.0f) continue;
        std::vector<Vec3f> pts;
        for (int k = 0; k < 20; ++k) pts.push_back(at + n * (15.0f * static_cast<float>(k) / 19.0f));
        Guide& g = set->add(static_cast<std::uint64_t>(set->size() + 1), std::move(pts));
        g.root = root;
        g.attributes["mask"] = rng.nextFloat();
    }
    const GuidesPtr guides = set;
    auto msPerFrame = [&](const SimSettings& s, const SimMapsPtr& maps) {
        SimInputs in{guides, head, {}};
        in.maps = maps;
        SimCache cache;
        cache.configure(s, kFps);
        cache.start(in, 1);
        cache.step(in, 1);
        const auto started = std::chrono::steady_clock::now();
        for (int f = 0; f < 6; ++f) cache.step(in, 1);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 6.0;
    };
    const SimSettings plain;
    ParamSet params;
    for (const std::string& name : simulationMapParams()) params.set(name, guideMap("mask"));
    const auto mapStarted = std::chrono::steady_clock::now();
    const SimMapsPtr maps = simulationMaps(params, *guides, head);
    const double mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStarted).count();
    SimSettings volume = plain;
    volume.volume = 0.3f;
    volume.volumeCellSize = 1.0f;
    msPerFrame(plain, nullptr);  // warm
    const double base = msPerFrame(plain, nullptr), mapped = msPerFrame(plain, maps), full = msPerFrame(volume, maps);
    std::printf("[ sim cost ] 5k guides: %.1f ms/frame; every param mapped: %.1f (+ %.1f ms reading the maps); "
                "with volume too: %.1f\n", base, mapped, mapMs, full);
    // Relative (other tests may run beside it): the maps and volume at most add a share of the cost,
    // or keep the whole within the simulation's budget (a frame in 83 ms at 5k guides): the rod
    // solve got about 3x faster (four guides a lane) while the hair volume's grid did not.
    EXPECT_LT(full, std::max(base * 3.0 + 10.0, 83.0)) << full << " ms per frame against " << base;
}

// ---- Through a groom session ------------------------------------------------------------------

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "ball", "type": "ColliderMesh", "params": {"name": "ball", "friction": FRICTION}},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "startFrame": 1SIMULATE}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 20.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "sim", "input": "guides"},
    {"from": "ball", "to": "sim", "input": "collider"},
    {"from": "sim", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::string graphWith(const std::string& friction, const std::string& simulate = "") {
    std::string g = kGraph;
    g.replace(g.find("FRICTION"), 8, friction);
    g.replace(g.find("SIMULATE"), 8, simulate);
    return g;
}

GroomData sessionData(const std::string& graph) {
    GroomData d;
    d.graphJson = graph;
    d.scalp = test::makeSphere(12, 24);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 10; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 10.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 8), tube);
    }
    h.evaluate(d.scalp.get());
    d.hierarchyJson = h.toJson();
    d.meshes["ball"] = test::makeSphere(12, 24, 1.3f);  // around the head: the hair rests against it
    return d;
}

std::vector<Vec3f> guidesAt(GroomSession& session, int frame) {
    session.simulateTo(frame);
    const GuidesPtr g = session.simulatedFrame("sim", frame);
    EXPECT_TRUE(g);
    std::vector<Vec3f> out;
    if (g) {
        for (const Guide& guide : g->guides()) out.insert(out.end(), guide.points.begin(), guide.points.end());
    }
    return out;
}

TEST(SimulateNode, TheColliderMeshsFrictionAndMapsReachTheSolver) {
    GroomSession global(sessionData(graphWith("-1"))), same(sessionData(graphWith("0.3"))),
        sticky(sessionData(graphWith("1.0")));
    const std::vector<Vec3f> a = guidesAt(global, 12);
    ASSERT_FALSE(a.empty());
    EXPECT_TRUE(sameBits(guidesAt(same, 12), a)) << "the global friction on the ColliderMesh differs";
    EXPECT_FALSE(sameBits(guidesAt(sticky, 12), a)) << "the ColliderMesh's friction changed nothing";
    // simulate mapped to 0 (an expression): every guide held at its input, frame after frame.
    GroomSession held(sessionData(graphWith("-1", R"(, "strength": {"expression": "0"})")));
    const std::vector<Vec3f> h = guidesAt(held, 12);
    const GuidesPtr input = held.simulatedFrame("sim", 1);
    ASSERT_TRUE(input);
    std::vector<Vec3f> rest;
    for (const Guide& guide : input->guides()) rest.insert(rest.end(), guide.points.begin(), guide.points.end());
    ASSERT_EQ(h.size(), rest.size());
    float apart = 0.0f;
    for (std::size_t i = 0; i < h.size(); ++i) apart = std::max(apart, (h[i] - rest[i]).length());
    EXPECT_LT(apart, 1e-4f) << "guides held by a strength map moved";
    EXPECT_GT([&] {
        float most = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i) most = std::max(most, (a[i] - rest[i]).length());
        return most;
    }(), 0.05f) << "the unmapped run did not move: the test shows nothing";
}

}  // namespace
}  // namespace groomlab
