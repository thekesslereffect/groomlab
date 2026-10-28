// SPDX-License-Identifier: Apache-2.0
// Operator costs at 500k strands x 16 points on a head (reported only): the nodes no other budget
// measures at that size, with Noise and Curl beside them, each on the same groom (the XGen
// comparison's head, its strands interpolated from guides bent down over it), so their times rank
// against each other and against Interpolate's (test_budgets.cpp). Each line gives the median time
// and the time per million points.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/cards.h"
#include "groomlab/display.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The median of `runs` timed calls after one untimed (it warms the pools and caches up).
template <class Fn>
double medianMs(int runs, Fn&& fn) {
    fn();
    std::vector<double> ms;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        ms.push_back(msSince(t0));
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

// The XGen comparison's head (as Budget.InterpolateKeepingClearanceOnAHead): a sphere of radius
// 10 cut 15% of it under the equator, scaled 1, 1.1, 1.2; about 600 guides of 4 points out of it
// along the normal and falling down past it (they cross the head's sides, for Collide); 500k
// strands of 16 points scattered and interpolated from them.
struct HeadGroom {
    MeshPtr head;
    GuidesPtr guides;
    StrandsPtr strands;
    ExternalInputs ext;
    EvalContext ctx;
};

const HeadGroom& headGroom() {
    static const HeadGroom* groom = [] {
        auto* g = new HeadGroom;
        const auto sphere = test::makeSphere(100, 160, 10.0f);
        std::vector<Vec3f> points;
        std::vector<ScalpMesh::Triangle> triangles;
        std::vector<std::int32_t> renumbered(sphere->pointCount(), -1);
        for (const ScalpMesh::Triangle& t : sphere->triangles()) {
            const Vec3f& a = sphere->points()[static_cast<std::size_t>(t[0])];
            const Vec3f& b = sphere->points()[static_cast<std::size_t>(t[1])];
            const Vec3f& c = sphere->points()[static_cast<std::size_t>(t[2])];
            if ((a.y + b.y + c.y) / 3.0f < -1.5f) continue;
            ScalpMesh::Triangle kept;
            for (int k = 0; k < 3; ++k) {
                std::int32_t& to = renumbered[static_cast<std::size_t>(t[k])];
                if (to < 0) {
                    const Vec3f& p = sphere->points()[static_cast<std::size_t>(t[k])];
                    to = static_cast<std::int32_t>(points.size());
                    points.emplace_back(p.x, p.y * 1.1f, p.z * 1.2f);
                }
                kept[k] = to;
            }
            triangles.push_back(kept);
        }
        g->head = std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
        auto guides = std::make_shared<GuideSet>();
        for (int i = 0, id = 1; i < 700; ++i) {  // a Fibonacci lattice over the sphere, those on the head
            const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 700.0f;
            const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
            if (y < -0.1f) continue;
            const SurfacePoint at = g->head->closestPoint(Vec3f(r * std::cos(a), y * 1.1f, r * std::sin(a) * 1.2f) * 10.0f);
            if (at.root.face < 0) continue;
            const Vec3f n = at.normal, down(0.0f, -1.0f, 0.0f);
            Guide& guide = guides->add(static_cast<std::uint64_t>(id++),
                                       {at.position, at.position + n * 3.0f, at.position + n * 4.5f + down * 4.0f,
                                        at.position + n * 5.0f + down * 12.0f});
            guide.root = at.root;
        }
        g->guides = guides;
        double area = 0.0;
        for (const ScalpMesh::Triangle& t : g->head->triangles()) {
            const Vec3f &a = g->head->points()[static_cast<std::size_t>(t[0])],
                        &b = g->head->points()[static_cast<std::size_t>(t[1])],
                        &c = g->head->points()[static_cast<std::size_t>(t[2])];
            area += 0.5 * (b - a).cross(c - a).length();
        }
        char graph[1024];
        std::snprintf(graph, sizeof(graph), R"({"version": 1, "output": "out",
          "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": %f, "length": 15.0}},
            {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 15}}],
          "connections": [
            {"from": "scalp", "to": "scatter", "input": "scalp"},
            {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "out", "input": "strands"}]})",
                      500000.0 / area);
        GraphDocument doc = loadGraphJson(graph);
        setGroomExternals(g->ext, g->head, nullptr, nullptr);
        g->ctx.externals = &g->ext;
        const auto scattered = valueAs<StrandsPtr>(doc.graph.evaluate(doc.node("scatter"), g->ctx));
        auto interpolate = OperatorRegistry::instance().create(op::kInterpolate);
        g->strands = valueAs<StrandsPtr>(
            interpolate->evaluate(g->ctx, {Value(scattered), Value(g->guides), Value(MeshPtr(g->head))}));
        return g;
    }();
    return *groom;
}

