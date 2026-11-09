// SPDX-License-Identifier: Apache-2.0
// Interpolate's keepCurls: guides curling out of step cancel where they blend (two helices half a
// turn apart average to a straight strand); with keepCurls the strands between them curl as much
// as their guides again, in their own phase, their length kept, and in-step guides blend as ever.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/guide_set.h"
#include "groomlab/look/look_stage_params.h"
#include "groomlab/operators.h"
#include "groomlab/strand_set.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kRadius = 0.25f, kWave = 1.5f, kHeight = 6.0f;

// A helix up +y from (x, 0, z), `kRadius` wide, a turn every kWave of height, starting at `phase`.
std::vector<Vec3f> helix(float x, float z, float phase) {
    std::vector<Vec3f> p;
    const int n = 241;
    for (int i = 0; i < n; ++i) {
        const float h = kHeight * static_cast<float>(i) / static_cast<float>(n - 1);
        const float a = 6.2831853f * h / kWave + phase;
        // (the root on the axis's side: the helix's start offset taken off, so it grows from the root)
        p.push_back({x + kRadius * (std::cos(a) - std::cos(phase)), h, z + kRadius * (std::sin(a) - std::sin(phase))});
    }
    return p;
}

StrandsPtr grow(float phaseB, const std::function<void(ParamSet&)>& set) {
    const MeshPtr plane = test::makePlane(8, 4.0f);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, helix(1.0f, 2.0f, 0.0f));
    guides->add(2, helix(3.0f, 2.0f, phaseB));
    guides->rootOnMesh(*plane);
    ExternalInputs ext;
    ext.set("scalp", plane, 1);
    ext.set("guides", GuidesPtr(guides), 2);
    EvalContext ctx;
    ctx.externals = &ext;
    ctx.strandSegments = 60;
    Graph g;
    const NodeId scalpNode = g.addNode(op::kScalp);
    const NodeId guideNode = g.addNode(op::kGuides);
    const NodeId scatter = g.addNode(op::kScatter);
    const NodeId interp = g.addNode(op::kInterpolate);
    g.node(scatter).params().set("density", 40.0);
    g.node(interp).params().set("neighbors", std::int64_t{2});
    g.node(interp).params().set("smoothness", 1.0);
    set(g.node(interp).params());
    g.connect(scalpNode, scatter, 0);
    g.connect(scatter, interp, 0);
    g.connect(guideNode, interp, 1);
    g.connect(scalpNode, interp, 2);
    return g.evaluateStrands(interp, ctx);
}

// A strand's curl radius: its points' RMS distance across the hair (in x z) from their middle.
float curlRadius(const StrandSet& s, std::size_t i) {
    const Vec3f* p = s.strandPoints(i);
    const std::size_t n = s.strandPointCount(i);
    float mx = 0.0f, mz = 0.0f;
    for (std::size_t j = 0; j < n; ++j) mx += p[j].x, mz += p[j].z;
    mx /= static_cast<float>(n);
    mz /= static_cast<float>(n);
    float sum = 0.0f;
    for (std::size_t j = 0; j < n; ++j) sum += (p[j].x - mx) * (p[j].x - mx) + (p[j].z - mz) * (p[j].z - mz);
    return std::sqrt(sum / static_cast<float>(n));
}

float arcLength(const StrandSet& s, std::size_t i) {
    const Vec3f* p = s.strandPoints(i);
    float len = 0.0f;
    for (std::size_t j = 0; j + 1 < s.strandPointCount(i); ++j) len += (p[j + 1] - p[j]).length();
    return len;
}

struct Middle {
    float radius = 0.0f, length = 0.0f;
    std::size_t count = 0;
};
// The strands rooted midway between the guides: their mean curl radius and length.
Middle middle(const StrandSet& s) {
    Middle m;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f root = s.strandPoints(i)[0];
        if (std::abs(root.x - 2.0f) > 0.25f || std::abs(root.z - 2.0f) > 1.0f) continue;
        m.radius += curlRadius(s, i);
        m.length += arcLength(s, i);
        ++m.count;
    }
    if (m.count) {
        m.radius /= static_cast<float>(m.count);
        m.length /= static_cast<float>(m.count);
    }
    return m;
}

