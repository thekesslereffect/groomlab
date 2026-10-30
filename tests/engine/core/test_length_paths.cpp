// SPDX-License-Identifier: Apache-2.0
// Length paths (length_paths.h): lengths along a path of keys, the fade at its reach, several
// paths blending, a guide at a new length (scaled or trimmed), defaults from the guides near a
// key, mirroring, and the paths saved with the guides.

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/length_paths.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

LengthKey key(float x, float z, float length) {
    LengthKey k;
    k.point = {x, 0.0f, z};
    k.length = length;
    return k;
}

// Every key of the path reaching `reach`.
void reachAll(LengthPath& path, float reach) {
    for (LengthKey& k : path.keys) k.reach = reach;
}

// A straight guide of `points` points standing up at (x, 0, z), `length` long.
std::vector<Vec3f> standing(float x, float z, float length, int points = 5) {
    std::vector<Vec3f> out;
    for (int i = 0; i < points; ++i) out.push_back({x, length * static_cast<float>(i) / static_cast<float>(points - 1), z});
    return out;
}

TEST(LengthPaths, KeysBlendAlongThePathAndFadeAtItsReach) {
    LengthPath path;
    path.keys = {key(0.0f, 0.0f, 1.0f), key(1.0f, 0.0f, 3.0f)};
    reachAll(path, 0.5f);
    path.smooth = false;
    // On the path: straight between the keys.
    EXPECT_NEAR(sampleLengthPath(path, {0.0f, 0.0f, 0.0f}).length, 1.0f, 1e-5f);
    EXPECT_NEAR(sampleLengthPath(path, {0.5f, 0.0f, 0.0f}).length, 2.0f, 1e-5f);
    EXPECT_NEAR(sampleLengthPath(path, {1.0f, 0.0f, 0.0f}).length, 3.0f, 1e-5f);
    EXPECT_NEAR(sampleLengthPath(path, {0.5f, 0.0f, 0.0f}).weight, 1.0f, 1e-6f);
    // Off to the side: the nearest point's length, full within 60% of the reach, easing to 0 at it.
    EXPECT_NEAR(sampleLengthPath(path, {0.25f, 0.0f, 0.25f}).length, 1.5f, 1e-5f);
    EXPECT_NEAR(sampleLengthPath(path, {0.25f, 0.0f, 0.25f}).weight, 1.0f, 1e-6f);
    const float fading = sampleLengthPath(path, {0.25f, 0.0f, 0.4f}).weight;
    EXPECT_GT(fading, 0.0f);
    EXPECT_LT(fading, 1.0f);
    EXPECT_EQ(sampleLengthPath(path, {0.25f, 0.0f, 0.6f}).weight, 0.0f);
    // Smooth: the same at the keys, eased between them.
    path.smooth = true;
    EXPECT_NEAR(sampleLengthPath(path, {0.0f, 0.0f, 0.0f}).length, 1.0f, 1e-5f);
    EXPECT_NEAR(sampleLengthPath(path, {0.5f, 0.0f, 0.0f}).length, 2.0f, 1e-5f);
    EXPECT_LT(sampleLengthPath(path, {0.1f, 0.0f, 0.0f}).length, 1.2f);
}

TEST(LengthPaths, EachKeyHasItsOwnReachBlendedBetweenKeys) {
    // Wide at the first key, narrow at the second (a leg: thick at the top, thin at the paw).
    LengthPath path;
    path.keys = {key(0.0f, 0.0f, 2.0f), key(2.0f, 0.0f, 2.0f)};
    path.keys[0].reach = 1.0f;
    path.keys[1].reach = 0.2f;
    EXPECT_NEAR(sampleLengthPath(path, {0.1f, 0.0f, 0.5f}).weight, 1.0f, 1e-6f);  // well inside the wide end
    EXPECT_EQ(sampleLengthPath(path, {1.9f, 0.0f, 0.5f}).weight, 0.0f);           // past the narrow end's
    const float middle = sampleLengthPath(path, {1.0f, 0.0f, 0.5f}).weight;       // reach 0.6 there: fading
    EXPECT_GT(middle, 0.0f);
    EXPECT_LT(middle, 1.0f);
}

TEST(LengthPaths, ASmoothPathNeverOvershootsAKey) {
    // Long, short, long (a V fringe): the middle key is the shortest anywhere along the path.
    LengthPath path;
    path.keys = {key(0.0f, 0.0f, 4.0f), key(1.0f, 0.0f, 1.0f), key(2.0f, 0.0f, 4.0f), key(3.0f, 0.0f, 4.5f)};
    reachAll(path, 0.5f);
    float least = 1e9f, most = 0.0f;
    for (int i = 0; i <= 300; ++i) {
        const float l = sampleLengthPath(path, {0.01f * static_cast<float>(i), 0.0f, 0.0f}).length;
        least = std::min(least, l);
        most = std::max(most, l);
    }
    EXPECT_NEAR(least, 1.0f, 1e-4f);
    EXPECT_NEAR(most, 4.5f, 1e-4f);
    EXPECT_NEAR(sampleLengthPath(path, {1.0f, 0.0f, 0.0f}).length, 1.0f, 1e-5f);
}

