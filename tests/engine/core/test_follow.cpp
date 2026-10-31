// SPDX-License-Identifier: Apache-2.0
// A scalp that moves: meshes made by withPoints (the same topology, the closest-point tree
// refitted rather than built again) answer as meshes built from scratch, and evaluations on a
// deforming scalp give the same strands either way. Following the scalp (follow_scalp.h): the
// groom at rest carried root by root.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "groomlab/display.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/strand_geometry.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A frame of a deforming head: squashed, twisted about Y and moved, a little more each frame.
std::vector<Vec3f> deformedPoints(const ScalpMesh& rest, int frame) {
    std::vector<Vec3f> points = rest.points();
    const float f = static_cast<float>(frame);
    for (Vec3f& p : points) {
        const float a = 0.05f * f * (1.0f + p.y);
        const float c = std::cos(a), s = std::sin(a);
        p = Vec3f(p.x * c - p.z * s, p.y * (1.0f - 0.01f * f), p.x * s + p.z * c) + Vec3f(0.02f * f, 0.0f, -0.01f * f);
    }
    return points;
}

// The same mesh built from scratch: points, triangles, UVs, color sets and rest pose.
ScalpMesh rebuilt(const ScalpMesh& like, std::vector<Vec3f> points) {
    ScalpMesh mesh(std::move(points), like.triangles());
    if (like.hasUVs()) mesh.setCornerUVs(like.cornerUVs());
    for (const std::string& name : like.colorSetNames()) mesh.setCornerColors(name, *like.cornerColors(name));
    if (like.hasRest()) mesh.setRestPoints(like.restPoints());
    return mesh;
}

std::shared_ptr<ScalpMesh> headWithEverything() {
    const auto sphere = test::makeSphere(24, 48, 1.0f);
    auto head = std::make_shared<ScalpMesh>(*sphere);
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> colors;
    for (std::size_t c = 0; c < head->triangleCount() * 3; ++c) {
        const Vec3f& p = head->points()[static_cast<std::size_t>(head->triangles()[c / 3][c % 3])];
        uvs.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y});
        colors.push_back({p.z * 0.5f + 0.5f, 0.25f, 0.75f, 1.0f});
    }
    head->setCornerUVs(std::move(uvs));
    head->setCornerColors("mask", std::move(colors));
    head->setRestPoints(sphere->points());
    return head;
}

void expectSamePoint(const SurfacePoint& a, const SurfacePoint& b) {
    EXPECT_EQ(a.root.face, b.root.face);
    EXPECT_EQ(a.root.u, b.root.u);
    EXPECT_EQ(a.root.v, b.root.v);
    EXPECT_EQ(a.position, b.position);
    EXPECT_EQ(a.normal, b.normal);
}

