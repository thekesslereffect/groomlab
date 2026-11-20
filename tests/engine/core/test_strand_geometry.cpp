// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/random.h"
#include "groomlab/strand_geometry.h"
#include "test_helpers.h"

using namespace groomlab;

namespace {

// `n` strands of `points` points lying along +X over the XZ plane (rooted on face 0), widths 0.1.
StrandSet flatStrands(std::size_t n, std::size_t points = 5) {
    StrandSet s;
    for (std::size_t k = 0; k < n; ++k) {
        std::vector<Vec3f> p;
        for (std::size_t i = 0; i < points; ++i) p.emplace_back(0.1f * i, 0.0f, 0.01f * k);
        StrandRoot root;
        root.face = 0;
        root.u = 0.2f;
        root.v = 0.2f;
        s.addStrand(1000 + k * 7, p, std::vector<float>(points, 0.1f), root);
    }
    return s;
}

}  // namespace

TEST(StrandGeometry, ToMeshAsEveryHostAsksForIt) {
    // strandMeshOptionsFrom / strandsForMesh: Maya's groomLabConvert, the Blender helper's
    // strand_mesh and the app's Strands to Mesh check and thin the same way.
    const StrandMeshOptions o = strandMeshOptionsFrom("strands", "ribbon", 6, 2.0, 0.01, true);
    EXPECT_EQ(o.make, StrandMeshOptions::Make::Strands);
    EXPECT_EQ(o.style, StrandMeshOptions::Style::Ribbon);
    EXPECT_EQ(o.sides, 6);
    EXPECT_TRUE(o.strandUVs);
    EXPECT_THROW(strandMeshOptionsFrom("blobs"), std::invalid_argument);
    EXPECT_THROW(strandMeshOptionsFrom("auto", "cone"), std::invalid_argument);
    EXPECT_THROW(strandMeshOptionsFrom("auto", "tube", 2), std::invalid_argument);
    EXPECT_THROW(strandMeshOptionsFrom("auto", "tube", kMaxMeshSides + 1), std::invalid_argument);
    EXPECT_THROW(strandMeshOptionsFrom("auto", "tube", 4, 0.0), std::invalid_argument);
    const StrandSet s = flatStrands(200);
    EXPECT_FALSE(strandsForMesh(s, o, 500).has_value()) << "under the budget: every strand as it is";
    const std::optional<StrandSet> fifty = strandsForMesh(s, o, 50);
    ASSERT_TRUE(fifty.has_value());
    EXPECT_EQ(fifty->strandCount(), thinStrandsById(s, 50).size());
}

TEST(StrandGeometry, ThinByIdKeepsTheLowestHashesAsASubset) {
    const StrandSet s = flatStrands(200);
    EXPECT_EQ(thinStrandsById(s, 500).size(), 200u);  // under the budget: every strand
    const std::vector<std::size_t> fifty = thinStrandsById(s, 50);
    const std::vector<std::size_t> twenty = thinStrandsById(s, 20);
    ASSERT_EQ(fifty.size(), 50u);
    ASSERT_EQ(twenty.size(), 20u);
    EXPECT_TRUE(std::is_sorted(fifty.begin(), fifty.end()));
    EXPECT_TRUE(std::includes(fifty.begin(), fifty.end(), twenty.begin(), twenty.end()));  // a smaller budget: a subset
    // The kept strands hash lower than every dropped one; the same IDs in another order keep the same strands.
    std::uint64_t highestKept = 0;
    for (const std::size_t i : fifty) highestKept = std::max(highestKept, hash64(s.ids()[i]));
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        if (!std::binary_search(fifty.begin(), fifty.end(), i)) EXPECT_GT(hash64(s.ids()[i]), highestKept);
    }
    std::vector<std::size_t> reversed(s.strandCount());
    for (std::size_t i = 0; i < reversed.size(); ++i) reversed[i] = reversed.size() - 1 - i;
    const StrandSet flipped = s.subset(reversed);
    std::set<StrandId> a, b;
    for (const std::size_t i : fifty) a.insert(s.ids()[i]);
    for (const std::size_t i : thinStrandsById(flipped, 50)) b.insert(flipped.ids()[i]);
    EXPECT_EQ(a, b);
}

