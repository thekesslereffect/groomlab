// SPDX-License-Identifier: Apache-2.0
// The interactive brush at production strand counts, timed as an artist feels it: a stroke of
// dabs through the core's BrushTool on a GroomSession (as Maya's brush context drives it), each
// dab followed by the view's evaluation (the shape's ViewRequest: previewDensity times
// sculptDensity while the stroke is live) and the viewport's strand lines written into a host's
// buffers (DisplayBuilder's StrandLines, as Maya's draw override writes its GPU buffers). Reports
// the press (the stroke's first dab), the dabs after it and the release (the commit and the
// evaluation back at the full density). Budgets report only, like test_budgets.cpp's.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#undef near  // (windows.h's; names in the headers below)
#undef far
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "groomlab/display.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Whether the timings gate (test_budgets.cpp's rule: locally yes, on CI no, GROOMLAB_ENFORCE_BUDGETS
// either way).
bool enforced() {
    if (const char* force = std::getenv("GROOMLAB_ENFORCE_BUDGETS")) return *force && std::string(force) != "0";
    return std::getenv("CI") == nullptr;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The process's private bytes (0 where not known): what holding something costs.
double privateMegabytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return static_cast<double>(counters.PrivateUsage) / 1e6;
    }
#endif
    return 0.0;
}

double median(std::vector<double> xs) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    const std::size_t n = xs.size();
    return n % 2 ? xs[n / 2] : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

// The XGen comparison's graph (tools/bench/xgen_compare/xgc_common.py groomlab_graph): Scatter ->
// Interpolate -> Clump (400 centers of its own) -> Noise -> Curl -> Width -> Resample (even, 15 segments).
std::string benchGraph(double density) {
    char buffer[4096];
    std::snprintf(buffer, sizeof(buffer), R"({"version": 1, "output": "output",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": %f, "length": 15.0}},
        {"name": "interpolate", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"count": 400, "strength": 0.8}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.5, "frequency": 1.0}},
        {"name": "curl", "type": "Curl", "params": {"radius": 0.3, "frequency": 0.5, "tolerance": 0.0}},
        {"name": "width", "type": "Width", "params": {"width": 0.01}},
        {"name": "resample", "type": "Resample", "params": {"segments": 15, "mode": "even"}},
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "noise", "input": "strands"},
        {"from": "noise", "to": "curl", "input": "strands"},
        {"from": "curl", "to": "width", "input": "strands"},
        {"from": "width", "to": "resample", "input": "strands"},
        {"from": "resample", "to": "output", "input": "strands"}]})",
                  density);
    return buffer;
}

// What a bench graph has between Interpolate and the clumps: nothing, a Collide (its default
// method, the signed distance field), a Collide asking the mesh at every point (exact), or one
// bypassed.
enum class Collider { None, Collide, Exact, Bypassed };

// The bench graph with a Collide against the scalp after Interpolate, with the app's head graph's
// settings (tests/data/app/head_graph.json: a deep search, every point near the scalp queried),
// or that node bypassed.
std::string benchGraph(double density, Collider collider) {
    std::string graph = benchGraph(density);
    if (collider == Collider::None) return graph;
    auto replace = [&](const std::string& from, const std::string& to) {
        const std::size_t at = graph.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) graph.replace(at, from.size(), to);
    };
    replace(R"({"name": "interpolate", "type": "Interpolate"},)",
            std::string(R"({"name": "interpolate", "type": "Interpolate"}, {"name": "collide", "type": "Collide", )") +
                (collider == Collider::Bypassed ? R"("bypass": true, )" : "") +
                (collider == Collider::Exact ? R"("params": {"offset": 0.08, "maxDepth": 30.0, "iterations": 3, "method": "exact"}},)"
                                             : R"("params": {"offset": 0.08, "maxDepth": 30.0, "iterations": 3}},)"));
    replace(R"({"from": "interpolate", "to": "clump", "input": "strands"},)",
            R"({"from": "interpolate", "to": "collide", "input": "strands"}, {"from": "scalp", "to": "collide", "input": "scalp"},
        {"from": "collide", "to": "clump", "input": "strands"},)");
    return graph;
}

