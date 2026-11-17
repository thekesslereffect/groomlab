// SPDX-License-Identifier: Apache-2.0
// Subdividing a root tube (root_subdivision.h, issue 85): one big root cut into Voronoi cells
// that seal, smaller where the density map is high, remembered and cut again only where the map
// changed.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>

#include "groomlab/root_subdivision.h"
#include "groomlab/root_tubes.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

std::shared_ptr<const ScalpMesh> head() {
    static const std::shared_ptr<const ScalpMesh> sphere = test::makeSphere(48, 96, 1.0f);
    return sphere;
}

std::vector<Vec3f> ring(const Vec3f& center, float radius, int count = 12) {
    const Vec3f up = center.normalized();
    const Vec3f a = up.cross({0, 0, 1}).normalized();
    const Vec3f b = up.cross(a);
    std::vector<Vec3f> out;
    for (int k = 0; k < count; ++k) {
        const float t = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(count);
        out.push_back((up + a * (radius * std::cos(t)) + b * (radius * std::sin(t))).normalized());
    }
    return out;
}

// The sphere with the density map painted: `density(p)` at each vertex.
ScalpMesh painted(const std::function<float(const Vec3f&)>& density) {
    ScalpMesh s = *head();
    std::vector<ScalpMesh::Color> corners;
    for (std::size_t t = 0; t < s.triangleCount(); ++t) {
        for (int c = 0; c < 3; ++c) {
            const float d = density(s.points()[static_cast<std::size_t>(s.triangles()[t][c])]);
            corners.push_back({d, d, d, 1.0f});
        }
    }
    s.setCornerColors(kSubdivisionDensityMap, std::move(corners));
    return s;
}

