// SPDX-License-Identifier: Apache-2.0
// Subdividing a root tube (root_subdivision.h, issue 85): one big root cut into Voronoi cells
// that seal, smaller where the density map is high, remembered and cut again only where the map
// changed.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>

#include "groomlab/display.h"
#include "groomlab/root_subdivision.h"
#include "groomlab/root_tubes.h"
#include "groomlab/tube.h"
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

// A point's section coordinates in a spine's sample i (its center and axes).
SectionPoint inSection(const TubeSpine& spine, std::size_t i, const Vec3f& p) {
    const Vec3f d = p - spine.center(i);
    const Vec3f a = spine.axisU(i), b = spine.axisV(i);
    const float aa = a.dot(a), bb = b.dot(b), ab = a.dot(b), da = d.dot(a), db = d.dot(b);
    const float det = aa * bb - ab * ab;
    return {(da * bb - db * ab) / det, (db * aa - da * ab) / det};
}

// Whether `p` lies inside polygon `poly` deeper than `slack` (its distance to the edge), or (with
// `outside`) outside it farther than `slack`.
bool deepInside(const std::vector<SectionPoint>& poly, const SectionPoint& p, float slack, bool outside = false) {
    bool in = false;
    float edge = 1e30f;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const SectionPoint& a = poly[i];
        const SectionPoint& b = poly[j];
        if ((a.v > p.v) != (b.v > p.v) && p.u < (b.u - a.u) * (p.v - a.v) / (b.v - a.v) + a.u) in = !in;
        const float du = b.u - a.u, dv = b.v - a.v, span = du * du + dv * dv;
        const float f = span > 0.0f ? std::clamp(((p.u - a.u) * du + (p.v - a.v) * dv) / span, 0.0f, 1.0f) : 0.0f;
        edge = std::min(edge, std::hypot(p.u - (a.u + du * f), p.v - (a.v + dv * f)));
    }
    return (outside ? !in : in) && edge > slack;
}

// A drawn root's split: two slices of its root shape, side by side, each a shape of its own; all
// along the tube neither slice reaches into the other (they never pass through each other) and
// both stay inside their parent's tube.
void expectSliced(const GuideHierarchy& h, GuideId parentId) {
    const HierarchyGuide* parent = h.guide(parentId);
    ASSERT_NE(parent, nullptr);
    std::vector<GuideId> kids = h.childrenOf(parentId);
    ASSERT_EQ(kids.size(), 2u);
    const TubeSpine pspine(parent->points.data(), parent->points.size(), parent->frameUp, parent->tube, h.scale());
    std::vector<TubeSpine> spines;
    for (const GuideId k : kids) {
        const HierarchyGuide* kid = h.guide(k);
        ASSERT_NE(kid, nullptr);
        ASSERT_TRUE(kid->tube.custom);
        EXPECT_TRUE(isRootTube(kid->tube, 0)) << "a slice takes a root shape of its own";
        EXPECT_NE(kid->tube.custom->name, parent->tube.custom->name);
        ASSERT_EQ(kid->points.size(), parent->points.size());
        spines.emplace_back(kid->points.data(), kid->points.size(), kid->frameUp, kid->tube, h.scale());
    }
    const std::vector<SectionPoint>& own = tubeOutline(parent->tube);
    for (std::size_t i = 1; i < parent->points.size(); ++i) {
        // Each slice's outline at sample i in the parent's section there.
        std::vector<SectionPoint> at[2];
        for (std::size_t k = 0; k < 2; ++k) {
            for (const SectionPoint& q : tubeOutline(h.guide(kids[k])->tube)) at[k].push_back(inSection(pspine, i, spines[k].atSample(i, q.u, q.v)));
        }
        for (std::size_t k = 0; k < 2; ++k) {
            for (const SectionPoint& q : at[k]) {
                EXPECT_FALSE(deepInside(at[1 - k], q, 0.03f)) << "slice " << k << " reaches into the other at sample " << i;
                EXPECT_FALSE(deepInside(own, q, 0.05f, true)) << "slice " << k << " leaves its parent's tube at sample " << i;
            }
        }
    }
}

TEST(RootSlices, ASplitSlicesTheRootShapeInTwo) {
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    o.segments = 8;
    const std::vector<Vec3f> oval = [] {
        std::vector<Vec3f> out;
        const Vec3f up{0, 1, 0};
        for (int k = 0; k < 10; ++k) {
            const float t = 2.0f * kPi * static_cast<float>(k) / 10.0f;
            out.push_back((up + Vec3f{0.3f * std::cos(t), 0.0f, 0.12f * std::sin(t)}).normalized());
        }
        return out;
    }();
    // Straight off the scalp, and along a drawn curve.
    const GuideId straight = makeRootTube(h, *scalp, oval, {}, o);
    addTubeLevel(h, scalp.get(), {straight});
    expectSliced(h, straight);
    GuideHierarchy curved;
    const std::vector<Vec3f> path{{0, 1, 0}, {0.1f, 1.3f, 0}, {0.35f, 1.5f, 0}, {0.7f, 1.55f, 0.05f}};
    const GuideId bent = makeRootTube(curved, *scalp, oval, path, o);
    addTubeLevel(curved, scalp.get(), {bent});
    expectSliced(curved, bent);
    // A level deeper: each slice sliced again.
    addTubeLevel(curved, scalp.get(), {bent});
    for (const GuideId k : curved.childrenOf(bent)) expectSliced(curved, k);
}

TEST(RootSlices, ADrawnTubeStartsOnItsRootOutline) {
    // Issue 86: the tube drawn (its wire's first ring) lies on the scalp, on the outline drawn,
    // however the guide leans; the hair's footprint and the drawn tube meet.
    GuideHierarchy h;
    const auto scalp = head();
    const std::vector<Vec3f> outline = ring({0, 1, 0}, 0.2f, 8);
    const std::vector<Vec3f> path{{0, 1, 0}, {0.3f, 1.1f, 0}, {0.8f, 1.15f, 0}};  // leaning hard over
    const GuideId g = makeRootTube(h, *scalp, outline, path);
    h.evaluate(scalp.get());
    std::vector<Vec3f> wire;
    appendTubeWire(wire, *h.guide(g), h.scale(), 8, 1);
    ASSERT_GE(wire.size(), 64u);
    float off = 0.0f;
    for (std::size_t i = 0; i < 64; ++i) off = std::max(off, std::abs(wire[i].length() - 1.0f));  // the first ring
    // On the footprint, flat on the scalp as the hair's roots are: off the unit sphere by its sag
    // alone (a root of radius 0.2: about 0.02 at its edge). Drawn as the root section instead, a
    // guide leaning this far would start its tube some 0.15 above or into the scalp.
    EXPECT_LT(off, 0.035f) << "the root ring lies on the scalp";
}

TEST(RootSubdivision, PaintingAgainRebuildsTheLevelsFromTheNewCells) {
    const ScalpMesh before = painted([](const Vec3f&) { return 0.3f; });
    GuideHierarchy h;
    const GuideId root = bigRoot(h, before);
    addTubeLevel(h, &before, {root});
    subdivideRootTubes(h, before, {root});
    const ScalpMesh after = painted([](const Vec3f& p) { return p.x > 0.2f ? 1.0f : 0.3f; });
    const auto again = applySubdivisionDensity(h, after);
    ASSERT_FALSE(again.cells.empty());
    for (const GuideId c : again.cells) expectSliced(h, c);
}

}  // namespace
}  // namespace groomlab
