// SPDX-License-Identifier: Apache-2.0
// The agent tools (engine/editor agent_tools.h, agent_grooming.h): landmarks on a scalp, paths
// along it, what a stroke changed and numbers that describe guides; and the tools run on a target
// of a bare GroomSession (as a host's live link will run them): a stroke is one undo step, undo and
// redo put the groom back, a map is made, linked and painted in one step, the preview compares.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/editor/agent_canvas.h"
#include "groomlab/editor/agent_expressions.h"
#include "groomlab/editor/agent_grooming.h"
#include "groomlab/editor/agent_photos.h"
#include "groomlab/editor/agent_tools.h"
#include "groomlab/groom_session.h"
#include "groomlab/png.h"
#include "../core/test_helpers.h"

using namespace groomlab;
using namespace groomlab::agent;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A ball of `radius` with UVs (u around, v from the bottom pole up), Y-up.
std::shared_ptr<const ScalpMesh> ball(float radius = 10.0f, int rings = 24, int segments = 48) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makeSphere(rings, segments, radius));
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

// A groom on a bare session, as a host's would be.
class SessionGroom : public AgentGroom {
public:
    SessionGroom(std::string name, GroomFile file) : AgentGroom(std::move(name)), file_(std::move(file)), session_(file_.data) {}
    GroomSession& session() override { return session_; }
    std::string upAxis() const override { return file_.upAxis; }
    std::string units() const override { return file_.units; }
    double framesPerSecond() const override { return 24.0; }
    const ViewResult& view(const ViewRequest& request) override { return session_.evaluate(request); }
    GroomFile toFile() override {
        GroomFile f = file_;
        f.data = session_.data();
        return f;
    }

private:
    GroomFile file_;
    GroomSession session_;
};

// Every mesh read is the ball; the files under a fresh folder (a sample "ball.obj" beside them).
class SessionTarget : public AgentTarget {
public:
    SessionTarget() {
        const fs::path dir = fs::temp_directory_path() / "groomlab_agent_tools";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "meshes");
        std::ofstream(dir / "meshes" / "ball.obj") << "# the ball\n";
        folders_.root = fs::weakly_canonical(dir).u8string();
        folders_.meshDir = (dir / "meshes").u8string();
    }
    const AgentFolders& folders() const override { return folders_; }
    std::vector<AgentGroom*> grooms() override {
        std::vector<AgentGroom*> out;
        for (auto& [n, g] : grooms_) out.push_back(g.get());
        return out;
    }
    AgentGroom& open(const std::string& name, GroomFile file) override {
        grooms_[name] = std::make_unique<SessionGroom>(name, std::move(file));
        return *grooms_[name];
    }
    void close(const std::string& name) override { grooms_.erase(name); }
    std::shared_ptr<const ScalpMesh> readMesh(const std::string&, const std::string&) override { return ball(); }
    ExportReport exportCurves(AgentGroom&, const ExportRequest&) override { throw ToolError("no export here"); }

private:
    AgentFolders folders_;
    std::map<std::string, std::unique_ptr<SessionGroom>> grooms_;
};

// A tool's answer (its last text block as JSON); fails the test on an error unless `error`.
json call(AgentTools& tools, const std::string& name, const json& args, bool error = false) {
    const ToolResult r = tools.call(name, args);
    EXPECT_EQ(r.isError, error) << name << ": " << r.content.dump();
    for (auto it = r.content.rbegin(); it != r.content.rend(); ++it) {
        if ((*it)["type"] == "text") return json::parse((*it)["text"].get<std::string>());
    }
    return json();
}

std::size_t authoredCount(AgentGroom& g) {
    const auto h = g.session().authoredHierarchy();
    return h ? h->guides().size() : 0;
}

GuideHierarchy standing(const ScalpMesh& scalp, const std::vector<Vec3f>& roots, float length) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (const Vec3f& r : roots) {
        const Vec3f n = r.normalized();
        h.addGuide(g, test::straightCurve(r, n * length, 8));
    }
    h.evaluate(&scalp);
    return h;
}

}  // namespace

