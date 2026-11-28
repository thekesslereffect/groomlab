// SPDX-License-Identifier: Apache-2.0
// The Tube tool's root-first mode in a viewport (root_tube_tool.h), driven as a host drives it:
// presses, drags and releases in a top view of a head, edits committed through the host (one
// undo step each), the overlay a host draws. (Maya's test_root_tubes.py pinned the same with its
// first, Python tool.)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "groomlab/display.h"
#include "groomlab/groom_session.h"
#include "groomlab/root_tube_tool.h"
#include "groomlab/root_tubes.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

Vec3f onSphere(const Vec3f& p) { return p.normalized(); }

std::vector<Vec3f> ring(const Vec3f& center, float radius, int count = 5, float turn = 0.0f) {
    const Vec3f up = onSphere(center);
    const Vec3f a = up.cross({0, 0, 1}).normalized();
    const Vec3f b = up.cross(a);
    std::vector<Vec3f> out;
    for (int k = 0; k < count; ++k) {
        const float t = turn + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(count);
        out.push_back(onSphere(up + a * (radius * std::cos(t)) + b * (radius * std::sin(t))));
    }
    return out;
}

// A head (a unit sphere) with no guides, as a session and its host.
struct Head {
    GroomData data;
    std::unique_ptr<GroomSession> session;
    std::unique_ptr<SessionStrokeHost> host;
    Head() {
        data.scalp = test::makeSphere(48, 96, 1.0f);
        data.hierarchyJson = GuideHierarchy{}.toJson();
        session = std::make_unique<GroomSession>(data);
        host = std::make_unique<SessionStrokeHost>(*session);
    }
    std::shared_ptr<const GuideHierarchy> guides() { return host->authoredHierarchy(); }
    std::vector<GuideId> roots() { return rootTubesOf(*guides()); }
    std::vector<Vec3f> outline(GuideId g) { return rootTubeOutline(*guides(), *host->scalp(), g); }
};

// A top view (looking down -y), 1000 port pixels a unit, orthographic.
ToolView topView() {
    const double pv[4][4] = {{2, 0, 0, 0}, {0, 0, -2, 0}, {0, -0.1, 0, 0}, {0, 0, 0, 1}};
    std::array<double, 16> m{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) m[static_cast<std::size_t>(row * 4 + col)] = pv[col][row];
    }
    return ToolView::fromMatrix(m, 1000, 1000);
}

std::array<float, 2> px(const ToolView& view, const Vec3f& p) {
    float x = 0, y = 0;
    EXPECT_TRUE(view.project(p, x, y));
    return {x, y};
}

void click(RootTubeTool& tool, Head& head, const ToolView& view, const Vec3f& at, unsigned modifiers = 0) {
    const std::array<float, 2> s = px(view, at);
    tool.press(*head.host, view, s[0], s[1], modifiers);
    tool.release(*head.host, view);
}

// A root drawn by the tool, straight off the scalp (no path).
GuideId draw(RootTubeTool& tool, Head& head, const ToolView& view, const std::vector<Vec3f>& cvs) {
    tool.settings.drawPath = false;
    tool.start(*head.host, RootTubeTool::Mode::Place);
    for (const Vec3f& p : cvs) click(tool, head, view, p);
    click(tool, head, view, cvs.front());  // closed: made at once
    const std::vector<GuideId> roots = head.roots();
    return roots.empty() ? kNoGuide : roots.back();
}

float nearest(const Vec3f& p, const std::vector<Vec3f>& pts) {
    float best = 1e30f;
    for (const Vec3f& q : pts) best = std::min(best, (p - q).length());
    return best;
}

