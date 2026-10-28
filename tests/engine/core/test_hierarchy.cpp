// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/random.h"

#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

void expectOrthonormal(const Frame& f) {
    EXPECT_NEAR(f.tangent.length(), 1.0f, 1e-4f);
    EXPECT_NEAR(f.normal.length(), 1.0f, 1e-4f);
    EXPECT_NEAR(f.binormal.length(), 1.0f, 1e-4f);
    EXPECT_NEAR(f.tangent.dot(f.normal), 0.0f, 1e-4f);
    EXPECT_NEAR(f.tangent.dot(f.binormal), 0.0f, 1e-4f);
}

// A guide rising from `root` and bending toward +X.
std::vector<Vec3f> bentGuide(Vec3f root, int segments = 12) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments;
        pts.push_back(root + Vec3f(0.6f * t * t, t, 0.0f));
    }
    return pts;
}

// Two primary guides on the unit plane, evaluated and rooted.
struct TwoGuides {
    std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    GroupId primary = kNoGroup;

    TwoGuides() {
        primary = h.addGroup("primary");
        TubeProfile tube;
        tube.width = {0.1f, 0.2f};
        h.addGuide(primary, bentGuide({0.3f, 0, 0.3f}), tube);
        h.addGuide(primary, bentGuide({0.7f, 0, 0.7f}), tube);
        h.evaluate(scalp.get());
    }
};

// ---------------------------------------------------------------------------

TEST(Frames, StraightLineKeepsItsFrame) {
    const std::vector<Vec3f> pts = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}};
    const auto frames = rotationMinimizingFrames(pts.data(), pts.size(), {1, 0, 0});
    for (const Frame& f : frames) {
        expectOrthonormal(f);
        EXPECT_NEAR(f.normal.x, 1.0f, 1e-5f);
        EXPECT_NEAR(f.tangent.y, 1.0f, 1e-5f);
    }
}

TEST(Frames, PlanarArcDoesNotTwist) {
    // Arc in the XY plane: a rotation-minimizing normal starting along +Z stays +Z.
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 32; ++i) {
        const float a = 1.5f * static_cast<float>(i) / 32.0f;
        pts.emplace_back(std::cos(a), std::sin(a), 0.0f);
    }
    const auto frames = rotationMinimizingFrames(pts.data(), pts.size(), {0, 0, 1});
    for (const Frame& f : frames) {
        expectOrthonormal(f);
        EXPECT_NEAR(f.normal.z, 1.0f, 1e-4f);
    }
}

