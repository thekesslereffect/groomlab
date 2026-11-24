// SPDX-License-Identifier: Apache-2.0
// The SculptStrands node (sculpt_strands.h): strokes on the strands themselves, kept by strand ID as
// relative forms, riding on what feeds the node, filled into strands with none (a raised density).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include <gtest/gtest.h>

#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/sculpt.h"
#include "groomlab/sculpt_strands.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Scalp -> Scatter (every strand shown) -> Noise -> SculptStrands -> Output.
std::string strandGraph(double density, double noise = 0.0, double strength = 1.0, bool fill = true) {
    char text[2048];
    std::snprintf(text, sizeof(text), R"({"version": 1, "output": "output",
     "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "scatter", "type": "Scatter", "params": {"density": %g, "length": 0.6, "previewAll": true}},
      {"name": "noise", "type": "Noise", "params": {"amplitude": %g}},
      {"name": "sculpt", "type": "SculptStrands", "params": {"strength": %g, "fill": %s}},
      {"name": "output", "type": "Output"}],
     "connections": [
      {"from": "scalp", "to": "scatter", "input": "scalp"},
      {"from": "scatter", "to": "noise", "input": "strands"},
      {"from": "noise", "to": "sculpt", "input": "strands"},
      {"from": "sculpt", "to": "output", "input": "strands"}]})",
                  density, noise, strength, fill ? "true" : "false");
    return text;
}

struct Groom {
    MeshPtr scalp = test::makeSphere(24, 48);
    std::unique_ptr<GroomSession> session;
    std::unique_ptr<SessionStrokeHost> host;
    explicit Groom(const std::string& graph) {
        GroomData data;
        data.scalp = scalp;
        data.graphJson = graph;
        session = std::make_unique<GroomSession>(std::move(data));
        host = std::make_unique<SessionStrokeHost>(*session);
        const ViewResult view = session->evaluate(ViewRequest{});
        EXPECT_TRUE(view.strands) << "the graph evaluates";
        for (const auto& e : view.errors) ADD_FAILURE() << e;
    }
    StrandsPtr node(const std::string& name) { return valueAs<StrandsPtr>(session->evaluateNode(name, 1.0f, 0.0)); }
    StrandsPtr input() { return node("noise"); }
    StrandsPtr output() { return node("sculpt"); }
};

std::vector<Vec3f> pointsOf(const StrandSet& set, std::size_t s) {
    const Vec3f* p = set.strandPoints(s);
    return {p, p + set.strandPointCount(s)};
}

// The strand's tip end turned about X at its middle point: a hook.
std::vector<Vec3f> hooked(std::vector<Vec3f> pts, float angle) {
    const std::size_t from = pts.size() / 2;
    const float c = std::cos(angle), sn = std::sin(angle);
    for (std::size_t k = from + 1; k < pts.size(); ++k) {
        const Vec3f d = pts[k] - pts[from];
        pts[k] = pts[from] + Vec3f(d.x, d.y * c - d.z * sn, d.y * sn + d.z * c);
    }
    return pts;
}

// The strands whose roots are within `radius` of `center` (by their first point).
std::vector<std::size_t> near(const StrandSet& set, const Vec3f& center, float radius) {
    std::vector<std::size_t> out;
    for (std::size_t s = 0; s < set.strandCount(); ++s) {
        if ((set.strandPoints(s)[0] - center).length() < radius) out.push_back(s);
    }
    return out;
}

// A stroke hooking every strand within `radius` of the crown, recorded into the session's sculpts.
SculptEdits brushCrown(Groom& g, float radius, float angle) {
    const StrandsPtr in = g.input();
    std::vector<std::pair<std::size_t, std::vector<Vec3f>>> sculpted;
    for (const std::size_t s : near(*in, {0.0f, 1.0f, 0.0f}, radius)) sculpted.emplace_back(s, hooked(pointsOf(*in, s), angle));
    SculptEdits edits;
    recordStrandStroke(edits, *in, sculpted, 1.0f, g.scalp.get());
    SculptSet set;
    set.nodes["sculpt"] = edits;
    g.session->setSculptsJson(set.toJson());
    return edits;
}

