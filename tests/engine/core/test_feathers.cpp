// SPDX-License-Identifier: Apache-2.0
// Feathers: Propagate (barbs grown off shafts) and Split (strands routed into shares), and the
// look nodes after Propagate.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

// A 2 x 2 plane scalp (normal +Y) with UVs, and one shaft lying on it along +X from (0.5, 0, 1):
// its vane should lie in the plane, the right side toward +Z.
struct OneShaft {
    MeshPtr scalp;
    StrandsPtr shaft;
    ExternalInputs ext;

    OneShaft() {
        auto plane = std::make_shared<ScalpMesh>(*test::makePlane(8, 2.0f));
        std::vector<ScalpMesh::UV> uvs;
        for (const ScalpMesh::Triangle& t : plane->triangles()) {
            for (const std::int32_t v : t) {
                const Vec3f& p = plane->points()[static_cast<std::size_t>(v)];
                uvs.push_back({p.x * 0.5f, p.z * 0.5f});
            }
        }
        plane->setCornerUVs(std::move(uvs));
        scalp = plane;
        auto s = std::make_shared<StrandSet>();
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 10; ++i) pts.push_back({0.5f + 0.1f * static_cast<float>(i), 0.0f, 1.0f});
        s->addStrand(77, pts, std::vector<float>(pts.size(), 0.02f), scalp->closestPoint(pts[0]).root);
        s->addStrandChannel("colorR", 0.5f)[0] = 0.25f;
        s->addStrandChannel("clumpId", -1.0f)[0] = 3.0f;  // replaced: a barb's clump is its vane
        std::vector<float>& tint = s->addPointChannel("tint", 0.0f);
        for (std::size_t i = 0; i < tint.size(); ++i) tint[i] = static_cast<float>(i) / 10.0f;
        shaft = s;
        ext.set("scalp", scalp, 1);
    }

    std::unique_ptr<Operator> propagate() const {
        auto op = OperatorRegistry::instance().create(op::kPropagate);
        op->params().set("count", std::int64_t{10});
        op->params().set("lengthRandom", 0.0);
        op->params().set("angleRandom", 0.0);
        op->params().set("curl", 0.0);
        op->params().set("droop", 0.0);
        return op;
    }
    StrandsPtr run(const Operator& op, const Value& input) const {
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(op.evaluate(ctx, {input}));
    }
    StrandsPtr run(const Operator& op) const { return run(op, shaft); }
};

float channel(const StrandSet& s, const char* name, std::size_t i) { return (*s.strandChannel(name))[i]; }

