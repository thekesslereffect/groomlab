// SPDX-License-Identifier: Apache-2.0
// GroomSession (docs/SESSION.md): setters that no-op on the same input, previews, views,
// versions, cards and stats, against what a fresh session of the same inputs gives.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/correctives.h"
#include "groomlab/export_view.h"
#include "groomlab/groom_session.h"
#include "groomlab/hair_color.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "groomlab/renderman_look.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"strength": 0.6, "count": 40}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::shared_ptr<const ScalpMesh> sphere() { return test::makeSphere(12, 24); }

std::string hierarchyJson() {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 6), tube);
    }
    const auto scalp = sphere();
    h.evaluate(scalp.get());
    return h.toJson();
}

GroomData groomData(const std::string& graph = kGraph) {
    GroomData d;
    d.graphJson = graph;
    d.hierarchyJson = hierarchyJson();
    d.scalp = sphere();
    return d;
}

// The same graph with `node` (JSON) between width and the output.
std::string withNode(const std::string& node, const std::string& name) {
    std::string g = kGraph;
    const std::string output = R"({"name": "output", "type": "Output"})";
    g.replace(g.find(output), output.size(), node + ", " + output);
    const std::string link = R"({"from": "width", "to": "output", "input": "strands"})";
    g.replace(g.find(link), link.size(),
              R"({"from": "width", "to": ")" + name + R"(", "input": "strands"}, {"from": ")" + name +
                  R"(", "to": "output", "input": "strands"})");
    return g;
}

// kGraph with `first` and then `second` (each a node's JSON, by name) between its last node and its output.
std::string withNodes(const std::string& first, const std::string& firstName, const std::string& second,
                      const std::string& secondName) {
    std::string g = withNode(first, firstName);
    const std::string output = R"({"name": "output", "type": "Output"})";
    g.replace(g.find(output), output.size(), second + ", " + output);
    const std::string link = R"({"from": ")" + firstName + R"(", "to": "output", "input": "strands"})";
    g.replace(g.find(link), link.size(),
              R"({"from": ")" + firstName + R"(", "to": ")" + secondName + R"(", "input": "strands"}, {"from": ")" +
                  secondName + R"(", "to": "output", "input": "strands"})");
    return g;
}

void expectSameStrands(const StrandSet& a, const StrandSet& b) {
    ASSERT_EQ(a.ids(), b.ids());
    ASSERT_EQ(a.offsets(), b.offsets());
    ASSERT_EQ(a.positions(), b.positions());
    ASSERT_EQ(a.widths(), b.widths());
}

TEST(Session, IsTheGroomAndMatchesItsOneCallEvaluate) {
    GroomSession session(groomData());
    ViewRequest request;
    request.time = 0.0;
    const ViewResult& view = session.evaluate(request);
    ASSERT_TRUE(view.strands);
    EXPECT_TRUE(view.errors.empty());
    const StrandsPtr fresh = Groom(groomData()).evaluate(1.0f, 0.0);
    expectSameStrands(*view.strands, *fresh);
    EXPECT_TRUE(view.hierarchy && view.authoredHierarchy);
    EXPECT_EQ(view.hierarchy, view.authoredHierarchy);  // nothing posed or corrected
    EXPECT_TRUE(view.bounds.valid);
    EXPECT_EQ(view.versions.strands, 1u);
    EXPECT_EQ(view.versions.hierarchy, 1u);

    // The one-call evaluate leaves the bounds out; the next view that wants them works them out.
    GroomSession quick(groomData());
    ASSERT_TRUE(quick.evaluate(1.0f, 0.0));
    EXPECT_FALSE(quick.last().bounds.valid);
    const ViewResult& later = quick.evaluate(request);
    ASSERT_TRUE(later.bounds.valid);
    EXPECT_EQ(later.bounds.lo, view.bounds.lo);
    EXPECT_EQ(later.bounds.hi, view.bounds.hi);
}

