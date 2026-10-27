// SPDX-License-Identifier: Apache-2.0
// Performance budgets from the spec, measured and asserted. Each test prints
// its numbers ("[ budget ] ..."). ctest runs every test in its own process,
// so peak memory is the test's own. On CI (shared runners with few cores)
// the numbers are reported but not enforced.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "groomlab/brush.h"
#include "groomlab/cards.h"
#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/parts.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/process_caches.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

double peakMegabytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_maxrss) / 1024.0;  // kilobytes on Linux
#endif
}

// Budgets hold on a developer machine (the spec's 16-core reference). Shared
// runners (CI=1, as GitHub Actions sets) only report, unless
// GROOMLAB_ENFORCE_BUDGETS=1 makes them gate: the workflow's budgets job runs
// so on a dedicated runner, and a release build can be checked anywhere with it.
bool enforced() {
    if (const char* force = std::getenv("GROOMLAB_ENFORCE_BUDGETS")) {
        return *force && std::string(force) != "0";
    }
    return std::getenv("CI") == nullptr;
}
#define EXPECT_BUDGET(value, limit)                \
    do {                                           \
        const double measured = (value);           \
        if (enforced()) EXPECT_LT(measured, limit); \
    } while (0)

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The budgets assume parallel loops: gating a build without TBB would only measure that.
TEST(Budget, GatedBuildsAreParallel) {
    if (!enforced()) GTEST_SKIP() << "budgets only report here";
    EXPECT_TRUE(GROOMLAB_HAS_TBB) << "this build runs parallel loops serially (no TBB found at configure)";
}

// A production-like graph: Scatter -> Interpolate -> Clump (400 centers of its own) -> Noise -> Width.
std::string lookGraph(double density, int segments) {
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer), R"({"version": 1, "output": "out",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": %f}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"strength": 0.7, "count": 400}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.02}},
        {"name": "width", "type": "Width"},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": %d}}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "interp", "input": "strands"},
        {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"},
        {"from": "interp", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "noise", "input": "strands"},
        {"from": "noise", "to": "width", "input": "strands"},
        {"from": "width", "to": "out", "input": "strands"}]})",
                  density, segments);
    return buffer;
}

GuideHierarchy guidesOn(const ScalpMesh& scalp, int primaries, std::size_t children) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(4);
    for (int i = 0; i < primaries; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f(0.2f, -0.3f, 0), 15));
    }
    h.evaluate(&scalp);
    if (children > 0) {
        ChildGenerationParams p;
        p.count = children;
        h.generateChildren(primary, "children", p);
        h.evaluate(&scalp);
    }
    return h;
}

TEST(Budget, RenderExpansionOfAMillionStrands) {
    // Spec: 1M strands x 24 points expand in < 5 s with < 1.25 GB peak (the CPU engine's: the
    // scalp's clearance field built whole for Interpolate's walk, ~1.1 GB measured; 0.9 GB before).
    const double baseline = peakMegabytes();
    const auto scalp = test::makeSphere(120, 240, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = lookGraph(80000.0, 23);  // ~1M strands on the unit sphere
    data.hierarchyJson = guidesOn(*scalp, 300, 0).toJson();

    const auto t0 = std::chrono::steady_clock::now();
    Groom groom(data);
    groom.setCacheIntermediates(false);  // render: nothing is re-evaluated, keep only what is needed
    const StrandsPtr strands = groom.evaluate(1.0f, 0.0);
    const double ms = msSince(t0);
    const double peak = peakMegabytes() - baseline;
    ASSERT_TRUE(strands);
    std::printf("[ budget ] render expansion: %zu strands x %zu points in %.0f ms, peak +%.0f MB\n",
                strands->strandCount(), strands->uniformPointCount(), ms, peak);
    EXPECT_GT(strands->strandCount(), 950000u);
    EXPECT_EQ(strands->uniformPointCount(), 24u);
    EXPECT_BUDGET(ms, 5000.0);
    EXPECT_BUDGET(peak, 1280.0);
}

TEST(Budget, ParameterTweakAtPreviewDensity) {
    // Spec: a single-parameter tweak with upstream cached, at preview density, < 33 ms.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = lookGraph(8000.0, 15);  // 100k strands at full density
    data.hierarchyJson = guidesOn(*scalp, 300, 0).toJson();
    GraphDocument doc = loadGraphJson(data.graphJson);
    auto hierarchy = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(data.hierarchyJson));
    hierarchy->evaluate(scalp.get());
    ExternalInputs ext;
    setGroomExternals(ext, scalp, nullptr, hierarchy);
    EvalContext ctx;
    ctx.previewDensity = 0.1f;
    ctx.externals = &ext;
    doc.graph.evaluateStrands(doc.output, ctx);  // warm the cache

    double worst = 0.0;
    for (int i = 0; i < 5; ++i) {
        doc.graph.node(doc.node("noise")).params().set("amplitude", 0.01 + 0.01 * i);
        const auto t0 = std::chrono::steady_clock::now();
        doc.graph.evaluateStrands(doc.output, ctx);
        worst = std::max(worst, msSince(t0));
    }
    std::printf("[ budget ] noise tweak at 10%% preview (10k strands): %.1f ms worst of 5\n", worst);
    EXPECT_BUDGET(worst, 33.0);
}

// Worst time of five brush dabs on 2k guides (200 primaries + 1800 children),
// each followed by a re-evaluation of `graph` at 10% preview.
double brushDabMs(const std::string& graph, const char* label) {
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GuideHierarchy h = guidesOn(*scalp, 200, 9);
    EXPECT_EQ(h.guides().size(), 2000u);
    GraphDocument doc = loadGraphJson(graph);
    EvalContext ctx;
    ctx.previewDensity = 0.1f;
    ExternalInputs ext;
    auto preview = [&] {
        auto snapshot = std::make_shared<GuideHierarchy>(h);
        setGroomExternals(ext, scalp, nullptr, snapshot);
        ctx.externals = &ext;
        return doc.graph.evaluateStrands(doc.output, ctx);
    };
    preview();

    BrushSettings brush;
    brush.radius = 0.3f;
    brush.level = 0;
    brush.propagate = true;
    double worst = 0.0;
    for (int i = 0; i < 5; ++i) {
        const float x = -0.2f + 0.1f * static_cast<float>(i);
        const auto t0 = std::chrono::steady_clock::now();
        applyBrushStroke(h, scalp.get(), brush, {{x, 1.2f, 0.0f}, {x + 0.1f, 1.2f, 0.0f}});
        const double strokeMs = msSince(t0);
        preview();
        const double totalMs = msSince(t0);
        std::printf("[ budget ]   dab %d: stroke %.1f ms + preview %.1f ms\n", i, strokeMs, totalMs - strokeMs);
        worst = std::max(worst, totalMs);
    }
    std::printf("[ budget ] brush dab on 2000 guides + 10%% preview of %s: %.1f ms worst of 5\n", label, worst);
    return worst;
}

TEST(Budget, AParamTweakThroughTheGraphText) {
    // What a slider tick costs on the way a host takes: the graph's JSON written again with one
    // param changed, parsed, every document cloned from the parse and the old cache adopted
    // (GroomSession::setGraphJson), then the evaluation at preview density. The text part alone
    // must stay well under a frame on a look graph of a dozen nodes.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = lookGraph(8000.0, 15);
    data.hierarchyJson = guidesOn(*scalp, 100, 0).toJson();
    Groom groom(data);
    ASSERT_TRUE(groom.evaluate(0.1f, 0.0));
    double textMs = 0.0, evalMs = 0.0;
    const std::string was = "\"amplitude\": 0.02";
    for (int i = 0; i < 5; ++i) {
        char amplitude[32];
        std::snprintf(amplitude, sizeof amplitude, "\"amplitude\": %.3f", 0.03 + 0.01 * i);
        std::string text = data.graphJson;
        const std::size_t at = text.find(was);
        ASSERT_NE(at, std::string::npos);
        text.replace(at, was.size(), amplitude);
        const auto t0 = std::chrono::steady_clock::now();
        groom.setGraphJson(text);
        const double t = msSince(t0);
        const auto t1 = std::chrono::steady_clock::now();
        ASSERT_TRUE(groom.evaluate(0.1f, 0.0));
        textMs = std::max(textMs, t);
        evalMs = std::max(evalMs, msSince(t1));
    }
    std::printf("[ budget ] a param tweak through the graph text: text %.2f ms + evaluation %.1f ms, worst of 5\n", textMs,
                evalMs);
    EXPECT_BUDGET(textMs, 3.0);
}

TEST(Budget, BrushDabWithInterpolationPreview) {
    // Spec: a brush dab on 2k guides, with the interpolation preview, < 16 ms.
    // The interpolation preview is the default graph (Scatter, Interpolate,
    // Length, Width); a full look graph (clumps, noise) is measured too.
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Scatter\"";
    graph.replace(graph.find(from), from.size(), from + ", \"params\": {\"density\": 8000.0, \"segments\": 15}");
    EXPECT_BUDGET(brushDabMs(graph, "the interpolation graph"), 16.0);
    brushDabMs(lookGraph(8000.0, 15), "a full look graph (reported only)");
    // What keeping clearance costs of that (reported only).
    const std::string node = "\"type\": \"Interpolate\"";
    graph.replace(graph.find(node), node.size(), node + ", \"params\": {\"keepClearance\": false}");
    brushDabMs(graph, "the interpolation graph without keeping clearance (reported only)");
}

// The scalp with its "mask" color set at `value` on every corner.
std::shared_ptr<const ScalpMesh> withMask(const ScalpMesh& scalp, float value) {
    auto out = std::make_shared<ScalpMesh>(scalp);
    out->setCornerColors("mask", std::vector<ScalpMesh::Color>(scalp.triangleCount() * 3, {value, value, value, 1.0f}));
    return out;
}

TEST(Budget, VertexMapPaintDab) {
    // A dab on a vertex map (a color set of the scalp) re-evaluates what reads the map and what
    // follows it; Scatter and Interpolate stay cached.
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    std::string graph = lookGraph(8000.0, 15);
    // A Transform reading the map (its scale), right after Interpolate.
    const std::string node = R"({"name": "interp", "type": "Interpolate"},)";
    graph.replace(graph.find(node), node.size(),
                  node + R"( {"name": "mask", "type": "Transform", "params": {"scale": {"colorSet": "mask", "min": 0.5, "max": 1.0}}},)");
    const std::string link = R"({"from": "interp", "to": "clump", "input": "strands"},)";
    graph.replace(graph.find(link), link.size(),
                  R"({"from": "interp", "to": "mask", "input": "strands"},
        {"from": "mask", "to": "clump", "input": "strands"},)");
    GraphDocument doc = loadGraphJson(graph);
    auto hierarchy = std::make_shared<GuideHierarchy>(guidesOn(*sphere, 300, 0));
    ExternalInputs ext;
    EvalContext ctx;
    ctx.previewDensity = 0.1f;
    ctx.externals = &ext;
    setGroomExternals(ext, withMask(*sphere, 1.0f), nullptr, hierarchy);
    const StrandsPtr first = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(first);

    double worst = 0.0;
    for (int i = 0; i < 5; ++i) {
        setGroomExternals(ext, withMask(*sphere, 0.9f - 0.1f * static_cast<float>(i)), nullptr, hierarchy);
        const std::size_t before = doc.graph.evaluationCount();
        const auto t0 = std::chrono::steady_clock::now();
        const StrandsPtr strands = doc.graph.evaluateStrands(doc.output, ctx);
        worst = std::max(worst, msSince(t0));
        ASSERT_TRUE(strands);
        // The map's Transform and what follows it (clump, noise, width, out); not the Scalp,
        // Guides, Scatter or Interpolate.
        EXPECT_EQ(doc.graph.evaluationCount() - before, 5u) << "dab " << i;
    }
    std::printf("[ budget ] vertex map dab, 10%% preview (10k strands): %.1f ms worst of 5\n", worst);
}