float maxDistance(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float d = 0.0f;
    for (std::size_t k = 0; k < std::min(a.size(), b.size()); ++k) d = std::max(d, (a[k] - b[k]).length());
    return d;
}

std::size_t indexOf(const StrandSet& set, StrandId id) {
    const auto& ids = set.ids();
    return static_cast<std::size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
}

TEST(SculptStrands, WithoutEditsTheNodeIsTheIdentity) {
    Groom g(strandGraph(60.0));
    EXPECT_EQ(g.output(), g.input()) << "the very strands it is fed";
}

TEST(SculptStrands, AStrokeGivesTheBrushedStrandsAndLeavesTheOthers) {
    Groom g(strandGraph(60.0));
    const StrandsPtr in = g.input();
    const std::vector<std::size_t> brushed = near(*in, {0.0f, 1.0f, 0.0f}, 0.5f);
    ASSERT_GT(brushed.size(), 5u);
    const SculptEdits edits = brushCrown(g, 0.5f, 0.9f);
    EXPECT_EQ(edits.strands.size(), brushed.size());
    const StrandsPtr out = g.output();
    ASSERT_EQ(out->strandCount(), in->strandCount());
    for (const std::size_t s : brushed) {
        EXPECT_LT(maxDistance(pointsOf(*out, s), hooked(pointsOf(*in, s), 0.9f)), 1e-4f) << "brushed strand " << s;
        EXPECT_GT(edits.strands.at(in->ids()[s]).spacing, 0.0f);
    }
    // Strands far from the stroke (past its region and the fill's reach) as fed, to the bit.
    for (const std::size_t s : near(*in, {0.0f, -1.0f, 0.0f}, 1.0f)) EXPECT_EQ(pointsOf(*out, s), pointsOf(*in, s)) << s;
    // Strength scales every bend: 0 the strands as fed.
    Groom half(strandGraph(60.0, 0.0, 0.0));
    half.session->setSculptsJson(g.session->data().sculptsJson);
    EXPECT_EQ(half.output(), half.input());
}

TEST(SculptStrands, TheEditsSaveAndLoadAndAGuideOnlyTextIsUnchanged) {
    Groom g(strandGraph(60.0));
    const SculptEdits edits = brushCrown(g, 0.5f, 0.6f);
    const std::string text = g.session->data().sculptsJson;
    const SculptSet read = SculptSet::fromJson(text);
    ASSERT_TRUE(read.node("sculpt"));
    EXPECT_EQ(read.node("sculpt")->strands, edits.strands) << "roots, spacings and edits, as recorded";
    EXPECT_EQ(read.node("sculpt")->contentHash(), edits.contentHash());
    // A Sculpt node's guides alone: the text has no "strands" (what builds before read).
    SculptSet guides;
    guides.nodes["s"].guides[3].samples = {{0.5f, 0.1f, 0.0f, 1.0f}};
    EXPECT_EQ(guides.toJson().find("strands"), std::string::npos);
    EXPECT_EQ(SculptSet::fromJson(guides.toJson()).nodes, guides.nodes);
    // Renamed with its node, as the Sculpt node's are.
    const SculptSet renamed = SculptSet::fromJson(renameSculptNode(text, "sculpt", "fringe"));
    ASSERT_TRUE(renamed.node("fringe"));
    EXPECT_EQ(renamed.node("fringe")->strands, edits.strands);
}