TEST(RootTubeTool, DrawsARootThenItsPathAndUndoBringsTheOutlineBack) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    tool.start(*head.host, RootTubeTool::Mode::Place);
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Place);
    const std::vector<Vec3f> cvs = ring({0, 1, 0}, 0.15f);
    for (const Vec3f& p : cvs) click(tool, head, view, p);
    EXPECT_EQ(tool.outlineCvs(), 5u);
    click(tool, head, view, cvs.front());  // the first CV again: closed
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Draw);
    EXPECT_TRUE(head.roots().empty()) << "nothing made before its path";
    // The path: a drag out from the root's middle.
    const Vec3f middle = scalpPointNormal(*head.host->scalp(), outlineCentroid(cvs)).point;
    std::array<float, 2> s = px(view, middle);
    tool.press(*head.host, view, s[0], s[1], 0);
    for (int k = 1; k <= 30; ++k) tool.drag(*head.host, view, s[0] + 15.0f * k, s[1] + 4.0f * k);
    tool.release(*head.host, view);
    ASSERT_EQ(head.roots().size(), 1u) << tool.message();
    const GuideId made = head.roots().front();
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Place);
    EXPECT_EQ(head.host->edits().size(), 1u) << "one undo step";
    for (std::size_t i = 0; i < cvs.size(); ++i) EXPECT_LT((head.outline(made)[i] - cvs[i]).length(), 0.003f);
    // Undo: the tube goes, its outline comes back to draw the path again; redo puts it away.
    ASSERT_TRUE(head.host->undo());
    tool.sync(*head.host);
    EXPECT_TRUE(head.roots().empty());
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Draw);
    EXPECT_EQ(tool.outlineCvs(), 5u);
    ASSERT_TRUE(head.host->redo());
    tool.sync(*head.host);
    EXPECT_EQ(head.roots().size(), 1u);
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Place);
    EXPECT_EQ(tool.outlineCvs(), 0u);
    // A short stroke: no tube, the path asked for again.
    tool.settings.drawPath = true;
    for (const Vec3f& p : ring({0.4f, 0.9f, 0.0f}, 0.1f)) click(tool, head, view, p);
    click(tool, head, view, ring({0.4f, 0.9f, 0.0f}, 0.1f).front());
    s = px(view, scalpPointNormal(*head.host->scalp(), outlineCentroid(ring({0.4f, 0.9f, 0.0f}, 0.1f))).point);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.release(*head.host, view);
    EXPECT_EQ(head.roots().size(), 1u);
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Draw);
    // Undo CV: the path dropped, then CVs taken back one by one.
    tool.undoCv(*head.host);
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Place);
    tool.undoCv(*head.host);
    EXPECT_EQ(tool.outlineCvs(), 4u);
}

TEST(RootTubeTool, ANewCvSnapsOntoANeighbourSoRootsSeal) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 4));
    ASSERT_NE(a, kNoGuide) << tool.message();
    const std::vector<Vec3f> cvs = head.outline(a);
    // A root drawn beside it on two of its CVs (clicked 6 pixels off them).
    tool.start(*head.host, RootTubeTool::Mode::Place);
    const Vec3f off(0.004f, 0.0f, 0.004f);
    std::vector<Vec3f> next{cvs[0] + off, cvs[1] + off, onSphere(cvs[1] + (cvs[1] - cvs[2])), onSphere(cvs[0] + (cvs[0] - cvs[3]))};
    for (const Vec3f& p : next) click(tool, head, view, p);
    EXPECT_NE(tool.message().find("snapped"), std::string::npos) << tool.message();
    click(tool, head, view, next.front());
    ASSERT_EQ(head.roots().size(), 2u) << tool.message();
    const GuideId b = head.roots().back();
    const std::vector<int> sealed = sealedCvs(head.outline(b), {cvs});
    EXPECT_EQ(sealed.size(), 2u) << "its first two CVs on its neighbour's";
}

