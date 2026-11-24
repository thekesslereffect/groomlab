// SPDX-License-Identifier: Apache-2.0
// The Sculpt node (sculpt.h): brushed edits of generated guides kept as relative forms, by guide
// ID, in the groom; strokes on the node's output through the stroke system.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

#include <gtest/gtest.h>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/graph_edit.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/random.h"
#include "groomlab/sculpt.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

// Rotates p about the X axis through `pivot` by `angle` radians.
Vec3f turnAboutX(const Vec3f& p, const Vec3f& pivot, float angle) {
    const Vec3f d = p - pivot;
    const float c = std::cos(angle), s = std::sin(angle);
    return pivot + Vec3f(d.x, d.y * c - d.z * s, d.y * s + d.z * c);
}

// A guide rooted at `root` on the plane (normal +Y), `segments` segments of `length` in all,
// bent toward +Z by `bend` radians spread evenly along it (0: straight up).
Guide planeGuide(std::uint64_t id, const Vec3f& root, int segments, float length, float bend, const ScalpMesh& plane) {
    Guide g;
    g.id = id;
    g.root = plane.closestPoint(root).root;
    Vec3f p = root;
    g.points.push_back(p);
    for (int i = 0; i < segments; ++i) {
        const float a = bend * (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
        p = p + Vec3f(0.0f, std::cos(a), std::sin(a)) * (length / static_cast<float>(segments));
        g.points.push_back(p);
    }
    return g;
}

// The guide with its last `tip` segments turned `angle` about X at the point they start from: a hook.
std::vector<Vec3f> hooked(const std::vector<Vec3f>& pts, std::size_t tip, float angle) {
    std::vector<Vec3f> out = pts;
    const std::size_t from = pts.size() - 1 - tip;
    for (std::size_t k = from + 1; k < pts.size(); ++k) out[k] = turnAboutX(pts[k], pts[from], angle);
    return out;
}

float angleBetween(const Vec3f& a, const Vec3f& b) {
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f));
}

float maxDistance(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float d = 0.0f;
    for (std::size_t k = 0; k < std::min(a.size(), b.size()); ++k) d = std::max(d, (a[k] - b[k]).length());
    return d;
}

TEST(SculptNode, ARecordedEditGivesTheSculptedShapeBack) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(7, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.8f, *plane);
    // A hook near the tip, a comb at the root (the whole guide turned about the root), longer.
    std::vector<Vec3f> sculpted = hooked(g.points, 4, 0.6f);
    for (Vec3f& p : sculpted) p = turnAboutX(p, g.points[0], -0.3f);
    for (std::size_t k = 1; k < sculpted.size(); ++k) sculpted[k] = sculpted[0] + (sculpted[k] - sculpted[0]) * 1.2f;
    for (const float strength : {1.0f, 0.5f, 1.7f}) {
        const GuideSculpt edit = recordSculpt(g, sculpted, strength, plane.get());
        ASSERT_EQ(edit.samples.size(), g.points.size() - 1);
        EXPECT_LT(maxDistance(applySculpt(g, edit, strength, plane.get()), sculpted), 1e-5f) << strength;
        // Strength 0: the input, to the bit.
        EXPECT_EQ(applySculpt(g, edit, 0.0f, plane.get()), g.points);
    }
    EXPECT_THROW(recordSculpt(g, sculpted, 0.0f, plane.get()), std::invalid_argument);
    // An edit of nothing is none.
    EXPECT_TRUE(recordSculpt(g, g.points, 1.0f, plane.get()).identity());
}

TEST(SculptNode, AHookKeepsItsRelativeFormWhenTheInputChanges) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide straight = planeGuide(3, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.0f, *plane);
    const float hook = 30.0f * kPi / 180.0f;
    const GuideSculpt edit = recordSculpt(straight, hooked(straight.points, 4, hook), 1.0f, plane.get());
    // The input made again upstream: bent, half as long again, with another point count.
    for (const int segments : {15, 22, 9}) {
        const Guide bent = planeGuide(3, {1.0f, 0.0f, 1.0f}, segments, 1.5f, 1.1f, *plane);
        const std::vector<Vec3f> out = applySculpt(bent, edit, 1.0f, plane.get());
        ASSERT_EQ(out.size(), bent.points.size());
        float total = 0.0f, along = 0.0f;
        for (std::size_t k = 0; k + 1 < bent.points.size(); ++k) total += (bent.points[k + 1] - bent.points[k]).length();
        for (std::size_t k = 0; k + 1 < bent.points.size(); ++k) {
            const Vec3f in = bent.points[k + 1] - bent.points[k];
            const Vec3f mine = out[k + 1] - out[k];
            const float t = (along + 0.5f * in.length()) / total;
            along += in.length();
            // Lengths kept; the bend the input has, plus the hook past 11/15 of the length.
            EXPECT_NEAR(mine.length(), in.length(), 1e-5f);
            EXPECT_NEAR(out[k + 1].x, 1.0f, 1e-5f) << "stays in the bend's plane";
            if (t > 12.0f / 15.0f) EXPECT_NEAR(angleBetween(mine, in), hook, 2e-3f) << segments << " " << t;
            if (t < 10.0f / 15.0f) EXPECT_NEAR(angleBetween(mine, in), 0.0f, 2e-3f) << segments << " " << t;
        }
    }
    // Turned rigidly with its root (a scalp turned with it): the result turns the same way.
    const Guide bent = planeGuide(3, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.5f, *plane);
    std::vector<Vec3f> turnedPoints;
    for (const Vec3f& p : plane->points()) turnedPoints.push_back(turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f));
    const auto turnedPlane = std::make_shared<const ScalpMesh>(turnedPoints, plane->triangles());
    Guide turned = bent;
    for (Vec3f& p : turned.points) p = turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f);
    std::vector<Vec3f> expected = applySculpt(bent, edit, 1.0f, plane.get());
    for (Vec3f& p : expected) p = turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f);
    EXPECT_LT(maxDistance(applySculpt(turned, edit, 1.0f, turnedPlane.get()), expected), 1e-5f);
}

