// SPDX-License-Identifier: Apache-2.0
// Bend as a comb, Collide (scalp and collider meshes), BlendNeighbors and RestoreLength.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Straight strands rooted on the plane (normal +Y), pointing along `dir`.
StrandsPtr strandsOn(const ScalpMesh& plane, Vec3f dir, int count = 20) {
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < count; ++i) {
        const StrandRoot root{2 * i % static_cast<int>(plane.triangleCount()), 0.3f, 0.3f};
        s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve(plane.position(root), dir, 8), {}, root);
    }
    return s;
}

struct Harness {
    MeshPtr scalp = test::makePlane(4);
    ExternalInputs ext;
    EvalContext ctx;
    Harness() {
        ext.set("scalp", scalp, scalp->contentHash());
        ctx.externals = &ext;
    }
    StrandsPtr run(const char* type, const std::vector<Value>& inputs, const std::map<std::string, ParamValue>& params) {
        auto op = OperatorRegistry::instance().create(type);
        for (const auto& [k, v] : params) op->params().set(k, v);
        return valueAs<StrandsPtr>(op->evaluate(ctx, inputs));
    }
};

float segmentLength(const StrandSet& s, std::size_t strand, std::size_t i) {
    return (s.strandPoints(strand)[i] - s.strandPoints(strand)[i - 1]).length();
}

// The Comb node Bend replaced: each segment's direction blended toward `dir` by strength x the
// profile at its end point, renormalized, its length kept; the root stays.
std::vector<Vec3f> oldComb(const Vec3f* pts, std::size_t n, Vec3f dir, float strength, const Ramp& profile) {
    std::vector<Vec3f> out(pts, pts + n);
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f seg = pts[i] - pts[i - 1];
        const float len = seg.length();
        const float w = std::clamp(strength * profile(static_cast<float>(i) / static_cast<float>(n - 1)), 0.0f, 1.0f);
        out[i] = out[i - 1] + (seg / len * (1.0f - w) + dir * w).normalized() * len;
    }
    return out;
}

float maxDistance(const StrandSet& a, std::size_t s, const std::vector<Vec3f>& b) {
    float d = 0.0f;
    for (std::size_t i = 0; i < b.size(); ++i) d = std::max(d, (a.strandPoints(s)[i] - b[i]).length());
    return d;
}

// The angle Comb's blend by k turned a segment by, from its angle theta to the direction.
double combDegrees(float k, float theta) {
    return std::atan2(k * std::sin(theta), 1.0f - k + k * std::cos(theta)) * 180.0 / 3.14159265358979;
}

TEST(Bend, CombsAsTheOldCombDid) {
    Harness h;
    const float tilt = 0.6f;  // radians from the plane's normal (+Y), leaning toward -Z
    const Vec3f way{0.0f, std::cos(tilt), -std::sin(tilt)};
    const StrandsPtr in = strandsOn(*h.scalp, way);
    // The direction laid onto the plane (+Y normal), as Comb's alongSurface laid it: +X.
    const Vec3f direction{1.0f, -1.0f, 0.0f}, laid{1.0f, 0.0f, 0.0f};
    const float length = 1.0f;  // test::straightCurve's
    const float theta = std::acos(way.dot(laid));

    // Strength 1 over the whole length (the scales presets' comb): angle 180 with start and end 0,
    // never past the direction, points every segment along it.
    const StrandsPtr full = h.run(op::kBend, {in}, {{"angle", 180.0}, {"start", 0.0}, {"end", 0.0},
                                                    {"along", std::string("direction")}, {"direction", direction}});
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        EXPECT_LT(maxDistance(*full, s, oldComb(in->strandPoints(s), in->strandPointCount(s), laid, 1.0f, Ramp::constant(1.0f))),
                  1e-5f * length);
    }
    // A constant strength k: on a straight strand every segment turns by the same angle.
    for (const float k : {0.3f, 0.7f}) {
        const StrandsPtr part = h.run(op::kBend, {in}, {{"angle", combDegrees(k, theta)}, {"start", 0.0}, {"end", 0.0},
                                                        {"along", std::string("direction")}, {"direction", direction}});
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            EXPECT_LT(maxDistance(*part, s, oldComb(in->strandPoints(s), in->strandPointCount(s), laid, k, Ramp::constant(1.0f))),
                      1e-5f * length)
                << k;
        }
    }
    // Comb's default, strength 0.9 growing from the root (a linear profile): start 0, end 1 and
    // the angle its tip turned by. Comb's blend turned the middle a little less than the even
    // bend (its angle is not linear in the blend): within 4% of the length.
    const float k = 0.9f;
    const StrandsPtr ramped = h.run(op::kBend, {in}, {{"angle", combDegrees(k, theta)}, {"along", std::string("direction")},
                                                      {"direction", direction}});
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const std::vector<Vec3f> comb = oldComb(in->strandPoints(s), in->strandPointCount(s), laid, k, Ramp::linear(0.0f, 1.0f));
        EXPECT_LT(maxDistance(*ramped, s, comb), 0.04f * length);
        EXPECT_GT(maxDistance(*in, s, comb), 0.3f * length);  // a real bend
        EXPECT_EQ(ramped->strandPoints(s)[0], in->strandPoints(s)[0]);  // roots stay
        for (std::size_t i = 1; i < in->strandPointCount(s); ++i) {
            EXPECT_NEAR(segmentLength(*ramped, s, i), segmentLength(*in, s, i), 1e-5f);
        }
    }
}

