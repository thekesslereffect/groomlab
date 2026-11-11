// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include <nlohmann/json.hpp>
#include "../core/test_helpers.h"
#include "groomlab/groom_session.h"
#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/guide_set.h"
#include "groomlab/operators.h"

namespace groomlab {
namespace {

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

// Three guides, IDs 1-3, five points up +Y from z = 0, 0.1, 0.2.
GuidesPtr restGuides() {
    auto g = std::make_shared<GuideSet>();
    for (std::uint64_t id = 1; id <= 3; ++id) {
        const float z = 0.1f * static_cast<float>(id - 1);
        std::vector<Vec3f> pts;
        for (int k = 0; k < 5; ++k) pts.push_back({0.0f, 0.25f * static_cast<float>(k), z});
        g->add(id, pts);
    }
    return g;
}

// The simulated curves: the guides with these IDs swung +X by `dx` at the tip (three points:
// another count than the guides', resampled), each with its strandId.
StrandSet swung(const std::vector<std::uint64_t>& ids, float dx) {
    StrandSet s;
    for (const std::uint64_t id : ids) {
        const float z = 0.1f * static_cast<float>(id - 1);
        s.addStrand(id, {{0.0f, 0.0f, z}, {0.5f * dx, 0.5f, z}, {dx, 1.0f, z}});
    }
    return s;
}

void writeCache(const std::string& path, const std::vector<StrandSet>& frames) {
    AlembicCurvesWriter writer(path, "simGuides", 1.0, 1.0);  // frames at t = 1, 2, ...
    for (const StrandSet& f : frames) writer.writeFrame(f);
}

struct CacheGraph {
    Graph graph;
    ExternalInputs externals;
    NodeId guides, cache;

    explicit CacheGraph(const std::string& path) {
        registerAlembicOperators(OperatorRegistry::instance());
        externals.set("guides", restGuides(), 7);
        guides = graph.addNode(op::kGuides);
        cache = graph.addNode(op::kGuideCache);
        graph.node(cache).params().set("path", path);
        graph.connect(guides, cache, 0);
    }

