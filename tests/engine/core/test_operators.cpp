// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/map_ref.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Builds and evaluates a single operator fed by constant inputs.
struct OneOp {
    Graph graph;
    ExternalInputs externals;
    NodeId scalp = 0, guides = 0, scatter = 0;

    explicit OneOp(double density = 200.0) {
        externals.set("scalp", test::makePlane(8, 1.0f), 1);
        scalp = graph.addNode(op::kScalp);
        scatter = graph.addNode(op::kScatter);
        graph.node(scatter).params().set("density", density);
        graph.connect(scalp, scatter, 0);
    }

    StrandsPtr eval(NodeId id, float previewDensity = 1.0f) {
        EvalContext ctx;
        ctx.previewDensity = previewDensity;
        ctx.externals = &externals;
        return graph.evaluateStrands(id, ctx);
    }
};

TEST(Registry, HasBuiltins) {
    auto& reg = OperatorRegistry::instance();
    for (const char* name : {op::kScalp, op::kGuides, op::kScatter, op::kInterpolate, op::kLength, op::kWidth,
                             op::kOutput}) {
        EXPECT_TRUE(reg.contains(name)) << name;
    }
    // Defaults are applied on creation.
    auto scatter = reg.create(op::kScatter);
    EXPECT_DOUBLE_EQ(scatter->params().getDouble("density"), 80.0);  // light enough to preview every strand (real hair: ~200/cm²)
}

TEST(Scatter, CountMatchesDensity) {
    OneOp g(1000.0);
    const auto strands = g.eval(g.scatter);
    ASSERT_TRUE(strands);
    EXPECT_NEAR(static_cast<double>(strands->strandCount()), 1000.0, 40.0);  // area 1
    EXPECT_EQ(strands->uniformPointCount(), 9u);                              // 8 segments
    EXPECT_TRUE(strands->validate());

    // Roots on the plane, strands along +Y with length 1.
    const Vec3f* p = strands->strandPoints(0);
    EXPECT_NEAR(p[0].y, 0.0f, 1e-6f);
    EXPECT_NEAR(p[8].y, 1.0f, 1e-5f);
}

TEST(Scatter, IdsAreUniqueAndStableAcrossDensity) {
    OneOp low(200.0), high(400.0);
    const auto a = low.eval(low.scatter);
    const auto b = high.eval(high.scatter);

    std::set<StrandId> idsB(b->ids().begin(), b->ids().end());
    EXPECT_EQ(idsB.size(), b->strandCount());
    std::size_t shared = 0;
    for (StrandId id : a->ids()) shared += idsB.count(id);
    EXPECT_EQ(shared, a->strandCount());  // doubling density keeps every existing strand
}

TEST(Scatter, PreviewDensityIsSubset) {
    OneOp g(1000.0);
    const auto full = g.eval(g.scatter, 1.0f);
    const auto preview = g.eval(g.scatter, 0.1f);
    EXPECT_NEAR(static_cast<double>(preview->strandCount()), full->strandCount() * 0.1, 30.0);

    std::set<StrandId> fullIds(full->ids().begin(), full->ids().end());
    for (StrandId id : preview->ids()) EXPECT_TRUE(fullIds.count(id));
}

// Nearest-neighbor distances between roots away from the plane's edges:
// the smallest, the mean, and their spread (standard deviation / mean).
struct Spacing {
    float smallest = 1e9f, mean = 0.0f, spread = 0.0f;
};
Spacing nearestSpacing(const StrandSet& s) {
    std::vector<float> nearest;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f p = s.strandPoints(i)[0];
        if (p.x < 0.1f || p.x > 0.9f || p.z < 0.1f || p.z > 0.9f) continue;
        float best = 1e9f;
        for (std::size_t j = 0; j < s.strandCount(); ++j) {
            if (i != j) best = std::min(best, (p - s.strandPoints(j)[0]).length());
        }
        nearest.push_back(best);
    }
    Spacing out;
    for (const float d : nearest) out.smallest = std::min(out.smallest, d), out.mean += d;
    out.mean /= static_cast<float>(nearest.size());
    for (const float d : nearest) out.spread += (d - out.mean) * (d - out.mean);
    out.spread = std::sqrt(out.spread / static_cast<float>(nearest.size())) / out.mean;
    return out;
}

TEST(Scatter, LowRandomnessSpreadsRootsEvenlyAndKeepsIds) {
    OneOp random(1000.0), even(1000.0), half(1000.0);
    even.graph.node(even.scatter).params().set("randomness", 0.0);
    half.graph.node(half.scatter).params().set("randomness", 0.5);
    const auto a = random.eval(random.scatter);
    const auto b = even.eval(even.scatter);
    const auto c = half.eval(half.scatter);
    ASSERT_EQ(a->ids(), b->ids());  // the same strands, moved
    ASSERT_EQ(a->ids(), c->ids());
    const float hex = 1.0746f / std::sqrt(1000.0f);  // hexagonal spacing at this density (unit plane)
    const Spacing sa = nearestSpacing(*a), sb = nearestSpacing(*b), sc = nearestSpacing(*c);
    EXPECT_GT(sa.spread, 0.4f);  // white noise
    EXPECT_LT(sb.spread, 0.07f);  // even: every root about the same distance from its nearest
    EXPECT_GT(sb.smallest, 0.65f * hex);  // no clumped pairs
    EXPECT_GT(sb.mean, 0.85f * hex);
    EXPECT_GT(sc.spread, sb.spread);  // halfway: in between
    EXPECT_LT(sc.spread, sa.spread);
    // The open edges push back: no row of roots piled up along them.
    std::size_t onEdge = 0;
    for (std::size_t i = 0; i < b->strandCount(); ++i) {  // still on the plane
        const Vec3f p = b->strandPoints(i)[0];
        EXPECT_NEAR(p.y, 0.0f, 1e-5f);
        EXPECT_NEAR(c->strandPoints(i)[0].y, 0.0f, 1e-5f);
        const float edge = std::min(std::min(p.x, 1.0f - p.x), std::min(p.z, 1.0f - p.z));
        onEdge += edge < 0.2f * hex ? 1 : 0;
    }
    EXPECT_LT(onEdge, b->strandCount() / 50);  // a piled-up row would be about 4 / sqrt(1000) = 13%
}

TEST(Scatter, RelaxationRunsItsFixedIterations) {
    // Relaxing takes every one of its 24 iterations (none stops early: a root a rounding apart, the
    // same scalp in other units, ends a rounding apart) and spreads the roots evenly.
    OneOp g(1000.0);
    g.graph.node(g.scatter).params().set("randomness", 0.0);
    const std::size_t before = scatterRelaxIterationCount();
    const auto a = g.eval(g.scatter);
    ASSERT_TRUE(a);
    const std::size_t first = scatterRelaxIterationCount() - before;
    EXPECT_EQ(first, 24u);
    const Spacing sa = nearestSpacing(*a);
    const float hex = 1.0746f / std::sqrt(1000.0f);
    EXPECT_LT(sa.spread, 0.07f);
    EXPECT_GT(sa.smallest, 0.65f * hex);
}

TEST(Scatter, ARelaxedPreviewShowsTheRenderPositions) {
    OneOp g(1000.0);
    g.graph.node(g.scatter).params().set("randomness", 0.3);
    const auto full = g.eval(g.scatter, 1.0f);
    const auto preview = g.eval(g.scatter, 0.2f);
    std::map<StrandId, Vec3f> at;
    for (std::size_t i = 0; i < full->strandCount(); ++i) at[full->ids()[i]] = full->strandPoints(i)[0];
    ASSERT_GT(preview->strandCount(), 0u);
    for (std::size_t i = 0; i < preview->strandCount(); ++i) {
        const Vec3f d = at.at(preview->ids()[i]) - preview->strandPoints(i)[0];
        EXPECT_LT(d.length(), 1e-5f);
    }
}

TEST(Scatter, AThinnedPreviewKeepsItsShareByIdAtEveryRandomness) {
    // The preview's 20% are the strands a pick by ID keeps, whatever the randomness: the same
    // strands at 0, 0.5, 1 and past it (no swap as the randomness changes), and the ones a node
    // downstream that keeps a preview share of its own (Clump's candidates, Tangle's partners)
    // finds in the preview as in the render.
    OneOp g(5000.0);
    std::vector<StrandId> byId;
    for (const StrandId id : g.eval(g.scatter, 1.0f)->ids()) {
        if (keepAtDensity(id, 0.2f)) byId.push_back(id);
    }
    for (const double randomness : {0.0, 0.5, 1.0, 1.5}) {
        g.graph.node(g.scatter).params().set("randomness", randomness);
        EXPECT_EQ(g.eval(g.scatter, 0.2f)->ids(), byId) << randomness;
    }
}

// Same strands to the bit: IDs, roots and points.
void expectIdentical(const StrandSet& a, const StrandSet& b, const std::string& what) {
    ASSERT_EQ(a.ids(), b.ids()) << what;
    ASSERT_EQ(a.roots().size(), b.roots().size()) << what;
    for (std::size_t i = 0; i < a.roots().size(); ++i) {
        ASSERT_EQ(a.roots()[i].face, b.roots()[i].face) << what;
        ASSERT_EQ(a.roots()[i].u, b.roots()[i].u) << what;
        ASSERT_EQ(a.roots()[i].v, b.roots()[i].v) << what;
    }
    ASSERT_EQ(a.positions(), b.positions()) << what;
}

// Scalp (with a rest pose) and guides into a relaxed Scatter (its guides input connected).
struct RelaxedScatter {
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "scatter", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 1500.0, "randomness": 0.4}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "guides", "to": "scatter", "input": "guides"}]})");
    Operator& scatter() { return doc.graph.node(doc.node("scatter")); }
};

struct ScatterInputs {
    MeshPtr scalp;
    GuidesPtr guides;
    float previewDensity = 1.0f;
    double maxPoints = 0.0;
};

StrandsPtr evalScatter(RelaxedScatter& g, const ScatterInputs& in) {
    ExternalInputs ext;
    setGroomExternals(ext, in.scalp, in.guides, nullptr);
    EvalContext ctx;
    ctx.previewDensity = in.previewDensity;
    ctx.maxPoints = in.maxPoints;
    ctx.externals = &ext;
    return g.doc.graph.evaluateStrands(g.doc.output, ctx);
}

