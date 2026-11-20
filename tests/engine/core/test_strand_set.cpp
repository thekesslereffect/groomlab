// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/process_caches.h"
#include "groomlab/strand_set.h"

namespace groomlab {
namespace {

std::vector<Vec3f> line(float x, int points) {
    std::vector<Vec3f> pts;
    for (int i = 0; i < points; ++i) pts.emplace_back(x, static_cast<float>(i), 0.0f);
    return pts;
}

TEST(StrandSet, AddStrandTracksOffsets) {
    StrandSet s;
    s.addStrand(10, line(0, 3));
    s.addStrand(11, line(1, 5));

    EXPECT_EQ(s.strandCount(), 2u);
    EXPECT_EQ(s.pointCount(), 8u);
    EXPECT_EQ(s.strandPointCount(0), 3u);
    EXPECT_EQ(s.strandPointCount(1), 5u);
    EXPECT_EQ(s.strandPoints(1)[0], Vec3f(1, 0, 0));
    EXPECT_EQ(s.widths().size(), 8u);
    EXPECT_TRUE(s.validate());
}

TEST(StrandSet, RejectsMismatchedWidths) {
    StrandSet s;
    EXPECT_THROW(s.addStrand(1, line(0, 3), {1.0f, 2.0f}), std::invalid_argument);
}

TEST(StrandSet, ChannelsExtendWithNewStrands) {
    StrandSet s;
    s.addStrand(1, line(0, 2));
    s.addStrandChannel("melanin", 0.5f);
    s.addPointChannel("u", 0.0f);
    s.addStrand(2, line(1, 3));

    ASSERT_NE(s.strandChannel("melanin"), nullptr);
    EXPECT_EQ(s.strandChannel("melanin")->size(), 2u);
    EXPECT_FLOAT_EQ((*s.strandChannel("melanin"))[1], 0.5f);
    EXPECT_EQ(s.pointChannel("u")->size(), 5u);
    EXPECT_EQ(s.strandChannel("missing"), nullptr);
    EXPECT_TRUE(s.validate());
}

TEST(StrandSet, WithDensityKeepsStableSubset) {
    StrandSet s;
    for (StrandId id = 0; id < 2000; ++id) s.addStrand(id, line(static_cast<float>(id), 4));
    s.addStrandChannel("tag", 1.0f);

    const StrandSet low = s.withDensity(0.1f);
    const StrandSet high = s.withDensity(0.5f);
    EXPECT_TRUE(low.validate());
    EXPECT_TRUE(high.validate());
    EXPECT_LT(low.strandCount(), high.strandCount());
    ASSERT_NE(low.strandChannel("tag"), nullptr);
    EXPECT_EQ(low.strandChannel("tag")->size(), low.strandCount());

    // Every strand in the low-density set is present, unchanged, at high density.
    std::size_t j = 0;
    for (std::size_t i = 0; i < low.strandCount(); ++i) {
        while (j < high.strandCount() && high.ids()[j] != low.ids()[i]) ++j;
        ASSERT_LT(j, high.strandCount());
        EXPECT_EQ(high.strandPoints(j)[0], low.strandPoints(i)[0]);
    }
}

TEST(StrandSet, AppendFillsAMissingPointChannelFromTheSidesStrandChannel) {
    // `tipped` has the channel per point; `flat` only per strand (2 strands of 2 and 3 points);
    // `bare` not at all.
    StrandSet tipped;
    tipped.addStrand(1, line(0, 4));
    tipped.addStrandChannel("tone", 0.25f) = {0.3f};
    tipped.addPointChannel("tone", 0.25f) = {0.1f, 0.2f, 0.3f, 0.4f};
    tipped.addPointChannel("u", 0.5f) = {0.0f, 0.3f, 0.6f, 1.0f};
    StrandSet flat;
    flat.addStrand(2, line(1, 2));
    flat.addStrand(3, line(2, 3));
    flat.addStrandChannel("tone", 0.25f) = {0.7f, 0.9f};
    StrandSet bare;
    bare.addStrand(4, line(3, 2));

    // Appended after: each of flat's strands along its points.
    StrandSet after = tipped;
    after.append(flat);
    ASSERT_TRUE(after.validate());
    EXPECT_EQ(*after.pointChannel("tone"), (std::vector<float>{0.1f, 0.2f, 0.3f, 0.4f, 0.7f, 0.7f, 0.9f, 0.9f, 0.9f}));
    EXPECT_EQ(*after.strandChannel("tone"), (std::vector<float>{0.3f, 0.7f, 0.9f}));
    // A point channel without a strand channel of its name: the default, as before.
    EXPECT_EQ(*after.pointChannel("u"), (std::vector<float>{0.0f, 0.3f, 0.6f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}));

    // Appended before: the same for this set's own strands.
    StrandSet before = flat;
    before.append(tipped);
    ASSERT_TRUE(before.validate());
    EXPECT_EQ(*before.pointChannel("tone"), (std::vector<float>{0.7f, 0.7f, 0.9f, 0.9f, 0.9f, 0.1f, 0.2f, 0.3f, 0.4f}));
    EXPECT_FLOAT_EQ(before.pointChannelDefault("tone"), 0.25f);
    EXPECT_EQ(*before.pointChannel("u"), (std::vector<float>{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.3f, 0.6f, 1.0f}));
    EXPECT_EQ(flat.pointChannel("tone"), nullptr);  // the appended set is left alone
    EXPECT_EQ(flat.pointChannel("u"), nullptr);

    // A side with neither: the default, in either order.
    StrandSet withBare = tipped;
    withBare.append(bare);
    EXPECT_EQ(*withBare.pointChannel("tone"), (std::vector<float>{0.1f, 0.2f, 0.3f, 0.4f, 0.25f, 0.25f}));
    EXPECT_EQ(*withBare.strandChannel("tone"), (std::vector<float>{0.3f, 0.25f}));
    StrandSet bareFirst = bare;
    bareFirst.append(tipped);
    EXPECT_EQ(*bareFirst.pointChannel("tone"), (std::vector<float>{0.25f, 0.25f, 0.1f, 0.2f, 0.3f, 0.4f}));

    // Three in a row: the filled values stay where they are.
    StrandSet chain = flat;
    chain.append(tipped);
    chain.append(flat);
    EXPECT_EQ(*chain.pointChannel("tone"),
              (std::vector<float>{0.7f, 0.7f, 0.9f, 0.9f, 0.9f, 0.1f, 0.2f, 0.3f, 0.4f, 0.7f, 0.7f, 0.9f, 0.9f, 0.9f}));
}

TEST(StrandSet, ContentHashChangesWithData) {
    StrandSet a;
    a.addStrand(1, line(0, 3));
    StrandSet b = a;
    EXPECT_EQ(a.contentHash(), b.contentHash());
    b.mutablePositions()[1].z = 0.001f;
    EXPECT_NE(a.contentHash(), b.contentHash());
}

TEST(StrandSet, ParallelEditIsDeterministic) {
    StrandSet a;
    for (StrandId id = 0; id < 5000; ++id) a.addStrand(id, line(0, 8));
    StrandSet b = a;

    auto lift = [](StrandSet& s) {
        parallelFor(0, s.strandCount(), [&s](std::size_t i) {
            Vec3f* p = s.strandPoints(i);
            for (std::size_t k = 0; k < s.strandPointCount(i); ++k) p[k].z += static_cast<float>(s.ids()[i] % 7);
        });
    };
    lift(a);
    lift(b);
    EXPECT_EQ(a.contentHash(), b.contentHash());
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

TEST(StrandSet, CopiesShareArraysUntilWritten) {
    StrandSet a;
    a.addStrand(1, {Vec3f(0, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 2, 0)});
    a.addStrandChannel("mask", 0.5f);
    StrandSet b = a;
    // Reading through const accessors shares the storage.
    EXPECT_EQ(a.positions().data(), b.positions().data());
    EXPECT_EQ(a.ids().data(), b.ids().data());
    EXPECT_EQ(a.strandChannel("mask")->data(), b.strandChannel("mask")->data());
    // Writing points clones only the points.
    b.strandPoints(0)[1].x = 5.0f;
    EXPECT_NE(a.positions().data(), b.positions().data());
    EXPECT_EQ(a.ids().data(), b.ids().data());
    EXPECT_EQ(a.widths().data(), b.widths().data());
    EXPECT_EQ(a.strandChannel("mask")->data(), b.strandChannel("mask")->data());
    EXPECT_FLOAT_EQ(a.positions()[1].x, 0.0f);
    EXPECT_FLOAT_EQ(b.positions()[1].x, 5.0f);
    // The source writes into its own copy too (the array was shared).
    a.mutableWidths()[0] = 2.0f;
    EXPECT_FLOAT_EQ(b.widths()[0], 1.0f);
    // A new channel on the copy leaves the original's channels alone.
    b.addStrandChannel("other", 1.0f);
    EXPECT_EQ(a.strandChannel("other"), nullptr);
    EXPECT_TRUE(a.validate());
    EXPECT_TRUE(b.validate());
}

TEST(StrandSet, ArraysAreOwnAgainOnceTheCopiesSharingThemDie) {
    // A copy marks the array shared on both sides; when the copy dies, the original writes in
    // place again (no clone of an array nobody else holds), through mutable* and overwrite*.
    StrandSet a;
    a.addStrand(1, {Vec3f(0, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 2, 0)});
    const Vec3f* points = a.positions().data();
    const float* widths = a.widths().data();
    {
        StrandSet b = a;
        EXPECT_EQ(b.positions().data(), points);
    }
    EXPECT_EQ(a.mutablePositions().data(), points);
    EXPECT_EQ(a.overwritePositions().data(), points);
    EXPECT_EQ(a.mutableWidths().data(), widths);
    a.mutablePositions()[1].x = 3.0f;
    EXPECT_FLOAT_EQ(a.positions()[1].x, 3.0f);
    // The same for an assignment, both ways round, and while a copy lives, the clone as before.
    StrandSet c;
    c = a;
    EXPECT_NE(a.mutablePositions().data(), points);
    const Vec3f* cloned = a.positions().data();
    EXPECT_EQ(c.positions().data(), points);
    EXPECT_EQ(c.mutablePositions().data(), points);  // a cloned away: c alone holds the original
    EXPECT_EQ(a.mutablePositions().data(), cloned);
    EXPECT_TRUE(a.validate());
    EXPECT_TRUE(c.validate());
}

TEST(StrandSet, TakingPositionsFromItselfKeepsThem) {
    StrandSet s;
    s.addStrand(1, {Vec3f(1, 0, 0), Vec3f(1, 1, 0)});
    s.addStrand(2, {Vec3f(2, 0, 0), Vec3f(2, 1, 0)}, {0.5f, 0.25f});
    const Vec3f* points = s.positions().data();
    const float* widths = s.widths().data();
    EXPECT_EQ(s.takePositionsFrom(s).data(), points);
    EXPECT_EQ(s.takeWidthsFrom(s).data(), widths);
    EXPECT_EQ(s.pointCount(), 4u);
    EXPECT_EQ(s.positions()[2], Vec3f(2, 0, 0));
    EXPECT_FLOAT_EQ(s.widths()[3], 0.25f);
    EXPECT_TRUE(s.validate());
    // Shared with a copy: its own copy, the copy's left alone.
    StrandSet copy = s;
    EXPECT_NE(s.takePositionsFrom(s).data(), points);
    EXPECT_EQ(copy.positions().data(), points);
    EXPECT_EQ(s.positions(), copy.positions());
}

TEST(StrandSet, ClonesOntoRecycledArraysHoldTheirOwnContents) {
    // Arrays large enough for the pool: a node's clone, freed, then taken by the next clone of
    // the same size, which must hold its source's points, not the stale ones.
    constexpr std::size_t kStrands = 40000, kPoints = 16;  // 7.7 MB of positions
    StrandSet base;
    base.resizeUniform(kStrands, kPoints);
    std::vector<Vec3f>& pts = base.mutablePositions();
    for (std::size_t i = 0; i < pts.size(); ++i) pts[i] = Vec3f(static_cast<float>(i), 1.0f, 2.0f);
    const std::uint64_t baseHash = base.contentHash();
    for (int round = 0; round < 4; ++round) {
        auto edited = std::make_unique<StrandSet>(base);
        std::vector<Vec3f>& p = edited->mutablePositions();
        ASSERT_NE(p.data(), base.positions().data());
        ASSERT_EQ(p, base.positions()) << "round " << round;  // the clone has the source's points
        for (Vec3f& q : p) q.y = static_cast<float>(round + 5);
        StrandSet again(*edited);
        edited.reset();  // the copy keeps the array: nothing goes back yet
        EXPECT_FLOAT_EQ(again.positions()[7].y, static_cast<float>(round + 5));
    }
    EXPECT_EQ(base.contentHash(), baseHash);  // the source untouched throughout
}

TEST(StrandSet, ResizeFillsRecycledArraysAndLeavesWhatItIsTold) {
    // Arrays large enough for the pool, left with stale contents: a resize takes them back and
    // fills every element (in parallel blocks) with the defaults, except the arrays a caller
    // says it writes itself.
    constexpr std::size_t kStrands = 120000, kPoints = 16;  // 23 MB of positions, 7.7 MB of widths
    std::vector<std::size_t> counts(kStrands, kPoints);
    for (std::size_t i = 0; i < kStrands; i += 7) counts[i] = 3;
    for (int round = 0; round < 2; ++round) {
        {
            StrandSet stale;
            if (round == 0) {
                stale.resizeUniform(kStrands, kPoints);
            } else {
                stale.resize(counts);
            }
            for (Vec3f& p : stale.mutablePositions()) p = Vec3f(7.0f, 8.0f, 9.0f);
            for (float& w : stale.mutableWidths()) w = 0.125f;
            for (float& c : stale.addPointChannel("tint", 0.0f)) c = -3.0f;
            for (StrandId& id : stale.mutableIds()) id = 77;
        }  // its arrays go to the pool
        StrandSet s;
        s.addPointChannel("tint", 2.0f);
        s.addStrandChannel("mask", 0.5f);
        if (round == 0) {
            s.resizeUniform(kStrands, kPoints);
        } else {
            s.resize(counts);
        }
        ASSERT_TRUE(s.validate());
        const std::size_t points = round == 0 ? kStrands * kPoints : s.pointCount();
        EXPECT_EQ(s.strandCount(), kStrands);
        EXPECT_EQ(s.pointCount(), points);
        EXPECT_EQ(s.strandPointCount(7), round == 0 ? kPoints : 3u);
        EXPECT_TRUE(std::all_of(s.positions().begin(), s.positions().end(), [](const Vec3f& p) { return p == Vec3f(); }));
        EXPECT_TRUE(std::all_of(s.widths().begin(), s.widths().end(), [](float w) { return w == 1.0f; }));
        EXPECT_TRUE(std::all_of(s.ids().begin(), s.ids().end(), [](StrandId id) { return id == 0; }));
        EXPECT_TRUE(std::all_of(s.roots().begin(), s.roots().end(), [](const StrandRoot& r) { return r.face == -1; }));
        const std::vector<float>& tint = *s.pointChannel("tint");
        EXPECT_EQ(tint.size(), points);
        EXPECT_TRUE(std::all_of(tint.begin(), tint.end(), [](float c) { return c == 2.0f; }));
        const std::vector<float>& mask = *s.strandChannel("mask");
        EXPECT_EQ(mask.size(), kStrands);
        EXPECT_TRUE(std::all_of(mask.begin(), mask.end(), [](float c) { return c == 0.5f; }));
        // Left as they come: the positions (Scatter writes every point), the widths still 1.
        StrandSet own;
        if (round == 0) {
            own.resizeUniform(kStrands, kPoints, StrandSet::kLeavePositions);
        } else {
            own.resize(counts, StrandSet::kLeavePositions);
        }
        ASSERT_TRUE(own.validate());
        EXPECT_EQ(own.pointCount(), points);
        EXPECT_TRUE(std::all_of(own.widths().begin(), own.widths().end(), [](float w) { return w == 1.0f; }));
        std::vector<Vec3f>& p = own.mutablePositions();
        for (std::size_t i = 0; i < p.size(); ++i) p[i] = Vec3f(static_cast<float>(i), 0.0f, 0.0f);
        EXPECT_FLOAT_EQ(own.strandPoints(1)[0].x, static_cast<float>(own.strandBegin(1)));
    }
}

// What a serial walk over every strand gives: the bounds of every point, and the longest strand
// (its segment lengths summed root to tip).
StrandSet::Extent serialExtent(const StrandSet& s) {
    StrandSet::Extent e{Vec3f(1e30f, 1e30f, 1e30f), Vec3f(-1e30f, -1e30f, -1e30f), 0.0f};
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f* p = s.strandPoints(i);
        float length = 0.0f;
        for (std::size_t k = 0; k < s.strandPointCount(i); ++k) {
            e.lo = Vec3f(std::min(e.lo.x, p[k].x), std::min(e.lo.y, p[k].y), std::min(e.lo.z, p[k].z));
            e.hi = Vec3f(std::max(e.hi.x, p[k].x), std::max(e.hi.y, p[k].y), std::max(e.hi.z, p[k].z));
            if (k > 0) length += (p[k] - p[k - 1]).length();
        }
        e.longestStrand = std::max(e.longestStrand, length);
    }
    return e;
}

TEST(StrandSet, WidthsSetIsKnownUntilTheWidthsAreWritten) {
    // 1 on every point is no width set (a Scatter's strands before any Width node): known as a
    // set is sized, worked out once otherwise, and asked again after every way of writing them.
    StrandSet s;
    EXPECT_FALSE(s.widthsSet());  // no points
    s.resizeUniform(5000, 6);
    EXPECT_FALSE(s.widthsSet());
    StrandSet copy(s);  // (shares the widths)
    EXPECT_FALSE(copy.widthsSet());
    copy.mutableWidths()[4321] = 0.02f;
    EXPECT_TRUE(copy.widthsSet());
    EXPECT_FALSE(s.widthsSet());  // its own stay
    const StrandSet kept(copy);
    EXPECT_TRUE(kept.widthsSet());  // a copy carries what its source knew
    copy.mutableWidths()[4321] = 1.0f;
    EXPECT_FALSE(copy.widthsSet());
    for (float& w : copy.overwriteWidths()) w = 0.5f;
    EXPECT_TRUE(copy.widthsSet());
    // Copied or taken from another set, then some written.
    StrandSet from(s), into(s);
    into.copyWidthsFrom(kept);
    EXPECT_TRUE(into.widthsSet());
    into.copyWidthsFrom(s)[17] = 3.0f;
    EXPECT_TRUE(into.widthsSet());
    StrandSet taken(s);
    EXPECT_FALSE(taken.widthsSet());
    from.mutableWidths()[9] = 2.0f;
    taken.takeWidthsFrom(from);
    EXPECT_TRUE(taken.widthsSet());
    // A strand added with widths, one appended, a set sized again and cleared.
    StrandSet grown(s);
    EXPECT_FALSE(grown.widthsSet());
    const std::vector<Vec3f> two{Vec3f(0.0f, 0.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f)};
    grown.addStrand(1, two);
    EXPECT_FALSE(grown.widthsSet());
    grown.addStrand(2, two, {0.1f, 0.05f});
    EXPECT_TRUE(grown.widthsSet());
    StrandSet joined(s);
    EXPECT_FALSE(joined.widthsSet());
    joined.append(grown);
    EXPECT_TRUE(joined.widthsSet());
    joined.resizeUniform(10, 3);
    EXPECT_FALSE(joined.widthsSet());
    joined.resizeUniform(10, 3, StrandSet::kLeaveWidths);  // (left for the caller to write)
    for (float& w : joined.mutableWidths()) w = 0.25f;
    EXPECT_TRUE(joined.widthsSet());
    joined.clear();
    EXPECT_FALSE(joined.widthsSet());
    const StrandSet moved(std::move(taken));
    EXPECT_TRUE(moved.widthsSet());
}

TEST(StrandSet, ExtentIsTheSerialWalksAndFollowsWrites) {
    StrandSet s;
    EXPECT_TRUE(s.extent().empty());
    // Enough strands for several blocks of the parallel pass, of uneven lengths and wiggles.
    for (int i = 0; i < 9000; ++i) {
        std::vector<Vec3f> pts;
        const int n = 2 + i % 7;
        for (int k = 0; k < n; ++k) {
            pts.emplace_back(std::sin(0.37f * static_cast<float>(i + k)) * 3.0f, 0.1f * static_cast<float>(k * (1 + i % 5)),
                             std::cos(0.11f * static_cast<float>(i * k)));
        }
        s.addStrand(static_cast<StrandId>(i), pts);
    }
    const StrandSet::Extent want = serialExtent(s);
    const StrandSet::Extent got = s.extent();
    EXPECT_EQ(got.lo, want.lo);
    EXPECT_EQ(got.hi, want.hi);
    EXPECT_EQ(got.longestStrand, want.longestStrand);  // exactly: each strand summed in order
    EXPECT_EQ(s.extent().longestStrand, want.longestStrand);  // kept

    // A copy keeps it; writing the copy's points moves the copy's alone.
    StrandSet copy = s;
    EXPECT_EQ(copy.extent().hi, want.hi);
    copy.strandPoints(5)[1] = Vec3f(100.0f, 0.0f, 0.0f);
    EXPECT_EQ(copy.extent().hi.x, 100.0f);
    EXPECT_GT(copy.extent().longestStrand, want.longestStrand);
    EXPECT_EQ(s.extent().hi, want.hi);
    copy.mutablePositions()[0] = Vec3f(-50.0f, 0.0f, 0.0f);
    EXPECT_EQ(copy.extent().lo.x, -50.0f);
    // Strands added or appended, and the set resized or cleared, count too.
    copy.addStrand(99999, {Vec3f(0, 0, 0), Vec3f(0, 0, 500.0f)});
    EXPECT_EQ(copy.extent().hi.z, 500.0f);
    EXPECT_EQ(copy.extent().longestStrand, 500.0f);
    StrandSet more;
    more.addStrand(1, {Vec3f(0, -200.0f, 0), Vec3f(0, 0, 0)});
    copy.append(more);
    EXPECT_EQ(copy.extent().lo.y, -200.0f);
    const StrandSet::Extent appended = serialExtent(copy);
    EXPECT_EQ(copy.extent().lo, appended.lo);
    EXPECT_EQ(copy.extent().hi, appended.hi);
    EXPECT_EQ(copy.extent().longestStrand, appended.longestStrand);
    copy.resizeUniform(3, 2);
    EXPECT_EQ(copy.extent().lo, Vec3f(0, 0, 0));
    EXPECT_EQ(copy.extent().longestStrand, 0.0f);
    copy.clear();
    EXPECT_TRUE(copy.extent().empty());
    // Moved: the extent goes along.
    StrandSet moved = std::move(s);
    EXPECT_EQ(moved.extent().hi, want.hi);
}

TEST(StrandSet, LaidOutAnewItKeepsItsStrandsAndReusesTheArraysOfTheLayoutBefore) {
    // relayout: the same strands at other point counts, the per-point arrays its own at the new
    // size, the rest shared as it was. The arrays come with room to spare and go back to the pool
    // when let go, so a set laid out a few points more or less takes them again (a comb dab's
    // incremental Resample by curvature) instead of allocating.
    releaseArrayPools();  // (what other tests left: the pool holds this test's arrays alone)
    const std::size_t count = 4000;
    StrandSet base;
    base.addStrandChannel("mask", 0.5f);
    base.addPointChannel("along", 0.25f);
    base.resizeUniform(count, 12);
    for (std::size_t s = 0; s < count; ++s) base.mutableIds()[s] = 100 + s;
    std::vector<std::size_t> points(count, 12);
    points[7] = 3;
    points[900] = 30;
    auto pooled = [] { return detail::ArrayPool<Vec3f>::instance().bytes(); };
    const Vec3f* held = nullptr;
    {
        StrandSet relaid = base;
        relaid.relayout(points);
        EXPECT_TRUE(relaid.validate());
        EXPECT_EQ(relaid.strandCount(), count);
        EXPECT_EQ(relaid.pointCount(), count * 12 - 9 + 18);
        EXPECT_EQ(relaid.strandPointCount(7), 3u);
        EXPECT_EQ(relaid.strandBegin(901), 900u * 12 - 9 + 30);
        EXPECT_EQ(relaid.pointChannel("along")->size(), relaid.pointCount());
        EXPECT_EQ(relaid.widths().size(), relaid.pointCount());
        EXPECT_EQ(relaid.ids().data(), base.ids().data());  // shared still
        EXPECT_EQ(relaid.strandChannel("mask")->data(), base.strandChannel("mask")->data());
        EXPECT_EQ(base.pointCount(), count * 12);  // the set it was copied from stands
        held = relaid.positions().data();
    }
    // Let go: its points (576 KB, with room) are kept; a layout a few points smaller or larger
    // takes that very array, one far larger does not.
    const std::size_t kept = pooled();
    EXPECT_GE(kept, (count * 12 + 9) * sizeof(Vec3f));
    for (const std::size_t other : {std::size_t{2}, std::size_t{40}}) {
        points[900] = other;
        StrandSet again = base;
        again.relayout(points);
        EXPECT_EQ(again.positions().data(), held) << other;
        EXPECT_LT(pooled(), kept);
        EXPECT_EQ(again.pointCount(), count * 12 - 9 - 12 + other);
    }
    EXPECT_EQ(pooled(), kept);
    std::vector<std::size_t> twice(count, 24);
    StrandSet larger = base;
    larger.relayout(twice);
    EXPECT_NE(larger.positions().data(), held);
    EXPECT_EQ(pooled(), kept);
}

}  // namespace
}  // namespace groomlab