// `count` guides of 4 points along the normals of a radius-10 sphere, evenly spread (a Fibonacci
// lattice, about 1.6 apart: the XGen comparison's head has ~350 of them), rooted on it.
GuideHierarchy latticeGuides(const ScalpMesh& scalp, int count) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    for (int i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
        const Vec3f n(r * std::cos(a), y, r * std::sin(a));
        h.addGuide(primary, test::straightCurve(n * 10.0f, n * 15.0f, 3));
    }
    h.evaluate(&scalp);
    return h;
}

// The viewport's side of a redraw: the builder's update, then every strand-lines item it built
// written into host buffers, as GroomSubSceneOverride::updateItem writes its GPU buffers: only the
// changed strands' vertex ranges when the item holds the strands they changed from
// (DisplayItem::updated), else the positions (and frames when lit) whole, the indices and uvs
// when the point counts changed; an item kept hidden keeps its buffers.
class HostViewport {
public:
    HostViewport() {
        builder_.setRetain(false);
        builder_.setWriteLines(true);
    }
    double draw(const ViewResult& view, bool stroke, std::uint64_t version) {
        const auto t0 = std::chrono::steady_clock::now();
        DisplayInputs in = DisplayInputs::fromView(view, stroke);
        in.strandsVersion = version;
        in.hierarchyVersion = version;
        const DisplayFrame& frame = builder_.update(in, settings_);
        for (const DisplayItem& item : frame.items) {
            if (!item.rebuilt || item.keep || !item.geometry || !item.geometry->lines) continue;
            const StrandLines& lines = *item.geometry->lines;
            Buffers& b = buffers_[item.name];
            const std::size_t n = lines.vertexCount();
            if (item.updated && b.layout == lines.layoutKey() && b.positions.size() == n * 3) {
                const StrandSet& strands = *lines.strands;
                for (const auto& [first, end] : strandRuns(item.updated->strands)) {
                    const std::size_t at = strands.strandBegin(first) * 3;
                    lines.writePositions(b.positions.data() + at, first, end);
                    if (lines.lit) lines.writeFrames(b.normals.data() + at, b.tangents.data() + at, first, end);
                }
                continue;
            }
            if (b.positions.size() != n * 3) {
                for (auto* v : {&b.positions, &b.normals, &b.tangents}) v->assign(n * 3, 0.0f);
                b.uvs.assign(n, 0.0f);
                b.indices.assign(lines.indexCount(), 0u);
                b.layout = 0;
            }
            lines.writePositions(b.positions.data());
            if (b.layout != lines.layoutKey()) {
                if (b.indices.size() != lines.indexCount()) b.indices.assign(lines.indexCount(), 0u);
                lines.writeIndices(b.indices.data());
                if (lines.lit) lines.writeUvs(b.uvs.data());
                b.layout = lines.layoutKey();
            }
            if (lines.lit) lines.writeFrames(b.normals.data(), b.tangents.data());
        }
        return msSince(t0);
    }

private:
    struct Buffers {
        std::vector<float> positions, normals, tangents, uvs;
        std::vector<std::uint32_t> indices;
        std::uint64_t layout = 0;
    };
    DisplayBuilder builder_;
    DisplaySettings settings_;
    std::map<std::string, Buffers> buffers_;
};

// Each node's time as it last ran, for the report.
std::string nodeTimes(const GroomSession& session) {
    std::string out;
    for (const auto& [name, ms] : session.nodeTimes()) {
        char one[96];
        std::snprintf(one, sizeof(one), "%s%s %.1f", out.empty() ? "" : ", ", name.c_str(), ms);
        out += one;
    }
    return out;
}

struct StrokeTimes {
    std::size_t strands = 0, sculptStrands = 0;
    double firstMs = 0.0;              // the groom's first evaluation and drawing
    std::vector<double> pressMs;       // per stroke: its first dab (stamp, evaluation, drawing)
    std::vector<double> dabMs;         // the dabs after the first, every stroke
    std::vector<double> dabEvalMs;     // ... their evaluations alone
    std::vector<double> dabDrawMs;     // ... their drawing alone
    std::vector<double> dabChanged;    // ... the share of the strands each changed (-1: not known)
    std::vector<double> releaseMs;     // per stroke: the commit, the evaluation at full density, drawing
    std::vector<double> commitMs;      // ... the commit alone (the tool's end: the stored texts set)
    std::vector<double> releaseEvalMs; // ... the evaluation alone
    std::vector<double> releaseDrawMs; // ... the drawing alone
    std::vector<double> releaseChanged;  // ... the share of the strands it changed (-1: not known)
    std::vector<std::size_t> dabIncremental;  // ... the nodes each ran incrementally
    std::vector<double> dabCollideMs;  // ... the Collide node's time in each (a graph with one)
    std::string dabNodes;              // the last dab's node times
    std::string releaseNodes;          // the last release's node times
    std::vector<double> warmMs;        // warmed: the stroke document evaluated ahead, first and after each release
    double warmMegabytes = 0.0;        // ... the private bytes the first warm-up took
};