// The plane, its rest pose the flat plane, its points bent by `bend` (a deforming scalp), with a
// "mask" color set of `mask` at x below 0.5 and 1 above.
MeshPtr bentPlane(float bend, float mask = 0.3f, float restLift = 0.0f) {
    const auto flat = test::makePlane(8, 1.0f);
    auto mesh = std::make_shared<ScalpMesh>(*flat);
    std::vector<Vec3f> rest = flat->points();
    for (Vec3f& p : rest) p.y += restLift * p.x * p.z;
    mesh->setRestPoints(rest);
    std::vector<ScalpMesh::Color> colors;
    for (const ScalpMesh::Triangle& t : flat->triangles()) {
        for (const std::int32_t v : t) {
            const float m = flat->points()[v].x < 0.5f ? mask : 1.0f;
            colors.push_back({m, m, m, 1.0f});
        }
    }
    mesh->setCornerColors("mask", std::move(colors));
    std::vector<Vec3f> points = flat->points();
    for (Vec3f& p : points) p.y += bend * (p.x - 0.5f) * (p.x - 0.5f);
    return std::make_shared<const ScalpMesh>(mesh->withPoints(std::move(points)));
}

GuidesPtr fourGuides(const ScalpMesh& scalp, float layer = 1.0f, float shift = 0.0f) {
    auto g = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (const float x : {0.25f, 0.75f}) {
        for (const float z : {0.25f, 0.75f}) {
            Guide& guide = g->add(id++, test::straightCurve({x + shift, 0.0f, z}, {0, 0.5f, 0}, 4));
            guide.attributes["dens"] = x < 0.5f ? layer : 1.0f;
        }
    }
    g->rootOnMesh(scalp);
    return g;
}

TEST(Scatter, ADeformingScalpMovesRelaxedRootsWithoutRelaxingThemAgain) {
    for (const float density : {1.0f, 0.2f}) {  // the render, and a preview relaxed with every root
        RelaxedScatter kept;
        ScatterInputs in{bentPlane(0.0f), nullptr, density};
        in.guides = fourGuides(*in.scalp);
        ASSERT_TRUE(evalScatter(kept, in));
        for (int frame = 1; frame <= 3; ++frame) {
            in.scalp = bentPlane(0.2f * static_cast<float>(frame));
            const std::size_t relaxed = scatterRelaxCount();
            const StrandsPtr moved = evalScatter(kept, in);
            EXPECT_EQ(scatterRelaxCount(), relaxed) << "frame " << frame << " relaxed again";
            RelaxedScatter fresh;
            const StrandsPtr expected = evalScatter(fresh, in);
            EXPECT_EQ(scatterRelaxCount(), relaxed + 1);
            ASSERT_TRUE(moved && expected);
            ASSERT_GT(moved->strandCount(), 100u);
            expectIdentical(*moved, *expected, "frame " + std::to_string(frame));
            EXPECT_GT(moved->strandPoints(0)[0].y + moved->strandPoints(moved->strandCount() - 1)[0].y, 0.0f);  // bent
        }
    }
}

TEST(Scatter, RelaxedRootsAreKeptOnlyWhileWhatPlacedThemStays) {
    // Each change of what places relaxed roots, evaluated through the graph that kept the last
    // placement: the same strands as a fresh graph's, worked out again.
    using Change = std::function<void(RelaxedScatter&, ScatterInputs&)>;
    const std::vector<std::pair<const char*, Change>> changes = {
        {"rest pose", [](RelaxedScatter&, ScatterInputs& in) { in.scalp = bentPlane(0.2f, 0.3f, 0.3f); }},
        {"density", [](RelaxedScatter& g, ScatterInputs&) { g.scatter().params().set("density", 1300.0); }},
        {"seed", [](RelaxedScatter& g, ScatterInputs&) { g.scatter().params().set("seed", std::int64_t{3}); }},
        {"color set density map, repainted",
         [](RelaxedScatter& g, ScatterInputs& in) {
             MapRef map;
             map.source = MapRef::Source::ColorSet;
             map.name = "mask";
             map.max = 1500.0f;
             g.scatter().params().set("density", map);
             in.scalp = bentPlane(0.2f, 0.6f);  // the "before" ran with 0.3 (set below)
         }},
        {"guide layer density map, repainted",
         [](RelaxedScatter& g, ScatterInputs& in) {
             MapRef map;
             map.source = MapRef::Source::Guide;
             map.name = "dens";
             map.max = 1500.0f;
             g.scatter().params().set("density", map);
             in.guides = fourGuides(*in.scalp, 0.2f);
         }},
        {"guide radius", [](RelaxedScatter& g, ScatterInputs&) { g.scatter().params().set("guideRadius", 0.3); }},
        {"guide roots under a guide radius",
         [](RelaxedScatter& g, ScatterInputs& in) {
             g.scatter().params().set("guideRadius", 0.3);
             in.guides = fourGuides(*in.scalp, 1.0f, 0.1f);
         }},
    };
    for (const auto& [what, change] : changes) {
        RelaxedScatter kept;
        ScatterInputs in{bentPlane(0.2f), nullptr, 1.0f};
        in.guides = fourGuides(*in.scalp);
        // The maps' "before": the same map, painted as at first.
        if (std::string(what).find("density map") != std::string::npos) {
            RelaxedScatter probe;
            ScatterInputs before = in;
            change(probe, before);  // sets the map param on `probe` only
            kept.scatter().params().set("density", probe.scatter().params().get("density"));
        } else if (std::string(what) == "guide roots under a guide radius") {
            kept.scatter().params().set("guideRadius", 0.3);
        }
        const StrandsPtr before = evalScatter(kept, in);
        ASSERT_TRUE(before) << what;
        change(kept, in);
        const std::size_t relaxed = scatterRelaxCount();
        const StrandsPtr after = evalScatter(kept, in);
        EXPECT_EQ(scatterRelaxCount(), relaxed + 1) << what << ": not placed again";
        RelaxedScatter fresh;
        fresh.scatter().params() = kept.scatter().params();
        const StrandsPtr expected = evalScatter(fresh, in);
        ASSERT_TRUE(after && expected) << what;
        expectIdentical(*after, *expected, what);
        EXPECT_FALSE(after->ids() == before->ids() && after->roots().size() == before->roots().size() &&
                     std::equal(after->roots().begin(), after->roots().end(), before->roots().begin(),
                                [](const StrandRoot& x, const StrandRoot& y) { return x.face == y.face && x.u == y.u && x.v == y.v; }))
            << what << ": the change changed nothing";
    }
}

TEST(Scatter, APreviewKeepsTheRelaxedRootsAndHoldsTheRendersStrands) {
    // Every root is relaxed whatever a preview holds of them (preview_core.h): another preview
    // density or point budget relaxes nothing again, and each strand it holds sits where the
    // render's does, bit for bit.
    RelaxedScatter kept;
    ScatterInputs in{bentPlane(0.2f), nullptr, 1.0f};
    in.guides = fourGuides(*in.scalp);
    const StrandsPtr render = evalScatter(kept, in);
    ASSERT_TRUE(render && render->strandCount() > 500u);
    std::map<StrandId, std::size_t> at;
    for (std::size_t s = 0; s < render->strandCount(); ++s) at[render->ids()[s]] = s;
    for (const auto& [density, budget] : {std::pair<float, double>{0.3f, 0.0}, {0.02f, 0.0}, {1.0f, 4000.0}}) {
        in.previewDensity = density;
        in.maxPoints = budget;
        const std::size_t relaxed = scatterRelaxCount();
        const StrandsPtr preview = evalScatter(kept, in);
        EXPECT_EQ(scatterRelaxCount(), relaxed) << density << ": relaxed again";
        ASSERT_TRUE(preview && preview->strandCount() > 0u);
        EXPECT_LT(preview->strandCount(), render->strandCount());
        for (std::size_t s = 0; s < preview->strandCount(); ++s) {
            const auto it = at.find(preview->ids()[s]);
            ASSERT_NE(it, at.end());
            EXPECT_EQ(std::memcmp(&preview->roots()[s], &render->roots()[it->second], sizeof(StrandRoot)), 0) << density;
        }
    }
}

TEST(Scatter, ARandomnessTweakMixesTheKeptPlacementsWithoutRelaxingAgain) {
    // The even and random spots are kept apart from the randomness: a tweak (to 1, to 0, past 1)
    // only mixes them again, and gives what a fresh graph gives. A render at randomness 1 works
    // out no even spots; the first tweak away from 1 relaxes once.
    for (const float density : {1.0f, 0.2f}) {
        RelaxedScatter kept;
        ScatterInputs in{bentPlane(0.2f), nullptr, density};
        in.guides = fourGuides(*in.scalp);
        const std::size_t first = scatterRelaxCount();
        ASSERT_TRUE(evalScatter(kept, in));
        EXPECT_EQ(scatterRelaxCount(), first + 1);
        for (const double randomness : {0.2, 1.0, 0.0, 1.7, 0.4}) {
            kept.scatter().params().set("randomness", randomness);
            const std::size_t relaxed = scatterRelaxCount();
            const StrandsPtr after = evalScatter(kept, in);
            EXPECT_EQ(scatterRelaxCount(), relaxed) << randomness << " relaxed again";
            RelaxedScatter fresh;
            fresh.scatter().params() = kept.scatter().params();
            const StrandsPtr expected = evalScatter(fresh, in);
            ASSERT_TRUE(after && expected);
            expectIdentical(*after, *expected, "randomness " + std::to_string(randomness));
        }
    }
    RelaxedScatter render;
    render.scatter().params().set("randomness", 1.0);
    ScatterInputs in{bentPlane(0.2f), nullptr, 1.0f};
    const std::size_t before = scatterRelaxCount();
    ASSERT_TRUE(evalScatter(render, in));
    EXPECT_EQ(scatterRelaxCount(), before) << "a render at randomness 1 needs no even spots";
    render.scatter().params().set("randomness", 0.99);
    ASSERT_TRUE(evalScatter(render, in));
    EXPECT_EQ(scatterRelaxCount(), before + 1);
}

