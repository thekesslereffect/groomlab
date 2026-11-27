// SPDX-License-Identifier: Apache-2.0
// The agent tools (engine/editor agent_tools.h, agent_grooming.h): landmarks on a scalp, paths
// along it, what a stroke changed and numbers that describe guides; and the tools run on a target
// of a bare GroomSession (as a host's live link will run them): a stroke is one undo step, undo and
// redo put the groom back, a map is made, linked and painted in one step, the preview compares.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "groomlab/editor/agent_align.h"
#include "groomlab/editor/agent_canvas.h"
#include "groomlab/editor/agent_expressions.h"
#include "groomlab/editor/agent_face.h"
#include "groomlab/editor/agent_grooming.h"
#include "groomlab/editor/agent_link.h"
#include "groomlab/editor/agent_photos.h"
#include "groomlab/editor/agent_shoot.h"
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
        // (a folder of its own a shard: shards run side by side, and each target starts it empty)
        const char* shard = std::getenv("GTEST_SHARD_INDEX");
        const fs::path dir = fs::temp_directory_path() / ("groomlab_agent_tools" + std::string(shard ? shard : ""));
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

TEST(AgentTools, LockToolsStrokesCombShellAndReshape) {
    SessionTarget target;
    AgentTools tools(target);
    for (const char* name : {"guides_from_strokes", "comb_guides", "hair_shell", "photo_project", "reshape_guides"}) EXPECT_TRUE(tools.has(name)) << name;
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    AgentGroom& g = target.groom("ball");

    // reshape_guides: new points in place, the id, group and tube kept; one undo step.
    const json made = call(tools, "add_guides", {{"guides", {{{"root", {0, 10, 0}}, {"length", 4.0}}, {{"root", {10, 0, 0}}, {"length", 4.0}}}},
                                                 {"group", "keys"}, {"tube", {{"shape", "ribbon"}}}});
    const GuideId first = made["ids"][0].get<GuideId>();
    const std::size_t steps = g.history().size();
    const json reshaped = call(tools, "reshape_guides", {{"guides", {{{"id", first}, {"points", {{0.5, 10.5, 0}, {1, 13, 1}, {2, 15, 3}}}}}}});
    EXPECT_EQ(reshaped["count"], 1);
    EXPECT_EQ(g.history().size(), steps + 1);
    {
        const auto h = g.session().authoredHierarchy();
        const HierarchyGuide* gd = h->guide(first);
        ASSERT_NE(gd, nullptr);
        EXPECT_EQ(gd->points.size(), 3u);
        EXPECT_NEAR(gd->points.front().length(), 10.0f, 0.05f);  // the root snapped onto the ball
        EXPECT_NEAR(gd->points.back().z, 3.0f - 0.0f + (gd->points.front().z - 0.0f), 0.3f);
        EXPECT_EQ(h->group(gd->group)->name, "keys");
        EXPECT_EQ(gd->tube.shapeName(), "ribbon");
    }
    call(tools, "reshape_guides", {{"guides", {{{"id", 999}, {"points", {{0, 10, 0}, {0, 12, 0}}}}}}}, true);
    call(tools, "undo", json::object());
    EXPECT_EQ(g.session().authoredHierarchy()->guide(first)->points.size(), 9u);  // (8 segments as added)

    // delete_guides takes a whole group.
    const json deleted = call(tools, "guide_op", {{"op", "delete_guides"}, {"group", "keys"}});
    EXPECT_EQ(deleted["count"], 2);
    EXPECT_EQ(authoredCount(g), 0u);

    // sample_scalp: the hair region (all of a ball with no face), no two nearer than min_spacing.
    const json spread = call(tools, "sample_scalp", {{"count", 200}, {"region", "hair"}, {"min_spacing", 4.0}});
    ASSERT_GT(spread["points"].size(), 10u);
    EXPECT_LT(spread["points"].size(), 200u);
    for (std::size_t i = 0; i < spread["points"].size(); ++i)
        for (std::size_t j = i + 1; j < spread["points"].size(); ++j) {
            const json& a = spread["points"][i]["p"];
            const json& b = spread["points"][j]["p"];
            const double d = std::hypot(a[0].get<double>() - b[0].get<double>(), a[1].get<double>() - b[1].get<double>(), a[2].get<double>() - b[2].get<double>());
            EXPECT_GE(d, 4.0 - 1e-3);
        }

    // The hair's shell without photos: a guessed volume over the head.
    const json shell = call(tools, "hair_shell", json::object());
    EXPECT_EQ(shell["from"], "volume");
    EXPECT_NEAR(shell["head"]["radius"].get<double>(), 10.0, 0.2);
    EXPECT_GT(shell["regions"]["top"]["thickness"]["mean"].get<double>(), 1.0);

    // comb_guides: guides over the hair region, off the head, one undo step; its look on request.
    const std::size_t before = g.history().size();
    const json combed = call(tools, "comb_guides", {{"count", 60}, {"volume", 2.0}, {"part", "center"}, {"apply_look", true},
                                                    {"wave", {{"amplitude", 0.5}}}, {"lengths", {{"back", 12.0}}}});
    EXPECT_EQ(g.history().size(), before + 1);
    EXPECT_GT(combed["guides"].get<int>(), 40);
    EXPECT_EQ(combed["shell"]["from"], "volume");
    for (const HierarchyGuide& gd : g.session().authoredHierarchy()->guides())
        for (const Vec3f& p : gd.points) EXPECT_GE(p.length(), 9.9f);
    const json graph = call(tools, "get_graph", json::object());
    std::set<std::string> names;
    for (const json& n : graph["nodes"]) names.insert(n["name"].get<std::string>());
    EXPECT_TRUE(names.count("subClump") && names.count("tipTrim") && names.count("hairline")) << graph.dump();
    call(tools, "comb_guides", {{"part", json::array({1, 2})}}, true);  // not two points
    call(tools, "comb_guides", {{"sweep", {{"front", "sideways"}}}}, true);
    EXPECT_EQ(combed["mode"], "field");
    EXPECT_GT(combed["field"]["field"]["cells"].get<int>(), 100);
    EXPECT_EQ(combed["field"]["part"]["from"], "comb");
    EXPECT_TRUE(combed["field"]["part"].contains("offset"));  // (the center part)
    // The comb on its own, each guide combed alone.
    const json alone = call(tools, "comb_guides", {{"count", 60}, {"volume", 2.0}, {"mode", "comb"}, {"group", "combAlone"}});
    EXPECT_EQ(alone["mode"], "comb");
    EXPECT_GT(alone["guides"].get<int>(), 40);
    call(tools, "comb_guides", {{"mode", "blend"}}, true);
    call(tools, "guide_op", {{"op", "delete_guides"}, {"group", "combAlone"}});

    // fill_guides: the hair region filled between a group's guides through their field, one undo
    // step; the new guides rooted on the head, off it; again: the group replaced.
    {
        const std::size_t keyCount = g.session().authoredHierarchy()->guidesInGroup(g.session().authoredHierarchy()->findGroup("combed")).size();
        const std::size_t steps0 = g.history().size();
        const json filled = call(tools, "fill_guides", {{"group", "combed"}, {"count", 80}, {"volume", 2.0}});
        EXPECT_EQ(g.history().size(), steps0 + 1);
        EXPECT_EQ(filled["group"], "fill");
        EXPECT_EQ(filled["mode"], "field");
        EXPECT_EQ(filled["keys"].get<std::size_t>(), keyCount);
        EXPECT_GT(filled["guides"].get<int>(), 50);
        const json& st = filled["stopped"];
        EXPECT_EQ(st["length"].get<int>() + st["faded"].get<int>() + st["hull"].get<int>() + st["turn"].get<int>() + st["nothing"].get<int>(),
                  filled["roots"].get<int>());
        EXPECT_EQ(filled["part"]["from"], "none");
        EXPECT_FALSE(filled.contains("score"));  // (no photos)
        const auto h = g.session().authoredHierarchy();
        const GroupId fill = h->findGroup("fill");
        ASSERT_NE(fill, kNoGroup);
        EXPECT_EQ(h->guidesInGroup(fill).size(), filled["guides"].get<std::size_t>());
        for (const GuideId id : h->guidesInGroup(fill)) {
            const HierarchyGuide* gd = h->guide(id);
            EXPECT_NEAR(gd->points.front().length(), 10.0f, 0.1f);
            for (const Vec3f& p : gd->points) EXPECT_GE(p.length(), 9.9f);
        }
        const json again = call(tools, "fill_guides", {{"group", "combed"}, {"count", 40}, {"volume", 2.0}, {"part", "center"}, {"wave", false}});
        EXPECT_EQ(again["part"]["from"], "comb");
        EXPECT_EQ(g.session().authoredHierarchy()->guidesInGroup(g.session().authoredHierarchy()->findGroup("fill")).size(),
                  again["guides"].get<std::size_t>());
        call(tools, "fill_guides", {{"group", "nothing"}}, true);
        call(tools, "fill_guides", {{"group", "combed"}, {"fill_group", "combed"}}, true);
        call(tools, "fill_guides", {{"group", "combed"}, {"uncovered", "maybe"}}, true);
        call(tools, "guide_op", {{"op", "delete_guides"}, {"group", "fill"}});
    }

    // A photo lined up from pairs a known camera gives (as above).
    writePhoto(fs::u8path(target.folders().root) / "shot.png", 320, 240);
    call(tools, "add_photo", {{"path", "shot.png"}});
    const json marks = call(tools, "mesh_landmarks", {{"view", "front"}, {"photo", "shot"}, {"count", 12}});
    const RefCamera truth = viewCamera(*g.session().scalp(), "Y", "three_quarter", 320, 240, 40.0);
    json pairs = json::array();
    for (const json& p : marks["points"]) {
        float u = 0.0f, v = 0.0f;
        const Vec3f at(p["p"][0].get<float>(), p["p"][1].get<float>(), p["p"][2].get<float>());
        if (truth.project(at, u, v) && u > 0 && v > 0 && u < 320 && v < 240) pairs.push_back({{"pixel", {u, v}}, {"point", p["p"]}});
    }
    ASSERT_GE(pairs.size(), 6u);
    call(tools, "align_photo", {{"pairs", pairs}});

    // photo_project: the photo's middle onto the ball and a point back to its pixel.
    float cu = 0.0f, cv = 0.0f;
    ASSERT_TRUE(truth.project(Vec3f(0, 0, 0), cu, cv));
    const json cast = call(tools, "photo_project", {{"pixels", {{cu, cv}, {2, 2}}}, {"points", {{0, 10, 0}}}, {"onto", "shell"}});
    ASSERT_EQ(cast["hits"].size(), 2u);
    EXPECT_EQ(cast["hits"][0]["hit"], true);
    EXPECT_NEAR(std::hypot(cast["hits"][0]["p"][0].get<double>(), cast["hits"][0]["p"][1].get<double>(), cast["hits"][0]["p"][2].get<double>()), 10.0, 0.1);
    EXPECT_EQ(cast["hits"][0]["hairRegion"], true);
    EXPECT_EQ(cast["hits"][1]["hit"], false);
    EXPECT_TRUE(cast["hits"][0].contains("shell"));
    float tu = 0.0f, tv = 0.0f;
    ASSERT_TRUE(truth.project(Vec3f(0, 10, 0), tu, tv));
    EXPECT_NEAR(cast["projected"][0]["pixel"][0].get<double>(), tu, 1.0);
    EXPECT_NEAR(cast["projected"][0]["pixel"][1].get<double>(), tv, 1.0);
    call(tools, "photo_project", json::object(), true);

    // guides_from_strokes: a stroke over the top in the photo's pixels, onto the guessed volume
    // (depth volume), mirrored, filled; one undo step.
    json stroke = json::array();
    for (int i = 0; i <= 20; ++i) {
        const float a = (5.0f + 70.0f * static_cast<float>(i) / 20.0f) * 3.14159265f / 180.0f;
        float u = 0.0f, v = 0.0f;
        ASSERT_TRUE(truth.project(Vec3f(3.0f, 12.5f * std::cos(a), 12.5f * std::sin(a)), u, v));
        stroke.push_back({u, v});
    }
    const std::size_t at = g.history().size();
    const ToolResult lifted = tools.call("guides_from_strokes", {{"photo", "shot"}, {"strokes", {stroke}}, {"depth", "volume"}, {"mirror", "x"},
                                                                 {"fill", true}, {"fill_count", 30}, {"clip", false}});
    ASSERT_FALSE(lifted.isError) << lifted.content.dump();
    EXPECT_EQ(images(lifted), 1);
    const json out = json::parse(lifted.content.back()["text"].get<std::string>());
    EXPECT_EQ(g.history().size(), at + 1);
    ASSERT_EQ(out["ids"].size(), 2u);  // the stroke's guide and its mirror
    EXPECT_GT(out["strokes"][0]["hitShare"].get<double>(), 0.8);
    EXPECT_GT(out["strokes"][0]["length"].get<double>(), 8.0);
    EXPECT_EQ(out["strokes"][0]["mirrored"], true);
    EXPECT_GT(out["fill"]["guides"].get<int>(), 20);
    {
        const auto h = g.session().authoredHierarchy();
        const HierarchyGuide* a = h->guide(out["ids"][0].get<GuideId>());
        const HierarchyGuide* b = h->guide(out["ids"][1].get<GuideId>());
        ASSERT_TRUE(a && b);
        EXPECT_NEAR(a->points.back().x, -b->points.back().x, 0.5f);
        EXPECT_NEAR(a->points.front().length(), 10.0f, 0.1f);
    }
    // The same strokes again replace the groups; strokes from an image of a wrong size are refused.
    const json again = call(tools, "guides_from_strokes", {{"photo", "shot"}, {"strokes", {stroke}}, {"depth", "volume"}});
    EXPECT_EQ(again["ids"].size(), 1u);
    writePhoto(fs::u8path(target.folders().root) / "odd.png", 300, 300);
    const json odd = call(tools, "guides_from_strokes", {{"photo", "shot"}, {"strokes_image", "odd.png"}}, true);
    EXPECT_NE(odd["error"].get<std::string>().find("image_transform"), std::string::npos) << odd.dump();
    call(tools, "guides_from_strokes", {{"photo", "shot"}}, true);  // no strokes
}