TEST(Frames, RotateBetweenMapsNormals) {
    const Vec3f from(0, 1, 0), to(1, 0, 0);
    const Vec3f r = rotateBetween({0, 2, 0}, from, to);
    EXPECT_NEAR(r.x, 2.0f, 1e-5f);
    EXPECT_NEAR(r.y, 0.0f, 1e-5f);
    const Vec3f side = rotateBetween({0, 0, 1}, from, to);  // on the rotation axis
    EXPECT_NEAR(side.z, 1.0f, 1e-5f);
    const Vec3f flip = rotateBetween({0, 1, 0}, from, -from);
    EXPECT_NEAR(flip.y, -1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------

TEST(Hierarchy, GroupsFormATree) {
    GuideHierarchy h;
    const GroupId a = h.addGroup("a");
    const GroupId b = h.addGroup("b", a);
    const GroupId c = h.addGroup("c", b);
    EXPECT_EQ(h.groupDepth(a), 0);
    EXPECT_EQ(h.groupDepth(c), 2);
    EXPECT_EQ(h.maxDepth(), 2);
    EXPECT_EQ(h.findGroup("b"), b);
    EXPECT_THROW(h.addGroup("a"), std::invalid_argument);
    EXPECT_THROW(h.addGuide(b, bentGuide({0, 0, 0})), std::invalid_argument);  // primaries only at depth 0

    h.removeGroup(b);
    EXPECT_EQ(h.groups().size(), 1u);
    EXPECT_EQ(h.findGroup("c"), kNoGroup);
}

TEST(Hierarchy, PrimaryGuidesRootAndFlip) {
    auto scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    std::vector<Vec3f> tipFirst = bentGuide({0.5f, 0.2f, 0.5f});
    std::reverse(tipFirst.begin(), tipFirst.end());
    const GuideId id = h.addGuide(g, tipFirst);
    h.evaluate(scalp.get());

    const HierarchyGuide& guide = *h.guide(id);
    EXPECT_GE(guide.root.face, 0);
    EXPECT_NEAR(guide.points.front().y, 0.0f, 1e-5f);
    EXPECT_GT(guide.points.back().y, 0.9f);
}

// The unit plane with every point mapped through `f` (same topology).
template <class F>
std::shared_ptr<const ScalpMesh> transformedPlane(F f) {
    const auto plane = test::makePlane(8);
    std::vector<Vec3f> pts;
    for (const Vec3f& p : plane->points()) pts.push_back(f(p));
    return std::make_shared<const ScalpMesh>(pts, plane->triangles());
}

TEST(Hierarchy, GuidesFollowAMovingScalp) {
    TwoGuides s;
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId primaryId = s.h.guidesInGroup(s.primary)[0];
    const GuideId childId = s.h.childrenOf(primaryId)[0];
    const std::vector<Vec3f> primaryBefore = s.h.guide(primaryId)->points;
    const std::vector<Vec3f> childBefore = s.h.guide(childId)->points;

    // Translate the scalp up by 2: every guide moves with it, no re-rooting.
    const auto lifted = transformedPlane([](Vec3f p) { return p + Vec3f(0, 2, 0); });
    s.h.evaluate(lifted.get());
    for (std::size_t i = 0; i < primaryBefore.size(); ++i) {
        EXPECT_NEAR((s.h.guide(primaryId)->points[i] - (primaryBefore[i] + Vec3f(0, 2, 0))).length(), 0.0f, 1e-4f);
        EXPECT_NEAR((s.h.guide(childId)->points[i] - (childBefore[i] + Vec3f(0, 2, 0))).length(), 0.0f, 1e-4f);
    }

    // Rotate it 90 degrees about Z, (x, y) -> (-y, x): a guide that stood along
    // +Y (the old normal) now stands along -X (the new normal).
    const auto turned = transformedPlane([](Vec3f p) { return Vec3f(-p.y, p.x, p.z); });
    s.h.evaluate(turned.get());
    const auto& pts = s.h.guide(primaryId)->points;
    const Vec3f rootToTip = pts.back() - pts.front();
    EXPECT_NEAR(rootToTip.x, -1.0f, 1e-3f);  // was +1 along Y
    EXPECT_NEAR(rootToTip.y, 0.6f, 1e-3f);   // was +0.6 along X
    EXPECT_NEAR(s.h.guide(childId)->points.front().x, 0.0f, 1e-4f);  // child still rooted on the plane (x = 0 now)

    // The binding survives JSON.
    const GuideHierarchy back = GuideHierarchy::fromJson(s.h.toJson());
    EXPECT_EQ(back.guide(primaryId)->rootLocal.size(), pts.size());
}

TEST(Hierarchy, ChildrenFillTheParentTube) {
    TwoGuides s;
    ChildGenerationParams params;
    params.count = 20;
    params.jitter = 0.0f;
    const GroupId kids = s.h.generateChildren(s.primary, "children", params);
    s.h.evaluate(s.scalp.get());

    EXPECT_EQ(s.h.guidesInGroup(kids).size(), 40u);
    EXPECT_EQ(s.h.groupDepth(kids), 1);
    for (const GuideId id : s.h.guidesInGroup(kids)) {
        const HierarchyGuide& c = *s.h.guide(id);
        const HierarchyGuide& p = *s.h.guide(c.parent);
        ASSERT_EQ(c.points.size(), p.points.size());
        EXPECT_GE(c.root.face, 0);
        EXPECT_NEAR(c.points.front().y, 0.0f, 1e-5f);  // rooted on the scalp
        // Tip within the tip disc: tipSpread * parent tip radius (0.2).
        EXPECT_LE((c.points.back() - p.points.back()).length(), 1.0f * 0.2f + 1e-4f);
        // Root near the parent root (disc of 0.8 * 0.1, plus the snap onto the plane).
        EXPECT_LE((c.points.front() - p.points.front()).length(), 0.12f);
        EXPECT_NEAR(c.tube.widthAt(1.0f), 0.2f * params.childTubeScale, 1e-6f);
    }
}

TEST(Hierarchy, ChildIdsAreStable) {
    TwoGuides a, b;
    const GroupId ka = a.h.generateChildren(a.primary, "children", {});
    const GroupId kb = b.h.generateChildren(b.primary, "children", {});
    EXPECT_EQ(a.h.guidesInGroup(ka), b.h.guidesInGroup(kb));

    // Regenerating after removal reproduces the same IDs.
    const auto before = a.h.guidesInGroup(ka);
    a.h.removeGroup(ka);
    const GroupId again = a.h.generateChildren(a.primary, "children", {});
    EXPECT_EQ(a.h.guidesInGroup(again), before);

    std::set<GuideId> unique(before.begin(), before.end());
    EXPECT_EQ(unique.size(), before.size());
}

TEST(Hierarchy, EditingAParentCarriesItsChildren) {
    TwoGuides s;
    const GroupId kids = s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());

    const GuideId parentId = s.h.guidesInGroup(s.primary)[0];
    const GuideId childId = s.h.childrenOf(parentId)[0];
    const Vec3f childTipBefore = s.h.guide(childId)->points.back();
    const Vec3f parentTipBefore = s.h.guide(parentId)->points.back();

    // Comb the parent 0.5 further along +Z, growing toward the tip.
    std::vector<Vec3f> combed = s.h.guide(parentId)->points;
    for (std::size_t i = 0; i < combed.size(); ++i) combed[i].z += 0.5f * i / (combed.size() - 1);
    s.h.setPoints(parentId, combed);
    s.h.evaluate(s.scalp.get());

    const Vec3f parentMove = s.h.guide(parentId)->points.back() - parentTipBefore;
    const Vec3f childMove = s.h.guide(childId)->points.back() - childTipBefore;
    EXPECT_NEAR(parentMove.z, 0.5f, 1e-5f);
    EXPECT_NEAR(childMove.z, 0.5f, 0.08f);  // follows, up to the frame turning with the comb
    EXPECT_EQ(s.h.guidesInGroup(kids).size(), 10u);
}

TEST(Hierarchy, LockDetachesAndUnlockReattachesWithoutAJump) {
    TwoGuides s;
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId parentId = s.h.guidesInGroup(s.primary)[0];
    const GuideId childId = s.h.childrenOf(parentId)[0];
    const std::vector<Vec3f> childBefore = s.h.guide(childId)->points;

    s.h.lock(childId);
    std::vector<Vec3f> moved = s.h.guide(parentId)->points;
    for (Vec3f& p : moved) p.x += 0.3f;
    moved[0].x -= 0.3f;  // keep the root where it is
    s.h.setPoints(parentId, moved);
    s.h.evaluate(s.scalp.get());
    for (std::size_t i = 0; i < childBefore.size(); ++i) {
        EXPECT_NEAR((s.h.guide(childId)->points[i] - childBefore[i]).length(), 0.0f, 1e-5f);
    }

    const std::vector<Vec3f> lockedShape = s.h.guide(childId)->points;
    s.h.unlock(childId);
    s.h.evaluate(s.scalp.get());
    for (std::size_t i = 0; i < lockedShape.size(); ++i) {
        EXPECT_NEAR((s.h.guide(childId)->points[i] - lockedShape[i]).length(), 0.0f, 1e-3f);
    }
}

TEST(Hierarchy, EditingAFollowerIsStable) {
    TwoGuides s;
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId childId = s.h.childrenOf(s.h.guidesInGroup(s.primary)[0])[0];

    std::vector<Vec3f> edited = s.h.guide(childId)->points;
    edited.back().z += 0.2f;
    s.h.setPoints(childId, edited);
    s.h.evaluate(s.scalp.get());
    EXPECT_NEAR((s.h.guide(childId)->points.back() - edited.back()).length(), 0.0f, 1e-4f);
}

TEST(Hierarchy, ThreeLevelsFlattenByLevel) {
    TwoGuides s;
    ChildGenerationParams params;
    params.count = 4;
    const GroupId kids = s.h.generateChildren(s.primary, "children", params);
    s.h.evaluate(s.scalp.get());
    params.count = 3;
    const GroupId grandkids = s.h.generateChildren(kids, "grandchildren", params);
    s.h.evaluate(s.scalp.get());

    EXPECT_EQ(s.h.maxDepth(), 2);
    EXPECT_EQ(s.h.flatten(0).size(), 2u);
    EXPECT_EQ(s.h.flatten(1).size(), 8u);
    EXPECT_EQ(s.h.flatten(-1).size(), 24u);
    EXPECT_EQ(s.h.flattenGroup(grandkids).size(), 24u);
    const GuideSet flat = s.h.flatten(-1);  // guides() is the set's own: not a temporary's
    for (const Guide& g : flat.guides()) EXPECT_GE(g.root.face, 0);
}

TEST(Hierarchy, EvaluatesThousandsOfGuidesQuickly) {
    // 60 primaries x 12 children x 6 grandchildren = 5040 guides, 3 levels, on a
    // 40k-triangle sphere. Every child root is snapped with a closest-point query.
    const auto scalp = test::makeSphere(100, 200, 1.0f);
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(11);
    for (int i = 0; i < 60; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 12; ++k) pts.push_back(n * (1.0f + 0.1f * k) + Vec3f(0, -0.04f * k * k / 12.0f, 0));
        h.addGuide(primary, pts);
    }
    h.evaluate(scalp.get());
    ChildGenerationParams params;
    params.count = 12;
    const GroupId kids = h.generateChildren(primary, "children", params);
    h.evaluate(scalp.get());
    params.count = 6;
    h.generateChildren(kids, "grandchildren", params);

    const auto t0 = std::chrono::steady_clock::now();
    h.evaluate(scalp.get());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::printf("[ hierarchy ] evaluate %zu guides on 40k triangles: %.1f ms\n", h.guides().size(), ms);
    RecordProperty("evaluate_ms", std::to_string(ms));
    EXPECT_EQ(h.guides().size(), 60u + 720u + 4320u);
    EXPECT_LT(ms, 250.0);  // brute-force rooting took seconds here
    const GuideSet flat = h.flatten(-1);  // guides() is the set's own: not a temporary's
    for (const Guide& g : flat.guides()) ASSERT_GE(g.root.face, 0);
}