TEST(SculptNode, EditsFollowIdsAndWaitForMissingOnes) {
    const auto plane = test::makePlane(8, 2.0f);
    auto set = std::make_shared<GuideSet>();
    for (std::uint64_t id = 1; id <= 5; ++id) {
        set->guides().push_back(planeGuide(id * 1000003u, {0.2f + 0.3f * static_cast<float>(id), 0.0f, 1.0f}, 12, 0.8f, 0.3f, *plane));
    }
    SculptEdits edits;
    const Guide& second = set->guides()[1];
    edits.guides[second.id] = recordSculpt(second, hooked(second.points, 3, 0.5f), 1.0f, plane.get());
    edits.guides[999] = edits.guides[second.id];  // an ID the input does not have: dormant
    const GuidesPtr out = applySculpts(set, edits, 1.0f, plane.get());
    // Shuffled: each guide gets what its ID gets.
    auto shuffled = std::make_shared<GuideSet>(*set);
    std::mt19937 rng(4);
    std::shuffle(shuffled->guides().begin(), shuffled->guides().end(), rng);
    const GuidesPtr outShuffled = applySculpts(shuffled, edits, 1.0f, plane.get());
    for (const Guide& g : outShuffled->guides()) {
        const auto it = std::find_if(out->guides().begin(), out->guides().end(), [&](const Guide& o) { return o.id == g.id; });
        ASSERT_NE(it, out->guides().end());
        EXPECT_EQ(it->points, g.points);
        const auto in = std::find_if(set->guides().begin(), set->guides().end(), [&](const Guide& o) { return o.id == g.id; });
        if (g.id == second.id) {
            EXPECT_GT(maxDistance(g.points, in->points), 0.05f);
        } else {
            EXPECT_EQ(g.points, in->points);  // untouched guides as they came
        }
    }
    // The missing ID comes back: its edit applies again.
    Guide back = planeGuide(999, {1.5f, 0.0f, 0.5f}, 12, 0.8f, 0.3f, *plane);
    auto withBack = std::make_shared<GuideSet>(*set);
    withBack->guides().push_back(back);
    const GuidesPtr outBack = applySculpts(withBack, edits, 1.0f, plane.get());
    EXPECT_GT(maxDistance(outBack->guides().back().points, back.points), 0.05f);
    // No edit reaches a set: the set itself.
    SculptEdits elsewhere;
    elsewhere.guides[999] = edits.guides[second.id];
    EXPECT_EQ(applySculpts(set, elsewhere, 1.0f, plane.get()), set);
}