// ---- M2: the agent link's protocol ----------------------------------------------------------------

namespace {

long long processAliveSelf() {
#ifdef _WIN32
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

}  // namespace

TEST(AgentLink, DiscoveryFilesComeAndGoWithTheirHosts) {
    const fs::path dir = fs::temp_directory_path() / "groomlab_agent_link_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    LinkInfo info;
    info.host = "maya";
    info.pid = processAliveSelf();
    info.port = 51234;
    info.token = newLinkToken();
    info.groom = "groomShape1";
    info.started = linkClock();
    EXPECT_EQ(info.token.size(), 32u);
    EXPECT_NE(info.token, newLinkToken());
    const std::string path = writeLinkInfo(info, dir.u8string());
    EXPECT_TRUE(fs::exists(fs::u8path(path)));
    // Another host's file, older; a dead process's and a file that is no link: left out, removed.
    LinkInfo older = info;
    older.host = "app";
    older.started -= 100.0;
    writeLinkInfo(older, dir.u8string());
    LinkInfo dead = info;
    dead.host = "blender";
    dead.pid = 0;
    const std::string deadPath = writeLinkInfo(dead, dir.u8string());
    std::ofstream(dir / "junk.json") << "{\"not\": \"a link\"}";
    std::vector<LinkInfo> live = liveLinks({}, dir.u8string());
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0].host, "maya");  // newest first
    EXPECT_EQ(live[0].token, info.token);
    EXPECT_EQ(live[0].file, path);
    EXPECT_FALSE(fs::exists(fs::u8path(deadPath)));
    EXPECT_FALSE(fs::exists(dir / "junk.json"));
    EXPECT_EQ(liveLinks("app", dir.u8string()).size(), 1u);
    removeLinkInfo(path);
    EXPECT_EQ(liveLinks("maya", dir.u8string()).size(), 0u);
    EXPECT_THROW(LinkInfo::fromJson(json{{"host", "maya"}, {"pid", 1}, {"port", 0}, {"token", "t"}}), std::invalid_argument);
}

