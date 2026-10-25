// SPDX-License-Identifier: Apache-2.0
// The core's interactive tools (stroke.h) driven from screen points on a GroomSession, scripted
// strokes, edits, undo, and guide picking in a view (guide_selection.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "groomlab/brush.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_selection.h"
#include "groomlab/random.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

TEST(Stroke, ToolColorsAreEveryHosts) {
    // The ring, the path and the hover lines every host draws (stroke.h): Maya's colors.
    using Color = std::array<float, 3>;
    BrushToolSettings s;
    s.setMode("comb");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.8f, 0.2f}));
    EXPECT_EQ(brushHoverColor(s), (Color{1.0f, 1.0f, 1.0f}));
    s.setMode("erase");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.3f, 0.3f}));
    EXPECT_EQ(brushHoverColor(s), (Color{1.0f, 0.35f, 0.3f}));
    s.setMode("place");
    EXPECT_EQ(brushRingColor(s), (Color{0.3f, 1.0f, 0.4f}));
    s.setMode("part");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.3f, 0.3f}));
    s.setMode("flow");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.55f, 0.1f}));
    EXPECT_EQ(brushPathColor(s), (Color{1.0f, 0.3f, 0.3f}));  // a part or flow stroke's path: red
}

// The unit sphere with UVs (u around, v from the bottom pole up).
std::shared_ptr<const ScalpMesh> sphereWithUVs(int rings = 24, int segments = 48) {
    auto base = test::makeSphere(rings, segments);
    auto mesh = std::make_shared<ScalpMesh>(*base);
    std::vector<std::array<float, 2>> uvs;
    for (const ScalpMesh::Triangle& t : mesh->triangles()) {
        for (const int v : {t[0], t[1], t[2]}) {
            const int r = v / (segments + 1), s = v % (segments + 1);
            uvs.push_back({static_cast<float>(s) / segments, 1.0f - static_cast<float>(r) / rings});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

GuideHierarchy groomOn(const ScalpMesh& scalp, int primaries, std::size_t children, std::uint64_t seed = 5) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(seed);
    for (int i = 0; i < primaries; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.3f, 1), rng.uniform(0.2f, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.05f, 0.1f, 0.05f), 10));
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

struct Groom {
    MeshPtr scalp;
    std::unique_ptr<GroomSession> session;
    std::unique_ptr<SessionStrokeHost> host;
    explicit Groom(int primaries = 40, std::size_t children = 0, const std::string& maps = {}) : scalp(sphereWithUVs()) {
        GroomData data;
        data.scalp = scalp;
        data.hierarchyJson = groomOn(*scalp, primaries, children).toJson();
        session = std::make_unique<GroomSession>(std::move(data));
        if (!maps.empty()) session->setScalpMapsJson(maps);
        host = std::make_unique<SessionStrokeHost>(*session);
    }
    std::string guides() const { return session->data().hierarchyJson; }
};

ToolView frontView() { return ToolView::fromMatrix(test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.0), 800, 800); }

// The view's pixels of `n` points from a to b (world).
std::vector<std::array<float, 2>> screenLine(const ToolView& view, const Vec3f& a, const Vec3f& b, int n) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    view.project(a, ax, ay);
    view.project(b, bx, by);
    std::vector<std::array<float, 2>> out;
    for (int k = 0; k < n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n - 1);
        out.push_back({std::round(ax + (bx - ax) * t), std::round(ay + (by - ay) * t)});
    }
    return out;
}

// The same view without its matrix: the host's projection and rays (serial weights, nothing
// culled by bounds), as a host whose matrix could not be checked.
ToolView hostOnly(const ToolView& view) {
    ToolView out;
    const ViewProjection projection = view.projection;
    out.hostProject = [projection](const Vec3f& p, float& x, float& y) { return projection.project(p, x, y); };
    out.hostRay = [view](float x, float y, Vec3f& o, Vec3f& d) { return view.ray(x, y, o, d); };
    out.hasCamera = view.hasCamera;
    out.eye = view.eye;
    out.viewDirection = view.viewDirection;
    return out;
}