TEST(Budget, CachedReevaluationWithUdimMap) {
    // Nothing changed: every node is a cache hit, and the keys (a UDIM texture's stamp) cost little.
    const fs::path dir = fs::temp_directory_path() / "groomlab_budget_udim";
    fs::create_directories(dir);
    for (const int tile : {1001, 1002}) {
        std::ofstream f(dir / ("map." + std::to_string(tile) + ".pgm"), std::ios::binary);
        f << "P5\n2 2\n255\n";
        for (int k = 0; k < 4; ++k) f.put(static_cast<char>(tile == 1001 ? 60 : 200));
    }
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(16, 2.0f));
    std::vector<ScalpMesh::UV> uvs;
    for (const ScalpMesh::Triangle& t : plane->triangles()) {
        for (const std::int32_t v : t) uvs.push_back({plane->points()[v].x, plane->points()[v].z * 0.5f});
    }
    plane->setCornerUVs(std::move(uvs));
    const std::string pattern = (dir / "map.<UDIM>.pgm").generic_u8string();
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 500}},
        {"name": "length", "type": "Transform", "params": {"scale": {"texture": ")" + pattern + R"("}}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "length", "input": "strands"}, {"from": "length", "to": "out", "input": "strands"}]})");
    ExternalInputs ext;
    setGroomExternals(ext, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    const std::size_t runs = doc.graph.evaluationCount();
    constexpr int kRepeats = 20;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRepeats; ++i) doc.graph.evaluateStrands(doc.output, ctx);
    const double ms = msSince(t0) / kRepeats;
    EXPECT_EQ(doc.graph.evaluationCount(), runs);  // all cached
    std::printf("[ budget ] cached re-evaluation with a UDIM map param: %.3f ms\n", ms);
    EXPECT_BUDGET(ms, 1.0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(Budget, CollidePlaybackFrame) {
    // A deforming scalp: every frame re-evaluates; Collide's own time is reported (~50k strands,
    // a head of radius 1 with strands 0.6 long hanging down onto it, maxDepth 5% of the radius).
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 4000, "length": 0.6}},
        {"name": "comb", "type": "Gravity", "params": {"strength": 0.6}},
        {"name": "collide", "type": "Collide", "params": {"maxDepth": 0.05}}, {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "comb", "input": "strands"}, {"from": "comb", "to": "collide", "input": "strands"},
        {"from": "scalp", "to": "collide", "input": "scalp"}, {"from": "collide", "to": "out", "input": "strands"}]})");
    const NodeId collide = doc.node("collide");
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    double total = 0.0, collideMs = 0.0;
    std::size_t strands = 0;
    constexpr int kFrames = 5;
    for (int frame = 0; frame <= kFrames; ++frame) {
        std::vector<Vec3f> points = sphere->points();
        const float s = 1.0f + 0.02f * static_cast<float>(frame);
        for (Vec3f& p : points) p = Vec3f(p.x * s, p.y, p.z * s);
        setGroomExternals(ext, std::make_shared<const ScalpMesh>(rest->withPoints(std::move(points))), nullptr, nullptr);
        const auto t0 = std::chrono::steady_clock::now();
        const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
        const double ms = msSince(t0);
        ASSERT_TRUE(out);
        strands = out->strandCount();
        if (frame == 0) continue;  // warm-up
        total += ms;
        collideMs += doc.graph.lastEvaluationTimes().at(collide);
    }
    std::printf("[ budget ] playback of a deforming scalp, %zu strands: %.1f ms per frame, Collide %.1f ms\n", strands,
                total / kFrames, collideMs / kFrames);
}

TEST(Budget, FollowScalpCarryFrame) {
    // Following the scalp, every frame of a moving scalp carries the strands evaluated at rest
    // (carryStrands): a wolf's fur, ~180k strands of 51 points with an instance on every tenth,
    // turned a little more each frame. The carried points go straight into an array of their own
    // in parallel (not the rest pose's copied first and then moved).
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    constexpr std::size_t kStrands = 180000, kPoints = 51;
    auto strands = std::make_shared<StrandSet>();
    strands->resizeUniform(kStrands, kPoints);
    {
        Rng rng(11);
        std::vector<StrandId>& ids = strands->mutableIds();
        std::vector<StrandRoot>& roots = strands->mutableRoots();
        std::vector<Vec3f>& positions = strands->mutablePositions();
        for (std::size_t s = 0; s < kStrands; ++s) {
            ids[s] = hashCombine(hash64(s), 0x5EEDu);
            StrandRoot root;
            root.face = static_cast<std::int32_t>(rng.uniform(0.0f, 1.0f) * static_cast<float>(rest->triangleCount() - 1));
            root.u = rng.uniform(0.0f, 0.5f);
            root.v = rng.uniform(0.0f, 0.5f);
            roots[s] = root;
            const Vec3f at = rest->position(root), n = rest->normal(root);
            for (std::size_t k = 0; k < kPoints; ++k) positions[s * kPoints + k] = at + n * (0.01f * static_cast<float>(k));
        }
        auto instances = std::make_shared<InstanceSet>();
        instances->prototypes.push_back("bead");
        for (std::size_t s = 0; s < kStrands; s += 10) {
            InstanceSet::Instance i;
            i.strand = ids[s];
            i.id = s;
            i.orient = InstanceOrient::Random;
            instances->instances.push_back(i);
        }
        strands->setInstances(instances);
    }
    std::vector<double> times;
    StrandsPtr carried;
    for (int frame = 0; frame <= 6; ++frame) {
        const float a = 0.02f * static_cast<float>(frame + 1), c = std::cos(a), s = std::sin(a);
        std::vector<Vec3f> points = sphere->points();
        for (Vec3f& p : points) p = Vec3f(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
        const ScalpMesh now = rest->withPoints(std::move(points));
        const auto t0 = std::chrono::steady_clock::now();
        carried = carryStrands(strands, *rest, now);  // (the last frame's goes as a playback's would)
        times.push_back(msSince(t0));
        ASSERT_TRUE(carried);
        ASSERT_NE(carried, strands);
    }
    times.erase(times.begin());  // warm-up
    std::sort(times.begin(), times.end());
    std::printf("[ budget ] follow-the-scalp carry of %zu strands x %zu points: %.1f ms per frame (median)\n", kStrands, kPoints,
                times[times.size() / 2]);
    EXPECT_BUDGET(times[times.size() / 2], 30.0);
}

TEST(Budget, SelfCollideEvaluation) {
    // SelfCollide is global (every strand's push reads the others'), so a brush stroke upstream
    // evaluates it whole. Long hair (18 units, 16 points) combed flat over a head-sized sphere
    // (radius 10, the scalp: roots over its top, every strand at one height over the skin, so most
    // of the hair is overpacked), 100k and 200k strands: its time, median of 5. Every strand core
    // (the most it splats), and as a render of a Scatter carries them (a core of a sixteenth, a
    // thirty-second).
    constexpr float kRadius = 10.0f, kLift = 0.05f, kLength = 18.0f;
    constexpr std::size_t kPoints = 16;
    const auto sphere = test::makeSphere(48, 96, kRadius);
    ExternalInputs ext;
    setGroomExternals(ext, sphere, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    for (const std::size_t count : {std::size_t{100000}, std::size_t{200000}}) {
        auto strands = std::make_shared<StrandSet>();
        strands->reserve(count, count * kPoints);
        std::vector<Vec3f> points(kPoints);
        const float r = kRadius + kLift;
        for (std::size_t i = 0; i < count; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
            const float theta0 = std::acos(1.0f - u * 0.5f), phi = static_cast<float>(i) * 2.39996323f;
            const float arc = r * (1.5707963f - theta0);
            for (std::size_t k = 0; k < kPoints; ++k) {
                const float along = kLength * static_cast<float>(k) / static_cast<float>(kPoints - 1);
                const float rr = k == 0 ? kRadius : r;
                const float theta = theta0 + along / r;
                points[k] = along <= arc ? Vec3f(rr * std::sin(theta) * std::cos(phi), rr * std::cos(theta), rr * std::sin(theta) * std::sin(phi))
                                         : Vec3f(r * std::cos(phi), -(along - arc), r * std::sin(phi));
            }
            strands->addStrand(static_cast<StrandId>(i), points, {}, sphere->closestPoint(points[0]).root);
        }
        auto rendered = std::make_shared<StrandSet>(*strands);
        std::vector<float>& roles = rendered->addStrandChannel(kPreviewChannel, kCoreShown);
        const float core = coreShare(static_cast<double>(count));
        for (std::size_t i = 0; i < count; ++i) previewRole(rendered->ids()[i], 2.0f, core, &roles[i]);
        for (const StrandsPtr& in : {StrandsPtr(strands), StrandsPtr(rendered)}) {
            auto node = OperatorRegistry::instance().create(op::kSelfCollide);
            std::vector<double> times;
            StrandsPtr out;
            for (int run = 0; run < 5; ++run) {
                const auto t0 = std::chrono::steady_clock::now();
                out = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(in)}));
                times.push_back(msSince(t0));
            }
            ASSERT_TRUE(out && out != in);
            std::size_t moved = 0;
            for (std::size_t i = 0; i < out->pointCount(); ++i) moved += (out->positions()[i] - in->positions()[i]).length() > 0.05f ? 1 : 0;
            std::sort(times.begin(), times.end());
            std::printf("[ budget ] SelfCollide, %zu strands x %zu points, %s: %.1f ms (%.0f%% of the points moved)\n", count, kPoints,
                        in == rendered ? "a render's core" : "every strand core", times[2],
                        100.0 * static_cast<double>(moved) / static_cast<double>(out->pointCount()));
            EXPECT_GT(moved, out->pointCount() / 4);
            EXPECT_BUDGET(times[2], count < 150000 ? 150.0 : 250.0);  // (the aim: about 100 ms at 100k)
        }
    }
}