TEST(Session, RootDepthBuriesAPointUnderEachRootForRendersOnly) {
    // The Output's rootDepth: a point under each root, inside the skin, after every node, for
    // renders (not the viewport unless asked, never a game export); roots, IDs and widths kept.
    const StrandsPtr plain = GroomSession(groomData()).evaluate(1.0f, 0.0);
    ASSERT_TRUE(plain && plain->strandCount() > 50);
    auto deep = [](bool inViewport) {
        std::string g = kGraph;
        const std::string output = R"({"name": "output", "type": "Output"})";
        g.replace(g.find(output), output.size(),
                  std::string(R"({"name": "output", "type": "Output", "params": {"rootDepth": 0.05, "rootDepthInViewport": )") +
                      (inViewport ? "true" : "false") + "}}");
        return g;
    };
    GroomSession session(groomData(deep(false)));
    const StrandsPtr render = session.evaluate(1.0f, 0.0);
    ASSERT_EQ(render->strandCount(), plain->strandCount());
    EXPECT_EQ(render->pointCount(), plain->pointCount() + plain->strandCount());
    for (std::size_t i = 0; i < plain->strandCount(); ++i) {
        ASSERT_EQ(render->strandPointCount(i), plain->strandPointCount(i) + 1);
        EXPECT_EQ(render->ids()[i], plain->ids()[i]);
        const Vec3f* p = render->strandPoints(i);
        const Vec3f* q = plain->strandPoints(i);
        for (std::size_t j = 0; j < plain->strandPointCount(i); ++j) ASSERT_EQ(p[j + 1], q[j]);
        EXPECT_NEAR((p[0] - p[1]).length(), 0.05f, 1e-5f);
        EXPECT_LT(p[0].length(), p[1].length() - 0.02f) << "under the skin of the sphere (radius 1)";
        // On the strand's own line: the way down from the root is the way up to the hair above it.
        const Vec3f down = (p[1] - p[0]).normalized(), up = (p[2] - p[1]).normalized();
        if (up.dot(p[1].normalized()) > 0.2f) EXPECT_GT(down.dot(up), 0.995f) << "strand " << i;
        const std::size_t b = render->strandBegin(i);
        EXPECT_EQ(render->widths()[b], render->widths()[b + 1]);
    }
    // A game export and the viewport: as without it.
    EXPECT_EQ(session.evaluate(1.0f, 0.0, false, true)->contentHash(), plain->contentHash());
    ViewRequest viewport;
    viewport.viewport = true;
    EXPECT_EQ(session.evaluate(viewport).strands->pointCount(), plain->pointCount());
    // The same frame again: the same strands.
    EXPECT_EQ(session.evaluate(1.0f, 0.0), session.evaluate(1.0f, 0.0));
    // rootDepthInViewport: the viewport has them too.
    GroomSession shown(groomData(deep(true)));
    EXPECT_EQ(shown.evaluate(viewport).strands->pointCount(), plain->pointCount() + plain->strandCount());
}

TEST(Session, SettersWithTheSameInputsChangeNothing) {
    const GroomData data = groomData();
    GroomSession session(data);
    const ViewResult first = session.evaluate(ViewRequest{});
    const std::size_t runs = session.evaluationCount();
    // The same texts, a copy of the same scalp, the same controls: nothing to evaluate again.
    session.setGraphJson(data.graphJson);
    session.setHierarchyJson(data.hierarchyJson);
    session.setPartsJson("");
    session.setFlowJson("");
    session.setCorrectivesJson("");
    session.setScalp(std::make_shared<ScalpMesh>(*data.scalp));
    session.setControls({});
    session.setMeshes({});
    session.setGuidePoses(nullptr);
    const ViewResult& again = session.evaluate(ViewRequest{});
    EXPECT_EQ(session.evaluationCount(), runs);
    EXPECT_EQ(again.strands, first.strands);
    EXPECT_EQ(again.hierarchy, first.hierarchy);
    EXPECT_EQ(again.versions.evaluation, first.versions.evaluation);
    EXPECT_EQ(again.versions.strands, first.versions.strands);
    EXPECT_EQ(again.versions.hierarchy, first.versions.hierarchy);
    EXPECT_EQ(session.rawScalp(), data.scalp);  // the old mesh, with what it built
}

TEST(Session, UpdatedScalpPointsKeepTheTopologyAndMatchANewScalp) {
    GroomSession session(groomData());
    session.evaluate(ViewRequest{});
    std::vector<Vec3f> moved = sphere()->points();
    for (Vec3f& p : moved) p = p + Vec3f{0.0f, 0.5f, 0.0f};
    session.updateScalpPoints(moved);
    const ViewResult& view = session.evaluate(ViewRequest{});
    GroomData d = groomData();
    d.scalp = std::make_shared<ScalpMesh>(sphere()->withPoints(moved));
    expectSameStrands(*view.strands, *Groom(d).evaluate(1.0f, 0.0));
    EXPECT_EQ(session.rawScalp()->topologyHash(), sphere()->topologyHash());
    const std::uint64_t version = view.versions.strands;
    session.updateScalpPoints(moved);  // the same points: nothing
    EXPECT_EQ(session.evaluate(ViewRequest{}).versions.strands, version);
    EXPECT_THROW(session.updateScalpPoints({Vec3f{}}), std::invalid_argument);
}

TEST(Session, ScalpMapsLayVertexMapsAsTheScalpCarryingThemDoes) {
    // A Length cut by a painted vertex map: the session's scalpMaps against a scalp already
    // carrying the color set with the catalog (a groom file).
    const std::string graph =
        withNode(R"({"name": "cut", "type": "Length", "params": {"keep": {"colorSet": "tint", "min": 0.2, "max": 1.0}}})", "cut");
    const MeshPtr scalp = sphere();
    std::vector<float> values(scalp->pointCount());
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = scalp->points()[i].x > 0.0f ? 1.0f : 0.0f;
    ScalpMaps maps;
    maps.maps["tint"] = ScalpMapEntry{"vertices", values, "", 1};

    GroomSession painted(groomData(graph));
    painted.setScalpMapsJson(maps.toJson());
    const ViewResult& view = painted.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
    ASSERT_TRUE(view.scalp && view.scalp->hasColorSet("tint"));
    EXPECT_EQ(painted.scalpMaps().maps.size(), 1u);

    GroomData carried = groomData(graph);
    carried.scalp = withVertexMaps(scalp, ScalpMaps::fromJson(maps.toJson()), "", nullptr, nullptr);
    carried.maps = maps.catalog();
    expectSameStrands(*view.strands, *Groom(carried).evaluate(1.0f, 0.0));

    // Painting (a preview of the map) re-runs only the map's readers: Scatter, Interpolate stay.
    const std::size_t runs = painted.evaluationCount();
    auto stroke = std::make_shared<std::vector<float>>(values.size(), 1.0f);
    painted.setPreviewVertexMap("tint", stroke);
    const ViewResult& previewed = painted.evaluate(ViewRequest{});
    EXPECT_EQ(painted.evaluationCount() - runs, 2u);  // the cut and the output
    EXPECT_NE(previewed.strands->pointCount(), 0u);
    painted.setPreviewVertexMap("tint", nullptr);
    expectSameStrands(*painted.evaluate(ViewRequest{}).strands, *Groom(carried).evaluate(1.0f, 0.0));

    // A map painted on another scalp is said, not fatal.
    painted.setScalpMapsJson(R"({"tint": {"kind": "vertices", "values": [1, 0, 1]}})");
    const ViewResult& wrong = painted.evaluate(ViewRequest{});
    ASSERT_FALSE(wrong.errors.empty());
    EXPECT_NE(wrong.errors.front().find("was painted on a scalp with 3 vertices"), std::string::npos);
    painted.setScalpMapsJson("{not json");
    EXPECT_FALSE(painted.scalpMapsError().empty());
    EXPECT_EQ(painted.inputErrors().size(), 1u);
}