TEST(SculptNode, AStrokesRecordKeptDabAfterDabIsTheWholeRecord) {
    // A live stroke records only the guides its dabs changed (SculptStrokeRecord): after every dab,
    // the edits recordSculptStroke gives from the stored ones, to the bit.
    const auto plane = test::makePlane(8, 2.0f);
    auto set = std::make_shared<GuideSet>();
    for (std::uint64_t k = 0; k < 40; ++k) {
        const Vec3f root{0.15f + 0.21f * static_cast<float>(k % 8), 0.0f, 0.3f + 0.3f * static_cast<float>(k / 8)};
        set->guides().push_back(planeGuide(1000 + k * 7919u, root, 10, 0.6f, 0.2f, *plane));
    }
    SculptEdits stored;
    for (const std::size_t k : {std::size_t{3}, std::size_t{11}, std::size_t{12}, std::size_t{30}}) {
        const Guide& g = set->guides()[k];
        stored.guides[g.id] = recordSculpt(g, hooked(g.points, 3, 0.4f), 1.0f, plane.get());
    }
    stored.guides[5] = stored.guides.begin()->second;  // an ID the input does not have: kept as it is
    for (const float strength : {1.0f, 0.6f}) {
        const std::shared_ptr<GuideHierarchy> working =
            sculptHierarchy(*applySculpts(set, stored, strength, plane.get()), "sculpt1", plane.get());
        const GuideHierarchy start = *working;
        BrushSettings brush;
        brush.mode = BrushMode::Comb;
        brush.radius = 0.35f;
        SculptStrokeRecord record;
        SculptEdits edits;
        std::size_t recordedGuides = 0;
        const auto check = [&](const char* when) {
            record.record(edits, stored, *set, *working, strength, plane.get());
            SculptEdits whole = stored;
            recordedGuides = recordSculptStroke(whole, *set, *working, strength, plane.get()).size();
            EXPECT_EQ(edits, whole) << when;
            EXPECT_EQ(edits.contentHash(), whole.contentHash()) << when;
        };
        check("before any dab");
        for (int dab = 0; dab < 6; ++dab) {
            const Vec3f center{0.3f + 0.25f * static_cast<float>(dab), 0.3f, 0.5f + 0.15f * static_cast<float>(dab)};
            applyBrushStamp(*working, plane.get(), brush, center, {0.05f, 0.0f, 0.03f});
            check("after a dab");
            check("with nothing changed since");
        }
        EXPECT_GT(recordedGuides, 0u);
        EXPECT_LT(recordedGuides, set->guides().size());  // the brush reached some guides only
        EXPECT_FALSE(edits == stored);
        // A guide with a stored edit and one without put back as they were: their stored edits again.
        for (const std::size_t k : {std::size_t{3}, std::size_t{4}}) {
            const GuideId id = set->guides()[k].id;
            working->pose(id, start.guide(id)->points);
            check("after a guide went back");
        }
        // A guide gone from the stroke's guides keeps its stored edit; another length is left alone.
        working->removeGuides({set->guides()[11].id});
        check("after a guide went");
        std::vector<Vec3f> shorter = working->guide(set->guides()[12].id)->points;
        shorter.pop_back();
        working->pose(set->guides()[12].id, std::move(shorter));
        check("after a guide lost a point");
        // Cleared: recorded whole again.
        record.clear();
        check("after clear");
    }
}

TEST(SculptNode, BrushedShapesRideOnTheInputWhenItIsPosedMovedOrBent) {
    // A stretch of a guide brushed aside (its middle pushed out of the guide's way, the tip back in
    // line), then the guide fed again otherwise upstream: a simulation's pose swinging it about its
    // root, the groom moved, a primary form reshaped (bent). The push rides on the input: posed or
    // moved, the brushed shape moves with it; bent, the push stays the same push off the guide in
    // its frame (out of the bend's plane to the bit, in it to within a quarter of the push).
    const auto plane = test::makePlane(8, 2.0f);
    const Vec3f root{1.0f, 0.0f, 1.0f};
    const Guide straight = planeGuide(5, root, 15, 1.0f, 0.0f, *plane);
    const std::size_t n = straight.points.size();
    const auto bump = [&](std::size_t k) {
        const float u = static_cast<float>(k) / static_cast<float>(n - 1);
        const float s = u < 0.3f || u > 0.7f ? 0.0f : std::sin(kPi * (u - 0.3f) / 0.4f);
        return 0.05f * s * s;
    };
    for (const Vec3f& across : {Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 1.0f}}) {  // (out of the bend's plane, in it)
        std::vector<Vec3f> brushed = straight.points;
        for (std::size_t k = 0; k < n; ++k) brushed[k] += across * bump(k);
        const GuideSculpt edit = recordSculpt(straight, brushed, 1.0f, plane.get());
        ASSERT_LT(maxDistance(applySculpt(straight, edit, 1.0f, plane.get()), brushed), 1e-5f);
        // Posed: swung about its root (a simulation's frame), the scalp as it was: swung the same.
        for (const Vec3f& axis : {Vec3f{0.0f, 0.0f, 1.0f}, Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.6f, 0.0f, 0.8f}}) {
            Guide posed = straight;
            std::vector<Vec3f> expected = brushed;
            for (Vec3f& p : posed.points) p = root + rotateAxisAngle(p - root, axis, 0.6f);
            for (Vec3f& p : expected) p = root + rotateAxisAngle(p - root, axis, 0.6f);
            EXPECT_LT(maxDistance(applySculpt(posed, edit, 1.0f, plane.get()), expected), 1e-5f);
        }
        // Moved with the groom (no scalp connected: the edit in world axes, moved along).
        const GuideSculpt free = recordSculpt(straight, brushed, 1.0f, nullptr);
        Guide moved = straight;
        std::vector<Vec3f> expected = brushed;
        for (Vec3f& p : moved.points) p += Vec3f{0.3f, -0.2f, 0.5f};
        for (Vec3f& p : expected) p += Vec3f{0.3f, -0.2f, 0.5f};
        EXPECT_LT(maxDistance(applySculpt(moved, free, 1.0f, nullptr), expected), 1e-5f);
        // Bent upstream (toward +Z, a radian over its length): the bent guide pushed the same way off
        // it, in its frame there (out of the plane: +X still; in it: square to the bent guide).
        const Guide bent = planeGuide(5, root, 15, 1.0f, 1.0f, *plane);
        const std::vector<Vec3f> out = applySculpt(bent, edit, 1.0f, plane.get());
        float worst = 0.0f;
        for (std::size_t k = 0; k < n; ++k) {
            const Vec3f tangent = centralTangent(bent.points, k);
            const Vec3f off = across.x > 0.5f ? across : Vec3f{1.0f, 0.0f, 0.0f}.cross(tangent).normalized();
            worst = std::max(worst, (out[k] - (bent.points[k] + off * bump(k))).length());
        }
        // (In the plane the push's turns ride on a turning guide: kept as bends, not as offsets, so
        // over a radian of bend under the push it lands within a quarter of it, about a fifth.)
        EXPECT_LT(worst, across.x > 0.5f ? 1e-5f : 0.25f * 0.05f) << "push along " << across.x << " " << across.z;
    }
}