TEST(Hierarchy, EveryChangeChangesTheKeptContentHash) {
    // The hash is kept between changes: after each public change it is what working it out
    // anew gives, and differs from before.
    const auto sphere = test::makeSphere(16, 32, 1.0f);
    std::vector<Vec3f> stretched = sphere->points();
    for (Vec3f& p : stretched) p = Vec3f(p.x * 1.1f, p.y, p.z);
    const auto moved = std::make_shared<const ScalpMesh>(sphere->withPoints(stretched));
    GuideHierarchy base;
    const GroupId primary = base.addGroup("primary");
    const GroupId second = base.addGroup("second");
    std::vector<GuideId> primaries;
    for (int i = 0; i < 6; ++i) {
        const Vec3f n = Vec3f(std::cos(static_cast<float>(i)), 0.5f, std::sin(static_cast<float>(i))).normalized();
        primaries.push_back(base.addGuide(primary, test::straightCurve(n, n * 0.5f + Vec3f(0.1f, 0, 0), 6)));
    }
    base.addGuide(second, test::straightCurve(Vec3f(0, 1, 0), {0, 0.5f, 0.1f}, 6));
    base.evaluate(sphere.get());
    ChildGenerationParams params;
    params.count = 3;
    const GroupId kids = base.generateChildren(primary, "kids", params);
    base.evaluate(sphere.get());
    base.setAttribute(primaries[0], "clump", 0.5f);
    const GuideId child = base.childrenOf(primaries[0]).front();
    const auto curve = [](float x) { return test::straightCurve(Vec3f(x, 0.9f, 0.3f).normalized(), {0.2f, 0.4f, 0}, 6); };

    using Change = std::function<void(GuideHierarchy&)>;
    const std::vector<std::pair<const char*, Change>> changes = {
        {"addGroup", [](GuideHierarchy& h) { h.addGroup("more"); }},
        {"renameGroup", [&](GuideHierarchy& h) { h.renameGroup(primary, "renamed"); }},
        {"removeGroup", [&](GuideHierarchy& h) { h.removeGroup(second); }},
        {"addGuide", [&](GuideHierarchy& h) { h.addGuide(primary, curve(0.2f)); }},
        {"addGuides",
         [&](GuideHierarchy& h) {
             GuideSet set;
             set.add(500, curve(-0.2f));
             h.addGuides(second, set);
         }},
        {"generateChildren",
         [&](GuideHierarchy& h) {
             ChildGenerationParams other = params;
             other.seed = 7;
             h.generateChildren(primary, "kids", other);
         }},
        {"addChildGuide", [&](GuideHierarchy& h) { h.addChildGuide(kids, primaries[1], h.guide(primaries[1])->points); }},
        {"evaluate", [&](GuideHierarchy& h) { h.evaluate(moved.get()); }},
        {"evaluateSubtrees", [&](GuideHierarchy& h) { h.evaluateSubtrees(moved.get(), {primaries[2]}); }},
        {"pose", [&](GuideHierarchy& h) { h.pose(primaries[3], curve(0.4f)); }},
        {"evaluateDescendants", [&](GuideHierarchy& h) { h.evaluateDescendants(moved.get(), {primaries[3]}); }},
        {"reparent", [&](GuideHierarchy& h) { h.reparent({child}, primaries[4]); }},
        {"promoteGroup", [&](GuideHierarchy& h) { h.promoteGroup(kids); }},
        {"demoteGroup", [&](GuideHierarchy& h) { h.demoteGroup(second, primary); }},
        {"mirrorGroup", [&](GuideHierarchy& h) { h.mirrorGroup(primary, 0, 0.0f, true); }},
        {"mirrorAll", [](GuideHierarchy& h) { h.mirrorAll(0, 0.0f, true); }},
        {"transfer", [&](GuideHierarchy& h) { h.transfer(*sphere, *moved); }},
        {"removeGuides", [&](GuideHierarchy& h) { h.removeGuides({primaries[5]}); }},
        {"lock", [&](GuideHierarchy& h) { h.lock(child); }},
        {"unlock", [&](GuideHierarchy& h) { h.unlock(child); }},  // locked first, below
        {"setPoints", [&](GuideHierarchy& h) { h.setPoints(primaries[1], curve(0.6f)); }},
        {"setTube",
         [&](GuideHierarchy& h) {
             TubeProfile tube;
             tube.width = {0.3f, 0.1f};
             h.setTube(primaries[1], tube);
         }},
        {"setAttribute", [&](GuideHierarchy& h) { h.setAttribute(primaries[2], "clump", 0.25f); }},
        {"setPointAttribute", [&](GuideHierarchy& h) { h.setPointAttribute(primaries[2], "melanin", {0.1f, 0.9f}); }},
        {"removeAttribute", [](GuideHierarchy& h) { h.removeAttribute("clump"); }},
        {"renameAttribute", [](GuideHierarchy& h) { h.renameAttribute("clump", "density"); }},
    };
    for (const auto& [what, change] : changes) {
        GuideHierarchy h = base;
        if (std::string(what) == "unlock") h.lock(child);
        const std::uint64_t before = h.contentHash();
        EXPECT_EQ(before, h.computeContentHash()) << what;
        change(h);
        EXPECT_EQ(h.contentHash(), h.computeContentHash()) << what << ": the kept hash is stale";
        EXPECT_NE(h.contentHash(), before) << what << ": the change changed nothing";
        EXPECT_EQ(base.contentHash(), base.computeContentHash()) << what << ": the copy's change reached the original";
    }
    // A loaded hierarchy, and an assigned one, hash as they hold.
    GuideHierarchy loaded = GuideHierarchy::fromJson(base.toJson());
    EXPECT_EQ(loaded.contentHash(), loaded.computeContentHash());
    loaded = base;
    EXPECT_EQ(loaded.contentHash(), base.computeContentHash());
}