TEST(Session, AGuidePreviewReplacesTheStoredGuidesUntilCleared) {
    GroomSession session(groomData());
    const ViewResult before = session.evaluate(ViewRequest{});
    auto preview = std::make_shared<GuideHierarchy>(*before.authoredHierarchy);
    for (const HierarchyGuide& g : before.authoredHierarchy->guides()) {
        std::vector<Vec3f> points = g.points;
        for (Vec3f& p : points) p = p + Vec3f{0.0f, 0.2f, 0.0f};
        preview->setPoints(g.id, points);
    }
    session.setPreviewHierarchy(preview);
    EXPECT_TRUE(session.hasPreview());
    ViewRequest request;
    request.sculptDensity = 0.5f;
    const ViewResult during = session.evaluate(request);
    EXPECT_EQ(during.hierarchy.get(), preview.get());
    EXPECT_LT(during.strands->strandCount(), before.strands->strandCount());  // the sculpt density
    EXPECT_GT(during.versions.hierarchy, before.versions.hierarchy);
    // Edited in place between evaluations: every evaluation is a change.
    EXPECT_GT(session.evaluate(request).versions.hierarchy, during.versions.hierarchy);
    session.setPreviewHierarchy(nullptr);
    const ViewResult& after = session.evaluate(ViewRequest{});
    EXPECT_EQ(after.hierarchy, before.hierarchy);
    expectSameStrands(*after.strands, *before.strands);
}

TEST(Session, AStrokeAtTheSculptDensityKeepsTheViewsCache) {
    // A stroke evaluates at its own density on a document of its own: its first dab leaves the
    // view's cache alone, so the release (the same guides) runs nothing again, and the next
    // stroke's dabs find the last stroke's results.
    GroomSession session(groomData());
    ViewRequest request;
    request.sculptDensity = 0.5f;
    const StrandsPtr before = session.evaluate(request).strands;
    const std::size_t viewRuns = session.evaluationCount();
    auto preview = std::make_shared<GuideHierarchy>(*session.authoredHierarchy());
    session.setPreviewHierarchy(preview);
    const StrandsPtr during = session.evaluate(request).strands;
    EXPECT_LT(during->strandCount(), before->strandCount());
    const std::size_t strokeRuns = session.evaluationCount();
    EXPECT_GT(strokeRuns, viewRuns);
    session.setPreviewHierarchy(nullptr);
    const ViewResult& after = session.evaluate(request);
    EXPECT_EQ(session.evaluationCount(), strokeRuns);  // nothing ran again: the view's cache stayed
    EXPECT_EQ(after.strands.get(), before.get());
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));  // the next stroke, the same guides
    EXPECT_EQ(session.evaluate(request).strands->contentHash(), during->contentHash());
    EXPECT_EQ(session.evaluationCount(), strokeRuns);
    session.setPreviewHierarchy(nullptr);
    request.sculptDensity = 1.0f;  // a stroke at the view's density evaluates the view's document
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));
    EXPECT_EQ(session.evaluate(request).strands.get(), before.get());
    EXPECT_EQ(session.evaluationCount(), strokeRuns);
}