TEST(SculptNode, ScaleInvariantAndDeterministic) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(11, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.6f, *plane);
    const GuideSculpt edit = recordSculpt(g, hooked(g.points, 5, 0.7f), 1.0f, plane.get());
    // The same at 100 times the size: the scene and the guide scaled, the edit as it was.
    std::vector<Vec3f> big;
    for (const Vec3f& p : plane->points()) big.push_back(p * 100.0f);
    const auto bigPlane = std::make_shared<const ScalpMesh>(big, plane->triangles());
    Guide bigGuide = g;
    for (Vec3f& p : bigGuide.points) p = p * 100.0f;
    const std::vector<Vec3f> small = applySculpt(g, edit, 1.0f, plane.get());
    const std::vector<Vec3f> large = applySculpt(bigGuide, edit, 1.0f, bigPlane.get());
    for (std::size_t k = 0; k < small.size(); ++k) EXPECT_LT((large[k] * 0.01f - small[k]).length(), 1e-5f);
    // Recorded at that size: the same edit.
    const GuideSculpt bigEdit = recordSculpt(bigGuide, hooked(bigGuide.points, 5, 0.7f), 1.0f, bigPlane.get());
    ASSERT_EQ(bigEdit.samples.size(), edit.samples.size());
    for (std::size_t k = 0; k < edit.samples.size(); ++k) {
        EXPECT_NEAR(bigEdit.samples[k].ax, edit.samples[k].ax, 1e-5f);
        EXPECT_NEAR(bigEdit.samples[k].ay, edit.samples[k].ay, 1e-5f);
        EXPECT_NEAR(bigEdit.samples[k].scale, edit.samples[k].scale, 1e-5f);
    }
    EXPECT_EQ(applySculpt(g, edit, 1.0f, plane.get()), small);  // to the bit, every time
}

TEST(SculptNode, TextsRoundTripRenameCopyAndClear) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(42, {1.0f, 0.0f, 1.0f}, 10, 1.0f, 0.4f, *plane);
    SculptSet set;
    set.nodes["sculpt1"].guides[std::uint64_t{18446744073709551557ull}] = recordSculpt(g, hooked(g.points, 3, 0.4f), 1.0f, plane.get());
    const std::string text = set.toJson();
    EXPECT_EQ(SculptSet::fromJson(text).nodes, set.nodes);  // floats exactly, 64-bit IDs whole
    EXPECT_TRUE(SculptSet::fromJson("").empty());
    EXPECT_THROW(SculptSet::fromJson("{nope"), std::invalid_argument);
    const std::string renamed = renameSculptNode(text, "sculpt1", "shape");
    EXPECT_EQ(sculptedNodes(renamed), std::vector<std::string>{"shape"});
    EXPECT_EQ(SculptSet::fromJson(renamed).nodes.at("shape"), set.nodes.at("sculpt1"));
    EXPECT_EQ(renameSculptNode(text, "other", "x"), text);
    const std::string copied = copySculptNode(text, "sculpt1", "sculpt2");
    EXPECT_EQ(sculptedNodes(copied), (std::vector<std::string>{"sculpt1", "sculpt2"}));
    EXPECT_EQ(sculptedNodes(clearSculptNode(copied, "sculpt1")), std::vector<std::string>{"sculpt2"});

    // A compound renamed or copied takes its inner nodes' edits along ("compound1/sculpt1"), not
    // those of a node whose name only starts the same ("compound10/...").
    SculptSet inner;
    inner.nodes["compound1/sculpt1"] = set.nodes.at("sculpt1");
    inner.nodes["compound1/deep/sculpt1"] = set.nodes.at("sculpt1");
    inner.nodes["compound10/sculpt1"] = set.nodes.at("sculpt1");
    const std::string grouped = inner.toJson();
    EXPECT_EQ(sculptedNodes(renameSculptNode(grouped, "compound1", "shaping")),
              (std::vector<std::string>{"compound10/sculpt1", "shaping/deep/sculpt1", "shaping/sculpt1"}));
    EXPECT_EQ(sculptedNodes(copySculptNode(grouped, "compound1/deep", "deep")),
              (std::vector<std::string>{"compound1/deep/sculpt1", "compound1/sculpt1", "compound10/sculpt1", "deep/sculpt1"}));
    // Moves in order: grouped, then the compound renamed.
    EXPECT_EQ(sculptedNodes(moveSculptNodes(text, {{"sculpt1", "compound1/sculpt1"}, {"compound1", "shaping"}})),
              std::vector<std::string>{"shaping/sculpt1"});
    EXPECT_EQ(moveSculptNodes(text, {{"other", "x"}}), text);
}

