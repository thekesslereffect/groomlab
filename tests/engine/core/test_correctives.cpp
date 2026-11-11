// SPDX-License-Identifier: Apache-2.0
// Shot sculpting: corrective layers recorded from sculpts, applied over time.

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/correctives.h"
#include "groomlab/groom.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

struct Scene {
    std::shared_ptr<const ScalpMesh> scalp = test::makeSphere(24, 48, 1.0f);
    GuideHierarchy h;
    GroupId primary = kNoGroup, children = kNoGroup;

    Scene() {
        primary = h.addGroup("primary");
        TubeProfile tube;
        tube.width = {0.15f, 0.1f};
        for (const float a : {0.3f, 0.9f, 1.5f}) {
            const Vec3f n = Vec3f{std::sin(a), std::cos(a), 0.25f}.normalized();
            h.addGuide(primary, test::straightCurve(n, n * 0.8f, 8), tube);
        }
        h.evaluate(scalp.get());
        ChildGenerationParams p;
        p.count = 3;
        children = h.generateChildren(primary, "children", p);
        h.evaluate(scalp.get());
    }
};

float maxDistance(const GuideHierarchy& a, const GuideHierarchy& b) {
    float d = 0.0f;
    for (const HierarchyGuide& g : a.guides()) {
        const HierarchyGuide* o = b.guide(g.id);
        EXPECT_NE(o, nullptr);
        if (!o) continue;
        for (std::size_t k = 0; k < g.points.size(); ++k) d = std::max(d, (g.points[k] - o->points[k]).length());
    }
    return d;
}

// A sculpt: a primary's tip half bent sideways (children follow), and one
// child of another primary bent on its own.
GuideHierarchy sculpt(const Scene& s, GuideId& bentPrimary, GuideId& bentChild) {
    GuideHierarchy out = s.h;
    bentPrimary = out.guidesInGroup(s.primary)[0];
    std::vector<Vec3f> pts = out.guide(bentPrimary)->points;
    for (std::size_t k = 4; k < pts.size(); ++k) pts[k] += Vec3f(0.0f, 0.0f, 0.03f * float(k - 3));
    out.pose(bentPrimary, pts);
    out.evaluateDescendants(s.scalp.get(), {bentPrimary});
    bentChild = out.childrenOf(out.guidesInGroup(s.primary)[1]).at(0);
    pts = out.guide(bentChild)->points;
    for (std::size_t k = 2; k < pts.size(); ++k) pts[k] += Vec3f(0.02f * float(k), 0.0f, 0.0f);
    out.pose(bentChild, pts);
    return out;
}

TEST(Correctives, RecordThenApplyReproducesTheSculpt) {
    Scene s;
    GuideId bentPrimary = kNoGuide, bentChild = kNoGuide;
    const GuideHierarchy sculpted = sculpt(s, bentPrimary, bentChild);
    Correctives c;
    const std::vector<GuideId> recorded = c.record("fix", s.h, sculpted, 1.0, s.scalp.get());
    // Only what was sculpted gets offsets: not the children that followed the primary.
    EXPECT_EQ(recorded.size(), 2u);
    EXPECT_EQ(c.layer("fix")->offsets.size(), 2u);
    EXPECT_TRUE(c.layer("fix")->offsets.count(bentPrimary));
    EXPECT_TRUE(c.layer("fix")->offsets.count(bentChild));

    GuideHierarchy applied = s.h;
    c.apply(applied, 1.0, s.scalp.get());
    EXPECT_LT(maxDistance(applied, sculpted), 1e-4f);

    // A second sculpt on top, in the same layer, keeps the first.
    GuideHierarchy more = applied;
    std::vector<Vec3f> pts = more.guide(bentPrimary)->points;
    pts.back() += Vec3f(0.05f, 0.0f, 0.0f);
    more.pose(bentPrimary, pts);
    more.evaluateDescendants(s.scalp.get(), {bentPrimary});
    c.record("fix", s.h, more, 1.0, s.scalp.get());
    GuideHierarchy again = s.h;
    c.apply(again, 1.0, s.scalp.get());
    EXPECT_LT(maxDistance(again, more), 1e-4f);

    // Through JSON.
    const Correctives loaded = Correctives::fromJson(c.toJson());
    GuideHierarchy fromJson = s.h;
    loaded.apply(fromJson, 1.0, s.scalp.get());
    EXPECT_LT(maxDistance(fromJson, more), 1e-5f);
}

