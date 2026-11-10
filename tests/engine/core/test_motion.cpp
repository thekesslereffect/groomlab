// SPDX-License-Identifier: Apache-2.0
// Motion without a cache: Wind (a wind moving through space and time) and Jiggle (lag and
// overshoot from the scalp's recent motion). Both must give their input back to the bit when
// there is nothing to do, and bypassing them must give the groom as it was.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "groomlab/scalp_history.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr double kFrame = 1.0 / 24.0;
constexpr double kTwoPiD = 6.283185307179586;

// Straight strands rooted on the plane (normal +Y), `points` points each, pointing along `dir`.
StrandsPtr strandsOn(const ScalpMesh& plane, Vec3f dir = {0, 1, 0}, int count = 40, int segments = 8) {
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < count; ++i) {
        const StrandRoot root{(3 * i) % static_cast<int>(plane.triangleCount()), 0.25f + 0.01f * static_cast<float>(i % 7),
                              0.3f};
        s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve(plane.position(root), dir, segments), {}, root);
    }
    return s;
}

std::unique_ptr<Operator> make(const char* type, const std::map<std::string, ParamValue>& params = {}) {
    auto op = OperatorRegistry::instance().create(type);
    for (const auto& [k, v] : params) op->params().set(k, v);
    return op;
}

float segmentLength(const StrandSet& s, std::size_t strand, std::size_t i) {
    return (s.strandPoints(strand)[i] - s.strandPoints(strand)[i - 1]).length();
}

void expectLengthsAndRootsKept(const StrandSet& in, const StrandSet& out) {
    ASSERT_EQ(in.strandCount(), out.strandCount());
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        EXPECT_EQ(out.strandPoints(s)[0], in.strandPoints(s)[0]) << "strand " << s << ": the root moved";
        for (std::size_t i = 1; i < in.strandPointCount(s); ++i) {
            EXPECT_NEAR(segmentLength(out, s, i), segmentLength(in, s, i), 1e-5f) << "strand " << s << " segment " << i;
        }
    }
}

// A plane scalp and its externals.
struct Scene {
    MeshPtr scalp;
    ExternalInputs ext;
    EvalContext ctx;
    explicit Scene(MeshPtr mesh = test::makePlane(4)) : scalp(std::move(mesh)) {
        ext.set("scalp", scalp, scalp->contentHash());
        ctx.externals = &ext;
    }
};

// ---------------------------------------------------------------------------
// Wind

TEST(Wind, StrengthZeroGivesTheInputBackToTheBit) {
    Scene scene;
    scene.ctx.time = 1.25;
    const StrandsPtr in = strandsOn(*scene.scalp);
    const auto wind = make(op::kWind, {{"strength", 0.0}, {"turbulence", 1.0}, {"gust", 1.0}});
    const StrandsPtr out = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    ASSERT_TRUE(out);
    EXPECT_EQ(out.get(), in.get()) << "no wind: the very same strands";
    EXPECT_EQ(out->positions(), in->positions());
}

TEST(Wind, BendsDownwindKeepingLengthsAndRoots) {
    Scene scene;
    scene.ctx.time = 0.5;
    const StrandsPtr in = strandsOn(*scene.scalp);
    const auto wind = make(op::kWind, {{"direction", Vec3f{1, 0, 0}}, {"strength", 0.6}});
    const StrandsPtr out = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    ASSERT_TRUE(out);
    expectLengthsAndRootsKept(*in, *out);
    float push = 0.0f;
    for (std::size_t s = 0; s < in.get()->strandCount(); ++s) {
        const std::size_t tip = in->strandPointCount(s) - 1;
        push += out->strandPoints(s)[tip].x - in->strandPoints(s)[tip].x;
    }
    EXPECT_GT(push / static_cast<float>(in->strandCount()), 0.05f) << "tips go with the wind (+x)";
}