bool allFinite(const StrandSet& s) {
    for (const Vec3f& p : s.positions()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    for (const float w : s.widths()) {
        if (!std::isfinite(w)) return false;
    }
    return true;
}

TEST(Propagate, BarbsFollowTheProfilesInTheVanePlane) {
    OneShaft f;
    auto op = f.propagate();
    const Ramp outline{{0.0f, 0.5f}, {0.5f, 1.0f}, {1.0f, 0.2f}};
    op->params().set("lengthProfile", outline);
    op->params().set("length", 0.4);
    op->params().set("angle", 60.0);
    op->params().set("start", 0.2);
    op->params().set("end", 0.9);
    const StrandsPtr out = f.run(*op);
    std::string error;
    ASSERT_TRUE(out->validate(&error)) << error;
    ASSERT_EQ(out->strandCount(), 1u + 20u);  // the shaft, then 10 barbs a side
    EXPECT_EQ(out->ids()[0], 77u);
    EXPECT_EQ(channel(*out, "isShaft", 0), 1.0f);
    EXPECT_EQ(channel(*out, "barbIndex", 0), -1.0f);
    EXPECT_FLOAT_EQ(channel(*out, "colorR", 0), 0.25f);

    int left = 0, right = 0;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const Vec3f* pts = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        ASSERT_EQ(n, 7u);  // 6 segments
        const float u = channel(*out, "shaftU", s);
        const float side = channel(*out, "side", s);
        EXPECT_GT(u, 0.2f);
        EXPECT_LT(u, 0.9f);
        EXPECT_EQ(channel(*out, "featherId", s), channel(*out, "featherId", 0));  // the shaft's feather
        EXPECT_EQ(channel(*out, "isShaft", s), 0.0f);
        EXPECT_FLOAT_EQ(channel(*out, "colorR", s), 0.25f);  // the shaft's channels come along
        EXPECT_NE(channel(*out, "clumpId", s), 3.0f);        // but a barb's clump is its vane
        EXPECT_NEAR((*out->pointChannel("tint"))[out->strandBegin(s)], u, 1e-4f);  // the shaft's tint where it grows
        EXPECT_FLOAT_EQ(out->widths()[out->strandBegin(s)], 0.02f);  // the shaft's width there
        EXPECT_EQ(out->roots()[s].face, f.shaft->roots()[0].face);   // the shaft's root: maps read the feather's base
        // Rooted on the shaft where shaftU says, lying in the scalp's plane, on its side.
        EXPECT_NEAR(pts[0].x, 0.5f + u, 1e-4f);
        EXPECT_NEAR(pts[0].z, 1.0f, 1e-5f);
        for (std::size_t i = 0; i < n; ++i) EXPECT_NEAR(pts[i].y, 0.0f, 1e-5f);
        EXPECT_GT((pts[n - 1].z - 1.0f) * side, 0.0f);
        (side > 0 ? right : left) += 1;
        // Length from the profile, angle from the shaft (+X) as set.
        EXPECT_NEAR(arcLength(pts, n), 0.4f * outline(u), 1e-4f);
        const Vec3f d = (pts[1] - pts[0]).normalized();
        EXPECT_NEAR(std::acos(std::clamp(d.x, -1.0f, 1.0f)), 60.0f * kPi / 180.0f, 1e-3f);
    }
    EXPECT_EQ(left, 10);
    EXPECT_EQ(right, 10);
}

TEST(Propagate, MapsAtTheRootReadTheFeathersBase) {
    // A color painted on the scalp reaches every barb as its shaft's: HairColor samples the root,
    // and the barbs carry the shaft's.
    OneShaft f;
    auto painted = std::make_shared<ScalpMesh>(*f.scalp);
    std::vector<ScalpMesh::Color> colors;
    for (const ScalpMesh::Triangle& t : painted->triangles()) {
        for (const std::int32_t v : t) {
            const float x = painted->points()[static_cast<std::size_t>(v)].x * 0.5f;
            colors.push_back({x, 1.0f - x, 0.2f, 1.0f});
        }
    }
    painted->setCornerColors("paint", std::move(colors));
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(painted), 2);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr barbs = valueAs<StrandsPtr>(f.propagate()->evaluate(ctx, {Value(f.shaft)}));
    auto color = OperatorRegistry::instance().create(op::kHairColor);
    color->applyDefaults();
    color->params().set("mode", std::string("color"));
    color->params().set("map", std::string("paint"));
    const StrandsPtr out = valueAs<StrandsPtr>(color->evaluate(ctx, {Value(barbs)}));
    ASSERT_EQ(out->strandCount(), 21u);
    const float shaftRed = channel(*out, "colorR", 0);
    for (std::size_t s = 1; s < out->strandCount(); ++s) EXPECT_FLOAT_EQ(channel(*out, "colorR", s), shaftRed);
    // The barbs reach well past where the color changes: it is the base's, not theirs.
    EXPECT_GT(out->strandPoints(20)[0].x - out->strandPoints(0)[0].x, 0.5f);
}