TEST(Scatter, NoMeshGivesNoStrands) {
    OneOp g;
    g.externals = ExternalInputs{};
    EXPECT_EQ(g.eval(g.scatter)->strandCount(), 0u);
}

TEST(Interpolate, StrandsFollowNearestGuide) {
    OneOp g(300.0);
    auto guides = std::make_shared<GuideSet>();
    // Left half combed toward -X, right half toward +X.
    guides->add(1, test::straightCurve({0.1f, 0, 0.5f}, {-1, 0, 0}));
    guides->add(2, test::straightCurve({0.9f, 0, 0.5f}, {1, 0, 0}));
    g.externals.set("guides", GuidesPtr(guides), guides->contentHash());

    const NodeId guideNode = g.graph.addNode(op::kGuides);
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.node(interp).params().set("neighbors", std::int64_t{1});
    g.graph.connect(g.scatter, interp, 0);
    g.graph.connect(guideNode, interp, 1);

    const auto out = g.eval(interp);
    ASSERT_TRUE(out);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* p = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        const float dx = p[n - 1].x - p[0].x;
        if (p[0].x < 0.4f) EXPECT_NEAR(dx, -1.0f, 1e-3f);
        if (p[0].x > 0.6f) EXPECT_NEAR(dx, 1.0f, 1e-3f);
        EXPECT_NEAR(p[n - 1].y, 0.0f, 1e-4f);
    }
}

TEST(Interpolate, NoGuidesPassesThrough) {
    OneOp g;
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.connect(g.scatter, interp, 0);
    EXPECT_EQ(g.eval(interp)->contentHash(), g.eval(g.scatter)->contentHash());
}

TEST(Length, KeepsAShareOfTheLength) {
    OneOp g;
    const NodeId len = g.graph.addNode(op::kLength);
    g.graph.connect(g.scatter, len, 0);
    const std::size_t n = g.eval(g.scatter)->strandPointCount(0);
    g.graph.node(len).params().set("keep", 0.25);
    const auto out = g.eval(len);
    EXPECT_NEAR(out->strandPoints(0)[n - 1].y, 0.25f, 1e-4f);
    EXPECT_EQ(out->strandPointCount(0), n);
}

TEST(Length, TrimsAShareOfTheStrandsByARandomRange) {
    // Each strand's length share, keep 0.8: a fraction of the strands trimmed, each by a share of
    // its kept length up to randomness.
    OneOp g(2000.0);
    const NodeId len = g.graph.addNode(op::kLength);
    g.graph.connect(g.scatter, len, 0);
    const StrandsPtr in = g.eval(g.scatter);
    auto shares = [&](double fraction, double randomness) {
        ParamSet& p = g.graph.node(len).params();
        p.set("keep", 0.8);
        p.set("fraction", fraction);
        p.set("randomness", randomness);
        const StrandsPtr out = g.eval(len);
        std::vector<float> share(out->strandCount());
        for (std::size_t s = 0; s < share.size(); ++s) {
            share[s] = arcLength(out->strandPoints(s), out->strandPointCount(s)) /
                       arcLength(in->strandPoints(s), in->strandPointCount(s));
        }
        return share;
    };
    for (const float s : shares(1.0, 0.0)) EXPECT_NEAR(s, 0.8f, 1e-4f) << "no range: keep alone";
    const std::vector<float> all = shares(1.0, 0.5);
    float lo = 1.0f, hi = 0.0f;
    for (const float s : all) {
        lo = std::min(lo, s);
        hi = std::max(hi, s);
    }
    EXPECT_GE(lo, 0.8f * 0.5f - 1e-4f);
    EXPECT_LE(hi, 0.8f + 1e-4f);
    EXPECT_GT(hi - lo, 0.25f) << "spread across the range, not split in two";
    // A fraction: those strands (by ID) as with all of them, the rest kept; raising it only adds.
    const std::vector<float> some = shares(0.3, 0.5), more = shares(0.6, 0.5);
    std::size_t trimmed = 0, moreTrimmed = 0;
    for (std::size_t s = 0; s < all.size(); ++s) {
        const bool cut = std::abs(some[s] - 0.8f) > 1e-4f;
        trimmed += cut ? 1 : 0;
        moreTrimmed += std::abs(more[s] - 0.8f) > 1e-4f ? 1 : 0;
        EXPECT_FLOAT_EQ(cut ? all[s] : 0.8f, some[s]);
        if (cut) EXPECT_FLOAT_EQ(more[s], all[s]) << "a strand trimmed at 0.3 is trimmed at 0.6 too, alike";
    }
    EXPECT_NEAR(static_cast<double>(trimmed) / all.size(), 0.3, 0.06);
    EXPECT_NEAR(static_cast<double>(moreTrimmed) / all.size(), 0.6, 0.06);
    // A wider range scales each strand's trim (its own draw), no strand jumps.
    const std::vector<float> wider = shares(1.0, 0.6);
    for (std::size_t s = 0; s < all.size(); ++s) {
        EXPECT_LE(wider[s], all[s] + 1e-5f);
        EXPECT_NEAR(1.0f - wider[s] / 0.8f, (1.0f - all[s] / 0.8f) * 6.0f / 5.0f, 1e-3f);
    }
}

TEST(Width, RootToTip) {
    OneOp g;
    const NodeId w = g.graph.addNode(op::kWidth);
    g.graph.node(w).params().set("width", 0.2);
    g.graph.node(w).params().set("profile", Ramp::linear(1.0f, 0.0f));
    g.graph.connect(g.scatter, w, 0);

    const auto out = g.eval(w);
    const std::size_t n = out->strandPointCount(0);
    const float* widths = out->widths().data();
    EXPECT_FLOAT_EQ(widths[0], 0.2f);
    EXPECT_FLOAT_EQ(widths[n - 1], 0.0f);
    EXPECT_NEAR(widths[n / 2], 0.1f, 0.02f);

    // The default profile tapers to a fifth of the width at the tip.
    g.graph.node(w).params().set("profile", Ramp::linear(1.0f, 0.2f));
    const auto tapered = g.eval(w);
    EXPECT_FLOAT_EQ(tapered->widths()[0], 0.2f);
    EXPECT_NEAR(tapered->widths()[n - 1], 0.04f, 1e-6f);
}

TEST(Width, APerPointExpressionShapesTheWidthAlongTheStrand) {
    // width = 0.2 * (1 - t) with a flat profile: t is the arc-length position,
    // so the width falls from 0.2 at the root to 0 at the tip, 0.1 half way.
    OneOp g;
    const NodeId w = g.graph.addNode(op::kWidth);
    MapRef map;
    map.source = MapRef::Source::Expression;
    map.name = "1 - t";
    map.min = 0.0f;
    map.max = 0.2f;
    g.graph.node(w).params().set("width", map);
    g.graph.node(w).params().set("profile", Ramp::constant(1.0f));
    g.graph.connect(g.scatter, w, 0);

    const auto out = g.eval(w);
    ASSERT_GT(out->strandCount(), 0u);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s), begin = out->strandBegin(s);
        const float* widths = out->widths().data() + begin;
        EXPECT_NEAR(widths[0], 0.2f, 1e-6f);
        EXPECT_NEAR(widths[n - 1], 0.0f, 1e-6f);
        for (std::size_t k = 1; k < n; ++k) EXPECT_LT(widths[k], widths[k - 1]);  // strictly thinner toward the tip
        EXPECT_NEAR(widths[(n - 1) / 2], 0.2f * (1.0f - static_cast<float>((n - 1) / 2) / static_cast<float>(n - 1)), 1e-5f);
    }
    // A per-strand expression (no point variable) is read once per strand: one width along it.
    map.name = "0.5 + 0.5 * rand";
    g.graph.node(w).params().set("width", map);
    const auto flat = g.eval(w);
    for (std::size_t s = 0; s < flat->strandCount(); ++s) {
        const std::size_t n = flat->strandPointCount(s), begin = flat->strandBegin(s);
        for (std::size_t k = 1; k < n; ++k) EXPECT_FLOAT_EQ(flat->widths()[begin + k], flat->widths()[begin]);
    }
}

TEST(GraphIo, DefaultGraphEvaluates) {
    GraphDocument doc = loadGraphJson("");
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(8), 1);
    EvalContext ctx;
    ctx.externals = &ext;

    const auto strands = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(strands);
    EXPECT_NEAR(static_cast<double>(strands->strandCount()), 80.0, 25.0);  // default density 80, area 1
    EXPECT_FLOAT_EQ(strands->widths()[0], 0.007f);  // Width's default, at the root
}

TEST(GraphIo, RoundTripKeepsParamsAndConnections) {
    GraphDocument doc = loadGraphJson("{}");
    doc.graph.node(doc.node("scatter")).params().set("density", 42.0);
    doc.graph.node(doc.node("resample")).params().set("segments", std::int64_t{5});

    const std::string text = saveGraphJson(doc);
    GraphDocument again = loadGraphJson(text);
    EXPECT_DOUBLE_EQ(again.graph.node(again.node("scatter")).params().getDouble("density"), 42.0);
    EXPECT_EQ(again.graph.node(again.node("resample")).params().getInt("segments"), 5);
    EXPECT_EQ(again.nameOf(again.output), "output");
    EXPECT_EQ(saveGraphJson(again), text);
}