TEST(ToolView, CameraAndRaysFromTheMatrix) {
    const Vec3f eye{0, 1.2f, 4}, target{0, 0.6f, 0};
    const ToolView view = frontView();
    ASSERT_TRUE(view.hasCamera);
    EXPECT_FALSE(view.orthographic);
    EXPECT_NEAR((view.eye - eye).length(), 0.0f, 1e-3f);
    EXPECT_NEAR(view.viewDirection.dot((target - eye).normalized()), 1.0f, 1e-5f);
    // The ray under a point's pixel passes through it.
    const Vec3f p{0.3f, 0.9f, 0.6f};
    float x = 0, y = 0;
    ASSERT_TRUE(view.project(p, x, y));
    Vec3f o, d;
    ASSERT_TRUE(view.ray(x, y, o, d));
    const Vec3f toP = p - o;
    EXPECT_NEAR((o + d * toP.dot(d) - p).length(), 0.0f, 1e-4f);
}

// The brush ring's world radius (what every host draws and the brush's map dabs touch): the ray
// a radius to the right of the center's pixel meets the plane through the center facing the
// camera there, so that point projects a radius away; none behind the camera.
TEST(ToolView, WorldRadiusIsTheRingsEdge) {
    const ToolView view = frontView();
    const Vec3f center{0.1f, 0.7f, 0.2f};
    float x = 0, y = 0;
    ASSERT_TRUE(view.project(center, x, y));
    for (float pixels : {1.0f, 25.0f, 60.0f, 200.0f}) {
        const float radius = worldRadiusAt(view, center, x, y, pixels);
        ASSERT_GT(radius, 0.0f);
        // A point that far from the center across the view lands a radius away on screen.
        const Vec3f across = view.viewDirection.cross(Vec3f{0, 1, 0}).normalized();
        float px = 0, py = 0, qx = 0, qy = 0;
        ASSERT_TRUE(view.project(center + across * radius, px, py));
        ASSERT_TRUE(view.project(center - across * radius, qx, qy));
        EXPECT_NEAR(0.5f * std::hypot(px - qx, py - qy), pixels, 0.02f * pixels + 0.05f) << pixels;
    }
    EXPECT_EQ(worldRadiusAt(view, Vec3f{0, 1.2f, 9}, x, y, 60.0f), 0.0f);  // behind the camera
}

// The settings every host sets, kept in range the same way (Maya's flags, Blender's settings).
TEST(Stroke, ToolSettingsStayInRange) {
    BrushToolSettings s;
    s.setScreenRadius(0.2f);
    EXPECT_EQ(s.screenRadius, 1.0f);
    s.setSegments(0);
    EXPECT_EQ(s.place.segments, 1);
    s.setPaintResolution(1);
    EXPECT_EQ(s.paintResolution, 4);
    s.setPaintResolution(100000);
    EXPECT_EQ(s.paintResolution, 16384);
    s.setHardness(1.5f);
    EXPECT_EQ(s.brush.hardness, 1.0f);
    s.setSmoothStrength(-1.0f);
    EXPECT_EQ(s.smoothStrength, 0.0f);
    s.setPaintColor({-0.5f, 0.25f, 2.0f, 1.0f});
    EXPECT_EQ(s.paintColor, (PaintColor{0.0f, 0.25f, 1.0f, 1.0f}));
}

TEST(ToolView, PixelsAsWorldToViewGivesThem) {
    const ViewProjection view(test::lookAtProjection({0, 1, 5}, {0, 0.5f, 0}, 40.0, 1.0), 640, 480);
    int ix = 0, iy = 0;
    float fx = 0, fy = 0;
    const Vec3f p{0.37f, 0.81f, 0.2f};
    ASSERT_TRUE(view.projectPixel(p, ix, iy));
    ASSERT_TRUE(view.project(p, fx, fy));
    EXPECT_EQ(ix, static_cast<int>(fx));  // truncated
    EXPECT_EQ(iy, static_cast<int>(fy));
    EXPECT_FALSE(view.projectPixel({50.0f, 0.5f, 0.0f}, ix, iy));  // outside the view: no pixel
    EXPECT_TRUE(view.project({50.0f, 0.5f, 0.0f}, fx, fy));        // ... though in front of the camera
    EXPECT_FALSE(view.projectPixel({0.0f, 1.0f, 9.0f}, ix, iy));   // behind it
}