TEST(Propagate, IdsAreUniqueAndDeterministic) {
    // Many shafts from a scatter: every barb ID differs from every other and from the shafts'.
    const GraphDocument doc = loadGraphJson(R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "scatter", "type": "Scatter", "params": {"density": 60.0}},
      {"name": "comb", "type": "Bend", "params": {"angle": 90.0, "along": "direction", "direction": [1, 0, 0]}},
      {"name": "feathers", "type": "Propagate", "params": {"count": 30, "sides": "both"}},
      {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "comb", "input": "strands"}, {"from": "comb", "to": "feathers", "input": "shafts"},
        {"from": "feathers", "to": "out", "input": "strands"}], "output": "out"})");
    ExternalInputs ext;
    setGroomExternals(ext, test::makePlane(8, 2.0f), nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    GraphDocument a = loadGraphJson(saveGraphJson(doc));
    GraphDocument b = loadGraphJson(saveGraphJson(doc));
    const StrandsPtr x = a.graph.evaluateStrands(a.output, ctx);
    const StrandsPtr y = b.graph.evaluateStrands(b.output, ctx);
    ASSERT_GT(x->strandCount(), 1000u);
    EXPECT_EQ(x->ids(), y->ids());
    EXPECT_EQ(x->positions(), y->positions());
    const std::set<StrandId> unique(x->ids().begin(), x->ids().end());
    EXPECT_EQ(unique.size(), x->strandCount());
    // Shafts number the feathers 0, 1, 2... in input order; each feather has its 60 barbs.
    std::map<int, int> barbs;
    for (std::size_t s = 0; s < x->strandCount(); ++s) {
        if (channel(*x, "isShaft", s) == 0.0f) ++barbs[static_cast<int>(channel(*x, "featherId", s))];
    }
    for (const auto& [feather, count] : barbs) EXPECT_EQ(count, 60) << feather;

#if GROOMLAB_HAS_TBB
    StrandsPtr serial;
    tbb::task_arena single(1);
    GraphDocument c = loadGraphJson(saveGraphJson(doc));
    single.execute([&] { serial = c.graph.evaluateStrands(c.output, ctx); });
    EXPECT_EQ(serial->ids(), x->ids());
    EXPECT_EQ(serial->positions(), x->positions()) << "thread count changes the barbs";
    EXPECT_EQ(serial->widths(), x->widths());
#endif
}

TEST(Propagate, LiftDroopCurlAndSides) {
    OneShaft f;
    {
        auto op = f.propagate();
        op->params().set("droop", 40.0);
        const StrandsPtr out = f.run(*op);
        for (std::size_t s = 1; s < out->strandCount(); ++s) {
            const Vec3f* pts = out->strandPoints(s);
            EXPECT_LT(pts[out->strandPointCount(s) - 1].y, -1e-3f);  // bent down toward the body: a cupped vane
        }
    }
    {
        auto op = f.propagate();
        op->params().set("lift", 20.0);
        const StrandsPtr out = f.run(*op);
        for (std::size_t s = 1; s < out->strandCount(); ++s) {
            const Vec3f* pts = out->strandPoints(s);
            EXPECT_GT(pts[1].y, 1e-4f);  // raised off the body at the shaft
        }
    }
    {
        // Curl turns each barb toward the tip (+X) along its length.
        auto op = f.propagate();
        op->params().set("curl", 60.0);
        op->params().set("angle", 80.0);
        const StrandsPtr out = f.run(*op);
        for (std::size_t s = 1; s < out->strandCount(); ++s) {
            const Vec3f* pts = out->strandPoints(s);
            const std::size_t n = out->strandPointCount(s);
            const Vec3f first = (pts[1] - pts[0]).normalized(), last = (pts[n - 1] - pts[n - 2]).normalized();
            EXPECT_GT(last.x, first.x + 0.3f);
        }
    }
    {
        auto op = f.propagate();
        op->params().set("sides", std::string("right"));
        op->params().set("keepShaft", false);
        const StrandsPtr out = f.run(*op);
        ASSERT_EQ(out->strandCount(), 10u);
        for (std::size_t s = 0; s < out->strandCount(); ++s) EXPECT_EQ(channel(*out, "side", s), 1.0f);
    }
    {
        // Around: twice the count, spiralling round the shaft (above and below the vane plane).
        auto op = f.propagate();
        op->params().set("sides", std::string("around"));
        const StrandsPtr out = f.run(*op);
        ASSERT_EQ(out->strandCount(), 1u + 20u);
        int above = 0, below = 0;
        for (std::size_t s = 1; s < out->strandCount(); ++s) {
            EXPECT_EQ(channel(*out, "side", s), 0.0f);
            const float y = out->strandPoints(s)[out->strandPointCount(s) - 1].y;
            above += y > 1e-3f ? 1 : 0;
            below += y < -1e-3f ? 1 : 0;
        }
        EXPECT_GT(above, 3);
        EXPECT_GT(below, 3);
    }
    {
        // Asymmetry: the left vane shorter than the right.
        auto op = f.propagate();
        op->params().set("asymmetry", 0.5);
        const StrandsPtr out = f.run(*op);
        float leftLength = 0.0f, rightLength = 0.0f;
        for (std::size_t s = 1; s < out->strandCount(); ++s) {
            const float l = arcLength(out->strandPoints(s), out->strandPointCount(s));
            (channel(*out, "side", s) < 0 ? leftLength : rightLength) += l;
        }
        EXPECT_NEAR(leftLength * 3.0f, rightLength, 1e-3f * rightLength);
    }
}

