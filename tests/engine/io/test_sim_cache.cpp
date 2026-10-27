// SPDX-License-Identifier: Apache-2.0
// Simulate, bake to a cache, render: a groom file's scene steps its Simulate node over the scalp
// cache's frames (its feed), a bake written from the session reads back as the same frames, and a
// groom file naming the bake renders them without simulating (unless the node's settings changed).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#include "../core/test_helpers.h"
#include "groomlab/alembic_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_scene.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/sim_cache.h"

namespace groomlab {
namespace {

// A temporary file of the running test's own: ctest runs each test in a process of its own, side by
// side, and several build a Scene (one test removing the cache another is reading).
std::string tempPath(const char* name) {
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    return (std::filesystem::temp_directory_path() / (test + "_" + name)).string();
}

const char* kGraph = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "damping": 0.2}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
  "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                  {"from": "scatter", "to": "interpolate", "input": "strands"},
                  {"from": "guides", "to": "sim", "input": "guides"},
                  {"from": "sim", "to": "interpolate", "input": "guides"},
                  {"from": "scalp", "to": "interpolate", "input": "scalp"},
                  {"from": "interpolate", "to": "output", "input": "strands"}], "output": "output"})";

// A plane scalp sliding +x over frames 2-8 and still after, cached at 24 fps; guides standing on it.
struct Scene {
    MeshPtr plane = test::makePlane(4);
    std::string cache = tempPath("groomlab_sim_scalp.abc");
    Scene() {
        AlembicMeshWriter writer(cache, "head", 0.0, 1.0 / 24.0);
        for (int f = 0; f <= 30; ++f) {
            std::vector<Vec3f> points = plane->points();
            for (Vec3f& p : points) p.x += 0.1f * static_cast<float>(std::clamp(f - 2, 0, 6));
            writer.writeFrame(plane->withPoints(std::move(points)));
        }
    }
    ~Scene() {
        std::error_code ec;
        std::filesystem::remove(cache, ec);
    }
    GroomFile file() const {
        GuideHierarchy h;
        const GroupId group = h.addGroup("primary");
        for (int i = 0; i < 6; ++i) {
            const Vec3f root{0.15f + 0.14f * static_cast<float>(i), 0.0f, 0.5f};
            h.addGuide(group, test::straightCurve(root, {0.1f, 0.6f, 0.0f}, 8));
        }
        h.evaluate(plane.get());
        GroomFile f;
        f.data.scalp = plane;
        f.data.graphJson = kGraph;
        f.data.hierarchyJson = h.toJson();
        f.scalpCache = {cache, "head"};
        return f;
    }
};

TEST(SimulationCache, TheSceneSimulatesOverTheScalpCacheAndABakeRendersTheSameFrames) {
    Scene scene;
    GroomScene played(scene.file());
    // Simulate Range over the cache's frames: the scene's feed gives each frame's scalp.
    played.setTime(1 / 24.0);
    const SimRun run = played.session().simulateTo(24);
    EXPECT_TRUE(run.errors.empty());
    EXPECT_EQ(run.frames, 24);
    // A fresh scene jumping straight to frame 20 steps the same frames on the way.
    GroomScene jumped(scene.file());
    EXPECT_EQ(jumped.evaluate(20 / 24.0)->positions(), played.evaluate(20 / 24.0)->positions());
    // The motion reached the guides: the simulated frame differs from the guides carried rigidly.
    GroomFile still = scene.file();
    std::string bypassed = kGraph;
    bypassed.replace(bypassed.find(R"("type": "Simulate",)"), 19, R"("type": "Simulate", "bypass": true,)");
    still.data.graphJson = bypassed;
    EXPECT_NE(GroomScene(still).evaluate(6 / 24.0)->positions(), played.evaluate(6 / 24.0)->positions());

    // Bake: the frames as an Alembic guide cache, named in a groom file with the settings' stamp.
    const std::string bake = tempPath("groomlab_sim_bake.abc");
    EXPECT_EQ(writeSimulationCache(played.session(), "sim", bake, 1, 24), 24u);
    GroomFile baked = scene.file();
    baked.simCaches["sim"] = {bake, "", played.session().simulationStamp("sim")};
    const std::string groom = tempPath("groomlab_sim_baked.groom");
    writeGroomFile(groom, baked);
    {
        GroomScene render(groom);
        ASSERT_EQ(render.file().simCaches.size(), 1u);
        EXPECT_EQ(render.file().simCaches.at("sim").stamp, played.session().simulationStamp("sim"));
        for (int f : {1, 7, 12, 24}) {
            const StrandsPtr a = render.evaluate(f / 24.0), b = played.evaluate(f / 24.0);
            ASSERT_EQ(a->pointCount(), b->pointCount());
            float worst = 0.0f;
            for (std::size_t i = 0; i < a->pointCount(); ++i) worst = std::max(worst, (a->positions()[i] - b->positions()[i]).length());
            EXPECT_LT(worst, 1e-5f) << "frame " << f;
        }
        const std::vector<SimStatus> status = render.session().simulationStatus();
        ASSERT_EQ(status.size(), 1u);
        EXPECT_TRUE(status[0].baked);
        EXPECT_EQ(status[0].bakedFirst, 1);
        EXPECT_EQ(status[0].bakedLast, 24);
        EXPECT_EQ(status[0].frames, 0u) << "the render simulated rather than read the bake";
    }
    {
        // Other settings since the bake: it is stale, ignored, and the node simulates.
        GroomFile changed = baked;
        changed.simCaches["sim"].stamp ^= 1;
        GroomScene render(std::move(changed));
        render.evaluate(4 / 24.0);
        const SimStatus status = render.session().simulationStatus()[0];
        EXPECT_FALSE(status.baked);
        EXPECT_TRUE(status.bakeStale);
        EXPECT_EQ(status.frames, 4u);
    }
    // A frame not simulated is not baked.
    EXPECT_THROW(writeSimulationCache(played.session(), "sim", bake, 1, 40), std::runtime_error);
    std::error_code ec;
    std::filesystem::remove(bake, ec);
    std::filesystem::remove(groom, ec);
}