TEST(ScalpRefit, WithPointsMatchesAMeshBuiltFromScratch) {
    const auto head = headWithEverything();
    (void)head->closestPoint({0.0f, 2.0f, 0.0f});  // its tree is built: the next frames refit it
    ScalpMesh previous = *head;
    for (int frame = 1; frame <= 4; ++frame) {
        const std::vector<Vec3f> points = deformedPoints(*head, frame);
        const ScalpMesh moved = previous.withPoints(points);  // a chain of refits, frame after frame
        const ScalpMesh fresh = rebuilt(*head, points);
        EXPECT_EQ(moved.contentHash(), fresh.contentHash());
        EXPECT_EQ(moved.surfaceHash(), fresh.surfaceHash());
        EXPECT_EQ(moved.topologyHash(), fresh.topologyHash());
        EXPECT_EQ(moved.restSurfaceHash(), fresh.restSurfaceHash());
        EXPECT_EQ(moved.restPoseHash(), fresh.restPoseHash());
        EXPECT_EQ(moved.colorSetHash("mask"), fresh.colorSetHash("mask"));
        EXPECT_EQ(moved.totalArea(), fresh.totalArea());
        EXPECT_TRUE(moved.samePointsLayout(fresh));
        EXPECT_TRUE(moved.samePointsLayout(*head));
        EXPECT_FALSE(fresh.treeRefitted());
        EXPECT_TRUE(moved.treeRefitted());

        // Queries: random points near and far, every vertex pushed out along its normal (nearest
        // a shared vertex: exact ties between faces), edge midpoints, and rays through vertices.
        Rng rng(static_cast<std::uint64_t>(frame));
        std::vector<Vec3f> probes;
        for (int i = 0; i < 400; ++i) {
            probes.push_back(Vec3f(rng.uniform(-1.6f, 1.6f), rng.uniform(-1.6f, 1.6f), rng.uniform(-1.6f, 1.6f)));
        }
        for (std::size_t v = 0; v < points.size(); v += 3) {
            probes.push_back(points[v] * 1.1f);
            probes.push_back(points[v]);
        }
        for (std::size_t t = 0; t < moved.triangleCount(); t += 7) {
            const auto& tri = moved.triangles()[t];
            probes.push_back((points[static_cast<std::size_t>(tri[0])] + points[static_cast<std::size_t>(tri[1])]) * 0.5f * 1.05f);
        }
        for (const Vec3f& p : probes) {
            expectSamePoint(moved.closestPoint(p), fresh.closestPoint(p));
            expectSamePoint(moved.closestPoint(p, 0.05f), fresh.closestPoint(p, 0.05f));
            float lowA = 0, lowB = 0;
            expectSamePoint(moved.closestPointBounded(p, 0.2f, &lowA), fresh.closestPointBounded(p, 0.2f, &lowB));
            if (std::isfinite(lowA) && fresh.closestPoint(p, 0.2f).root.face >= 0) EXPECT_EQ(lowA, lowB);
        }
        for (std::size_t v = 0; v < points.size(); v += 5) {
            const Vec3f origin = points[v] * 3.0f;  // straight through the vertex: ties between its faces
            SurfacePoint a, b;
            float ta = 0, tb = 0;
            const bool hitA = moved.intersectRay(origin, points[v] - origin, &a, &ta);
            const bool hitB = fresh.intersectRay(origin, points[v] - origin, &b, &tb);
            ASSERT_EQ(hitA, hitB);
            if (hitA) {
                expectSamePoint(a, b);
                EXPECT_EQ(ta, tb);
            }
        }
        previous = moved;
    }
}

TEST(ScalpRefit, AMeshNeverQueriedHandsNoTreeOnAndItsCopiesBuildTheirOwn) {
    const auto head = headWithEverything();
    const ScalpMesh moved = head->withPoints(deformedPoints(*head, 2));
    EXPECT_FALSE(moved.treeRefitted());  // the source never built a tree: this one builds its own
    const ScalpMesh next = moved.withPoints(deformedPoints(*head, 3));
    EXPECT_TRUE(next.treeRefitted());
    EXPECT_THROW((void)head->withPoints({Vec3f{}}), std::invalid_argument);
}

const char* kDeformingGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "comb", "type": "Gravity", "params": {"strength": 0.7}},
    {"name": "collide", "type": "Collide", "params": {"maxDepth": 0.05}},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "comb", "input": "strands"},
    {"from": "comb", "to": "collide", "input": "strands"},
    {"from": "scalp", "to": "collide", "input": "scalp"},
    {"from": "collide", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::string guidesJson(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(9);
    for (int i = 0; i < 24; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.2f, 0), 8));
    }
    h.evaluate(&scalp);
    ChildGenerationParams p;
    p.count = 3;
    h.generateChildren(primary, "children", p);
    h.evaluate(&scalp);
    return h.toJson();
}