TEST(Hierarchy, JsonRoundTrip) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const GuideId id = h.addGuide(g, bentGuide({0.2f, 0, 0.2f}));
    h.setAttribute(id, "clump", 0.5f);
    const GroupId kids = h.generateChildren(g, "children", {});
    h.lock(h.guidesInGroup(kids)[1]);
    h.evaluate(nullptr);

    const GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    EXPECT_EQ(back.contentHash(), h.contentHash());
    EXPECT_FLOAT_EQ(back.guide(id)->attributes.at("clump"), 0.5f);
    EXPECT_TRUE(back.guide(h.guidesInGroup(kids)[1])->locked);

    EXPECT_TRUE(GuideHierarchy::fromJson("").guides().empty());
    EXPECT_THROW(GuideHierarchy::fromJson("{bad"), std::runtime_error);
}

// The guides are written and read side by side (toJson, fromJson): the text is the one document
// dump would give, and any text reads as one parse of it reads (spaces, a group named like the
// guides key, the key twice, errors).
TEST(Hierarchy, JsonGuidesSideBySide) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary \"guides\":[{,}] \\ name");
    for (int i = 0; i < 40; ++i) {
        const GuideId id = h.addGuide(g, bentGuide({0.05f * i, 0, 0.3f}, 7 + i % 5));
        h.setAttribute(id, "clump", 0.01f * i);
        if (i % 3 == 0) h.setPointAttribute(id, "melanin", std::vector<float>(8 + i % 5, 0.25f * (i % 4)));
    }
    const GroupId kids = h.generateChildren(g, "children", {});
    h.lock(h.guidesInGroup(kids)[1]);
    h.evaluate(nullptr);

    const std::string text = h.toJson();
    EXPECT_EQ(text, nlohmann::json::parse(text).dump()) << "the text one document's dump gives";
    auto same = [&](const GuideHierarchy& back, const char* what) {
        EXPECT_EQ(back.contentHash(), h.contentHash()) << what;
        EXPECT_EQ(back.toJson(), text) << what;
        EXPECT_EQ(back.groups().size(), h.groups().size()) << what;
        // The next guide made gets the same ID (they seed what grows from it).
        GuideHierarchy a = back, b = GuideHierarchy::fromJson(text);
        EXPECT_EQ(a.addGuide(g, bentGuide({0.9f, 0, 0.9f})), b.addGuide(g, bentGuide({0.9f, 0, 0.9f}))) << what;
    };
    same(GuideHierarchy::fromJson(text), "compact");
    same(GuideHierarchy::fromJson(nlohmann::json::parse(text).dump(2)), "indented");
    // The guides key twice: the last one counts, as a parse of the whole text reads it.
    nlohmann::json twice = nlohmann::json::parse(text);
    const std::string guides = twice["guides"].dump();
    twice.erase("guides");
    std::string doubled = twice.dump();
    doubled.insert(1, "\"guides\":[{\"id\":1}],\"guides\":" + guides + ",");
    same(GuideHierarchy::fromJson(doubled), "the key twice");
    // Errors read as before: a guide that does not parse, one with a bad point array.
    std::string broken = text;
    broken.insert(broken.find("\"points\""), "\"x\":,");
    EXPECT_THROW(GuideHierarchy::fromJson(broken), std::runtime_error);
    std::string uneven = text;
    uneven.insert(uneven.find("\"points\":[") + 10, "1.0,");
    try {
        GuideHierarchy::fromJson(uneven);
        ADD_FAILURE() << "a point array of 3n + 1 numbers reads";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "point array length is not a multiple of 3");
    }
}