TEST(AgentGrooming, LandmarksOnABallFallWhereTheirNamesSay) {
    const auto scalp = ball(10.0f);
    const ScalpLandmarks m = scalpLandmarks(*scalp, "Y");
    ASSERT_EQ(m.points.size(), landmarkNames().size());
    auto at = [&](const char* name) {
        const Landmark* l = m.find(name);
        EXPECT_NE(l, nullptr) << name;
        return l ? l->point : Vec3f();
    };
    EXPECT_NEAR(at("top").y, 10.0f, 0.2f);
    EXPECT_NEAR(m.find("top")->normal.y, 1.0f, 0.05f);
    EXPECT_GT(at("crown").y, 5.0f);
    EXPECT_LT(at("crown").z, -3.0f);
    EXPECT_GT(at("front_hairline").z, 5.0f);
    EXPECT_LT(at("nape").y, 0.0f);
    EXPECT_LT(at("nape").z, -5.0f);
    EXPECT_GT(at("left_temple").x, 5.0f);
    EXPECT_LT(at("right_temple").x, -5.0f);
    EXPECT_GT(at("left_ear").x, 9.0f);
    EXPECT_NEAR(at("back").z, -10.0f, 0.3f);
    for (const Landmark& l : m.points) EXPECT_NEAR(l.point.length(), 10.0f, 0.3f) << l.name;
    EXPECT_EQ(m.find("chin"), nullptr);
    // Z-up: front is -Y, the character's left still +X.
    std::vector<Vec3f> zUp;
    for (const Vec3f& p : scalp->points()) zUp.emplace_back(p.x, -p.z, p.y);
    const ScalpMesh turned(zUp, scalp->triangles());
    const ScalpLandmarks z = scalpLandmarks(turned, "Z");
    EXPECT_NEAR(z.find("top")->point.z, 10.0f, 0.2f);
    EXPECT_LT(z.find("front_hairline")->point.y, -5.0f);
    EXPECT_GT(z.find("crown")->point.y, 3.0f);
    EXPECT_GT(z.find("left_temple")->point.x, 5.0f);
}

TEST(AgentGrooming, ScalpPathRunsOverTheSurfaceFromStopToStop) {
    const auto scalp = ball(10.0f);
    const std::vector<Vec3f> stops{{0, 10, 0}, {0, 0, -10}, {10, 0, 0}};
    const std::vector<Vec3f> path = scalpPath(*scalp, Vec3f(0, 0, 0), stops, 1.0f, 0.0f);
    ASSERT_GT(path.size(), 25u);  // two quarter arcs of about 15.7 at 1 apart
    EXPECT_LT((path.front() - stops[0]).length(), 0.2f);
    EXPECT_LT((path.back() - stops[2]).length(), 0.2f);
    float longest = 0.0f;
    for (std::size_t i = 0; i < path.size(); ++i) {
        EXPECT_NEAR(path[i].length(), 10.0f, 0.15f);  // on the surface, not through the ball
        if (i > 0) longest = std::max(longest, (path[i] - path[i - 1]).length());
    }
    EXPECT_LT(longest, 1.2f);
    // Lifted off the surface by the offset.
    for (const Vec3f& p : scalpPath(*scalp, Vec3f(0, 0, 0), stops, 2.0f, 1.5f)) EXPECT_NEAR(p.length(), 11.5f, 0.2f);
    EXPECT_THROW(scalpPath(*scalp, Vec3f(0, 0, 0), {stops[0]}, 1.0f, 0.0f), std::invalid_argument);
}