TEST(RootTubeTool, EditMovesAddsAndTakesOutCvsAndFinishesInOneStep) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 4));
    const std::vector<Vec3f> ca = head.outline(a);
    tool.start(*head.host, RootTubeTool::Mode::Place);
    std::vector<Vec3f> next{ca[0], ca[1], onSphere(ca[1] + (ca[1] - ca[2])), onSphere(ca[0] + (ca[0] - ca[3]))};
    for (const Vec3f& p : next) click(tool, head, view, p);
    click(tool, head, view, next.front());
    ASSERT_EQ(head.roots().size(), 2u);
    const GuideId b = head.roots().back();
    const std::size_t steps = head.host->edits().size();

    tool.start(*head.host, RootTubeTool::Mode::Edit);
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Edit);
    EXPECT_FALSE(tool.hidesGuides()) << "Edit roots leaves the guides to the Visibility switches";
    // Ctrl+click the edge both roots share: a CV added on both, dragged out together.
    const Vec3f mid = onSphere((ca[0] + ca[1]) * 0.5f);
    std::array<float, 2> s = px(view, mid);
    tool.press(*head.host, view, s[0], s[1], kStrokeCtrl);
    EXPECT_NE(tool.message().find("2 sealed outlines"), std::string::npos) << tool.message();
    tool.drag(*head.host, view, s[0] + 20.0f, s[1]);
    tool.release(*head.host, view);
    // Ctrl+click a CV of root a only it has: taken out of it.
    s = px(view, ca[2]);
    tool.press(*head.host, view, s[0], s[1], kStrokeCtrl);
    tool.release(*head.host, view);
    // Undo CV puts that CV back; Finish gives both roots their outlines in one step.
    tool.undoCv(*head.host);
    EXPECT_EQ(tool.finishEdit(*head.host), 2) << tool.message();
    EXPECT_EQ(head.host->edits().size(), steps + 1) << "one undo step";
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Place);
    EXPECT_EQ(head.outline(a).size(), 5u) << "a CV more, the one taken out back";
    EXPECT_EQ(head.outline(b).size(), 5u);
    EXPECT_EQ(sealedCvs(head.outline(b), {head.outline(a)}).size(), 3u) << "still sealed, the new CV too";
    // A root keeps 3 CVs at least.
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    const GuideId c = draw(tool, head, view, ring({-0.5f, 0.85f, 0.0f}, 0.08f, 3));
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    s = px(view, head.outline(c)[0]);
    tool.press(*head.host, view, s[0], s[1], kStrokeCtrl);
    EXPECT_NE(tool.message().find("at least 3"), std::string::npos) << tool.message();
    // Leaving the tool while editing keeps the edits.
    s = px(view, head.outline(c)[1]);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.drag(*head.host, view, s[0] + 15.0f, s[1] + 10.0f);
    tool.release(*head.host, view);
    const Vec3f before = head.outline(c)[1];
    tool.leave(*head.host);
    EXPECT_GT((head.outline(c)[1] - before).length(), 0.005f) << "finished on leaving";
}

TEST(RootTubeTool, EditSnapsOntoOtherRootsOnlyAndCtrlClickTakesACvOutOfTheRootMeant) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 5));
    const std::vector<Vec3f> ca = head.outline(a);
    tool.start(*head.host, RootTubeTool::Mode::Place);
    std::vector<Vec3f> next{ca[0], ca[1], onSphere(ca[1] + (ca[1] - ca[2])), onSphere(ca[0] + (ca[0] - ca[3]))};
    for (const Vec3f& p : next) click(tool, head, view, p);
    click(tool, head, view, next.front());
    const GuideId b = head.roots().back();
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    // A CV of a dragged right next to another of its own CVs: it does not snap onto it.
    std::array<float, 2> s = px(view, ca[3]);
    const std::array<float, 2> own = px(view, ca[2]);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.drag(*head.host, view, own[0] + 5.0f, own[1]);
    tool.release(*head.host, view);
    EXPECT_EQ(tool.finishEdit(*head.host), 1);
    const std::vector<Vec3f> moved = head.outline(a);
    EXPECT_GT((moved[3] - moved[2]).length(), 0.003f) << "never onto its own CV";
    // Ctrl+click on a CV a and b share: out of the root touched last (b) only.
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    s = px(view, head.outline(b)[2]);  // (a CV of b alone: b touched)
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.release(*head.host, view);
    s = px(view, head.outline(b)[0]);  // (shared with a)
    tool.press(*head.host, view, s[0], s[1], kStrokeCtrl);
    EXPECT_NE(tool.message().find("root " + std::to_string(b)), std::string::npos) << tool.message();
    EXPECT_NE(tool.message().find("neighbour keeps its own"), std::string::npos) << tool.message();
    tool.finishEdit(*head.host);
    EXPECT_EQ(head.outline(b).size(), 3u);
    EXPECT_EQ(head.outline(a).size(), 5u) << "a keeps its CV";
}