void report(const char* what, const StrandSet& in, double ms) {
    const double points = static_cast<double>(in.pointCount());
    std::printf("[ budget ] %s, %zu strands x %zu points: %.1f ms (%.1f ms per million points)\n", what, in.strandCount(),
                in.strandCount() ? in.pointCount() / in.strandCount() : 0, ms, ms * 1e6 / points);
}

// One operator on the head's strands: a new node each run (nothing it keeps between evaluations
// helps), its params set by `setup`; median of 5.
template <class Setup>
double timeOp(const char* type, const StrandsPtr& in, Setup&& setup, std::vector<Value> extra = {}) {
    const HeadGroom& g = headGroom();
    std::vector<Value> inputs{Value(in)};
    for (Value& v : extra) inputs.push_back(std::move(v));
    StrandsPtr out;
    const double ms = medianMs(5, [&] {
        auto node = OperatorRegistry::instance().create(type);
        setup(*node);
        out = valueAs<StrandsPtr>(node->evaluate(g.ctx, inputs));
    });
    EXPECT_TRUE(out);
    return ms;
}

TEST(Budget, ShapeOperatorsOnAHead) {
    const HeadGroom& g = headGroom();
    const StrandsPtr& in = g.strands;
    report("Length (keep 0.8, trim up to 0.3)", *in, timeOp(op::kLength, in, [](Operator& o) {
               o.params().set("keep", 0.8);
               o.params().set("randomness", 0.3);
           }));
    report("Width", *in, timeOp(op::kWidth, in, [](Operator& o) { o.params().set("width", 0.01); }));
    report("Bend", *in, timeOp(op::kBend, in, [](Operator& o) { o.params().set("angle", 40.0); }));
    report("Smooth", *in, timeOp(op::kSmooth, in, [](Operator&) {}));
    report("Gravity", *in, timeOp(op::kGravity, in, [](Operator&) {}));
    report("Noise (strand space, smooth)", *in, timeOp(op::kNoise, in, [](Operator& o) { o.params().set("amplitude", 0.5); }));
    report("Noise (world space)", *in, timeOp(op::kNoise, in, [](Operator& o) {
               o.params().set("amplitude", 0.5);
               o.params().set("space", std::string("world"));
           }));
    report("Noise (kinky: frizz)", *in, timeOp(op::kNoise, in, [](Operator& o) {
               o.params().set("amplitude", 0.3);
               o.params().set("mode", std::string("kinky"));
               o.params().set("fraction", 0.2);
           }));
    report("Curl", *in, timeOp(op::kCurl, in, [](Operator& o) { o.params().set("radius", 0.3); }));
    report("Collide (the scalp)", *in, timeOp(op::kCollide, in, [](Operator&) {}, {Value(MeshPtr(g.head))}));
}

TEST(Budget, ClumpOnAHead) {
    const HeadGroom& g = headGroom();
    const StrandsPtr& in = g.strands;
    report("Clump by count 400 (it picks the centers)", *in,
           timeOp(op::kClump, in, [](Operator& o) { o.params().set("count", std::int64_t{400}); }, {Value()}));
    report("Clump by count 2000", *in,
           timeOp(op::kClump, in, [](Operator& o) { o.params().set("count", std::int64_t{2000}); }, {Value()}));
    report("Clump on the guides' roots (inputPoints)", *in, timeOp(op::kClump, in, [](Operator& o) {
               o.params().set("centersFrom", std::string("inputPoints"));
           }, {Value(g.guides)}));
    report("Clump along the guides (inputCurves)", *in, timeOp(op::kClump, in, [](Operator& o) {
               o.params().set("centersFrom", std::string("inputCurves"));
           }, {Value(g.guides)}));
    // A strength tweak: the same node, its pick kept.
    auto node = OperatorRegistry::instance().create(op::kClump);
    node->params().set("count", std::int64_t{400});
    int r = 0;
    report("Clump by count 400, a strength tweak", *in, medianMs(5, [&] {
               node->params().set("strength", ++r % 2 ? 0.7 : 0.75);
               node->evaluate(g.ctx, {Value(in), Value()});
           }));
}