TEST(Wind, VariesOverTimeAndIsTheSameEveryRun) {
    Scene scene;
    const StrandsPtr in = strandsOn(*scene.scalp);
    const auto wind = make(op::kWind, {{"strength", 0.5}, {"gust", 1.0}, {"frequency", 1.0}, {"turbulence", 0.5}});
    scene.ctx.time = 0.0;
    const StrandsPtr a = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    const StrandsPtr again = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    scene.ctx.time = 0.5;
    const StrandsPtr b = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    EXPECT_EQ(a->positions(), again->positions()) << "a pure function of the strands, time and params";
    EXPECT_NE(a->positions(), b->positions()) << "the wind moves with time";
    expectLengthsAndRootsKept(*in, *b);
}

TEST(Wind, ParallelMatchesSerial) {
#if GROOMLAB_HAS_TBB
    Scene scene;
    scene.ctx.time = 2.0;
    const StrandsPtr in = strandsOn(*scene.scalp, {0, 1, 0}, 600);
    const auto wind = make(op::kWind, {{"strength", 0.7}, {"turbulence", 0.8}});
    const StrandsPtr many = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    StrandsPtr one;
    tbb::task_arena single(1);
    single.execute([&] { one = valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in})); });
    EXPECT_EQ(one->positions(), many->positions());
#else
    GTEST_SKIP() << "built without TBB";
#endif
}

// scalp -> scatter -> [extra] -> width -> output: the graph with and without the node.
std::string chainGraph(const std::string& node) {
    std::string nodes = R"({"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
        {"name": "width", "type": "Width"}, {"name": "output", "type": "Output"})";
    std::string links = R"({"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "width", "to": "output", "input": "strands"})";
    if (node.empty()) {
        links += R"(, {"from": "scatter", "to": "width", "input": "strands"})";
    } else {
        nodes += ", " + node;
        links += R"(, {"from": "scatter", "to": "motion", "input": "strands"},
                      {"from": "motion", "to": "width", "input": "strands"})";
    }
    return R"({"version": 1, "nodes": [)" + nodes + R"(], "connections": [)" + links + R"(], "output": "output"})";
}

TEST(Wind, BypassedGivesTheGroomWithoutIt) {
    Scene scene(test::makeSphere(12, 24, 1.0f));
    scene.ctx.time = 0.75;
    GraphDocument plain = loadGraphJson(chainGraph(""));
    GraphDocument windy = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Wind", "params": {"strength": 0.8}})"));
    const StrandsPtr without = plain.graph.evaluateStrands(plain.output, scene.ctx);
    const StrandsPtr with = windy.graph.evaluateStrands(windy.output, scene.ctx);
    ASSERT_TRUE(without && with);
    EXPECT_NE(with->positions(), without->positions());
    windy.graph.setBypass(windy.node("motion"), true);
    const StrandsPtr bypassed = windy.graph.evaluateStrands(windy.output, scene.ctx);
    EXPECT_EQ(bypassed->ids(), without->ids());
    EXPECT_EQ(bypassed->positions(), without->positions());
    EXPECT_EQ(bypassed->widths(), without->widths());
    // Strength 0 is the same groom as no wind at all, too.
    windy.graph.setBypass(windy.node("motion"), false);
    windy.graph.node(windy.node("motion")).params().set("strength", 0.0);
    const StrandsPtr calm = windy.graph.evaluateStrands(windy.output, scene.ctx);
    EXPECT_EQ(calm->positions(), without->positions());
    EXPECT_EQ(calm->widths(), without->widths());
}