TEST(RootTubeTool, AnUndoWhileEditingTakesTheRootsAsTheGroomHasThem) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 5));
    const GuideId b = draw(tool, head, view, ring({0.4f, 0.9f, 0.0f}, 0.08f, 5));
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    // b edited (not finished); then the groom's undo takes b away.
    std::array<float, 2> s = px(view, head.outline(b)[0]);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.drag(*head.host, view, s[0] + 10.0f, s[1]);
    tool.release(*head.host, view);
    ASSERT_TRUE(head.host->undo());
    tool.sync(*head.host);
    EXPECT_NE(tool.message().find("Undo changed 1 root"), std::string::npos) << tool.message();
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Edit);
    // a still edits; Finish works on what is left.
    s = px(view, head.outline(a)[1]);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.drag(*head.host, view, s[0] - 12.0f, s[1] + 6.0f);
    tool.release(*head.host, view);
    EXPECT_EQ(tool.finishEdit(*head.host), 1) << tool.message();
    EXPECT_EQ(head.roots(), std::vector<GuideId>{a});
}

TEST(RootTubeTool, SelectCarriesRootsDuplicatesAndDeletesThem) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 5));
    const GuideId b = draw(tool, head, view, ring({0.35f, 0.93f, 0.0f}, 0.08f, 5));
    ASSERT_EQ(head.roots().size(), 2u);
    tool.start(*head.host, RootTubeTool::Mode::Select);
    const Vec3f ma = rootTubeCenter(*head.guides(), *head.host->scalp(), a).point;
    const Vec3f mb = rootTubeCenter(*head.guides(), *head.host->scalp(), b).point;
    // A click on a's middle selects it; Shift+click b adds it; a drag carries both.
    click(tool, head, view, ma);
    EXPECT_EQ(tool.selected(), std::vector<GuideId>{a});
    std::array<float, 2> s = px(view, mb);
    tool.press(*head.host, view, s[0], s[1], kStrokeShift);
    EXPECT_EQ(tool.selected(), (std::vector<GuideId>{a, b}));
    tool.release(*head.host, view);
    // Shift+click on the Select root button: every root shown (from Place too) selected.
    tool.start(*head.host, RootTubeTool::Mode::Place);
    EXPECT_EQ(tool.selectShown(*head.host), 2) << tool.message();
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Select);
    std::vector<GuideId> all = tool.selected();
    std::sort(all.begin(), all.end());
    EXPECT_EQ(all, (std::vector<GuideId>{std::min(a, b), std::max(a, b)}));
    tool.press(*head.host, view, s[0], s[1], kStrokeShift);  // (b out, then in again: a, b as before)
    tool.release(*head.host, view);
    tool.press(*head.host, view, s[0], s[1], kStrokeShift);
    const std::size_t steps = head.host->edits().size();
    tool.drag(*head.host, view, s[0] - 60.0f, s[1] + 30.0f);
    tool.release(*head.host, view);
    EXPECT_EQ(head.host->edits().size(), steps + 1) << "one undo step: " << tool.message();
    EXPECT_GT((rootTubeCenter(*head.guides(), *head.host->scalp(), a).point - ma).length(), 0.03f) << "carried";
    // Duplicate: the copies follow the cursor and a click sets them down.
    ASSERT_TRUE(tool.beginDuplicate(*head.host));
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Duplicate);
    const Vec3f there = onSphere({-0.4f, 0.85f, 0.2f});
    s = px(view, there);
    tool.hover(*head.host, view, s[0], s[1]);
    EXPECT_GT(tool.overlay(*head.host).size(), 4u) << "the copies shown where they would go";
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.release(*head.host, view);
    EXPECT_EQ(head.roots().size(), 4u) << tool.message();
    EXPECT_EQ(tool.mode(), RootTubeTool::Mode::Select);
    EXPECT_EQ(tool.selected().size(), 2u) << "the copies selected";
    // Delete takes the copies away.
    EXPECT_EQ(tool.deleteSelected(*head.host), 2);
    EXPECT_EQ(head.roots(), (std::vector<GuideId>{a, b}));
    EXPECT_FALSE(tool.beginDuplicate(*head.host)) << "nothing selected";
    // An undo reloads the roots.
    ASSERT_TRUE(head.host->undo());
    tool.sync(*head.host);
    EXPECT_EQ(head.roots().size(), 4u);
}