TEST(Correctives, EveryHostNamesAndKeysLayersAlike) {
    // correctives.h: Maya's shots.py, the Blender helper and the app (which once named them
    // "layer", "layer1" and took a rename as typed).
    Correctives c;
    EXPECT_EQ(c.newLayerName(true, 0.0), "sculpt1");
    EXPECT_EQ(c.newLayerName(false, 11.6), "shot12");
    EXPECT_EQ(c.freeLayerName(" hair fix! "), "hair_fix_");
    EXPECT_EQ(c.freeLayerName("3d"), "c3d");
    EXPECT_EQ(c.freeLayerName(""), "corrective");
    CorrectiveLayer a;
    a.name = "shot12";
    c.layers.push_back(a);
    EXPECT_EQ(c.freeLayerName("shot12"), "shot12_2");
    EXPECT_EQ(c.freeLayerName("shot12", "shot12"), "shot12") << "renaming a layer to its own name";
    EXPECT_EQ(c.freeLayerName("fix", {}, {"fix", "fix_2"}), "fix_3") << "names the host holds";
    EXPECT_EQ(c.newLayerName(true, 0.0), "sculpt2");
    // Keys: set or changed, kept sorted; a layer with none, held both ways, is on every frame.
    CorrectiveLayer& l = c.layers.front();
    l.holdBefore = l.holdAfter = true;
    EXPECT_TRUE(l.allFrames());
    l.keyAt(2.0, 1.0f);
    l.keyAt(1.0, 0.0f);
    l.keyAt(2.0, 0.5f);
    ASSERT_EQ(l.weightCurve.size(), 2u);
    EXPECT_DOUBLE_EQ(l.weightCurve[0].first, 1.0);
    EXPECT_FLOAT_EQ(l.weightCurve[1].second, 0.5f);
    EXPECT_FALSE(l.allFrames());
}

TEST(Correctives, FadeAndHold) {
    CorrectiveLayer l;
    l.time = 2.0;
    l.fadeIn = 1.0;
    l.fadeOut = 0.5;
    EXPECT_FLOAT_EQ(l.weightAt(2.0), 1.0f);
    EXPECT_FLOAT_EQ(l.weightAt(0.9), 0.0f);
    EXPECT_FLOAT_EQ(l.weightAt(2.6), 0.0f);
    EXPECT_NEAR(l.weightAt(1.5), 0.5f, 1e-5f);
    EXPECT_GT(l.weightAt(2.2), 0.0f);
    EXPECT_LT(l.weightAt(2.2), 1.0f);
    l.holdAfter = true;
    EXPECT_FLOAT_EQ(l.weightAt(10.0), 1.0f);
    l.weight = 0.5f;
    EXPECT_FLOAT_EQ(l.weightAt(10.0), 0.5f);
    l.enabled = false;
    EXPECT_FLOAT_EQ(l.weightAt(2.0), 0.0f);

    Scene s;
    GuideId p = kNoGuide, ch = kNoGuide;
    const GuideHierarchy sculpted = sculpt(s, p, ch);
    Correctives c;
    c.record("fix", s.h, sculpted, 2.0, s.scalp.get());
    c.layer("fix")->fadeOut = 0.5;
    GuideHierarchy later = s.h;
    c.apply(later, 3.0, s.scalp.get());  // faded out
    EXPECT_LT(maxDistance(later, s.h), 1e-6f);
    GuideHierarchy half = s.h;
    c.apply(half, 2.25, s.scalp.get());
    const float full = maxDistance(sculpted, s.h), partial = maxDistance(half, s.h);
    EXPECT_GT(partial, 0.2f * full);
    EXPECT_LT(partial, 0.8f * full);
    EXPECT_THROW(c.record("fix", s.h, sculpted, 3.0, s.scalp.get()), std::invalid_argument);  // no weight there
}