// The groom a stroke is timed on, and where on it the strokes start (world space).
struct Bench {
    GroomData data;
    Vec3f strokeAt{0.0f, 10.5f, 5.0f};
    // The camera, looking at strokeAt from `eye` (aimed; else the benchmark's own, below).
    bool aimed = false;
    Vec3f eye;
};

// The bench sphere (radius 10) with benchGraph's nodes at `count` strands and 500 lattice guides.
Bench sphereBench(std::size_t count, Collider collider = Collider::None) {
    Bench bench;
    const auto scalp = test::makeSphere(60, 120, 10.0f);
    bench.data.scalp = scalp;
    bench.data.graphJson = benchGraph(static_cast<double>(count) / (4.0 * 3.14159265358979 * 100.0), collider);
    bench.data.hierarchyJson = latticeGuides(*scalp, 500).toJson();
    return bench;
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The points and triangles of an OBJ of triangles ("v x y z", "f a/t b/t c/t").
std::shared_ptr<const ScalpMesh> readObj(const std::string& path) {
    std::istringstream in(readText(path));
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "v") {
            Vec3f p;
            words >> p.x >> p.y >> p.z;
            points.push_back(p);
        } else if (kind == "f") {
            ScalpMesh::Triangle t{};
            std::string corner;
            for (int k = 0; k < 3 && words >> corner; ++k) t[k] = std::stoi(corner.substr(0, corner.find('/'))) - 1;
            triangles.push_back(t);
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
}

// The app's lookdev head (tests/data/app: head_scalp.obj, an ellipsoid cap about 16 cm across,
// and head_graph.json: strands combed along the scalp and down, kept out of it by a Collide with
// a deep search, clumped, noised, colored, tapered) at `count` strands, with 500 guides 12 cm
// long along its normals; its Collide as `collider` says.
Bench headBench(std::size_t count, Collider collider) {
    Bench bench;
    const auto scalp = readObj(std::string(GROOMLAB_TEST_DATA) + "/app/head_scalp.obj");
    bench.data.scalp = scalp;
    double area = 0.0;
    for (const auto& t : scalp->triangles()) {
        const Vec3f& a = scalp->points()[static_cast<std::size_t>(t[0])];
        area += 0.5 * (scalp->points()[static_cast<std::size_t>(t[1])] - a).cross(scalp->points()[static_cast<std::size_t>(t[2])] - a).length();
    }
    std::string graph = readText(std::string(GROOMLAB_TEST_DATA) + "/app/head_graph.json");
    auto replace = [&](const std::string& from, const std::string& to) {
        const std::size_t at = graph.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) graph.replace(at, from.size(), to);
    };
    char density[64];
    std::snprintf(density, sizeof(density), "\"density\": %f", static_cast<double>(count) / area);
    replace("\"density\": 175.0", density);
    if (collider == Collider::Bypassed) replace(R"("type": "Collide",)", R"("type": "Collide", "bypass": true,)");
    if (collider == Collider::Exact) replace(R"("iterations": 3})", R"("iterations": 3, "method": "exact"})");
    bench.data.graphJson = graph;
    // Guides from every so many triangles' middles, along their normals (outward: from the
    // ellipsoid's center, 2 cm below the origin).
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    const std::size_t step = std::max<std::size_t>(1, scalp->triangles().size() / 500);
    for (std::size_t f = 0; f < scalp->triangles().size(); f += step) {
        const auto& t = scalp->triangles()[f];
        const Vec3f& a = scalp->points()[static_cast<std::size_t>(t[0])];
        const Vec3f& b = scalp->points()[static_cast<std::size_t>(t[1])];
        const Vec3f& c = scalp->points()[static_cast<std::size_t>(t[2])];
        Vec3f n = (b - a).cross(c - a);
        if (n.length() < 1e-6f) continue;  // the crown's degenerate triangles
        const Vec3f middle = (a + b + c) / 3.0f;
        n = n.normalized();
        if (n.dot(middle - Vec3f(0.0f, -2.0f, 0.0f)) < 0.0f) n = n * -1.0f;
        h.addGuide(primary, test::straightCurve(middle, n * 12.0f, 3));
    }
    h.evaluate(scalp.get());
    bench.data.hierarchyJson = h.toJson();
    bench.strokeAt = Vec3f(0.0f, 7.0f, 3.9f);  // over the brow
    return bench;
}