TEST(SculptStrands, TheBrushedBendsRideOnWhatFeedsTheNode) {
    // The strokes made on straight strands; a Noise turned up above the node afterwards: each brushed
    // strand keeps its hook (its relative form) on the noisy shape, none snaps back.
    Groom g(strandGraph(60.0));
    const StrandsPtr straight = g.input();
    brushCrown(g, 0.5f, 0.9f);
    const std::string sculpts = g.session->data().sculptsJson;
    Groom noisy(strandGraph(60.0, 0.15));
    noisy.session->setSculptsJson(sculpts);
    const StrandsPtr in = noisy.input(), out = noisy.output();
    ASSERT_EQ(in->ids(), straight->ids()) << "the same strands";
    const StrandsPtr hookedStraight = g.output();
    const auto turn = [](const Vec3f& x, const Vec3f& y) { return std::acos(std::clamp(x.normalized().dot(y.normalized()), -1.0f, 1.0f)); };
    std::size_t checked = 0;
    for (const std::size_t s : near(*in, {0.0f, 1.0f, 0.0f}, 0.4f)) {
        // The angle between the input's and the output's segment, segment by segment, as on the
        // straight strands: the same bends laid on the new shape, by normalized arc length (a
        // segment Noise moved between the hook's last unturned sample and its first turned one
        // takes a turn between: left out).
        const std::vector<Vec3f> a0 = pointsOf(*straight, s), b0 = pointsOf(*hookedStraight, s);
        const std::vector<Vec3f> a1 = pointsOf(*in, s), b1 = pointsOf(*out, s);
        ASSERT_EQ(a1.size(), a0.size());
        const std::size_t segments = a1.size() - 1;
        float total = 0.0f, along = 0.0f;
        for (std::size_t k = 0; k < segments; ++k) total += (a1[k + 1] - a1[k]).length();
        const float t0 = (static_cast<float>(segments / 2) - 0.5f) / static_cast<float>(segments);  // the straight strands' samples
        const float t1 = (static_cast<float>(segments / 2) + 0.5f) / static_cast<float>(segments);  // round the hook's start
        for (std::size_t k = 0; k < segments; ++k) {
            const float len = (a1[k + 1] - a1[k]).length();
            const float t = (along + 0.5f * len) / total;
            along += len;
            EXPECT_NEAR((b1[k + 1] - b1[k]).length(), len, 1e-4f) << "lengths kept";
            if (t > t0 - 1e-3f && t < t1 + 1e-3f) continue;
            // The straight strands' turns: none before the hook, 0.9 radians after it.
            const float want = turn(a0[(t < t0 ? 0 : segments - 1) + 1] - a0[t < t0 ? 0 : segments - 1],
                                    b0[(t < t0 ? 0 : segments - 1) + 1] - b0[t < t0 ? 0 : segments - 1]);
            EXPECT_NEAR(turn(a1[k + 1] - a1[k], b1[k + 1] - b1[k]), want, 2e-3f) << s << " " << k;
        }
        EXPECT_GT(maxDistance(b1, a1), 0.05f) << "still hooked";
        ++checked;
    }
    EXPECT_GT(checked, 3u);
}

TEST(SculptStrands, ARaisedDensityFillsTheNewStrandsFromTheirNeighbours) {
    // Sculpted at a low density; the density raised: the strands it adds inside the brushed region
    // hook as their neighbours do, those outside it stay as fed, and the recorded ones keep theirs.
    Groom low(strandGraph(60.0));
    const StrandsPtr before = low.input();
    const SculptEdits edits = brushCrown(low, 0.6f, 0.9f);
    Groom high(strandGraph(240.0));
    high.session->setSculptsJson(low.session->data().sculptsJson);
    const StrandsPtr in = high.input(), out = high.output();
    ASSERT_GT(in->strandCount(), before->strandCount() * 3);
    const std::set<StrandId> old(before->ids().begin(), before->ids().end());
    float spacing = 0.0f;
    for (const auto& [id, s] : edits.strands) spacing = std::max(spacing, s.spacing);
    std::size_t inside = 0, outside = 0;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const StrandId id = in->ids()[s];
        const float r = (in->strandPoints(s)[0] - Vec3f(0.0f, 1.0f, 0.0f)).length();
        const std::vector<Vec3f> fed = pointsOf(*in, s), shown = pointsOf(*out, s);
        if (old.count(id)) {
            if (edits.strands.count(id)) {
                EXPECT_LT(maxDistance(shown, hooked(fed, 0.9f)), 1e-4f) << "a recorded strand keeps its own edit";
            }
            continue;
        }
        if (r < 0.6f - 2.0f * spacing) {  // well inside: hooked as its neighbours (the same hook)
            EXPECT_LT(maxDistance(shown, hooked(fed, 0.9f)), 0.25f * maxDistance(hooked(fed, 0.9f), fed)) << "new strand " << id;
            ++inside;
        } else if (r > 0.6f + 2.0f * spacing) {  // past the fill's reach: as fed
            EXPECT_EQ(shown, fed) << "new strand " << id;
            ++outside;
        }
    }
    EXPECT_GT(inside, 10u);
    EXPECT_GT(outside, 100u);
    // Fill off: the new strands as fed.
    Groom off(strandGraph(240.0, 0.0, 1.0, false));
    off.session->setSculptsJson(low.session->data().sculptsJson);
    const StrandsPtr offIn = off.input(), offOut = off.output();
    for (std::size_t s = 0; s < offIn->strandCount(); ++s) {
        if (!old.count(offIn->ids()[s])) EXPECT_EQ(pointsOf(*offOut, s), pointsOf(*offIn, s));
    }
}