TEST(AgentGrooming, SphereSelectionChangeAndMeasures) {
    const auto scalp = ball(10.0f);
    GuideHierarchy h = standing(*scalp, {{0, 10, 0}, {0, 0, 10}, {10, 0, 0}}, 4.0f);
    // A sphere about the top takes the guide rooted there (by root), and by any point one reaching in.
    EXPECT_EQ(guidesInSphere(h, Vec3f(0, 10, 0), 1.0f, true).size(), 1u);
    EXPECT_EQ(guidesInSphere(h, Vec3f(0, 13, 0), 1.0f, true).size(), 0u);
    EXPECT_EQ(guidesInSphere(h, Vec3f(0, 13, 0), 1.0f, false).size(), 1u);
    EXPECT_EQ(guidesInSphere(h, Vec3f(0, 10, 0), 1.0f, true, 1).size(), 0u);  // (level 1 has none)

    // Straight guides standing out: straight, no curl, standing on the normal, each its length.
    std::vector<GuideId> all;
    for (const HierarchyGuide& g : h.guides()) all.push_back(g.id);
    GuideStats st = measureGuides(h, all, scalp.get());
    EXPECT_EQ(st.count, 3u);
    EXPECT_NEAR(st.lengthMean, 4.0f, 0.05f);
    EXPECT_NEAR(st.straightness, 1.0f, 1e-3f);
    EXPECT_NEAR(st.curlDegrees, 0.0f, 0.5f);
    EXPECT_NEAR(st.rootAngleDegrees, 0.0f, 3.0f);
    const GuideStats top = measureGuides(h, {all[0]}, scalp.get());
    EXPECT_NEAR(top.direction.y, 1.0f, 1e-3f);
    EXPECT_EQ(directionWords(top.direction, ScalpFrame::of("Y")), "up");
    EXPECT_EQ(directionWords(Vec3f(0, -0.7f, -0.7f), ScalpFrame::of("Y")), "down, back");
    EXPECT_EQ(directionWords(Vec3f(1, 0, 0), ScalpFrame::of("Y")), "to the left");

    // A guide bent through a quarter turn: curled 90 degrees, less straight.
    GuideHierarchy bent;
    const GroupId g = bent.addGroup("primary");
    std::vector<Vec3f> arc;
    for (int i = 0; i <= 16; ++i) {
        const float a = 1.5707963f * static_cast<float>(i) / 16.0f;
        arc.emplace_back(0.0f, 10.0f + 3.0f * std::sin(a), 3.0f - 3.0f * std::cos(a));
    }
    bent.addGuide(g, arc);
    bent.evaluate(scalp.get());
    const GuideStats b = measureGuides(bent, {bent.guides().front().id}, scalp.get());
    EXPECT_NEAR(b.curlDegrees, 90.0f * 15.0f / 16.0f, 1.0f);  // (15 bends of 1/16 of the turn between its 16 segments)
    EXPECT_LT(b.straightness, 0.95f);

    // What changed: one guide moved, one removed, one added.
    GuideHierarchy after = h;
    std::vector<Vec3f> moved = after.guide(all[0])->points;
    for (std::size_t i = 1; i < moved.size(); ++i) moved[i] += Vec3f(0, 0, 0.5f * static_cast<float>(i));
    after.setPoints(all[0], moved);
    after.evaluate(scalp.get());
    after.removeGuides({all[1]}, true);
    after.addGuide(after.findGroup("primary"), test::straightCurve(Vec3f(-10, 0, 0), Vec3f(-4, 0, 0), 8));
    const GuideChange c = guideChange(h, after);
    EXPECT_EQ(c.touched, 1u);
    EXPECT_EQ(c.removed, 1u);
    EXPECT_EQ(c.added, 1u);
    EXPECT_GT(c.maxMove, 3.0f);
    EXPECT_GT(c.meanMove, 0.0f);
    EXPECT_EQ(guideChange(h, h).touched, 0u);
}