TEST(Bend, NeverPastTheDirectionAndLaidOntoTheScalp) {
    Harness h;
    const StrandsPtr in = strandsOn(*h.scalp, {0.0f, 1.0f, 0.0f});
    // However big the angle, a strand stops along the direction (+X), and bending away stops at
    // its opposite.
    for (const double degrees : {120.0, 180.0, 400.0}) {
        const StrandsPtr out = h.run(op::kBend, {in}, {{"angle", degrees}, {"end", 0.0}, {"along", std::string("direction")},
                                                       {"direction", Vec3f{1, 0, 0}}});
        const Vec3f tip = (out->strandPoints(0)[8] - out->strandPoints(0)[0]).normalized();
        EXPECT_NEAR(tip.x, 1.0f, 1e-4f) << degrees;
    }
    const StrandsPtr away = h.run(op::kBend, {in}, {{"angle", -400.0}, {"end", 0.0}, {"along", std::string("direction")},
                                                    {"direction", Vec3f{1, 0, 0}}});
    EXPECT_NEAR((away->strandPoints(0)[8] - away->strandPoints(0)[0]).normalized().x, -1.0f, 1e-4f);
    // Straight down onto a +Y scalp: nothing left of the direction laid onto it, so no bend.
    const StrandsPtr flat = h.run(op::kBend, {in}, {{"angle", 90.0}, {"along", std::string("direction")},
                                                    {"direction", Vec3f{0, -1, 0}}});
    EXPECT_EQ(flat->positions(), in->positions());
    // The direction's part along the normal does not count: (1, -5, 0) bends toward +X.
    const StrandsPtr steep = h.run(op::kBend, {in}, {{"angle", 180.0}, {"end", 0.0}, {"along", std::string("direction")},
                                                     {"direction", Vec3f{1, -5, 0}}});
    EXPECT_NEAR((steep->strandPoints(0)[8] - steep->strandPoints(0)[0]).normalized().x, 1.0f, 1e-4f);
    // Angle 0 leaves strands alone.
    EXPECT_EQ(h.run(op::kBend, {in}, {{"angle", 0.0}})->positions(), in->positions());
}

TEST(Collide, PushesStrandsOutOfTheScalp) {
    Harness h;
    const StrandsPtr in = strandsOn(*h.scalp, Vec3f{0.3f, -0.3f, 0.0f});  // sinking below the plane
    const StrandsPtr out = h.run(op::kCollide, {in, h.scalp}, {{"offset", 0.02}, {"iterations", std::int64_t{4}}});
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        EXPECT_EQ(out->strandPoints(s)[0], in->strandPoints(s)[0]);
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            EXPECT_GT(out->strandPoints(s)[i].y, 0.0f) << "strand " << s << " point " << i;
            EXPECT_NEAR(segmentLength(*out, s, i), segmentLength(*in, s, i), 0.05f * segmentLength(*in, s, i));
        }
    }
    // Deeper than maxDepth: left alone.
    const StrandsPtr deep = strandsOn(*h.scalp, Vec3f{0.0f, -3.0f, 0.0f});
    const StrandsPtr kept = h.run(op::kCollide, {deep, h.scalp}, {{"maxDepth", 0.2}});
    EXPECT_LT(kept->strandPoints(0)[8].y, -2.0f);
    // No scalp: unchanged.
    EXPECT_EQ(h.run(op::kCollide, {in, Value{}}, {})->positions(), in->positions());
}