TEST(ScalpRefit, ADeformingScalpSequenceEvaluatesAsFreshScalpsDo) {
    // Frame after frame, one session fed the points (updateScalpPoints: refits) and one fed a
    // scalp built from scratch on every frame: the same strands, to the bit.
    const auto head = headWithEverything();
    GroomData data;
    data.graphJson = kDeformingGraph;
    data.hierarchyJson = guidesJson(*head);
    data.scalp = head;
    GroomSession refit(data), fresh(data);
    ASSERT_EQ(refit.evaluate(1.0f, 0.0)->contentHash(), fresh.evaluate(1.0f, 0.0)->contentHash());
    for (int frame = 1; frame <= 5; ++frame) {
        const std::vector<Vec3f> points = deformedPoints(*head, frame);
        refit.updateScalpPoints(points);
        fresh.setScalp(std::make_shared<const ScalpMesh>(rebuilt(*head, points)));
        const StrandsPtr a = refit.evaluate(1.0f, frame / 24.0);
        const StrandsPtr b = fresh.evaluate(1.0f, frame / 24.0);
        ASSERT_TRUE(a && b);
        ASSERT_GT(a->strandCount(), 100u);
        EXPECT_EQ(a->contentHash(), b->contentHash()) << "frame " << frame;
        EXPECT_EQ(refit.rawScalp()->contentHash(), fresh.rawScalp()->contentHash());
        EXPECT_EQ(refit.hierarchy()->contentHash(), fresh.hierarchy()->contentHash());
        EXPECT_TRUE(refit.rawScalp()->treeRefitted());  // Collide and the guides queried a refitted tree
    }
}

// ---- Following the scalp ----------------------------------------------------------------------

const char* kLookGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.6}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.03}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// A rigid motion: a turn about an axis, then a move.
struct Rigid {
    Vec3f axis = Vec3f(0.3f, 1.0f, 0.2f).normalized();
    float angle = 0.7f;
    Vec3f move{0.4f, -0.2f, 1.1f};
    Vec3f operator()(const Vec3f& p) const {
        const float c = std::cos(angle), s = std::sin(angle);
        const Vec3f& k = axis;
        return p * c + k.cross(p) * s + k * (k.dot(p) * (1.0f - c)) + move;
    }
};

GroomData followData(const char* graph) {
    const auto head = headWithEverything();
    GroomData data;
    data.graphJson = graph;
    data.hierarchyJson = guidesJson(*head);
    data.scalp = head;
    return data;
}

float maxDistance(const StrandSet& a, const StrandSet& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.positions().size(); ++i) worst = std::max(worst, (a.positions()[i] - b.positions()[i]).length());
    return worst;
}

TEST(FollowScalp, AtRestOrWithoutARestPoseItIsTheGroomAsItIs) {
    GroomData data = followData(kLookGraph);
    GroomSession normal(data);
    data.followScalp = true;
    GroomSession follow(data);
    const StrandsPtr a = normal.evaluate(1.0f, 0.0);
    const StrandsPtr b = follow.evaluate(1.0f, 0.0);
    EXPECT_EQ(a->contentHash(), b->contentHash());  // the scalp at its rest pose: nothing to carry
    EXPECT_FALSE(follow.following());

    // A scalp without a rest pose: as it is, said once.
    auto moved = std::make_shared<ScalpMesh>(test::makeSphere(24, 48, 1.0f)->withPoints(deformedPoints(*data.scalp, 3)));
    normal.setScalp(moved);
    follow.setScalp(moved);
    const ViewResult& view = follow.evaluate(ViewRequest{});
    EXPECT_EQ(view.strands->contentHash(), normal.evaluate(1.0f, 0.0)->contentHash());
    ASSERT_EQ(view.messages.size(), 1u);
    EXPECT_NE(view.messages[0].text.find("no rest pose"), std::string::npos);
    EXPECT_TRUE(follow.evaluate(ViewRequest{}).messages.empty());
}