TEST(Wind, OnlyItAndWhatFollowsReEvaluateOnANewFrame) {
    Scene scene(test::makeSphere(12, 24, 1.0f));
    GraphDocument doc = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Wind", "params": {"strength": 0.5}})"));
    scene.ctx.time = 0.0;
    const StrandsPtr first = doc.graph.evaluateStrands(doc.output, scene.ctx);
    const std::size_t runs = doc.graph.evaluationCount();
    doc.graph.evaluateStrands(doc.output, scene.ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs) << "the same frame: all cached";
    scene.ctx.time = kFrame;
    const StrandsPtr second = doc.graph.evaluateStrands(doc.output, scene.ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs + 3) << "Wind, Width and Output run again; Scalp and Scatter stay cached";
    EXPECT_NE(first->positions(), second->positions());
    // No wind: nothing reads the time, so a new frame is all cache hits.
    doc.graph.node(doc.node("motion")).params().set("strength", 0.0);
    doc.graph.evaluateStrands(doc.output, scene.ctx);
    const std::size_t calm = doc.graph.evaluationCount();
    scene.ctx.time = 2 * kFrame;
    doc.graph.evaluateStrands(doc.output, scene.ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), calm);
    // Bypassed: the same, however strong the wind.
    doc.graph.node(doc.node("motion")).params().set("strength", 0.9);
    doc.graph.setBypass(doc.node("motion"), true);
    const StrandsPtr bypassed = doc.graph.evaluateStrands(doc.output, scene.ctx);
    const std::size_t still = doc.graph.evaluationCount();
    scene.ctx.time = 3 * kFrame;
    EXPECT_EQ(doc.graph.evaluateStrands(doc.output, scene.ctx).get(), bypassed.get());
    EXPECT_EQ(doc.graph.evaluationCount(), still) << "a bypassed Wind keeps what follows cached";
}

TEST(Wind, IsCheapEnoughForPlayback) {
    // 20k strands of 16 points, about a preview's worth: well under a frame's time.
    const MeshPtr sphere = test::makeSphere(24, 48, 1.0f);
    Scene scene(sphere);
    scene.ctx.time = 1.0;
    auto strands = std::make_shared<StrandSet>();
    for (int i = 0; i < 20000; ++i) {
        const StrandRoot root{i % static_cast<int>(sphere->triangleCount()), 0.3f, 0.3f};
        const Vec3f p = sphere->position(root);
        strands->addStrand(static_cast<StrandId>(i + 1), test::straightCurve(p, sphere->normal(root) * 0.5f, 15), {}, root);
    }
    const auto wind = make(op::kWind, {{"strength", 0.5}});
    wind->evaluate(scene.ctx, {StrandsPtr(strands)});  // warm up
    const auto start = std::chrono::steady_clock::now();
    wind->evaluate(scene.ctx, {StrandsPtr(strands)});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 250.0) << "Wind took " << ms << " ms for 320k points";
}

// ---------------------------------------------------------------------------
// Jiggle

// The plane moved by `offset(k)` k frames ago (k = 0: now), as a scene with its history.
struct Moving {
    MeshPtr rest = test::makePlane(4);
    Scene scene;
    ScalpHistoryPtr history;
    template <class Offset>
    Moving(int frames, Offset offset) : scene(shifted(offset(0))) {
        std::vector<std::vector<Vec3f>> past;
        for (int k = 1; k <= frames; ++k) past.push_back(shifted(offset(k))->points());
        history = ScalpHistory::make(kFrame, std::move(past));
        scene.ext.set("scalpHistory", history, history->hash);
    }
    MeshPtr shifted(Vec3f by) const {
        std::vector<Vec3f> pts = rest->points();
        for (Vec3f& p : pts) p += by;
        return std::make_shared<const ScalpMesh>(std::move(pts), rest->triangles());
    }
};

TEST(Jiggle, WithoutHistoryItPassesThroughAndSaysWhy) {
    Scene scene;
    const StrandsPtr in = strandsOn(*scene.scalp);
    const auto jiggle = make(op::kJiggle);
    EXPECT_EQ(valueAs<StrandsPtr>(jiggle->evaluate(scene.ctx, {in})).get(), in.get());
    EXPECT_NE(jiggle->note(scene.ctx).find("no scalp motion history"), std::string::npos);
}

TEST(Jiggle, AStillScalpGivesTheInputBackToTheBit) {
    Moving still(12, [](int) { return Vec3f{0.3f, 0.0f, -0.2f}; });
    const StrandsPtr in = strandsOn(*still.scene.scalp);
    const auto jiggle = make(op::kJiggle, {{"strength", 2.0}, {"frequency", 5.0}});
    const StrandsPtr out = valueAs<StrandsPtr>(jiggle->evaluate(still.scene.ctx, {in}));
    EXPECT_EQ(out.get(), in.get());
    EXPECT_TRUE(jiggle->note(still.scene.ctx).empty());
}