// ---- Through the graph and a session ------------------------------------------------------------

// Sparse hand guides -> an even Scatter + Interpolate -> ToGuides -> Sculpt -> Interpolate (the
// render's strands), on a unit sphere. `sculpt`: the Sculpt node's name.
std::string workflowGraph(const std::string& sculpt = "sculpt1", double strength = 1.0) {
    return R"({"output": "output", "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "sparse", "type": "Guides"},
      {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
      {"name": "dense", "type": "Interpolate"},
      {"name": "toGuides", "type": "ToGuides"},
      {"name": ")" + sculpt + R"(", "type": "Sculpt", "params": {"strength": )" + std::to_string(strength) + R"(}},
      {"name": "roots", "type": "Scatter", "params": {"density": 60.0}},
      {"name": "hair", "type": "Interpolate"},
      {"name": "output", "type": "Output"}],
     "connections": [
      {"from": "scalp", "to": "even", "input": "scalp"},
      {"from": "even", "to": "dense", "input": "strands"},
      {"from": "sparse", "to": "dense", "input": "guides"},
      {"from": "scalp", "to": "dense", "input": "scalp"},
      {"from": "dense", "to": "toGuides", "input": "strands"},
      {"from": "toGuides", "to": ")" + sculpt + R"(", "input": "guides"},
      {"from": "scalp", "to": ")" + sculpt + R"(", "input": "scalp"},
      {"from": "scalp", "to": "roots", "input": "scalp"},
      {"from": "roots", "to": "hair", "input": "strands"},
      {"from": ")" + sculpt + R"(", "to": "hair", "input": "guides"},
      {"from": "scalp", "to": "hair", "input": "scalp"},
      {"from": "hair", "to": "output", "input": "strands"}]})";
}

// A few hand guides over the upper half of the unit sphere, leaning by `lean` toward +Z.
std::string sparseGuides(const ScalpMesh& scalp, float lean) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (int i = 0; i < 12; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 12.0f;
        const Vec3f n = Vec3f(0.6f * std::cos(a), 0.75f, 0.6f * std::sin(a)).normalized();
        const Vec3f dir = (n + Vec3f(0.0f, 0.0f, lean)).normalized();
        h.addGuide(g, test::straightCurve(n, dir * 0.6f, 10));
    }
    h.evaluate(&scalp);
    return h.toJson();
}

struct Workflow {
    MeshPtr scalp = test::makeSphere(24, 48);
    std::unique_ptr<GroomSession> session;
    std::unique_ptr<SessionStrokeHost> host;
    explicit Workflow(const std::string& graph = workflowGraph()) {
        GroomData data;
        data.scalp = scalp;
        data.graphJson = graph;
        data.hierarchyJson = sparseGuides(*scalp, 0.0f);
        session = std::make_unique<GroomSession>(std::move(data));
        host = std::make_unique<SessionStrokeHost>(*session);
        ViewRequest request;
        EXPECT_TRUE(session->evaluate(request).strands);
    }
    GuidesPtr output(const std::string& node = "sculpt1") { return valueAs<GuidesPtr>(session->evaluateNode(node, 1.0f, 0.0)); }
    GuidesPtr input(const std::string& node = "sculpt1") { return session->sculptInput(node).guides; }
};