TEST(Session, AStrokeAHostRunsPastANodeEvaluatesWhatFeedsItAlone) {
    // ViewRequest::stopAt (Maya's GPU preview runs Interpolate and what follows on a live stroke's
    // dabs): the guides are evaluated, nothing after them, the view keeps its strands; the same
    // dab evaluated without it gives what a fresh session gives, and the operators' memos say what
    // the strands were made from (operator_state.h).
    GroomSession session(groomData());
    ViewRequest request;
    request.sculptDensity = 0.5f;
    auto preview = std::make_shared<GuideHierarchy>(*session.evaluate(ViewRequest{}).authoredHierarchy);
    session.setPreviewHierarchy(preview);
    const ViewResult pressed = session.evaluate(request);  // the press: the stroke's document in full
    const GraphDocument& doc = *session.viewDocument();
    const NodeId interpolate = doc.nodesByName.at("interpolate"), guides = doc.nodesByName.at("guides");
    const NodeId clump = doc.nodesByName.at("clump");
    const StrandsPtr blended = valueAs<StrandsPtr>(doc.graph.cachedValue(interpolate));
    InterpolateState interpolated;
    ASSERT_TRUE(interpolateState(doc.graph.node(interpolate), blended, &interpolated));
    EXPECT_EQ(interpolated.strandCount, blended->strandCount());
    EXPECT_EQ(interpolated.guides, valueAs<GuidesPtr>(doc.graph.cachedValue(guides)));
    InterpolateRecords records;
    interpolateRecords(interpolated, &records);
    ASSERT_EQ(records.offsets.size(), blended->strandCount() + 1);
    EXPECT_EQ(records.guides.size(), records.offsets.back());
    EXPECT_FALSE(interpolateState(doc.graph.node(interpolate), valueAs<StrandsPtr>(doc.graph.cachedValue(clump)), &interpolated));
    ClumpState clumped;
    ASSERT_TRUE(clumpState(doc.graph.node(clump), valueAs<StrandsPtr>(doc.graph.cachedValue(clump)), &clumped));
    EXPECT_EQ(clumped.assigned->size(), blended->strandCount());
    EXPECT_EQ(clumped.curves.size(), clumped.centerGuides.size() * kClumpCenterSamples);
    EXPECT_FALSE(clumped.chosen.empty());  // its own centers, chosen by root

    // A dab: every guide bent; evaluated up to Interpolate's inputs alone.
    for (const HierarchyGuide& g : pressed.authoredHierarchy->guides()) {
        std::vector<Vec3f> points = g.points;
        for (std::size_t i = 1; i < points.size(); ++i) points[i] = points[i] + Vec3f{0.05f * static_cast<float>(i), 0.0f, 0.0f};
        preview->setPoints(g.id, points);
    }
    const std::size_t runs = session.evaluationCount();
    ViewRequest stopped = request;
    stopped.stopAt = "interpolate";
    const ViewResult& dab = session.evaluate(stopped);
    EXPECT_EQ(dab.strands, pressed.strands);  // the view keeps its strands
    EXPECT_GT(dab.versions.hierarchy, pressed.versions.hierarchy);
    EXPECT_EQ(dab.versions.strands, pressed.versions.strands);
    EXPECT_LE(session.evaluationCount() - runs, 2u);  // the guides (and nothing after them)
    EXPECT_NE(valueAs<GuidesPtr>(doc.graph.cachedValue(guides)), interpolated.guides);  // the dab's guides
    EXPECT_EQ(valueAs<StrandsPtr>(doc.graph.cachedValue(interpolate)), blended);       // Interpolate untouched
    // The same dab evaluated in full: a fresh session's strands for those guides.
    const ViewResult& full = session.evaluate(request);
    GroomSession fresh(groomData());
    fresh.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));
    expectSameStrands(*full.strands, *fresh.evaluate(request).strands);
}

TEST(Session, AGraphEditKeepsTheWorkOfTheNodesItLeavesAlone) {
    // An edit after Interpolate (a new graph text: new operators, the old results adopted): the
    // binding and the center choice come along with the results they were made for
    // (Operator::adoptKeptWork), for a host reading them (operator_state.h) and the next
    // incremental evaluation alike.
    GroomSession session(groomData());
    session.evaluate(ViewRequest{});
    const std::size_t binds = interpolateBindCount();
    std::string edited = kGraph;
    const std::string strength = R"("strength": 0.6)";
    edited.replace(edited.find(strength), strength.size(), R"("strength": 0.7)");
    session.setGraphJson(edited);
    session.setGraphJson(kGraph);  // never evaluated in between
    session.setGraphJson(edited);
    session.evaluate(ViewRequest{});
    const GraphDocument& doc = *session.viewDocument();
    const NodeId interpolate = doc.nodesByName.at("interpolate"), clump = doc.nodesByName.at("clump");
    InterpolateState state;
    EXPECT_TRUE(interpolateState(doc.graph.node(interpolate), valueAs<StrandsPtr>(doc.graph.cachedValue(interpolate)), &state));
    ClumpState clumped;
    EXPECT_TRUE(clumpState(doc.graph.node(clump), valueAs<StrandsPtr>(doc.graph.cachedValue(clump)), &clumped));
    EXPECT_EQ(interpolateBindCount(), binds);  // Interpolate ran no more
}

TEST(Session, ALiveStrokeEvaluatesAtTheSculptDensityAndAsksForAnotherAfter) {
    GroomSession session(groomData());
    ViewRequest request;
    request.sculptDensity = 0.25f;
    const std::size_t all = session.evaluate(request).strands->strandCount();
    EXPECT_FALSE(session.setLiveStroke(true));
    EXPECT_LT(session.evaluate(request).strands->strandCount(), all);
    EXPECT_TRUE(session.setLiveStroke(false));  // evaluated at the stroke's density: once more
    EXPECT_EQ(session.evaluate(request).strands->strandCount(), all);
    EXPECT_FALSE(session.setLiveStroke(true));
    EXPECT_FALSE(session.setLiveStroke(false));  // nothing evaluated during it
}