TEST(AgentTools, StrokesAreUndoStepsAndTheToolsGroomABall) {
    SessionTarget target;
    AgentTools tools(target);
    for (const char* name : {"brush_stroke", "scalp_landmarks", "select_guides", "guide_op", "paint_map", "undo", "redo", "history",
                             "measure_guides", "render_preview"}) {
        EXPECT_TRUE(tools.has(name)) << name;
    }
    const json made = call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    EXPECT_EQ(made["summary"]["errors"], json::array());
    const json marks = call(tools, "scalp_landmarks", json::object());
    EXPECT_NEAR(marks["landmarks"]["top"]["p"][1].get<double>(), 10.0, 0.2);
    const json at = call(tools, "scalp_landmarks", {{"at", {{0, 20, 0}, "left_ear"}}});
    EXPECT_NEAR(at["at"][0]["p"][1].get<double>(), 10.0, 0.2);  // the scalp point under it, facing up
    EXPECT_NEAR(at["at"][0]["n"][1].get<double>(), 1.0, 0.05);
    EXPECT_GT(at["at"][1]["n"][0].get<double>(), 0.9);

    // Guides placed along the scalp, crown to nape over the back: one step.
    const json placed = call(tools, "brush_stroke", {{"mode", "place"}, {"from", "front_hairline"}, {"via", {"top", "crown"}},
                                                    {"to", "nape"}, {"length", 6.0}, {"spacing", 2.0}});
    const std::size_t count = placed["changed"]["guidesAdded"].get<std::size_t>();
    EXPECT_GT(count, 8u);
    AgentGroom& g = target.groom("");
    EXPECT_EQ(g.history().size(), 1u);
    const std::string placedGuides = g.session().data().hierarchyJson;

    // Combed toward the back, lifted off the scalp to reach along the guides: they lean back.
    const json before = call(tools, "measure_guides", json::object());
    const json combed = call(tools, "brush_stroke", {{"mode", "comb"}, {"from", "top"}, {"to", "back"}, {"offset", 2.0}, {"radius", 5.0}});
    EXPECT_GT(combed["changed"]["guidesTouched"].get<int>(), 0);
    EXPECT_GT(combed["changed"]["maxMove"].get<double>(), 0.5);
    EXPECT_NEAR(combed["changed"]["meanLengthAfter"].get<double>(), combed["changed"]["meanLengthBefore"].get<double>(), 0.05);
    const json after = call(tools, "measure_guides", json::object());
    EXPECT_GT(after["groups"][0]["curlDegrees"].get<double>(), before["groups"][0]["curlDegrees"].get<double>());
    // A stroke far from every guide changes nothing and is no step.
    const json missed = call(tools, "brush_stroke", {{"mode", "comb"}, {"points", {{0, -40, 0}, {0, -45, 0}}}, {"radius", 0.5}});
    EXPECT_EQ(missed["changed"]["guidesTouched"], 0);
    EXPECT_TRUE(missed.contains("note"));
    EXPECT_EQ(g.history().size(), 2u);

    // Undo puts the placed guides back exactly; redo the comb again.
    const std::string combedGuides = g.session().data().hierarchyJson;
    const json undone = call(tools, "undo", json::object());
    EXPECT_EQ(undone["undone"], json::array({"comb stroke"}));
    EXPECT_EQ(g.session().data().hierarchyJson, placedGuides);
    const json redone = call(tools, "redo", json::object());
    EXPECT_EQ(redone["redone"], json::array({"comb stroke"}));
    EXPECT_EQ(g.session().data().hierarchyJson, combedGuides);
    call(tools, "redo", json::object(), true);  // nothing to redo

    // Cut along a band: shorter guides.
    const json cut = call(tools, "brush_stroke", {{"mode", "cut"}, {"from", "left_ear"}, {"via", {"crown"}}, {"to", "right_ear"},
                                                 {"offset", 3.0}, {"radius", 3.0}});
    if (cut["changed"]["guidesTouched"].get<int>() > 0) {
        EXPECT_LT(cut["changed"]["meanLengthAfter"].get<double>(), cut["changed"]["meanLengthBefore"].get<double>());
    }

    // A density map made for scatter.density, filled and painted, in one step each.
    const std::size_t steps = g.history().size();
    const json filled = call(tools, "paint_map", {{"for", "scatter.density"}, {"fill", 1.0}});
    EXPECT_EQ(filled["made"], true);
    EXPECT_EQ(filled["values"]["min"], 1.0);
    const std::string map = filled["map"].get<std::string>();
    const json painted = call(tools, "paint_map", {{"for", "scatter.density"}, {"from", "left_ear"}, {"via", {"nape"}},
                                                  {"to", "right_ear"}, {"value", 0.0}, {"radius", 3.0}});
    EXPECT_EQ(painted["map"], map);
    EXPECT_GT(painted["verticesPainted"].get<int>(), 0);
    EXPECT_EQ(painted["values"]["min"], 0.0);
    EXPECT_LT(painted["summary"]["strands"].get<int>(), filled["summary"]["strands"].get<int>());
    EXPECT_EQ(g.history().size(), steps + 2);
    call(tools, "undo", json::object());
    EXPECT_EQ(g.session().scalpMaps().maps.at(map).values.front(), 1.0f);  // the stroke undone, the fill kept
    call(tools, "redo", json::object());

    // Select the guides about the crown and delete them; the selection follows.
    const json picked = call(tools, "select_guides", {{"center", "crown"}, {"radius", 4.0}});
    const std::size_t n = picked["selected"].get<std::size_t>();
    ASSERT_GT(n, 0u);
    const json sel = call(tools, "measure_guides", {{"selection", true}});
    EXPECT_EQ(sel["groups"][0]["count"].get<std::size_t>(), n);
    const std::size_t guides = authoredCount(g);
    const json deleted = call(tools, "guide_op", {{"op", "delete_guides"}});
    EXPECT_EQ(deleted["count"].get<std::size_t>(), n);
    EXPECT_EQ(authoredCount(g), guides - n);
    EXPECT_TRUE(g.selection.empty());
    call(tools, "guide_op", {{"op", "delete_guides"}}, true);  // none selected: says so
    const json children = call(tools, "guide_op", {{"op", "generate_children"}, {"count", 2}, {"group", "primary"}});
    EXPECT_EQ(children["added"].get<std::size_t>(), 2 * (guides - n));

    // The history lists the steps; a preview compares the last one with before.
    const json history = call(tools, "history", json::object());
    EXPECT_EQ(history["steps"].size(), g.history().size());
    EXPECT_EQ(history["undo"], "generate children");
    const ToolResult preview = tools.call("render_preview", {{"views", {"back", "left"}}, {"size", 128}, {"guides", true}, {"compare", true}});
    ASSERT_FALSE(preview.isError) << preview.content.dump();
    int images = 0;
    for (const json& c : preview.content) images += c["type"] == "image" ? 1 : 0;
    EXPECT_EQ(images, 2);
    EXPECT_EQ(g.history().toUndo().title(), "generate children");  // (put back as it was)
    EXPECT_EQ(authoredCount(g), 3 * (guides - n));

    // Bad asks say what is wrong.
    call(tools, "brush_stroke", {{"mode", "comb"}, {"from", "chin"}, {"to", "top"}}, true);
    call(tools, "brush_stroke", {{"mode", "comb"}}, true);
    call(tools, "brush_stroke", {{"mode", "width"}, {"points", {{0, 10, 0}}}}, true);
    call(tools, "select_guides", json::object(), true);
}