// Where the scalp is at frame f: still, then eased along +x over frames 0-8, then still again.
Vec3f path(int f) {
    const float t = std::clamp(static_cast<float>(f) / 8.0f, 0.0f, 1.0f);
    return {2.0f * t * t * (3.0f - 2.0f * t), 0.0f, 0.0f};
}

// The mean x offset of the tips from where the scalp carries them, at frame f.
float tipLag(int f, int history, double damping = 0.3) {
    Moving m(history, [f](int k) { return path(f - k); });
    const StrandsPtr in = strandsOn(*m.scene.scalp);
    const auto jiggle = make(op::kJiggle, {{"history", std::int64_t{history}}, {"damping", damping}});
    const StrandsPtr out = valueAs<StrandsPtr>(jiggle->evaluate(m.scene.ctx, {in}));
    expectLengthsAndRootsKept(*in, *out);
    float sum = 0.0f;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const std::size_t tip = in->strandPointCount(s) - 1;
        sum += out->strandPoints(s)[tip].x - in->strandPoints(s)[tip].x;
    }
    return sum / static_cast<float>(in->strandCount());
}

TEST(Jiggle, LagsBehindAMoveOvershootsAndSettles) {
    const int history = 24;
    EXPECT_LT(tipLag(4, history), -0.02f) << "mid-move, the tips trail (the scalp goes +x)";
    // After the stop the tips swing past (+x), then the swinging dies down.
    float firstSwing = 0.0f, lateSwing = 0.0f;
    for (int f = 9; f <= 16; ++f) firstSwing = std::max(firstSwing, std::abs(tipLag(f, history)));
    for (int f = 24; f <= 30; ++f) lateSwing = std::max(lateSwing, std::abs(tipLag(f, history)));
    float overshoot = 0.0f;
    for (int f = 9; f <= 16; ++f) overshoot = std::max(overshoot, tipLag(f, history));
    EXPECT_GT(overshoot, 0.01f) << "the tips swing past when the scalp stops";
    EXPECT_LT(lateSwing, 0.5f * firstSwing) << "damped: the swing dies down";
    // Once the move is older than the history, the scalp was still over it: the input as it was.
    Moving settled(history, [](int k) { return path(8 + history + 2 - k); });
    const StrandsPtr in = strandsOn(*settled.scene.scalp);
    const auto jiggle = make(op::kJiggle, {{"history", std::int64_t{history}}});
    EXPECT_EQ(valueAs<StrandsPtr>(jiggle->evaluate(settled.scene.ctx, {in})).get(), in.get());
}

TEST(Jiggle, MoreDampingSwingsLess) {
    float light = 0.0f, heavy = 0.0f;
    for (int f = 9; f <= 20; ++f) {
        light = std::max(light, tipLag(f, 24, 0.1));
        heavy = std::max(heavy, tipLag(f, 24, 1.0));
    }
    EXPECT_GT(light, heavy);
}

TEST(Jiggle, ATurningScalpDragsTheStrands) {
    // The plane turned about +y (through its corner) over the last frames: the strands lag the turn.
    const MeshPtr rest = test::makePlane(4);
    auto turned = [&](float angle) {
        std::vector<Vec3f> pts = rest->points();
        for (Vec3f& p : pts) p = rotateAxisAngle(p, {0, 0, 1}, angle);  // tilt about z
        return pts;
    };
    Scene scene;
    std::vector<std::vector<Vec3f>> past;
    for (int k = 1; k <= 12; ++k) past.push_back(turned(0.05f * static_cast<float>(std::min(k, 6))));
    const ScalpHistoryPtr history = ScalpHistory::make(kFrame, std::move(past));
    scene.ext.set("scalpHistory", history, history->hash);
    const StrandsPtr in = strandsOn(*scene.scalp);
    const StrandsPtr out = valueAs<StrandsPtr>(make(op::kJiggle)->evaluate(scene.ctx, {in}));
    ASSERT_NE(out.get(), in.get());
    expectLengthsAndRootsKept(*in, *out);
    EXPECT_NE(out->positions(), in->positions());
}