TEST(Budget, StaticEvaluation) {
    // Static is global (every strand's push reads the others' charge), so a brush stroke upstream
    // evaluates it whole. Long hair (18 units, 16 points) combed over a head-sized sphere (radius
    // 10, the scalp), 100k and 200k strands: its time, median of 5. Every strand core (the most it
    // splats), and as a render of a Scatter carries them.
    constexpr float kRadius = 10.0f, kLift = 0.05f, kLength = 18.0f;
    constexpr std::size_t kPoints = 16;
    const auto sphere = test::makeSphere(48, 96, kRadius);
    ExternalInputs ext;
    setGroomExternals(ext, sphere, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    for (const std::size_t count : {std::size_t{100000}, std::size_t{200000}}) {
        auto strands = std::make_shared<StrandSet>();
        strands->reserve(count, count * kPoints);
        std::vector<Vec3f> points(kPoints);
        const float r = kRadius + kLift;
        for (std::size_t i = 0; i < count; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
            const float theta0 = std::acos(1.0f - u * 0.5f), phi = static_cast<float>(i) * 2.39996323f;
            const float arc = r * (1.5707963f - theta0);
            for (std::size_t k = 0; k < kPoints; ++k) {
                const float along = kLength * static_cast<float>(k) / static_cast<float>(kPoints - 1);
                const float rr = k == 0 ? kRadius : r;
                const float theta = theta0 + along / r;
                points[k] = along <= arc ? Vec3f(rr * std::sin(theta) * std::cos(phi), rr * std::cos(theta), rr * std::sin(theta) * std::sin(phi))
                                         : Vec3f(r * std::cos(phi), -(along - arc), r * std::sin(phi));
            }
            strands->addStrand(static_cast<StrandId>(i), points, {}, sphere->closestPoint(points[0]).root);
        }
        auto rendered = std::make_shared<StrandSet>(*strands);
        std::vector<float>& roles = rendered->addStrandChannel(kPreviewChannel, kCoreShown);
        const float core = coreShare(static_cast<double>(count));
        for (std::size_t i = 0; i < count; ++i) previewRole(rendered->ids()[i], 2.0f, core, &roles[i]);
        for (const StrandsPtr& in : {StrandsPtr(strands), StrandsPtr(rendered)}) {
            auto node = OperatorRegistry::instance().create(op::kStatic);
            std::vector<double> times;
            StrandsPtr out;
            for (int run = 0; run < 5; ++run) {
                const auto t0 = std::chrono::steady_clock::now();
                out = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(in)}));
                times.push_back(msSince(t0));
            }
            ASSERT_TRUE(out && out != in);
            std::size_t moved = 0;
            for (std::size_t i = 0; i < out->pointCount(); ++i) moved += (out->positions()[i] - in->positions()[i]).length() > 0.05f ? 1 : 0;
            std::sort(times.begin(), times.end());
            std::printf("[ budget ] Static, %zu strands x %zu points, %s: %.1f ms (%.0f%% of the points moved)\n", count, kPoints,
                        in == rendered ? "a render's core" : "every strand core", times[2],
                        100.0 * static_cast<double>(moved) / static_cast<double>(out->pointCount()));
            EXPECT_GT(moved, out->pointCount() / 4);
            EXPECT_BUDGET(times[2], count < 150000 ? 150.0 : 250.0);  // (the aim: about 100 ms at 100k)
        }
    }
}

TEST(Budget, PropagateFiftyShafts) {
    // 50 hand-drawn shafts: few strands, many barbs each.
    const auto plane = test::makePlane(8, 2.0f);
    auto shafts = std::make_shared<StrandSet>();
    for (int k = 0; k < 50; ++k) {
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 20; ++i) {
            pts.push_back({0.1f + 0.035f * static_cast<float>(k), 0.02f * static_cast<float>(i),
                           0.2f + 0.06f * static_cast<float>(i)});
        }
        shafts->addStrand(static_cast<StrandId>(k + 1), pts, {}, plane->closestPoint(pts[0]).root);
    }
    auto op = OperatorRegistry::instance().create(op::kPropagate);
    op->params().set("count", std::int64_t{150});
    op->params().set("splits", 4.0);
    ExternalInputs ext;
    setGroomExternals(ext, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    double best = 1e30;
    std::size_t barbs = 0;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(shafts))}));
        best = std::min(best, msSince(t0));
        ASSERT_TRUE(out);
        barbs = out->strandCount();
    }
    std::printf("[ budget ] Propagate on 50 shafts (%zu strands out): %.1f ms best of 5\n", barbs, best);
}

TEST(Budget, CullAndSplitHalfMillion) {
    // 500k strands with 12 channels (8 per strand, 4 per point): the gathers into a subset.
    constexpr std::size_t kStrands = 500000, kPoints = 8;
    auto set = std::make_shared<StrandSet>();
    set->resizeUniform(kStrands, kPoints);
    for (std::size_t s = 0; s < kStrands; ++s) set->mutableIds()[s] = hash64(s + 1);
    for (int c = 0; c < 8; ++c) {
        std::vector<float>& ch = set->addStrandChannel("strand" + std::to_string(c), 0.0f);
        for (std::size_t s = 0; s < kStrands; ++s) ch[s] = static_cast<float>(s % 97) * 0.01f + c;
    }
    for (int c = 0; c < 4; ++c) {
        std::vector<float>& ch = set->addPointChannel("point" + std::to_string(c), 0.0f);
        for (std::size_t p = 0; p < ch.size(); ++p) ch[p] = static_cast<float>(p % 31) * 0.1f + c;
    }
    EvalContext ctx;
    auto time = [&](const char* type, auto setup) {
        auto op = OperatorRegistry::instance().create(type);
        setup(*op);
        double best = 1e30;
        std::size_t kept = 0;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(set))}));
            best = std::min(best, msSince(t0));
            kept = out ? out->strandCount() : 0;
        }
        std::printf("[ budget ] %s of %zu strands x %zu points, 12 channels (%zu kept): %.1f ms best of 3\n", type,
                    kStrands, kPoints, kept, best);
    };
    time(op::kCull, [](Operator& op) { op.params().set("keep", 0.5); });
    time(op::kSplit, [](Operator&) {});
}

// Hands on its input; incrementally it says the strands the test dabbed changed (a brush's
// changes, without a diff of the whole set).
class DabbedOp : public Operator {
public:
    explicit DabbedOp(const std::vector<std::uint32_t>* dabbed) : dabbed_(dabbed) {}
    std::string typeName() const override { return "TestDabbed"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs&, Value* out,
                             StrandChanges* changed) const override {
        *out = inputs.at(0);
        *changed = StrandChanges::listed(*dabbed_);
        return true;
    }

private:
    const std::vector<std::uint32_t>* dabbed_;
};