TEST(GraphIo, TheEditorsLayoutTravelsWithTheGraph) {
    // Each node's "ui", the graph's "backdrops" and its "pins" (the links' reroute points): carried
    // by load and save (and clone), so the layout Maya's Graph Editor and the app's node graph keep
    // survives every host.
    GraphDocument doc = loadGraphJson(R"({
        "nodes": [{"name": "s", "type": "Scalp", "ui": {"x": 12.5, "y": -3}},
                  {"name": "o", "type": "Output"}],
        "connections": [], "output": "o",
        "backdrops": [{"title": "Roots", "x": 0, "y": 0, "w": 10, "h": 20, "color": [1, 2, 3]}],
        "pins": {"points": [{"id": "pin1", "x": 5, "y": 6}], "routes": [{"from": "s", "to": "o", "input": "strands", "via": ["pin1"]}]}})");
    ASSERT_EQ(doc.positions.size(), 1u);
    EXPECT_DOUBLE_EQ(doc.positions.at("s").x, 12.5);
    EXPECT_DOUBLE_EQ(doc.positions.at("s").y, -3.0);
    const GraphDocument copy = doc.clone();
    const nlohmann::json saved = nlohmann::json::parse(saveGraphJson(copy));
    for (const nlohmann::json& n : saved["nodes"]) {
        if (n["name"] == "s") EXPECT_EQ(n["ui"], nlohmann::json({{"x", 12.5}, {"y", -3.0}}));
        if (n["name"] == "o") EXPECT_FALSE(n.contains("ui"));
    }
    EXPECT_EQ(saved["backdrops"][0]["title"], "Roots");
    EXPECT_EQ(saved["pins"]["points"][0]["id"], "pin1");
    EXPECT_EQ(saved["pins"]["routes"][0]["via"], nlohmann::json({"pin1"}));
    EXPECT_EQ(saveGraphJson(loadGraphJson(saveGraphJson(doc))), saveGraphJson(doc));
    const nlohmann::json bare = nlohmann::json::parse(saveGraphJson(loadGraphJson("{}")));
    EXPECT_FALSE(bare.contains("backdrops"));
    EXPECT_FALSE(bare.contains("pins"));
}

TEST(GraphIo, IntegralJsonNumbersBecomeDeclaredTypes) {
    GraphDocument doc = loadGraphJson(R"({
        "nodes": [{"name": "s", "type": "Scatter", "params": {"density": 50}},
                  {"name": "l", "type": "Resample", "params": {"segments": 4.0}},
                  {"name": "o", "type": "Output"}],
        "connections": [{"from": "s", "to": "l", "input": "strands"}, {"from": "l", "to": "o", "input": "strands"}],
        "output": "o"})");
    const ParamSet& p = doc.graph.node(doc.node("s")).params();
    EXPECT_TRUE(std::holds_alternative<double>(p.get("density")));
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(doc.graph.node(doc.node("l")).params().get("segments")));
    EXPECT_EQ(doc.graph.node(doc.node("l")).params().getInt("segments"), 4);
}

TEST(GraphIo, ErrorsAreGraphErrors) {
    EXPECT_THROW(loadGraphJson("{not json"), GraphError);
    EXPECT_THROW(loadGraphJson(R"({"nodes": [{"name": "a", "type": "Nope"}], "output": "a"})"), GraphError);
    EXPECT_THROW(loadGraphJson(R"({"nodes": [{"name": "a", "type": "Output"}]})"), GraphError);
    EXPECT_THROW(loadGraphJson(R"({"version": 99, "nodes": [], "output": "a"})"), GraphError);
}

TEST(GraphIo, EditingOneParamReevaluatesOnlyDownstream) {
    GraphDocument doc = loadGraphJson("");
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(8), 1);
    EvalContext ctx;
    ctx.externals = &ext;

    doc.graph.evaluate(doc.output, ctx);
    const std::size_t first = doc.graph.evaluationCount();
    doc.graph.node(doc.node("width")).params().set("width", 0.02);
    doc.graph.evaluate(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount() - first, 3u);  // width, resample, output

    ext.set("scalp", test::makePlane(9), 2);  // new mesh: everything downstream of scalp
    doc.graph.evaluate(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount() - first, 3u + 9u);  // guides unchanged
}


TEST(Interpolate, WeightsAreContinuousWhereTheNeighborSetChanges) {
    // Six guides pointing six ways, the default three blended per strand: as
    // a strand's nearest guides change across the plane, its shape must not
    // jump (Shepard weights reach zero at the guide dropping out, and every
    // guide tied at a Voronoi vertex still carries weight).
    OneOp g(3000.0);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.05f, 0, 0.45f}, {-1, 0, 0}));
    guides->add(2, test::straightCurve({0.95f, 0, 0.55f}, {1, 0, 0}));
    guides->add(3, test::straightCurve({0.45f, 0, 0.05f}, {0, 0, -1}));
    guides->add(4, test::straightCurve({0.55f, 0, 0.95f}, {0, 0, 1}));
    guides->add(5, test::straightCurve({0.30f, 0, 0.70f}, {-0.7f, 0, 0.7f}));
    guides->add(6, test::straightCurve({0.65f, 0, 0.35f}, {0.7f, 0, -0.7f}));
    g.externals.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = g.graph.addNode(op::kGuides);
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.connect(g.scatter, interp, 0);
    g.graph.connect(guideNode, interp, 1);
    // Tips of strands rooted close together stay close together, with either blend.
    auto worstFor = [&](const char* blend) {
        g.graph.node(interp).params().set("blend", std::string(blend));
        const auto out = g.eval(interp);
        EXPECT_TRUE(out && out->strandCount() > 1000u);
        float worst = 0.0f;
        const std::size_t n = out ? out->strandCount() : 0;
        for (std::size_t a = 0; a < n; ++a) {
            const Vec3f* pa = out->strandPoints(a);
            const std::size_t na = out->strandPointCount(a);
            for (std::size_t b = a + 1; b < n; ++b) {
                const Vec3f* pb = out->strandPoints(b);
                const float roots = (pa[0] - pb[0]).length();
                if (roots > 0.02f) continue;
                const float tips = ((pa[na - 1] - pa[0]) - (pb[out->strandPointCount(b) - 1] - pb[0])).length();
                worst = std::max(worst, tips);
            }
        }
        return worst;
    };
    // A pop (the old hard cutoff) swaps whole guides: about 2 here. Smooth
    // weights leave the gradient of this deliberately harsh layout (unit hair
    // on guides pointing against each other, a third of their length apart).
    const float offsets = worstFor("offsets");
    EXPECT_LT(offsets, 0.5f) << "neighbouring strands differ by " << offsets << " at the tip";
    // Blend directions turns strands between guides pointing apart where offsets shrinks them, so
    // its gradient is steeper there; where the guides cancel out it eases to no length, so no strand
    // flips (a flip: about 2 here).
    const float directions = worstFor("directions");
    EXPECT_LT(directions, 1.0f) << "neighbouring strands differ by " << directions << " at the tip (blend directions)";
}

TEST(Interpolate, AStrandRootedOnAGuideRootStaysFiniteAtAnyPower) {
    // A strand whose root sits on a guide root has a weight base of a million; raised to a
    // power past 6 that overflowed a float, and the blend came out NaN. Weights are now taken
    // relative to the largest base, so no power overflows.
    OneOp g(300.0);
    const auto bare = g.eval(g.scatter);
    ASSERT_TRUE(bare);
    ASSERT_GT(bare->strandCount(), 10u);
    const Vec3f onRoot = bare->strandPoints(3)[0];
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve(onRoot, {0, 1, 0}));
    guides->add(2, test::straightCurve({0.9f, 0, 0.9f}, {1, 0, 0}));
    guides->add(3, test::straightCurve({0.1f, 0, 0.9f}, {-1, 0, 0}));
    g.externals.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = g.graph.addNode(op::kGuides);
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.node(interp).params().set("power", 8.0);
    g.graph.connect(g.scatter, interp, 0);
    g.graph.connect(guideNode, interp, 1);
    const auto out = g.eval(interp);
    ASSERT_TRUE(out);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* p = out->strandPoints(s);
        for (std::size_t k = 0; k < out->strandPointCount(s); ++k) {
            ASSERT_TRUE(std::isfinite(p[k].x) && std::isfinite(p[k].y) && std::isfinite(p[k].z)) << "strand " << s;
        }
    }
    // The strand on the guide root takes that guide: straight up.
    const Vec3f* p = out->strandPoints(3);
    const Vec3f tip = p[out->strandPointCount(3) - 1] - p[0];
    EXPECT_NEAR(tip.x, 0.0f, 1e-3f);
    EXPECT_NEAR(tip.y, 1.0f, 1e-3f);
}

TEST(Interpolate, TheShapeIsContinuousAcrossTheGuidesFringe) {
    // Guides on a strip at one side of the plane, pointing different ways: past their reach a
    // strand blends the nearest few by inverse distance, and just inside the last reach it
    // took that one guide alone, so the shape popped along the fringe. The two blends are now
    // mixed over the outer band of the reaches: strands rooted close together stay close.
    OneOp g(6000.0);
    auto guides = std::make_shared<GuideSet>();
    int id = 1;
    for (int row = 0; row < 8; ++row) {
        const float z = 0.06f + 0.125f * static_cast<float>(row);
        guides->add(id++, test::straightCurve({0.08f, 0, z}, row % 2 ? Vec3f{1, 0, 0} : Vec3f{-1, 0, 0}));
        guides->add(id++, test::straightCurve({0.20f, 0, z + 0.06f}, row % 2 ? Vec3f{0, 0, 1} : Vec3f{0, 0, -1}));
    }
    g.externals.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = g.graph.addNode(op::kGuides);
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.connect(g.scatter, interp, 0);
    g.graph.connect(guideNode, interp, 1);
    // (With either blend: directions turns strands between guides pointing apart where offsets
    // shrinks them, a steeper gradient there, but no flip, a pop's size.)
    for (const auto& [blend, bound] : {std::pair<const char*, float>{"offsets", 0.8f}, {"directions", 1.2f}}) {
    SCOPED_TRACE(blend);
    g.graph.node(interp).params().set("blend", std::string(blend));
    const auto out = g.eval(interp);
    ASSERT_TRUE(out);
    const std::size_t n = out->strandCount();
    ASSERT_GT(n, 3000u);
    // The guides' reach ends somewhere past x = 0.2: every pair of strands rooted within 0.01 of
    // each other out there has tips within a fraction of the pop a hard fringe gives (1.4 here,
    // at the fringe). What remains (0.54) is the blend's own gradient right beside guides pointing
    // against each other, as deep inside the reaches as at the fringe.
    float worst = 0.0f;
    Vec3f worstAt;
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return out->strandPoints(a)[0].x < out->strandPoints(b)[0].x; });
    for (std::size_t ia = 0; ia < n; ++ia) {
        const std::size_t a = order[ia];
        const Vec3f* pa = out->strandPoints(a);
        if (pa[0].x < 0.25f) continue;
        const Vec3f ta = pa[out->strandPointCount(a) - 1] - pa[0];
        for (std::size_t ib = ia + 1; ib < n; ++ib) {
            const std::size_t b = order[ib];
            const Vec3f* pb = out->strandPoints(b);
            if (pb[0].x - pa[0].x > 0.01f) break;
            if ((pa[0] - pb[0]).length() > 0.01f) continue;
            const Vec3f tb = pb[out->strandPointCount(b) - 1] - pb[0];
            if ((ta - tb).length() > worst) {
                worst = (ta - tb).length();
                worstAt = pa[0];
            }
        }
    }
    EXPECT_LT(worst, bound) << "neighbouring strands across the fringe differ by " << worst << " at the tip, rooted at "
                            << worstAt.x << " " << worstAt.z;
    }
}