TEST(Stroke, CombOnASessionCommitsOneUndoableEdit) {
    Groom g;
    const std::string before = g.guides();
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 120.0f;
    ASSERT_TRUE(tool.stroke(*g.host, view, screenLine(view, {-0.5f, 1.1f, 0.6f}, {0.5f, 1.1f, 0.6f}, 16)));
    ASSERT_EQ(g.host->edits().size(), 1u);
    const StrokeEdit& edit = g.host->edits().back();
    EXPECT_EQ(edit.mode, "comb");
    ASSERT_TRUE(edit.text(StrokeField::Guides).set);
    EXPECT_FALSE(edit.text(StrokeField::Parts).set);
    EXPECT_EQ(edit.text(StrokeField::Guides).before, before);
    const std::string after = g.guides();
    EXPECT_NE(after, before);
    EXPECT_EQ(after, edit.text(StrokeField::Guides).after);
    EXPECT_FALSE(g.session->hasPreview());  // the preview went with the commit
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.guides(), before);
    ASSERT_TRUE(g.host->redo());
    EXPECT_EQ(g.guides(), after);
    // A stroke off the groom changes nothing and commits nothing.
    EXPECT_FALSE(tool.stroke(*g.host, view, {{2, 2}, {30, 2}}));
    EXPECT_EQ(g.host->edits().size(), 1u);
}

TEST(Stroke, UndoBringsBackGuidesAndMapsHandedOverAsObjects) {
    // A session given its guides as a hierarchy and its maps as maps (no texts): undo sets them
    // back as they were (the maps as they are stored: values to 1/1000).
    Groom g;
    const std::string original = g.session->storedHierarchy()->toJson();
    g.session->setHierarchy(std::make_shared<const GuideHierarchy>(*g.session->storedHierarchy()));
    ScalpMaps maps;
    maps.maps["crown"] = ScalpMapEntry{"vertices", std::vector<float>(g.scalp->pointCount(), 0.123456f), ""};
    g.session->setScalpMaps(maps);
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 120.0f;
    ASSERT_TRUE(tool.stroke(*g.host, view, screenLine(view, {-0.5f, 1.1f, 0.6f}, {0.5f, 1.1f, 0.6f}, 12)));
    tool.settings.setMode("paint");
    tool.settings.paintKind = "vertices";
    tool.settings.brush.attribute = "crown";
    ASSERT_TRUE(tool.stroke(*g.host, view, screenLine(view, Vec3f(-0.2f, 0.9f, 0.5f).normalized(), Vec3f(0.2f, 0.9f, 0.5f).normalized(), 6)));
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.session->scalpMaps().maps.at("crown").values, ScalpMaps::fromJson(maps.toJson()).maps.at("crown").values);
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.session->storedHierarchy()->toJson(), original);
}

TEST(Stroke, TheMatrixAndTheHostsProjectionStrokeAlike) {
    // Weights read from the matrix (parallel, culled by bounds) or through the host's projection
    // (serial, nothing culled): the same guides, by the same amounts, Mask by scalp on.
    const ToolView view = frontView();
    std::string results[2];
    for (int k = 0; k < 2; ++k) {
        Groom g(30, 4);
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.brush.level = -1;
        tool.settings.screenRadius = 150.0f;
        const ToolView used = k == 0 ? view : hostOnly(view);
        ASSERT_EQ(used.parallelWeights(), k == 0);
        ASSERT_TRUE(tool.stroke(*g.host, used, screenLine(view, {-0.6f, 1.0f, 0.7f}, {0.6f, 1.0f, 0.7f}, 20)));
        results[k] = g.guides();
    }
    EXPECT_EQ(results[0], results[1]);
}

TEST(Stroke, ScriptedCombIsTheCoreStroke) {
    Groom g;
    const std::vector<Vec3f> path{{-0.4f, 1.1f, 0.5f}, {0.0f, 1.2f, 0.55f}, {0.4f, 1.1f, 0.5f}};
    BrushToolSettings s;
    s.setMode("comb");
    s.brush.radius = 0.4f;
    s.brush.strength = 0.8f;
    GuideHierarchy direct(*g.session->authoredHierarchy());
    applyBrushStroke(direct, g.scalp.get(), s.brush, path);
    EXPECT_EQ(scriptedStroke(*g.host, s, path), 0u);
    EXPECT_EQ(g.guides(), direct.toJson());
    ASSERT_TRUE(g.host->undo());
    EXPECT_NE(g.guides(), direct.toJson());
}