// A sphere of `radius` about `center`.
MeshPtr sphereAt(const Vec3f& center, float radius) {
    const auto unit = test::makeSphere(16, 32, radius);
    std::vector<Vec3f> points = unit->points();
    for (Vec3f& p : points) p += center;
    return std::make_shared<const ScalpMesh>(std::move(points), unit->triangles());
}

TEST(Collide, PushesStrandsOutOfAColliderMesh) {
    Harness h;
    const StrandsPtr in = strandsOn(*h.scalp, Vec3f{0.0f, 1.0f, 0.0f});  // straight up from the plane
    const Vec3f center{0.5f, 0.5f, 0.5f};
    const MeshPtr ball = sphereAt(center, 0.3f);
    std::size_t inside = 0;
    for (const Vec3f& p : in->positions()) inside += (p - center).length() < 0.3f;
    ASSERT_GT(inside, 4u);  // the ball sits in the hair

    // The collider alone, and with the scalp: every point ends up outside the ball.
    for (const StrandsPtr& out : {h.run(op::kCollide, {in, Value{}, ball}, {{"offset", 0.02}, {"iterations", std::int64_t{4}}}),
                                  h.run(op::kCollide, {in, h.scalp, ball}, {{"offset", 0.02}, {"iterations", std::int64_t{4}}})}) {
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            EXPECT_EQ(out->strandPoints(s)[0], in->strandPoints(s)[0]);
            for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
                EXPECT_GT((out->strandPoints(s)[i] - center).length(), 0.3f + 0.02f - 2e-3f) << "strand " << s << " point " << i;
                EXPECT_GT(out->strandPoints(s)[i].y, 0.0f);
                EXPECT_NEAR(segmentLength(*out, s, i), segmentLength(*in, s, i), 0.05f * segmentLength(*in, s, i));
            }
        }
    }
    // Only the scalp: the ball is not there, nothing moves (the strands stand clear of the plane).
    EXPECT_EQ(h.run(op::kCollide, {in, h.scalp}, {})->positions(), in->positions());
}

// Strands rooted on the plane slanting down through a floor half a unit below it (a ColliderMesh's
// plane), finely sampled: where they meet it a hard push folds them flat.
struct FloorScene {
    Harness h;
    MeshPtr floor;
    StrandsPtr in;
    FloorScene() {
        const auto plane = test::makePlane(8, 4.0f);
        std::vector<Vec3f> points = plane->points();
        for (Vec3f& p : points) p += Vec3f(-1.0f, -0.5f, -1.0f);
        floor = std::make_shared<const ScalpMesh>(std::move(points), plane->triangles());
        auto s = std::make_shared<StrandSet>();
        for (int i = 0; i < 12; ++i) {
            const StrandRoot root{2 * i % static_cast<int>(h.scalp->triangleCount()), 0.3f, 0.3f};
            s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve(h.scalp->position(root), Vec3f(0.5f, -1.0f, 0.0f), 32), {}, root);
        }
        in = s;
    }
    StrandsPtr collide(double softness) {
        return h.run(op::kCollide, {in, Value{}, floor}, {{"offset", 0.02}, {"softness", softness}, {"iterations", std::int64_t{4}}});
    }
};

// The sharpest turn between two segments of any strand (radians).
float sharpestTurn(const StrandSet& s) {
    float most = 0.0f;
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        const Vec3f* p = s.strandPoints(k);
        for (std::size_t i = 2; i < s.strandPointCount(k); ++i) {
            const float c = std::clamp((p[i - 1] - p[i - 2]).normalized().dot((p[i] - p[i - 1]).normalized()), -1.0f, 1.0f);
            most = std::max(most, std::acos(c));
        }
    }
    return most;
}

