// SPDX-License-Identifier: Apache-2.0
// Scatter's follicular units: with `follicles` every root is a unit and extra hairs sprout beside
// it, about follicles x (follicleHairs - 1) a unit. The units are the roots a Scatter without them
// makes (same IDs, same spots); each extra has an ID of its own, so a preview shows the extras the
// full density has, a mask grows them only where it says and raising follicles only adds hairs.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/strand_set.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr double kDensity = 60.0;

StrandsPtr scatter(const MeshPtr& scalp, const std::function<void(ParamSet&)>& set, float previewDensity = 1.0f) {
    Graph graph;
    ExternalInputs externals;
    externals.set("scalp", scalp, scalp->surfaceHash());
    const NodeId s = graph.addNode(op::kScalp);
    const NodeId sc = graph.addNode(op::kScatter);
    graph.node(sc).params().set("density", kDensity);
    set(graph.node(sc).params());
    graph.connect(s, sc, 0);
    EvalContext ctx;
    ctx.externals = &externals;
    ctx.previewDensity = previewDensity;
    return graph.evaluateStrands(sc, ctx);
}

std::map<StrandId, Vec3f> rootsById(const StrandSet& s) {
    std::map<StrandId, Vec3f> out;
    for (std::size_t i = 0; i < s.strandCount(); ++i) out[s.ids()[i]] = s.positions()[s.strandBegin(i)];
    return out;
}

TEST(Follicles, ExtraHairsSproutBesideEachUnit) {
    const MeshPtr plane = test::makePlane(12, 10.0f);  // 100 units of area: about 6000 roots
    const StrandsPtr units = scatter(plane, [](ParamSet&) {});
    const StrandsPtr hairs = scatter(plane, [](ParamSet& p) {
        p.set("follicles", 1.0);
        p.set("follicleHairs", 3.0);
    });
    ASSERT_GT(units->strandCount(), 4000u);
    const double ratio = static_cast<double>(hairs->strandCount()) / static_cast<double>(units->strandCount());
    std::printf("[ follicles ] %zu units, %zu hairs: %.2f a unit (follicleHairs 3)\n", units->strandCount(), hairs->strandCount(), ratio);
    EXPECT_NEAR(ratio, 3.0, 0.15);
    // The units are the roots a Scatter without follicles makes: the same IDs at the same spots.
    const std::map<StrandId, Vec3f> unitRoots = rootsById(*units), hairRoots = rootsById(*hairs);
    for (const auto& [id, at] : unitRoots) {
        const auto it = hairRoots.find(id);
        ASSERT_NE(it, hairRoots.end()) << id;
        EXPECT_EQ(it->second, at);
    }
    // Every extra beside a unit (within follicleRadius, 0.03 by default, and on the scalp).
    std::vector<Vec3f> unitList;
    for (const auto& [id, at] : unitRoots) unitList.push_back(at);
    std::size_t far = 0;
    for (const auto& [id, at] : hairRoots) {
        if (unitRoots.count(id)) continue;
        float nearest = std::numeric_limits<float>::max();
        for (const Vec3f& u : unitList) nearest = std::min(nearest, (u - at).length());
        if (nearest > 0.0301f) ++far;
        EXPECT_NEAR(at.y, 0.0f, 1e-4f);
    }
    EXPECT_EQ(far, 0u) << "extras beside their units";
}

TEST(Follicles, FollicleRadiusSetsHowFarTheyReach) {
    const MeshPtr plane = test::makePlane(12, 10.0f);
    const std::map<StrandId, Vec3f> units = rootsById(*scatter(plane, [](ParamSet&) {}));
    std::vector<Vec3f> unitList;
    for (const auto& [id, at] : units) unitList.push_back(at);
    for (const double radius : {0.01, 0.08}) {
        const std::map<StrandId, Vec3f> hairs = rootsById(*scatter(plane, [radius](ParamSet& p) {
            p.set("follicles", 1.0);
            p.set("follicleRadius", radius);
        }));
        float farthest = 0.0f;
        for (const auto& [id, at] : hairs) {
            if (units.count(id)) continue;
            float nearest = std::numeric_limits<float>::max();
            for (const Vec3f& u : unitList) nearest = std::min(nearest, (u - at).length());
            farthest = std::max(farthest, nearest);
        }
        EXPECT_LE(farthest, static_cast<float>(radius) * 1.001f) << radius;
        EXPECT_GT(farthest, static_cast<float>(radius) * 0.8f) << radius << ": some reach near it";
    }
}

TEST(Follicles, APreviewShowsTheExtrasTheFullDensityHas) {
    const MeshPtr plane = test::makePlane(12, 10.0f);
    const auto set = [](ParamSet& p) { p.set("follicles", 1.0); };
    const std::map<StrandId, Vec3f> full = rootsById(*scatter(plane, set, 1.0f));
    const StrandsPtr half = scatter(plane, set, 0.5f);
    ASSERT_LT(half->strandCount(), full.size());
    ASSERT_GT(half->strandCount(), full.size() / 3);
    for (std::size_t i = 0; i < half->strandCount(); ++i) {
        const auto it = full.find(half->ids()[i]);
        ASSERT_NE(it, full.end()) << "strand " << half->ids()[i];
        EXPECT_EQ(half->positions()[half->strandBegin(i)], it->second);
    }
}

TEST(Follicles, AMaskGrowsThemOnlyWhereItSaysAndMoreOnlyAdds) {
    const MeshPtr plane = test::makePlane(12, 10.0f);
    const std::map<StrandId, Vec3f> units = rootsById(*scatter(plane, [](ParamSet&) {}));
    MapRef mask;
    mask.source = MapRef::Source::Expression;
    mask.name = "x > 5 ? 1 : 0";
    const std::map<StrandId, Vec3f> masked = rootsById(*scatter(plane, [&](ParamSet& p) { p.set("follicles", mask); }));
    std::size_t extras = 0, outside = 0;
    for (const auto& [id, at] : masked) {
        if (units.count(id)) continue;
        ++extras;
        if (at.x < 4.9f) ++outside;
    }
    EXPECT_GT(extras, 1000u);
    EXPECT_EQ(outside, 0u) << "no extras where the mask is 0";
    // Half the follicles: a subset of the hairs all of them grow.
    const std::map<StrandId, Vec3f> some = rootsById(*scatter(plane, [](ParamSet& p) { p.set("follicles", 0.5); }));
    const std::map<StrandId, Vec3f> more = rootsById(*scatter(plane, [](ParamSet& p) { p.set("follicles", 1.0); }));
    ASSERT_LT(some.size(), more.size());
    for (const auto& [id, at] : some) {
        const auto it = more.find(id);
        ASSERT_NE(it, more.end()) << "raising follicles only adds hairs";
        EXPECT_EQ(it->second, at);
    }
}

}  // namespace
}  // namespace groomlab