TEST(Stroke, CtrlInvertsAndShiftDragSmoothsForOneStroke) {
    const ToolView view = frontView();
    const auto path = screenLine(view, {-0.3f, 1.1f, 0.6f}, {0.3f, 1.1f, 0.6f}, 12);
    // Ctrl: Lift lays the guides down instead.
    std::string lifted, laid;
    for (const bool ctrl : {false, true}) {
        Groom g;
        BrushTool tool;
        tool.settings.setMode("lift");
        tool.settings.screenRadius = 150.0f;
        ASSERT_TRUE(tool.stroke(*g.host, view, path, 1.0f, ctrl ? kStrokeCtrl : 0u));
        (ctrl ? laid : lifted) = g.guides();
    }
    EXPECT_NE(lifted, laid);
    // Shift: a click does nothing (no stroke, no step), a drag smooths while the tool stays the Comb.
    Groom g(20, 3);
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 150.0f;
    tool.press(*g.host, view, path[0][0], path[0][1], 1.0f, kStrokeShift);
    EXPECT_FALSE(tool.stroking());
    EXPECT_EQ(tool.release(*g.host, view), StrokeRelease::None);
    const std::size_t clicked = g.host->edits().size();
    EXPECT_EQ(clicked, 0u);
    tool.press(*g.host, view, path[0][0], path[0][1], 1.0f, kStrokeShift);
    for (std::size_t i = 1; i < path.size(); ++i) tool.drag(*g.host, view, path[i][0], path[i][1], 1.0f);
    EXPECT_TRUE(tool.stroking());
    EXPECT_EQ(tool.settings.brush.mode, BrushMode::Smooth);
    EXPECT_EQ(tool.toolModeName(), "comb");
    EXPECT_EQ(tool.release(*g.host, view), StrokeRelease::Stroke);
    EXPECT_EQ(tool.settings.brush.mode, BrushMode::Comb);
    ASSERT_EQ(g.host->edits().size(), clicked + 1);
    EXPECT_EQ(g.host->edits().back().mode, "smooth");
}

TEST(Stroke, CtrlTakesHairAwayOnADensityOrWidthMapWhateverTheValue) {
    // The tester's report: with the value lowered, Ctrl painted hair back in (toward 1 - value).
    // Ctrl takes away (toward 0) whatever the value; without Ctrl, toward the value. The map is
    // the one + New Map > for a setting makes (paintParamMap).
    const ToolView view = frontView();
    const auto path = screenLine(view, Vec3f(-0.2f, 0.9f, 0.5f).normalized(), Vec3f(0.2f, 0.9f, 0.5f).normalized(), 8);
    for (const char* brush : {"density", "width"}) {
        for (const bool ctrl : {false, true}) {
            SCOPED_TRACE(std::string(brush) + (ctrl ? " with Ctrl" : ""));
            Groom g(4);
            BrushTool tool;
            const PaintableSetting* setting = paintableSetting(brush);
            ASSERT_TRUE(setting);
            paintParamMap(*g.host, tool.settings, firstNodeOfType(g.session->data().graphJson, setting->type), setting->param);
            tool.settings.brush.value = 0.3f;
            tool.settings.brush.strength = 1.0f;
            tool.settings.brush.hardness = 1.0f;
            tool.settings.screenRadius = 60.0f;
            ASSERT_TRUE(tool.stroke(*g.host, view, path, 1.0f, ctrl ? kStrokeCtrl : 0u));
            const ScalpMaps& maps = g.session->scalpMaps();
            const std::string name = std::string(brush) == "density" ? "scatter_density" : "width_width";
            ASSERT_TRUE(maps.maps.count(name)) << g.session->scalpMapsJson();
            const std::vector<float>& values = maps.maps.at(name).values;
            ASSERT_FALSE(values.empty());
            EXPECT_EQ(*std::max_element(values.begin(), values.end()), 1.0f);  // white where the stroke did not go
            EXPECT_NEAR(*std::min_element(values.begin(), values.end()), ctrl ? 0.0f : 0.3f, 1e-4f);
        }
    }
}