TEST(Collide, SoftnessCurvesStrandsOntoTheSurfaceKeepingLengths) {
    // Softness: the correction fades toward the root over that length (faded in from the root),
    // the strand turned segment by segment (lengths kept): no point inside the offset nor floated
    // above it, the fold spread over many segments, the strand lifted before where it meets the
    // floor; a softness longer than the strand's way to the floor kinks nothing at the root.
    FloorScene scene;
    const StrandsPtr hard = scene.collide(0.0);
    for (const double softness : {0.2, 0.6, 1.2, 3.0}) {
        SCOPED_TRACE(softness);
        const StrandsPtr soft = scene.collide(softness);
        bool liftedBefore = false;
        float lowest = 1e9f;
        for (std::size_t s = 0; s < soft->strandCount(); ++s) {
            EXPECT_EQ(soft->strandPoints(s)[0], scene.in->strandPoints(s)[0]);
            for (std::size_t i = 1; i < soft->strandPointCount(s); ++i) {
                lowest = std::min(lowest, soft->strandPoints(s)[i].y);
                EXPECT_GT(soft->strandPoints(s)[i].y, -0.5f + 0.02f - 2e-3f) << "strand " << s << " point " << i;
                EXPECT_NEAR(segmentLength(*soft, s, i), segmentLength(*scene.in, s, i), 0.1f * segmentLength(*scene.in, s, i));  // (the tip pushed again)
                // A point the hard push left where it was (above the floor) lifted ahead of the fold.
                if (hard->strandPoints(s)[i] == scene.in->strandPoints(s)[i] && soft->strandPoints(s)[i].y > hard->strandPoints(s)[i].y + 1e-3f) {
                    liftedBefore = true;
                }
            }
        }
        std::printf("[ collide  ] softness %.1f: sharpest turn %.3f rad (hard %.3f), lowest %.4f above the floor\n", softness,
                    sharpestTurn(*soft), sharpestTurn(*hard), lowest + 0.5f);
        EXPECT_TRUE(liftedBefore);
        EXPECT_LT(lowest, -0.5f + 0.02f + 0.01f);  // resting on the floor at the offset, not floating
        EXPECT_LT(sharpestTurn(*soft), 0.5f * sharpestTurn(*hard)) << sharpestTurn(*soft) << " against " << sharpestTurn(*hard);
    }
    // It acts on the collider alone: strands sinking into the scalp (which they grow from) and never
    // reaching the floor walk as the hard push walks them.
    const StrandsPtr sinking = strandsOn(*scene.h.scalp, Vec3f{0.3f, -0.3f, 0.0f});
    auto run = [&](double softness) {
        return scene.h.run(op::kCollide, {sinking, scene.h.scalp, scene.floor},
                           {{"offset", 0.02}, {"softness", softness}, {"iterations", std::int64_t{4}}});
    };
    const StrandsPtr scalpHard = run(0.0);
    EXPECT_NE(scalpHard->positions(), sinking->positions());
    EXPECT_EQ(run(0.6)->positions(), scalpHard->positions());
}

TEST(ColliderMesh, ReadsTheNamedMeshExternal) {
    Harness h;
    const MeshPtr ball = sphereAt({0.5f, 0.5f, 0.5f}, 0.3f);
    auto op = OperatorRegistry::instance().create(op::kColliderMesh);
    EXPECT_EQ(op->externalInputs(), std::vector<std::string>{"mesh:collider"});  // the default name
    EXPECT_EQ(valueAs<MeshPtr>(op->evaluate(h.ctx, {})), nullptr);  // no such mesh yet
    h.ext.set("mesh:collider", ball, ball->contentHash());
    EXPECT_EQ(valueAs<MeshPtr>(op->evaluate(h.ctx, {})), ball);
    op->params().set("name", std::string("hat"));
    EXPECT_EQ(op->externalInputs(), std::vector<std::string>{"mesh:hat"});
    EXPECT_EQ(valueAs<MeshPtr>(op->evaluate(h.ctx, {})), nullptr);

    // Through a groom: GroomData::meshes reaches ColliderMesh, and Collide keeps the hair out of it.
    const char* graph = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
        {"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},
        {"name": "collide", "type": "Collide", "params": {"offset": 0.02}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "collide", "input": "strands"},
                      {"from": "scalp", "to": "collide", "input": "scalp"},
                      {"from": "ball", "to": "collide", "input": "collider"},
                      {"from": "collide", "to": "out", "input": "strands"}]})";
    GroomData d;
    d.scalp = h.scalp;
    d.graphJson = graph;
    Groom groom(d);
    const Vec3f center{0.5f, 0.5f, 0.5f};
    auto insideCount = [&](const StrandsPtr& s) {
        std::size_t inside = 0;
        for (const Vec3f& p : s->positions()) inside += (p - center).length() < 0.3f;
        return inside;
    };
    EXPECT_GT(insideCount(groom.evaluate(1.0f, 0.0)), 10u);  // no collider registered: hair through the ball
    groom.setMesh("collider", ball);
    EXPECT_EQ(insideCount(groom.evaluate(1.0f, 0.0)), 0u);
    groom.setMesh("collider", nullptr);
    EXPECT_GT(insideCount(groom.evaluate(1.0f, 0.0)), 10u);
}