TEST(SculptStrands, StrandsBrushedAndLeftAsTheyWereStopTheFill) {
    // Two strokes: the crown hooked, a ring round it brushed but left as it was. At a raised density
    // the new strands in the ring stay (about) as fed: the ring's empty edits read as none.
    Groom low(strandGraph(60.0));
    const StrandsPtr in = low.input();
    std::vector<std::pair<std::size_t, std::vector<Vec3f>>> crown, ring;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const float r = (in->strandPoints(s)[0] - Vec3f(0.0f, 1.0f, 0.0f)).length();
        if (r < 0.4f) crown.emplace_back(s, hooked(pointsOf(*in, s), 0.9f));
        else if (r < 0.9f) ring.emplace_back(s, pointsOf(*in, s));
    }
    SculptEdits edits;
    recordStrandStroke(edits, *in, crown, 1.0f, low.scalp.get());
    recordStrandStroke(edits, *in, ring, 1.0f, low.scalp.get());
    for (const auto& [s, pts] : ring) EXPECT_TRUE(edits.strands.at(in->ids()[s]).edit.samples.empty()) << "kept, empty";
    SculptSet set;
    set.nodes["sculpt"] = edits;
    Groom high(strandGraph(240.0));
    high.session->setSculptsJson(set.toJson());
    const StrandsPtr hin = high.input(), hout = high.output();
    std::size_t checked = 0;
    for (std::size_t s = 0; s < hin->strandCount(); ++s) {
        const float r = (hin->strandPoints(s)[0] - Vec3f(0.0f, 1.0f, 0.0f)).length();
        if (r < 0.7f || r > 0.85f) continue;  // (the ring, past the crown's strands' reach)
        const std::vector<Vec3f> fed = pointsOf(*hin, s);
        EXPECT_LT(maxDistance(pointsOf(*hout, s), fed), 0.05f * maxDistance(hooked(fed, 0.9f), fed)) << s;
        ++checked;
    }
    EXPECT_GT(checked, 20u);
}