TEST(FollowScalp, ARigidlyMovedScalpCarriesTheGroomAtRestRigidly) {
    GroomData data = followData(kLookGraph);
    const StrandsPtr rest = GroomSession(data).evaluate(1.0f, 0.0);
    const Rigid motion;
    std::vector<Vec3f> points = data.scalp->points();
    for (Vec3f& p : points) p = motion(p);
    data.followScalp = true;
    GroomSession follow(data);
    follow.updateScalpPoints(points);
    const StrandsPtr carried = follow.evaluate(1.0f, 0.0);
    EXPECT_TRUE(follow.following());
    ASSERT_EQ(carried->strandCount(), rest->strandCount());
    EXPECT_EQ(carried->ids(), rest->ids());
    EXPECT_EQ(carried->widths(), rest->widths());
    float worst = 0.0f;
    for (std::size_t i = 0; i < rest->positions().size(); ++i) {
        worst = std::max(worst, (carried->positions()[i] - motion(rest->positions()[i])).length());
    }
    EXPECT_LT(worst, 2e-5f) << "the rest strands, moved rigidly";

    // Off, the same frame grows on the moved scalp (Noise is in world space): not the same.
    follow.setFollowScalp(false);
    EXPECT_GT(maxDistance(*follow.evaluate(1.0f, 0.0), *carried), 1e-3f);
}

TEST(FollowScalp, MotionKeysShareTheRestEvaluation) {
    // A deforming scalp at two motion keys and back: each key carries the one rest evaluation.
    GroomData data = followData(kLookGraph);
    data.followScalp = true;
    GroomSession keys(data);
    keys.setCacheIntermediates(true);
    std::vector<std::uint64_t> hashes;
    std::size_t runs = 0;
    for (const int frame : {2, 3, 2}) {
        keys.updateScalpPoints(deformedPoints(*data.scalp, frame));
        hashes.push_back(keys.evaluate(1.0f, frame / 24.0)->contentHash());
        if (frame == 2 && hashes.size() == 1) runs = keys.evaluationCount();
        GroomData fresh = data;
        fresh.scalp = std::make_shared<ScalpMesh>(data.scalp->withPoints(deformedPoints(*data.scalp, frame)));
        EXPECT_EQ(GroomSession(fresh).evaluate(1.0f, frame / 24.0)->contentHash(), hashes.back()) << "frame " << frame;
    }
    EXPECT_EQ(hashes[0], hashes[2]);
    EXPECT_NE(hashes[0], hashes[1]);
    EXPECT_EQ(keys.evaluationCount(), runs) << "the graph ran once, on the rest pose";
}

TEST(FollowScalp, AHostCarryingTheStrandsGetsThemAtRestAndTheSameBounds) {
    // ViewRequest::hostCarries (Maya's viewport carrying on the GPU): the strands at rest with what
    // carries them; placed() the strands the session carries itself, the bounds holding theirs, and the
    // strands' version moving with the scalp as theirs does.
    GroomData data = followData(kLookGraph);
    data.followScalp = true;
    GroomSession carried(data), handed(data);
    ViewRequest request;
    ViewRequest hosted = request;
    hosted.hostCarries = true;
    std::uint64_t version = 0, versionCarried = 0;
    StrandsPtr rest;
    for (const int frame : {2, 3, 3}) {
        SCOPED_TRACE("frame " + std::to_string(frame));
        carried.updateScalpPoints(deformedPoints(*data.scalp, frame));
        handed.updateScalpPoints(deformedPoints(*data.scalp, frame));
        const ViewResult& a = carried.evaluate(request);
        const ViewResult& b = handed.evaluate(hosted);
        EXPECT_FALSE(a.follow);
        ASSERT_TRUE(b.follow);
        EXPECT_EQ(b.follow->to(), handed.last().scalp);
        if (rest) {
            EXPECT_EQ(b.strands, rest);  // the one rest evaluation
        }
        rest = b.strands;
        EXPECT_EQ(b.placed()->contentHash(), a.strands->contentHash());
        EXPECT_EQ(b.placed(), b.placed());  // carried once
        // The bounds hold the carried strands' (each strand's box at rest carried), hardly more.
        EXPECT_TRUE(b.bounds.valid);
        const float diagonal = (a.bounds.hi - a.bounds.lo).length();
        for (int c = 0; c < 3; ++c) {
            EXPECT_LE((&b.bounds.lo.x)[c], (&a.bounds.lo.x)[c]);
            EXPECT_GE((&b.bounds.hi.x)[c], (&a.bounds.hi.x)[c]);
            EXPECT_LT((&a.bounds.lo.x)[c] - (&b.bounds.lo.x)[c], 0.05f * diagonal);
            EXPECT_LT((&b.bounds.hi.x)[c] - (&a.bounds.hi.x)[c], 0.05f * diagonal);
        }
        // The strands' version moves as the session carrying them itself moves it (with the scalp).
        EXPECT_EQ(b.versions.strands != version, a.versions.strands != versionCarried);
        version = b.versions.strands;
        versionCarried = a.versions.strands;
    }
    // At rest nothing is handed on (nothing to carry).
    handed.updateScalpPoints(data.scalp->points());
    EXPECT_FALSE(handed.evaluate(hosted).follow);
}