TEST(AgentLink, RequestsAreAnsweredOnlyWithTheToken) {
    SessionTarget target;
    AgentTools tools(target);
    const std::string token = newLinkToken();
    // Without the token, or with another: refused, nothing else said.
    json answer = json::parse(answerLinkRequest(tools, token, linkRequestLine("wrong", 1, "tools/list", nullptr)));
    EXPECT_EQ(answer["error"]["code"], kLinkBadToken);
    EXPECT_FALSE(answer.contains("result"));
    answer = json::parse(answerLinkRequest(tools, token, R"({"id": 2, "method": "tools/list"})"));
    EXPECT_EQ(answer["error"]["code"], kLinkBadToken);
    EXPECT_EQ(json::parse(answerLinkRequest(tools, token, "not json"))["error"]["code"], kLinkBadRequest);
    // With it: the tools, a call (its result as AgentTools gives it), ping; an unknown method.
    const json listed = linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 3, "tools/list", nullptr)), 3);
    EXPECT_EQ(listed["tools"].size(), tools.list().size());
    const json made = linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 4, "tools/call",
                                                                                {{"name", "new_groom"}, {"arguments", {{"name", "b"}, {"mesh", "ball"}}}})),
                                 4);
    EXPECT_EQ(made["isError"], false);
    EXPECT_EQ(made["content"][0]["type"], "text");
    const json failed = linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 5, "tools/call", {{"name", "undo"}})), 5);
    EXPECT_EQ(failed["isError"], true);  // (a tool's error is a result, not a protocol error)
    EXPECT_TRUE(linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 6, "ping", nullptr)), 6).is_object());
    EXPECT_THROW(linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 7, "files/delete", nullptr)), 7), std::runtime_error);
    EXPECT_THROW(linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 8, "tools/call", {{"name", "nope"}})), 8),
                 std::runtime_error);
    EXPECT_THROW(linkResult(answerLinkRequest(tools, token, linkRequestLine(token, 9, "ping", nullptr)), 10), std::runtime_error);
}

namespace {

// A host with a renderer of its own (a flat picture of the size asked for), as the app's lookdev.
class RenderingTarget : public SessionTarget {
public:
    std::vector<RendererInfo> renderers() override { return {{"flat", "one color"}, {"other", "another"}}; }
    RenderedFrame render(AgentGroom&, const RenderRequest& r) override {
        last = r;
        RenderedFrame f;
        f.image.width = r.width;
        f.image.height = r.height;
        f.image.png = encodePng(r.width, r.height, 3, std::vector<std::uint8_t>(static_cast<std::size_t>(r.width * r.height * 3), 128));
        f.seconds = 0.25;
        f.info = {{"note", "flat"}};
        return f;
    }
    RenderRequest last;
};

}  // namespace