TEST(Session, PartsPreviewAndBadTextsAreSaidNotThrown) {
    GroomSession session(groomData());
    session.setPartsJson("{not json");
    session.setFlowJson("[1, 2");
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands);
    ASSERT_EQ(view.errors.size(), 2u);
    EXPECT_EQ(view.errors[0].rfind("partData: ", 0), 0u);
    EXPECT_EQ(view.errors[1].rfind("flowData: ", 0), 0u);
    EXPECT_EQ(session.inputErrors().size(), 2u);
    session.setPartsJson("");
    session.setFlowJson("");
    EXPECT_TRUE(session.evaluate(ViewRequest{}).errors.empty());

    // A part previewed while it is brushed binds to the scalp as the stored one would.
    PartSet parts;
    std::vector<Vec3f> line;
    for (int i = 0; i < 5; ++i) line.push_back(Vec3f{0.0f, 1.0f, -0.5f + 0.25f * static_cast<float>(i)}.normalized());
    parts.add("middle", line, *sphere());
    session.setPreviewOverlays(std::make_shared<const PartSet>(parts), nullptr);
    const ViewResult& previewed = session.evaluate(ViewRequest{});
    ASSERT_TRUE(previewed.parts);
    session.setPreviewOverlays(nullptr, nullptr);
    session.setPartsJson(parts.toJson());
    const ViewResult& stored = session.evaluate(ViewRequest{});
    ASSERT_TRUE(stored.parts);
    EXPECT_EQ(stored.parts->contentHash(), previewed.parts->contentHash());

    // A graph that does not load: kept by setGraphJson, dropped by dropGraph.
    EXPECT_THROW(session.setGraphJson("{bad"), GraphError);
    EXPECT_TRUE(session.hasGraph());
    session.dropGraph("{bad", "graph: bad");
    const ViewResult& dropped = session.evaluate(ViewRequest{});
    EXPECT_FALSE(dropped.strands);
    ASSERT_FALSE(dropped.errors.empty());
    EXPECT_EQ(dropped.errors.front(), "graph: bad");
    session.setGraphJson("{bad");  // the same failing text: nothing
    session.setGraphJson(kGraph);
    EXPECT_TRUE(session.evaluate(ViewRequest{}).strands);
}

TEST(Session, LiveCorrectiveWeightsReplaceTheLayersEnvelopes) {
    // A layer at full weight moves the guides; its live weight at 0 leaves them as authored.
    GroomSession session(groomData());
    const ViewResult plain = session.evaluate(ViewRequest{});
    Correctives c;
    CorrectiveLayer layer;
    layer.name = "fix";
    layer.time = 0.0;
    layer.slot = 0;
    for (const HierarchyGuide& g : plain.authoredHierarchy->guides()) {
        layer.offsets[g.id] = std::vector<Vec3f>(g.points.size(), Vec3f{0.05f, 0.0f, 0.0f});
    }
    c.layers.push_back(layer);
    session.setCorrectivesJson(c.toJson());
    const ViewResult corrected = session.evaluate(ViewRequest{});
    EXPECT_NE(corrected.hierarchy->contentHash(), plain.hierarchy->contentHash());
    session.setCorrectiveWeights({{0, 0.0f}});
    const ViewResult& off = session.evaluate(ViewRequest{});
    EXPECT_EQ(off.hierarchy->contentHash(), plain.hierarchy->contentHash());
    expectSameStrands(*off.strands, *plain.strands);
    const HierarchyPtr kept = off.hierarchy;
    EXPECT_EQ(session.evaluate(ViewRequest{}).hierarchy, kept);  // the same weights: the same hierarchy
}

TEST(Session, ViewsANodeAndCountsWhatItShows) {
    GroomSession session(groomData());
    ViewRequest request;
    request.viewNode = "scatter";
    const ViewResult& view = session.evaluate(request);
    EXPECT_EQ(view.viewedNode, "scatter");
    EXPECT_TRUE(view.viewedRoots);
    request.viewNode = "nope";  // not a node: the output
    EXPECT_EQ(session.evaluate(request).viewedNode, "");
    const GroomStats stats = session.stats();
    EXPECT_EQ(stats.strands, session.last().strands->strandCount());
    EXPECT_EQ(stats.guides, 8u);
    EXPECT_TRUE(stats.times.count("scatter"));
    const nlohmann::json json = nlohmann::json::parse(stats.toJson());
    for (const char* key : {"view", "strands", "points", "guides", "cards", "cardNodes", "hairColor", "renderData", "colorNodes", "colorBlends",
                            "times", "channels", "notes", "scalpHistory", "instances", "restEvaluations", "cardsStale"}) {
        EXPECT_TRUE(json.contains(key)) << key;
    }
}