// Strokes of `dabs` points (a press and drags 10 pixels apart, as the GUI benchmark's) through
// BrushTool on a session of `bench` at previewDensity 1 and `sculptDensity`, each dab evaluated
// and drawn. `warm`: the stroke document evaluated ahead, after the groom's first evaluation and
// each release, as a host does in idle time while its brush tool is up.
StrokeTimes strokeLatency(Bench bench, float sculptDensity, int strokes, int dabs, bool warm = false) {
    GroomSession session(std::move(bench.data));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.density = 1.0f;
    request.sculptDensity = sculptDensity;
    request.pointBudget = 16.0e6;  // a 16 M point preview budget (groomLabShape's previewPointBudget)
    HostViewport viewport;
    std::uint64_t version = 0;
    double changed = -1.0;  // the last evaluation's changed share
    auto evaluateAndDraw = [&](double* evalMs, double* drawMs) {
        const auto t0 = std::chrono::steady_clock::now();
        const ViewResult& view = session.evaluate(request);
        EXPECT_TRUE(view.errors.empty()) << (view.errors.empty() ? "" : view.errors[0]);
        const double e = msSince(t0);
        const double d = viewport.draw(view, session.hasPreview(), ++version);
        if (evalMs) *evalMs = e;
        if (drawMs) *drawMs = d;
        changed = view.strandsChanges && !view.strandsChanges->all && view.strands
                      ? static_cast<double>(view.strandsChanges->strands.size()) / static_cast<double>(view.strands->strandCount())
                      : -1.0;
        return view.strands ? view.strands->strandCount() : 0;
    };

    StrokeTimes out;
    auto t0 = std::chrono::steady_clock::now();
    out.strands = evaluateAndDraw(nullptr, nullptr);
    out.firstMs = msSince(t0);
    auto warmUp = [&] {
        if (!warm) return;
        const double before = privateMegabytes();
        const auto started = std::chrono::steady_clock::now();
        session.warmStroke(request);
        out.warmMs.push_back(msSince(started));
        if (out.warmMs.size() == 1) out.warmMegabytes = privateMegabytes() - before;
    };
    warmUp();

    // The benchmark's camera (xgc_common.set_camera: at (0, 22, 62) pitched 18 degrees down, a
    // 35 mm lens, 1600 x 1000) and its stroke, from above the front of the head; each stroke a
    // little further along.
    const float pitch = 18.0f * 3.14159265f / 180.0f;
    const Vec3f eye{0.0f, 22.0f, 62.0f};
    const double verticalFov = 2.0 * std::atan(std::tan(0.5 * 2.0 * std::atan(36.0 / 70.0)) * 1000.0 / 1600.0) * 180.0 / 3.14159265;
    const ToolView view = ToolView::fromMatrix(
        bench.aimed ? test::lookAtProjection(bench.eye, bench.strokeAt, verticalFov, 1.6)
                    : test::lookAtProjection(eye, eye + Vec3f(0.0f, -std::sin(pitch), -std::cos(pitch)) * 60.0f, verticalFov, 1.6),
        1600, 1000);
    float x0 = 0.0f, y0 = 0.0f;
    EXPECT_TRUE(view.project(bench.strokeAt, x0, y0));
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 120.0f;
    tool.settings.brush.strength = 1.0f;
    for (int stroke = 0; stroke < strokes; ++stroke) {
        const float sx = x0 - 40.0f + 4.0f * static_cast<float>(stroke);
        t0 = std::chrono::steady_clock::now();
        EXPECT_TRUE(tool.begin(host, view, sx, y0, 1.0f));
        out.sculptStrands = evaluateAndDraw(nullptr, nullptr);
        out.pressMs.push_back(msSince(t0));
        for (int k = 1; k < dabs; ++k) {
            double e = 0.0, d = 0.0;
            t0 = std::chrono::steady_clock::now();
            tool.moveTo(host, view, sx + 10.0f * static_cast<float>(k), y0, 1.0f);
            const std::size_t incremental = session.incrementalCount();
            evaluateAndDraw(&e, &d);
            out.dabMs.push_back(msSince(t0));
            out.dabEvalMs.push_back(e);
            out.dabDrawMs.push_back(d);
            out.dabChanged.push_back(changed);
            out.dabIncremental.push_back(session.incrementalCount() - incremental);
            for (const auto& [name, ms] : session.nodeTimes()) {
                if (name == "collide") out.dabCollideMs.push_back(ms);
            }
        }
        out.dabNodes = nodeTimes(session);
        double e = 0.0, d = 0.0;
        t0 = std::chrono::steady_clock::now();
        tool.end(host, view);
        out.commitMs.push_back(msSince(t0));
        evaluateAndDraw(&e, &d);
        out.releaseMs.push_back(msSince(t0));
        out.releaseEvalMs.push_back(e);
        out.releaseDrawMs.push_back(d);
        out.releaseChanged.push_back(changed);
        out.releaseNodes = nodeTimes(session);
        EXPECT_TRUE(tool.changed());
        warmUp();
    }
    return out;
}