TEST(AgentTools, RenderFrameIsTheHostsRenderersOnly) {
    // Headless: no renderers, no render_frame.
    SessionTarget headless;
    AgentTools plain(headless);
    EXPECT_FALSE(plain.has("render_frame"));
    RenderingTarget target;
    AgentTools tools(target);
    ASSERT_TRUE(tools.has("render_frame"));
    call(tools, "new_groom", {{"name", "b"}, {"mesh", "ball"}});
    // The request as the host gets it; the image and what it says.
    const ToolResult r = tools.call("render_frame", {{"renderer", "other"}, {"width", 64}, {"height", 32}, {"quality", "final"}});
    ASSERT_FALSE(r.isError) << r.content.dump();
    EXPECT_EQ(r.content[0]["type"], "image");
    const json info = json::parse(r.content[1]["text"].get<std::string>());
    EXPECT_EQ(info["renderer"], "other");
    EXPECT_EQ(info["width"], 64);
    EXPECT_EQ(info["note"], "flat");
    EXPECT_EQ(target.last.groom, "b");
    EXPECT_EQ(target.last.quality, "final");
    // The defaults, and what is out of range.
    call(tools, "render_frame", json::object());
    EXPECT_EQ(target.last.renderer, "flat");
    EXPECT_EQ(target.last.width, 640);
    EXPECT_EQ(target.last.height, 480);
    call(tools, "render_frame", {{"width", 4096}}, true);
    call(tools, "render_frame", {{"samples", 5000}}, true);
    call(tools, "render_frame", {{"renderer", "cycles"}}, true);

    // A host whose glue renders: the line checked into a request, the PNG it wrote answered.
    const std::string token = newLinkToken();
    const std::string line = linkRequestLine(token, 11, "tools/call", {{"name", "render_frame"}, {"arguments", {{"width", 20}, {"height", 16}}}});
    EXPECT_TRUE(isToolCall(line, "render_frame"));
    EXPECT_FALSE(isToolCall(line, "undo"));
    const json begun = beginHostRender(tools, token, line);
    ASSERT_TRUE(begun.contains("render")) << begun.dump();
    EXPECT_EQ(begun["render"]["groom"], "b");
    EXPECT_EQ(begun["render"]["width"], 20);
    EXPECT_EQ(begun["id"], 11);
    const fs::path png = fs::path(target.folders().root) / "glue.png";
    {
        const std::vector<std::uint8_t> bytes = encodePng(20, 16, 3, std::vector<std::uint8_t>(960, 200));
        std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const json done = linkResult(finishHostRender(tools, begun, png.u8string(), 1.5, {{"camera", "persp"}}, ""), 11);
    EXPECT_EQ(done["isError"], false);
    EXPECT_EQ(done["content"][0]["type"], "image");
    const json doneInfo = json::parse(done["content"][1]["text"].get<std::string>());
    EXPECT_EQ(doneInfo["width"], 20);
    EXPECT_EQ(doneInfo["camera"], "persp");
    EXPECT_EQ(doneInfo["file"], "glue.png");  // (inside the root: shown relative)
    // Its failure, a bad argument and a wrong token, each answered.
    EXPECT_EQ(linkResult(finishHostRender(tools, begun, "", 0.0, json::object(), "no renderer here"), 11)["isError"], true);
    const json bad = beginHostRender(tools, token, linkRequestLine(token, 12, "tools/call", {{"name", "render_frame"}, {"arguments", {{"width", 1}}}}));
    EXPECT_EQ(linkResult(bad["reply"].get<std::string>(), 12)["isError"], true);
    const json refused = json::parse(beginHostRender(tools, token, linkRequestLine("wrong", 13, "tools/call", {{"name", "render_frame"}}))["reply"].get<std::string>());
    EXPECT_EQ(refused["error"]["code"], kLinkBadToken);
}

namespace {

// An OBJ's vertices and faces (fans), as the shipped sample meshes have them.
std::shared_ptr<const ScalpMesh> readObjFile(const fs::path& path) {
    std::ifstream in(path);
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    for (std::string line; std::getline(in, line);) {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            s >> x >> y >> z;
            points.push_back({x, y, z});
        } else if (tag == "f") {
            std::vector<std::int32_t> ids;
            for (std::string t; s >> t;) ids.push_back(std::stoi(t.substr(0, t.find('/'))) - 1);
            for (std::size_t k = 2; k < ids.size(); ++k) triangles.push_back({ids[0], ids[k - 1], ids[k]});
        }
    }
    return std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
}

std::shared_ptr<const ScalpMesh> nikaHead() {
    return readObjFile(fs::u8path(GROOMLAB_TEST_DATA) / ".." / ".." / "engine" / "assets" / "meshes" / "nika_head.obj");
}

CameraMat3 mul3(const CameraMat3& a, const CameraMat3& b) {
    CameraMat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

}  // namespace

TEST(AgentFace, NikasFacialLandmarksLandWhereTheyAre) {
    const auto head = nikaHead();
    ASSERT_GT(head->triangleCount(), 1000u);
    const FaceLandmarks face = faceLandmarks(*head, "Y");
    ASSERT_TRUE(face.head) << face.why;
    auto at = [&](const std::string& name) {
        const FaceLandmark* l = face.find(name);
        EXPECT_TRUE(l) << name;
        return l ? l->point : Vec3f();
    };
    // Nika: centimetres, y up, facing +z; the nose tip at about (0, -0.6, 11.4), the chin under it.
    EXPECT_LT((at("nose_tip") - Vec3f(0.0f, -0.6f, 11.4f)).length(), 1.0f);
    EXPECT_GT(face.find("nose_tip")->confidence, 0.6f);
    EXPECT_NEAR(at("chin").y, -8.3f, 1.2f);
    EXPECT_GT(at("chin").z, 6.5f);
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? 1.0f : -1.0f;  // (the character's left is +x)
        const std::string p = side == 0 ? "left_" : "right_";
        const Vec3f eye = at(p + "eye"), inner = at(p + "eye_inner"), outer = at(p + "eye_outer");
        EXPECT_GT(s * eye.x, 1.5f) << p;
        EXPECT_LT(s * eye.x, 3.6f) << p;
        EXPECT_NEAR(eye.y, 2.5f, 1.2f) << p;
        EXPECT_LT(s * inner.x, s * eye.x) << p;  // (inner: toward the nose)
        EXPECT_GT(s * outer.x, s * eye.x) << p;
        const Vec3f mouth = at(p + "mouth_corner");
        EXPECT_NEAR(mouth.y, -3.3f, 1.0f) << p;
        EXPECT_GT(s * mouth.x, 1.2f) << p;
        const Vec3f top = at(p + "ear_top"), lobe = at(p + "earlobe");
        EXPECT_GT(s * top.x, 7.0f) << p;
        EXPECT_GT(top.y - lobe.y, 3.0f) << p;  // (an ear: 4-7 cm tall)
        EXPECT_LT(top.y - lobe.y, 8.0f) << p;
        EXPECT_GT(face.find(p + "ear_top")->confidence, 0.4f) << p;
    }
    const Vec3f hairline = at("hairline_middle");
    EXPECT_NEAR(hairline.y, 9.9f, 1.5f);
    EXPECT_GT(hairline.z, 4.0f);
    // The scalp's region: the top and the back of the head are hair, the face and the neck not.
    const ScalpLandmarks marks = scalpLandmarks(*head, "Y");
    EXPECT_GT(hairRegionWeight(face, marks, head->closestPoint({0.0f, 14.0f, 0.0f}).position), 0.95f);
    EXPECT_GT(hairRegionWeight(face, marks, head->closestPoint({0.0f, 6.0f, -12.0f}).position), 0.95f);
    EXPECT_LT(hairRegionWeight(face, marks, at("nose_tip")), 0.05f);
    EXPECT_LT(hairRegionWeight(face, marks, at("left_eye")), 0.05f);
    EXPECT_LT(hairRegionWeight(face, marks, head->closestPoint({0.0f, -12.0f, -4.0f}).position), 0.05f);
    // The front hairline on the face's hairline, not low on the forehead.
    EXPECT_NEAR(hairlinePoint(*head, face, marks, 0.0f).y, hairline.y, 0.6f);
    // A scalp cap and a ball (no face): nothing named, and why.
    const FaceLandmarks none = faceLandmarks(*readObjFile(fs::u8path(GROOMLAB_TEST_DATA) / "app" / "head_scalp.obj"), "Y");
    EXPECT_FALSE(none.head);
    EXPECT_FALSE(none.why.empty());
    EXPECT_FALSE(faceLandmarks(*ball(), "Y").head);
}