TEST(Stroke, PickLevelAndHover) {
    Groom g(20, 4);
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 40.0f;
    // A child's tip: the pick is that child's level and group.
    const std::shared_ptr<const GuideHierarchy> h = g.session->hierarchy();
    const HierarchyGuide* child = nullptr;
    for (const HierarchyGuide& guide : h->guides()) {
        if (h->groupDepth(guide.group) == 1) {
            float x = 0, y = 0;
            if (view.project(guide.points.back(), x, y) && x > 50 && x < 750 && y > 50 && y < 750) {
                child = &guide;
                break;
            }
        }
    }
    ASSERT_NE(child, nullptr);
    float x = 0, y = 0;
    view.project(child->points.back(), x, y);
    const int level = tool.pickLevel(*g.host, view, x, y);
    EXPECT_GE(level, 0);
    EXPECT_EQ(tool.settings.brush.level, level);
    EXPECT_EQ(tool.pickLevel(*g.host, view, 2, 2), -2);
    // Hovering lights the brushable guides under the circle only.
    tool.settings.brush.level = -1;
    tool.settings.brush.group.clear();
    EXPECT_FALSE(tool.affectedLines(*g.host, view, x, y).empty());
    EXPECT_TRUE(tool.affectedLines(*g.host, view, 2, 2).empty());
    EXPECT_EQ(tool.affectedLines(*g.host, view, x, y).size(), tool.affectedLines(*g.host, hostOnly(view), x, y).size());
}

TEST(Stroke, PlaceDrawPartAndFlowTools) {
    Groom g(10);
    const ToolView view = frontView();
    BrushTool tool;
    const std::size_t guides = g.session->authoredHierarchy()->guides().size();
    tool.settings.setMode("place");
    tool.settings.place.matchNeighbors = false;
    float x = 0, y = 0;
    view.project(Vec3f(0.2f, 0.6f, 0.77f).normalized(), x, y);
    ASSERT_TRUE(tool.stroke(*g.host, view, {{std::round(x), std::round(y)}}));
    EXPECT_EQ(g.session->authoredHierarchy()->guides().size(), guides + 1);
    // A part drawn across the scalp, erased again with Ctrl.
    tool.settings.setMode("part");
    tool.settings.partName = "crown";
    const auto across = screenLine(view, Vec3f(-0.4f, 0.85f, 0.35f).normalized(), Vec3f(0.4f, 0.85f, 0.35f).normalized(), 21);
    tool.stroke(*g.host, view, across);
    PartSet parts = PartSet::fromJson(g.session->data().partsJson);
    ASSERT_EQ(parts.parts.size(), 1u);
    EXPECT_EQ(parts.parts[0].name, "crown");
    float mx = 0, my = 0;
    view.project(Vec3f(0.0f, 0.85f, 0.35f).normalized(), mx, my);
    tool.stroke(*g.host, view, {{std::round(mx), std::round(my) - 30}, {std::round(mx), std::round(my) + 30}}, 1.0f, kStrokeCtrl);
    EXPECT_TRUE(PartSet::fromJson(g.session->data().partsJson).empty());
    g.host->undo();
    EXPECT_EQ(PartSet::fromJson(g.session->data().partsJson).parts.size(), 1u);
    // Flow: one line of arrows along the stroke.
    tool.settings.setMode("flow");
    tool.settings.screenRadius = 100.0f;
    tool.stroke(*g.host, view, screenLine(view, Vec3f(-0.5f, 0.8f, 0.5f).normalized(), Vec3f(0.4f, 0.8f, 0.5f).normalized(), 16));
    const FlowSet flow = FlowSet::fromJson(g.session->data().flowJson);
    ASSERT_GT(flow.pins.size(), 2u);
    for (std::size_t i = 0; i < flow.pins.size(); ++i) EXPECT_EQ(flow.pins[i].seq, static_cast<int>(i));
    // Draw: a guide shaped like the stroke.
    tool.settings.setMode("draw");
    tool.settings.place.segments = 8;
    const std::size_t before = g.session->authoredHierarchy()->guides().size();
    view.project(Vec3f(0.0f, 0.6f, 0.8f).normalized(), x, y);
    std::vector<std::array<float, 2>> up;
    for (int k = 0; k < 20; ++k) up.push_back({std::round(x) + 3 * k, std::round(y) + 6 * k});
    ASSERT_TRUE(tool.stroke(*g.host, view, up));
    EXPECT_EQ(g.session->authoredHierarchy()->guides().size(), before + 1);
}