TEST(Interpolate, PreserveLengthKeepsTheGuidesLength) {
    // Two guides of length 1 pointing opposite ways: a strand between them
    // blends to a much shorter curve unless the length is restored.
    OneOp g(200.0);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0, 0.5f}, {-0.7f, 0.7f, 0}));
    guides->add(2, test::straightCurve({1.0f, 0, 0.5f}, {0.7f, 0.7f, 0}));
    g.externals.set("guides", GuidesPtr(guides), guides->contentHash());
    const NodeId guideNode = g.graph.addNode(op::kGuides);
    const NodeId interp = g.graph.addNode(op::kInterpolate);
    g.graph.node(interp).params().set("neighbors", std::int64_t{2});
    g.graph.node(interp).params().set("blend", std::string("offsets"));  // (directions keeps it already)
    g.graph.connect(g.scatter, interp, 0);
    g.graph.connect(guideNode, interp, 1);
    auto middleLength = [&](bool preserve) {
        g.graph.node(interp).params().set("preserveLength", preserve);
        const auto out = g.eval(interp);
        float sum = 0.0f;
        std::size_t count = 0;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const Vec3f* p = out->strandPoints(s);
            if (std::abs(p[0].x - 0.5f) > 0.05f) continue;
            float len = 0.0f;
            for (std::size_t i = 1; i < out->strandPointCount(s); ++i) len += (p[i] - p[i - 1]).length();
            sum += len;
            ++count;
        }
        return count ? sum / static_cast<float>(count) : 0.0f;
    };
    const float guideLength = 0.7f * std::sqrt(2.0f);
    EXPECT_NEAR(middleLength(true), guideLength, 0.02f);
    EXPECT_LT(middleLength(false), 0.85f * guideLength);
}

TEST(Interpolate, BlendDirectionsKeepsTheGuidesLengthBetweenPartedGuides) {
    // Two guides of the same length pointing apart (a parting): blend offsets pulls the strands
    // between them in (preserveLength off), blend directions keeps the guides' length and points
    // them the way between, straight up halfway. With 17 points a strand and with 600 (more samples
    // than the CPU engine holds: the streamed blend).
    const auto plane = test::makePlane(8, 1.0f);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.0f, 0, 0.5f}, {-0.7f, 0.7f, 0}));
    guides->add(2, test::straightCurve({1.0f, 0, 0.5f}, {0.7f, 0.7f, 0}));
    guides->rootOnMesh(*plane);
    const float guideLength = 0.7f * std::sqrt(2.0f);
    for (const std::size_t points : {std::size_t{17}, std::size_t{600}}) {
        auto strands = std::make_shared<StrandSet>();
        for (int i = 1; i < 100; ++i) {
            const SurfacePoint at = plane->closestPoint({0.01f * static_cast<float>(i), 0.0f, 0.5f});
            std::vector<Vec3f> pts(points);
            for (std::size_t j = 0; j < points; ++j) pts[j] = at.position + Vec3f(0, 0.3f * static_cast<float>(j) / static_cast<float>(points - 1), 0);
            strands->addStrand(static_cast<StrandId>(i), pts, {}, at.root);
        }
        auto run = [&](const std::string& blend) {
            auto op = OperatorRegistry::instance().create(op::kInterpolate);
            op->params().set("neighbors", std::int64_t{2});
            op->params().set("preserveLength", false);
            op->params().set("blend", blend);
            ExternalInputs ext;
            EvalContext ctx;
            ctx.externals = &ext;
            return valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(strands)), Value(GuidesPtr(guides)), Value(MeshPtr(plane))}));
        };
        // The strands rooted near the middle: their mean length; how far the middle one's tip leans in x.
        auto middle = [&](const StrandsPtr& out, float* lean) {
            float sum = 0.0f;
            std::size_t count = 0;
            *lean = 0.0f;
            for (std::size_t s = 0; s < out->strandCount(); ++s) {
                const Vec3f* p = out->strandPoints(s);
                const std::size_t n = out->strandPointCount(s);
                if (std::abs(p[0].x - 0.5f) > 0.03f) continue;
                float len = 0.0f;
                for (std::size_t i = 1; i < n; ++i) len += (p[i] - p[i - 1]).length();
                sum += len;
                ++count;
                if (std::abs(p[0].x - 0.5f) < 0.005f) *lean = std::max(*lean, std::abs(p[n - 1].x - 0.5f));
            }
            return count ? sum / static_cast<float>(count) : 0.0f;
        };
        float lean = 0.0f;
        EXPECT_LT(middle(run("offsets"), &lean), 0.85f * guideLength) << points;
        EXPECT_NEAR(middle(run("directions"), &lean), guideLength, 0.01f * guideLength) << points;
        EXPECT_LT(lean, 0.01f) << points;
        // At a guide's root, its shape (its own way, its length).
        const StrandsPtr out = run("directions");
        const Vec3f* first = out->strandPoints(0);
        const Vec3f tip = first[out->strandPointCount(0) - 1] - first[0];
        EXPECT_LT(tip.x, -0.5f) << points;
        EXPECT_NEAR(tip.length(), guideLength, 0.05f) << points;
    }
}

TEST(Interpolate, GuideWeightPullsTheBlendTowardTheHeavierGuides) {
    // Two guides pointing apart, their guideWeight from a guide layer: the strands between lean
    // toward the heavier one; a guide weighed 0 shapes nothing another guide reaches. A number, or
    // every guide weighed alike, blends as without.
    const auto plane = test::makePlane(8, 1.0f);
    auto strands = std::make_shared<StrandSet>();
    for (int i = 1; i < 100; ++i) {
        const SurfacePoint at = plane->closestPoint({0.01f * static_cast<float>(i), 0.0f, 0.5f});
        strands->addStrand(static_cast<StrandId>(i), {at.position, at.position + Vec3f(0, 0.15f, 0), at.position + Vec3f(0, 0.3f, 0)},
                           {}, at.root);
    }
    auto run = [&](float left, float right, const ParamValue& weight) {
        auto guides = std::make_shared<GuideSet>();
        guides->add(1, test::straightCurve({0.0f, 0, 0.5f}, {-0.7f, 0.7f, 0})).attributes["w"] = left;
        guides->add(2, test::straightCurve({1.0f, 0, 0.5f}, {0.7f, 0.7f, 0})).attributes["w"] = right;
        guides->rootOnMesh(*plane);
        auto op = OperatorRegistry::instance().create(op::kInterpolate);
        op->params().set("neighbors", std::int64_t{2});
        op->params().set("guideWeight", weight);
        ExternalInputs ext;
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(strands)), Value(GuidesPtr(guides)), Value(MeshPtr(plane))}));
    };
    // The tip of the strand rooted halfway, and of the one at the right guide's root, from their roots.
    auto tip = [](const StrandsPtr& out, std::size_t s) {
        const Vec3f* p = out->strandPoints(s);
        return p[out->strandPointCount(s) - 1] - p[0];
    };
    MapRef layer;
    layer.source = MapRef::Source::Guide;
    layer.name = "w";
    // (Positions: the layer's values become a strand channel.)
    const std::vector<Vec3f> plain = run(1.0f, 1.0f, 1.0)->positions();
    EXPECT_EQ(run(1.0f, 0.25f, 0.3)->positions(), plain);
    EXPECT_EQ(run(0.5f, 0.5f, ParamValue(layer))->positions(), plain);
    EXPECT_NEAR(tip(run(1.0f, 1.0f, 1.0), 49).x, 0.0f, 0.02f);
    const StrandsPtr leaning = run(1.0f, 0.25f, ParamValue(layer));
    EXPECT_LT(tip(leaning, 49).x, -0.1f);
    EXPECT_GT(tip(leaning, 98).x, 0.15f) << "at its root a guide still wins";
    // The right guide weighed 0: every strand the left one reaches takes its shape.
    const StrandsPtr left = run(1.0f, 0.0f, ParamValue(layer));
    EXPECT_LT(tip(left, 49).x, -0.2f);
    EXPECT_LT(tip(left, 98).x, -0.2f);
    // Both 0: alike, as without.
    EXPECT_EQ(run(0.0f, 0.0f, ParamValue(layer))->positions(), plain);
}

// Scalp, guides and parts into Scatter and Interpolate, the strands' channels included.
struct BoundInterpolate {
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "interp", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 1200.0}},
        {"name": "interp", "type": "Interpolate", "params": {"limitDistance": 0.4}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "interp", "input": "strands"},
                      {"from": "guides", "to": "interp", "input": "guides"},
                      {"from": "scalp", "to": "interp", "input": "scalp"}]})");
    Operator& node(const char* name) { return doc.graph.node(doc.node(name)); }
};

struct InterpInputs {
    MeshPtr scalp;
    std::shared_ptr<GuideSet> guides;
    PartsPtr parts;
};

StrandsPtr evalInterp(BoundInterpolate& g, const InterpInputs& in) {
    ExternalInputs ext;
    setGroomExternals(ext, in.scalp, in.guides, nullptr, in.parts);
    EvalContext ctx;
    ctx.externals = &ext;
    return g.doc.graph.evaluateStrands(g.doc.output, ctx);
}