TEST(Jiggle, AmountZeroAndBypassGiveTheGroomAsItWas) {
    Moving m(12, [](int k) { return path(6 - k); });
    const StrandsPtr in = strandsOn(*m.scene.scalp);
    const StrandsPtr none = valueAs<StrandsPtr>(make(op::kJiggle, {{"strength", 0.0}})->evaluate(m.scene.ctx, {in}));
    EXPECT_EQ(none->positions(), in->positions());

    Scene sphere(test::makeSphere(12, 24, 1.0f));
    std::vector<std::vector<Vec3f>> past;
    for (int k = 1; k <= 12; ++k) {
        std::vector<Vec3f> pts = sphere.scalp->points();
        for (Vec3f& p : pts) p.x -= 0.05f * static_cast<float>(k);
        past.push_back(std::move(pts));
    }
    // A move that stops now (moving until a frame ago, then still) jolts the strands.
    past[0] = sphere.scalp->points();
    const ScalpHistoryPtr history = ScalpHistory::make(kFrame, std::move(past));
    sphere.ext.set("scalpHistory", history, history->hash);
    GraphDocument plain = loadGraphJson(chainGraph(""));
    GraphDocument jiggly = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Jiggle"})"));
    const StrandsPtr without = plain.graph.evaluateStrands(plain.output, sphere.ctx);
    const StrandsPtr with = jiggly.graph.evaluateStrands(jiggly.output, sphere.ctx);
    EXPECT_NE(with->positions(), without->positions());
    jiggly.graph.setBypass(jiggly.node("motion"), true);
    const StrandsPtr bypassed = jiggly.graph.evaluateStrands(jiggly.output, sphere.ctx);
    EXPECT_EQ(bypassed->ids(), without->ids());
    EXPECT_EQ(bypassed->positions(), without->positions());
    EXPECT_EQ(bypassed->widths(), without->widths());
}

TEST(Jiggle, TheHistoryIsPartOfItsCacheKey) {
    Scene scene(test::makeSphere(12, 24, 1.0f));
    GraphDocument doc = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Jiggle"})"));
    auto historyMovedBy = [&](float dx) {
        std::vector<std::vector<Vec3f>> past;
        for (int k = 1; k <= 12; ++k) {
            std::vector<Vec3f> pts = scene.scalp->points();
            for (Vec3f& p : pts) p.x -= dx * static_cast<float>(std::min(k, 3));
            past.push_back(std::move(pts));
        }
        return ScalpHistory::make(kFrame, std::move(past));
    };
    const ScalpHistoryPtr a = historyMovedBy(0.05f), sameAsA = historyMovedBy(0.05f), b = historyMovedBy(0.1f);
    EXPECT_EQ(a->hash, sameAsA->hash);
    EXPECT_NE(a->hash, b->hash);
    scene.ext.set("scalpHistory", a, a->hash);
    const StrandsPtr first = doc.graph.evaluateStrands(doc.output, scene.ctx);
    const std::size_t runs = doc.graph.evaluationCount();
    scene.ext.set("scalpHistory", sameAsA, sameAsA->hash);
    doc.graph.evaluateStrands(doc.output, scene.ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs) << "the same motion: cached";
    scene.ext.set("scalpHistory", b, b->hash);
    const StrandsPtr second = doc.graph.evaluateStrands(doc.output, scene.ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs + 3) << "Jiggle, Width and Output";
    EXPECT_NE(first->positions(), second->positions());
    // The graph's note: none with history, one without.
    EXPECT_TRUE(doc.graph.lastNotes().empty());
    ExternalInputs bare;
    bare.set("scalp", scene.scalp, scene.scalp->contentHash());
    EvalContext noHistory;
    noHistory.externals = &bare;
    doc.graph.evaluateStrands(doc.output, noHistory);
    ASSERT_EQ(doc.graph.lastNotes().count(doc.node("motion")), 1u);
}