TEST(Session, CardsAtTheirDensityKeepABakedAtlas) {
    const std::string graph = withNode(R"({"name": "cards", "type": "Cards", "params": {"segments": 4, "maxCards": 40}})", "cards");
    GroomSession session(groomData(graph));
    EXPECT_TRUE(session.hasCardsNode());
    ViewRequest request;
    request.density = 0.2f;
    request.wantCards = true;
    const ViewResult first = session.evaluate(request);
    ASSERT_TRUE(first.cards);
    EXPECT_FALSE(first.cards->cards.empty());
    EXPECT_EQ(first.cardStrands->strandCount(), Groom(groomData(graph)).evaluate(1.0f, 0.0)->strandCount());
    EXPECT_EQ(session.evaluate(request).versions.cards, first.versions.cards);  // nothing changed: the same cards

    // A bake's atlas: the stamp says nothing is stale; cards rebuilt keep the baked cells.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_session_cards";
    std::filesystem::create_directories(dir);
    const std::string atlas = (dir / "hair_Atlas.json").generic_string();
    {
        std::ofstream out(atlas, std::ios::binary | std::ios::trunc);
        out << captureCardAtlas(*first.cards, first.cardStrands.get()).toJson();
    }
    session.setCardMaps("Coverage=" + (dir / "hair_Coverage.tga").generic_string() + ";Atlas=" + atlas +
                        ";Strands=" + cardsBakeStamp(first.cardNodes));
    const ViewResult& relaid = session.evaluate(request);
    EXPECT_GT(relaid.versions.cards, first.versions.cards);  // laid out for the bake
    GroomStats stats = session.stats();
    EXPECT_TRUE(stats.cardsBaked);
    EXPECT_FALSE(stats.cardsStale);
    EXPECT_EQ(stats.cards, first.cards->cards.size());

    request.wantCards = false;  // nothing wants them: dropped
    EXPECT_FALSE(session.evaluate(request).cards);
    std::filesystem::remove_all(dir);
}

TEST(Session, RachisAndScratchEvaluationsLeaveTheViewAlone) {
    GroomSession session(groomData());
    ViewRequest request;
    request.density = 0.25f;
    const ViewResult view = session.evaluate(request);
    const StrandsPtr full = session.evaluateFullDensity(0.0);
    ASSERT_TRUE(full);
    EXPECT_GT(full->strandCount(), view.strands->strandCount());
    const Value scatter = session.evaluateNode("scatter", 1.0f, 0.0);
    ASSERT_TRUE(std::holds_alternative<StrandsPtr>(scatter));
    EXPECT_EQ(std::get<StrandsPtr>(scatter)->strandCount(), full->strandCount());
    EXPECT_THROW(session.evaluateNode("nope", 1.0f, 0.0), GraphError);
    EXPECT_EQ(session.evaluate(request).strands, view.strands);
    session.releaseFullDensity();
}

TEST(Session, StartsWithTheDefaultGraphAndTakesInputsBySetters) {
    GroomSession session;
    EXPECT_TRUE(session.hasGraph());
    session.setScalp(sphere());
    session.setHierarchyJson(hierarchyJson());
    session.setGraphJson(kGraph);
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands);
    expectSameStrands(*view.strands, *Groom(groomData()).evaluate(1.0f, 0.0));
    GroomSession empty;
    empty.setHierarchyJson("");  // no guides: an empty hierarchy, no error
    EXPECT_TRUE(empty.evaluate(ViewRequest{}).errors.empty());
    EXPECT_FALSE(empty.hierarchy());
}


TEST(Session, TheHairLookCarriesTheIridescenceAndWhetherColorIsRgb) {
    {
        GroomSession session(groomData(withNode(R"({"name": "color", "type": "HairColor"})", "color")));
        session.evaluate(ViewRequest{});
        const HairLook look = session.hairLook();
        EXPECT_EQ(look.node, "color");
        EXPECT_FALSE(look.iridescent);
        EXPECT_TRUE(look.colorsByRgb);  // HairColor's RGB is the hair's color, in mode melanin too
        EXPECT_FALSE(look.colorBlends);
        EXPECT_FLOAT_EQ(look.dye[0], 1.0f);  // no dye: white
        EXPECT_FLOAT_EQ(look.specular, 0.3f);  // HairColor's defaults: the default highlight
        EXPECT_FLOAT_EQ(look.roughness, 0.5f);
        EXPECT_FLOAT_EQ(look.tilt, 3.0f);
    }
    {
        // The highlight is the first HairColor's (a RenderData's opacity aside), a map its default.
        GroomSession session(groomData(withNodes(R"({"name": "color", "type": "HairColor", "params": {"specular": 0.7,
                                                     "roughness": 0.2, "specularRandom": 0.1, "roughnessRandom": 0.3, "tilt": 6.0}})",
                                                 "color", R"({"name": "render", "type": "RenderData", "params": {"opacity": 0.5}})",
                                                 "render")));
        session.evaluate(ViewRequest{});
        const HairLook look = session.hairLook();
        EXPECT_FLOAT_EQ(look.specular, 0.7f);
        EXPECT_FLOAT_EQ(look.roughness, 0.2f);
        EXPECT_FLOAT_EQ(look.specularRandom, 0.1f);
        EXPECT_FLOAT_EQ(look.roughnessRandom, 0.3f);
        EXPECT_FLOAT_EQ(look.tilt, 6.0f);
        EXPECT_EQ(session.stats().renderData, "render");
        EXPECT_TRUE(look.colorsByRgb);
        // The lobes every host shades by (the viewport's uniforms, Arnold's standard_hair, the app).
        const HairLobes lobes = hairLobes(look);
        EXPECT_FLOAT_EQ(lobes.specularTint, 1.0f);
        EXPECT_FLOAT_EQ(lobes.roughness, 0.1f + 0.4f * 0.2f);
        EXPECT_FLOAT_EQ(lobes.shiftDegrees, 6.0f);
    }
    {
        GroomSession session(groomData(withNode(
            R"({"name": "color", "type": "HairColor", "params": {"roughness": {"expression": "u"}, "specular": 0.15}})", "color")));
        session.evaluate(ViewRequest{});
        EXPECT_FLOAT_EQ(session.hairLook().roughness, 0.5f) << "a map: its default";
        EXPECT_FLOAT_EQ(session.hairLook().specular, 0.15f);
    }
    {
        GroomSession session(groomData(withNode(
            R"({"name": "color", "type": "HairColor", "params": {"dye": [0.8, 0.6, 0.4]}})", "color")));
        session.evaluate(ViewRequest{});
        const HairLook look = session.hairLook();
        EXPECT_TRUE(look.colorsByRgb);  // the dye is in the RGB
        EXPECT_FLOAT_EQ(look.dye[1], 0.6f);
    }
    {
        GroomSession session(groomData(withNodes(R"({"name": "color", "type": "HairColor"})", "color",
                                                 R"({"name": "feathers", "type": "FeatherColor", "params": {"iridescence": 0.7,
                                                     "iridescenceColor": [0.1, 0.2, 0.9], "iridescenceShift": -0.2}})",
                                                 "feathers")));
        session.evaluate(ViewRequest{});
        const HairLook look = session.hairLook();
        EXPECT_EQ(look.node, "color");
        EXPECT_TRUE(look.iridescent);
        EXPECT_TRUE(look.colorsByRgb);  // a feather color: per point RGB
        EXPECT_FLOAT_EQ(look.iridescenceColor[2], 0.9f);
        EXPECT_FLOAT_EQ(look.iridescenceShift, -0.2f);
        EXPECT_EQ(session.stats().colorNodes, (std::vector<std::string>{"feathers", "color"}));
    }
    {
        GroomSession session(groomData(withNodes(R"({"name": "color", "type": "HairColor"})", "color",
                                                 R"({"name": "bands", "type": "Bands"})", "bands")));
        session.evaluate(ViewRequest{});
        EXPECT_TRUE(session.hairLook().colorsByRgb);  // bands: per point RGB
        EXPECT_FALSE(session.hairLook().iridescent);
    }
}

