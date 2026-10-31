// SPDX-License-Identifier: Apache-2.0
// Length field (length_field.h): the keys' scales at the keys and blended between them, one key
// scaling everything, the scale easing back to x1 away from the keys, distances along the skin, a
// guide at a new length (scaled or trimmed), mirroring, and the keys saved with the guides.

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/length_field.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A key at `point` (on `scalp`: its root found there), scaling by `scale`.
LengthKey key(const Vec3f& point, float scale, const ScalpMesh* scalp = nullptr) {
    LengthKey k;
    k.point = point;
    k.root.face = -1;
    k.scale = scale;
    if (scalp) {
        const SurfacePoint hit = scalp->closestPoint(point);
        k.root = hit.root;
        k.point = hit.position;
    }
    return k;
}

// A straight guide of `points` points standing up at (x, 0, z), `length` long.
std::vector<Vec3f> standing(float x, float z, float length, int points = 5) {
    std::vector<Vec3f> out;
    for (int i = 0; i < points; ++i) out.push_back({x, length * static_cast<float>(i) / static_cast<float>(points - 1), z});
    return out;
}

TEST(LengthField, TheKeysScalesAtTheKeysBlendedBetween) {
    const auto plane = test::makePlane(20, 2.0f);  // x and z from 0 to 2
    LengthKeySet set;
    set.keys = {key({0.5f, 0.0f, 1.0f}, 2.0f, plane.get()), key({1.5f, 0.0f, 1.0f}, 0.5f, plane.get())};
    const LengthField field(set, plane.get());
    EXPECT_NEAR(field.spacing(), 1.0f, 1e-3f);
    EXPECT_NEAR(field.scaleAt({0.5f, 0.0f, 1.0f}), 2.0f, 0.02f);
    EXPECT_NEAR(field.scaleAt({1.5f, 0.0f, 1.0f}), 0.5f, 0.01f);
    // Halfway: x2 and x0.5 meet at x1 (logarithms).
    EXPECT_NEAR(field.scaleAt({1.0f, 0.0f, 1.0f}), 1.0f, 1e-3f);
    // Down from one key to the other, never past either.
    float last = 3.0f;
    for (int i = 0; i <= 20; ++i) {
        const float s = field.scaleAt({0.5f + 0.05f * static_cast<float>(i), 0.0f, 1.0f});
        EXPECT_LE(s, last + 1e-5f) << i;
        EXPECT_LE(s, 2.0f + 1e-4f);
        EXPECT_GE(s, 0.5f - 1e-4f);
        last = s;
    }
    // No keys: x1; one key: its scale everywhere.
    EXPECT_EQ(LengthField(LengthKeySet{}, plane.get()).scaleAt({1.0f, 0.0f, 1.0f}), 1.0f);
    LengthKeySet one;
    one.keys = {key({0.5f, 0.0f, 1.0f}, 1.5f, plane.get())};
    EXPECT_NEAR(LengthField(one, plane.get()).scaleAt({2.0f, 0.0f, 0.0f}), 1.5f, 1e-5f);
}

TEST(LengthField, FarFromTheKeysTheScaleEasesBackToOne) {
    const auto plane = test::makePlane(40, 10.0f);
    LengthKeySet set;
    set.keys = {key({1.0f, 0.0f, 1.0f}, 2.0f, plane.get()), key({2.0f, 0.0f, 1.0f}, 2.0f, plane.get())};
    const LengthField field(set, plane.get());
    EXPECT_NEAR(field.scaleAt({1.5f, 0.0f, 1.0f}), 2.0f, 1e-4f);  // within the spacing: the keys'
    const float easing = field.scaleAt({1.5f, 0.0f, 2.6f});        // 1.5 from the nearest: easing back
    EXPECT_GT(easing, 1.05f);
    EXPECT_LT(easing, 1.95f);
    EXPECT_EQ(field.scaleAt({8.0f, 0.0f, 8.0f}), 1.0f);  // past twice the spacing: as it is
}

TEST(LengthField, DistancesRunAlongTheSkin) {
    // An ear: two faces 0.05 apart. A key at x2 low on the front face, a hold high on it. A point
    // low on the back face is a straight 0.05 from the x2 key but round the top along the skin:
    // nearer the hold, so barely scaled (straight lines would give it nearly x2).
    const float r = 0.025f;
    const auto ear = test::makeFin(1.0f, 2.0f * r, 0.5f, 0.02f);
    LengthKeySet set;
    set.keys = {key({0.25f, 0.1f, -r}, 2.0f, ear.get()), key({0.25f, 0.9f, -r}, 1.0f, ear.get())};
    const LengthField field(set, ear.get());
    EXPECT_NEAR(field.scaleAt({0.25f, 0.12f, -r}), 2.0f, 0.05f);
    EXPECT_LT(field.scaleAt({0.25f, 0.1f, r}), 1.3f);
    // Without a scalp: straight lines.
    EXPECT_GT(LengthField(set, nullptr).scaleAt({0.25f, 0.1f, r}), 1.8f);
}