TEST(Propagate, SplitsGatherNeighbouringBarbs) {
    OneShaft f;
    auto plain = f.propagate();
    plain->params().set("count", std::int64_t{40});
    auto split = f.propagate();
    split->params().set("count", std::int64_t{40});
    split->params().set("splits", 6.0);
    split->params().set("splitAmount", 0.8);
    const StrandsPtr a = f.run(*plain);
    const StrandsPtr b = f.run(*split);
    ASSERT_EQ(a->strandCount(), b->strandCount());
    EXPECT_EQ(a->ids(), b->ids());
    std::set<float> vanes, groups;
    for (std::size_t s = 1; s < a->strandCount(); ++s) {
        vanes.insert(channel(*a, "clumpId", s));
        groups.insert(channel(*b, "clumpId", s));
    }
    EXPECT_EQ(vanes.size(), 2u);  // without splits: one clump per vane
    EXPECT_GE(groups.size(), 6u);
    EXPECT_LE(groups.size(), 24u);
    // Split members' tips sit closer together than their unsplit neighbours'.
    auto tipSpread = [](const StrandSet& s) {
        float total = 0.0f;
        for (std::size_t i = 2; i < s.strandCount(); ++i) {
            if (channel(s, "side", i) != channel(s, "side", i - 1)) continue;
            total += (s.strandPoints(i)[s.strandPointCount(i) - 1] - s.strandPoints(i - 1)[s.strandPointCount(i - 1) - 1]).length();
        }
        return total;
    };
    EXPECT_LT(tipSpread(*b), tipSpread(*a));
    EXPECT_TRUE(allFinite(*b));
}