TEST(AgentAlign, TheOutlineTightensARoughCameraOnASyntheticPortrait) {
    const auto head = nikaHead();
    const FaceLandmarks face = faceLandmarks(*head, "Y");
    ASSERT_TRUE(face.head);
    // The truth: a portrait 384 x 512 from the front left, tilted a little; the head on a white
    // backdrop (no hair).
    RefCamera truth = viewCamera(*head, "Y", "three_quarter_front_left", 384, 512, 85.0);
    truth.pose.rotation = mul3(rotationFromEulerXYZDegrees({-4.0, 6.0, 0.0}), truth.pose.rotation);
    Canvas canvas(384, 512, {1.0f, 1.0f, 1.0f});
    std::vector<float> depth;
    canvas.mesh(*head, truth, {0.55f, 0.45f, 0.4f}, depth);
    RefImage photo;
    photo.width = 384;
    photo.height = 512;
    photo.channels = 3;
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 384; ++x)
            for (int c = 0; c < 3; ++c) photo.pixels.push_back(canvas.at(x, y)[static_cast<std::size_t>(c)]);
    // Rough pairs: six named points, each a few pixels off.
    std::vector<CameraCorrespondence> pairs;
    const double noise[6][2] = {{3, -2}, {-2, 3}, {2, 2}, {-3, -2}, {2, -3}, {-2, 2}};
    int k = 0;
    for (const char* name : {"nose_tip", "chin", "left_eye", "right_eye", "left_mouth_corner", "left_earlobe"}) {
        const Vec3f p = face.find(name)->point;
        float u = 0, v = 0;
        ASSERT_TRUE(truth.project(p, u, v)) << name;
        pairs.push_back({u + noise[k][0], v + noise[k][1], {p.x, p.y, p.z}});
        ++k;
    }
    CameraSolveOptions options;
    options.imageWidth = 384;
    options.imageHeight = 512;
    options.focalPx = truth.focalPx;
    options.fixFocal = true;
    options.hasInitialPose = true;
    options.initialPose = truth.pose;
    const CameraSolveResult solved = solveCamera(pairs, options);
    ASSERT_TRUE(solved.ok);
    RefCamera start = truth;
    start.pose = solved.pose;
    // How far a camera is from the truth: the mesh's points' mean distance in the picture.
    auto off = [&](const RefCamera& c) {
        double sum = 0.0;
        int n = 0;
        for (const Vec3f& p : head->points()) {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (!truth.project(p, u0, v0) || !c.project(p, u1, v1)) continue;
            sum += std::sqrt((u1 - u0) * (u1 - u0) + (v1 - v0) * (v1 - v0));
            ++n;
        }
        return sum / std::max(1, n);
    };
    const ScalpLandmarks marks = scalpLandmarks(*head, "Y");
    const OutlineFit fit = refineWithOutline(*head, photo, RefImage{}, start, pairs,
                                             [&](const Vec3f& q) { return hairRegionWeight(face, marks, q) < 0.5f; });
    ASSERT_TRUE(fit.ok) << fit.why;
    EXPECT_GT(fit.samples, 50);
    EXPECT_LT(fit.outlineAfter, fit.outlineBefore);
    EXPECT_LT(fit.outlineAfter, 1.5);
    EXPECT_LT(off(fit.camera), 0.6 * off(start)) << off(start) << " -> " << off(fit.camera);
    // A photo with no plain backdrop: nothing to match, said.
    RefImage busy = photo;
    for (std::size_t i = 0; i < busy.pixels.size(); ++i) busy.pixels[i] = static_cast<float>((i * 7919) % 255) / 255.0f;
    EXPECT_FALSE(refineWithOutline(*head, busy, RefImage{}, start, pairs, nullptr).ok);
}