class HeldOp : public Operator {
public:
    std::string typeName() const override { return "TestHeld"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    std::vector<std::string> externalInputs() const override { return {"strands"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("strands"); }
};

TEST(Budget, SplitDabHalfMillion) {
    // A brush dab (1000 strands moved) through Split into two looks (Noise on a, Curl on b) merged
    // again, at 500k strands x 8 points: what the graph takes per dab, median of 9.
    constexpr std::size_t kStrands = 500000, kPoints = 8, kDab = 1000;
    auto strands = std::make_shared<StrandSet>();
    strands->reserve(kStrands, kStrands * kPoints);
    Rng rng(17);
    std::vector<Vec3f> points(kPoints);
    for (std::size_t i = 0; i < kStrands; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        for (std::size_t k = 0; k < kPoints; ++k) points[k] = n * (10.0f + static_cast<float>(k));
        strands->addStrand(static_cast<StrandId>(i + 1), points);
    }
    std::vector<std::uint32_t> dabbed;
    Graph g;
    auto& reg = OperatorRegistry::instance();
    const NodeId held = g.addNode(std::make_unique<HeldOp>());
    const NodeId source = g.addNode(std::make_unique<DabbedOp>(&dabbed));
    const NodeId split = g.addNode(reg.create(op::kSplit));
    const NodeId noise = g.addNode(reg.create(op::kNoise));
    const NodeId curl = g.addNode(reg.create(op::kCurl));
    const NodeId merge = g.addNode(reg.create(op::kMerge));
    const NodeId out = g.addNode(reg.create(op::kOutput));
    g.node(noise).params().set("amplitude", 0.5);
    g.node(curl).params().set("radius", 0.3);
    g.connect(held, source, 0);
    g.connect(source, split, 0);
    g.connect(split, noise, 0, 0);
    g.connect(split, curl, 0, 1);
    g.connect(noise, merge, 0);
    g.connect(curl, merge, 1);
    g.connect(merge, out, 0);
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    std::uint64_t version = 1;
    ext.set("strands", StrandsPtr(strands), version);
    ASSERT_TRUE(g.evaluateStrands(out, ctx));
    std::vector<double> times;
    std::size_t incremental = 0;
    for (int d = 0; d < 9; ++d) {
        auto next = std::make_shared<StrandSet>(*strands);
        dabbed.clear();
        for (std::size_t k = 0; k < kDab; ++k) dabbed.push_back(static_cast<std::uint32_t>((d * 7919 + k * 389) % kStrands));
        std::sort(dabbed.begin(), dabbed.end());
        dabbed.erase(std::unique(dabbed.begin(), dabbed.end()), dabbed.end());
        for (const std::uint32_t s : dabbed) next->strandPoints(s)[kPoints - 1] += Vec3f(0.01f, 0.0f, 0.0f);
        strands = next;
        ext.set("strands", StrandsPtr(strands), ++version);
        const std::size_t before = g.incrementalCount();
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(g.evaluateStrands(out, ctx));
        times.push_back(msSince(t0));
        incremental = g.incrementalCount() - before;
    }
    std::sort(times.begin(), times.end());
    std::printf("[ budget ] a dab of %zu strands through Split -> Noise / Curl -> Merge, %zu strands x %zu points: %.1f ms "
                "median of 9 (%zu nodes incremental)\n",
                kDab, kStrands, kPoints, times[times.size() / 2], incremental);
    // Split, both looks and Merge make only the dabbed strands again (in full: 150-290 ms).
    EXPECT_EQ(incremental, 5u);
    EXPECT_BUDGET(times[times.size() / 2], 40.0);
}

TEST(Budget, LitStrandsOwnColorsHalfMillion) {
    // The lit strands' own colors (strandLookColors) at 500k strands x 16 points: HairColor's per
    // strand colors, then per point (a tip blend), each found varying and keyed (its colors hashed,
    // so a host keeps them while they stay), and written as the colors stream a host uploads.
    constexpr std::size_t kStrands = 500000, kPoints = 16;
    auto set = std::make_shared<StrandSet>();
    set->resizeUniform(kStrands, kPoints);
    for (int k = 0; k < 3; ++k) {
        std::vector<float>& c = set->addStrandChannel(kColorChannels[k], 0.0f);
        for (std::size_t s = 0; s < kStrands; ++s) c[s] = static_cast<float>((s * 7 + static_cast<std::size_t>(k)) % 101) / 100.0f;
    }
    std::vector<float> rgba(kStrands * kPoints * 4);
    auto time = [&](const char* what, const StrandsPtr& strands) {
        double find = 1e30, write = 1e30;
        std::shared_ptr<const StrandColors> colors;
        for (int i = 0; i < 3; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            colors = strandLookColors(strands);
            find = std::min(find, msSince(t0));
            ASSERT_TRUE(colors);
            t0 = std::chrono::steady_clock::now();
            colors->write(rgba.data());
            write = std::min(write, msSince(t0));
        }
        std::printf("[ budget ] lit strands' own colors per %s, %zu strands x %zu points: found and keyed %.1f ms, the %.0f MB "
                    "stream written %.1f ms (best of 3)\n",
                    what, kStrands, kPoints, find, static_cast<double>(rgba.size() * sizeof(float)) / 1e6, write);
        EXPECT_BUDGET(find, 50.0);
        EXPECT_BUDGET(write, 100.0);
    };
    time("strand", set);
    auto tipped = std::make_shared<StrandSet>(*set);
    for (int k = 0; k < 3; ++k) {
        std::vector<float>& c = tipped->addPointChannel(kColorChannels[k], 0.0f);
        for (std::size_t p = 0; p < c.size(); ++p) c[p] = static_cast<float>((p * 3 + static_cast<std::size_t>(k)) % 89) / 88.0f;
    }
    time("point", tipped);
}

TEST(Budget, GroomFileSizeFor5kGuides) {
    // Spec: graph + 5k guides (no maps) < 10 MB.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GroomFile file;
    file.data.scalp = scalp;
    file.data.graphJson = lookGraph(8000.0, 15);
    file.data.hierarchyJson = guidesOn(*scalp, 500, 9).toJson();  // 500 + 4500 guides, 16 points
    const std::size_t bytes = groomFileToJson(file).size();
    std::printf("[ budget ] groom file with 5000 guides and a %zu-point scalp: %.2f MB\n", scalp->pointCount(),
                static_cast<double>(bytes) / (1024.0 * 1024.0));
    EXPECT_LT(bytes, 10u * 1024u * 1024u);
}

// The node times of the last evaluation, by node name.
double nodeMs(const GraphDocument& doc, const char* name) {
    const auto& times = doc.graph.lastEvaluationTimes();
    const auto it = times.find(doc.node(name));
    return it == times.end() ? 0.0 : it->second;
}

// The sphere with a rest pose, stretched a little more on every frame (a deforming head).
std::shared_ptr<const ScalpMesh> stretchedFrame(const ScalpMesh& rest, int frame) {
    std::vector<Vec3f> points = rest.restPoints();
    const float s = 1.0f + 0.02f * static_cast<float>(frame);
    for (Vec3f& p : points) p = Vec3f(p.x * s, p.y, p.z * s);
    return std::make_shared<const ScalpMesh>(rest.withPoints(std::move(points)));
}

TEST(Budget, DeformingScalpRelaxedScatterFrame) {
    // Scatter at randomness 0.5 (relaxed roots), Interpolate and Clump (its own centers) at 100k strands on
    // a deforming scalp: every node re-runs on every frame; the roots stay put on the rest pose.
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    std::string graph = lookGraph(8000.0, 15);
    const std::string scatter = R"("type": "Scatter", "params": {)";
    graph.replace(graph.find(scatter), scatter.size(), scatter + R"("randomness": 0.5, )");
    const GuideHierarchy guides = guidesOn(*sphere, 300, 0);
    for (const float density : {1.0f, 0.1f}) {
        GraphDocument doc = loadGraphJson(graph);
        ExternalInputs ext;
        EvalContext ctx;
        ctx.previewDensity = density;
        ctx.externals = &ext;
        constexpr int kFrames = 5;
        double total = 0.0, scatterMs = 0.0, interpMs = 0.0, clumpMs = 0.0;
        std::size_t strands = 0;
        for (int frame = 0; frame <= kFrames; ++frame) {
            const auto scalp = stretchedFrame(*rest, frame);
            auto hierarchy = std::make_shared<GuideHierarchy>(guides);
            hierarchy->evaluate(scalp.get());
            setGroomExternals(ext, scalp, nullptr, hierarchy);
            const auto t0 = std::chrono::steady_clock::now();
            const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
            const double ms = msSince(t0);
            ASSERT_TRUE(out);
            strands = out->strandCount();
            if (frame == 0) continue;  // the first frame relaxes the roots
            total += ms;
            scatterMs += nodeMs(doc, "scatter");
            interpMs += nodeMs(doc, "interp");
            clumpMs += nodeMs(doc, "clump");
        }
        std::printf("[ budget ] deforming scalp, Scatter randomness 0.5, density %.1f (%zu strands): %.1f ms per frame "
                    "(Scatter %.1f, Interpolate %.1f, Clump %.1f)\n",
                    density, strands, total / kFrames, scatterMs / kFrames, interpMs / kFrames, clumpMs / kFrames);
    }
}

TEST(Budget, ScatterRandomness500k) {
    // Scatter at 500k strands on a head-sized sphere, the render and a 10% preview: the first
    // placement (its even spots relaxed, but for a render at randomness 1), then randomness tweaks,
    // which only mix the kept placements again.
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    const double density = 500000.0 / static_cast<double>(sphere->restTotalArea());
    for (const float preview : {1.0f, 0.1f}) {
        Graph graph;
        ExternalInputs ext;
        ext.set("scalp", sphere, sphere->surfaceHash());
        const NodeId scalp = graph.addNode(op::kScalp);
        const NodeId scatter = graph.addNode(op::kScatter);
        graph.node(scatter).params().set("density", density);
        graph.connect(scalp, scatter, 0);
        auto run = [&](double randomness) {
            graph.node(scatter).params().set("randomness", randomness);
            EvalContext ctx;
            ctx.previewDensity = preview;
            ctx.externals = &ext;
            const auto t0 = std::chrono::steady_clock::now();
            const StrandsPtr out = graph.evaluateStrands(scatter, ctx);
            const double ms = msSince(t0);
            EXPECT_TRUE(out);
            return std::make_pair(ms, out ? out->strandCount() : 0);
        };
        const auto [first, strands] = run(1.0);
        const double tweak = run(0.99).first, back = run(1.0).first, half = run(0.5).first, over = run(1.5).first;
        std::printf("[ budget ] Scatter 500k, preview %.1f (%zu strands): randomness 1 first %.1f ms; tweaks: 0.99 %.1f, "
                    "1 %.1f, 0.5 %.1f, 1.5 %.1f ms\n",
                    preview, strands, first, tweak, back, half, over);
    }
}

TEST(Budget, BrushDabInterpolate5kGuides) {
    // A dab on 5k guides (500 primaries + 4500 children), the 10% preview of 500k strands (50k):
    // Interpolate's time per dab (Scatter stays cached).
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GuideHierarchy h = guidesOn(*scalp, 500, 9);
    ASSERT_EQ(h.guides().size(), 5000u);
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Scatter\"";
    graph.replace(graph.find(from), from.size(), from + ", \"params\": {\"density\": 40000.0}");
    GraphDocument doc = loadGraphJson(graph);
    EvalContext ctx;
    ctx.previewDensity = 0.1f;
    ExternalInputs ext;
    auto preview = [&] {
        setGroomExternals(ext, scalp, nullptr, std::make_shared<GuideHierarchy>(h));
        ctx.externals = &ext;
        return doc.graph.evaluateStrands(doc.output, ctx);
    };
    const StrandsPtr first = preview();
    ASSERT_TRUE(first);
    const double coldMs = nodeMs(doc, "interpolate");
    BrushSettings brush;
    brush.radius = 0.3f;
    brush.level = 0;
    brush.propagate = true;
    double worst = 0.0, sum = 0.0;
    const std::size_t binds = interpolateBindCount(), searched = interpolateStrandBindCount();
    for (int i = 0; i < 5; ++i) {
        const float x = -0.2f + 0.1f * static_cast<float>(i);
        applyBrushStroke(h, scalp.get(), brush, {{x, 1.2f, 0.0f}, {x + 0.1f, 1.2f, 0.0f}});
        ASSERT_TRUE(preview());
        const double ms = nodeMs(doc, "interpolate");
        worst = std::max(worst, ms);
        sum += ms;
    }
    const std::size_t dabBinds = interpolateBindCount() - binds;
    const std::size_t dabSearched = (interpolateStrandBindCount() - searched) / 5;
    // Guides reshaped with their roots kept (poses, a simulation): the binding carries over.
    double posedSum = 0.0;
    for (int i = 0; i < 5; ++i) {
        auto posed = std::make_shared<GuideHierarchy>(h);
        for (const HierarchyGuide& g : h.guides()) {
            std::vector<Vec3f> points = g.points;
            for (std::size_t k = 1; k < points.size(); ++k) points[k].x += 0.01f * static_cast<float>(i + 1);
            posed->pose(g.id, std::move(points));
        }
        setGroomExternals(ext, scalp, nullptr, posed);
        ctx.externals = &ext;
        ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
        posedSum += nodeMs(doc, "interpolate");
    }
    std::printf("[ budget ] brush dab on 5000 guides, 10%% preview (%zu strands): Interpolate %.1f ms per dab "
                "(worst %.1f; first evaluation %.1f; %zu bindings in 5 dabs, %zu strands searched per dab); posed "
                "guides, roots kept: %.1f ms\n",
                first->strandCount(), sum / 5.0, worst, coldMs, dabBinds, dabSearched, posedSum / 5.0);
}

TEST(Budget, DeformingScalpPointsUpdate) {
    // A scalp of ~58k triangles with UVs, two color sets and a rest pose, moving frame to frame:
    // a mesh built from scratch every frame (triangles checked, UVs and colors hashed, the tree
    // built on the first query) against withPoints (only what the points change; the tree
    // refitted), each followed by 20k closest-point queries near the surface.
    const auto sphere = test::makeSphere(170, 340, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    std::vector<ScalpMesh::UV> uvs(rest->triangleCount() * 3);
    std::vector<ScalpMesh::Color> colors(rest->triangleCount() * 3);
    for (std::size_t c = 0; c < uvs.size(); ++c) {
        uvs[c] = {static_cast<float>(c % 97) / 97.0f, static_cast<float>(c % 89) / 89.0f};
        colors[c] = {0.5f, static_cast<float>(c % 7) / 7.0f, 0.25f, 1.0f};
    }
    rest->setCornerUVs(uvs);
    rest->setCornerColors("mask", colors);
    rest->setCornerColors("tint", colors);
    rest->setRestPoints(sphere->points());
    Rng rng(3);
    std::vector<Vec3f> probes(20000);
    for (Vec3f& p : probes) p = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() * 1.02f;
    auto framePoints = [&](int frame) {
        std::vector<Vec3f> points = rest->points();
        const float s = 1.0f + 0.01f * static_cast<float>(frame);
        for (Vec3f& p : points) p = Vec3f(p.x * s, p.y, p.z * s) + Vec3f(0.01f * static_cast<float>(frame), 0.0f, 0.0f);
        return points;
    };
    auto query = [&](const ScalpMesh& mesh) {
        std::uint64_t h = 0;
        for (const Vec3f& p : probes) h = hashCombine(h, static_cast<std::uint64_t>(mesh.closestPoint(p, 0.1f).root.face + 1));
        return h;
    };
    (void)query(*rest);
    constexpr int kFrames = 5;
    double freshMs = 0.0, freshQueryMs = 0.0, refitMs = 0.0, refitQueryMs = 0.0;
    std::uint64_t freshCheck = 0, refitCheck = 0;
    ScalpMesh previous = *rest;
    for (int frame = 1; frame <= kFrames; ++frame) {
        std::vector<Vec3f> points = framePoints(frame);
        auto t0 = std::chrono::steady_clock::now();
        ScalpMesh fresh(points, rest->triangles());
        fresh.setCornerUVs(uvs);
        fresh.setCornerColors("mask", colors);
        fresh.setCornerColors("tint", colors);
        fresh.setRestPoints(sphere->points());
        freshMs += msSince(t0);
        t0 = std::chrono::steady_clock::now();
        freshCheck = hashCombine(freshCheck, query(fresh));
        freshQueryMs += msSince(t0);

        t0 = std::chrono::steady_clock::now();
        ScalpMesh moved = previous.withPoints(std::move(points));
        refitMs += msSince(t0);
        t0 = std::chrono::steady_clock::now();
        refitCheck = hashCombine(refitCheck, query(moved));
        refitQueryMs += msSince(t0);
        EXPECT_EQ(moved.contentHash(), fresh.contentHash());
        previous = std::move(moved);
    }
    EXPECT_EQ(refitCheck, freshCheck);
    std::printf("[ budget ] deforming scalp of %zu triangles (UVs, 2 color sets, rest): per frame, built from scratch "
                "%.1f ms + first 20k queries %.1f ms (tree built); withPoints %.1f ms + queries %.1f ms (tree refitted)\n",
                rest->triangleCount(), freshMs / kFrames, freshQueryMs / kFrames, refitMs / kFrames, refitQueryMs / kFrames);
}

TEST(Budget, FollowScalpOnADeformingScalp) {
    // 100k strands (Scatter, Interpolate, Clump, Noise, Width) on a deforming scalp
    // with a rest pose, frame after frame through a GroomSession: normally every node re-runs on
    // every frame; following the scalp, the graph runs once on the rest pose and each frame only
    // carries the strands root by root.
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    GroomData data;
    data.graphJson = lookGraph(8000.0, 15);
    data.hierarchyJson = guidesOn(*sphere, 300, 0).toJson();
    data.scalp = rest;
    double perFrame[2] = {0.0, 0.0};
    std::size_t strands = 0;
    for (int follow = 0; follow < 2; ++follow) {
        data.followScalp = follow == 1;
        GroomSession session(data);
        constexpr int kFrames = 5;
        for (int frame = 0; frame <= kFrames; ++frame) {
            const auto scalp = stretchedFrame(*rest, frame + 1);
            const auto t0 = std::chrono::steady_clock::now();
            session.updateScalpPoints(scalp->points());
            const StrandsPtr out = session.evaluate(1.0f, frame / 24.0);
            const double ms = msSince(t0);
            ASSERT_TRUE(out);
            strands = out->strandCount();
            if (frame > 0) perFrame[follow] += ms / kFrames;  // frame 0: the first evaluation
        }
    }
    std::printf("[ budget ] deforming scalp, %zu strands per frame: normal %.1f ms, following the scalp %.1f ms\n", strands,
                perFrame[0], perFrame[1]);
}

TEST(Budget, HierarchyEvaluationOnADeformingScalp) {
    // 1000 primaries with 9 children each (10k guides), evaluated on a moving scalp, a frame at a time.
    const auto sphere = test::makeSphere(60, 120, 1.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    const GuideHierarchy base = guidesOn(*sphere, 1000, 9);
    ASSERT_EQ(base.guides().size(), 10000u);
    double best = 1e30;
    std::uint64_t check = 0;
    for (int frame = 1; frame <= 5; ++frame) {
        const auto scalp = stretchedFrame(*rest, frame);
        GuideHierarchy h = base;
        const auto t0 = std::chrono::steady_clock::now();
        h.evaluate(scalp.get());
        best = std::min(best, msSince(t0));
        check = hashCombine(check, h.computeContentHash());
    }
    std::printf("[ budget ] hierarchy of 10000 guides on a deforming scalp: %.1f ms per frame, best of 5 (check %016llx)\n",
                best, static_cast<unsigned long long>(check));
}

// `count` parts across the top of the unit sphere, each `points` long.
PartSet partsAcross(const ScalpMesh& scalp, int count, std::size_t points) {
    PartSet parts;
    for (int k = 0; k < count; ++k) {
        const float z = -0.5f + static_cast<float>(k) * 0.5f;
        std::vector<Vec3f> line;
        for (std::size_t i = 0; i < points; ++i) {
            const float x = -0.8f + 1.6f * static_cast<float>(i) / static_cast<float>(points - 1);
            line.push_back(Vec3f(x, 1.0f, z).normalized());
        }
        parts.add("part", line, scalp, points);
    }
    return parts;
}

TEST(Budget, PartedInterpolateHalfMillion) {
    // Interpolate respecting 3 parts of 200 points each, 500k strands, 300 guides: the side tests.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    const PartSet parts = partsAcross(*scalp, 3, 200);
    ASSERT_EQ(parts.parts.size(), 3u);
    auto lines = std::make_shared<const PartLines>(parts, *scalp);
    GuideHierarchy h = guidesOn(*scalp, 300, 0);
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Scatter\"";
    graph.replace(graph.find(from), from.size(), from + ", \"params\": {\"density\": 40000.0}");
    GraphDocument doc = loadGraphJson(graph);
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    setGroomExternals(ext, scalp, nullptr, std::make_shared<GuideHierarchy>(h), lines);
    const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(out);
    const double coldMs = nodeMs(doc, "interpolate");
    BrushSettings brush;
    brush.radius = 0.3f;
    applyBrushStroke(h, scalp.get(), brush, {{0.0f, 1.2f, 0.0f}, {0.1f, 1.2f, 0.0f}});
    setGroomExternals(ext, scalp, nullptr, std::make_shared<GuideHierarchy>(h), lines);
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    std::printf("[ budget ] Interpolate with 3 parts x 200 points, %zu strands: %.1f ms (after a dab %.1f ms)\n",
                out->strandCount(), coldMs, nodeMs(doc, "interpolate"));
}

TEST(Budget, InterpolateHalfMillion) {
    // Interpolate alone at 500k strands x 16 points from 500 guides of 4 points along the normals,
    // evenly spread (as the XGen comparison grooms its head: about 9 guides blend into each
    // strand) on a head-sized scalp with a rest pose, best of 3: bound again
    // (a new Interpolate, as a density change binds), a frame of the deforming scalp (the binding
    // kept, roots and guides moved) and a brush dab (some guides combed, the binding kept); on
    // every core, and on one (what the math costs, apart from how it spreads).
    const auto sphere = test::makeSphere(60, 120, 10.0f);
    auto rest = std::make_shared<ScalpMesh>(*sphere);
    rest->setRestPoints(sphere->points());
    {  // regions painted in four colors (a quarter of the sphere each), for regionMap below
        std::vector<ScalpMesh::Color> regions;
        const ScalpMesh::Color colors[4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
        for (const ScalpMesh::Triangle& t : rest->triangles()) {
            for (const std::int32_t v : t) {
                const Vec3f& p = rest->points()[static_cast<std::size_t>(v)];
                regions.push_back(colors[(p.x < 0.0f ? 1 : 0) + (p.z < 0.0f ? 2 : 0)]);
            }
        }
        rest->setCornerColors("regions", std::move(regions));
    }
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    constexpr int kGuides = 500;
    for (int i = 0; i < kGuides; ++i) {  // a Fibonacci lattice: about 1.6 apart
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kGuides);
        const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
        const Vec3f n(r * std::cos(a), y, r * std::sin(a));
        h.addGuide(primary, test::straightCurve(n * 10.0f, n * 15.0f, 3));
    }
    h.evaluate(rest.get());  // rooted once; on every frame the guides follow their roots
    std::string graph = R"({"version": 1, "output": "out",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 398.0}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 15}}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "out", "input": "strands"}]})";
    GraphDocument doc = loadGraphJson(graph);
    // The inputs on a frame: the strands scattered on the scalp there, the guides rooted on it.
    struct Frame {
        std::shared_ptr<const ScalpMesh> scalp;
        Value strands, guides;
    };
    auto frameInputs = [&](int frame, const GuideHierarchy& hierarchy) {
        Frame f;
        f.scalp = frame == 0 ? std::shared_ptr<const ScalpMesh>(rest) : stretchedFrame(*rest, frame);
        auto rooted = std::make_shared<GuideHierarchy>(hierarchy);
        rooted->evaluate(f.scalp.get());
        ExternalInputs ext;
        setGroomExternals(ext, f.scalp, nullptr, rooted);
        EvalContext ctx;
        ctx.externals = &ext;
        f.strands = doc.graph.evaluate(doc.node("scatter"), ctx);
        f.guides = doc.graph.evaluate(doc.node("guides"), ctx);
        return f;
    };
    const Frame still = frameInputs(0, h);
    std::vector<Frame> frames;
    for (int frame = 1; frame <= 3; ++frame) frames.push_back(frameInputs(frame, h));
    std::vector<Frame> dabs;
    {
        GuideHierarchy dabbed = h;
        BrushSettings brush;
        brush.radius = 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float x = -2.0f + 1.0f * static_cast<float>(i);
            applyBrushStroke(dabbed, rest.get(), brush, {{x, 12.0f, 0.0f}, {x + 1.0f, 12.0f, 0.0f}});
            dabs.push_back(frameInputs(0, dabbed));
        }
    }
    const std::size_t strandCount = valueAs<StrandsPtr>(still.strands)->strandCount();
    ExternalInputs none;
    EvalContext ctx;
    ctx.externals = &none;
    auto& reg = OperatorRegistry::instance();
    auto run = [&](const Operator& o, const Frame& f) {
        return valueAs<StrandsPtr>(o.evaluate(ctx, {f.strands, f.guides, Value(MeshPtr(f.scalp))}));
    };
    struct Times {
        double bindMs = 1e9, frameMs = 1e9, dabMs = 1e9;
    };
    bool clearance = false;  // keepClearance (on: measured on its own, below)
    bool regions = false;    // regionMap (on: measured on its own, below)
    auto measure = [&] {
        Times t;
        std::unique_ptr<Operator> kept;
        for (int r = 0; r < 3; ++r) {
            kept = reg.create(op::kInterpolate);
            kept->params().set("keepClearance", clearance);
            kept->params().set("regionMap", std::string(regions ? "regions" : ""));
            const auto t0 = std::chrono::steady_clock::now();
            run(*kept, still);
            t.bindMs = std::min(t.bindMs, msSince(t0));
        }
        for (const Frame& f : frames) {
            const auto t0 = std::chrono::steady_clock::now();
            run(*kept, f);
            t.frameMs = std::min(t.frameMs, msSince(t0));
        }
        for (const Frame& f : dabs) {
            run(*kept, still);  // the binding the dab starts from
            const auto t0 = std::chrono::steady_clock::now();
            run(*kept, f);
            t.dabMs = std::min(t.dabMs, msSince(t0));
        }
        return t;
    };
    // Now, and as first written (the kd-tree for each strand, the blend a point at a time, the
    // points copied first), on every core and on one.
    auto both = [&] {
        const Times now = measure();
        setInterpolateReferencePaths(true);
        const Times first = measure();
        setInterpolateReferencePaths(false);
        return std::make_pair(now, first);
    };
    measure();  // warming up (the threads, the recycled arrays)
    const auto many = both();
#if GROOMLAB_HAS_TBB
    std::pair<Times, Times> one;
    tbb::task_arena single(1);
    single.execute([&] { one = both(); });
#else
    const auto one = many;
#endif
    std::printf("[ budget ] Interpolate, %zu strands x 16 points, 500 guides (as first written): bound again %.1f ms "
                "(%.1f), deforming frame %.1f ms (%.1f), brush dab %.1f ms (%.1f); on one thread: %.0f ms (%.0f), %.0f ms "
                "(%.0f), %.0f ms (%.0f)\n",
                strandCount, many.first.bindMs, many.second.bindMs, many.first.frameMs, many.second.frameMs,
                many.first.dabMs, many.second.dabMs, one.first.bindMs, one.second.bindMs, one.first.frameMs,
                one.second.frameMs, one.first.dabMs, one.second.dabMs);
    // As first written: ~90 ms bound again, ~50 ms a frame.
    EXPECT_BUDGET(many.first.bindMs + many.first.frameMs, 90.0);
    // Keeping clearance (the default): every strand point asked of the scalp's field.
    clearance = true;
    measure();  // (the field's cells made)
    const Times kept = measure();
    std::printf("[ budget ] Interpolate keeping clearance, %zu strands x 16 points: bound again %.1f ms (+%.1f), deforming "
                "frame %.1f ms (+%.1f), brush dab %.1f ms (+%.1f)\n",
                strandCount, kept.bindMs, kept.bindMs - many.first.bindMs, kept.frameMs, kept.frameMs - many.first.frameMs,
                kept.dabMs, kept.dabMs - many.first.dabMs);
    // With a region map of four regions (not keeping clearance, as the first): each strand's color
    // read, each region bound on its own.
    clearance = false;
    regions = true;
    measure();
    const Times regioned = measure();
    std::printf("[ budget ] Interpolate with a region map (4 regions), %zu strands x 16 points: bound again %.1f ms (%+.1f), "
                "deforming frame %.1f ms (%+.1f), brush dab %.1f ms (%+.1f)\n",
                strandCount, regioned.bindMs, regioned.bindMs - many.first.bindMs, regioned.frameMs,
                regioned.frameMs - many.first.frameMs, regioned.dabMs, regioned.dabMs - many.first.dabMs);
}

// The app's head scalp (tests/data/app/head_scalp.obj: v and f lines alone).
std::shared_ptr<const ScalpMesh> headScalp() {
    std::ifstream in(fs::path(GROOMLAB_TEST_DATA) / "app" / "head_scalp.obj");
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) {
            Vec3f p;
            std::sscanf(line.c_str() + 2, "%f %f %f", &p.x, &p.y, &p.z);
            points.push_back(p);
        } else if (line.rfind("f ", 0) == 0) {
            int a = 0, b = 0, c = 0, ta = 0, tb = 0, tc = 0;
            std::sscanf(line.c_str() + 2, "%d/%d %d/%d %d/%d", &a, &ta, &b, &tb, &c, &tc);
            triangles.push_back({a - 1, b - 1, c - 1});
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
}

// The XGen comparison's head (tools/bench/xgen_compare make_app_scene.py make_head): a sphere of
// radius 10, 160 x 100, cut 15% of the radius under its equator, scaled 1, 1.1, 1.2.
std::shared_ptr<const ScalpMesh> benchScalp() {
    const auto sphere = test::makeSphere(100, 160, 10.0f);
    std::vector<Vec3f> points;
    std::vector<std::int32_t> remap(sphere->pointCount(), -1);
    std::vector<ScalpMesh::Triangle> triangles;
    for (const ScalpMesh::Triangle& t : sphere->triangles()) {
        const Vec3f c = (sphere->points()[t[0]] + sphere->points()[t[1]] + sphere->points()[t[2]]) * (1.0f / 3.0f);
        if (c.y < -1.5f) continue;
        ScalpMesh::Triangle kept;
        for (int k = 0; k < 3; ++k) {
            std::int32_t& v = remap[t[k]];
            if (v < 0) {
                const Vec3f p = sphere->points()[t[k]];
                v = static_cast<std::int32_t>(points.size());
                points.push_back({p.x, p.y * 1.1f, p.z * 1.2f});
            }
            kept[k] = v;
        }
        triangles.push_back(kept);
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
}

// One guide per 1.6 cm grid cell of the scalp's vertices (the first vertex in it), straight out
// along the normal 15 cm, 4 points, as the bench's guides.
GuidesPtr benchGuides(const ScalpMesh& scalp) {
    auto guides = std::make_shared<GuideSet>();
    std::set<std::tuple<int, int, int>> cells;
    for (std::size_t v = 0; v < scalp.pointCount(); ++v) {
        const Vec3f& p = scalp.points()[v];
        if (!cells.insert({static_cast<int>(std::floor(p.x / 1.6f)), static_cast<int>(std::floor(p.y / 1.6f)),
                           static_cast<int>(std::floor(p.z / 1.6f))}).second) {
            continue;
        }
        const SurfacePoint at = scalp.closestPoint(p);
        Guide& g = guides->add(guides->size() + 1, test::straightCurve(at.position, at.normal * 15.0f, 3));
        g.root = at.root;
    }
    return guides;
}

TEST(Budget, InterpolateFollowingTheSurface) {
    // Interpolate following the surface and not, on the app's head scalp and the XGen comparison's
    // head with its guides (one per 1.6 cm), at 100k and 500k strands: the first evaluation of a
    // new node (the scalp's graph and the guides' paths kept from the one before: another binding
    // of the same guides; the very first, which makes them, on its own), a brush dab (guides
    // reshaped, roots kept: the binding and paths kept) and a dab that moves 3 guide roots (their
    // paths and the strands near them made again).
    for (const bool bench : {false, true}) {
        const auto scalp = bench ? benchScalp() : headScalp();
        const GuidesPtr guides = benchGuides(*scalp);
        bool cold = true;
        for (const double count : {100000.0, 500000.0}) {
            StrandsPtr outputs[2];  // following the surface and not, the guides as given
            for (const bool followSurface : {true, false}) {
                const std::string graph = R"({"version": 1, "output": "interp", "nodes": [
                    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
                    {"name": "scatter", "type": "Scatter", "params": {"density": )" +
                                          std::to_string(count / scalp->totalArea()) + R"(, "length": 15.0}},
                    {"name": "interp", "type": "Interpolate", "params": {"followSurface": )" +
                                          (followSurface ? std::string("true") : std::string("false")) + R"(}}],
                  "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                                  {"from": "scatter", "to": "interp", "input": "strands"},
                                  {"from": "guides", "to": "interp", "input": "guides"},
                                  {"from": "scalp", "to": "interp", "input": "scalp"}]})";
                double first = 1e9, dab = 1e9, moved = 1e9;
                std::size_t strands = 0;
                for (int run = 0; run < 4; ++run) {
                    GraphDocument doc = loadGraphJson(graph);
                    ExternalInputs ext;
                    EvalContext ctx;
                    ctx.externals = &ext;
                    auto evaluate = [&](const GuidesPtr& g) {
                        setGroomExternals(ext, scalp, g, nullptr);
                        const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
                        strands = out ? out->strandCount() : 0;
                        return nodeMs(doc, "interp");
                    };
                    const double ms = evaluate(guides);
                    outputs[followSurface ? 0 : 1] = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.node("interp")));
                    if (cold) {
                        std::printf("[ budget ] Interpolate on the %s, %zu strands, the very first evaluation (the scalp's graph "
                                    "and the paths made): %.1f ms\n", bench ? "bench head" : "app head", strands, ms);
                        cold = false;
                        continue;
                    }
                    first = std::min(first, ms);
                    // A dab: the guides near the crown bent (roots kept).
                    auto bent = std::make_shared<GuideSet>(*guides);
                    for (Guide& g : bent->guides()) {
                        if (g.points[0].y < 6.0f) continue;
                        for (std::size_t k = 1; k < g.points.size(); ++k) g.points[k].x += 0.5f * static_cast<float>(k);
                    }
                    dab = std::min(dab, evaluate(bent));
                    // Three roots slid along the scalp.
                    auto slid = std::make_shared<GuideSet>(*bent);
                    for (const std::size_t which : {std::size_t{10}, slid->size() / 2, slid->size() - 10}) {
                        Guide& g = slid->guides()[which];
                        const SurfacePoint at = scalp->closestPoint(scalp->position(g.root) + Vec3f(0.4f, 0.0f, 0.3f));
                        for (Vec3f& p : g.points) p += at.position - scalp->position(g.root);
                        g.root = at.root;
                    }
                    moved = std::min(moved, evaluate(slid));
                }
                std::printf("[ budget ] Interpolate on the %s (%zu vertices, %zu guides), %zu strands, followSurface %s: first "
                            "evaluation %.1f ms, dab %.1f ms, roots moved %.1f ms\n",
                            bench ? "bench head" : "app head", scalp->pointCount(), guides->size(), strands,
                            followSurface ? "on " : "off", first, dab, moved);
            }
            // On a smooth head the two are the same: the farthest any point is apart.
            float apart = 0.0f;
            for (std::size_t i = 0; i < outputs[0]->positions().size(); ++i) {
                apart = std::max(apart, (outputs[0]->positions()[i] - outputs[1]->positions()[i]).length());
            }
            std::printf("[ budget ] Interpolate on the %s, %zu strands: following the surface and not, points at most %g apart\n",
                        bench ? "bench head" : "app head", outputs[0]->strandCount(), apart);
        }
    }
}

TEST(Budget, InterpolateKeepingClearanceOnFur) {
    // What keeping clearance costs on a creature's fur (reported only): a body of radius 40 with
    // bumps (a shoulder, a hip, a cheek), 150k strands of 51 points from 1500 guides lying 20
    // degrees off the skin, combed down; the shapes carried over the bumps go into them. A new
    // Interpolate (bound again), best of 3, keeping clearance and not, the scalp's field made first.
    const auto sphere = test::makeSphere(120, 240, 40.0f);
    const Vec3f bumps[3] = {Vec3f(0.6f, 0.5f, 0.6f).normalized(), Vec3f(-0.7f, -0.2f, 0.6f).normalized(),
                            Vec3f(0.1f, 0.3f, -1.0f).normalized()};
    std::vector<Vec3f> points;
    for (const Vec3f& p : sphere->points()) {
        const Vec3f n = p.normalized();
        float r = 40.0f;
        for (const Vec3f& b : bumps) {
            const float a = std::acos(std::clamp(n.dot(b), -1.0f, 1.0f)) / 0.25f;
            r += 6.0f * std::exp(-a * a);
        }
        points.push_back(n * r);
    }
    const auto body = std::make_shared<const ScalpMesh>(points, sphere->triangles());
    auto guides = std::make_shared<GuideSet>();
    constexpr int kGuides = 1500;
    for (int i = 0; i < kGuides; ++i) {  // a Fibonacci lattice
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kGuides);
        const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
        const SurfacePoint at = body->closestPoint(Vec3f(r * std::cos(a), y, r * std::sin(a)) * 40.0f);
        Vec3f down = Vec3f(0.0f, -1.0f, 0.0f) - at.normal * at.normal.y;
        down = down.length() > 1e-3f ? down.normalized() : Vec3f(1.0f, 0.0f, 0.0f);
        const Vec3f dir = (down * std::cos(0.35f) + at.normal * std::sin(0.35f)) * 4.0f;
        Guide& g = guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve(at.position, dir, 5));
        g.root = at.root;
    }
    auto strands = std::make_shared<StrandSet>();
    Rng rng(7);
    for (StrandId id = 1; id <= 150000; ++id) {
        Vec3f n;
        do n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        while (n.length() > 1.0f || n.length() < 0.1f);
        const SurfacePoint at = body->closestPoint(n.normalized() * 40.0f);
        strands->addStrand(id, test::straightCurve(at.position, at.normal * 3.0f, 50), {}, at.root);
    }
    ExternalInputs none;
    EvalContext ctx;
    ctx.externals = &none;
    auto timed = [&](bool keepClearance) {
        double best = 1e9;
        for (int r = 0; r < 4; ++r) {  // (the first makes the field)
            auto op = OperatorRegistry::instance().create(op::kInterpolate);
            op->params().set("keepClearance", keepClearance);
            const auto t0 = std::chrono::steady_clock::now();
            op->evaluate(ctx, {Value(StrandsPtr(strands)), Value(GuidesPtr(guides)), Value(MeshPtr(body))});
            if (r > 0) best = std::min(best, msSince(t0));
        }
        return best;
    };
    const double off = timed(false), on = timed(true);
    std::printf("[ budget ] Interpolate on fur, %zu strands x 51 points, %d guides: %.1f ms, keeping clearance %.1f ms (+%.1f)\n",
                strands->strandCount(), kGuides, off, on, on - off);
}