const Guide* byId(const GuideSet& set, std::uint64_t id) {
    for (const Guide& g : set.guides()) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

// Per segment: the angle between the output's and the input's segment, and their length ratio.
std::vector<std::pair<float, float>> relativeForm(const Guide& in, const Guide& out) {
    std::vector<std::pair<float, float>> form;
    for (std::size_t k = 0; k + 1 < in.points.size(); ++k) {
        const Vec3f a = in.points[k + 1] - in.points[k], b = out.points[k + 1] - out.points[k];
        form.emplace_back(angleBetween(a, b), b.length() / a.length());
    }
    return form;
}

TEST(SculptNode, WithoutEditsTheNodeIsTheIdentity) {
    Workflow w;
    EXPECT_EQ(w.output(), w.input());  // the very guides it is fed
    // Edits at strength 0: the guides as fed too.
    Workflow zero(workflowGraph("sculpt1", 0.0));
    const GuidesPtr in = zero.input();
    SculptSet set;
    set.nodes["sculpt1"].guides[in->guides()[0].id] =
        recordSculpt(in->guides()[0], hooked(in->guides()[0].points, 3, 0.5f), 1.0f, zero.scalp.get());
    zero.session->setSculptsJson(set.toJson());
    EXPECT_EQ(zero.output(), zero.input());
}

TEST(SculptNode, StrokesShapeTheNodesGuidesAndKeepTheirFormUpstream) {
    Workflow w;
    w.session->setSculptTarget("sculpt1");
    const GuidesPtr fed = w.input();
    ASSERT_GE(fed->size(), 20u);
    // The view shows the node's guides as the editable ones.
    const ViewResult& shown = w.session->evaluate(ViewRequest{});
    ASSERT_TRUE(shown.hierarchy);
    EXPECT_EQ(shown.hierarchy->guides().size(), fed->size());
    EXPECT_NE(shown.hierarchy->group(shown.hierarchy->guides()[0].group)->name.find("sculpt1"), std::string::npos);

    // A comb across the top, then a curl: two undo steps, each the node's edits.
    BrushToolSettings s;
    s.setMode("comb");
    s.brush.radius = 0.5f;
    s.brush.strength = 1.0f;
    EXPECT_EQ(scriptedStroke(*w.host, s, {{-0.4f, 1.3f, 0.0f}, {0.0f, 1.35f, 0.2f}, {0.4f, 1.3f, 0.4f}}), 0u);
    ASSERT_EQ(w.host->edits().size(), 1u);
    EXPECT_TRUE(w.host->edits()[0].text(StrokeField::Sculpts).set);
    EXPECT_FALSE(w.host->edits()[0].text(StrokeField::Guides).set);  // the stored guides untouched
    const SculptSet afterComb = SculptSet::fromJson(w.session->data().sculptsJson);
    ASSERT_TRUE(afterComb.node("sculpt1"));
    const std::size_t combed = afterComb.node("sculpt1")->guides.size();
    EXPECT_GT(combed, 0u);
    EXPECT_LT(combed, fed->size());  // only the guides it touched
    s.setMode("twist");
    s.brush.strength = 1.0f;
    scriptedStroke(*w.host, s, {{0.3f, 1.2f, 0.3f}, {0.35f, 1.2f, 0.4f}});
    ASSERT_EQ(w.host->edits().size(), 2u);

    // The output carries the edits; the hair downstream follows.
    const GuidesPtr out = w.output();
    std::uint64_t edited = 0;
    float most = 0.0f;
    for (const Guide& g : out->guides()) {
        const float d = maxDistance(g.points, byId(*fed, g.id)->points);
        if (d > most) {
            most = d;
            edited = g.id;
        }
    }
    ASSERT_GT(most, 0.05f);
    const auto formBefore = relativeForm(*byId(*fed, edited), *byId(*out, edited));

    // Undo and redo, stroke by stroke.
    ASSERT_TRUE(w.host->undo());
    ASSERT_TRUE(w.host->undo());
    EXPECT_EQ(w.output(), w.input());
    ASSERT_TRUE(w.host->redo());
    ASSERT_TRUE(w.host->redo());
    EXPECT_EQ(byId(*w.output(), edited)->points, byId(*out, edited)->points);

    // The sparse guides combed upstream: the dense guides are made again with the same IDs, and the
    // sculpted guide keeps its relative form (the same turns at the same places, the same lengths).
    w.session->setHierarchyJson(sparseGuides(*w.scalp, 0.8f));
    ASSERT_TRUE(w.session->evaluate(ViewRequest{}).strands);
    const GuidesPtr fedAgain = w.input();
    const GuidesPtr outAgain = w.output();
    const Guide* in2 = byId(*fedAgain, edited);
    ASSERT_TRUE(in2);
    EXPECT_GT(maxDistance(in2->points, byId(*fed, edited)->points), 0.05f);  // the input did move
    const auto formAfter = relativeForm(*in2, *byId(*outAgain, edited));
    ASSERT_EQ(formAfter.size(), formBefore.size());
    for (std::size_t k = 0; k < formBefore.size(); ++k) {
        EXPECT_NEAR(formAfter[k].first, formBefore[k].first, 2e-3f) << k;
        EXPECT_NEAR(formAfter[k].second, formBefore[k].second, 1e-4f) << k;
    }
    // ... and it did not snap back to where it was.
    EXPECT_GT(maxDistance(byId(*outAgain, edited)->points, byId(*out, edited)->points), 0.05f);
}

TEST(SculptNode, TheInteractiveBrushPreviewsAndCommitsTheNodesEdits) {
    Workflow w;
    w.session->setSculptTarget("sculpt1");
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0, 3.0f, 2.5f}, {0, 1.0f, 0}, 40.0, 1.0), 800, 800);
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 150.0f;
    tool.settings.brush.level = 1;  // whatever level the brush is at, the node's guides are brushed
    tool.settings.brush.stretch = true;  // (Comb's Stretch on: below, the node's guides keep their point counts)
    // Hover shows the node's guides.
    float cx = 0, cy = 0;
    view.project({0.0f, 1.3f, 0.3f}, cx, cy);
    EXPECT_FALSE(tool.affectedLines(*w.host, view, cx, cy).empty());
    bool previewed = false;
    std::vector<std::array<float, 2>> line;
    for (int k = 0; k < 12; ++k) line.push_back({cx - 120.0f + 20.0f * static_cast<float>(k), cy});
    ASSERT_TRUE(tool.stroke(*w.host, view, line, 1.0f, 0, [&] {
        previewed = previewed || w.session->hasPreviewSculpt();
        const ViewResult& r = w.session->evaluate(ViewRequest{});
        EXPECT_TRUE(r.strands);
        EXPECT_EQ(r.hierarchy.get(), w.session->sculptHierarchy().get());  // the stroke's guides drawn
    }));
    EXPECT_TRUE(previewed);
    EXPECT_FALSE(w.session->hasPreviewSculpt());
    EXPECT_FALSE(w.session->hasPreview());
    ASSERT_EQ(w.host->edits().size(), 1u);
    EXPECT_TRUE(w.host->edits()[0].text(StrokeField::Sculpts).set);
    EXPECT_NE(w.output(), w.input());
    // Comb's Stretch adds no points here: the node's edits move points, each guide keeping its count.
    const GuidesPtr in = w.input(), out = w.output();
    ASSERT_EQ(in->guides().size(), out->guides().size());
    for (std::size_t i = 0; i < in->guides().size(); ++i) {
        EXPECT_EQ(out->guides()[i].points.size(), in->guides()[i].points.size()) << i;
    }
    // Brushes a Sculpt node does not take are refused, saying why.
    tool.settings.setMode("erase");
    EXPECT_FALSE(tool.stroke(*w.host, view, line));
    EXPECT_FALSE(w.host->warnings().empty());
}