TEST(Propagate, WorksFromGuidesAndFreeCurves) {
    // Hand-placed guides as shafts, through a Guides node: rooted, their maps come along.
    auto scalp = test::makePlane(8, 2.0f);
    auto guides = std::make_shared<GuideSet>();
    Guide& g = guides->add(5, test::straightCurve({1.0f, 0.0f, 1.0f}, {0.0f, 0.2f, 0.8f}, 8));
    g.attributes["featherColor"] = 0.7f;
    guides->add(6, test::straightCurve({0.4f, 0.0f, 0.4f}, {0.6f, 0.3f, 0.0f}, 8));
    guides->rootOnMesh(*scalp);
    ExternalInputs ext;
    setGroomExternals(ext, scalp, guides, nullptr);
    GraphDocument doc = loadGraphJson(R"({"version": 1, "nodes": [
      {"name": "guides", "type": "Guides"},
      {"name": "feathers", "type": "Propagate", "params": {"count": 12}},
      {"name": "out", "type": "Output"}],
      "connections": [{"from": "guides", "to": "feathers", "input": "shafts"},
        {"from": "feathers", "to": "out", "input": "strands"}], "output": "out"})");
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->strandCount(), 2u * (1u + 24u));
    EXPECT_FLOAT_EQ(channel(*out, "featherColor", 1), 0.7f);
    EXPECT_TRUE(allFinite(*out));
    EXPECT_EQ(doc.graph.outputType(doc.node("feathers")), PortType::Strands);

    // A curve rooted nowhere, with no scalp at all: the vane faces away from the way it bends.
    OneShaft f;
    auto free = std::make_shared<StrandSet>();
    std::vector<Vec3f> arc;
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) / 10.0f;
        arc.push_back({t, 1.0f - 0.3f * t * t, 0.0f});  // drooping down along +X
    }
    free->addStrand(9, arc);
    auto op = f.propagate();
    EvalContext bare;
    const StrandsPtr barbs = valueAs<StrandsPtr>(op->evaluate(bare, {Value(StrandsPtr(free))}));
    ASSERT_EQ(barbs->strandCount(), 21u);
    EXPECT_TRUE(allFinite(*barbs));
    for (std::size_t s = 1; s < barbs->strandCount(); ++s) {
        const Vec3f* pts = barbs->strandPoints(s);
        EXPECT_NEAR(std::abs(pts[barbs->strandPointCount(s) - 1].z), arcLength(pts, barbs->strandPointCount(s)) *
                                                                         std::sin(50.0f * kPi / 180.0f), 0.02f);
    }
}

TEST(Propagate, PreviewBudgetThinsBarbsByTheirIds) {
    OneShaft f;
    auto op = f.propagate();
    op->params().set("count", std::int64_t{200});
    const StrandsPtr full = f.run(*op);
    EvalContext ctx;
    ctx.externals = &f.ext;
    ctx.maxPoints = 700.0;  // about a quarter of the 2811 points
    const StrandsPtr thin = valueAs<StrandsPtr>(op->evaluate(ctx, {Value(f.shaft)}));
    EXPECT_LT(thin->pointCount(), 1000u);
    EXPECT_GT(thin->pointCount(), 300u);
    const std::set<StrandId> all(full->ids().begin(), full->ids().end());
    for (const StrandId id : thin->ids()) EXPECT_TRUE(all.count(id));
}

// Shafts from a scatter laid along +X, feathered, then every common node after Propagate.
struct FeatherGraph {
    ExternalInputs ext;
    std::string head = R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "segments", "type": "Resample", "params": {"segments": 8}},
      {"name": "scatter", "type": "Scatter", "params": {"density": 12.0, "length": 0.8}},
      {"name": "comb", "type": "Bend", "params": {"angle": 180.0, "end": 0.0, "along": "direction", "direction": [1, 0, 0]}},
      {"name": "feathers", "type": "Propagate", "params": {"count": 25, "length": 0.25}})";
    std::string links = R"({"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "comb", "input": "strands"}, {"from": "comb", "to": "feathers", "input": "shafts"})";

    FeatherGraph() { setGroomExternals(ext, test::makePlane(8, 2.0f), nullptr, nullptr); }
    StrandsPtr run(const std::string& nodes, const std::string& connections, const char* output, bool cards = false) {
        const std::string text = head + (nodes.empty() ? "" : ",\n" + nodes) + R"(], "connections": [)" + links +
                                 (connections.empty() ? "" : ",\n" + connections) + R"(], "output": ")" + output + "\"}";
        GraphDocument doc = loadGraphJson(text);
        EvalContext ctx;
        ctx.externals = &ext;
        ctx.buildCards = cards;
        return doc.graph.evaluateStrands(doc.node(output), ctx);
    }
};