TEST(Budget, InterpolateKeepingClearanceOnAHead) {
    // What keeping clearance costs on the XGen comparison's head (reported only; tools/bench/
    // xgen_compare: a sphere of radius 10 cut below 15% of it under the equator, scaled 1, 1.1, 1.2,
    // about 17k triangles; guides of 4 points 15 long along the normals, about 1.6 apart; strands of
    // 16 points): 500k and 100k strands, a new Interpolate each time, median of 3. Cold: the
    // process's caches let go first, as Maya's first evaluation of a new groom finds them (the
    // scalp's field made by the walk); warm: the field kept. And what the field holds then.
    const auto sphere = test::makeSphere(100, 160, 10.0f);
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    {
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
    }
    const auto head = std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0, id = 1; i < 700; ++i) {  // a Fibonacci lattice over the whole sphere, those on the head
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 700.0f;
        const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
        if (y < -0.1f) continue;
        const SurfacePoint at = head->closestPoint(Vec3f(r * std::cos(a), y * 1.1f, r * std::sin(a) * 1.2f) * 10.0f);
        if (at.root.face < 0) continue;
        Guide& g = guides->add(static_cast<std::uint64_t>(id++), test::straightCurve(at.position, at.normal * 15.0f, 3));
        g.root = at.root;
    }
    // Scattered as the comparison's graph scatters them (in Scatter's order).
    double area = 0.0;
    for (const ScalpMesh::Triangle& t : head->triangles()) {
        const Vec3f &a = head->points()[static_cast<std::size_t>(t[0])], &b = head->points()[static_cast<std::size_t>(t[1])],
                    &c = head->points()[static_cast<std::size_t>(t[2])];
        area += 0.5 * (b - a).cross(c - a).length();
    }
    auto strandsOf = [&](std::size_t count) {
        char graph[1024];
        std::snprintf(graph, sizeof(graph), R"({"version": 1, "output": "out",
          "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": %f, "length": 15.0}},
            {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 15}}],
          "connections": [
            {"from": "scalp", "to": "scatter", "input": "scalp"},
            {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "out", "input": "strands"}]})",
                      static_cast<double>(count) / area);
        GraphDocument doc = loadGraphJson(graph);
        ExternalInputs ext;
        setGroomExternals(ext, head, nullptr, nullptr);
        EvalContext c;
        c.externals = &ext;
        return valueAs<StrandsPtr>(doc.graph.evaluate(doc.node("scatter"), c));
    };
    ExternalInputs none;
    EvalContext ctx;
    ctx.externals = &none;
    for (const std::size_t count : {std::size_t{500000}, std::size_t{100000}}) {
        const auto strands = strandsOf(count);
        auto timed = [&](bool keepClearance, bool cold, double* bytes) {
            std::vector<double> ms;
            for (int r = 0; r < 4; ++r) {  // (the first warms up)
                if (cold) releaseProcessCaches();
                auto op = OperatorRegistry::instance().create(op::kInterpolate);
                op->params().set("keepClearance", keepClearance);
                const auto t0 = std::chrono::steady_clock::now();
                op->evaluate(ctx, {Value(StrandsPtr(strands)), Value(GuidesPtr(guides)), Value(MeshPtr(head))});
                if (r > 0) ms.push_back(msSince(t0));
            }
            if (bytes) *bytes = static_cast<double>(processCacheBytes()) / (1024.0 * 1024.0);
            std::sort(ms.begin(), ms.end());
            return ms[ms.size() / 2];
        };
        // Cold against not keeping clearance with the caches let go too (the arrays they pooled
        // made again); warm against it with them kept. The field's bytes: the caches' difference.
        double offBytes = 0.0, coldBytes = 0.0;
        const double offCold = timed(false, true, &offBytes), cold = timed(true, true, &coldBytes);
        const double offWarm = timed(false, false, nullptr), warm = timed(true, false, nullptr);
        std::printf("[ budget ] Interpolate on a head, %zu strands x 16 points, %zu guides: keeping clearance cold %.1f ms (+%.1f), "
                    "warm %.1f ms (+%.1f); the scalp's field %.1f MB\n",
                    strands->strandCount(), guides->size(), cold, cold - offCold, warm, warm - offWarm, coldBytes - offBytes);
    }
}