void report(const char* label, const StrokeTimes& t) {
    std::string presses, releases;
    char one[64];
    for (const double ms : t.pressMs) {
        std::snprintf(one, sizeof(one), "%s%.0f", presses.empty() ? "" : ", ", ms);
        presses += one;
    }
    for (const double ms : t.releaseMs) {
        std::snprintf(one, sizeof(one), "%s%.0f", releases.empty() ? "" : ", ", ms);
        releases += one;
    }
    const double share = median(t.dabChanged);
    char changed[64] = "which strands changed not known";
    if (share >= 0.0) std::snprintf(changed, sizeof(changed), "%.0f%% of the strands changed", 100.0 * share);
    std::printf("[ budget ] brush stroke, %s: %zu strands (%zu while stroking): first evaluation %.0f ms; per dab "
                "median %.1f ms (evaluation %.1f + drawing %.1f, %s), worst %.1f; presses %s ms; releases %s ms\n"
                "[ budget ]   the last dab's nodes (ms, each as it last ran): %s\n"
                "[ budget ]   release median: commit %.1f, evaluation %.1f, drawing %.1f ms, %.0f%% of the strands changed; "
                "its nodes: %s\n",
                label, t.strands, t.sculptStrands, t.firstMs, median(t.dabMs), median(t.dabEvalMs), median(t.dabDrawMs),
                changed, *std::max_element(t.dabMs.begin(), t.dabMs.end()), presses.c_str(), releases.c_str(),
                t.dabNodes.c_str(), median(t.commitMs), median(t.releaseEvalMs), median(t.releaseDrawMs),
                100.0 * median(t.releaseChanged), t.releaseNodes.c_str());
    if (!t.warmMs.empty()) {
        std::string warms;
        for (const double ms : t.warmMs) {
            std::snprintf(one, sizeof(one), "%s%.0f", warms.empty() ? "" : ", ", ms);
            warms += one;
        }
        std::printf("[ budget ]   the stroke document warmed ahead: %s ms (first, then after each release); the first took "
                    "%.0f MB of private bytes\n", warms.c_str(), t.warmMegabytes);
    }
}

TEST(Budget, BrushStrokeLatency) {
    // Maya's defaults: the stroke at sculptDensity 0.35 of previewDensity 1; and every strand (1).
    for (const std::size_t count : {std::size_t{100000}, std::size_t{500000}}) {
        for (const float sculpt : {0.35f, 1.0f}) {
            char label[96];
            std::snprintf(label, sizeof(label), "%zuk strands at sculptDensity %.2f", count / 1000, sculpt);
            report(label, strokeLatency(sphereBench(count), sculpt, 2, 10));
        }
    }
    // The stroke document evaluated ahead (a host's idle time while its brush tool is up): the
    // first press makes again only what it changed.
    report("500k strands at sculptDensity 0.35, the stroke document warmed ahead", strokeLatency(sphereBench(500000), 0.35f, 2, 10, true));
    // The same at 500k with every node evaluated in full (incremental evaluation off): what the
    // incremental nodes save on this stroke.
    Graph::setIncrementalEvaluation(false);
    report("500k strands at sculptDensity 0.35, incremental evaluation off", strokeLatency(sphereBench(500000), 0.35f, 2, 10));
    Graph::setIncrementalEvaluation(true);
}