TEST(SculptNode, GroomFilesKeepTheEditsAndRenamesCarryThem) {
    Workflow w;
    w.session->setSculptTarget("sculpt1");
    BrushToolSettings s;
    s.setMode("comb");
    s.brush.radius = 0.5f;
    scriptedStroke(*w.host, s, {{-0.4f, 1.3f, 0.0f}, {0.4f, 1.3f, 0.4f}});
    const GuidesPtr out = w.output();
    const std::string sculpts = w.session->data().sculptsJson;
    ASSERT_FALSE(SculptSet::fromJson(sculpts).empty());

    // Through a groom file.
    const auto path = std::filesystem::temp_directory_path() / "groomlab_sculpt_node.groom";
    GroomFile file;
    file.data = w.session->data();
    writeGroomFile(path.string(), file);
    const GroomFile read = readGroomFile(path.string());
    std::filesystem::remove(path);
    EXPECT_EQ(SculptSet::fromJson(read.data.sculptsJson).nodes, SculptSet::fromJson(sculpts).nodes);
    GroomData data = read.data;
    data.scalp = w.scalp;
    GroomSession again(std::move(data));
    again.evaluate(ViewRequest{});
    const GuidesPtr outAgain = valueAs<GuidesPtr>(again.evaluateNode("sculpt1", 1.0f, 0.0));
    ASSERT_EQ(outAgain->size(), out->size());
    for (std::size_t k = 0; k < out->size(); ++k) EXPECT_EQ(outAgain->guides()[k].points, out->guides()[k].points);

    // The node renamed with its edits: the same guides.
    w.session->setGraphJson(workflowGraph("shape"));
    w.session->setSculptsJson(renameSculptNode(sculpts, "sculpt1", "shape"));
    w.session->evaluate(ViewRequest{});
    const GuidesPtr renamed = w.output("shape");
    ASSERT_EQ(renamed->size(), out->size());
    for (std::size_t k = 0; k < out->size(); ++k) EXPECT_EQ(renamed->guides()[k].points, out->guides()[k].points);
    // Renamed without them: the guides as fed.
    w.session->setSculptsJson(sculpts);
    EXPECT_EQ(w.output("shape"), w.input("shape"));
    // A bad text is said, and no edits apply.
    w.session->setSculptsJson("{nope");
    ASSERT_FALSE(w.session->inputErrors().empty());
    EXPECT_NE(w.session->inputErrors().back().find("sculptData"), std::string::npos);
}

