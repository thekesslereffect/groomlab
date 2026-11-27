// SPDX-License-Identifier: Apache-2.0
// Lock pieces joined end to end in 3D (guide_merge.h).

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/guide_merge.h"

using namespace groomlab;

namespace {

// A straight piece from `a` to `b`, `n` points.
std::vector<Vec3f> line(const Vec3f& a, const Vec3f& b, int n = 8) {
    std::vector<Vec3f> p;
    for (int i = 0; i < n; ++i) p.push_back(a + (b - a) * (static_cast<float>(i) / static_cast<float>(n - 1)));
    return p;
}

PieceMergeOptions lockScale() {
    PieceMergeOptions o;
    o.distanceStart = 0.1f;
    o.distanceEnd = 0.3f;
    o.angleStart = 20.0f;
    o.angleEnd = 40.0f;
    o.levels = 4;
    return o;
}

}  // namespace

// Two pieces of one straight lock with a small gap: one chain from the first's start to the
// second's end, the gap's ends fused at their midpoint.
TEST(GuideMerge, CollinearPiecesJoin) {
    const std::vector<std::vector<Vec3f>> pieces = {line({0, 0, 0}, {1, 0, 0}), line({1.05f, 0, 0}, {2, 0, 0})};
    const PieceMergeResult r = mergePieces(pieces, lockScale());
    ASSERT_EQ(r.chains.size(), 1u);
    EXPECT_EQ(r.merges, 1);
    EXPECT_EQ(r.mergesPerPass.front(), 1);  // at the first, tightest level
    const std::vector<Vec3f>& c = r.chains.front();
    EXPECT_EQ(c.size(), 15u);
    EXPECT_NEAR(c.front().x, 0.0f, 1e-5f);
    EXPECT_NEAR(c.back().x, 2.0f, 1e-5f);
    EXPECT_NEAR(c[7].x, 1.025f, 1e-5f);
    ASSERT_EQ(r.members.front().size(), 2u);
    EXPECT_EQ(r.members.front()[0], 0);
    EXPECT_EQ(r.members.front()[1], 1);
}

// Pieces given either way round still join (each end's inward tangent decides, not the order).
TEST(GuideMerge, PiecesJoinWhicheverWayTheyRun) {
    const std::vector<std::vector<Vec3f>> pieces = {line({1, 0, 0}, {0, 0, 0}), line({2, 0, 0}, {1.05f, 0, 0})};
    const PieceMergeResult r = mergePieces(pieces, lockScale());
    ASSERT_EQ(r.chains.size(), 1u);
    EXPECT_NEAR(std::abs(r.chains.front().front().x - r.chains.front().back().x), 2.0f, 1e-5f);
}

// Two pieces meeting tip to tip and folding back (a hairpin: the second runs back alongside the
// first) are two locks: the signed continuation test rejects them at every level.
TEST(GuideMerge, AHairpinDoesNotJoin) {
    const std::vector<std::vector<Vec3f>> pieces = {line({0, 0, 0}, {1, 0, 0}), line({1.0f, 0.05f, 0}, {0, 0.05f, 0})};
    const PieceMergeResult r = mergePieces(pieces, lockScale());
    EXPECT_EQ(r.chains.size(), 2u);
    EXPECT_EQ(r.merges, 0);
}

// Neighbouring locks side by side (the second's start beside the first's end, not ahead of it)
// do not join either: the gap runs across the lock.
TEST(GuideMerge, SideBySidePiecesDoNotJoin) {
    const std::vector<std::vector<Vec3f>> pieces = {line({0, 0, 0}, {1, 0, 0}), line({0.95f, 0.2f, 0}, {2, 0.2f, 0})};
    const PieceMergeResult r = mergePieces(pieces, lockScale());
    EXPECT_EQ(r.merges, 0);
}

// A pair farther apart than the first level's radius, and turning more than its angle, joins only
// once the thresholds have relaxed.
TEST(GuideMerge, AFarPairJoinsOnlyAtARelaxedPass) {
    // (a 0.22 gap; the second piece turned 30 degrees)
    const float c = std::cos(30.0f * 3.14159265f / 180.0f), s = std::sin(30.0f * 3.14159265f / 180.0f);
    const Vec3f start(1.22f, 0.0f, 0.0f);
    const std::vector<std::vector<Vec3f>> pieces = {line({0, 0, 0}, {1, 0, 0}), line(start, start + Vec3f(c, s, 0.0f))};
    PieceMergeOptions o = lockScale();
    o.tangentLength = 0.3f;
    const PieceMergeResult r = mergePieces(pieces, o);
    ASSERT_EQ(r.chains.size(), 1u);
    ASSERT_GE(r.mergesPerPass.size(), 2u);
    EXPECT_EQ(r.mergesPerPass.front(), 0);
    EXPECT_GT(r.passDistance.back(), r.passDistance.front());
    EXPECT_GT(r.passAngle.back(), r.passAngle.front());
    // (the pass it joined at allowed both the gap and the turn)
    for (std::size_t k = 0; k < r.mergesPerPass.size(); ++k)
        if (r.mergesPerPass[k] > 0) {
            EXPECT_GE(r.passDistance[k], 0.22f);
            EXPECT_GE(r.passAngle[k], 30.0f);
        }
    // (with the first level's thresholds alone it stays two)
    o.distanceEnd = o.distanceStart;
    o.angleEnd = o.angleStart;
    EXPECT_EQ(mergePieces(pieces, o).chains.size(), 2u);
}

// A chain of several pieces joins over passes, one join per chain a pass (no loops).
TEST(GuideMerge, ManyPiecesBecomeOneChain) {
    std::vector<std::vector<Vec3f>> pieces;
    for (int k = 0; k < 6; ++k) pieces.push_back(line({1.05f * static_cast<float>(k), 0, 0}, {1.05f * static_cast<float>(k) + 1.0f, 0, 0}));
    std::swap(pieces[1], pieces[4]);
    const PieceMergeResult r = mergePieces(pieces, lockScale());
    ASSERT_EQ(r.chains.size(), 1u);
    EXPECT_EQ(r.merges, 5);
    EXPECT_EQ(r.members.front().size(), 6u);
}

// The same lock lifted from two photos (the second a little off and shorter) is one: the shorter
// repeats the longer and is dropped; a neighbouring lock farther off is kept.
TEST(GuideMerge, ADuplicateIsDropped) {
    const std::vector<std::vector<Vec3f>> pieces = {line({0, 0, 0}, {1, 0, 0}, 20), line({0.1f, 0.02f, 0.01f}, {0.9f, 0.03f, 0.0f}, 16),
                                                    line({0, 0.2f, 0}, {1, 0.2f, 0}, 20),
                                                    // (the same lock traced the other way: still a repeat)
                                                    line({0.8f, 0.01f, 0}, {0.2f, 0.0f, 0.01f}, 10)};
    DuplicatePieceOptions o;
    o.distance = 0.06f;
    const std::vector<int> of = duplicatePieces(pieces, o);
    ASSERT_EQ(of.size(), 4u);
    EXPECT_EQ(of[0], -1);
    EXPECT_EQ(of[1], 0);
    EXPECT_EQ(of[2], -1);
    EXPECT_EQ(of[3], 0);
}