std::string link(const char* from, const char* to, const char* input) {
    return std::string(R"({"from": ")") + from + R"(", "to": ")" + to + R"(", "input": ")" + input + R"("})";
}

TEST(Propagate, EveryCommonNodeWorksOnBarbs) {
    FeatherGraph g;
    const StrandsPtr barbs = g.run("", "", "feathers");
    ASSERT_GT(barbs->strandCount(), 200u);
    const std::size_t count = barbs->strandCount();
    struct Node {
        const char* type;
        const char* params;
    };
    const Node sameCount[] = {
        {"Noise", R"({"amplitude": 0.02, "frequency": 6.0})"},
        {"Curl", R"({"radius": 0.01})"},
        {"Bend", R"({"angle": 20.0, "along": "direction", "direction": [0, 0, 1]})"},
        {"Length", R"({"keep": 0.7, "randomness": 0.2})"},
        {"Width", R"({"width": 0.004, "profile": [[0, 1], [1, 0.2]]})"},
        {"HairColor", R"({"melanin": 0.3, "melaninRandom": 0.2, "clumpShare": 0.8})"},
        {"Bend", R"({"angle": 15.0, "end": 0.0})"},
        {"Smooth", R"({"strength": 0.5})"},
        {"Resample", R"({"segments": 4})"},
        {"Flyaway", R"({"fraction": 0.1})"},
    };
    for (const Node& n : sameCount) {
        const std::string node = std::string(R"({"name": "op", "type": ")") + n.type + R"(", "params": )" + n.params + "}";
        const StrandsPtr out = g.run(node, link("feathers", "op", "strands"), "op");
        std::string error;
        ASSERT_TRUE(out->validate(&error)) << n.type << ": " << error;
        EXPECT_EQ(out->strandCount(), count) << n.type;
        EXPECT_TRUE(allFinite(*out)) << n.type;
        EXPECT_EQ(out->ids(), barbs->ids()) << n.type;
    }
    // Cull thins, Merge adds, Collide keeps them out of the scalp.
    const StrandsPtr culled = g.run(R"({"name": "op", "type": "Cull", "params": {"keep": 0.5}})",
                                    link("feathers", "op", "strands"), "op");
    EXPECT_LT(culled->strandCount(), count * 3 / 4);
    EXPECT_GT(culled->strandCount(), count / 4);
    const StrandsPtr merged = g.run(R"({"name": "more", "type": "Scatter", "params": {"density": 50.0, "seed": 4}},
                                       {"name": "op", "type": "Merge"})",
                                    link("scalp", "more", "scalp") + "," + link("more", "op", "a") + "," +
                                        link("feathers", "op", "b"),
                                    "op");
    EXPECT_GT(merged->strandCount(), count);
    const std::set<StrandId> unique(merged->ids().begin(), merged->ids().end());
    EXPECT_EQ(unique.size(), merged->strandCount());
    const StrandsPtr collided = g.run(R"({"name": "op", "type": "Collide", "params": {"offset": 0.01}})",
                                      link("feathers", "op", "strands") + "," + link("scalp", "op", "scalp"), "op");
    EXPECT_EQ(collided->strandCount(), count);
    EXPECT_TRUE(allFinite(*collided));
    // Cards: a card per vane (clumpId), built from the barbs.
    const StrandsPtr carded = g.run(R"({"name": "op", "type": "Cards", "params": {"segments": 4}})",
                                    link("feathers", "op", "strands"), "op", true);
    ASSERT_EQ(carded->cardLayers().size(), 1u);
    const CardMesh& cards = *carded->cardLayers().front();
    EXPECT_GT(cards.cards.size(), 10u);
    for (const Vec3f& p : cards.positions) ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}

TEST(Propagate, ClumpGathersBarbsWithinTheirOwnVane) {
    FeatherGraph g;
    const StrandsPtr out = g.run(R"({"name": "op", "type": "Clump", "params": {"count": 60, "strength": 1.0, "profile": [[0, 0], [1, 1]]}})",
                                 link("feathers", "op", "strands"), "op");
    const StrandsPtr before = g.run("", "", "feathers");
    ASSERT_EQ(out->strandCount(), before->strandCount());
    EXPECT_TRUE(allFinite(*out));
    // Every barb keeps to its side of its shaft (clumping across the shaft would flip some), and
    // every barb is in a clump; shafts are not.
    std::set<float> clumps;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const float clump = channel(*out, "clumpId", s);
        if (channel(*out, "isShaft", s) > 0.5f) {
            EXPECT_EQ(clump, -1.0f);
            EXPECT_EQ(out->strandPoints(s)[1], before->strandPoints(s)[1]);  // untouched
            continue;
        }
        EXPECT_GE(clump, 0.0f);
        clumps.insert(clump);
        const std::size_t n = out->strandPointCount(s);
        const Vec3f tipBefore = before->strandPoints(s)[n - 1] - before->strandPoints(s)[0];
        const Vec3f tipAfter = out->strandPoints(s)[n - 1] - out->strandPoints(s)[0];
        EXPECT_GT(tipBefore.z * tipAfter.z, 0.0f) << "barb " << s << " crossed its shaft";
    }
    EXPECT_GE(clumps.size(), 40u);  // about 60 spread over the vanes, at least one each
}