TEST(AgentTools, AHistoryKeepsSixtyFourSteps) {
    SessionTarget target;
    AgentTools tools(target);
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    for (int i = 0; i < 70; ++i) call(tools, "add_guides", {{"guides", {{{"root", {0, 10, 0}}, {"length", 2.0}}}}});
    AgentGroom& g = target.groom("ball");
    EXPECT_EQ(g.history().size(), kMaxUndo);
    EXPECT_EQ(call(tools, "undo", {{"steps", 64}})["undone"].size(), 64u);
    EXPECT_EQ(authoredCount(g), 6u);
}

// ---- M1b: photos, roots and tubes, expressions, simulation, previews ----------------------------

namespace {

// A gray photo `w` x `h` written as a PNG at `path` (a bright disc in its middle: something to see).
void writePhoto(const fs::path& path, int w, int h) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 30);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x - w / 2), dy = static_cast<float>(y - h / 2);
            if (dx * dx + dy * dy >= static_cast<float>(w * w) / 16.0f) continue;
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3;
            for (std::size_t k = 0; k < 3; ++k) pixels[at + k] = 200;
        }
    }
    const std::vector<std::uint8_t> png = encodePng(w, h, 3, pixels);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

int images(const ToolResult& r) {
    int n = 0;
    for (const json& c : r.content) n += c["type"] == "image" ? 1 : 0;
    return n;
}

}  // namespace

TEST(AgentPhotos, ViewCamerasFrameTheScalpAndCandidatesSolveItBack) {
    const auto scalp = ball(10.0f);
    const RefCamera front = viewCamera(*scalp, "Y", "front", 640, 480, 50.0);
    float u = 0.0f, v = 0.0f;
    ASSERT_TRUE(front.project(Vec3f(0, 0, 0), u, v));
    EXPECT_NEAR(u, 320.0f, 0.5f);  // the scalp's middle in the image's middle
    EXPECT_NEAR(v, 240.0f, 0.5f);
    EXPECT_GT(front.eye().z, 20.0f);  // in front of it, looking back at it
    EXPECT_NEAR(focalPixelsToMm(front.focalPx, 640.0), 50.0, 1e-6);
    const RefCamera left = viewCamera(*scalp, "Y", "left", 640, 480);
    EXPECT_GT(left.eye().x, 20.0f);  // the character's left: +X
    EXPECT_THROW(viewCamera(*scalp, "Y", "under", 640, 480), std::invalid_argument);

    // Numbered points the camera sees (the near side of the ball), where it sees them.
    const std::vector<MeshCandidate> found = meshCandidates(*scalp, front, 20);
    ASSERT_EQ(found.size(), 20u);
    for (const MeshCandidate& c : found) {
        EXPECT_GT(c.point.z, -1.0f) << "on the side the camera sees";
        float pu = 0.0f, pv = 0.0f;
        ASSERT_TRUE(front.project(c.point, pu, pv));
        EXPECT_NEAR(pu, c.u, 0.01f);
        EXPECT_NEAR(pv, c.v, 0.01f);
    }
    // Paired with where a camera of its own sees them, the solve finds that camera.
    const RefCamera truth = viewCamera(*scalp, "Y", "three_quarter", 640, 480, 35.0);
    MarkerLists pairs;
    for (const MeshCandidate& c : meshCandidates(*scalp, truth, 12)) {
        pairs.add(kLandmarkPhoto, ReferencePhotoEditor::photoPoint(c.u, c.v));
        MarkerPoint m;
        m.point = c.point;
        pairs.add(kLandmarkMesh, m);
    }
    const ReferenceLandmarkSolve solve = solveReferenceLandmarks(pairs, 640, 480, focalMmToPixels(50.0, 640), false);
    ASSERT_TRUE(solve.ok) << solve.why;
    EXPECT_LT(solve.rms, 0.05);
    EXPECT_LT((solve.camera.eye() - truth.eye()).length(), 0.05f);
    EXPECT_NEAR(solve.camera.focalPx, truth.focalPx, 0.5);

    // The groom file's form, both ways.
    AgentPhoto p;
    p.name = "side";
    p.image = "/photos/side.png";
    p.width = 640;
    p.height = 480;
    p.camera = solve.camera;
    p.state.landmarks = pairs;
    p.state.strokes.push_back({{{10, 10}, {20, 40}}, true});
    const std::vector<AgentPhoto> back = photosFromJson(photosToJson({p}));
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].name, "side");
    ASSERT_TRUE(back[0].camera.has_value());
    EXPECT_LT((back[0].camera->eye() - solve.camera.eye()).length(), 1e-4f);
    EXPECT_EQ(back[0].state.landmarks.pairCount(), pairs.pairCount());
    EXPECT_EQ(back[0].state.strokes.size(), 1u);
    EXPECT_TRUE(photosFromJson("").empty());
    EXPECT_EQ(photosToJson({}), "");
    EXPECT_THROW(photosFromJson("{}"), std::invalid_argument);
}