TEST(AgentTools, CheckpointsBatchesMapsLabelsAndPhotosBeside) {
    SessionTarget target;
    AgentTools tools(target);
    for (const char* name : {"checkpoint", "restore", "batch", "show_map", "suggest_pairs"}) EXPECT_TRUE(tools.has(name)) << name;
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    AgentGroom& g = target.groom("ball");
    call(tools, "brush_stroke", {{"mode", "place"}, {"from", "front_hairline"}, {"via", {"top", "crown"}}, {"to", "nape"},
                                 {"length", 6.0}, {"spacing", 2.0}});
    // A checkpoint, a change, the checkpoint back as one undo step (and undone again).
    const std::string guides = g.session().data().hierarchyJson, graph = g.session().data().graphJson;
    const json saved = call(tools, "checkpoint", {{"name", "placed"}});
    EXPECT_EQ(saved["checkpoints"], json::array({"placed"}));
    call(tools, "brush_stroke", {{"mode", "comb"}, {"from", "top"}, {"to", "back"}, {"offset", 2.0}, {"radius", 5.0}});
    call(tools, "set_params", {{"node", "length"}, {"params", {{"randomness", 0.6}}}});
    ASSERT_NE(g.session().data().hierarchyJson, guides);
    const std::size_t steps = g.history().size();
    const json back = call(tools, "restore", {{"name", "placed"}});
    EXPECT_EQ(g.session().data().hierarchyJson, guides);
    EXPECT_EQ(g.session().data().graphJson, graph);
    EXPECT_EQ(g.history().size(), steps + 1);
    EXPECT_FALSE(back["changed"].empty());
    call(tools, "undo", json::object());
    EXPECT_NE(g.session().data().hierarchyJson, guides);
    call(tools, "restore", {{"name", "nothing"}}, true);
    EXPECT_EQ(call(tools, "checkpoint", json::object())["checkpoints"], json::array({"placed"}));

    // A batch: each call answered in order, stopping at the first error; images only when asked.
    const ToolResult batch = tools.call("batch", {{"calls", {{{"tool", "evaluate"}, {"arguments", json::object()}},
                                                             {{"tool", "render_preview"}, {"arguments", {{"size", 96}}}},
                                                             {{"tool", "set_params"}, {"arguments", {{"node", "nope"}, {"params", json::object()}}}},
                                                             {{"tool", "evaluate"}, {"arguments", json::object()}}}}});
    EXPECT_TRUE(batch.isError);
    EXPECT_EQ(images(batch), 0);
    const json ran = json::parse(batch.content.back()["text"].get<std::string>());
    EXPECT_EQ(ran["ran"], 3);
    EXPECT_EQ(ran["stoppedAt"], 2);
    EXPECT_EQ(ran["calls"][0]["ok"], true);
    EXPECT_EQ(ran["calls"][2]["ok"], false);
    const ToolResult pictures = tools.call("batch", {{"calls", {{{"tool", "render_preview"}, {"arguments", {{"size", 96}}}}}}, {"images", true}});
    EXPECT_FALSE(pictures.isError);
    EXPECT_EQ(images(pictures), 1);
    call(tools, "batch", {{"calls", {{{"tool", "batch"}, {"arguments", json::object()}}}}}, true);

    // A heatmap of an expression over the scalp: the values and a legend.
    const ToolResult map = tools.call("show_map", {{"expression", "$y"}, {"views", {"front"}}, {"size", 256}});
    ASSERT_FALSE(map.isError) << map.content.dump();
    EXPECT_EQ(images(map), 1);
    const json shown = json::parse(map.content.back()["text"].get<std::string>());
    EXPECT_GT(shown["values"]["max"].get<double>(), shown["values"]["min"].get<double>() + 5.0);
    call(tools, "show_map", json::object(), true);
    const ToolResult checked = tools.call("check_expression", {{"expression", "$y"}, {"image", true}});
    EXPECT_EQ(images(checked), 1);

    // Guides labelled with their IDs.
    const ToolResult labelled = tools.call("render_preview", {{"views", {"top"}}, {"label_guides", true}, {"size", 256}});
    ASSERT_FALSE(labelled.isError) << labelled.content.dump();
    const json drawn = json::parse(labelled.content.back()["text"].get<std::string>());
    ASSERT_TRUE(drawn.contains("guidesLabelled")) << drawn.dump();
    EXPECT_GT(drawn["guidesLabelled"]["count"].get<int>(), 8);
    call(tools, "render_preview", {{"label_guides", "no such group"}}, true);

    // The groom beside a lined-up photo: the photo and the groom through its camera, the same size.
    writePhoto(fs::u8path(target.folders().root) / "shot.png", 320, 240);
    call(tools, "add_photo", {{"path", "shot.png"}});
    const RefCamera camera = viewCamera(*g.session().scalp(), "Y", "front", 320, 240, 50.0);
    const json marks = call(tools, "mesh_landmarks", {{"view", "front"}, {"photo", "shot"}, {"count", 12}});
    json pairs = json::array();
    for (const json& p : marks["points"]) {
        float u = 0, v = 0;
        if (camera.project(Vec3f(p["p"][0].get<float>(), p["p"][1].get<float>(), p["p"][2].get<float>()), u, v)) pairs.push_back({{"pixel", {u, v}}, {"point", p["p"]}});
    }
    call(tools, "align_photo", {{"pairs", pairs}, {"focal_mm", 50.0}, {"keep_focal", true}, {"view", "front"}});
    const ToolResult beside = tools.call("render_preview", {{"beside", "shot"}});
    ASSERT_FALSE(beside.isError) << beside.content.dump();
    const json besideInfo = json::parse(beside.content.back()["text"].get<std::string>());
    EXPECT_EQ(besideInfo["width"], 640);
    EXPECT_EQ(besideInfo["height"], 240);
    const json blended = call(tools, "render_preview", {{"beside", "shot"}, {"blend", 0.5}});
    EXPECT_EQ(blended["width"], 320);
    // A ball has no face to name: suggest_pairs says so.
    call(tools, "suggest_pairs", {{"photo", "shot"}}, true);
}