TEST(Jiggle, ParallelMatchesSerial) {
#if GROOMLAB_HAS_TBB
    Moving m(12, [](int k) { return path(5 - k); });
    const StrandsPtr in = strandsOn(*m.scene.scalp, {0.2f, 1.0f, 0.1f}, 700);
    const auto jiggle = make(op::kJiggle, {{"strength", 1.5}});
    const StrandsPtr many = valueAs<StrandsPtr>(jiggle->evaluate(m.scene.ctx, {in}));
    StrandsPtr one;
    tbb::task_arena single(1);
    single.execute([&] { one = valueAs<StrandsPtr>(jiggle->evaluate(m.scene.ctx, {in})); });
    EXPECT_NE(many->positions(), in->positions());
    EXPECT_EQ(one->positions(), many->positions());
#else
    GTEST_SKIP() << "built without TBB";
#endif
}

TEST(Jiggle, HostsSampleHistoryOnlyForActiveJiggleNodes) {
    GraphDocument doc = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Jiggle", "params": {"history": 7}})"));
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output), 7);
    // Off where it evaluates by its evaluation mode: no history read there (an animator's
    // renderOnly Jiggle costs the viewport nothing; a viewportOnly one, a render).
    const NodeId jiggle = doc.node("motion");
    doc.graph.setEvaluationMode(jiggle, EvaluationMode::RenderOnly);
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output, 1), 0) << "the viewport";
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output, 0), 7) << "a render";
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output), 7) << "either";
    doc.graph.setEvaluationMode(jiggle, EvaluationMode::ViewportOnly);
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output, 1), 7);
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output, 0), 0);
    doc.graph.setEvaluationMode(jiggle, EvaluationMode::Always);
    doc.graph.setBypass(doc.node("motion"), true);
    EXPECT_EQ(scalpHistoryFrames(doc.graph, doc.output), 0);
    GraphDocument plain = loadGraphJson(chainGraph(""));
    EXPECT_EQ(scalpHistoryFrames(plain.graph, plain.output), 0);
    GraphDocument greedy = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Jiggle", "params": {"history": 1000}})"));
    EXPECT_EQ(scalpHistoryFrames(greedy.graph, greedy.output), kMaxScalpHistory);
}

TEST(Jiggle, GroomPassesItsHistoryToTheGraph) {
    const MeshPtr sphere = test::makeSphere(12, 24, 1.0f);
    GroomData data;
    data.scalp = sphere;
    data.graphJson = chainGraph(R"({"name": "motion", "type": "Jiggle"})");
    Groom groom(data);
    EXPECT_EQ(groom.scalpHistoryFrames(), 12);
    const StrandsPtr still = groom.evaluate(1.0f, 0.0);
    ASSERT_FALSE(groom.nodeNotes().empty());
    std::vector<std::vector<Vec3f>> past;
    for (int k = 1; k <= 12; ++k) {
        std::vector<Vec3f> pts = sphere->points();
        for (Vec3f& p : pts) p.z -= 0.04f * static_cast<float>(std::min(k, 4));
        past.push_back(std::move(pts));
    }
    groom.setScalpHistory(ScalpHistory::make(kFrame, std::move(past)));
    const StrandsPtr moving = groom.evaluate(1.0f, 0.0);
    EXPECT_TRUE(groom.nodeNotes().empty());
    EXPECT_NE(moving->positions(), still->positions());
}