TEST(FollowScalp, AHostCarryingInstancedStrandsDrawsTheInstancesWhereTheSessionPlacesThem) {
    // Instances on the strands (Instance after the look, randomly turned ones too): the host still
    // gets the strands at rest to carry, and the display places the instances on their strands
    // carried, as where they sit on the strands the session carries itself. A scale (bent onto the
    // skin) leaves the carry to the session.
    std::string graph = kLookGraph;
    const std::string tail = R"({"from": "width", "to": "output", "input": "strands"})";
    graph.replace(graph.find(tail), tail.size(),
                  R"({"from": "width", "to": "snow", "input": "strands"}, {"from": "snow", "to": "dust", "input": "strands"},
                     {"from": "dust", "to": "output", "input": "strands"})");
    const std::string lastNode = R"({"name": "output", "type": "Output"})";
    graph.replace(graph.find(lastNode), lastNode.size(),
                  R"({"name": "snow", "type": "Instance", "params": {"probability": 0.4, "tMin": 0.3, "scale": 0.05}},
                     {"name": "dust", "type": "Instance", "params": {"probability": 0.2, "orient": "random", "seed": 2}},
                     {"name": "output", "type": "Output"})");
    GroomData data = followData(graph.c_str());
    data.followScalp = true;
    GroomSession carried(data), handed(data);
    ViewRequest hosted;
    hosted.hostCarries = true;
    DisplaySettings settings;
    settings.instances = true;
    DisplayBuilder fromSession, fromHost;
    for (const int frame : {2, 3}) {
        SCOPED_TRACE("frame " + std::to_string(frame));
        carried.updateScalpPoints(deformedPoints(*data.scalp, frame));
        handed.updateScalpPoints(deformedPoints(*data.scalp, frame));
        const ViewResult& a = carried.evaluate(ViewRequest{});
        const ViewResult& b = handed.evaluate(hosted);
        ASSERT_TRUE(a.strands && a.strands->instances() && !a.strands->instances()->empty());
        ASSERT_TRUE(b.follow) << "instances on the strands: still the host's to carry";
        const DisplayItem& want = fromSession.update(DisplayInputs::fromView(a), settings).item(DisplayItemId::InstanceMarkers);
        const DisplayItem& got = fromHost.update(DisplayInputs::fromView(b), settings).item(DisplayItemId::InstanceMarkers);
        ASSERT_TRUE(want.geometry && got.geometry);
        EXPECT_GT(want.vertexCount, 0u);
        EXPECT_EQ(got.geometry->digest(), want.geometry->digest());
    }
    // A scale: the session carries (it bends onto the skin as it is now).
    std::string scaled = graph;
    const std::string snow = R"("type": "Instance", "params": {"probability": 0.4, "tMin": 0.3, "scale": 0.05})";
    scaled.replace(scaled.find(snow), snow.size(), R"("type": "Scales")");
    data.graphJson = scaled;
    GroomSession scales(data);
    scales.updateScalpPoints(deformedPoints(*data.scalp, 2));
    EXPECT_FALSE(scales.evaluate(hosted).follow);
}