TEST(RootTubeTool, TheOverlayDrawsEachRootInItsOwnColorLaidOnTheScalp) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 4));
    draw(tool, head, view, ring({0.35f, 0.93f, 0.0f}, 0.08f, 4));
    tool.start(*head.host, RootTubeTool::Mode::Edit);
    std::array<float, 2> s = px(view, head.outline(a)[0]);
    tool.press(*head.host, view, s[0], s[1], 0);
    tool.release(*head.host, view);
    const std::vector<RootTubeTool::Item> items = tool.overlay(*head.host);
    ASSERT_EQ(items.size(), 2u);
    for (std::size_t k = 0; k < items.size(); ++k) {
        const RootTubeTool::Item& item = items[k];
        EXPECT_EQ(item.color, rootTubeColor(k)) << "its own color";
        EXPECT_TRUE(item.style & RootTubeTool::Filled);
        EXPECT_TRUE(item.style & RootTubeTool::Dots);
        EXPECT_EQ(item.dots.size(), 4u);
        EXPECT_GE(item.edge.size(), 4u);
        ASSERT_FALSE(item.triangles.empty());
        EXPECT_EQ(item.triangles.size() % 3, 0u);
        for (const Vec3f& p : item.edge) EXPECT_NEAR(p.length(), 1.0f, 0.01f) << "laid on the scalp";
        for (const Vec3f& p : item.triangles) EXPECT_NEAR(p.length(), 1.0f, 0.01f);
    }
    EXPECT_TRUE(items[0].style & RootTubeTool::Highlight) << "the root touched";
    EXPECT_FALSE(items[1].style & RootTubeTool::Highlight);
    EXPECT_EQ(RootTubeTool::edgeColor(items[0]), RootTubeTool::kHighlight);
    tool.leave(*head.host);
    EXPECT_TRUE(tool.overlay(*head.host).empty()) << "nothing drawn once left";
    EXPECT_FALSE(tool.hidesGuides());
}

TEST(RootTubeTool, TheBrushToolHandsItsEventsOn) {
    Head head;
    const ToolView view = topView();
    BrushTool brush;
    brush.settings.setMode("roots");
    EXPECT_EQ(brush.settings.modeName(), "roots");
    brush.roots().settings.drawPath = false;
    brush.roots().start(*head.host, RootTubeTool::Mode::Place);
    const std::vector<Vec3f> cvs = ring({0, 1, 0}, 0.12f);
    for (const Vec3f& p : cvs) {
        const std::array<float, 2> s = px(view, p);
        brush.press(*head.host, view, s[0], s[1], 1.0f, 0);
        brush.release(*head.host, view);
    }
    const std::array<float, 2> s = px(view, cvs.front());
    brush.press(*head.host, view, s[0], s[1], 1.0f, 0);
    brush.release(*head.host, view);
    EXPECT_EQ(head.roots().size(), 1u) << brush.roots().message();
}