TEST(ScalpHistory, TheSamplerReadsEachFrameOnceAndNoticesAnEditedAnimation) {
    // A scalp of one point at x = frame (frames 1 apart, keys in frames).
    double shift = 0.0;  // an edit of the animation
    std::size_t asked = 0;
    auto pointsAt = [&](std::int64_t frame) { return std::vector<Vec3f>{Vec3f(static_cast<float>(frame + shift), 0, 0)}; };
    ScalpHistorySampler sampler;
    auto at = [&](std::int64_t now, bool reread) {
        std::vector<std::int64_t> keys;
        for (std::int64_t k = 1; k <= 3; ++k) keys.push_back(now - k);
        return sampler.history(now, keys, 1.0 / 24.0, pointsAt(now), [&](std::size_t k) {
            ++asked;
            return pointsAt(keys[k]);
        }, reread);
    };
    const ScalpHistoryPtr first = at(10, false);
    ASSERT_TRUE(first);
    EXPECT_EQ(asked, 3u);
    ASSERT_EQ(first->frames.size(), 3u);
    EXPECT_EQ(first->frames[0][0].x, 9.0f);  // nearest first
    EXPECT_EQ(first->frames[2][0].x, 7.0f);
    EXPECT_EQ(at(10, false), first);  // the same request: the same history, nothing read
    EXPECT_EQ(asked, 3u);
    at(11, false);  // playing on: frame 10 was the current one, 9 and 8 are kept
    EXPECT_EQ(asked, 3u);
    EXPECT_EQ(sampler.reads(), 3u);
    // The animation edited: rereading notices it and drops what may be stale.
    shift = 0.5;
    const ScalpHistoryPtr edited = at(11, true);
    EXPECT_EQ(asked, 6u);
    EXPECT_EQ(edited->frames[0][0].x, 10.5f);
    sampler.clear();
    EXPECT_TRUE(sampler.empty());
    EXPECT_EQ(sampler.history(5, {}, 1.0 / 24.0, pointsAt(5), [&](std::size_t) { return pointsAt(0); }, false), nullptr);
}

// ---------------------------------------------------------------------------
// Looping

// The most any point moved from a to b.
float mostMoved(const StrandSet& a, const StrandSet& b) {
    float most = 0.0f;
    for (std::size_t i = 0; i < a.pointCount(); ++i) most = std::max(most, (a.positions()[i] - b.positions()[i]).length());
    return most;
}

TEST(Wind, ALoopComesBackSeamlesslyAndRepeats) {
    Scene scene;
    const StrandsPtr in = strandsOn(*scene.scalp);
    const std::map<std::string, ParamValue> windy = {{"strength", 0.6}, {"gust", 0.8},       {"frequency", 0.5},
                                                     {"speed", 12.0},   {"turbulence", 0.6}, {"direction", Vec3f{1, 0, 0.3f}}};
    auto looped = windy;
    looped["loop"] = true;
    looped["loopStart"] = std::int64_t{10};
    looped["loopEnd"] = std::int64_t{34};
    const auto wind = make(op::kWind, looped);
    auto at = [&](double frame) {
        scene.ctx.time = frameToSeconds(frame, scene.ctx.framesPerSecond);
        return valueAs<StrandsPtr>(wind->evaluate(scene.ctx, {in}));
    };
    EXPECT_EQ(at(10)->positions(), at(34)->positions()) << "the loop's end is its start";
    EXPECT_EQ(at(13)->positions(), at(13 + 2 * 24)->positions()) << "it repeats past the range";
    EXPECT_EQ(at(5)->positions(), at(29)->positions()) << "and before it";
    EXPECT_NE(at(10)->positions(), at(20)->positions()) << "it moves within the loop";
    expectLengthsAndRootsKept(*in, *at(20));
    // Seamless: the steps across the seam are ordinary frame steps, and so is the change of step
    // (no kink in the motion).
    float step = 0.0f, bend = 0.0f;
    for (int f = 14; f < 30; ++f) {
        step = std::max(step, mostMoved(*at(f), *at(f + 1)));
        bend = std::max(bend, std::abs(mostMoved(*at(f - 1), *at(f)) - mostMoved(*at(f), *at(f + 1))));
    }
    ASSERT_GT(step, 1e-4f);
    for (const double f : {32.0, 33.0, 34.0, 35.0}) EXPECT_LT(mostMoved(*at(f), *at(f + 1)), 1.5f * step) << "frame " << f;
    EXPECT_LT(std::abs(mostMoved(*at(33), *at(34)) - mostMoved(*at(34), *at(35))), 1.5f * bend + 1e-5f);
    // Off (or a range of no length): the wind as without a loop, to the bit.
    const auto plain = make(op::kWind, windy);
    auto off = looped;
    off["loop"] = false;
    auto empty = looped;
    empty["loopEnd"] = std::int64_t{10};
    scene.ctx.time = 1.3;
    const StrandsPtr want = valueAs<StrandsPtr>(plain->evaluate(scene.ctx, {in}));
    EXPECT_EQ(valueAs<StrandsPtr>(make(op::kWind, off)->evaluate(scene.ctx, {in}))->positions(), want->positions());
    EXPECT_EQ(valueAs<StrandsPtr>(make(op::kWind, empty)->evaluate(scene.ctx, {in}))->positions(), want->positions());
}