TEST(StrandGeometry, TubesHaveARingPerPointAndAClosedTip) {
    const StrandSet s = flatStrands(3, 5);
    StrandMeshOptions o;
    o.sides = 4;
    const StrandMesh m = buildStrandMesh(s, nullptr, o);
    // Per strand: 5 rings of 4 vertices and a tip apex; 4 quads between each ring pair, a fan of 4 at the tip.
    EXPECT_EQ(m.mesh.points.size(), 3u * (5 * 4 + 1));
    EXPECT_EQ(m.mesh.faceCount(), 3u * (4 * 4 + 4));
    EXPECT_EQ(m.mesh.u.size(), 3u * (5 * 5 + 1));  // a seam column of its own
    EXPECT_TRUE(m.u2.empty());
    // Ring radius is half the width: every ring vertex sits 0.05 from its point.
    EXPECT_NEAR((m.mesh.points[0] - Vec3f(0, 0, 0)).length(), 0.05f, 1e-5f);
    // v runs root to tip; faces name their strand.
    EXPECT_FLOAT_EQ(*std::min_element(m.mesh.v.begin(), m.mesh.v.end()), 0.0f);
    EXPECT_FLOAT_EQ(*std::max_element(m.mesh.v.begin(), m.mesh.v.end()), 1.0f);
    EXPECT_EQ(m.mesh.faceFeather.front(), 0);
    EXPECT_EQ(m.mesh.faceFeather.back(), 2);
    // widthScale widens every ring.
    o.widthScale = 2.0f;
    EXPECT_NEAR((buildStrandMesh(s, nullptr, o).mesh.points[0]).length(), 0.1f, 1e-5f);
}

TEST(StrandGeometry, RibbonsLieFlatOnTheScalpWithStrandUVs) {
    const auto plane = test::makePlane(4);
    const StrandSet s = flatStrands(5, 4);
    StrandMeshOptions o;
    o.style = StrandMeshOptions::Style::Ribbon;
    o.strandUVs = true;
    const StrandMesh m = buildStrandMesh(s, plane.get(), o);
    EXPECT_EQ(m.mesh.points.size(), 5u * 4 * 2);  // two vertices across each point, no tip fan
    EXPECT_EQ(m.mesh.faceCount(), 5u * 3);
    // Flat along the scalp (normal +Y): both vertices of a ring at the strand's height, across it in z.
    for (const Vec3f& p : m.mesh.points) EXPECT_NEAR(p.y, 0.0f, 1e-5f);
    EXPECT_NEAR(std::abs(m.mesh.points[0].z - m.mesh.points[1].z), 0.1f, 1e-5f);
    // uv2: each strand in its own cell of a 3 x 3 grid.
    ASSERT_EQ(m.u2.size(), m.mesh.u.size());
    std::size_t corner = 0;
    for (std::size_t f = 0; f < m.mesh.faceCount(); ++f) {
        const int strand = m.mesh.faceFeather[f];
        for (int c = 0; c < m.mesh.faceCounts[f]; ++c, ++corner) {
            const auto uv = static_cast<std::size_t>(m.mesh.faceUVs[corner]);
            EXPECT_EQ(static_cast<int>(m.u2[uv] * 3.0f), strand % 3);
            EXPECT_EQ(static_cast<int>(m.v2[uv] * 3.0f), strand / 3);
        }
    }
}

TEST(StrandGeometry, ShortStrandsAreSkipped) {
    StrandSet s = flatStrands(2, 4);
    s.addStrand(5, {Vec3f(0, 0, 0)});
    StrandMeshOptions o;
    o.sides = 3;
    const StrandMesh m = buildStrandMesh(s, nullptr, o);
    EXPECT_EQ(m.mesh.points.size(), 2u * (4 * 3 + 1));
}

TEST(StrandGeometry, PointsAloneAreTheMeshesPointsAndTheKeyFollowsTheTopology) {
    const auto plane = test::makePlane(4);
    StrandSet s = flatStrands(150, 6);  // past a parallel block of strands
    s.addStrand(9, {Vec3f(0, 0, 0)});   // skipped: no vertices
    // Bent a little, so the frames turn.
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        Vec3f* p = s.strandPoints(k);
        for (std::size_t i = 0; i < s.strandPointCount(k); ++i) p[i].y += 0.02f * static_cast<float>(i * i) + 0.001f * k;
    }
    for (const auto style : {StrandMeshOptions::Style::Tube, StrandMeshOptions::Style::Ribbon}) {
        StrandMeshOptions o;
        o.style = style;
        o.sides = 5;
        o.widthScale = 1.5f;
        const StrandMesh m = buildStrandMesh(s, plane.get(), o);
        ASSERT_EQ(strandMeshPointCount(s, o), m.mesh.points.size());
        std::vector<Vec3f> points(m.mesh.points.size());
        strandMeshPoints(s, plane.get(), o, points.data());
        EXPECT_TRUE(points == m.mesh.points);  // to the bit
    }
    // The key: the same for moved points and widths; another for other sides, strands or point counts.
    StrandMeshOptions o;
    const std::uint64_t key = strandMeshTopologyKey(s, o);
    StrandSet moved = s;
    for (Vec3f& p : moved.mutablePositions()) p = p + Vec3f(0.3f, 0.1f, 0.0f);
    EXPECT_EQ(strandMeshTopologyKey(moved, o), key);
    o.widthScale = 3.0f;
    EXPECT_EQ(strandMeshTopologyKey(s, o), key);
    o.sides = 6;
    EXPECT_NE(strandMeshTopologyKey(s, o), key);
    o.sides = 4;
    EXPECT_NE(strandMeshTopologyKey(s.subset({0, 1, 2}), o), key);
    EXPECT_NE(strandMeshTopologyKey(flatStrands(150, 7), o), strandMeshTopologyKey(flatStrands(150, 6), o));
}