// A 4 x 4 grid of guides on the plane, each its own way, with a painted value and a per-point layer.
std::shared_ptr<GuideSet> guideGrid(const ScalpMesh& scalp) {
    auto g = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            const float x = 0.12f + 0.25f * static_cast<float>(i), z = 0.12f + 0.25f * static_cast<float>(j);
            Guide& guide = g->add(id, test::straightCurve({x, 0.0f, z}, {0.1f * static_cast<float>(i - 1), 0.5f, 0.05f * static_cast<float>(j)}, 6));
            guide.attributes["clump"] = static_cast<float>(id) * 0.1f;
            guide.pointAttributes["melanin"] = std::vector<float>(7, static_cast<float>(j) * 0.25f);
            ++id;
        }
    }
    g->rootOnMesh(scalp);
    return g;
}

PartsPtr middleParts(const ScalpMesh& scalp, float x = 0.5f) {
    PartSet set;
    set.parts.push_back(PartSet::fromPoints("middle", {{x, 0.0f, 0.1f}, {x, 0.0f, 0.5f}, {x, 0.0f, 0.9f}}, scalp));
    return std::make_shared<const PartLines>(set, scalp);
}

void expectSameStrandsAndChannels(const StrandSet& a, const StrandSet& b, const std::string& what) {
    expectIdentical(a, b, what);
    ASSERT_EQ(a.strandChannelNames(), b.strandChannelNames()) << what;
    for (const std::string& name : a.strandChannelNames()) ASSERT_EQ(*a.strandChannel(name), *b.strandChannel(name)) << what << " " << name;
    ASSERT_EQ(a.pointChannelNames(), b.pointChannelNames()) << what;
    for (const std::string& name : a.pointChannelNames()) ASSERT_EQ(*a.pointChannel(name), *b.pointChannel(name)) << what << " " << name;
}

TEST(Interpolate, ABrushDabReblendsWithoutBindingAgain) {
    // The guides' shapes change (not their roots), and the scalp deforms under a rest pose: the
    // strands keep their guides and weights, and come out as a fresh Interpolate makes them.
    BoundInterpolate kept;
    InterpInputs in{bentPlane(0.0f), nullptr, nullptr};
    in.guides = guideGrid(*in.scalp);
    in.parts = middleParts(*in.scalp);
    ASSERT_TRUE(evalInterp(kept, in));
    for (int dab = 1; dab <= 3; ++dab) {
        auto guides = std::make_shared<GuideSet>(*in.guides);
        for (Guide& g : guides->guides()) {
            for (std::size_t i = 1; i < g.points.size(); ++i) g.points[i].x += 0.02f * static_cast<float>(dab * i);
            g.attributes["clump"] += 0.5f;
        }
        in.guides = guides;
        if (dab == 3) {
            in.scalp = bentPlane(0.3f);  // the rest pose stays: so do the roots there
            in.parts = middleParts(*in.scalp);
        }
        const std::size_t bound = interpolateBindCount();
        const StrandsPtr out = evalInterp(kept, in);
        EXPECT_EQ(interpolateBindCount(), bound) << "dab " << dab << " bound the strands again";
        BoundInterpolate fresh;
        const StrandsPtr expected = evalInterp(fresh, in);
        ASSERT_TRUE(out && expected);
        expectSameStrandsAndChannels(*out, *expected, "dab " + std::to_string(dab));
    }
}

TEST(Interpolate, TheBindingKeepsEachStrandsWeightsAsSixteenBitSharesOfOne) {
    // Each bound strand's weights are codes whose shares (code + 1, in 65536ths) sum to 65536
    // exactly, a guide each at most once; a strand left as it came has none. The records hold
    // 16-bit guide indices and weights (no twists on a plane).
    BoundInterpolate g;
    InterpInputs in{bentPlane(0.0f), nullptr, nullptr};
    in.guides = guideGrid(*in.scalp);
    in.parts = middleParts(*in.scalp);
    const StrandsPtr out = evalInterp(g, in);
    ASSERT_TRUE(out);
    InterpolateState state;
    ASSERT_TRUE(interpolateState(g.node("interp"), out, &state));
    InterpolateRecords r;
    interpolateRecords(state, &r);
    const std::size_t n = out->strandCount();
    ASSERT_EQ(r.offsets.size(), n + 1);
    std::size_t bound = 0;
    for (std::size_t s = 0; s < n; ++s) {
        const std::uint32_t first = r.offsets[s], count = r.offsets[s + 1] - first;
        EXPECT_EQ(r.bound[s] != 0, count > 0) << s;
        if (count == 0) continue;
        ++bound;
        std::uint32_t shares = 0;
        std::set<std::uint32_t> guides;
        for (std::uint32_t i = first; i < first + count; ++i) {
            shares += r.weights[i] + 1u;
            guides.insert(r.guides[i]);
            EXPECT_EQ(interpolateWeight(r.weights[i]), static_cast<float>(r.weights[i] + 1u) / 65536.0f);
        }
        EXPECT_EQ(shares, 65536u) << s;
        EXPECT_EQ(guides.size(), count) << s;
    }
    EXPECT_GT(bound, n / 2);
    const std::size_t chunks = (n + interpolateChunkStrands() - 1) / interpolateChunkStrands();
    EXPECT_EQ(interpolateRecordBytes(state), 4 * (n + chunks) + 4 * r.guides.size() + 5 * n);
}

TEST(Interpolate, TheBindingIsMadeAgainWhenRootsPartsOrItsParamsChange) {
    using Change = std::function<void(BoundInterpolate&, InterpInputs&)>;
    auto edited = [](InterpInputs& in, const std::function<void(GuideSet&)>& edit) {
        auto guides = std::make_shared<GuideSet>(*in.guides);
        edit(*guides);
        in.guides = guides;
    };
    const std::vector<std::pair<const char*, Change>> changes = {
        {"guide added",
         [&](BoundInterpolate&, InterpInputs& in) {
             edited(in, [&](GuideSet& g) {
                 g.add(99, test::straightCurve({0.5f, 0.0f, 0.5f}, {0, 0.4f, 0.2f}, 6));
                 g.guides().back().root = in.scalp->closestPoint({0.5f, 0.0f, 0.5f}).root;
             });
         }},
        {"guide removed", [&](BoundInterpolate&, InterpInputs& in) { edited(in, [](GuideSet& g) { g.guides().erase(g.guides().begin() + 5); }); }},
        {"guide slid", [&](BoundInterpolate&, InterpInputs& in) {
             edited(in, [&](GuideSet& g) { g.guides()[6].root = in.scalp->closestPoint({0.4f, 0.0f, 0.55f}).root; });
         }},
        {"part moved", [](BoundInterpolate&, InterpInputs& in) { in.parts = middleParts(*in.scalp, 0.6f); }},
        {"parts removed", [](BoundInterpolate&, InterpInputs& in) { in.parts = nullptr; }},
        {"respectParts off", [](BoundInterpolate& g, InterpInputs&) { g.node("interp").params().set("respectParts", false); }},
        {"neighbors", [](BoundInterpolate& g, InterpInputs&) { g.node("interp").params().set("neighbors", std::int64_t{5}); }},
        {"smoothness", [](BoundInterpolate& g, InterpInputs&) { g.node("interp").params().set("smoothness", 0.8); }},
        {"strand roots", [](BoundInterpolate& g, InterpInputs&) { g.node("scatter").params().set("seed", std::int64_t{4}); }},
        {"rest pose", [](BoundInterpolate&, InterpInputs& in) { in.scalp = bentPlane(0.0f, 0.3f, 0.4f); }},
    };
    for (const auto& [what, change] : changes) {
        BoundInterpolate kept;
        InterpInputs in{bentPlane(0.0f), nullptr, nullptr};
        in.guides = guideGrid(*in.scalp);
        in.parts = middleParts(*in.scalp);
        const StrandsPtr before = evalInterp(kept, in);
        ASSERT_TRUE(before);
        change(kept, in);
        const std::size_t bound = interpolateBindCount();
        const StrandsPtr after = evalInterp(kept, in);
        EXPECT_EQ(interpolateBindCount(), bound + 1) << what << ": not bound again";
        BoundInterpolate fresh;
        fresh.node("interp").params() = kept.node("interp").params();
        fresh.node("scatter").params() = kept.node("scatter").params();
        const StrandsPtr expected = evalInterp(fresh, in);
        ASSERT_TRUE(after && expected) << what;
        expectSameStrandsAndChannels(*after, *expected, what);
        EXPECT_NE(after->positions(), before->positions()) << what << ": the change changed nothing";
    }
}

TEST(Interpolate, MovingAFewGuideRootsBindsOnlyTheStrandsTheyReach) {
    // Strands and guides on lattices (many strands equally far from several guides: ties), one
    // Interpolate kept across root moves, each result as a fresh Interpolate's; only the strands
    // near the moved guides are searched again.
    const auto plane = test::makePlane(10, 1.0f);
    auto strands = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 60; ++j) {
            const Vec3f root(0.005f + static_cast<float>(i) / 60.0f, 0.0f, 0.005f + static_cast<float>(j) / 60.0f);
            strands->addStrand(id++, test::straightCurve(root, {0, 0.3f, 0}, 7), {}, plane->closestPoint(root).root);
        }
    }
    auto lattice = std::make_shared<GuideSet>();
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            const Vec3f root(0.005f + static_cast<float>(i) / 10.0f, 0.0f, 0.005f + static_cast<float>(j) / 10.0f);
            Guide& g = lattice->add(static_cast<std::uint64_t>(100 + i * 10 + j),
                                    test::straightCurve(root, {0.05f * static_cast<float>(j - 5), 0.4f, 0.02f * static_cast<float>(i)}, 6));
            g.root = plane->closestPoint(root).root;
        }
    }
    ExternalInputs ext;
    setGroomExternals(ext, plane, nullptr, nullptr, middleParts(*plane, 0.47f));
    EvalContext ctx;
    ctx.externals = &ext;
    const auto kept = OperatorRegistry::instance().create(op::kInterpolate);
    auto run = [&](const Operator& op, const GuidesPtr& guides) {
        return valueAs<StrandsPtr>(op.evaluate(ctx, {Value(StrandsPtr(strands)), Value(guides), Value(MeshPtr(plane))}));
    };
    ASSERT_TRUE(run(*kept, lattice));
    GuidesPtr guides = lattice;
    for (int step = 1; step <= 4; ++step) {
        auto moved = std::make_shared<GuideSet>(*guides);
        // One guide slides a little (step 4: one slides onto another lattice point's tie line).
        const std::size_t which = static_cast<std::size_t>(step * 23 % 100);
        Guide& g = moved->guides()[which];
        const Vec3f to = plane->position(g.root) + Vec3f(step == 4 ? 0.05f : 0.013f, 0.0f, 0.0f);
        g.root = plane->closestPoint(to).root;
        for (Vec3f& p : g.points) p.x += 0.013f;
        guides = moved;
        const std::size_t searched = interpolateStrandBindCount();
        const StrandsPtr out = run(*kept, guides);
        const std::size_t again = interpolateStrandBindCount() - searched;
        const auto fresh = OperatorRegistry::instance().create(op::kInterpolate);
        const StrandsPtr expected = run(*fresh, guides);
        ASSERT_TRUE(out && expected);
        expectSameStrandsAndChannels(*out, *expected, "step " + std::to_string(step));
        EXPECT_GT(again, 0u) << "step " << step;
        EXPECT_LT(again, strands->strandCount() * 3 / 4) << "step " << step << ": most strands searched again";
    }
}