TEST(Correctives, AWeightCurveReplacesTheEnvelope) {
    CorrectiveLayer l;
    l.time = 5.0;  // an envelope that would be zero everywhere below
    l.fadeIn = l.fadeOut = 0.1;
    l.weight = 0.25f;
    l.slot = 3;
    l.weightCurve = {{1.0, 0.0f}, {2.0, 1.0f}, {3.0, 0.5f}};
    EXPECT_FLOAT_EQ(l.weightAt(0.0), 0.0f);   // held before the first key
    EXPECT_FLOAT_EQ(l.weightAt(1.5), 0.5f);   // linear between samples
    EXPECT_FLOAT_EQ(l.weightAt(2.0), 1.0f);
    EXPECT_FLOAT_EQ(l.weightAt(2.5), 0.75f);
    EXPECT_FLOAT_EQ(l.weightAt(9.0), 0.5f);   // held after the last
    l.enabled = false;
    EXPECT_FLOAT_EQ(l.weightAt(2.0), 0.0f);
    l.enabled = true;

    Correctives c;
    c.layers.push_back(l);
    c.layers.push_back(CorrectiveLayer{});  // no slot, no curve
    c.layers[1].name = "plain";
    const Correctives back = Correctives::fromJson(c.toJson());
    ASSERT_EQ(back.layers.size(), 2u);
    EXPECT_EQ(back.layers[0].slot, 3);
    ASSERT_EQ(back.layers[0].weightCurve.size(), 3u);
    EXPECT_FLOAT_EQ(back.layers[0].weightAt(2.5), 0.75f);
    EXPECT_EQ(back.layers[1].slot, -1);
    EXPECT_TRUE(back.layers[1].weightCurve.empty());
    EXPECT_EQ(c.toJson().find("weightCurve", c.toJson().find("plain")), std::string::npos);  // only when there is one
}

TEST(Correctives, RideTheAnimation) {
    // Sculpt on one pose; on another pose (the groom moved) the correction moves with it.
    Scene s;
    GuideId p = kNoGuide, ch = kNoGuide;
    const GuideHierarchy sculpted = sculpt(s, p, ch);
    Correctives c;
    c.record("fix", s.h, sculpted, 1.0, s.scalp.get());

    GuideHierarchy moved = s.h;  // every primary shifted up, children following (a "sim" pose)
    std::vector<GuideId> primaries = moved.guidesInGroup(s.primary);
    for (const GuideId id : primaries) {
        std::vector<Vec3f> pts = moved.guide(id)->points;
        for (Vec3f& q : pts) q += Vec3f(0.0f, 0.2f, 0.0f);
        moved.pose(id, pts);
    }
    moved.evaluateDescendants(s.scalp.get(), primaries);
    GuideHierarchy corrected = moved;
    c.apply(corrected, 1.0, s.scalp.get());
    const HierarchyGuide& a = *corrected.guide(p);
    for (std::size_t k = 0; k < a.points.size(); ++k) {
        const Vec3f expected = sculpted.guide(p)->points[k] + Vec3f(0.0f, 0.2f, 0.0f);
        EXPECT_LT((a.points[k] - expected).length(), 1e-4f) << k;
    }
}

TEST(Correctives, GroomAppliesThemAtEvaluationTime) {
    Scene s;
    GuideId p = kNoGuide, ch = kNoGuide;
    const GuideHierarchy sculpted = sculpt(s, p, ch);
    Correctives c;
    c.record("fix", s.h, sculpted, 1.0, s.scalp.get());
    c.layer("fix")->fadeOut = 0.5;

    GroomData d;
    d.scalp = s.scalp;
    d.hierarchyJson = s.h.toJson();
    d.correctivesJson = c.toJson();
    d.graphJson = "";
    Groom groom(d);
    groom.evaluate(0.02f, 1.0);
    const HierarchyPtr atKey = groom.hierarchy();
    EXPECT_LT((atKey->guide(p)->points.back() - sculpted.guide(p)->points.back()).length(), 1e-3f);
    groom.evaluate(0.02f, 5.0);
    const HierarchyPtr later = groom.hierarchy();
    EXPECT_GT((later->guide(p)->points.back() - sculpted.guide(p)->points.back()).length(), 0.05f);
}

}  // namespace
}  // namespace groomlab
