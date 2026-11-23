// SPDX-License-Identifier: Apache-2.0
// SkinResponse and the skin strain it reads (skin_strain.h): a band of a plane squashed or
// stretched across x, the rest of it as it was. Strands in the band rise or lie down by the angles
// asked; strands beside a stretched band lean away from it; skin that did not change, a uniform
// scale and a scalp at rest leave the strands as they are.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/math.h"
#include "groomlab/operators.h"
#include "groomlab/skin_strain.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kDegrees = 57.29577951308232f;

// The 20 x 20 cell unit plane (normal +y) with the band 0.4 <= x <= 0.6 scaled by `band` across x
// (0.5: squashed to half, 1.5: stretched by half), what lies beyond moved along with it; its rest
// pose the plane as made. Everything times `size`.
std::shared_ptr<const ScalpMesh> bandPlane(float band, float size = 1.0f) {
    const auto plane = test::makePlane(20, 1.0f);
    std::vector<Vec3f> rest = plane->points(), posed = rest;
    for (std::size_t i = 0; i < posed.size(); ++i) {
        const float x = rest[i].x;
        posed[i].x = x < 0.4f ? x : x <= 0.6f ? 0.4f + (x - 0.4f) * band : 0.4f + 0.2f * band + (x - 0.6f);
        rest[i] = rest[i] * size;
        posed[i] = posed[i] * size;
    }
    auto mesh = std::make_shared<ScalpMesh>(posed, plane->triangles());
    mesh->setRestPoints(rest);
    return mesh;
}

// One strand per (x, z) on `scalp` (x, z of the rest plane: placed where that point is now), leaning
// `elevation` degrees up from the skin toward +z, `length` long, 8 segments.
StrandsPtr strandsAt(const ScalpMesh& scalp, const std::vector<std::pair<float, float>>& at, float elevation, float length = 0.3f) {
    auto out = std::make_shared<StrandSet>();
    const float a = elevation / kDegrees;
    const Vec3f dir = Vec3f(0.0f, std::sin(a), std::cos(a)) * length;
    StrandId id = 1;
    for (const auto& [x, z] : at) {
        const SurfacePoint near = scalp.restPose().closestPoint(Vec3f(x, 0.0f, z) * (scalp.restPoints()[1].x / 0.05f));
        const Vec3f root = scalp.position(near.root);
        out->addStrand(id++, test::straightCurve(root, dir, 8), {}, near.root);
    }
    return out;
}

using Params = std::vector<std::pair<std::string, ParamValue>>;

StrandsPtr run(const StrandsPtr& in, const MeshPtr& scalp, const Params& params, const MeshPtr& pose = nullptr) {
    auto op = OperatorRegistry::instance().create(op::kSkinResponse);
    for (const auto& [name, value] : params) op->params().set(name, value);
    ExternalInputs externals;
    if (scalp) externals.set("scalp", scalp, scalp->surfaceHash());
    if (pose) externals.set(kScalpPoseExternal, pose, pose->surfaceHash());
    EvalContext ctx;
    ctx.externals = &externals;
    return valueAs<StrandsPtr>(op->evaluate(ctx, {Value(in)}));
}

// Degrees a strand's root-to-tip line stands above the plane (+y).
float elevationOf(const StrandSet& strands, std::size_t s) {
    const std::size_t n = strands.strandPointCount(s);
    const Vec3f d = strands.strandPoints(s)[n - 1] - strands.strandPoints(s)[0];
    return std::atan2(d.y, std::hypot(d.x, d.z)) * kDegrees;
}

float largestMove(const StrandSet& a, const StrandSet& b, std::size_t s) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.strandPointCount(s); ++i) {
        worst = std::max(worst, (a.strandPoints(s)[i] - b.strandPoints(s)[i]).length());
    }
    return worst;
}

const Params kOnlyLift{{"lift", 25.0}, {"flatten", 0.0}, {"spread", 0.0}, {"smoothing", 0.0}};
const Params kOnlyFlatten{{"lift", 0.0}, {"flatten", 10.0}, {"spread", 0.0}, {"smoothing", 0.0}};
const Params kOnlySpread{{"lift", 0.0}, {"flatten", 0.0}, {"spread", 15.0}, {"smoothing", 0.0}};