// HairColor's mode color: the picked color is what every host shows and renders. The look colors
// by RGB (Maya's viewport and create_hair_shader, the app, Arnold's default shader, Blender), the
// pigment params count for nothing, and RenderMan's primvars render the RGB as picked.
TEST(Session, AHairColorInColorModeRendersThePickedColorEverywhere) {
    GroomSession session(groomData(withNode(
        R"({"name": "color", "type": "HairColor", "params": {"mode": "color", "color": [0.9, 0.7, 0.6],
            "melanin": 0.9, "redness": 0.8, "dye": [0.1, 0.1, 0.1], "greys": 0.5}})", "color")));
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands && view.strands->strandCount() > 0);
    const HairLook look = session.hairLook();
    EXPECT_TRUE(look.colorsByRgb);
    EXPECT_FLOAT_EQ(look.dye[0], 1.0f);  // the melanin mode's dye: none
    const StrandSet& s = *view.strands;
    const std::vector<std::array<float, 3>> colors = strandColors(s);
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        EXPECT_FLOAT_EQ(colors[i][0], 0.9f);
        EXPECT_FLOAT_EQ(colors[i][1], 0.7f);
        EXPECT_FLOAT_EQ(colors[i][2], 0.6f);
        EXPECT_FLOAT_EQ((*s.strandChannel(kMelaninChannel))[i], 0.0f);  // no pigment under it
    }
    const ExportView exported = describeExport(s, nullptr, kExportPointColors);
    const std::vector<renderman::Primvar> primvars = renderman::curvePrimvars(exported, look);
    auto find = [&](const char* name) -> const renderman::Primvar* {
        for (const renderman::Primvar& p : primvars) {
            if (p.name == name) return &p;
        }
        return nullptr;
    };
    ASSERT_TRUE(find("rman_rgbShare") && find("rman_color") && find("rman_dye"));
    EXPECT_FLOAT_EQ(find("rman_rgbShare")->floats[0], 1.0f);  // the RGB's look
    EXPECT_NEAR(find("rman_color")->floats[0], 0.9f, 1e-5f);
    EXPECT_NEAR(find("rman_color")->floats[1], 0.7f, 1e-5f);

    // Back to melanin: the pigment again, the picked color left aside.
    GroomSession natural(groomData(withNode(
        R"({"name": "color", "type": "HairColor", "params": {"mode": "melanin", "color": [0.9, 0.7, 0.6]}})", "color")));
    const ViewResult& naturalView = natural.evaluate(ViewRequest{});
    EXPECT_TRUE(natural.hairLook().colorsByRgb);
    const std::array<float, 3> brown = strandColors(*naturalView.strands)[0];
    EXPECT_LT(brown[0], 0.5f);
    EXPECT_GT(brown[0], brown[2]);
}