    GuidesPtr eval(double seconds, double fps = 1.0) {
        EvalContext ctx;
        ctx.time = seconds;
        ctx.framesPerSecond = fps;
        ctx.externals = &externals;
        return valueAs<GuidesPtr>(graph.evaluate(cache, ctx));
    }
    const Guide& guide(const GuidesPtr& g, std::uint64_t id) {
        for (const Guide& x : g->guides()) {
            if (x.id == id) return x;
        }
        throw std::runtime_error("no guide");
    }
};

TEST(GuideCache, PosesTheGuidesItsCurvesNameAndKeepsTheRest) {
    const std::string path = tempPath("groomlab_guide_cache_pose.abc");
    writeCache(path, {swung({2, 3}, 0.4f)});  // guide 1 not in the cache
    CacheGraph g(path);
    const GuidesPtr out = g.eval(1.0);
    ASSERT_TRUE(out);
    ASSERT_EQ(out->size(), 3u);
    const Guide& one = g.guide(out, 1);
    EXPECT_NEAR(one.points.back().x, 0.0f, 1e-6f);  // passes through
    for (const std::uint64_t id : {2u, 3u}) {
        const Guide& posed = g.guide(out, id);
        ASSERT_EQ(posed.points.size(), 5u);  // the guide's own count (the curve resampled)
        EXPECT_NEAR(posed.points.back().x, 0.4f, 1e-4f);
        EXPECT_NEAR(posed.points.front().x, 0.0f, 1e-4f);  // the root stays
    }
    std::remove(path.c_str());
}

TEST(GuideCache, ReadsTheFrameWithItsOffsetAndBlends) {
    const std::string path = tempPath("groomlab_guide_cache_frames.abc");
    writeCache(path, {swung({1, 2, 3}, 0.2f), swung({1, 2, 3}, 0.6f)});
    CacheGraph g(path);
    EXPECT_NEAR(g.guide(g.eval(1.0), 1).points.back().x, 0.2f, 1e-4f);
    EXPECT_NEAR(g.guide(g.eval(2.0), 1).points.back().x, 0.6f, 1e-4f);
    g.graph.node(g.cache).params().set("frameOffset", 1.0);  // frame 1 shows the cache's frame 2
    EXPECT_NEAR(g.guide(g.eval(1.0), 1).points.back().x, 0.6f, 1e-4f);
    g.graph.node(g.cache).params().set("frameOffset", 0.0);
    g.graph.node(g.cache).params().set("blend", 0.5);  // halfway between the guides and the cache
    EXPECT_NEAR(g.guide(g.eval(1.0), 1).points.back().x, 0.1f, 1e-4f);
    std::remove(path.c_str());
}

TEST(GuideCache, ByOrderForACacheWithoutIdsAndNoFileIsAPassThrough) {
    const std::string path = tempPath("groomlab_guide_cache_order.abc");
    writeCache(path, {swung({40, 41, 42}, 0.3f)});  // IDs no guide has
    CacheGraph g(path);
    const GuidesPtr byId = g.eval(1.0);
    for (const Guide& x : byId->guides()) EXPECT_NEAR(x.points.back().x, 0.0f, 1e-6f);  // none matched
    g.graph.node(g.cache).params().set("matchBy", std::string("order"));
    const GuidesPtr byOrder = g.eval(1.0);
    for (const Guide& x : byOrder->guides()) EXPECT_NEAR(x.points.back().x, 0.3f, 1e-4f);
    // No file: the guides as fed, the same data.
    g.graph.node(g.cache).params().set("path", std::string());
    const GuidesPtr fed = g.eval(1.0);
    for (const Guide& x : fed->guides()) EXPECT_NEAR(x.points.back().x, 0.0f, 1e-6f);
    std::remove(path.c_str());
}

TEST(GuideCache, TheViewGivesTheGuidesItMovesForTheViewport) {
    // The session's view: the guides Interpolate takes after the GuideCache (ViewResult::
    // movingGuides, drawn over the groomed guides); none when the node is bypassed or reads nothing.
    const std::string path = tempPath("groomlab_guide_cache_view.abc");
    writeCache(path, {swung({1, 2, 3}, 0.4f)});
    registerAlembicOperators(OperatorRegistry::instance());
    auto graphWith = [&](const std::string& cachePath, bool bypass) {
        nlohmann::json g = {
            {"version", 1},
            {"nodes",
             {{{"name", "scalp"}, {"type", "Scalp"}},
              {{"name", "guides"}, {"type", "Guides"}},
              {{"name", "cache"}, {"type", "GuideCache"}, {"params", {{"path", cachePath}}}, {"bypass", bypass}},
              {{"name", "scatter"}, {"type", "Scatter"}, {"params", {{"density", 40.0}}}},
              {{"name", "interpolate"}, {"type", "Interpolate"}},
              {{"name", "output"}, {"type", "Output"}}}},
            {"connections",
             {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
              {{"from", "guides"}, {"to", "cache"}, {"input", "guides"}},
              {{"from", "cache"}, {"to", "interpolate"}, {"input", "guides"}},
              {{"from", "scatter"}, {"to", "interpolate"}, {"input", "strands"}},
              {{"from", "interpolate"}, {"to", "output"}, {"input", "strands"}}}},
            {"output", "output"}};
        return g.dump();
    };
    auto moving = [&](const std::string& graph) {
        GroomData data;
        data.scalp = test::makePlane(8);
        data.guides = restGuides();
        data.graphJson = graph;
        GroomSession session(data);
        ViewRequest request;
        request.time = 1.0;
        return session.evaluate(request).movingGuides;
    };
    const GuidesPtr moved = moving(graphWith(path, false));
    ASSERT_TRUE(moved);
    ASSERT_EQ(moved->size(), 3u);
    EXPECT_NEAR(moved->guides()[0].points.back().x, 0.4f, 1e-4f);
    EXPECT_FALSE(moving(graphWith(path, true)));          // bypassed
    EXPECT_FALSE(moving(graphWith(std::string(), false)));  // no file: the guides pass through
    std::remove(path.c_str());
}

TEST(GuideCache, LinkedByHandIntoAClumpEachClumpFollowsItsGuide) {
    // A GuideCache's guides (or an ImportCurves'), linked by hand into a picking Clump's centers,
    // directly or through a curve node: centersFrom inputCurves, each clump along its guide.
    registerAlembicOperators(OperatorRegistry::instance());
    for (const char* kind : {"GuideCache", "ImportCurves"}) {
        for (const char* source : {"moved", "wave"}) {
            const nlohmann::json g = {
                {"version", 1},
                {"nodes",
                 {{{"name", "s"}, {"type", "Scatter"}},
                  {{"name", "c"}, {"type", "Clump"}},
                  {{"name", "moved"}, {"type", kind}},
                  {{"name", "wave"}, {"type", "Noise"}}}},
                {"connections",
                 {{{"from", "s"}, {"to", "c"}, {"input", "strands"}}, {{"from", "moved"}, {"to", "wave"}, {"input", "strands"}}}}};
            GraphEdit edit(g.dump());
            EXPECT_TRUE(edit.connectByHand(source, "c", "centers")) << kind << " " << source;
            const nlohmann::json now = nlohmann::json::parse(edit.toJson());
            EXPECT_EQ(now["nodes"][1]["params"].value("centersFrom", std::string()), "inputCurves") << kind << " " << source;
        }
    }
}

TEST(GuideCache, AMissingFileIsTheNodesError) {
    CacheGraph g(tempPath("groomlab_guide_cache_missing.abc"));
    EXPECT_THROW(g.eval(1.0), GraphError);
}

}  // namespace
TEST(GuideCache, LoadGuideCacheGoesAfterTheGuidesInEveryHost) {
    // graph_inserts.h insertGuideCache (Maya's add_guide_cache, the Blender helper, the app): right
    // after the Guides node, feeding all it fed; loaded again, the same node reads the new file.
    registerAlembicOperators(OperatorRegistry::instance());
    GraphEdit edit(defaultGraphJson());
    const std::string cache = insertGuideCache(edit, "sims/take1.abc", "hair");
    const nlohmann::json g = nlohmann::json::parse(edit.toJson());
    bool fromGuides = false, intoInterpolate = false, guidesStillDirect = false;
    for (const nlohmann::json& c : g["connections"]) {
        fromGuides = fromGuides || (c["from"] == "guides" && c["to"] == cache && c["input"] == "guides");
        intoInterpolate = intoInterpolate || (c["from"] == cache && c["to"] == "interpolate" && c["input"] == "guides");
        guidesStillDirect = guidesStillDirect || (c["from"] == "guides" && c["to"] == "interpolate");
    }
    EXPECT_TRUE(fromGuides);
    EXPECT_TRUE(intoInterpolate);
    EXPECT_FALSE(guidesStillDirect);
    EXPECT_EQ(insertGuideCache(edit, "sims/take2.abc"), cache);
    const nlohmann::json again = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& n : again["nodes"]) {
        if (n["name"] != cache) continue;
        EXPECT_EQ(n["params"]["path"], "sims/take2.abc");
        EXPECT_EQ(n["params"]["object"], "");
    }
}

}  // namespace groomlab