TEST(Stroke, VertexMapPaintedFromScreenAndWorldPoints) {
    Groom g(4, 0, R"({"crown": {"kind": "vertices", "values": []}})");
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("paint");
    tool.settings.paintKind = "vertices";
    tool.settings.brush.attribute = "crown";
    tool.settings.brush.value = 1.0f;
    tool.settings.screenRadius = 60.0f;
    ASSERT_TRUE(tool.stroke(*g.host, view, screenLine(view, Vec3f(-0.2f, 0.9f, 0.5f).normalized(), Vec3f(0.2f, 0.9f, 0.5f).normalized(), 8)));
    const ScalpMaps maps = ScalpMaps::fromJson(g.host->edits().back().text(StrokeField::ScalpMaps).after);
    const std::vector<float>& values = maps.maps.at("crown").values;
    ASSERT_EQ(values.size(), g.scalp->pointCount());
    EXPECT_GT(*std::max_element(values.begin(), values.end()), 0.5f);
    EXPECT_EQ(*std::min_element(values.begin(), values.end()), 0.0f);
    EXPECT_EQ(g.session->scalpMaps().maps.at("crown").values, values);
    // Scripted: dabs a quarter radius apart along the points, the same code as paintMapStroke.
    BrushToolSettings s;
    s.setMode("paint");
    s.paintKind = "vertices";
    s.brush.attribute = "crown";
    s.brush.value = 0.25f;
    s.brush.radius = 0.3f;
    s.brush.mirrorAxis = 0;
    const std::vector<Vec3f> path{{0.3f, 0.8f, 0.5f}, {0.5f, 0.6f, 0.6f}};
    std::vector<float> expected = values;
    MapCanvas canvas;
    canvas.vertices = &expected;
    const std::size_t changed = paintMapStroke(canvas, *g.scalp, MapDab::of(s, s.brush, kGrayMap, false, 1.0f), path, 0.3f);
    EXPECT_EQ(scriptedStroke(*g.host, s, path), changed);
    EXPECT_EQ(g.session->scalpMaps().maps.at("crown").values, ScalpMaps::fromJson(withVertexMap({}, "crown", expected, 1).toJson()).maps.at("crown").values);
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.session->scalpMaps().maps.at("crown").values, values);
}

TEST(Stroke, TextureMapStrokeUndoesByTiles) {
    Groom g(4, 0, R"({"tint": {"kind": "texture", "texture": "tint.png", "channels": 4}})");
    auto paint = std::make_shared<TexturePaint>(*g.scalp, 128, 128, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    g.host->setTexturePaint("tint", paint);
    const std::vector<float> blank = paint->values();
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("paint");
    tool.settings.paintKind = "texture";
    tool.settings.brush.attribute = "tint";
    tool.settings.paintColor = {0.0f, 0.0f, 1.0f, 1.0f};
    tool.settings.paintChannels = channelMask("b");
    tool.settings.screenRadius = 60.0f;
    ASSERT_TRUE(tool.stroke(*g.host, view, screenLine(view, Vec3f(-0.2f, 0.9f, 0.5f).normalized(), Vec3f(0.2f, 0.9f, 0.5f).normalized(), 8)));
    const StrokeEdit& edit = g.host->edits().back();
    EXPECT_EQ(edit.textureMap, "tint");
    EXPECT_FALSE(edit.texture.empty());
    EXPECT_FALSE(paint->editing());
    const std::vector<float> painted = paint->values();
    ASSERT_NE(painted, blank);
    for (std::size_t i = 0; i < painted.size(); i += 4) {
        EXPECT_EQ(painted[i], 0.0f);  // red untouched
    }
    g.host->undo();
    EXPECT_EQ(paint->values(), blank);
    g.host->redo();
    EXPECT_EQ(paint->values(), painted);
    // A map without a paint is refused with a warning, nothing committed.
    tool.settings.brush.attribute = "missing";
    EXPECT_FALSE(tool.stroke(*g.host, view, {{400, 600}}));
    ASSERT_FALSE(g.host->warnings().empty());
    EXPECT_NE(g.host->warnings().back().find("GroomLab paint:"), std::string::npos);
}

TEST(GuidePicking, LassoBoundsAndClicks) {
    const auto scalp = sphereWithUVs();
    const GuideHierarchy h = groomOn(*scalp, 60, 3);
    const ViewProjection view(test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.25), 1000, 800);
    const ViewProjector project = pixelProjector(view);
    const std::vector<GuideId> all = guidesAt(h, -1);
    Rng rng(9);
    for (int k = 0; k < 20; ++k) {
        std::vector<std::array<float, 2>> lasso;
        const float cx = rng.uniform(100, 900), cy = rng.uniform(100, 700), r = rng.uniform(20, 300);
        for (int i = 0; i < 9; ++i) {
            const float a = 6.2831853f * i / 9 + rng.uniform(0, 0.3f);
            lasso.push_back({cx + r * std::cos(a) * rng.uniform(0.5f, 1), cy + r * std::sin(a)});
        }
        for (const bool roots : {false, true}) {
            EXPECT_EQ(pickGuides(h, all, project, lasso, roots, 12.0f, &view), guidesInLasso(h, all, project, lasso, roots));
        }
    }
    // A click picks the guide with the nearest point within the pixels.
    const HierarchyGuide& g = h.guides()[7];
    std::array<float, 2> at{};
    ASSERT_TRUE(project(g.points.back(), &at));
    EXPECT_EQ(pickGuides(h, all, project, {{at[0] + 1, at[1]}}, false, 12.0f), std::vector<GuideId>{g.id});
    EXPECT_TRUE(pickGuides(h, all, project, {{-500, -500}}, false, 12.0f).empty());
    // Hidden groups are not candidates.
    EXPECT_EQ(guidesAt(h, -1, "", {"children"}), guidesAt(h, 0));
}