// Straight strands of `segments` segments from roots at the given (x, z) on the plane, each along its `dir`.
StrandsPtr tiltedStrands(const ScalpMesh& plane, const std::vector<std::pair<Vec3f, Vec3f>>& rootsAndDirs, int segments = 8) {
    auto s = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (const auto& [at, dir] : rootsAndDirs) {
        const StrandRoot root = plane.closestPoint(at).root;
        s->addStrand(id++, test::straightCurve(plane.position(root), dir.normalized(), segments), {}, root);
    }
    return s;
}

Vec3f tipOffset(const StrandSet& s, std::size_t strand) {
    return s.strandPoints(strand)[s.strandPointCount(strand) - 1] - s.strandPoints(strand)[0];
}

// Mean squared distance of the tip offsets from their mean.
float tipSpread(const StrandSet& s) {
    Vec3f mean;
    for (std::size_t i = 0; i < s.strandCount(); ++i) mean += tipOffset(s, i);
    mean = mean / static_cast<float>(s.strandCount());
    float spread = 0.0f;
    for (std::size_t i = 0; i < s.strandCount(); ++i) spread += (tipOffset(s, i) - mean).dot(tipOffset(s, i) - mean);
    return spread / static_cast<float>(s.strandCount());
}

TEST(BlendNeighbors, PullsNoisyStrandsTogetherKeepingRootsAndLengths) {
    Harness h;
    Rng rng(5);
    std::vector<std::pair<Vec3f, Vec3f>> layout;
    for (int x = 0; x < 6; ++x) {
        for (int z = 0; z < 6; ++z) {
            layout.push_back({{0.05f + 0.18f * x, 0.0f, 0.05f + 0.18f * z}, {rng.uniform(-0.5f, 0.5f), 1.0f, rng.uniform(-0.5f, 0.5f)}});
        }
    }
    const StrandsPtr in = tiltedStrands(*h.scalp, layout);
    const StrandsPtr out = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"neighbors", std::int64_t{6}}});
    ASSERT_EQ(out->strandCount(), in->strandCount());
    EXPECT_LT(tipSpread(*out), 0.6f * tipSpread(*in));
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        EXPECT_EQ(out->strandPoints(s)[0], in->strandPoints(s)[0]);  // roots stay
        EXPECT_NEAR(arcLength(out->strandPoints(s), out->strandPointCount(s)), arcLength(in->strandPoints(s), in->strandPointCount(s)), 1e-4f);
    }
    EXPECT_NE(out->positions(), in->positions());
    // Zero amount: untouched. A tip-only profile keeps the lower half where it was.
    EXPECT_EQ(h.run(op::kBlendNeighbors, {in}, {{"strength", 0.0}})->positions(), in->positions());
    const StrandsPtr tips = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"preserveLength", false},
                                                              {"profile", Ramp::linear(0.0f, 1.0f)}});
    EXPECT_NEAR((tips->strandPoints(0)[1] - in->strandPoints(0)[1]).length(), 0.0f, 0.03f);
    EXPECT_GT((tips->strandPoints(0)[8] - in->strandPoints(0)[8]).length(), 0.05f);
    // Without preserveLength, blending tilted strands shortens them a little.
    const StrandsPtr loose = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"preserveLength", false}});
    float shortened = 0;
    for (std::size_t s = 0; s < loose->strandCount(); ++s) shortened += arcLength(loose->strandPoints(s), 9) < arcLength(in->strandPoints(s), 9) - 1e-4f;
    EXPECT_GT(shortened, 0);
}