TEST(FollowScalp, PosesInWorldSpaceKeepTheirPlace) {
    // Every guide posed where it is on the moved scalp (a simulation that followed the head
    // exactly): the poses, taken back to rest, change nothing.
    GroomData data = followData(kLookGraph);
    data.followScalp = true;
    const Rigid motion;
    std::vector<Vec3f> points = data.scalp->points();
    for (Vec3f& p : points) p = motion(p);
    GroomSession follow(data);
    follow.updateScalpPoints(points);
    const StrandsPtr plain = follow.evaluate(1.0f, 0.0);
    auto poses = std::make_shared<GuideSet>();
    for (const HierarchyGuide& g : follow.hierarchy()->guides()) {
        if (!g.isChild()) poses->add(g.id, g.points);  // on the scalp now
    }
    follow.setGuidePoses(poses);
    EXPECT_LT(maxDistance(*follow.evaluate(1.0f, 0.0), *plain), 1e-4f);
}

TEST(FollowScalp, JiggleReactsToTheHeadsMotionAsItDoesUnfollowed) {
    // A head turning and moving rigidly over the last frames: Jiggle at rest (its history turned
    // into rest space) and then carried lags as Jiggle on the moving head does.
    const std::string graph = R"({"version": 1, "output": "output", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
        {"name": "interpolate", "type": "Interpolate"},
        {"name": "jiggle", "type": "Jiggle", "params": {"history": 6}}, {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"}, {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"}, {"from": "interpolate", "to": "jiggle", "input": "strands"},
        {"from": "jiggle", "to": "output", "input": "strands"}]})";
    GroomData data = followData(graph.c_str());
    auto at = [&](int k) {  // k frames ago
        Rigid m;
        m.angle = 0.6f - 0.08f * static_cast<float>(k);
        m.move = Vec3f(0.5f - 0.1f * static_cast<float>(k), 0.0f, 0.0f);
        std::vector<Vec3f> points = data.scalp->restPoints();
        for (Vec3f& p : points) p = m(p);
        return points;
    };
    std::vector<std::vector<Vec3f>> past;
    for (int k = 1; k <= 6; ++k) past.push_back(at(k));
    const ScalpHistoryPtr history = ScalpHistory::make(1.0 / 24.0, past);
    GroomSession normal(data);
    data.followScalp = true;
    GroomSession follow(data);
    StrandsPtr results[2];
    GroomSession* sessions[2] = {&normal, &follow};
    for (int i = 0; i < 2; ++i) {
        sessions[i]->updateScalpPoints(at(0));
        sessions[i]->setScalpHistory(history);
        results[i] = sessions[i]->evaluate(1.0f, 0.0);
    }
    ASSERT_EQ(results[0]->strandCount(), results[1]->strandCount());
    // Without the history the strands are elsewhere (Jiggle moved them); followed, they moved the same way.
    sessions[0]->setScalpHistory(nullptr);
    const float lag = maxDistance(*results[0], *sessions[0]->evaluate(1.0f, 0.0));
    EXPECT_GT(lag, 0.01f);
    EXPECT_LT(maxDistance(*results[0], *results[1]), 0.02f * lag + 1e-4f);
}