TEST(GuidePicking, SelectionModes) {
    const std::vector<GuideId> selected{4, 2, 9}, picked{9, 7, 7, 1};
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Replace), picked);
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Add), (std::vector<GuideId>{4, 2, 9, 7, 1}));
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Deselect), (std::vector<GuideId>{4, 2}));
    // Toggle: each picked guide in turn (7 twice: in, then out again).
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Toggle), (std::vector<GuideId>{4, 2, 1}));
    EXPECT_EQ(selectModeFromName("toggle"), SelectMode::Toggle);
    EXPECT_THROW(selectModeFromName("xor"), std::invalid_argument);
}

TEST(GuidePicking, MirrorSelectionAsAScanWouldPick) {
    const auto scalp = sphereWithUVs();
    GuideHierarchy h = groomOn(*scalp, 40, 2);
    // Twins: two guides at the same root (equally near any target).
    const GroupId primary = h.findGroup("primary");
    const HierarchyGuide first = h.guides().front();
    h.addGuide(primary, first.points);
    h.addGuide(primary, first.points);
    h.evaluate(scalp.get());
    auto scan = [&](const std::vector<GuideId>& selected, int axis, float center, float tolerance) {
        std::vector<GuideId> out;
        for (const GuideId id : selected) {
            const HierarchyGuide* g = h.guide(id);
            if (!g || g->points.empty()) continue;
            const Vec3f target = mirrorPoint(g->points.front(), axis, center);
            GuideId best = kNoGuide;
            float bestDist = tolerance;
            for (const HierarchyGuide& other : h.guides()) {
                if (other.group != g->group || other.points.empty()) continue;
                const float d = (other.points.front() - target).length();
                if (d <= bestDist) {
                    bestDist = d;
                    best = other.id;
                }
            }
            if (best != kNoGuide && std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
        }
        return out;
    };
    const std::vector<GuideId> all = guidesAt(h, -1);
    for (const int axis : {0, 1, 2}) {
        for (const float tolerance : {1e30f, 0.2f, 0.0f}) {
            EXPECT_EQ(mirrorSelection(h, all, axis, 0.0f, tolerance), scan(all, axis, 0.0f, tolerance));
        }
    }
    // Mirrored onto itself (center through the twins' root): the last twin, as the scan picks.
    const float center = first.points.front().x;
    EXPECT_EQ(mirrorSelection(h, {first.id}, 0, center, 1e30f), scan({first.id}, 0, center, 1e30f));
}

}  // namespace
}  // namespace groomlab