TEST(SculptStrands, ScriptedStrokesBrushTheNodesStrandsAnUndoStepEach) {
    Groom g(strandGraph(60.0));
    g.session->setSculptTarget("sculpt");
    const StrandsPtr in = g.input();
    // The view shows the node's strands as the brushes' guides, one a strand.
    const ViewResult& shown = g.session->evaluate(ViewRequest{});
    ASSERT_TRUE(shown.hierarchy);
    EXPECT_EQ(shown.hierarchy->guides().size(), in->strandCount());
    EXPECT_NE(shown.hierarchy->group(shown.hierarchy->guides()[0].group)->name.find("sculpt"), std::string::npos);
    // A comb across the crown: one undo step, the node's edits alone.
    BrushToolSettings s;
    s.setMode("comb");
    s.brush.radius = 0.4f;
    s.brush.strength = 1.0f;
    scriptedStroke(*g.host, s, {{-0.4f, 1.3f, 0.0f}, {0.0f, 1.35f, 0.2f}, {0.4f, 1.3f, 0.4f}});
    ASSERT_EQ(g.host->edits().size(), 1u);
    EXPECT_TRUE(g.host->edits()[0].text(StrokeField::Sculpts).set);
    EXPECT_FALSE(g.host->edits()[0].text(StrokeField::Guides).set);
    const SculptSet set = SculptSet::fromJson(g.session->data().sculptsJson);
    ASSERT_TRUE(set.node("sculpt"));
    const std::size_t combed = set.node("sculpt")->strands.size();
    EXPECT_GT(combed, 3u);
    EXPECT_LT(combed, in->strandCount()) << "only the strands it moved";
    EXPECT_TRUE(set.node("sculpt")->guides.empty());
    const StrandsPtr out = g.output();
    std::size_t moved = 0;
    for (std::size_t k = 0; k < in->strandCount(); ++k) {
        ASSERT_EQ(out->strandPointCount(k), in->strandPointCount(k)) << "point counts kept";
        if (maxDistance(pointsOf(*out, k), pointsOf(*in, k)) > 1e-3f) ++moved;
    }
    EXPECT_GT(moved, 3u);
    // Undo and redo.
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.output(), g.input());
    ASSERT_TRUE(g.host->redo());
    EXPECT_EQ(pointsOf(*g.output(), 0), pointsOf(*out, 0));
    // A second stroke on top keeps the first's strands it does not reach.
    s.setMode("lift");
    scriptedStroke(*g.host, s, {{0.0f, 1.3f, -0.3f}, {0.1f, 1.3f, -0.3f}});
    ASSERT_EQ(g.host->edits().size(), 2u);
    EXPECT_GE(SculptSet::fromJson(g.session->data().sculptsJson).node("sculpt")->strands.size(), combed);
}

TEST(SculptStrands, TheInteractiveBrushPreviewsAndCommitsTheNodesStrands) {
    Groom g(strandGraph(60.0));
    g.session->setSculptTarget("sculpt");
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0, 3.0f, 2.5f}, {0, 1.0f, 0}, 40.0, 1.0), 800, 800);
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 150.0f;
    float cx = 0, cy = 0;
    view.project({0.0f, 1.3f, 0.3f}, cx, cy);
    EXPECT_FALSE(tool.affectedLines(*g.host, view, cx, cy).empty()) << "hover shows the node's strands";
    bool previewed = false;
    std::vector<std::array<float, 2>> line;
    for (int k = 0; k < 12; ++k) line.push_back({cx - 120.0f + 20.0f * static_cast<float>(k), cy});
    ASSERT_TRUE(tool.stroke(*g.host, view, line, 1.0f, 0, [&] {
        previewed = previewed || g.session->hasPreviewSculpt();
        EXPECT_TRUE(g.session->previewSculptsStrands()) << "the CPU's preview, not the GPU's guide stroke";
        const ViewResult& r = g.session->evaluate(ViewRequest{});
        EXPECT_TRUE(r.strands);
        EXPECT_EQ(r.hierarchy.get(), g.session->sculptHierarchy().get());  // the stroke's strands drawn
    }));
    EXPECT_TRUE(previewed);
    EXPECT_FALSE(g.session->hasPreviewSculpt());
    ASSERT_EQ(g.host->edits().size(), 1u);
    EXPECT_TRUE(g.host->edits()[0].text(StrokeField::Sculpts).set);
    EXPECT_NE(g.output(), g.input());
    // The committed edits are the preview's last: the strands as the stroke left them.
    const SculptSet set = SculptSet::fromJson(g.session->data().sculptsJson);
    ASSERT_TRUE(set.node("sculpt"));
    EXPECT_GT(set.node("sculpt")->strands.size(), 3u);
    // Brushes the node does not take are refused, saying why.
    tool.settings.setMode("erase");
    EXPECT_FALSE(tool.stroke(*g.host, view, line));
    EXPECT_FALSE(g.host->warnings().empty());
}