TEST(Interpolate, BrushDabsOnAHierarchyGiveWhatAFreshInterpolateGives) {
    // A dab re-roots the guides it touches (their children follow): kept and fresh agree.
    const auto sphere = test::makeSphere(24, 48, 1.0f);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(9);
    for (int i = 0; i < 120; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.2f, 0), 10));
    }
    h.evaluate(sphere.get());
    ChildGenerationParams params;
    params.count = 4;
    h.generateChildren(primary, "children", params);
    h.evaluate(sphere.get());
    GraphDocument kept = loadGraphJson(R"({"version": 1, "output": "interp", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 3000.0}},
        {"name": "interp", "type": "Interpolate"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "interp", "input": "strands"},
                      {"from": "guides", "to": "interp", "input": "guides"},
                      {"from": "scalp", "to": "interp", "input": "scalp"}]})");
    auto evaluate = [&](GraphDocument& doc) {
        ExternalInputs ext;
        setGroomExternals(ext, sphere, nullptr, std::make_shared<GuideHierarchy>(h));
        EvalContext ctx;
        ctx.externals = &ext;
        return doc.graph.evaluateStrands(doc.output, ctx);
    };
    ASSERT_TRUE(evaluate(kept));
    BrushSettings brush;
    brush.radius = 0.3f;
    brush.level = 0;
    brush.propagate = true;
    for (int dab = 0; dab < 4; ++dab) {
        const float x = -0.3f + 0.15f * static_cast<float>(dab);
        applyBrushStroke(h, sphere.get(), brush, {{x, 1.2f, 0.0f}, {x + 0.1f, 1.2f, 0.05f}});
        const std::size_t searched = interpolateStrandBindCount();
        const StrandsPtr out = evaluate(kept);
        const std::size_t again = interpolateStrandBindCount() - searched;
        GraphDocument fresh = loadGraphJson(saveGraphJson(kept));
        const StrandsPtr expected = evaluate(fresh);
        ASSERT_TRUE(out && expected);
        expectSameStrandsAndChannels(*out, *expected, "dab " + std::to_string(dab));
        EXPECT_LT(again, out->strandCount()) << "dab " << dab << ": every strand searched again";
    }
}

// Strands rooted on `scalp` at `roots` (none: unrooted, at the roots as given), of 16, 0, 5, 1, 33,
// 2 and 40 points in turn.
StrandsPtr strandsAt(const ScalpMesh* scalp, const std::vector<Vec3f>& roots, const Vec3f& up) {
    auto strands = std::make_shared<StrandSet>();
    const std::size_t counts[] = {16, 0, 5, 1, 33, 2, 40, 16, 16};
    for (std::size_t s = 0; s < roots.size(); ++s) {
        StrandRoot root;
        Vec3f at = roots[s];
        if (scalp) {
            const SurfacePoint p = scalp->closestPoint(roots[s]);
            root = p.root;
            at = p.position;
        }
        const std::size_t n = counts[s % 9];
        std::vector<Vec3f> points(n);
        for (std::size_t j = 0; j < n; ++j) {
            const float t = static_cast<float>(j) / 15.0f;
            points[j] = at + up * (0.4f * t) + Vec3f(0.05f * std::sin(static_cast<float>(s + j)), 0.0f, 0.02f * t * t);
        }
        strands->addStrand(static_cast<StrandId>(s + 1), points, {}, root);
    }
    return StrandsPtr(strands);
}

// Interpolate's outputs over the cases its code branches on, in order, each named: a curved scalp
// with a rest pose, a sphere with guides opposite strands (normals turned half way round), no
// scalp; parts; a radius and its falloff; take length; preserveLength off; blend directions; one neighbor and
// sixteen (every guide, plain inverse distance); powers 0, 1 and 3; straight guide spans; guide
// attributes and per-point layers (one longer than its guide, one missing on a guide); a guide of
// one point (skipped) and one of two; strands of 0, 1, 2, 5, 16, 33 and 40 points (fewer and more
// than the guides' 32 samples), some beyond every guide's reach; strands as far from several
// guides to the bit (ties); and guides crowded at one place.
std::vector<std::pair<std::string, StrandsPtr>> interpolateCases() {
    const MeshPtr plane = bentPlane(0.6f);
    std::vector<Vec3f> planeRoots;
    for (int i = 0; i < 80; ++i) {  // (enough strands that the binding searches from cells)
        for (int j = 0; j < 80; ++j) planeRoots.push_back({0.02f + 0.0122f * static_cast<float>(i), 0.0f, 0.03f + 0.0121f * static_cast<float>(j)});
    }
    const StrandsPtr planeStrands = strandsAt(plane.get(), planeRoots, {0.0f, 1.0f, 0.0f});
    auto planeGuides = [&] {
        auto g = std::make_shared<GuideSet>(*guideGrid(*plane));
        g->guides()[3].pointAttributes["melanin"] = std::vector<float>(11, 0.9f);  // longer than its guide
        g->guides()[5].pointAttributes.erase("melanin");                            // its scalar stands in
        g->guides()[5].attributes["melanin"] = 0.4f;
        g->add(100, {{0.5f, 0.0f, 0.5f}});                                           // one point: skipped
        g->add(101, test::straightCurve({0.4f, 0.0f, 0.6f}, {0.2f, 0.3f, -0.1f}, 1));  // two points
        g->rootOnMesh(*plane);
        return GuidesPtr(g);
    }();
    // The sphere: strands on its vertices, guides on the vertices opposite (turned half way round).
    const auto sphere = test::makeSphere(8, 16, 1.0f);
    std::vector<Vec3f> sphereRoots, guideRoots;
    for (int copy = 0; copy < 100; ++copy) {  // (each many times over: enough to search from cells)
        for (std::size_t v = 0; v < sphere->points().size(); v += 3) sphereRoots.push_back(sphere->points()[v]);
    }
    auto sphereGuides = std::make_shared<GuideSet>();
    for (std::size_t v = 0; v < sphere->points().size(); v += 7) {
        const Vec3f p = sphere->points()[v];
        sphereGuides->add(v + 1, test::straightCurve(-p, -p * 0.5f + Vec3f(0.1f, 0.0f, 0.0f), 3));
    }
    sphereGuides->rootOnMesh(*sphere);
    const StrandsPtr sphereStrands = strandsAt(sphere.get(), sphereRoots, {0.0f, 1.0f, 0.0f});
    // Unrooted: no scalp; some strands far beyond every guide.
    std::vector<Vec3f> looseRoots = planeRoots;
    for (int i = 0; i < 20; ++i) looseRoots.push_back({3.0f + 0.1f * static_cast<float>(i), 0.0f, 0.5f});
    const StrandsPtr looseStrands = strandsAt(nullptr, looseRoots, {0.2f, 1.0f, 0.0f});
    // Ties: guides on a whole-number grid, strands half way between two or four of them (as far
    // from each to the bit), unrooted.
    auto tieGuides = std::make_shared<GuideSet>();
    std::vector<Vec3f> tieRoots;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            const float x = static_cast<float>(i), z = static_cast<float>(j);
            tieGuides->add(static_cast<std::uint64_t>(200 + 6 * i + j), test::straightCurve({x, 0.0f, z}, {0.1f * x, 1.0f, 0.05f * z}, 3));
            tieRoots.push_back({x + 0.5f, 0.0f, z + 0.5f});
            tieRoots.push_back({x, 0.0f, z + 0.5f});
        }
    }
    const std::vector<Vec3f> tieOnce = tieRoots;  // (each many times over: enough to search from cells)
    for (int copy = 0; copy < 70; ++copy) tieRoots.insert(tieRoots.end(), tieOnce.begin(), tieOnce.end());
    const StrandsPtr tieStrands = strandsAt(nullptr, tieRoots, {0.0f, 1.0f, 0.0f});
    // Crowded: 150 guides within a hair's breadth of one place among the plane's (more than a
    // search keeps near there), the plane's strands around them.
    auto crowdedGuides = std::make_shared<GuideSet>(*planeGuides);
    Rng rng(17);
    for (std::uint64_t k = 0; k < 150; ++k) {
        const Vec3f at(0.5f + rng.uniform(-0.01f, 0.01f), 0.0f, 0.5f + rng.uniform(-0.01f, 0.01f));
        crowdedGuides->add(300 + k, test::straightCurve(at, {rng.uniform(-0.2f, 0.2f), 0.5f, 0.0f}, 3));
    }
    crowdedGuides->rootOnMesh(*plane);
    // A thin flap: guides on both faces, a few over its top; strands on both faces, whose paths to
    // the other face's guides go round the top (twisted on the way).
    const MeshPtr fin = test::makeFin(1.0f, 0.06f, 1.0f, 0.04f);
    auto finGuides = std::make_shared<GuideSet>();
    std::vector<Vec3f> finRoots;
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            const float x = 0.1f + 0.2f * static_cast<float>(i), y = 0.15f + 0.2f * static_cast<float>(j);
            for (const float side : {-1.0f, 1.0f}) {
                Guide& g = finGuides->add(static_cast<std::uint64_t>(400 + finGuides->size()),
                                          test::straightCurve({x, y, 0.03f * side}, {0.2f * side, 0.1f, 0.3f * side + 0.1f * x}, 4));
                g.root = fin->closestPoint({x, y, 0.03f * side}).root;
            }
        }
    }
    for (int copy = 0; copy < 4; ++copy) {  // (enough strands to search from cells)
        for (int i = 0; i < 30; ++i) {
            for (int j = 0; j < 34; ++j) {
                finRoots.push_back({0.017f + 0.033f * static_cast<float>(i), 0.02f + 0.029f * static_cast<float>(j), copy % 2 ? 0.03f : -0.03f});
            }
        }
    }
    const StrandsPtr finStrands = strandsAt(fin.get(), finRoots, {0.0f, 0.0f, 1.0f});

    struct Case {
        std::string name;
        std::function<void(ParamSet&)> params;
        int input = 0;  // 0 plane, 1 sphere, 2 no scalp, 3 ties (no scalp), 4 crowded (plane), 5 a flap
        bool parts = false;
    };
    const std::vector<Case> cases = {
        {"defaults", [](ParamSet&) {}},
        {"parts", [](ParamSet&) {}, 0, true},
        {"radius", [](ParamSet& p) { p.set("limitDistance", 0.3); }},
        {"radius falloff", [](ParamSet& p) {
             p.set("limitDistance", 0.35);
             p.set("falloff", Ramp{{0.0f, 1.2f}, {0.5f, 0.6f}, {0.8f, -0.1f}});
         }},
        {"radius take length", [](ParamSet& p) {
             p.set("limitDistance", 0.3);
             p.set("take", std::string("length"));
         }},
        {"take length", [](ParamSet& p) { p.set("take", std::string("length")); }},
        {"no preserveLength", [](ParamSet& p) { p.set("preserveLength", false); }},
        {"directions", [](ParamSet& p) { p.set("blend", std::string("directions")); }},
        {"directions radius", [](ParamSet& p) {
             p.set("blend", std::string("directions"));
             p.set("limitDistance", 0.3);
         }},
        {"directions sixteen", [](ParamSet& p) {
             p.set("blend", std::string("directions"));
             p.set("neighbors", std::int64_t{16});
         }},
        {"one neighbor", [](ParamSet& p) { p.set("neighbors", std::int64_t{1}); }},
        {"one neighbor parts", [](ParamSet& p) { p.set("neighbors", std::int64_t{1}); }, 0, true},
        {"sixteen neighbors", [](ParamSet& p) { p.set("neighbors", std::int64_t{16}); }},
        {"power 0", [](ParamSet& p) { p.set("power", 0.0); }},
        {"power 1 reach 3", [](ParamSet& p) {
             p.set("power", 1.0);
             p.set("reach", 3.0);
         }},
        {"power 3", [](ParamSet& p) { p.set("power", 3.0); }},
        {"straight spans", [](ParamSet& p) { p.set("smoothGuides", false); }},
        {"sphere", [](ParamSet&) {}, 1},
        {"sphere sixteen", [](ParamSet& p) { p.set("neighbors", std::int64_t{16}); }, 1},
        {"no scalp", [](ParamSet&) {}, 2},
        {"no scalp radius", [](ParamSet& p) { p.set("limitDistance", 0.5); }, 2},
        {"no scalp no preserveLength", [](ParamSet& p) { p.set("preserveLength", false); }, 2},
        {"no scalp directions", [](ParamSet& p) { p.set("blend", std::string("directions")); }, 2},
        {"sphere directions", [](ParamSet& p) { p.set("blend", std::string("directions")); }, 1},
        {"ties", [](ParamSet&) {}, 3},
        {"ties one neighbor", [](ParamSet& p) { p.set("neighbors", std::int64_t{1}); }, 3},
        {"ties sixteen", [](ParamSet& p) { p.set("neighbors", std::int64_t{16}); }, 3},
        {"crowded", [](ParamSet&) {}, 4},
        {"crowded sixteen parts", [](ParamSet& p) { p.set("neighbors", std::int64_t{16}); }, 4, true},
        {"flap", [](ParamSet&) {}, 5},
        {"flap straight", [](ParamSet& p) { p.set("followSurface", false); }, 5},
        {"flap one neighbor", [](ParamSet& p) { p.set("neighbors", std::int64_t{1}); }, 5},
        {"flap radius reach 3", [](ParamSet& p) {
             p.set("limitDistance", 0.4);
             p.set("reach", 3.0);
         }, 5},
    };
    std::vector<std::pair<std::string, StrandsPtr>> out;
    for (const Case& c : cases) {
        auto op = OperatorRegistry::instance().create(op::kInterpolate);
        c.params(op->params());
        ExternalInputs ext;
        if (c.parts) ext.set("parts", Value(middleParts(*plane)), 1);
        EvalContext ctx;
        ctx.externals = &ext;
        std::vector<Value> inputs;
        if (c.input == 0) inputs = {Value(planeStrands), Value(planeGuides), Value(plane)};
        if (c.input == 1) inputs = {Value(sphereStrands), Value(GuidesPtr(sphereGuides)), Value(MeshPtr(sphere))};
        if (c.input == 2) inputs = {Value(looseStrands), Value(planeGuides), Value()};
        if (c.input == 3) inputs = {Value(tieStrands), Value(GuidesPtr(tieGuides)), Value()};
        if (c.input == 4) inputs = {Value(planeStrands), Value(GuidesPtr(crowdedGuides)), Value(plane)};
        if (c.input == 5) inputs = {Value(finStrands), Value(GuidesPtr(finGuides)), Value(fin)};
        out.emplace_back(c.name, valueAs<StrandsPtr>(op->evaluate(ctx, inputs)));
    }
    return out;
}