// The overlay as display geometry (Blender's and the app's): the CVs placed as dots, the outline
// so far as edges; a closed root's fill with its middle as a marker in Select.
TEST(RootTubeTool, TheOverlayAsGeometryForBlenderAndTheApp) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    tool.start(*head.host, RootTubeTool::Mode::Place);
    const std::vector<Vec3f> cvs = ring({0, 1, 0}, 0.1f, 4);
    for (std::size_t k = 0; k < 3; ++k) click(tool, head, view, cvs[k]);
    RootOverlayGeometry g = rootOverlayGeometry(tool.overlay(*head.host));
    EXPECT_EQ(g.dots.positions.size(), 9u) << "three CVs";
    EXPECT_TRUE(g.dots.points);
    EXPECT_EQ(g.dots.indices.size(), 3u);
    EXPECT_EQ(g.dots.colors.size(), 12u);
    const DisplayGeometry& outline = g.edges.indices.empty() ? g.highlighted : g.edges;  // (the one drawn: highlighted)
    EXPECT_FALSE(outline.indices.empty());
    EXPECT_EQ(outline.indices.size() % 2, 0u) << "line pairs";
    tool.leave(*head.host);
    draw(tool, head, view, cvs);
    tool.start(*head.host, RootTubeTool::Mode::Select);
    g = rootOverlayGeometry(tool.overlay(*head.host));
    EXPECT_EQ(g.markers.indices.size(), 1u) << "the root's middle";
    EXPECT_FALSE(g.fills.indices.empty());
    EXPECT_TRUE(g.fills.triangles);
    EXPECT_EQ(g.fills.indices.size() % 3, 0u);
    EXPECT_FLOAT_EQ(g.fills.colors[3], RootTubeTool::kFillOpacity);
    tool.leave(*head.host);
    EXPECT_TRUE(rootOverlayGeometry(tool.overlay(*head.host)).empty());
}

// The Tube card's Colors, as every host draws them (buildTubeDisplay): each tube in its root tube's
// color on the wires, or on the surfaces (the wires gray) with Shaded; the selected ones apart.
TEST(RootTubeTool, TheTubesTakeTheirRootTubesColorsInEveryHost) {
    Head head;
    const ToolView view = topView();
    RootTubeTool tool;
    const GuideId a = draw(tool, head, view, ring({0, 1, 0}, 0.1f, 4));
    const GuideId b = draw(tool, head, view, ring({0.35f, 0.93f, 0.0f}, 0.08f, 4));
    const GuideHierarchy& h = *head.guides();
    const std::vector<GuideId> ids = h.rootTubeIds();
    ASSERT_EQ(ids.size(), 2u);
    auto colorOf = [&](GuideId g) { return rootTubeColor(static_cast<std::size_t>(std::find(ids.begin(), ids.end(), g) - ids.begin())); };
    auto has = [](const std::vector<float>& colors, const std::array<float, 3>& c) {
        for (std::size_t i = 0; i + 3 < colors.size(); i += 4) {
            if (std::fabs(colors[i] - c[0]) < 1e-5f && std::fabs(colors[i + 1] - c[1]) < 1e-5f && std::fabs(colors[i + 2] - c[2]) < 1e-5f) return true;
        }
        return false;
    };
    TubeDisplayOptions options;
    TubeDisplay plain = buildTubeDisplay(h, options, {});
    EXPECT_FALSE(plain.wires.positions.empty());
    EXPECT_TRUE(plain.wires.colors.empty()) << "Colors off: the plain wire color";
    EXPECT_TRUE(plain.surfaces.positions.empty());

    options.colors = true;
    TubeDisplay colored = buildTubeDisplay(h, options, {});
    ASSERT_EQ(colored.wires.colors.size(), colored.wires.positions.size() / 3 * 4);
    EXPECT_TRUE(has(colored.wires.colors, colorOf(a)));
    EXPECT_TRUE(has(colored.wires.colors, colorOf(b)));

    options.shaded = true;
    TubeDisplay shaded = buildTubeDisplay(h, options, {b});
    EXPECT_TRUE(shaded.wires.colors.empty()) << "Shaded: the colors on the surfaces, the wires gray";
    EXPECT_FLOAT_EQ(shaded.wireColor[0], 0.62f);
    ASSERT_EQ(shaded.surfaces.colors.size(), shaded.surfaces.positions.size() / 3 * 4);
    EXPECT_TRUE(shaded.surfaces.scaleSurface);
    EXPECT_TRUE(has(shaded.surfaces.colors, colorOf(a)));
    EXPECT_TRUE(has(shaded.surfaces.colors, colorOf(b)));
    EXPECT_FALSE(shaded.selectedWires.positions.empty()) << "the selected tube's wires apart";

    options.wireframe = false;
    EXPECT_TRUE(buildTubeDisplay(h, options, {}).wires.positions.empty()) << "Wireframe off: the surfaces alone";
}

}  // namespace
}  // namespace groomlab