TEST(SimulationCache, AWetSimulationBakesItsWetness) {
    // The guides under water: the bake carries each point's wetness, and the render reads it back
    // as the strands' wetness channel.
    Scene scene;
    GroomFile wet = scene.file();
    std::string graph = kGraph;
    const std::string params = R"("damping": 0.2})";
    graph.replace(graph.find(params), params.size(), R"("damping": 0.2, "water": true, "waterLevel": 0.3})");
    wet.data.graphJson = graph;
    GroomScene played(wet);
    played.setTime(1 / 24.0);
    played.session().simulateTo(12);
    const std::string bake = tempPath("groomlab_sim_wet_bake.abc");
    writeSimulationCache(played.session(), "sim", bake, 1, 12);
    wet.simCaches["sim"] = {bake, "", played.session().simulationStamp("sim")};
    {
        GroomScene render(wet);
        const StrandsPtr a = render.evaluate(12 / 24.0), b = played.evaluate(12 / 24.0);
        EXPECT_TRUE(render.session().simulationStatus()[0].baked);
        const std::vector<float>* baked = a->pointChannel(kWetnessChannel);
        const std::vector<float>* live = b->pointChannel(kWetnessChannel);
        ASSERT_TRUE(baked && live);
        ASSERT_EQ(baked->size(), live->size());
        float worst = 0.0f, most = 0.0f;
        for (std::size_t i = 0; i < baked->size(); ++i) {
            worst = std::max(worst, std::abs((*baked)[i] - (*live)[i]));
            most = std::max(most, (*live)[i]);
        }
        EXPECT_LT(worst, 1e-6f);
        EXPECT_GT(most, 0.5f) << "nothing got wet: the test shows nothing";
    }
    std::error_code ec;
    std::filesystem::remove(bake, ec);
}

// The worst distance between two sets of strands' points (infinity for other counts).
float worstApart(const StrandSet& a, const StrandSet& b) {
    if (a.pointCount() != b.pointCount()) return std::numeric_limits<float>::infinity();
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.pointCount(); ++i) worst = std::max(worst, (a.positions()[i] - b.positions()[i]).length());
    return worst;
}

// Guides the graph makes (ToGuides of strands grown from the hand guides) into the Simulate node:
// IDs that are not the hierarchy's.
const char* kMadeGuidesGraph = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
    {"name": "dense", "type": "Interpolate"},
    {"name": "toGuides", "type": "ToGuides"},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "damping": 0.2}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
  "connections": [{"from": "scalp", "to": "even", "input": "scalp"},
                  {"from": "even", "to": "dense", "input": "strands"},
                  {"from": "guides", "to": "dense", "input": "guides"},
                  {"from": "scalp", "to": "dense", "input": "scalp"},
                  {"from": "dense", "to": "toGuides", "input": "strands"},
                  {"from": "toGuides", "to": "sim", "input": "guides"},
                  {"from": "scalp", "to": "scatter", "input": "scalp"},
                  {"from": "scatter", "to": "interpolate", "input": "strands"},
                  {"from": "sim", "to": "interpolate", "input": "guides"},
                  {"from": "scalp", "to": "interpolate", "input": "scalp"},
                  {"from": "interpolate", "to": "output", "input": "strands"}], "output": "output"})";