TEST(LengthField, AGuideScalesOrTrimsToItsNewLength) {
    // A bent guide: scaled keeps its shape (every point about the root), trimmed keeps its start.
    const std::vector<Vec3f> bent = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 2, 0}};
    const std::vector<Vec3f> scaled = guideAtLength(bent, 6.0f, LengthChange::Scale);
    EXPECT_NEAR(arcLength(scaled), 6.0f, 1e-4f);
    EXPECT_NEAR(scaled[2].x, 2.0f, 1e-5f);
    EXPECT_NEAR(scaled[2].y, 2.0f, 1e-5f);
    const std::vector<Vec3f> shorter = guideAtLength(bent, 1.5f, LengthChange::Trim);
    ASSERT_EQ(shorter.size(), bent.size());
    EXPECT_NEAR(arcLength(shorter), 1.5f, 1e-4f);
    EXPECT_GT(shorter.back().x, 0.0f);  // cut along the second segment
    EXPECT_LT(shorter.back().x, 1.0f);
    const std::vector<Vec3f> longer = guideAtLength(bent, 4.0f, LengthChange::Trim);
    EXPECT_NEAR(arcLength(longer), 4.0f, 1e-3f);
    EXPECT_GT(longer.back().y, 2.5f);  // on up its last direction
    EXPECT_EQ(guideAtLength(bent, 0.0f, LengthChange::Scale), bent);
    EXPECT_EQ(lengthChangeFromName("trim"), LengthChange::Trim);
    EXPECT_THROW(lengthChangeFromName("stretch"), std::invalid_argument);
}

TEST(LengthField, AppliedToGuides) {
    const auto plane = test::makePlane(20, 2.0f);
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    std::vector<GuideId> ids;
    for (int i = 0; i <= 10; ++i) ids.push_back(h.addGuide(group, standing(0.2f * static_cast<float>(i), 1.0f, 1.0f + 0.1f * static_cast<float>(i))));
    EXPECT_NEAR(nearestGuideLength(h, ids, {1.05f, 0.0f, 1.0f}), 1.5f, 1e-5f);
    // x2 at one end, a hold at the other: each guide scaled from x2 down to x1, its root kept.
    LengthKeySet set;
    set.keys = {key({0.0f, 0.0f, 1.0f}, 2.0f, plane.get()), key({2.0f, 0.0f, 1.0f}, 1.0f, plane.get())};
    const std::vector<LengthResult> results = applyLengthField(h, ids, LengthField(set, plane.get()), LengthChange::Scale);
    ASSERT_EQ(results.size(), ids.size() - 1);  // the last guide, on the hold, unchanged
    float last = 3.0f;
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].id, ids[i]);
        const float scale = arcLength(results[i].points) / arcLength(h.guide(ids[i])->points);
        EXPECT_LT(scale, last) << i;
        last = scale;
        EXPECT_EQ(results[i].points[0], h.guide(ids[i])->points[0]);
    }
    EXPECT_NEAR(arcLength(results[0].points), 2.0f, 0.02f);
}

TEST(LengthField, TopologySymmetryFollowsAPosedScalp) {
    // A sphere at rest, posed turned 40 degrees about y (a head turned): topology symmetry puts a
    // key's mirror at its partner spot on the turned sphere (the turn of the rest pose's mirror
    // image), where the world plane's image would not be.
    const auto rest = test::makeSphere(24, 48, 1.0f);
    const float a = 40.0f * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    auto turn = [&](const Vec3f& p) { return Vec3f(c * p.x + s * p.z, p.y, -s * p.x + c * p.z); };
    std::vector<Vec3f> points;
    for (const Vec3f& p : rest->points()) points.push_back(turn(p));
    auto posed = std::make_shared<ScalpMesh>(rest->withPoints(std::move(points)));
    posed->setRestPoints(rest->points());
    BrushSettings b;
    b.mirrorTopology = true;
    const StrokeSymmetry sym = StrokeSymmetry::of(b, posed.get());
    ASSERT_TRUE(sym.topology());
    const Vec3f onRest = Vec3f(0.5f, 0.6f, 0.62f).normalized();
    LengthKeySet set;
    set.keys = {key(turn(onRest), 2.0f)};
    const LengthKeySet mirrored = mirroredLengthKeys(set, posed.get(), [&sym](const Vec3f& p) { return sym.point(p); });
    const Vec3f want = turn(Vec3f(-onRest.x, onRest.y, onRest.z));
    EXPECT_LT((mirrored.keys[0].point - want).length(), 0.03f);
    const Vec3f plane(-set.keys[0].point.x, set.keys[0].point.y, set.keys[0].point.z);
    EXPECT_GT((plane - want).length(), 0.3f) << "the world plane's image misses on a posed scalp";
}

TEST(LengthField, MirroredAndSavedWithTheGuides) {
    const auto plane = test::makePlane(4, 2.0f);  // x and z from 0 to 2
    LengthKeySet set;
    set.keys = {key({-1.0f, 0.0f, 0.5f}, 3.0f)};
    const LengthKeySet mirrored = mirroredLengthKeys(set, plane.get(), [](const Vec3f& p) { return Vec3f(-p.x, p.y, p.z); });
    EXPECT_NEAR(mirrored.keys[0].point.x, 1.0f, 1e-5f);
    EXPECT_GE(mirrored.keys[0].root.face, 0);
    EXPECT_EQ(mirrored.keys[0].scale, 3.0f);
    EXPECT_TRUE(mirrored.scaled());

    GuideHierarchy h;
    h.addGuide(h.addGroup("primary"), standing(0.0f, 0.0f, 1.0f));
    const std::uint64_t before = h.contentHash();
    const std::string plain = h.toJson();
    h.setLengthKeys(mirrored);
    EXPECT_NE(h.contentHash(), before);
    const GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    ASSERT_EQ(back.lengthKeys().keys.size(), 1u);
    EXPECT_EQ(back.lengthKeys().toJson(), mirrored.toJson());
    EXPECT_EQ(back.contentHash(), h.contentHash());
    h.setLengthKeys({});
    EXPECT_EQ(h.toJson(), plain);  // no keys: the text as it always was
    EXPECT_EQ(h.contentHash(), before);
    EXPECT_THROW(LengthKeySet::fromJson("{}"), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