TEST(SculptStrands, FreezingTheNodesStrandsKeepsThemFromItsBrushes) {
    // The Freeze brush on a targeted SculptStrands node: what it paints is kept in the node's
    // edits (frozen, by strand ID), drawn frozen, and its brushes leave it alone. Unfreeze All and
    // Invert Frozen act on the node (an undo step each), not the groom's guides.
    Groom g(strandGraph(60.0));
    g.session->setSculptTarget("sculpt");
    const StrandsPtr in = g.input();
    BrushToolSettings freeze;
    freeze.setMode("paint");
    freeze.brush.attribute = "freeze";
    freeze.brush.radius = 0.4f;
    freeze.brush.strength = 1.0f;
    scriptedStroke(*g.host, freeze, {{0.0f, 1.3f, 0.0f}});
    ASSERT_EQ(g.host->edits().size(), 1u);
    SculptSet set = SculptSet::fromJson(g.session->data().sculptsJson);
    ASSERT_TRUE(set.node("sculpt"));
    const std::map<std::uint64_t, float> frozen = set.node("sculpt")->frozen;
    EXPECT_GT(frozen.size(), 3u);
    EXPECT_TRUE(set.node("sculpt")->strands.empty()) << "nothing sculpted";
    // Drawn frozen.
    const ViewResult& shown = g.session->evaluate(ViewRequest{});
    ASSERT_TRUE(shown.hierarchy);
    const HierarchyGuide* one = shown.hierarchy->guide(frozen.begin()->first);
    ASSERT_TRUE(one);
    ASSERT_TRUE(one->attributes.count("freeze"));
    EXPECT_GT(one->attributes.at("freeze"), 0.0f);
    // A comb over the frozen crown and past it: the frozen strands keep no edit.
    BrushToolSettings comb;
    comb.setMode("comb");
    comb.brush.radius = 0.6f;
    comb.brush.strength = 1.0f;
    scriptedStroke(*g.host, comb, {{-0.5f, 1.3f, 0.0f}, {0.0f, 1.35f, 0.2f}, {0.5f, 1.3f, 0.4f}});
    set = SculptSet::fromJson(g.session->data().sculptsJson);
    std::size_t movedFrozen = 0;
    for (const auto& [id, v] : frozen) {
        if (v >= 0.99f && set.node("sculpt")->strands.count(id)) ++movedFrozen;
    }
    EXPECT_EQ(movedFrozen, 0u);
    EXPECT_EQ(set.node("sculpt")->frozen, frozen) << "the comb kept the freeze";
    // Invert Frozen, then Unfreeze All: the node's, an undo step each.
    const std::size_t steps = g.host->edits().size();
    EXPECT_TRUE(sculptFreezeEdit(*g.host, "invert"));
    ASSERT_EQ(g.host->edits().size(), steps + 1);
    set = SculptSet::fromJson(g.session->data().sculptsJson);
    for (const auto& [id, v] : frozen) {
        if (v >= 0.99f) EXPECT_FALSE(set.node("sculpt")->frozen.count(id)) << "was frozen: now not";
    }
    EXPECT_GT(set.node("sculpt")->frozen.size(), in->strandCount() / 2) << "the rest frozen";
    EXPECT_TRUE(sculptFreezeEdit(*g.host, "unfreeze_all"));
    EXPECT_TRUE(SculptSet::fromJson(g.session->data().sculptsJson).node("sculpt")->frozen.empty());
    ASSERT_TRUE(g.host->undo());
    EXPECT_FALSE(SculptSet::fromJson(g.session->data().sculptsJson).node("sculpt")->frozen.empty());
    // No node targeted: not the node's (the host edits the groom's guides).
    g.session->setSculptTarget("");
    EXPECT_FALSE(sculptFreezeEdit(*g.host, "unfreeze_all"));
}