TEST(Budget, BrushStrokeLatencyWithCollide) {
    // A Collide (the app's head graph's settings: a deep search, so every point is queried) walks
    // again only the strands a dab moved, and every node after it stays incremental: a dab costs
    // what it costs with the node bypassed plus Collide's walk of the moved strands. (While Collide
    // evaluated in full it walked every strand, and every node after it evaluated in full too.)
    // That walk asks the meshes' signed distance fields (made at the first evaluation, kept), and
    // the exact method's walk (the closest point on the mesh at every point) is timed beside it.
    // On the bench sphere (Collide after Interpolate) and on the app's lookdev head.
    for (const bool head : {false, true}) {
        for (const std::size_t count : {std::size_t{100000}, std::size_t{500000}}) {
            const char* where = head ? "the head graph" : "the bench graph";
            char label[128];
            std::snprintf(label, sizeof(label), "%s, %zuk strands at sculptDensity 1.00 with Collide", where, count / 1000);
            const StrokeTimes with = strokeLatency(head ? headBench(count, Collider::Collide) : sphereBench(count, Collider::Collide), 1.0f, 2, 10);
            report(label, with);
            std::snprintf(label, sizeof(label), "%s, %zuk strands at sculptDensity 1.00 with Collide, method exact", where, count / 1000);
            const StrokeTimes exact = strokeLatency(head ? headBench(count, Collider::Exact) : sphereBench(count, Collider::Exact), 1.0f, 2, 10);
            report(label, exact);
            std::snprintf(label, sizeof(label), "%s, %zuk strands at sculptDensity 1.00, Collide bypassed", where, count / 1000);
            const StrokeTimes bypassed =
                strokeLatency(head ? headBench(count, Collider::Bypassed) : sphereBench(count, Collider::Bypassed), 1.0f, 2, 10);
            report(label, bypassed);
            const double collide = median(with.dabCollideMs);
            const std::size_t fewest = *std::min_element(with.dabIncremental.begin(), with.dabIncremental.end());
            const std::size_t fewestBypassed = *std::min_element(bypassed.dabIncremental.begin(), bypassed.dabIncremental.end());
            std::printf("[ budget ]   %s, %zuk strands: a dab with Collide %.1f ms, of which Collide %.1f (the strands "
                        "it moved walked again; exact %.1f ms, of which Collide %.1f); first evaluation %.0f ms (exact %.0f); "
                        "bypassed %.1f ms; nodes incremental per dab at least %zu with Collide, %zu bypassed\n",
                        where, count / 1000, median(with.dabEvalMs), collide, median(exact.dabEvalMs), median(exact.dabCollideMs),
                        with.firstMs, exact.firstMs, median(bypassed.dabEvalMs), fewest, fewestBypassed);
            // Collide and every node the bypassed graph runs incrementally, on every dab.
            EXPECT_EQ(fewest, fewestBypassed + 1) << where << ", " << count << " strands";
            // The rest of a dab as without Collide (generous: the machine may be busy). A timing:
            // it gates where the budgets do.
            if (enforced()) {
                EXPECT_LT(median(with.dabEvalMs) - collide, 1.5 * median(bypassed.dabEvalMs) + 5.0) << where << ", " << count;
            }
        }
    }
}