TEST(Budget, ScatterOnAHead) {
    const HeadGroom& g = headGroom();
    double area = g.head->restTotalArea();
    for (const double randomness : {1.0, 0.0}) {
        StrandsPtr out;
        const double ms = medianMs(3, [&] {
            auto node = OperatorRegistry::instance().create(op::kScatter);
            node->params().set("density", 500000.0 / area);
            node->params().set("length", 15.0);
            node->params().set("randomness", randomness);
            out = valueAs<StrandsPtr>(node->evaluate(g.ctx, {Value(MeshPtr(g.head)), Value()}));
        });
        ASSERT_TRUE(out);
        report(randomness == 1.0 ? "Scatter, randomness 1 (a new node)" : "Scatter, randomness 0 (relaxed, a new node)", *out, ms);
    }
}

TEST(Budget, ColorAndUtilityOperatorsOnAHead) {
    const HeadGroom& g = headGroom();
    const StrandsPtr& in = g.strands;
    report("HairColor (melanin)", *in, timeOp(op::kHairColor, in, [](Operator&) {}));
    report("HairColor (greys 0.2, value and hue random)", *in, timeOp(op::kHairColor, in, [](Operator& o) {
               o.params().set("greys", 0.2);
               o.params().set("valueRandom", 0.2);
               o.params().set("hueRandom", 0.1);
           }));
    std::vector<Value> halves;
    report("Split (random, a and b)", *in, medianMs(5, [&] {
               auto node = OperatorRegistry::instance().create(op::kSplit);
               halves = node->evaluateOutputs(g.ctx, {Value(in)});
           }));
    ASSERT_GE(halves.size(), 2u);
    report("Merge (Split's a and b)", *in, medianMs(5, [&] {
               auto node = OperatorRegistry::instance().create(op::kMerge);
               node->evaluate(g.ctx, {halves[0], halves[1]});
           }));
}

TEST(Budget, BraidOnAHead) {
    const HeadGroom& g = headGroom();
    const StrandsPtr& in = g.strands;
    report("Braid (picked, region 3)", *in, timeOp(op::kBraid, in, [](Operator& o) { o.params().set("region", 3.0); }, {Value()}));
}

TEST(Budget, CardsAndDisplayOnAHead) {
    const HeadGroom& g = headGroom();
    auto clump = OperatorRegistry::instance().create(op::kClump);
    clump->params().set("count", std::int64_t{400});
    const StrandsPtr clumped = valueAs<StrandsPtr>(clump->evaluate(g.ctx, {Value(g.strands), Value()}));
    ASSERT_TRUE(clumped);
    CardSettings settings;
    settings.maxCards = 1000;
    CardMesh cards;
    report("buildCards (400 clumps, at most 1000 cards)", *clumped,
           medianMs(3, [&] { cards = buildCards(*clumped, g.head.get(), settings); }));
    CardBakeSettings bake;
    bake.size = 512;
    bake.scalp = g.head.get();
    report("bakeCardTextures at 512", *clumped, medianMs(3, [&] { bakeCardTextures(*clumped, cards, bake); }));
    // The viewport's lit strands, built into vectors (a host that copies) and as lines a host writes.
    DisplayInputs inputs;
    inputs.strands = clumped;
    const DisplaySettings display;
    std::uint64_t version = 0;
    for (const bool lines : {false, true}) {
        report(lines ? "display: lit strands as lines" : "display: lit strands into vectors", *clumped, medianMs(5, [&] {
                   DisplayBuilder builder;
                   builder.setRetain(false);
                   builder.setWriteLines(lines);
                   inputs.strandsVersion = ++version;
                   builder.update(inputs, display);
               }));
    }
}

}  // namespace
}  // namespace groomlab