TEST(FollowScalp, ReadsTimeKnowsWhatMoves) {
    GroomData data = followData(kLookGraph);
    GroomSession session(data);
    EXPECT_FALSE(session.readsTime());
    std::string wind = kLookGraph;
    const std::string widthNode = R"({"name": "width", "type": "Width"},)";
    wind.replace(wind.find(widthNode), widthNode.size(), widthNode + R"({"name": "wind", "type": "Wind"},)");
    const std::string toOutput = R"({"from": "width", "to": "output", "input": "strands"})";
    wind.replace(wind.find(toOutput), toOutput.size(),
                 R"({"from": "width", "to": "wind", "input": "strands"}, {"from": "wind", "to": "output", "input": "strands"})");
    session.setGraphJson(wind);
    EXPECT_TRUE(session.readsTime());
    std::string still = wind;
    still.replace(still.find(R"("type": "Wind"})"), 15, R"("type": "Wind", "params": {"strength": 0.0}})");
    session.setGraphJson(still);
    EXPECT_FALSE(session.readsTime());
    session.setScalpHistory(ScalpHistory::make(1.0 / 24.0, {data.scalp->points()}));
    EXPECT_TRUE(session.readsTime());
}

TEST(FollowScalp, AStrandMeshNodeFollowsWithAFixedTopology) {
    // A StrandMesh node between Clump and Noise: the output is as without it; its strands are the
    // clumped ones at its density, thinned to maxStrands, carried with the scalp frame after frame:
    // the same strands and point counts (one topology key), other points.
    std::string graph = kLookGraph;
    const std::string noiseNode = R"({"name": "noise", "type": "Noise", "params": {"amplitude": 0.03}},)";
    graph.replace(graph.find(noiseNode), noiseNode.size(),
                  R"({"name": "tubes", "type": "ToMesh", "params": {"density": 0.5, "maxStrands": 120, "sides": 5}},)" +
                      noiseNode);
    const std::string toNoise = R"({"from": "clump", "to": "noise", "input": "strands"})";
    graph.replace(graph.find(toNoise), toNoise.size(),
                  R"({"from": "clump", "to": "tubes", "input": "strands"}, {"from": "tubes", "to": "noise", "input": "strands"})");
    // Its params go through the graph's JSON.
    const GraphDocument doc = loadGraphJson(graph);
    const GraphDocument again = loadGraphJson(saveGraphJson(doc));
    const StrandMeshSettings settings = strandMeshSettings(again.graph.node(again.node("tubes")).params());
    EXPECT_EQ(settings.options.sides, 5);
    EXPECT_EQ(settings.options.style, StrandMeshOptions::Style::Tube);
    EXPECT_FLOAT_EQ(settings.density, 0.5f);
    EXPECT_EQ(settings.maxStrands, 120u);

    GroomData data = followData(graph.c_str());
    EXPECT_EQ(GroomSession(data).evaluate(1.0f, 0.0)->contentHash(),
              GroomSession(followData(kLookGraph)).evaluate(1.0f, 0.0)->contentHash());  // handed on as they came
    data.followScalp = true;
    GroomSession session(data);
    std::uint64_t key = 0;
    StrandsPtr last;
    int lastFrame = 0;
    for (const int frame : {2, 3, 3}) {
        SCOPED_TRACE("frame " + std::to_string(frame));
        session.updateScalpPoints(deformedPoints(*data.scalp, frame));
        session.evaluate(ViewRequest{});
        const auto sources = session.evaluateStrandMeshes(frame / 24.0);
        ASSERT_EQ(sources.size(), 1u);
        EXPECT_EQ(sources[0].node, "tubes");
        EXPECT_EQ(sources[0].options.sides, 5);
        const StrandsPtr strands = sources[0].strands;
        ASSERT_TRUE(strands);
        EXPECT_EQ(strands->strandCount(), 120u);
        // The node's strands as evaluateNode gives them, thinned the same way.
        const StrandsPtr node = std::get<StrandsPtr>(session.evaluateNode("tubes", 0.5f, frame / 24.0));
        EXPECT_EQ(strands->contentHash(), node->subset(thinStrandsById(*node, 120)).contentHash());
        const std::uint64_t k = strandMeshTopologyKey(*strands, sources[0].options);
        if (last) {
            EXPECT_EQ(k, key);
            if (frame == lastFrame) {
                EXPECT_EQ(strands, last);  // the same frame again: the same strands
            } else {
                EXPECT_GT(maxDistance(*strands, *last), 1e-3f);  // carried with the scalp
            }
        }
        key = k;
        last = strands;
        lastFrame = frame;
    }
    // The view's (wantStrandMeshes): the same strands, a version that moves with them; drawn
    // instead of the strands (DisplaySettings::strandMeshView) as lit triangles.
    ViewRequest request;
    request.wantStrandMeshes = true;
    EXPECT_TRUE(session.hasStrandMeshNode());
    session.updateScalpPoints(deformedPoints(*data.scalp, 4));
    const ViewResult first = session.evaluate(request);
    ASSERT_EQ(first.strandMeshes.size(), 1u);
    EXPECT_EQ(first.strandMeshes[0].strands->strandCount(), 120u);
    EXPECT_EQ(session.evaluate(request).versions.strandMeshes, first.versions.strandMeshes);  // nothing moved
    session.updateScalpPoints(deformedPoints(*data.scalp, 5));
    const ViewResult moved = session.evaluate(request);
    EXPECT_GT(moved.versions.strandMeshes, first.versions.strandMeshes);
    EXPECT_GT(moved.versions.evaluation, first.versions.evaluation);
    DisplayBuilder builder;
    DisplaySettings shown;
    shown.strandMeshView = shown.drawStrandMesh = true;
    const DisplayFrame& frame = builder.update(DisplayInputs::fromView(moved), shown);
    const DisplayItem& mesh = frame.item(DisplayItemId::StrandMesh);
    ASSERT_TRUE(mesh.geometry);
    const auto& strands = *moved.strandMeshes[0].strands;
    EXPECT_EQ(mesh.vertexCount, strandMeshPointCount(strands, moved.strandMeshes[0].options));
    EXPECT_TRUE(mesh.geometry->triangles && mesh.geometry->scaleSurface);
    std::size_t triangles = 0;
    for (std::size_t k = 0; k < strands.strandCount(); ++k) triangles += 2 * 5 * (strands.strandPointCount(k) - 1) + 5;
    EXPECT_EQ(mesh.geometry->indices.size(), 3 * triangles);
    EXPECT_FALSE(frame.item(DisplayItemId::Strands).shown);  // the mesh instead of the strands
    // A mesh of the host's shows it (drawStrandMesh off): the strands still step aside.
    shown.drawStrandMesh = false;
    const DisplayFrame& hosted = builder.update(DisplayInputs::fromView(moved), shown);
    EXPECT_FALSE(hosted.item(DisplayItemId::StrandMesh).shown);
    EXPECT_FALSE(hosted.item(DisplayItemId::Strands).shown);
    shown.strandMeshView = false;  // off: the strands
    EXPECT_TRUE(builder.update(DisplayInputs::fromView(moved), shown).item(DisplayItemId::Strands).shown);
    request.wantStrandMeshes = false;
    EXPECT_TRUE(session.evaluate(request).strandMeshes.empty());

    // No StrandMesh node: nothing.
    session.setGraphJson(kLookGraph);
    session.evaluate(ViewRequest{});
    EXPECT_TRUE(session.evaluateStrandMeshes(0.0).empty());
}

TEST(FollowScalp, GroomFilesKeepTheFlag) {
    GroomFile file;
    file.data = followData(kLookGraph);
    EXPECT_EQ(groomFileToJson(file).find("followScalp"), std::string::npos);  // off: not written
    EXPECT_FALSE(groomFileFromJson(groomFileToJson(file)).data.followScalp);
    file.data.followScalp = true;
    const GroomFile read = groomFileFromJson(groomFileToJson(file));
    EXPECT_TRUE(read.data.followScalp);
    // A session of the file follows: a moved scalp carries the groom at rest.
    GroomSession session(read.data);
    EXPECT_TRUE(session.followScalp());
    session.updateScalpPoints(deformedPoints(*read.data.scalp, 2));
    session.evaluate(1.0f, 0.0);
    EXPECT_TRUE(session.following());
}

}  // namespace
}  // namespace groomlab