// ---------------------------------------------------------------------------
// Graph integration

TEST(Hierarchy, GuidesOperatorFlattensLevelsAndGroups) {
    TwoGuides s;
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    auto hierarchy = std::make_shared<GuideHierarchy>(s.h);

    ExternalInputs ext;
    ext.set("hierarchy", HierarchyPtr(hierarchy), hierarchy->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;

    Graph g;
    const NodeId guides = g.addNode(op::kGuides);
    EXPECT_EQ(valueAs<GuidesPtr>(g.evaluate(guides, ctx))->size(), 10u);  // deepest level
    g.node(guides).params().set("level", std::int64_t{0});
    EXPECT_EQ(valueAs<GuidesPtr>(g.evaluate(guides, ctx))->size(), 2u);
    g.node(guides).params().set("group", std::string("primary"));
    EXPECT_EQ(valueAs<GuidesPtr>(g.evaluate(guides, ctx))->size(), 2u);
    g.node(guides).params().set("group", std::string("primary children, primary"));  // several: together, once each
    EXPECT_EQ(valueAs<GuidesPtr>(g.evaluate(guides, ctx))->size(), 12u);
    g.node(guides).params().set("group", std::string("nope"));
    EXPECT_THROW(g.evaluate(guides, ctx), GraphError);
}

TEST(Hierarchy, LevelMinusOneTakesEachFamilysDeepestGroups) {
    // A group with children (theirs), a group without (its own), and one whose child group is
    // empty (its own): level -1 takes all three families, not only the hierarchy's deepest depth
    // (which left the plain groups out of Interpolate and Scatter's guideRadius). Scatter's
    // guideRadius grows round every level of them: as with the groups listed by hand.
    TwoGuides s;
    const GroupId children = s.h.generateChildren(s.primary, "children", {});
    TubeProfile tube;
    tube.width = {0.1f, 0.2f};
    const GroupId bangs = s.h.addGroup("bangs");
    s.h.addGuide(bangs, bentGuide({0.2f, 0, 0.8f}), tube);
    s.h.addGuide(bangs, bentGuide({0.4f, 0, 0.8f}), tube);
    const GroupId lone = s.h.addGroup("lone");
    s.h.addGuide(lone, bentGuide({0.8f, 0, 0.2f}), tube);
    s.h.addGroup("loneKids", lone);
    s.h.evaluate(s.scalp.get());
    EXPECT_EQ(s.h.deepestGroups(), (std::set<GroupId>{children, bangs, lone}));
    EXPECT_EQ(s.h.flatten().size(), 13u);  // 10 children, 2 bangs, 1 lone

    auto hierarchy = std::make_shared<GuideHierarchy>(s.h);
    ExternalInputs ext;
    ext.set("hierarchy", HierarchyPtr(hierarchy), hierarchy->contentHash());
    ext.set("scalp", s.scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    Graph g;
    const NodeId scalp = g.addNode(op::kScalp);
    const NodeId guides = g.addNode(op::kGuides);
    const NodeId scatter = g.addNode(op::kScatter);
    g.node(scatter).params().set("density", 4000.0);
    g.node(scatter).params().set("guideRadius", 0.08);
    g.connect(scalp, scatter, 0);
    g.connect(guides, scatter, 1);
    EXPECT_EQ(valueAs<GuidesPtr>(g.evaluate(guides, ctx))->size(), 13u);
    const std::size_t byDefault = g.evaluateStrands(scatter, ctx)->strandCount();
    g.node(guides).params().set("group", std::string("primary children bangs lone"));
    const std::size_t listed = g.evaluateStrands(scatter, ctx)->strandCount();
    EXPECT_GT(byDefault, 0u);
    EXPECT_EQ(byDefault, listed);
}

TEST(Hierarchy, AGuidesNodeOnAGroupMadeEmptyGivesNoGuidesBeforeAnyIsDrawn) {
    // A groom with no guides yet and its hand-placed group made (the hand_placed_feathers preset):
    // an empty set, so the feathers it feeds grow nothing rather than missing their input.
    auto hierarchy = std::make_shared<GuideHierarchy>();
    hierarchy->addGroup("feathers");
    ExternalInputs ext;
    ext.set("hierarchy", HierarchyPtr(hierarchy), hierarchy->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    Graph g;
    const NodeId guides = g.addNode(op::kGuides);
    g.node(guides).params().set("group", std::string("feathers"));
    const GuidesPtr none = valueAs<GuidesPtr>(g.evaluate(guides, ctx));
    ASSERT_TRUE(none);
    EXPECT_EQ(none->size(), 0u);
    g.node(guides).params().set("group", std::string("feathers tail"));  // a group not made: as before, nothing
    EXPECT_FALSE(valueAs<GuidesPtr>(g.evaluate(guides, ctx)));
    g.node(guides).params().set("group", std::string());  // no group named: the flat guides (none here)
    EXPECT_FALSE(valueAs<GuidesPtr>(g.evaluate(guides, ctx)));
}

// Scalp shaped like a step: a horizontal face (normal +Y, x in [-1, 0]) meeting
// a vertical face (normal +X, y in [-1, 0]) along the Z axis.
std::shared_ptr<const ScalpMesh> makeStep() {
    std::vector<Vec3f> pts;
    std::vector<ScalpMesh::Triangle> tris;
    const int cells = 6;
    const float step = 1.0f / cells;
    auto quad = [&](Vec3f a, Vec3f b, Vec3f c, Vec3f d) {  // a-b-c-d counter-clockwise seen from outside
        const auto i = static_cast<std::int32_t>(pts.size());
        pts.insert(pts.end(), {a, b, c, d});
        tris.push_back({i, i + 1, i + 3});
        tris.push_back({i + 1, i + 2, i + 3});
    };
    for (int u = 0; u < cells; ++u) {
        for (int v = 0; v < cells; ++v) {
            const float x0 = -1.0f + u * step, x1 = x0 + step, z0 = v * step, z1 = z0 + step;
            quad({x0, 0, z0}, {x0, 0, z1}, {x1, 0, z1}, {x1, 0, z0});  // horizontal, +Y
            const float y0 = -1.0f + u * step, y1 = y0 + step;
            quad({0, y0, z0}, {0, y1, z0}, {0, y1, z1}, {0, y0, z1});  // vertical, +X
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(pts), std::move(tris));
}

TEST(Interpolate, AlignsGuideShapesToTheStrandNormal) {
    auto scalp = makeStep();
    ASSERT_NEAR(scalp->normal({0, 0.3f, 0.3f}).y, 1.0f, 1e-4f);

    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({-0.5f, 0.0f, 0.5f}, {0, 1, 0}));  // up, on the horizontal face
    guides->rootOnMesh(*scalp);

    for (const bool connectScalp : {false, true}) {
        ExternalInputs ext;
        ext.set("scalp", MeshPtr(scalp), 1);
        ext.set("guides", GuidesPtr(guides), 2);
        EvalContext ctx;
        ctx.externals = &ext;

        Graph g;
        const NodeId scalpNode = g.addNode(op::kScalp);
        const NodeId guideNode = g.addNode(op::kGuides);
        const NodeId scatter = g.addNode(op::kScatter);
        const NodeId interp = g.addNode(op::kInterpolate);
        g.node(scatter).params().set("density", 200.0);
        g.node(interp).params().set("neighbors", std::int64_t{1});
        g.connect(scalpNode, scatter, 0);
        g.connect(scatter, interp, 0);
        g.connect(guideNode, interp, 1);
        if (connectScalp) g.connect(scalpNode, interp, 2);

        const StrandsPtr out = g.evaluateStrands(interp, ctx);
        std::size_t checked = 0;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const Vec3f* p = out->strandPoints(s);
            if (p[0].y > -0.2f) continue;  // only strands well down the vertical face
            const Vec3f dir = (p[out->strandPointCount(s) - 1] - p[0]).normalized();
            if (connectScalp) {
                EXPECT_NEAR(dir.x, 1.0f, 1e-3f);  // outward from the vertical face
            } else {
                EXPECT_NEAR(dir.y, 1.0f, 1e-3f);  // world-space copy: straight up, into the step
            }
            ++checked;
        }
        EXPECT_GT(checked, 50u);
    }
}


TEST(Hierarchy, ARemeshedScalpReRootsInsteadOfUsingStaleFaces) {
    // Bound guides store their shape in their root face's frame. The same
    // surface with other triangles (a subdivided head) must not rebuild them
    // from face indices that now mean other triangles: they re-root where
    // they are.
    TwoGuides s;
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    const GuideId primaryId = s.h.guidesInGroup(s.primary)[0];
    const std::vector<Vec3f> before = s.h.guide(primaryId)->points;
    const std::uint64_t boundOn = s.h.boundTopology();
    EXPECT_EQ(boundOn, s.scalp->topologyHash());

    const auto finer = test::makePlane(16);  // the same unit plane, four times the triangles
    EXPECT_NE(finer->topologyHash(), boundOn);
    s.h.evaluate(finer.get());
    const auto& after = s.h.guide(primaryId)->points;
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_NEAR((after[i] - before[i]).length(), 0.0f, 1e-3f);
    EXPECT_EQ(s.h.boundTopology(), finer->topologyHash());
    // Rooted on the new mesh, and the children still sit on the surface.
    EXPECT_LT(s.h.guide(primaryId)->root.face, static_cast<std::int32_t>(finer->triangleCount()));
    for (const GuideId child : s.h.childrenOf(primaryId)) EXPECT_NEAR(s.h.guide(child)->points.front().y, 0.0f, 1e-4f);
    // The binding's topology travels with the JSON.
    EXPECT_EQ(GuideHierarchy::fromJson(s.h.toJson()).boundTopology(), finer->topologyHash());
}

}  // namespace
}  // namespace groomlab