TEST(SkinStrain, ABandSquashedOrStretchedReadsAsSuch) {
    const auto squashed = bandPlane(0.5f);
    const SkinStrain s = skinStrain(*squashed, 0);
    ASSERT_FALSE(s.empty());
    // A vertex inside the band (x 0.5, z 0.5): only band triangles around it.
    const std::size_t inside = 10 * 21 + 10, outside = 10 * 21 + 2;
    EXPECT_NEAR(s.squash[inside], 0.5f, 1e-5f);
    EXPECT_NEAR(s.stretch[inside], 0.0f, 1e-5f);
    EXPECT_NEAR(s.squash[outside], 0.0f, 1e-6f);

    const SkinStrain t = skinStrain(*bandPlane(1.5f), 0);
    EXPECT_NEAR(t.stretch[inside], 0.5f, 1e-5f);
    EXPECT_NEAR(t.squash[inside], 0.0f, 1e-5f);

    // Relaxed: spread to the neighbours, lower at the middle.
    const SkinStrain relaxed = skinStrain(*squashed, 8);
    EXPECT_LT(relaxed.squash[inside], s.squash[inside]);
    EXPECT_GT(relaxed.squash[10 * 21 + 7], 0.0f);
}

TEST(SkinStrain, AtRestScaledOrWithoutARestPoseThereIsNone) {
    const auto plane = test::makePlane(6, 1.0f);
    EXPECT_TRUE(skinStrain(*plane, 0).empty());  // no rest pose
    ScalpMesh atRest(plane->points(), plane->triangles());
    atRest.setRestPoints(plane->points());
    EXPECT_TRUE(skinStrain(atRest, 0).empty());

    // Scaled whole (a rig scaled at its root): no strain.
    std::vector<Vec3f> scaled = plane->points();
    for (Vec3f& p : scaled) p = p * 3.0f;
    ScalpMesh big(scaled, plane->triangles());
    big.setRestPoints(plane->points());
    const SkinStrain s = skinStrain(big, 0);
    ASSERT_FALSE(s.empty());
    for (std::size_t v = 0; v < s.squash.size(); ++v) {
        EXPECT_NEAR(s.squash[v], 0.0f, 1e-5f);
        EXPECT_NEAR(s.stretch[v], 0.0f, 1e-5f);
    }
}

TEST(SkinResponse, SquashedSkinLiftsTheStrands) {
    const auto scalp = bandPlane(0.5f);
    const StrandsPtr in = strandsAt(*scalp, {{0.5f, 0.52f}, {0.1f, 0.52f}}, 45.0f);
    const StrandsPtr out = run(in, scalp, kOnlyLift);
    EXPECT_NEAR(elevationOf(*out, 0), 70.0f, 0.2f) << "in the band: up by lift (squash 0.5 past range 0.3: full)";
    EXPECT_LT(largestMove(*out, *in, 1), 1e-5f) << "beside it, where the skin did not change: as it was";
    const std::vector<float>* squash = out->strandChannel("skinSquash");
    ASSERT_NE(squash, nullptr);
    EXPECT_FLOAT_EQ((*squash)[0], 1.0f);
    EXPECT_NEAR((*squash)[1], 0.0f, 1e-5f);
    ASSERT_NE(out->strandChannel("skinStretch"), nullptr);

    // Never past the normal: a strand nearly upright stops there.
    const StrandsPtr steep = run(strandsAt(*scalp, {{0.5f, 0.52f}}, 80.0f), scalp, kOnlyLift);
    EXPECT_NEAR(elevationOf(*steep, 0), 90.0f, 0.2f);

    // Half the strength, half the turn (mappable strength, here a number).
    const StrandsPtr half = run(in, scalp, {{"lift", 25.0}, {"flatten", 0.0}, {"spread", 0.0}, {"smoothing", 0.0}, {"strength", 0.5}});
    EXPECT_NEAR(elevationOf(*half, 0), 57.5f, 0.2f);
}

TEST(SkinResponse, StretchedSkinLaysTheStrandsDown) {
    const auto scalp = bandPlane(1.5f);
    const StrandsPtr in = strandsAt(*scalp, {{0.5f, 0.52f}}, 45.0f);
    EXPECT_NEAR(elevationOf(*run(in, scalp, kOnlyFlatten), 0), 35.0f, 0.2f);
    // Never below the skin.
    const StrandsPtr low = run(strandsAt(*scalp, {{0.5f, 0.52f}}, 5.0f), scalp, kOnlyFlatten);
    EXPECT_NEAR(elevationOf(*low, 0), 0.0f, 0.2f);
}