TEST(BlendNeighbors, RespectsParts) {
    Harness h;
    // A part down the middle of the plane (x = 0.5); strands lean +Z on the left, -Z on the right.
    std::vector<Vec3f> line;
    for (int i = 0; i <= 10; ++i) line.push_back({0.5f, 0.0f, 0.1f * i});
    PartSet set;
    set.parts.push_back(PartSet::fromPoints("mid", line, *h.scalp));
    const auto lines = std::make_shared<const PartLines>(set, *h.scalp);
    std::vector<std::pair<Vec3f, Vec3f>> layout;
    for (int x = 1; x <= 4; ++x) {
        for (float z : {0.3f, 0.5f, 0.7f}) {
            layout.push_back({{0.1f * x, 0.0f, z}, {0.0f, 1.0f, 0.5f}});
            layout.push_back({{0.5f + 0.1f * x, 0.0f, z}, {0.0f, 1.0f, -0.5f}});
        }
    }
    const StrandsPtr in = tiltedStrands(*h.scalp, layout);
    // Without parts, every strand leans toward its neighbours across the middle.
    const StrandsPtr mixed = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"neighbors", std::int64_t{16}}});
    std::size_t straightened = 0;
    for (std::size_t s = 0; s < mixed->strandCount(); ++s) {
        straightened += std::abs(tipOffset(*mixed, s).z) < 0.9f * std::abs(tipOffset(*in, s).z);
    }
    EXPECT_GT(straightened, in->strandCount() / 2);
    // With the part: each side only sees its own, so every strand keeps its lean.
    h.ext.set("parts", PartsPtr(lines), lines->contentHash());
    const StrandsPtr parted = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"neighbors", std::int64_t{16}}});
    for (std::size_t s = 0; s < parted->strandCount(); ++s) {
        EXPECT_NEAR(tipOffset(*parted, s).z, tipOffset(*in, s).z, 1e-4f) << "strand " << s;
    }
    const StrandsPtr ignored = h.run(op::kBlendNeighbors, {in}, {{"strength", 1.0}, {"neighbors", std::int64_t{16}}, {"respectParts", false}});
    EXPECT_EQ(ignored->positions(), mixed->positions());
}

TEST(RestoreLength, RestoresSegmentLengthsOrTheWholeLengthFromTheReference) {
    Harness h;
    std::vector<std::pair<Vec3f, Vec3f>> layout;
    for (int i = 0; i < 12; ++i) layout.push_back({{0.05f + 0.08f * i, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}});
    const StrandsPtr reference = tiltedStrands(*h.scalp, layout);  // 8 segments of 1/8
    // Deformed: stretched 1.5x and bent, same IDs and roots.
    auto bent = std::make_shared<StrandSet>(*reference);
    Rng rng(9);
    for (std::size_t s = 0; s < bent->strandCount(); ++s) {
        Vec3f* pts = bent->strandPoints(s);
        for (std::size_t i = 1; i < 9; ++i) {
            pts[i] = pts[0] + (pts[i] - pts[0]) * 1.5f + Vec3f{rng.uniform(-0.1f, 0.1f), 0.0f, rng.uniform(-0.1f, 0.1f)};
        }
    }
    const StrandsPtr deformed = bent;

    const StrandsPtr segments = h.run(op::kRestoreLength, {deformed, reference}, {});
    for (std::size_t s = 0; s < segments->strandCount(); ++s) {
        EXPECT_EQ(segments->strandPoints(s)[0], deformed->strandPoints(s)[0]);
        for (std::size_t i = 1; i < 9; ++i) EXPECT_NEAR(segmentLength(*segments, s, i), 0.125f, 1e-5f);
        // The first segment keeps the deformed direction.
        const Vec3f want = (deformed->strandPoints(s)[1] - deformed->strandPoints(s)[0]).normalized();
        const Vec3f got = (segments->strandPoints(s)[1] - segments->strandPoints(s)[0]).normalized();
        EXPECT_NEAR(want.dot(got), 1.0f, 1e-5f);
    }

    const StrandsPtr scaled = h.run(op::kRestoreLength, {deformed, reference}, {{"mode", std::string("scale")}});
    for (std::size_t s = 0; s < scaled->strandCount(); ++s) {
        EXPECT_NEAR(arcLength(scaled->strandPoints(s), 9), 1.0f, 1e-4f);
        // The deformed shape, scaled about the root.
        const Vec3f a = tipOffset(*scaled, s).normalized(), b = tipOffset(*deformed, s).normalized();
        EXPECT_NEAR(a.dot(b), 1.0f, 1e-5f);
    }

    // Matched by ID: a reference without this strand's ID leaves it alone.
    const StrandsPtr others = std::make_shared<StrandSet>(reference->subset({0, 1, 2}));
    const StrandsPtr partial = h.run(op::kRestoreLength, {deformed, others}, {});
    EXPECT_NEAR(segmentLength(*partial, 1, 3), 0.125f, 1e-5f);
    EXPECT_EQ(partial->strandPoints(5)[8], deformed->strandPoints(5)[8]);
    // No reference: nothing to restore from.
    EXPECT_EQ(h.run(op::kRestoreLength, {deformed, Value{}}, {})->positions(), deformed->positions());
    EXPECT_THROW(h.run(op::kRestoreLength, {deformed, reference}, {{"mode", std::string("stretch")}}), GraphError);
}