// Bit for bit (memcmp: -0 and NaN as themselves, which comparing floats would not tell): the
// strands, positions, widths and every channel.
void expectSameBits(const StrandSet& a, const StrandSet& b, const std::string& what) {
    auto same = [&](const auto& x, const auto& y, const std::string& part) {
        ASSERT_EQ(x.size(), y.size()) << what << ": " << part;
        if (!x.empty()) EXPECT_EQ(std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])), 0) << what << ": " << part;
    };
    same(a.ids(), b.ids(), "ids");
    same(a.offsets(), b.offsets(), "offsets");
    same(a.positions(), b.positions(), "positions");
    same(a.widths(), b.widths(), "widths");
    ASSERT_EQ(a.strandChannelNames(), b.strandChannelNames()) << what;
    for (const std::string& name : a.strandChannelNames()) same(*a.strandChannel(name), *b.strandChannel(name), name);
    ASSERT_EQ(a.pointChannelNames(), b.pointChannelNames()) << what;
    for (const std::string& name : a.pointChannelNames()) same(*a.pointChannel(name), *b.pointChannel(name), name);
}

TEST(Interpolate, FastPathsGiveWhatTheFirstWrittenOnesGive) {
    // Every case above with its strands bound from cells and blended as rows (SIMD lanes), not
    // copied first, against the binding from the kd-tree for each strand, the blend one point at
    // a time and the points copied first (setInterpolateReferencePaths): the same, bit for bit.
    setInterpolateReferencePaths(true);
    const auto reference = interpolateCases();
    setInterpolateReferencePaths(false);
    const auto fast = interpolateCases();
    ASSERT_EQ(fast.size(), reference.size());
    for (std::size_t i = 0; i < fast.size(); ++i) {
        ASSERT_TRUE(fast[i].second && reference[i].second) << fast[i].first;
        expectSameBits(*fast[i].second, *reference[i].second, fast[i].first);
    }
}

TEST(Interpolate, FastPathsHoldOverManyStrandsAndMovedGuideRoots) {
    // 40k strands on a sphere from 300 guides (many strands to a cell), bound and blended; then a
    // few guide roots moved (the strands they reach bound again, from cells over just those
    // strands): each as the first written paths make it, bit for bit.
    const auto sphere = test::makeSphere(40, 80, 1.0f);
    Rng rng(23);
    auto onSphere = [&] { return Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized(); };
    auto guides = std::make_shared<GuideSet>();
    for (std::uint64_t g = 0; g < 300; ++g) {
        const Vec3f n = onSphere();
        guides->add(g + 1, test::straightCurve(n, n * 0.5f + Vec3f(0.1f, -0.2f, 0.0f), 4));
    }
    guides->rootOnMesh(*sphere);
    auto moved = std::make_shared<GuideSet>(*guides);
    for (std::size_t g = 0; g < 300; g += 60) {
        for (Vec3f& p : moved->guides()[g].points) p += Vec3f(0.03f, 0.0f, -0.02f);
    }
    moved->rootOnMesh(*sphere);
    std::vector<Vec3f> roots(40000);
    for (Vec3f& r : roots) r = onSphere();
    const StrandsPtr strands = strandsAt(sphere.get(), roots, {0.0f, 1.0f, 0.0f});
    auto run = [&](bool reference) {
        setInterpolateReferencePaths(reference);
        auto op = OperatorRegistry::instance().create(op::kInterpolate);
        ExternalInputs ext;
        EvalContext ctx;
        ctx.externals = &ext;
        std::vector<StrandsPtr> out;
        for (const auto& g : {guides, moved}) {
            out.push_back(valueAs<StrandsPtr>(op->evaluate(ctx, {Value(strands), Value(GuidesPtr(g)), Value(MeshPtr(sphere))})));
        }
        setInterpolateReferencePaths(false);
        return out;
    };
    const std::size_t searched = interpolateStrandBindCount();
    const std::vector<StrandsPtr> reference = run(true);
    const std::size_t referenceSearched = interpolateStrandBindCount() - searched;
    const std::vector<StrandsPtr> fast = run(false);
    EXPECT_EQ(interpolateStrandBindCount() - searched - referenceSearched, referenceSearched)
        << "the strands bound again after the roots moved differ";
    EXPECT_LT(referenceSearched, 2 * strands->strandCount()) << "the moved roots bound every strand again";
    ASSERT_EQ(fast.size(), reference.size());
    for (std::size_t i = 0; i < fast.size(); ++i) expectSameBits(*fast[i], *reference[i], i == 0 ? "bound" : "roots moved");
}

}  // namespace
}  // namespace groomlab