// The bench graph with field nodes plugged into params: a FieldNoise into Noise's amplitude, and
// a FieldRemap of it into Clump's strength.
std::string benchGraphWithFields(double density) {
    std::string graph = benchGraph(density);
    auto replace = [&](const std::string& from, const std::string& to) {
        const std::size_t at = graph.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) graph.replace(at, from.size(), to);
    };
    replace(R"({"name": "clump", "type": "Clump", "params": {"count": 400, "strength": 0.8}},)",
            R"({"name": "clump", "type": "Clump", "params": {"count": 400, "strength": 0.8}, "paramInputs": ["strength"]},
        {"name": "spots", "type": "FieldNoise", "params": {"frequency": 0.3}},
        {"name": "remap", "type": "FieldRemap", "params": {"outMin": 0.5}},)");
    replace(R"({"name": "noise", "type": "Noise", "params": {"amplitude": 0.5, "frequency": 1.0}},)",
            R"({"name": "noise", "type": "Noise", "params": {"amplitude": 0.5, "frequency": 1.0}, "paramInputs": ["amplitude"]},)");
    replace(R"({"from": "clump", "to": "noise", "input": "strands"},)",
            R"({"from": "clump", "to": "noise", "input": "strands"}, {"from": "spots", "to": "noise", "input": "amplitude"},
        {"from": "spots", "to": "remap", "input": "in"}, {"from": "remap", "to": "clump", "input": "strength"},)");
    return graph;
}

TEST(Budget, BrushStrokeLatencyWithFields) {
    // Field nodes plugged into params read each strand alone (FieldValue::strandLocal): the nodes
    // they feed, and those after them, stay incremental through a stroke.
    for (const float sculpt : {0.35f, 1.0f}) {
        Bench bench = sphereBench(500000);
        bench.data.graphJson = benchGraphWithFields(500000.0 / (4.0 * 3.14159265358979 * 100.0));
        char label[96];
        std::snprintf(label, sizeof(label), "500k strands at sculptDensity %.2f, fields plugged into params", sculpt);
        const StrokeTimes t = strokeLatency(std::move(bench), sculpt, 2, 10);
        report(label, t);
        std::printf("[ budget ]   nodes incremental per dab at least %zu\n",
                    *std::min_element(t.dabIncremental.begin(), t.dabIncremental.end()));
    }
}

TEST(Budget, ProfileAGroomFile) {
    // A groom file of one's own (GROOMLAB_PROFILE_GROOM: its path; skipped without), profiled: its
    // first evaluation and a second, then a comb stroke on it (the camera aimed at the scalp's
    // highest point from a scalp's size away), each with its nodes' times.
    const char* path = std::getenv("GROOMLAB_PROFILE_GROOM");
    if (!path || !*path) GTEST_SKIP() << "GROOMLAB_PROFILE_GROOM names no groom file";
    const GroomFile file = readGroomFile(path);
    // (Its maps named relative to it: read from beside it.)
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::absolute(path).parent_path());
    struct Back {
        std::filesystem::path to;
        ~Back() { std::filesystem::current_path(to); }
    } back{before};
    for (const bool cached : {true, false}) {
        // (Not caching intermediates: the second evaluation runs every node again, warm.)
        GroomSession session(file.data);
        session.setCacheIntermediates(cached);
        ViewRequest request;
        request.density = 1.0f;
        request.pointBudget = 16.0e6;
        for (const char* pass : {cached ? "first" : "uncached first", cached ? "second" : "uncached second (every node again)"}) {
            const auto t0 = std::chrono::steady_clock::now();
            const ViewResult& view = session.evaluate(request);
            std::printf("[ profile ] %s evaluation: %.0f ms, %zu strands\n[ profile ]   nodes: %s\n", pass, msSince(t0),
                        view.strands ? view.strands->strandCount() : std::size_t{0}, nodeTimes(session).c_str());
        }
    }
    Bench bench;
    bench.data = file.data;
    const ScalpMesh& scalp = *bench.data.scalp;
    Vec3f lo = scalp.points().front(), hi = lo;
    for (const Vec3f& q : scalp.points()) {
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        if (q.y > bench.strokeAt.y || &q == &scalp.points().front()) bench.strokeAt = q;
    }
    const float size = (hi - lo).length();
    bench.aimed = true;
    bench.eye = bench.strokeAt + Vec3f(0.0f, 0.6f, 0.8f) * size;
    for (const float sculpt : {0.35f, 1.0f}) {
        char label[96];
        std::snprintf(label, sizeof(label), "the groom file at sculptDensity %.2f", sculpt);
        report(label, strokeLatency(bench, sculpt, 2, 10));
    }
}

}  // namespace
}  // namespace groomlab