TEST(SimulationCache, PlayingSimulateRangeAndTheBakeShowTheSameFrames) {
    // A head turning and moving over frames 1-10 and still after, played frame by frame (the
    // live view stepping), simulated with Simulate Range (the host's feed giving each frame's
    // scalp) and baked then read back in cached mode: the same frames. Following the scalp or not,
    // of the hierarchy's guides or of guides the graph makes.
    const auto sphere = test::makeSphere(12, 24);
    auto pointsAt = [&](int frame) {
        const float t = static_cast<float>(std::clamp(frame - 1, 0, 9));
        const float a = 0.08f * t, c = std::cos(a), s = std::sin(a);
        std::vector<Vec3f> points = sphere->points();
        for (Vec3f& p : points) p = Vec3f{c * p.x + s * p.z + 0.05f * t, p.y + 0.02f * t, -s * p.x + c * p.z};
        return points;
    };
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    for (int i = 0; i < 10; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 10.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(group, test::straightCurve(n, n * 0.8f + Vec3f{0.3f, 0.0f, 0.2f}, 8));
    }
    h.evaluate(sphere.get());
    const std::string sim = R"("damping": 0.2})";
    for (const int run : {0, 1, 2, 3}) {
        const bool follow = run % 2 == 1, made = run >= 2;
        SCOPED_TRACE(std::string(follow ? "following the scalp" : "not following the scalp") + (made ? ", guides made" : ""));
        GroomData data;
        auto scalp = std::make_shared<ScalpMesh>(*sphere);
        scalp->setRestPoints(sphere->points());
        data.scalp = scalp;
        data.graphJson = made ? kMadeGuidesGraph : kGraph;
        data.hierarchyJson = h.toJson();
        data.followScalp = follow;
        auto view = [](GroomSession& session, int frame) {
            ViewRequest request;
            request.time = frame / 24.0;
            const ViewResult& result = session.evaluate(request);
            EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front());
            return result.strands;
        };
        auto feed = [&](GroomSession& session) {
            session.setSimulationFeed([&session, &pointsAt](double seconds) {
                session.updateScalpPoints(pointsAt(static_cast<int>(std::lround(seconds * 24.0))));
                return true;
            });
        };
        constexpr int kLast = 16;
        GroomSession played(data);
        feed(played);
        std::vector<StrandsPtr> playedStrands(kLast + 1);
        for (int f = 1; f <= kLast; ++f) {
            played.updateScalpPoints(pointsAt(f));
            playedStrands[f] = view(played, f);
        }
        GroomSession ranged(data);
        feed(ranged);
        ranged.updateScalpPoints(pointsAt(1));
        view(ranged, 1);
        EXPECT_TRUE(ranged.simulateTo(kLast).errors.empty());
        for (int f = 1; f <= kLast; ++f) {
            const GuidesPtr a = played.simulatedFrame("sim", f), b = ranged.simulatedFrame("sim", f);
            ASSERT_TRUE(a && b) << "frame " << f;
            ASSERT_EQ(a->size(), b->size());
            for (std::size_t g = 0; g < a->size(); ++g) {
                EXPECT_EQ(a->guides()[g].points, b->guides()[g].points) << "Simulate Range's frame " << f << ", guide " << g;
            }
            ranged.updateScalpPoints(pointsAt(f));
            EXPECT_EQ(worstApart(*view(ranged, f), *playedStrands[f]), 0.0f) << "Simulate Range's strands at frame " << f;
        }
        const std::string bake = tempPath(("groomlab_sim_bake" + std::to_string(run) + ".abc").c_str());
        writeSimulationCache(ranged, "sim", bake, 1, kLast);
        {
            GroomData cachedData = data;
            cachedData.graphJson.replace(cachedData.graphJson.find(sim), sim.size(), R"("damping": 0.2, "cacheMode": "cached"})");
            GroomSession cached(cachedData);
            cached.setBakedSimulation("sim", readSimulationCache(bake, "", cached.simulationStamp("sim")));
            for (int f = 1; f <= kLast; ++f) {
                cached.updateScalpPoints(pointsAt(f));
                // (The bake's round trip moves the strands by about a millionth: 0.99e-6 before
                // smoothPolyline began each span at its point exactly, 1.12e-6 since.)
                EXPECT_LT(worstApart(*view(cached, f), *playedStrands[f]), 2e-6f) << "the bake's strands at frame " << f;
            }
            EXPECT_TRUE(cached.simulationStatus()[0].baked);
            EXPECT_EQ(cached.simulationStatus()[0].frames, 0u);
        }
        std::error_code ec;
        std::filesystem::remove(bake, ec);
    }
}

TEST(SimulationCache, GroomFilesKeepTheBakesTheyName) {
    GroomFile file;
    file.data.scalp = test::makePlane(2);
    file.simCaches["hair"] = {"hair_sim/hair.abc", "hair", 0x0123456789ABCDEFull};
    file.simCaches["beard"] = {"beard.abc", "", 7};
    const GroomFile back = groomFileFromJson(groomFileToJson(file), "C:/shows/abc");
    ASSERT_EQ(back.simCaches.size(), 2u);
    EXPECT_EQ(back.simCaches.at("hair").stamp, 0x0123456789ABCDEFull);
    EXPECT_EQ(back.simCaches.at("hair").object, "hair");
    EXPECT_EQ(std::filesystem::path(back.simCaches.at("beard").file), std::filesystem::path("C:/shows/abc/beard.abc"));
    EXPECT_EQ(formatSimulationStamp(7), "0000000000000007");
    EXPECT_EQ(parseSimulationStamp("not hex"), 0u);
}

}  // namespace
}  // namespace groomlab