TEST(Propagate, ClumpBySizeSpacesCentersAlongEachVane) {
    // clumpBy size on barbs: as many centers on each vane as fit `size` apart along it, so half
    // the size gives about twice the centers; the count is not read.
    FeatherGraph g;
    auto centers = [&](double size) {
        const std::string node = R"({"name": "op", "type": "Clump", "params": {"strength": 1.0, "clumpBy": "size", "count": 1, "size": )" +
                                 std::to_string(size) + "}}";
        const StrandsPtr out = g.run(node, link("feathers", "op", "strands"), "op");
        std::set<float> clumps;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            if (channel(*out, "clumpId", s) >= 0.0f) clumps.insert(channel(*out, "clumpId", s));
        }
        return clumps.size();
    };
    const std::size_t coarse = centers(0.2), fine = centers(0.1);
    EXPECT_GT(coarse, 4u);
    EXPECT_GT(static_cast<double>(fine), 1.5 * static_cast<double>(coarse));
}

// ---------------------------------------------------------------------------

TEST(Split, WeightsPartitionEveryStrandOnceAndStably) {
    FeatherGraph g;
    const std::string node = R"({"name": "split", "type": "Split", "params": {"weightA": 3.0, "weightB": 1.0, "weightC": 0.0, "weightD": 1.0, "seed": 2}})";
    GraphDocument doc = loadGraphJson(R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
      )" + node + R"(], "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
      {"from": "scatter", "to": "split", "input": "strands"}], "output": "split"})");
    EvalContext ctx;
    ctx.externals = &g.ext;
    const NodeId split = doc.node("split");
    const StrandsPtr all = doc.graph.evaluateStrands(doc.node("scatter"), ctx);
    const std::vector<Value> outs = doc.graph.evaluateOutputs(split, ctx);
    ASSERT_EQ(outs.size(), 4u);
    std::set<StrandId> seen;
    std::size_t sizes[4] = {};
    for (std::size_t k = 0; k < 4; ++k) {
        const StrandsPtr s = valueAs<StrandsPtr>(outs[k]);
        ASSERT_TRUE(s) << k;
        sizes[k] = s->strandCount();
        for (const StrandId id : s->ids()) EXPECT_TRUE(seen.insert(id).second) << "a strand went to two outputs";
    }
    EXPECT_EQ(seen.size(), all->strandCount());  // and every strand went somewhere
    EXPECT_EQ(sizes[2], 0u);
    const double total = static_cast<double>(all->strandCount());
    EXPECT_NEAR(sizes[0] / total, 0.6, 0.05);
    EXPECT_NEAR(sizes[1] / total, 0.2, 0.05);
    EXPECT_NEAR(sizes[3] / total, 0.2, 0.05);
    EXPECT_EQ(doc.graph.outputIndex(split, "d"), 3u);
    // The same strands every time, and a strand's output depends on it alone: fewer strands
    // (a thinner preview) keep their outputs.
    GraphDocument again = loadGraphJson(saveGraphJson(doc));
    EvalContext thin = ctx;
    thin.previewDensity = 0.5f;
    const StrandsPtr b = valueAs<StrandsPtr>(again.graph.evaluateOutput(again.node("split"), 1, thin));
    const StrandsPtr fullB = valueAs<StrandsPtr>(outs[1]);
    const std::set<StrandId> full(fullB->ids().begin(), fullB->ids().end());
    for (const StrandId id : b->ids()) EXPECT_TRUE(full.count(id));
}