TEST(SkinResponse, StrandsBesideAStretchLeanAwayFromIt) {
    const auto scalp = bandPlane(1.5f);
    // Upright strands just outside the band's two edges (rest x 0.35 and 0.65).
    const StrandsPtr in = strandsAt(*scalp, {{0.375f, 0.52f}, {0.625f, 0.52f}}, 90.0f);
    const StrandsPtr out = run(in, scalp, kOnlySpread);
    const std::size_t n = out->strandPointCount(0);
    const float left = out->strandPoints(0)[n - 1].x - in->strandPoints(0)[n - 1].x;
    const float right = out->strandPoints(1)[n - 1].x - in->strandPoints(1)[n - 1].x;
    EXPECT_LT(left, -0.01f) << "the left one leans left, away from the band";
    EXPECT_GT(right, 0.01f) << "the right one leans right";
    EXPECT_LE(std::abs(elevationOf(*out, 0) - 90.0f), 15.0f + 0.2f);  // by at most spread
}

TEST(SkinResponse, KeepsSegmentLengthsWithAnyProfile) {
    const auto scalp = bandPlane(0.5f);
    const StrandsPtr in = strandsAt(*scalp, {{0.5f, 0.52f}}, 30.0f);
    const StrandsPtr out =
        run(in, scalp, {{"lift", 40.0}, {"smoothing", 0.0}, {"profile", Ramp{{0.0f, 0.0f}, {1.0f, 1.0f}}}});
    EXPECT_GT(largestMove(*out, *in, 0), 1e-3f);
    for (std::size_t i = 1; i < out->strandPointCount(0); ++i) {
        const float before = (in->strandPoints(0)[i] - in->strandPoints(0)[i - 1]).length();
        const float after = (out->strandPoints(0)[i] - out->strandPoints(0)[i - 1]).length();
        EXPECT_NEAR(after, before, 1e-5f);
    }
    EXPECT_EQ(out->strandPoints(0)[0], in->strandPoints(0)[0]);  // the root stays
}

TEST(SkinResponse, AtRestOrWithoutARestPoseThePointsPassThrough) {
    const auto plane = test::makePlane(8, 1.0f);
    const StrandsPtr in = strandsAt(*bandPlane(1.0f), {{0.5f, 0.52f}}, 45.0f);
    // No rest pose: the input itself.
    EXPECT_EQ(run(in, plane, {}), in);
    // At rest: the points to the bit, the channels 0.
    const auto atRest = bandPlane(1.0f);
    const StrandsPtr out = run(in, atRest, {});
    EXPECT_EQ(out->positions(), in->positions());
    ASSERT_NE(out->strandChannel("skinSquash"), nullptr);
    EXPECT_EQ((*out->strandChannel("skinSquash"))[0], 0.0f);
}

TEST(SkinResponse, ReadsThePoseFromItsOwnExternalOverTheRestPose) {
    // Following the scalp: "scalp" is the rest pose, "scalpPose" the scalp now. The strands are
    // turned on the rest pose by the pose's squash.
    const auto posed = bandPlane(0.5f);
    const auto rest = std::make_shared<const ScalpMesh>(posed->restPose());
    const StrandsPtr in = strandsAt(*rest, {{0.5f, 0.52f}}, 45.0f);
    EXPECT_EQ(run(in, rest, kOnlyLift)->positions(), in->positions());  // the rest pose alone: at rest
    EXPECT_NEAR(elevationOf(*run(in, rest, kOnlyLift, posed), 0), 70.0f, 0.2f);
}

TEST(SkinResponse, ScaleInvariant) {
    const Params params{{"lift", 25.0}, {"flatten", 10.0}, {"spread", 15.0}, {"smoothing", 0.1}};
    const Params scaledParams{{"lift", 25.0}, {"flatten", 10.0}, {"spread", 15.0}, {"smoothing", 1.0}};
    const auto small = bandPlane(1.5f), large = bandPlane(1.5f, 10.0f);
    const std::vector<std::pair<float, float>> at{{0.5f, 0.52f}, {0.375f, 0.3f}, {0.66f, 0.7f}};
    const StrandsPtr a = run(strandsAt(*small, at, 50.0f), small, params);
    const StrandsPtr b = run(strandsAt(*large, at, 50.0f, 3.0f), large, scaledParams);
    for (std::size_t i = 0; i < a->positions().size(); ++i) {
        EXPECT_LT((a->positions()[i] * 10.0f - b->positions()[i]).length(), 1e-3f) << "point " << i;
    }
}

}  // namespace
}  // namespace groomlab