TEST(AgentPhotos, PairsSnapToTheNearestCorner) {
    // A light square on a dark ground: a prediction a few pixels off its corner snaps onto it.
    RefImage im;
    im.width = im.height = 80;
    im.channels = 3;
    im.pixels.assign(80 * 80 * 3, 0.1f);
    for (int y = 30; y < 60; ++y)
        for (int x = 30; x < 60; ++x)
            for (int c = 0; c < 3; ++c) im.pixels[static_cast<std::size_t>((y * 80 + x) * 3 + c)] = 0.9f;
    const PairSnap snap = snapToFeature(im, 34.0f, 26.0f, 8.0f);
    EXPECT_NEAR(snap.u, 30.0f, 2.0f);
    EXPECT_NEAR(snap.v, 30.0f, 2.0f);
    EXPECT_GT(snap.confidence, 0.5f);
    // A flat patch: nothing stands out, the prediction kept.
    const PairSnap flat = snapToFeature(im, 10.0f, 10.0f, 5.0f);
    EXPECT_EQ(flat.confidence, 0.0f);
    EXPECT_FLOAT_EQ(flat.u, 10.0f);
    EXPECT_FALSE(pairCropPng(im, 34.0f, 26.0f, snap, 8.0f, 4).empty());
}

TEST(AgentTools, MorphToPhotosLandsThePairsAndUndoes) {
    SessionTarget target;
    AgentTools tools(target);
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    AgentGroom& g = target.groom("ball");
    const std::vector<Vec3f> original = g.session().rawScalp()->points();
    writePhoto(fs::u8path(target.folders().root) / "wide.png", 320, 240);
    call(tools, "add_photo", {{"path", "wide.png"}});
    // The photo's head a quarter wider than the ball: its pairs where a wider ball would put them.
    const RefCamera truth = viewCamera(*g.session().scalp(), "Y", "front", 320, 240, 50.0);
    const json marks = call(tools, "mesh_landmarks", {{"view", "front"}, {"photo", "wide"}, {"count", 14}});
    json pairs = json::array();
    for (const json& p : marks["points"]) {
        const Vec3f at(p["p"][0].get<float>(), p["p"][1].get<float>(), p["p"][2].get<float>());
        float u = 0, v = 0;
        if (truth.project(Vec3f(at.x * 1.25f, at.y, at.z), u, v) && u > 0 && v > 0 && u < 320 && v < 240) {
            pairs.push_back({{"pixel", {u, v}}, {"point", p["p"]}});
        }
    }
    ASSERT_GE(pairs.size(), 6u);
    const json aligned = call(tools, "align_photo", {{"pairs", pairs}, {"focal_mm", 50.0}, {"keep_focal", true}, {"view", "front"}});
    ASSERT_EQ(aligned["linedUp"], true);
    const ToolResult morphed = tools.call("morph_to_photos", json::object());
    ASSERT_FALSE(morphed.isError) << morphed.content.dump();
    EXPECT_EQ(images(morphed), 1);
    const json info = json::parse(morphed.content.back()["text"].get<std::string>());
    const json& photo = info["photos"][0];
    EXPECT_GT(photo["rmsBefore"].get<double>(), 2.0);
    EXPECT_LT(photo["rmsAfter"].get<double>(), 0.3 * photo["rmsBefore"].get<double>()) << info.dump();
    EXPECT_GT(info["pins"].get<int>(), 5);
    const std::vector<Vec3f> once = g.session().rawScalp()->points();
    EXPECT_NE(once, original);
    // Morphs never stack: the same morph again gives the same scalp.
    call(tools, "morph_to_photos", json::object());
    const std::vector<Vec3f> twice = g.session().rawScalp()->points();
    ASSERT_EQ(twice.size(), once.size());
    float most = 0.0f;
    for (std::size_t i = 0; i < once.size(); ++i) most = std::max(most, (twice[i] - once[i]).length());
    EXPECT_LT(most, 1e-3f);
    // Undo: the morph taken back; reset: the scalp before the first morph.
    call(tools, "undo", json::object());
    call(tools, "morph_to_photos", {{"reset", true}});
    EXPECT_EQ(g.session().rawScalp()->points(), original);
    call(tools, "undo", json::object());
    EXPECT_EQ(g.session().rawScalp()->points(), once);
    call(tools, "morph_to_photos", {{"stiffness", 2.0}}, true);
}

TEST(AgentTools, PhotoRegionsAndAPreviewAtTheCamerasSize) {
    SessionTarget target;
    AgentTools tools(target);
    call(tools, "new_groom", {{"name", "ball"}, {"mesh", "ball"}});
    writePhoto(fs::u8path(target.folders().root) / "shot.png", 320, 240);
    call(tools, "add_photo", {{"path", "shot.png"}});
    // A region enlarged under a fine grid, labelled in the photo's pixels.
    const ToolResult zoom = tools.call("photo_view", {{"region", {100, 80, 160, 120}}, {"size", 600}});
    ASSERT_FALSE(zoom.isError) << zoom.content.dump();
    const json info = json::parse(zoom.content.back()["text"].get<std::string>());
    EXPECT_EQ(info["region"], json::array({100, 80, 160, 120}));
    EXPECT_EQ(info["grid"], 5);
    EXPECT_EQ(info["shown"], json::array({600, 400}));
    call(tools, "photo_view", {{"region", {100, 80, 104, 120}}}, true);
    // A camera's picture at the camera's own size.
    const ToolResult shot = tools.call("render_preview", {{"camera", {{"eye", {0, 0, 60}}, {"target", {0, 0, 0}}, {"width", 385}, {"height", 510}}}});
    ASSERT_FALSE(shot.isError) << shot.content.dump();
    const json drawn = json::parse(shot.content.back()["text"].get<std::string>());
    EXPECT_EQ(drawn["width"], 385);
    EXPECT_EQ(drawn["height"], 510);
    // Not a head: no named landmarks, and new_groom says nothing of a face.
    const json marks = call(tools, "mesh_landmarks", {{"view", "front"}});
    EXPECT_FALSE(marks.contains("named"));
    call(tools, "scalp_from_landmarks", json::object(), true);
}