TEST(AgentCanvas, DrawsLabelsAndTheMeshThroughACamera) {
    Canvas c(64, 32);
    const int w = c.text(2, 2, "12", {1, 1, 1}, 2, false);
    EXPECT_EQ(w, Canvas::textWidth("12", 2));
    int lit = 0;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 20; ++x) lit += c.at(x, y)[0] > 0.9f ? 1 : 0;
    }
    EXPECT_GT(lit, 10);
    const auto scalp = ball(10.0f);
    Canvas m(128, 128);
    std::vector<float> depth;
    m.mesh(*scalp, viewCamera(*scalp, "Y", "front", 128, 128), {0.8f, 0.8f, 0.8f}, depth);
    EXPECT_GT(m.at(64, 64)[0], 0.2f);  // the ball in the middle
    EXPECT_LT(m.at(1, 1)[0], 0.1f);    // the background in the corner
    const std::vector<std::uint8_t> png = m.png();
    ASSERT_GT(png.size(), 8u);
    EXPECT_EQ(png[1], 'P');
}

TEST(AgentExpressions, ChecksCompileReadAndSample) {
    const auto scalp = ball(10.0f);
    // Before strands: sampled over the scalp.
    ExpressionCheck c = checkExpression("smoothstep($y, -5, 5)", {}, nullptr, scalp.get());
    ASSERT_TRUE(c.ok) << c.error;
    EXPECT_EQ(c.sampledOver, "scalp");
    EXPECT_EQ(c.variables, std::vector<std::string>{"y"});
    EXPECT_NEAR(c.min, 0.0f, 1e-3f);
    EXPECT_NEAR(c.max, 1.0f, 1e-3f);
    ASSERT_EQ(c.histogram.size(), 10u);
    std::size_t total = 0;
    for (const std::size_t n : c.histogram) total += n;
    EXPECT_EQ(total, c.samples);
    // Errors say where; channels it reads, and the ones missing.
    c = checkExpression("1 - $t ^", {}, nullptr, scalp.get());
    EXPECT_FALSE(c.ok);
    EXPECT_NE(c.error.find("position"), std::string::npos) << c.error;
    StrandSet strands;
    strands.addStrand(0, {Vec3f(0, 10, 0), Vec3f(0, 11, 0), Vec3f(0, 12, 0)});
    strands.addStrand(1, {Vec3f(0, 10, 0), Vec3f(0, 13, 0)});
    c = checkExpression("$length * $amount + $clumpId", {{"amount", 2.0f}}, &strands, scalp.get());
    ASSERT_TRUE(c.ok) << c.error;
    EXPECT_EQ(c.sampledOver, "strands");
    EXPECT_EQ(c.samples, 2u);
    EXPECT_NEAR(c.min, 4.0f, 1e-4f);
    EXPECT_NEAR(c.max, 6.0f, 1e-4f);
    EXPECT_EQ(c.missingChannels, std::vector<std::string>{"clumpId"});
    EXPECT_EQ(std::count(c.variables.begin(), c.variables.end(), "amount"), 0);  // (a constant, not a variable)
    c = checkExpression("$t", {}, &strands, scalp.get());
    ASSERT_TRUE(c.ok);
    EXPECT_TRUE(c.perPoint);
    EXPECT_EQ(c.sampledOver, "points");
    EXPECT_EQ(c.samples, 5u);
    c = checkExpression("[1, 0, 0]", {}, &strands, scalp.get());
    ASSERT_TRUE(c.ok);
    EXPECT_TRUE(c.vector);
    EXPECT_NEAR(c.mean, 1.0f, 1e-5f);
}