// The look as hosts read it (Maya's create_hair_shader, Blender's material): the HairLook, its
// lobes and the params of the nodes it comes from.
TEST(Session, TheHairLookJsonCarriesTheLobesAndTheNodesParams) {
    {
        GroomSession session(groomData(withNodes(R"({"name": "color", "type": "HairColor", "params": {"hueRandom": 0.2,
                                                     "specular": 0.15, "roughness": 0.25, "tilt": 7.5}})",
                                                 "color", R"({"name": "render", "type": "RenderData", "params": {"opacity": 0.8}})",
                                                 "render")));
        session.evaluate(ViewRequest{});
        const nlohmann::json look = nlohmann::json::parse(session.hairLookJson());
        EXPECT_EQ(look["node"], "color");
        EXPECT_TRUE(look["colorsByRgb"].get<bool>());  // HairColor: the RGB
        EXPECT_NEAR(look["specular"].get<double>(), 0.15, 1e-6);
        EXPECT_NEAR(look["tilt"].get<double>(), 7.5, 1e-6);
        EXPECT_NEAR(look["lobes"]["specularTint"].get<double>(), 0.5, 1e-6);
        EXPECT_NEAR(look["lobes"]["roughness"].get<double>(), 0.2, 1e-6);
        EXPECT_NEAR(look["lobes"]["ior"].get<double>(), 1.55, 1e-6);
        EXPECT_NEAR(look["lobes"]["shift"].get<double>(), 7.5, 1e-6);  // the tilt: Arnold's shift
        EXPECT_DOUBLE_EQ(look["hairColorParams"]["specular"].get<double>(), 0.15);  // the param as the graph holds it
        EXPECT_DOUBLE_EQ(look["hairColorParams"]["hueRandom"].get<double>(), 0.2);
        EXPECT_DOUBLE_EQ(look["renderDataParams"]["opacity"].get<double>(), 0.8);
        EXPECT_FALSE(look["renderDataParams"].contains("specular")) << "the highlight is HairColor's alone";
        ASSERT_EQ(look["colorNodes"].size(), 1u);
        EXPECT_EQ(look["colorNodes"][0]["path"], "color");
        EXPECT_EQ(look["colorNodes"][0]["type"], "HairColor");
    }
    {
        GroomSession session(groomData(withNode(R"({"name": "bands", "type": "Bands"})", "bands")));
        session.evaluate(ViewRequest{});
        const nlohmann::json look = nlohmann::json::parse(session.hairLookJson());
        EXPECT_TRUE(look["hairColorParams"].is_null());
        EXPECT_TRUE(look["renderDataParams"].is_null());
        EXPECT_NEAR(look["lobes"]["specularTint"].get<double>(), 1.0, 1e-6);  // HairColor's defaults
        EXPECT_EQ(look["colorNodes"][0]["type"], "Bands");
    }
}

// The defaults keep the look hosts shaded before HairColor held the highlight: the default
// graph's hairLookJson as it was (RenderData's defaults then: 0.3, 0.5; aiStandardHair's shift).
TEST(Session, TheDefaultLookIsUnchanged) {
    GroomSession session(groomData(withNode(R"({"name": "color", "type": "HairColor"})", "color")));
    const ViewResult& view = session.evaluate(ViewRequest{});
    nlohmann::json look = nlohmann::json::parse(session.hairLookJson());
    const nlohmann::json lobes = look["lobes"];
    EXPECT_NEAR(look["specular"].get<double>(), 0.3, 1e-6);
    EXPECT_NEAR(look["roughness"].get<double>(), 0.5, 1e-6);
    EXPECT_EQ(look["specularRandom"].get<double>(), 0.0);
    EXPECT_EQ(look["roughnessRandom"].get<double>(), 0.0);
    EXPECT_NEAR(lobes["specularTint"].get<double>(), 1.0, 1e-6);
    EXPECT_NEAR(lobes["roughness"].get<double>(), 0.3, 1e-6);
    EXPECT_NEAR(lobes["azimuthalRoughness"].get<double>(), 0.2, 1e-6);
    EXPECT_NEAR(lobes["ior"].get<double>(), 1.55, 1e-6);
    EXPECT_NEAR(lobes["shift"].get<double>(), 3.0, 1e-6);
    // The lobes the same as a look without any HairColor (the viewport's and Arnold's defaults).
    GroomSession bare(groomData(withNode(R"({"name": "bands", "type": "Bands"})", "bands")));
    bare.evaluate(ViewRequest{});
    EXPECT_EQ(nlohmann::json::parse(bare.hairLookJson())["lobes"], lobes);
    // And no highlight channels on the strands: exports and goldens as they were.
    ASSERT_TRUE(view.strands);
    for (const char* name : {kSpecularChannel, kRoughnessChannel, kTiltChannel}) EXPECT_FALSE(view.strands->strandChannel(name)) << name;
    // A groom saved before: a RenderData's highlight params load and are ignored (HairColor's now).
    GroomSession old(groomData(withNodes(R"({"name": "color", "type": "HairColor"})", "color",
                                         R"({"name": "render", "type": "RenderData", "params": {"specular": 0.7, "roughness": 0.2}})",
                                         "render")));
    const ViewResult& oldView = old.evaluate(ViewRequest{});
    EXPECT_TRUE(oldView.errors.empty()) << (oldView.errors.empty() ? "" : oldView.errors[0]);
    EXPECT_FLOAT_EQ(old.hairLook().specular, 0.3f);
    EXPECT_FLOAT_EQ(old.hairLook().roughness, 0.5f);
}

}  // namespace
}  // namespace groomlab