TEST(Budget, PointIndexMillion) {
    // Build a kd-tree over 1M points, then 1M nearest and 1M 8-nearest queries (parallel).
    constexpr std::size_t kPoints = 1000000;
    std::vector<Vec3f> points(kPoints), queries(kPoints);
    Rng rng(7);
    for (Vec3f& p : points) p = Vec3f(rng.nextFloat(), rng.nextFloat(), rng.nextFloat());
    for (Vec3f& q : queries) q = Vec3f(rng.nextFloat(), rng.nextFloat(), rng.nextFloat());
    double buildMs = 1e30, nearestMs = 1e30, kMs = 1e30;
    std::uint64_t check = 0;
    for (int run = 0; run < 3; ++run) {
        auto t0 = std::chrono::steady_clock::now();
        const PointIndex index(points);
        buildMs = std::min(buildMs, msSince(t0));
        std::vector<std::size_t> nearest(kPoints);
        t0 = std::chrono::steady_clock::now();
        parallelFor(0, kPoints, [&](std::size_t i) { nearest[i] = index.nearest(queries[i]); });
        nearestMs = std::min(nearestMs, msSince(t0));
        std::vector<std::size_t> eight(kPoints * 8);
        t0 = std::chrono::steady_clock::now();
        parallelFor(0, kPoints, [&](std::size_t i) {
            float d2[8];
            index.kNearest(queries[i], 8, eight.data() + i * 8, d2);
        });
        kMs = std::min(kMs, msSince(t0));
        check = 0;
        for (std::size_t i = 0; i < kPoints; i += 997) check = hashCombine(hashCombine(check, nearest[i]), eight[i * 8 + 7]);
    }
    std::printf("[ budget ] PointIndex over 1M points: build %.1f ms, 1M nearest %.1f ms, 1M 8-nearest %.1f ms "
                "(check %016llx)\n",
                buildMs, nearestMs, kMs, static_cast<unsigned long long>(check));
}