namespace {

// A camera of a shoot round the head: `yaw` degrees round from the front (toward the subject's
// left), `pitch` up, at `distance` from the head's middle, a `focalMm` lens, 385 x 510 pixels.
RefCamera shootCamera(const ScalpLandmarks& m, double yaw, double pitch, double distance, double focalMm) {
    const double y = yaw * 3.14159265 / 180.0, p = pitch * 3.14159265 / 180.0;
    const Vec3f toEye = (m.frame.front * static_cast<float>(std::cos(y) * std::cos(p)) + m.frame.left * static_cast<float>(std::sin(y) * std::cos(p)) +
                         m.frame.up * static_cast<float>(std::sin(p)))
                            .normalized();
    const Vec3f eye = m.middle + toEye * static_cast<float>(distance);
    const Vec3f z = toEye, x = m.frame.up.cross(z).normalized(), yy = z.cross(x);
    RefCamera c;
    c.width = 385;
    c.height = 510;
    c.focalPx = focalMm / 36.0 * 385.0;
    auto part = [](const Vec3f& v, int r) { return static_cast<double>(r == 0 ? v.x : r == 1 ? v.y : v.z); };
    for (int r = 0; r < 3; ++r) c.pose.rotation[static_cast<std::size_t>(r)] = {part(x, r), part(yy, r), part(z, r)};
    c.pose.position = {eye.x, eye.y, eye.z};
    return c;
}

// `count` points of the head the camera sees face on, spread, each picked a few pixels off.
std::vector<CameraCorrespondence> picks(const ScalpMesh& head, const RefCamera& c, int count, double noise, int seed) {
    std::vector<std::size_t> seen;
    const Vec3f eye = c.eye();
    for (std::size_t i = 0; i < head.pointCount(); ++i) {
        const Vec3f& p = head.points()[i];
        float u = 0, v = 0;
        if (!c.project(p, u, v) || u < 10 || v < 10 || u > c.width - 10 || v > c.height - 10) continue;
        if (head.vertexNormals()[i].dot((eye - p).normalized()) < 0.5f) continue;
        seen.push_back(i);
    }
    std::vector<CameraCorrespondence> out;
    for (int k = 0; k < count && !seen.empty(); ++k) {
        const Vec3f& p = head.points()[seen[(static_cast<std::size_t>(k) * 7919u + static_cast<std::size_t>(seed) * 131u) % seen.size()]];
        float u = 0, v = 0;
        c.project(p, u, v);
        const double du = noise * std::sin(1.7 * (k + 1) + seed), dv = noise * std::cos(2.3 * (k + 1) + seed);
        out.push_back({u + du, v + dv, {p.x, p.y, p.z}});
    }
    return out;
}

double lookAngle(const RefCamera& a, const RefCamera& b) {
    return std::acos(std::clamp(static_cast<double>(a.axis(2).dot(b.axis(2))), -1.0, 1.0)) * 57.29578;
}

}  // namespace

TEST(AgentShoot, PhotosOfOneShootSolveTogether) {
    const auto head = nikaHead();
    const ScalpLandmarks m = scalpLandmarks(*head, "Y");
    // Four photos round the head with a 120 mm lens at about 90 cm, a little off level.
    const double yaws[4] = {0.0, -90.0, 180.0, 45.0}, pitches[4] = {2.0, -1.0, 3.0, 0.0}, distances[4] = {90.0, 92.0, 88.0, 91.0};
    std::vector<RefCamera> truth;
    std::vector<ShootView> views;
    for (int i = 0; i < 4; ++i) {
        truth.push_back(shootCamera(m, yaws[i], pitches[i], distances[i], 120.0));
        ShootView v;
        v.pairs = picks(*head, truth.back(), 10, 2.0, i);
        v.width = 385;
        v.height = 510;
        v.yawDegrees = yaws[i];
        views.push_back(v);
    }
    ShootOptions o;
    o.center = m.middle;
    o.up = m.frame.up;
    o.front = m.frame.front;
    const ShootSolve s = solveShoot(views, o);
    ASSERT_TRUE(s.ok) << s.why;
    EXPECT_NEAR(s.focalMm, 120.0, 18.0);
    for (int i = 0; i < 4; ++i) {
        EXPECT_LT(lookAngle(s.cameras[static_cast<std::size_t>(i)].camera, truth[static_cast<std::size_t>(i)]), 3.0) << i;
        EXPECT_LT(s.cameras[static_cast<std::size_t>(i)].rms, 4.0) << i;
        EXPECT_FALSE(s.cameras[static_cast<std::size_t>(i)].fights) << i;
    }
    // A photo whose picks are muddled (each pixel paired with another point): it fights the rest.
    std::vector<ShootView> wrong = views;
    std::vector<CameraCorrespondence>& muddled = wrong[3].pairs;
    for (std::size_t k = 0; k + 3 < muddled.size(); k += 2) std::swap(muddled[k].u, muddled[k + 3].u), std::swap(muddled[k].v, muddled[k + 3].v);
    const ShootSolve f = solveShoot(wrong, o);
    ASSERT_TRUE(f.ok);
    EXPECT_TRUE(f.cameras[3].fights);
}

TEST(AgentShoot, ALoneProfileWithFourPicksStaysNearItsView) {
    const auto head = nikaHead();
    const ScalpLandmarks m = scalpLandmarks(*head, "Y");
    const RefCamera truth = shootCamera(m, -90.0, 0.0, 90.0, 110.0);
    ShootView v;
    v.pairs = picks(*head, truth, 4, 4.0, 3);
    v.width = 385;
    v.height = 510;
    v.yawDegrees = -90.0;
    // The shoot's focal from the photos lined up already.
    ShootOptions o;
    o.center = m.middle;
    o.up = m.frame.up;
    o.front = m.frame.front;
    o.focalMm = 110.0;
    const ShootSolve s = solveShoot({v}, o);
    ASSERT_TRUE(s.ok) << s.why;
    EXPECT_LT(lookAngle(s.cameras[0].camera, truth), 8.0);
    EXPECT_LT(std::abs(s.cameras[0].pitch), 8.0);
}

TEST(AgentShoot, AFreeFocalPortraitStaysALongLens) {
    const auto head = nikaHead();
    const ScalpLandmarks m = scalpLandmarks(*head, "Y");
    // A 135 mm portrait (near orthographic), picks a few pixels off: not a 60 mm solve.
    const RefCamera truth = shootCamera(m, 0.0, 1.0, 110.0, 135.0);
    ShootView v;
    v.pairs = picks(*head, truth, 10, 3.0, 5);
    v.width = 385;
    v.height = 510;
    v.yawDegrees = 0.0;
    ShootOptions o;
    o.center = m.middle;
    o.up = m.frame.up;
    o.front = m.frame.front;
    const ShootSolve s = solveShoot({v}, o);
    ASSERT_TRUE(s.ok) << s.why;
    EXPECT_GT(s.focalMm, 95.0) << s.focalNote;
    EXPECT_LT(s.focalMm, 180.0);
    EXPECT_LT(lookAngle(s.cameras[0].camera, truth), 4.0);
}