TEST(SculptStrands, CtrlShiftPaintsTheNodesStrandsTheRestFreeze) {
    // A shaping brush's Ctrl+Shift drag on a targeted SculptStrands node: the strands it passes
    // over stay brushable and every other strand of the node freezes (kept in the node, one undo
    // step); the groom's guide selection untouched. A drag over nothing unfreezes them all.
    Groom g(strandGraph(60.0));
    g.session->setSculptTarget("sculpt");
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0, 3.0f, 2.5f}, {0, 1.0f, 0}, 40.0, 1.0), 800, 800);
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 60.0f;
    tool.settings.brush.level = 1;  // (whatever the brush's level: the node's strands)
    float cx = 0, cy = 0;
    view.project({0.0f, 1.3f, 0.3f}, cx, cy);
    std::vector<std::array<float, 2>> line;
    for (int k = 0; k < 6; ++k) line.push_back({cx - 30.0f + 12.0f * static_cast<float>(k), cy});
    const std::size_t steps = g.host->edits().size();
    ASSERT_TRUE(tool.stroke(*g.host, view, line, 1.0f, kStrokeCtrl | kStrokeShift));
    ASSERT_EQ(g.host->edits().size(), steps + 1);
    EXPECT_FALSE(g.host->edits().back().text(StrokeField::Guides).set) << "the groom's guides untouched";
    const SculptSet set = SculptSet::fromJson(g.session->data().sculptsJson);
    ASSERT_TRUE(set.node("sculpt"));
    const std::size_t total = g.input()->strandCount();
    const std::size_t frozen = set.node("sculpt")->frozen.size();
    EXPECT_GT(frozen, total / 2) << "the rest frozen";
    EXPECT_LT(frozen, total) << "the painted strands kept";
    EXPECT_TRUE(g.host->selectedGuides().empty());
    // Over nothing (off the view): every strand unfrozen.
    std::vector<std::array<float, 2>> away = {{-600.0f, -600.0f}, {-560.0f, -600.0f}};
    ASSERT_TRUE(tool.stroke(*g.host, view, away, 1.0f, kStrokeCtrl | kStrokeShift));
    EXPECT_TRUE(SculptSet::fromJson(g.session->data().sculptsJson).node("sculpt") == nullptr ||
                SculptSet::fromJson(g.session->data().sculptsJson).node("sculpt")->frozen.empty());
}

TEST(SculptStrands, ApplyCostReported) {
    // GROOMLAB_BENCH=1: how long applying strokes over ~500k strands takes (reported, not checked).
    if (!std::getenv("GROOMLAB_BENCH")) GTEST_SKIP() << "set GROOMLAB_BENCH=1 to run";
    Groom g(strandGraph(40000.0));
    const StrandsPtr in = g.input();
    std::vector<std::pair<std::size_t, std::vector<Vec3f>>> sculpted;
    for (const std::size_t s : near(*in, {0.0f, 1.0f, 0.0f}, 0.6f)) sculpted.emplace_back(s, hooked(pointsOf(*in, s), 0.9f));
    SculptEdits edits;
    auto t0 = std::chrono::steady_clock::now();
    recordStrandStroke(edits, *in, sculpted, 1.0f, g.scalp.get());
    auto t1 = std::chrono::steady_clock::now();
    const StrandsPtr out = applyStrandSculpts(in, edits, 1.0f, g.scalp.get());
    auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("[ bench ] %zu strands, %zu brushed: record %.1f ms, apply %.1f ms\n", in->strandCount(), sculpted.size(), ms(t0, t1),
                ms(t1, t2));
    EXPECT_NE(out, in);
    // A stroke's start: the node's strands as guides the brushes shape, and its record's first dab.
    auto t3 = std::chrono::steady_clock::now();
    const std::shared_ptr<GuideHierarchy> h = sculptHierarchy(*strandsAsGuides(*out, Vec3f(1.0f, 0.5f, 0.2f)), "sculpt", g.scalp.get());
    auto t4 = std::chrono::steady_clock::now();
    StrandStrokeRecord record;
    record.begin(in, out, g.scalp.get());
    SculptEdits stroke;
    record.record(stroke, edits, *h, 1.0f);
    auto t5 = std::chrono::steady_clock::now();
    std::printf("[ bench ] stroke start: %zu guides %.1f ms, a dab's record %.1f ms\n", h->guides().size(), ms(t3, t4), ms(t4, t5));
}

}  // namespace
}  // namespace groomlab