TEST(Split, MapModeAndGuides) {
    // By a map: values under the first share go to a.
    FeatherGraph g;
    GraphDocument doc = loadGraphJson(R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
      {"name": "split", "type": "Split", "params": {"mode": "map", "select": {"expression": "u"}}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
      {"from": "scatter", "to": "split", "input": "strands"}], "output": "split"})");
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(8, 2.0f));
    std::vector<ScalpMesh::UV> uvs;
    for (const ScalpMesh::Triangle& t : plane->triangles()) {
        for (const std::int32_t v : t) uvs.push_back({plane->points()[static_cast<std::size_t>(v)].x * 0.5f, 0.0f});
    }
    plane->setCornerUVs(std::move(uvs));
    ExternalInputs ext;
    setGroomExternals(ext, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    const std::vector<Value> outs = doc.graph.evaluateOutputs(doc.node("split"), ctx);
    const StrandsPtr a = valueAs<StrandsPtr>(outs[0]), b = valueAs<StrandsPtr>(outs[1]);
    ASSERT_GT(a->strandCount(), 50u);
    ASSERT_GT(b->strandCount(), 50u);
    for (std::size_t s = 0; s < a->strandCount(); ++s) EXPECT_LT(a->strandPoints(s)[0].x, 1.0f + 1e-4f);
    for (std::size_t s = 0; s < b->strandCount(); ++s) EXPECT_GT(b->strandPoints(s)[0].x, 1.0f - 1e-4f);
    EXPECT_TRUE(valueAs<StrandsPtr>(outs[2])->empty());

    // Guides in, guides out: each output a share of the guides, still guides (Curves).
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < 40; ++i) {
        guides->add(100 + i, test::straightCurve({0.05f * static_cast<float>(i), 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}));
    }
    guides->rootOnMesh(*plane);
    ExternalInputs withGuides;
    setGroomExternals(withGuides, plane, guides, nullptr);
    GraphDocument gd = loadGraphJson(R"({"version": 1, "nodes": [
      {"name": "guides", "type": "Guides"},
      {"name": "split", "type": "Split"},
      {"name": "feathers", "type": "Propagate", "params": {"count": 5}}],
      "connections": [{"from": "guides", "to": "split", "input": "strands"},
        {"from": "split", "to": "feathers", "input": "shafts", "output": "b"}], "output": "feathers"})");
    EvalContext gctx;
    gctx.externals = &withGuides;
    EXPECT_EQ(gd.graph.outputType(gd.node("split"), 1), PortType::Guides);
    const std::vector<Value> gouts = gd.graph.evaluateOutputs(gd.node("split"), gctx);
    const GuidesPtr ga = valueAs<GuidesPtr>(gouts[0]), gb = valueAs<GuidesPtr>(gouts[1]);
    ASSERT_TRUE(ga && gb);
    EXPECT_EQ(ga->size() + gb->size(), 40u);
    const StrandsPtr feathers = gd.graph.evaluateStrands(gd.output, gctx);
    EXPECT_EQ(feathers->strandCount(), gb->size() * (1u + 10u));
}

}  // namespace
}  // namespace groomlab