double area(const std::vector<CutPoint>& poly) {
    double a = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const CutPoint& p = poly[i];
        const CutPoint& q = poly[(i + 1) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

GuideId bigRoot(GuideHierarchy& h, const ScalpMesh& scalp) {
    RootTubeOptions o;
    o.length = 0.8f;
    o.segments = 8;
    return makeRootTube(h, scalp, ring({0, 1, 0}, 0.3f), {}, o);
}

TEST(RootSubdivision, VoronoiCellsTileTheOutlineAndShareTheirPoints) {
    const std::vector<CutPoint> square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::vector<CutPoint> sites{{0.25, 0.25}, {0.75, 0.3}, {0.3, 0.8}, {0.7, 0.7}, {0.5, 0.5}, {2.0, 2.0}};
    const auto cells = voronoiCells(square, sites);
    ASSERT_EQ(cells.size(), sites.size());
    EXPECT_TRUE(cells.back().empty());  // a site outside: no cell
    double total = 0.0;
    std::map<std::pair<double, double>, int> uses;
    for (const auto& cell : cells) {
        if (cell.empty()) continue;
        EXPECT_GT(area(cell), 0.0);  // counter-clockwise
        total += area(cell);
        for (const CutPoint& p : cell) ++uses[{p.x, p.y}];
    }
    EXPECT_NEAR(total, 1.0, 1e-9);
    // Every point inside the square is shared exactly by two cells or more (welded).
    for (const auto& [p, n] : uses) {
        const bool onBorder = p.first < 1e-9 || p.first > 1 - 1e-9 || p.second < 1e-9 || p.second > 1 - 1e-9;
        if (!onBorder) EXPECT_GE(n, 2) << p.first << ", " << p.second;
    }
}

TEST(RootSubdivision, SitesAreSpacedByTheirSize) {
    const std::vector<CutPoint> square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const auto coarse = subdivisionSites(square, [](const CutPoint&) { return 0.3f; }, 7, 0);
    const auto fine = subdivisionSites(square, [](const CutPoint&) { return 0.1f; }, 7, 0);
    EXPECT_GT(fine.size(), 3 * coarse.size());
    for (std::size_t i = 0; i < coarse.size(); ++i) {
        for (std::size_t j = i + 1; j < coarse.size(); ++j) {
            EXPECT_GE(std::hypot(coarse[i].x - coarse[j].x, coarse[i].y - coarse[j].y), 0.3 - 1e-9);
        }
    }
    // Denser on the right half.
    const auto mixed = subdivisionSites(square, [](const CutPoint& p) { return p.x > 0.5 ? 0.08f : 0.3f; }, 7, 0);
    std::size_t left = 0, right = 0;
    for (const CutPoint& p : mixed) (p.x > 0.5 ? right : left)++;
    EXPECT_GT(right, 3 * left);
    // The same seed: the same sites.
    const auto again = subdivisionSites(square, [](const CutPoint&) { return 0.1f; }, 7, 0);
    ASSERT_EQ(again.size(), fine.size());
    for (std::size_t i = 0; i < fine.size(); ++i) EXPECT_EQ(again[i].x, fine[i].x);
}

TEST(RootSubdivision, ARootIsReplacedByCellsThatSealAndAreRemembered) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId root = bigRoot(h, *scalp);
    const SubdivisionResult cut = subdivideRootTubes(h, *scalp, {root});
    EXPECT_GE(cut.cells.size(), 4u);
    EXPECT_EQ(h.guide(root), nullptr);
    const std::vector<GuideId> roots = rootTubesOf(h);
    EXPECT_EQ(roots.size(), cut.cells.size());
    // The cells share CVs: each cell touches another.
    std::vector<std::vector<Vec3f>> outlines;
    for (const GuideId c : cut.cells) outlines.push_back(rootTubeOutline(h, *scalp, c));
    for (std::size_t i = 0; i < outlines.size(); ++i) {
        std::vector<std::vector<Vec3f>> others;
        for (std::size_t j = 0; j < outlines.size(); ++j) {
            if (j != i) others.push_back(outlines[j]);
        }
        EXPECT_FALSE(sealedCvs(outlines[i], others, 1e-4f).empty()) << "cell " << i;
    }
    // Each cell's guide leaves the scalp as the old one did (straight up), about as long.
    for (const GuideId c : cut.cells) {
        const HierarchyGuide* g = h.guide(c);
        ASSERT_NE(g, nullptr);
        const Vec3f out = (g->points.back() - g->points.front()).normalized();
        EXPECT_GT(out.dot(g->points.front().normalized()), 0.8f);
    }
    ASSERT_EQ(h.rootSubdivisions().size(), 1u);
    EXPECT_EQ(h.rootSubdivisions()[0].cells, cut.cells);
    // Saved with the groom.
    const GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    ASSERT_EQ(back.rootSubdivisions().size(), 1u);
    EXPECT_TRUE(back.rootSubdivisions()[0] == h.rootSubdivisions()[0]);
    EXPECT_EQ(back.contentHash(), h.contentHash());
}

TEST(RootSubdivision, ThePaintedDensityMakesMoreSmallerCells) {
    const ScalpMesh flat = painted([](const Vec3f&) { return 0.0f; });
    const ScalpMesh dense = painted([](const Vec3f&) { return 1.0f; });
    GuideHierarchy a, b;
    const std::size_t few = subdivideRootTubes(a, flat, {bigRoot(a, flat)}).cells.size();
    const std::size_t many = subdivideRootTubes(b, dense, {bigRoot(b, dense)}).cells.size();
    EXPECT_GT(many, 3 * few);

    // Half painted: the dense half (x > 0) gets most cells.
    const ScalpMesh half = painted([](const Vec3f& p) { return p.x > 0.0f ? 1.0f : 0.0f; });
    GuideHierarchy c;
    const auto cells = subdivideRootTubes(c, half, {bigRoot(c, half)}).cells;
    std::size_t right = 0;
    for (const GuideId id : cells) right += c.guide(id)->points.front().x > 0.0f;
    EXPECT_GT(right, 2 * (cells.size() - right));
}

TEST(RootSubdivision, TheHierarchyUnderTheRootIsMadeAgainInEachCell) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId root = bigRoot(h, *scalp);
    addTubeLevel(h, scalp.get(), {root});
    addTubeLevel(h, scalp.get(), {root});
    const auto cut = subdivideRootTubes(h, *scalp, {root});
    const TubeLevels levels = tubeLevels(h, cut.cells);
    ASSERT_EQ(levels.levels.size(), 3u);
    for (const GuideId c : cut.cells) {
        const auto kids = h.childrenOf(c);
        ASSERT_EQ(kids.size(), 2u);
        for (const GuideId k : kids) EXPECT_EQ(h.childrenOf(k).size(), 2u);
    }
}

TEST(RootSubdivision, PaintingAgainCutsOnlyWhereTheDensityChanged) {
    const ScalpMesh before = painted([](const Vec3f&) { return 0.3f; });
    GuideHierarchy h;
    const auto first = subdivideRootTubes(h, before, {bigRoot(h, before)}).cells;
    // Nothing changed: nothing cut.
    const auto none = applySubdivisionDensity(h, before);
    EXPECT_TRUE(none.cells.empty());
    EXPECT_EQ(none.removed, 0u);

    // Denser at the +x edge only.
    const ScalpMesh after = painted([](const Vec3f& p) { return p.x > 0.2f ? 1.0f : 0.3f; });
    const auto again = applySubdivisionDensity(h, after);
    EXPECT_FALSE(again.cells.empty());
    EXPECT_GT(again.removed, 0u);
    std::size_t untouched = 0;
    for (const GuideId c : first) {
        if (const HierarchyGuide* g = h.guide(c)) {
            ++untouched;
            EXPECT_LT(g->points.front().x, 0.2f);  // the cells left are out of the change
        }
    }
    EXPECT_GT(untouched, 0u);
    // The record follows: its cells are the roots there now.
    const auto& r = h.rootSubdivisions().at(0);
    std::set<GuideId> live;
    for (const GuideId c : r.cells) {
        if (c != kNoGuide) live.insert(c);
    }
    const auto roots = rootTubesOf(h);
    EXPECT_EQ(live, std::set<GuideId>(roots.begin(), roots.end()));
}

}  // namespace
}  // namespace groomlab