TEST(SculptNode, GroupingUngroupingAndRenamingCarryTheEdits) {
    // Group puts the node inside a compound: its path becomes "compound/sculpt1", and the edits,
    // kept by node path outside the graph, move with it (GraphEdit::movedNodes), so the guides
    // stay sculpted; Ungroup moves them back out, a compound renamed takes them along.
    Workflow w;
    w.session->setSculptTarget("sculpt1");
    BrushToolSettings s;
    s.setMode("comb");
    s.brush.radius = 0.5f;
    scriptedStroke(*w.host, s, {{-0.4f, 1.3f, 0.0f}, {0.4f, 1.3f, 0.4f}});
    const GuidesPtr out = w.output();
    ASSERT_NE(out, w.input());
    const std::string sculpts = w.session->data().sculptsJson;
    auto samePoints = [&](const GuidesPtr& a) {
        if (!a || a->size() != out->size()) return false;
        for (std::size_t k = 0; k < out->size(); ++k) {
            if (a->guides()[k].points != out->guides()[k].points) return false;
        }
        return true;
    };

    GraphEdit group(w.session->data().graphJson);
    const std::string compound = group.collapse({"toGuides", "sculpt1"});
    EXPECT_EQ(group.movedNodes(), (std::vector<std::pair<std::string, std::string>>{
                                      {"toGuides", compound + "/toGuides"}, {"sculpt1", compound + "/sculpt1"}}));
    const std::string grouped = moveSculptNodes(sculpts, group.movedNodes());
    EXPECT_EQ(sculptedNodes(grouped), std::vector<std::string>{compound + "/sculpt1"});
    w.session->setSculptTarget("");
    w.session->setGraphJson(group.toJson());
    w.session->setSculptsJson(grouped);
    w.session->evaluate(ViewRequest{});
    EXPECT_TRUE(samePoints(w.output(compound))) << "grouped, the guides keep their sculpt";
    // Left under the old name (what grouping did before): the guides as fed.
    w.session->setSculptsJson(sculpts);
    EXPECT_FALSE(samePoints(w.output(compound)));

    // The compound renamed: its inner node's edits go along.
    GraphEdit rename(group.toJson());
    rename.renameNode(compound, "shaping");
    const std::string renamed = moveSculptNodes(grouped, rename.movedNodes());
    EXPECT_EQ(sculptedNodes(renamed), std::vector<std::string>{"shaping/sculpt1"});
    w.session->setGraphJson(rename.toJson());
    w.session->setSculptsJson(renamed);
    EXPECT_TRUE(samePoints(w.output("shaping")));

    // Grouped again with another node: one compound inside another ("compound/shaping/sculpt1").
    GraphEdit nest(rename.toJson());
    const std::string outer = nest.collapse({"dense", "shaping"});
    const std::string nested = moveSculptNodes(renamed, nest.movedNodes());
    EXPECT_EQ(sculptedNodes(nested), std::vector<std::string>{outer + "/shaping/sculpt1"});
    w.session->setGraphJson(nest.toJson());
    w.session->setSculptsJson(nested);
    EXPECT_TRUE(samePoints(w.output(outer)));

    // Ungrouped, both levels: back at the top as "sculpt1", the same guides.
    GraphEdit ungroup(nest.toJson());
    ungroup.expand(outer);
    ungroup.expand("shaping");
    const std::string back = moveSculptNodes(nested, ungroup.movedNodes());
    EXPECT_EQ(sculptedNodes(back), std::vector<std::string>{"sculpt1"});
    w.session->setGraphJson(ungroup.toJson());
    w.session->setSculptsJson(back);
    w.session->evaluate(ViewRequest{});
    EXPECT_TRUE(samePoints(w.output("sculpt1")));
    EXPECT_EQ(SculptSet::fromJson(back).nodes, SculptSet::fromJson(sculpts).nodes);

    // Ungrouped where the name is taken: the node comes out renamed, its edits under the new name.
    GraphEdit taken(group.toJson());
    taken.addNode("Sculpt", "sculpt1");
    const std::vector<std::string> made = taken.expand(compound);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_NE(made[1], "sculpt1");
    EXPECT_EQ(sculptedNodes(moveSculptNodes(grouped, taken.movedNodes())), std::vector<std::string>{made[1]});
}

TEST(SculptNode, TheEditableGuidesDrawInTheirToGuidesNodesColor) {
    // ToGuides tags the guides it makes with its color; a Sculpt node's copy keeps it, and the
    // editable guides (sculptHierarchy) draw in it, not the hand guides' blue.
    GuideSet guides;
    guides.add(7, {{0, 1, 0}, {0, 2, 0}});
    EXPECT_FALSE(guides.hasDisplayColor());
    EXPECT_FALSE(sculptHierarchy(guides, "sculpt1", nullptr)->groups().front().hasColor);
    guides.setDisplayColor({0.9f, 0.2f, 0.1f});
    const std::uint64_t hash = guides.contentHash();
    const auto copied = std::make_shared<GuideSet>(guides);
    EXPECT_TRUE(copied->hasDisplayColor());
    const auto shown = sculptHierarchy(*copied, "sculpt1", nullptr);
    ASSERT_TRUE(shown->groups().front().hasColor);
    EXPECT_FLOAT_EQ(shown->groups().front().color[0], 0.9f);
    EXPECT_FLOAT_EQ(shown->groups().front().color[2], 0.1f);
    GuideSet plain;
    plain.add(7, {{0, 1, 0}, {0, 2, 0}});
    EXPECT_EQ(plain.contentHash(), hash);  // display only
}

}  // namespace
}  // namespace groomlab