TEST(LengthPaths, OneKeyIsAPointAndPathsBlendByNearness) {
    LengthPathSet set;
    LengthPath a, b;
    a.keys = {key(0.0f, 0.0f, 2.0f)};
    reachAll(a, 1.0f);
    b.keys = {key(1.0f, 0.0f, 4.0f)};
    reachAll(b, 1.0f);
    set.paths = {a, b};
    EXPECT_NEAR(sampleLengthPaths(set, {0.0f, 0.0f, 0.0f}).length, 2.0f, 1e-5f);  // only a reaches fully
    EXPECT_NEAR(sampleLengthPaths(set, {0.5f, 0.0f, 0.0f}).length, 3.0f, 1e-5f);  // halfway: both alike
    EXPECT_EQ(sampleLengthPaths(set, {5.0f, 0.0f, 0.0f}).weight, 0.0f);
    EXPECT_TRUE(LengthPathSet{}.empty());
    EXPECT_FALSE(set.empty());
}

TEST(LengthPaths, AGuideScalesOrTrimsToItsNewLength) {
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

TEST(LengthPaths, AppliedToGuidesAndANewKeysDefault) {
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    std::vector<GuideId> ids;
    for (int i = 0; i <= 10; ++i) ids.push_back(h.addGuide(group, standing(0.1f * static_cast<float>(i), 0.0f, 1.0f + 0.1f * static_cast<float>(i))));
    // The default: the lengths around the key, the nearer counting more.
    const float around = averageGuideLength(h, ids, {0.5f, 0.0f, 0.0f}, 0.25f);
    EXPECT_NEAR(around, 1.5f, 0.02f);
    EXPECT_NEAR(averageGuideLength(h, ids, {5.0f, 0.0f, 0.0f}, 0.25f), 2.0f, 1e-5f);  // none within: the nearest
    // A ramp from 2 to 4 over the row: each guide its length there; a guide past the reach left out.
    LengthPathSet set;
    LengthPath path;
    path.keys = {key(0.0f, 0.0f, 2.0f), key(1.0f, 0.0f, 4.0f)};
    reachAll(path, 0.3f);
    path.smooth = false;
    set.paths = {path};
    const GuideId far = h.addGuide(group, standing(0.5f, 2.0f, 1.0f));
    std::vector<GuideId> all = ids;
    all.push_back(far);
    const std::vector<LengthResult> results = applyLengthPaths(h, all, set, LengthChange::Scale);
    ASSERT_EQ(results.size(), ids.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].id, ids[i]);
        EXPECT_NEAR(arcLength(results[i].points), 2.0f + 0.2f * static_cast<float>(i), 1e-4f) << i;
        EXPECT_EQ(results[i].points[0], h.guide(ids[i])->points[0]);  // the root kept
    }
    // Applying twice gives what applying once did (the keys hold lengths, not shares).
    for (const LengthResult& r : results) h.setPoints(r.id, r.points);
    EXPECT_TRUE(applyLengthPaths(h, all, set, LengthChange::Scale).empty());
}

TEST(LengthPaths, TopologySymmetryFollowsAPosedScalp) {
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
    LengthPathSet set;
    LengthPath path;
    path.keys = {key(0.0f, 0.0f, 2.0f)};
    path.keys[0].point = turn(onRest);
    set.paths = {path};
    const LengthPathSet mirrored = mirroredLengthPaths(set, posed.get(), [&sym](const Vec3f& p) { return sym.point(p); });
    const Vec3f want = turn(Vec3f(-onRest.x, onRest.y, onRest.z));
    EXPECT_LT((mirrored.paths[0].keys[0].point - want).length(), 0.03f);
    const Vec3f plane(-path.keys[0].point.x, path.keys[0].point.y, path.keys[0].point.z);
    EXPECT_GT((plane - want).length(), 0.3f) << "the world plane's image misses on a posed scalp";
}

TEST(LengthPaths, MirroredAndSavedWithTheGuides) {
    const auto plane = test::makePlane(4, 2.0f);  // x and z from 0 to 2
    LengthPathSet set;
    LengthPath path;
    path.keys = {key(-1.0f, 0.5f, 3.0f)};
    reachAll(path, 0.4f);
    set.paths = {path};
    const LengthPathSet mirrored = mirroredLengthPaths(set, plane.get(), [](const Vec3f& p) { return Vec3f(-p.x, p.y, p.z); });
    EXPECT_NEAR(mirrored.paths[0].keys[0].point.x, 1.0f, 1e-5f);
    EXPECT_GE(mirrored.paths[0].keys[0].root.face, 0);
    EXPECT_EQ(mirrored.paths[0].keys[0].length, 3.0f);

    GuideHierarchy h;
    h.addGuide(h.addGroup("primary"), standing(0.0f, 0.0f, 1.0f));
    const std::uint64_t before = h.contentHash();
    const std::string plain = h.toJson();
    h.setLengthPaths(mirrored);
    EXPECT_NE(h.contentHash(), before);
    const GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    ASSERT_EQ(back.lengthPaths().paths.size(), 1u);
    EXPECT_EQ(back.lengthPaths().toJson(), mirrored.toJson());
    EXPECT_EQ(back.contentHash(), h.contentHash());
    h.setLengthPaths({});
    EXPECT_EQ(h.toJson(), plain);  // no paths: the text as it always was
    EXPECT_EQ(h.contentHash(), before);
    EXPECT_THROW(LengthPathSet::fromJson("{}"), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