TEST(AgentPreview, CameraLooksAndTurntables) {
    const auto scalp = ball(10.0f);
    PreviewOptions o;
    o.size = 128;
    o.camera = viewCamera(*scalp, "Y", "front", 400, 200);
    const PreviewImage wide = renderPreview(scalp.get(), nullptr, o);
    EXPECT_EQ(wide.width, 128);
    EXPECT_EQ(wide.height, 64);  // the camera's shape
    o.camera.reset();
    o.views.clear();
    for (int i = 0; i < 8; ++i) o.views.push_back("turn:" + std::to_string(i * 45));
    const PreviewImage sheet = renderPreview(scalp.get(), nullptr, o);
    EXPECT_EQ(sheet.columns, 4);
    EXPECT_EQ(sheet.height, 64);  // two rows of 32
    for (const std::string& look : previewLookNames()) {
        o.look = look;
        EXPECT_NO_THROW(renderPreview(scalp.get(), nullptr, o)) << look;
    }
    o.look = "neon";
    EXPECT_THROW(renderPreview(scalp.get(), nullptr, o), std::invalid_argument);
    o.look = "studio";
    o.views = {"turn:abc"};
    EXPECT_THROW(renderPreview(scalp.get(), nullptr, o), std::invalid_argument);
}

TEST(AgentTools, PhotosRootsTubesExpressionsAndSimulation) {
    SessionTarget target;
    AgentTools tools(target);
    for (const char* name : {"add_photo", "photo_view", "mesh_landmarks", "align_photo", "photo_edit", "guides_from_photos", "root_tube",
                             "tube_levels", "tube_shape", "check_expression", "import_animation", "animate_scalp", "simulate", "sim_cache",
                             "export_cards", "export_game", "stats"}) {
        EXPECT_TRUE(tools.has(name)) << name;
    }
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    AgentGroom& g = target.groom("ball");

    // A photo: added (one step, shown under a grid), lined up from pairs a known camera gives.
    writePhoto(fs::u8path(target.folders().root) / "shot.png", 320, 240);
    const ToolResult added = tools.call("add_photo", {{"path", "shot.png"}});
    ASSERT_FALSE(added.isError) << added.content.dump();
    EXPECT_EQ(images(added), 1);
    EXPECT_EQ(g.history().size(), 1u);
    EXPECT_NE(g.referencePhotos.find("shot"), std::string::npos);
    const json marks = call(tools, "mesh_landmarks", {{"view", "front"}, {"photo", "shot"}, {"count", 12}});
    ASSERT_EQ(marks["points"].size(), 12u);
    const RefCamera truth = viewCamera(*g.session().scalp(), "Y", "three_quarter", 320, 240, 40.0);
    json pairs = json::array();
    for (const json& p : marks["points"]) {
        float u = 0.0f, v = 0.0f;
        const Vec3f at(p["p"][0].get<float>(), p["p"][1].get<float>(), p["p"][2].get<float>());
        if (truth.project(at, u, v) && u > 0 && v > 0 && u < 320 && v < 240) pairs.push_back({{"pixel", {u, v}}, {"point", p["p"]}});
    }
    ASSERT_GE(pairs.size(), 6u);
    const json aligned = call(tools, "align_photo", {{"pairs", pairs}});
    ASSERT_EQ(aligned["linedUp"], true) << aligned.dump();
    EXPECT_LT(aligned["rmsPixels"].get<double>(), 0.5);
    EXPECT_NEAR(aligned["camera"]["focal_mm"].get<double>(), 40.0, 0.5);
    const std::vector<AgentPhoto> stored = photosFromJson(g.referencePhotos);
    ASSERT_TRUE(stored[0].camera.has_value());
    EXPECT_LT((stored[0].camera->eye() - truth.eye()).length(), 0.2f);
    // A flow arrow, then the edits undone back to no photo.
    call(tools, "photo_edit", {{"op", "add_flow"}, {"points", {{160, 60}, {170, 180}}}});
    EXPECT_EQ(photosFromJson(g.referencePhotos)[0].state.strokes.size(), 1u);
    call(tools, "photo_edit", {{"op", "add_flow"}, {"points", {{160, 60}}}}, true);  // one point: no arrow
    call(tools, "undo", {{"steps", 3}});
    EXPECT_EQ(g.referencePhotos, "");
    call(tools, "redo", {{"steps", 3}});
    EXPECT_EQ(photosFromJson(g.referencePhotos)[0].state.strokes.size(), 1u);
    const ToolResult overlay = tools.call("render_preview", {{"camera", {{"photo", "shot"}}}, {"size", 160}});
    ASSERT_FALSE(overlay.isError) << overlay.content.dump();

    // A root tube round the top, subdivided into cells, then split a level down.
    const json drawn = call(tools, "root_tube", {{"op", "draw"}, {"center", "top"}, {"radius", 3.0}, {"length", 6.0}});
    ASSERT_EQ(drawn["guides"].size(), 1u);
    const GuideId root = drawn["guides"][0].get<GuideId>();
    EXPECT_EQ(call(tools, "root_tube", {{"op", "list"}})["roots"].size(), 1u);
    const json cut = call(tools, "root_tube", {{"op", "subdivide"}, {"guides", {root}}, {"size_at_one", 0.3}, {"size_at_zero", 0.3}});
    EXPECT_GE(cut["guides"].size(), 3u);
    const json levels = call(tools, "tube_levels", {{"op", "add"}});
    EXPECT_GT(levels["split"].get<int>(), 0);
    const std::size_t depth = levels["levels"].size();
    call(tools, "tube_levels", {{"op", "view"}, {"level", 2}});
    EXPECT_FALSE(g.mixedView.empty());
    const ToolResult tubes = tools.call("render_preview", {{"views", {"top"}}, {"tubes", true}, {"size", 128}});
    ASSERT_FALSE(tubes.isError);
    EXPECT_EQ(call(tools, "tube_levels", {{"op", "remove"}})["levels"].size(), depth - 1);
    const json shape = call(tools, "tube_shape", {{"op", "add"}, {"name", "leaf"}, {"points", {{-1, 0}, {0, 0.4}, {1, 0}, {0, -0.4}}}});
    EXPECT_EQ(shape["shape"], "leaf");
    EXPECT_EQ(call(tools, "tube_shape", json::object())["library"], json::array({"leaf"}));

    // An expression checked on the strands; set on a param, a broken one refused with its place.
    const json ok = call(tools, "check_expression", {{"expression", "smoothstep($y, 0, 8)"}});
    EXPECT_EQ(ok["ok"], true);
    EXPECT_EQ(ok["sampledOver"], "strands");
    EXPECT_EQ(call(tools, "check_expression", {{"expression", "2 *"}})["ok"], false);
    const json bad = call(tools, "set_params", {{"node", "width"}, {"params", {{"width", {{"expression", "1 +"}}}}}}, true);
    EXPECT_NE(bad.value("error", std::string()).find("does not compile"), std::string::npos) << bad.dump();

    // Simulation: a Simulate node added before Interpolate, a few frames stepped.
    call(tools, "add_guides", {{"guides", {{{"root", {0, 10, 0}}, {"length", 6.0}}, {{"root", {10, 0, 0}}, {"length", 6.0}}}}});
    const json sim = call(tools, "simulate", {{"add", true}, {"to_frame", 4}});
    EXPECT_EQ(sim["framesStepped"], 4);
    ASSERT_EQ(sim["simulation"].size(), 1u);
    EXPECT_GT(sim["simulation"][0]["guides"].get<int>(), 0);
    EXPECT_LT(sim["simulation"][0]["stretch"]["max"].get<double>(), 0.2);
    call(tools, "simulate", {{"add", true}}, true);  // one already
    // What needs Alembic says this target cannot.
    const json abc = call(tools, "animate_scalp", {{"keys", {{{"frame", 1}}, {{"frame", 4}, {"translate", {1, 0, 0}}}}}}, true);
    EXPECT_NE(abc.value("error", std::string()).find("mesh caches"), std::string::npos);
    EXPECT_TRUE(call(tools, "stats", json::object())["stats"].is_object());
}