// A card graph whose Cards node keeps at most `maxCards` cards: one LOD of a game export.
std::string lodGraph(int maxCards) {
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer), R"({"version": 1, "output": "out",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 4000.0, "randomness": 0.5}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"count": 300, "strength": 0.7}},
        {"name": "cards", "type": "Cards", "params": {"maxCards": %d, "segments": 6}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 11}}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "interp", "input": "strands"},
        {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"},
        {"from": "interp", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "cards", "input": "strands"},
        {"from": "cards", "to": "out", "input": "strands"}]})",
                  maxCards);
    return buffer;
}

TEST(Budget, MultiLodCardEvaluation) {
    // Three LODs of a card export (50k strands, cards at 300, 150 and 60): each through a Groom of its own.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.hierarchyJson = guidesOn(*scalp, 300, 0).toJson();
    const int lods[] = {300, 150, 60};
    auto t0 = std::chrono::steady_clock::now();
    std::size_t cards = 0;
    for (const int maxCards : lods) {
        data.graphJson = lodGraph(maxCards);
        Groom groom(data);
        const StrandsPtr out = groom.evaluate(1.0f, 0.0, true);
        ASSERT_TRUE(out);
        cards += out->cardLayers().empty() ? 0 : out->cardLayers()[0]->cards.size();
    }
    const double freshMs = msSince(t0);
    // The same LODs through one Groom, swapping graphs: only Cards and the output run again.
    t0 = std::chrono::steady_clock::now();
    std::size_t swappedCards = 0, evaluations = 0;
    {
        data.graphJson = lodGraph(lods[0]);
        Groom groom(data);
        for (const int maxCards : lods) {
            if (maxCards != lods[0]) groom.setGraphJson(lodGraph(maxCards));
            const StrandsPtr out = groom.evaluate(1.0f, 0.0, true);
            ASSERT_TRUE(out);
            swappedCards += out->cardLayers().empty() ? 0 : out->cardLayers()[0]->cards.size();
            evaluations += groom.evaluationCount();
        }
    }
    const double swappedMs = msSince(t0);
    EXPECT_EQ(swappedCards, cards);
    std::printf("[ budget ] 3 card LODs through fresh Grooms: %.0f ms (%zu cards); through one Groom with "
                "setGraphJson: %.0f ms (%zu node runs)\n",
                freshMs, cards, swappedMs, evaluations);
}

TEST(Budget, DisplayLitStrandsHalfMillion) {
    // The viewport's lit strands at 500k strands x 16 points after an evaluation: the builder's
    // update, then the streams written into a host's buffers (StrandLines, as Maya's override
    // writes them into its GPU buffers), against building them into vectors (what a host that
    // copies would have).
    constexpr std::size_t kStrands = 500000, kPoints = 16;
    auto strands = std::make_shared<StrandSet>();
    strands->reserve(kStrands, kStrands * kPoints);
    Rng rng(11);
    std::vector<Vec3f> points(kPoints);
    for (std::size_t i = 0; i < kStrands; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        for (std::size_t k = 0; k < kPoints; ++k) {
            points[k] = n * (1.0f + 0.02f * static_cast<float>(k)) + Vec3f(0.003f * std::sin(static_cast<float>(i + k)), 0.0f, 0.0f);
        }
        strands->addStrand(static_cast<StrandId>(i), points);
    }
    DisplayInputs in;
    in.strands = strands;
    in.strandsVersion = 1;
    const DisplaySettings settings;
    DisplayBuilder writer;
    writer.setRetain(false);
    writer.setWriteLines(true);
    auto t0 = std::chrono::steady_clock::now();
    const DisplayFrame& frame = writer.update(in, settings);
    const double buildMs = msSince(t0);
    const DisplayGeometry& lines = *frame.item(DisplayItemId::Strands).geometry;
    ASSERT_TRUE(lines.lines && lines.lit());
    const std::size_t n = lines.vertexCount();
    std::vector<float> positions(n * 3), normals(n * 3), tangents(n * 3), uvs(n);
    std::vector<std::uint32_t> indices(lines.indexCount());
    // Touch the buffers first: a host's GPU buffers are mapped memory, not fresh pages.
    for (auto* v : {&positions, &normals, &tangents, &uvs}) std::fill(v->begin(), v->end(), 0.0f);
    std::fill(indices.begin(), indices.end(), 0u);
    t0 = std::chrono::steady_clock::now();
    lines.lines->writePositions(positions.data());
    lines.lines->writeFrames(normals.data(), tangents.data());
    const double writeMs = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    lines.lines->writeIndices(indices.data());
    lines.lines->writeUvs(uvs.data());
    const double layoutMs = msSince(t0);
    // Into vectors, as before.
    DisplayBuilder vectors;
    vectors.setRetain(false);
    t0 = std::chrono::steady_clock::now();
    const DisplayGeometry& built = *vectors.update(in, settings).item(DisplayItemId::Strands).geometry;
    const double vectorsMs = msSince(t0);
    EXPECT_EQ(built.positions, positions);
    EXPECT_EQ(built.normals, normals);
    EXPECT_EQ(built.tangents, tangents);
    EXPECT_TRUE(built.bitangents.empty());  // the host's cross product
    EXPECT_EQ(built.uvFloats, 1u);
    EXPECT_EQ(built.uvs, uvs);
    EXPECT_EQ(built.indices, indices);
    std::printf("[ budget ] lit strands, %zu points: update %.1f ms, positions + frames written %.1f ms (indices + uvs, "
                "kept while the point counts stay: %.1f ms); built into vectors instead: %.1f ms\n",
                n, buildMs, writeMs, layoutMs, vectorsMs);
    EXPECT_BUDGET(buildMs + writeMs, 100.0);
}