TEST(RestoreLength, EvenModeRespacesEachStrandKeepingItsLength) {
    Harness h;
    auto uneven = std::make_shared<StrandSet>();
    // Points bunched toward the root along a bent curve.
    for (int s = 0; s < 5; ++s) {
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            const float u = t * t;  // bunched
            pts.push_back({0.1f + 0.2f * s + 0.3f * u * u, u, 0.5f});
        }
        uneven->addStrand(static_cast<StrandId>(s + 1), pts, {}, h.scalp->closestPoint(pts[0]).root);
    }
    const StrandsPtr out = h.run(op::kRestoreLength, {uneven, Value{}}, {{"mode", std::string("even")}});
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        EXPECT_EQ(out->strandPoints(s)[0], uneven->strandPoints(s)[0]);
        const float total = arcLength(uneven->strandPoints(s), 9);
        EXPECT_NEAR(arcLength(out->strandPoints(s), 9), total, 2e-3f);  // straight chords of a curve: a little under
        for (std::size_t i = 1; i < 9; ++i) EXPECT_NEAR(segmentLength(*out, s, i), total / 8.0f, 2e-3f);
        EXPECT_GT(segmentLength(*out, s, 1), 2.0f * segmentLength(*uneven, s, 1));  // was bunched at the root
    }
}

TEST(Performance, GravityAndCollideAt100kStrands) {
    // Not in the spec's budget; printed so the docs can quote it.
    auto scalp = test::makeSphere(60, 120, 1.0f);
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    ctx.strandSegments = 15;  // the Output node's segments, as the graph passes them
    auto scatter = OperatorRegistry::instance().create(op::kScatter);
    scatter->params().set("density", 8000.0);
    const StrandsPtr strands = valueAs<StrandsPtr>(scatter->evaluate(ctx, {MeshPtr(scalp)}));
    ASSERT_GT(strands->strandCount(), 90000u);

    auto gravity = OperatorRegistry::instance().create(op::kGravity);
    gravity->params().set("strength", 3.0);
    gravity->params().set("stiffness", 0.0);  // hanging straight down: the upper half dips into the sphere
    auto t0 = std::chrono::steady_clock::now();
    const StrandsPtr hung = valueAs<StrandsPtr>(gravity->evaluate(ctx, {strands}));
    const double gravityMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    auto collide = OperatorRegistry::instance().create(op::kCollide);
    t0 = std::chrono::steady_clock::now();
    const StrandsPtr collided = valueAs<StrandsPtr>(collide->evaluate(ctx, {hung, MeshPtr(scalp)}));
    const double collideMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ perf ] %zu strands x 16: Gravity %.1f ms, Collide %.1f ms\n", strands->strandCount(), gravityMs, collideMs);
    EXPECT_LT(collideMs, 5000.0);

    // Hanging, strands dip under the surface; Collide lifts them out.
    std::size_t under = 0, still = 0;
    for (std::size_t i = 0; i < hung->pointCount(); ++i) under += hung->positions()[i].length() < 0.999f;
    for (std::size_t i = 0; i < collided->pointCount(); ++i) still += collided->positions()[i].length() < 0.999f;
    EXPECT_GT(under, 1000u);
    EXPECT_LT(still, under / 20);
}

}  // namespace
}  // namespace groomlab