TEST(KeepCurls, OutOfStepGuidesKeepTheirCurlWhereTheyBlend) {
    const float pi = 3.14159265f;
    const Middle plain = middle(*grow(pi, [](ParamSet&) {}));
    const Middle kept = middle(*grow(pi, [](ParamSet& p) { p.set("keepCurls", 1.0); }));
    std::printf("[ keepCurls ] guides %.3f; between them: plain %.3f, kept %.3f (%zu strands); length %.3f -> %.3f\n",
                kRadius, plain.radius, kept.radius, kept.count, plain.length, kept.length);
    ASSERT_GT(kept.count, 10u);
    EXPECT_LT(plain.radius, 0.35f * kRadius) << "half a turn apart, the plain blend all but cancels them";
    EXPECT_GT(kept.radius, 0.85f * kRadius) << "keepCurls brings the curl back";
    EXPECT_LT(kept.radius, 1.1f * kRadius);
    EXPECT_NEAR(kept.length, plain.length, 0.01f * plain.length) << "its length kept";
}

TEST(KeepCurls, InStepGuidesBlendAsEver) {
    const StrandsPtr plain = grow(0.0f, [](ParamSet&) {});
    const StrandsPtr kept = grow(0.0f, [](ParamSet& p) { p.set("keepCurls", 1.0); });
    ASSERT_EQ(plain->strandCount(), kept->strandCount());
    float worst = 0.0f;
    for (std::size_t i = 0; i < plain->strandCount(); ++i) {
        const Vec3f* a = plain->strandPoints(i);
        const Vec3f* b = kept->strandPoints(i);
        for (std::size_t j = 0; j < plain->strandPointCount(i); ++j) worst = std::max(worst, (a[j] - b[j]).length());
    }
    std::printf("[ keepCurls ] in step: points moved %.4f at most\n", worst);
    EXPECT_LT(worst, 0.05f * kRadius);
}

TEST(KeepCurls, AMaskKeepsThemOnlyWhereItSays) {
    MapRef mask;
    mask.source = MapRef::Source::Expression;
    mask.name = "z > 2 ? 1 : 0";
    const StrandsPtr s = grow(3.14159265f, [&](ParamSet& p) { p.set("keepCurls", mask); });
    float near = 0.0f, far = 0.0f;
    std::size_t nearCount = 0, farCount = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f root = s->strandPoints(i)[0];
        if (std::abs(root.x - 2.0f) > 0.25f) continue;
        if (root.z > 2.3f) far += curlRadius(*s, i), ++farCount;
        if (root.z < 1.7f) near += curlRadius(*s, i), ++nearCount;
    }
    ASSERT_GT(nearCount, 3u);
    ASSERT_GT(farCount, 3u);
    EXPECT_GT(far / static_cast<float>(farCount), 0.75f * kRadius) << "kept where the mask is 1";
    EXPECT_LT(near / static_cast<float>(nearCount), 0.35f * kRadius) << "as blended where it is 0";
}

TEST(KeepCurls, TheGpuPreviewLeavesItToTheCpu) {
    Graph g;
    const NodeId interp = g.addNode(op::kInterpolate);
    g.node(interp).params().set("keepCurls", 0.5);
    gpu::StageParamReader reader(g.node(interp).params(), "Interpolate");
    reader.samples = 32;
    reader.tubes = true;
    gpu::StageParams stage;
    std::string why;
    EXPECT_FALSE(gpu::readStageParams(gpu::StageKind::Interpolate, reader, &stage, &why));
    EXPECT_NE(why.find("keepCurls"), std::string::npos) << why;
    reader.curls = true;  // the CPU engine's own run, keeping them after it
    EXPECT_TRUE(gpu::readStageParams(gpu::StageKind::Interpolate, reader, &stage, &why)) << why;
}

}  // namespace
}  // namespace groomlab