TEST(Budget, LookOperatorsHalfMillion) {
    // The operators a look tweak runs again at 500k strands x 16 points (the XGen comparison's
    // graph: Noise 0.5 at frequency 1, Curl 0.3 at 0.5 turns, Width): each node's time, best of
    // 3, and what the copy of the positions alone costs.
    constexpr std::size_t kStrands = 500000, kPoints = 16;
    auto strands = std::make_shared<StrandSet>();
    strands->reserve(kStrands, kStrands * kPoints);
    Rng rng(12);
    std::vector<Vec3f> points(kPoints);
    for (std::size_t i = 0; i < kStrands; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        for (std::size_t k = 0; k < kPoints; ++k) points[k] = n * (10.0f + static_cast<float>(k));
        strands->addStrand(static_cast<StrandId>(i), points);
    }
    EvalContext ctx;
    auto best = [](auto&& fn) {
        double ms = 1e9;
        for (int r = 0; r < 3; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            ms = std::min(ms, msSince(t0));
        }
        return ms;
    };
    const double copyMs = best([&] {
        StrandSet copy(*strands);
        copy.mutablePositions();
    });
    auto& reg = OperatorRegistry::instance();
    auto noise = reg.create(op::kNoise);
    noise->params().set("amplitude", 0.5);
    noise->params().set("frequency", 1.0);
    auto curl = reg.create(op::kCurl);
    curl->params().set("radius", 0.3);
    curl->params().set("frequency", 0.5);
    curl->params().set("tolerance", 0.0);  // (7.5 turns on 16 points: the points it is given, as the bench graph's)
    auto width = reg.create(op::kWidth);
    width->params().set("width", 0.01);
    auto run = [&](const Operator& o, const StrandsPtr& in) {
        return valueAs<StrandsPtr>(o.evaluate(ctx, {Value(in)}));
    };
    StrandsPtr noised, curled;
    const double noiseMs = best([&] { noised = run(*noise, strands); });
    const double curlMs = best([&] { curled = run(*curl, noised); });
    const double widthMs = best([&] { run(*width, curled); });
    auto worldNoise = reg.create(op::kNoise);
    worldNoise->params().set("amplitude", 0.5);
    worldNoise->params().set("space", std::string("world"));
    const double worldMs = best([&] { run(*worldNoise, strands); });
    // Curl at its default tolerance: points added to every strand (each segment cut in two or more),
    // reported only.
    auto adding = reg.create(op::kCurl);
    adding->params().set("radius", 0.3);
    adding->params().set("frequency", 0.5);
    const double addingMs = best([&] { run(*adding, noised); });
    std::printf("[ budget ] look operators, %zu strands x %zu points: noise %.1f ms (world space %.1f ms), curl %.1f ms, "
                "width %.1f ms (the positions' copy-on-write clone alone: %.1f ms); curl adding points (tolerance 0.5) %.1f ms\n",
                kStrands, kPoints, noiseMs, worldMs, curlMs, widthMs, copyMs, addingMs);
    // Before the strand-space noise cache, four-lane frames and recycled arrays: ~150 ms.
    EXPECT_BUDGET(noiseMs + curlMs + widthMs, 120.0);

    // What a node that evaluated in full tells the nodes after it (Graph::setDiffFullResults):
    // its result against its last one, strand by strand, with 1% of the strands moved.
    StrandSet moved(*strands);
    for (std::size_t s = 0; s < kStrands; s += 100) moved.strandPoints(s)[kPoints - 1] += Vec3f(0.01f, 0.0f, 0.0f);
    const StrandsPtr after = std::make_shared<StrandSet>(std::move(moved));
    ChangesPtr changes;
    const double diffMs = best([&] { changes = strandChangesBetween(strands, after); });
    ASSERT_TRUE(changes);
    EXPECT_EQ(changes->strands.size(), kStrands / 100);
    // And with its widths and a channel written anew too (nothing shared but the layout).
    StrandSet rewritten(*after);
    rewritten.mutableWidths();
    rewritten.addStrandChannel("clumpId", 1.0f);
    StrandSet before(*strands);
    before.addStrandChannel("clumpId", 1.0f);
    const StrandsPtr a = std::make_shared<StrandSet>(std::move(before)), b = std::make_shared<StrandSet>(std::move(rewritten));
    const double diffAllMs = best([&] { changes = strandChangesBetween(a, b); });
    std::printf("[ budget ] a full result against the last, %zu strands x %zu points, 1%% moved: %.1f ms (points, widths "
                "and a channel apart: %.1f ms)\n", kStrands, kPoints, diffMs, diffAllMs);
}

TEST(Budget, ClumpStrengthTweak) {
    // A Clump strength tweak (the node evaluates in full) at 100k and 500k strands x 16 points with
    // 400 centers: what each run takes, median of 5 alternating strengths.
    for (const std::size_t count : {std::size_t{100000}, std::size_t{500000}}) {
        constexpr std::size_t kPoints = 16;
        auto strands = std::make_shared<StrandSet>();
        strands->reserve(count, count * kPoints);
        Rng rng(31);
        std::vector<Vec3f> points(kPoints);
        auto centers = std::make_shared<GuideSet>();
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
            for (std::size_t k = 0; k < kPoints; ++k) points[k] = n * (10.0f + 0.3f * static_cast<float>(k));
            strands->addStrand(static_cast<StrandId>(i), points);
            if (i % (count / 400) == 0) centers->add(static_cast<StrandId>(i), points);
        }
        auto clump = OperatorRegistry::instance().create(op::kClump);
        clump->params().set("centersFrom", std::string("inputPoints"));
        EvalContext ctx;
        const std::vector<Value> inputs{Value(StrandsPtr(strands)), Value(GuidesPtr(centers))};
        clump->evaluate(ctx, inputs);  // the first run
        std::vector<double> times;
        for (int r = 0; r < 5; ++r) {
            clump->params().set("strength", r % 2 ? 0.7 : 0.75);
            const auto t0 = std::chrono::steady_clock::now();
            clump->evaluate(ctx, inputs);
            times.push_back(msSince(t0));
        }
        std::sort(times.begin(), times.end());
        std::printf("[ budget ] Clump strength tweak, %zu strands x %zu points, %zu centers: %.1f ms\n", count, kPoints,
                    centers->size(), times[2]);
        // The same with the Clump picking its own centers (centersFrom pick): the pick is kept
        // across the tweak (ClumpOp::pickKey), so it costs what the centers input does.
        auto own = OperatorRegistry::instance().create(op::kClump);
        own->params().set("count", std::int64_t{400});
        const std::vector<Value> alone{Value(StrandsPtr(strands)), Value()};
        const auto t0 = std::chrono::steady_clock::now();
        own->evaluate(ctx, alone);  // the first run: the pick itself
        const double picked = msSince(t0);
        times.clear();
        for (int r = 0; r < 5; ++r) {
            own->params().set("strength", r % 2 ? 0.7 : 0.75);
            const auto t1 = std::chrono::steady_clock::now();
            own->evaluate(ctx, alone);
            times.push_back(msSince(t1));
        }
        std::sort(times.begin(), times.end());
        std::printf("[ budget ] Clump strength tweak, own centers, %zu strands x %zu points: %.1f ms (the first run, picking: %.1f ms)\n",
                    count, kPoints, times[2], picked);
    }
}

TEST(Budget, PreviewDensityHalfMillion) {
    // What a preview costs of a look graph whose render holds 500k strands of 16 points, at 10% and
    // 2%: a whole evaluation from nothing (median of 3), a Noise tweak (median of 5) and a Clump
    // strength tweak (median of 5); with Scatter's randomness 1 (the default) and 0.5 (relaxed
    // roots: the first evaluation relaxes them, kept after). Reported only.
    const auto scalp = test::makeSphere(60, 120, 1.0f);
    auto hierarchy = std::make_shared<GuideHierarchy>(guidesOn(*scalp, 300, 0));
    ExternalInputs ext;
    setGroomExternals(ext, scalp, nullptr, hierarchy);
    for (const double randomness : {1.0, 0.5}) {
        std::string graph = lookGraph(40000.0, 15);  // 500k strands at full density
        const std::string scatter = "\"density\": 40000.000000";
        graph.replace(graph.find(scatter), scatter.size(), scatter + ", \"randomness\": " + std::to_string(randomness));
        for (const float density : {0.1f, 0.02f}) {
            EvalContext ctx;
            ctx.previewDensity = density;
            ctx.externals = &ext;
            std::vector<double> cold, noise, clump;
            std::size_t shown = 0;
            for (int r = 0; r < 3; ++r) {
                GraphDocument doc = loadGraphJson(graph);
                const auto t0 = std::chrono::steady_clock::now();
                shown = doc.graph.evaluateStrands(doc.output, ctx)->strandCount();
                cold.push_back(msSince(t0));
                if (r > 0) continue;
                for (int i = 0; i < 5; ++i) {
                    doc.graph.node(doc.node("noise")).params().set("amplitude", 0.01 + 0.01 * i);
                    const auto t1 = std::chrono::steady_clock::now();
                    doc.graph.evaluateStrands(doc.output, ctx);
                    noise.push_back(msSince(t1));
                    doc.graph.node(doc.node("clump")).params().set("strength", 0.6 + 0.02 * i);
                    const auto t2 = std::chrono::steady_clock::now();
                    doc.graph.evaluateStrands(doc.output, ctx);
                    clump.push_back(msSince(t2));
                }
            }
            for (auto* v : {&cold, &noise, &clump}) std::sort(v->begin(), v->end());
            std::printf("[ budget ] preview of 500k strands at %.0f%% (randomness %.1f, %zu shown): from nothing %.1f ms, "
                        "noise tweak %.1f ms, clump tweak %.1f ms\n",
                        density * 100.0f, randomness, shown, cold[1], noise[2], clump[2]);
        }
    }
}

}  // namespace
}  // namespace groomlab