TEST(Jiggle, ALoopingScalpJigglesTheSameAtBothEnds) {
    // The plane swaying over frames 100-180 (back where it started at 180), still before them; a
    // looping Jiggle reads the frames before 100 from the end of the range (the host samples
    // them: scalpHistoryLoopFrames), so at 100 it is as at 180. Longer than the sampler's memory
    // of frames around now: the loop's frames are kept all the same.
    const int start = 100, end = 180;
    const MeshPtr rest = test::makePlane(4);
    auto pose = [&](double f) {
        double u = std::clamp(f, double(start), double(end)) - start;
        if (u >= end - start) u = 0.0;  // the end's pose the start's, exactly
        const float x = static_cast<float>(0.5 * std::sin(kTwoPiD * u / (end - start)) + 0.2 * std::sin(2 * kTwoPiD * u / (end - start)));
        std::vector<Vec3f> pts = rest->points();
        for (Vec3f& p : pts) p.x += x;
        return pts;
    };
    auto run = [&](bool loop, double now) {
        const std::string params = std::string(R"({"loop": )") + (loop ? "true" : "false") + R"(, "loopStart": 100, "loopEnd": 180})";
        GraphDocument doc = loadGraphJson(chainGraph(R"({"name": "motion", "type": "Jiggle", "params": )" + params + "}"));
        const MeshPtr scalp = std::make_shared<const ScalpMesh>(pose(now), rest->triangles());
        Scene scene(scalp);
        scene.ctx.time = frameToSeconds(now, scene.ctx.framesPerSecond);
        // As a host samples it: the frames before now, then the loop's.
        const int frames = scalpHistoryFrames(doc.graph, doc.output);
        const std::vector<double> loopFrames = scalpHistoryLoopFrames(doc.graph, doc.output, now);
        std::vector<double> at;
        std::vector<std::int64_t> keys;
        std::vector<double> loopSeconds;
        auto key = [](double seconds) { return static_cast<std::int64_t>(std::llround(seconds * 1e6)); };
        for (int k = 1; k <= frames; ++k) at.push_back(now - k);
        for (const double f : loopFrames) {
            at.push_back(f);
            loopSeconds.push_back(frameToSeconds(f, 24.0));
        }
        for (const double f : at) keys.push_back(key(frameToSeconds(f, 24.0)));
        ScalpHistorySampler sampler;
        const ScalpHistoryPtr history = sampler.history(key(scene.ctx.time), keys, kFrame, scalp->points(),
                                                        [&](std::size_t k) { return pose(at[k]); }, false, loopSeconds);
        scene.ext.set("scalpHistory", history, history->hash);
        return std::make_pair(doc.graph.evaluateStrands(doc.output, scene.ctx), loopFrames.size());
    };
    const auto [atEnd, endFrames] = run(true, end);
    const auto [atStart, startFrames] = run(true, start);
    EXPECT_EQ(endFrames, 0u) << "at the end its frames are the frames before now";
    EXPECT_EQ(startFrames, 12u) << "at the start, all 12 from the end of the range";
    EXPECT_EQ(atStart->positions(), atEnd->positions()) << "the loop's start jiggles as its end";
    // Off, at the start: the scalp was still before it, so the strands pass through (as before).
    const auto [still, none] = run(false, start);
    EXPECT_EQ(none, 0u);
    EXPECT_NE(still->positions(), atStart->positions());
    // Within the range, past the history: the loop changes nothing.
    EXPECT_EQ(run(true, 140).first->positions(), run(false, 140).first->positions());
}

}  // namespace
}  // namespace groomlab
