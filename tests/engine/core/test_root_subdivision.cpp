// SPDX-License-Identifier: Apache-2.0
// Subdividing a root tube (root_subdivision.h, issue 85): one big root cut into Voronoi cells
// that seal, smaller where the density map is high, remembered and cut again only where the map
// changed.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "groomlab/brush.h"
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

float nearestOf(const Vec3f& p, const std::vector<Vec3f>& pts) {
    float best = 1e30f;
    for (const Vec3f& q : pts) best = std::min(best, (p - q).length());
    return best;
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

TEST(RootSubdivision, ARootIsCutIntoCellsInTheLevelBelowThatSealAndAreRemembered) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId root = bigRoot(h, *scalp);
    const SubdivisionResult cut = subdivideRootTubes(h, *scalp, {root});
    EXPECT_GE(cut.cells.size(), 4u);
    // The root kept (its guide and tube shape them all), the cells its children a level below.
    ASSERT_NE(h.guide(root), nullptr);
    const TubeLevels levels = tubeLevels(h, {root});
    ASSERT_EQ(levels.levels.size(), 2u);
    for (const GuideId c : cut.cells) {
        EXPECT_EQ(h.guide(c)->parent, root);
        EXPECT_EQ(h.group(h.guide(c)->group)->name, levels.levels[1]);
    }
    const std::vector<GuideId> roots = rootTubesOf(h);
    EXPECT_EQ(roots.size(), cut.cells.size() + 1) << "the root and its cells (a cell is picked and cut as a root)";
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
    // Saved with the groom (the cells following the root: their shape from it, evaluated).
    GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    back.evaluate(scalp.get());
    ASSERT_EQ(back.rootSubdivisions().size(), 1u);
    EXPECT_TRUE(back.rootSubdivisions()[0] == h.rootSubdivisions()[0]);
    for (const GuideId c : cut.cells) {
        const HierarchyGuide* was = h.guide(c);
        const HierarchyGuide* now = back.guide(c);
        ASSERT_NE(now, nullptr);
        ASSERT_EQ(now->points.size(), was->points.size());
        for (std::size_t i = 0; i < was->points.size(); ++i) EXPECT_LT((now->points[i] - was->points[i]).length(), 1e-4f);
    }
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
    // The cells take the root's level 2; its level 3 made again under them.
    const auto cut = subdivideRootTubes(h, *scalp, {root});
    const TubeLevels levels = tubeLevels(h, cut.cells);
    ASSERT_EQ(levels.levels.size(), 3u);
    EXPECT_EQ(h.childrenOf(root).size(), cut.cells.size());
    for (const GuideId c : cut.cells) {
        const auto kids = h.childrenOf(c);
        ASSERT_EQ(kids.size(), 2u);
        for (const GuideId k : kids) EXPECT_EQ(h.childrenOf(k).size(), 0u);
    }
}

TEST(RootSubdivision, EveryHostFindsTheDensityMapAnOlderGroomPainted) {
    // Subdivide, Paint and Apply in every host: the one map, else an older numbered one (its
    // painting read as it is: nothing renamed), else none yet.
    EXPECT_EQ(subdivisionDensityMapIn({"length", kSubdivisionDensityMap, "tubeSubdivisionDensity01"}), kSubdivisionDensityMap);
    EXPECT_EQ(subdivisionDensityMapIn({"tubeSubdivisionDensity02", "length", "tubeSubdivisionDensity01"}), "tubeSubdivisionDensity01");
    EXPECT_EQ(subdivisionDensityMapIn({"length", "tubeSubdivisionDensityOld"}), "") << "a map only named like it";
    EXPECT_EQ(subdivisionDensityMapIn({}), "");
}

TEST(RootSubdivision, PaintingAgainCutsOnlyWhereTheDensityChanged) {
    const ScalpMesh before = painted([](const Vec3f&) { return 0.3f; });
    GuideHierarchy h;
    const SubdivisionResult cut = subdivideRootTubes(h, before, {bigRoot(h, before)});
    const auto first = cut.cells;
    EXPECT_EQ(cut.cellLevel, 2) << "the cells are in the level below the root tubes' (1; the card shows it)";
    // Nothing changed: nothing cut.
    const auto none = applySubdivisionDensity(h, before);
    EXPECT_TRUE(none.cells.empty());
    EXPECT_EQ(none.cellLevel, 0);
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
    std::set<GuideId> cells;
    for (const GuideId g : rootTubesOf(h)) {
        if (h.guide(g)->isChild()) cells.insert(g);  // (the root cut is a root too)
    }
    EXPECT_EQ(live, cells);
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
                EXPECT_FALSE(deepInside(at[1 - k], q, 0.0f)) << "slice " << k << " reaches into the other at sample " << i;
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
    EXPECT_TRUE(tubeOverlaps(curved).empty()) << "tubeOverlaps finds slices passing into each other";
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
    // The root loop on the scalp itself (its footprint's outline taken onto it): no gap however the
    // scalp curves. (Drawn as the root section, a guide leaning this far would start its tube
    // some 0.15 above or into the scalp.)
    EXPECT_LT(off, 0.002f) << "the root ring lies on the scalp";
}

TEST(RootSubdivision, PaintingAgainRebuildsTheLevelsFromTheNewCells) {
    const ScalpMesh before = painted([](const Vec3f&) { return 0.3f; });
    GuideHierarchy h;
    const GuideId root = bigRoot(h, before);
    addTubeLevel(h, &before, {root});
    addTubeLevel(h, &before, {root});  // (the cells take level 2: level 3 is made again under them)
    subdivideRootTubes(h, before, {root});
    const ScalpMesh after = painted([](const Vec3f& p) { return p.x > 0.2f ? 1.0f : 0.3f; });
    const auto again = applySubdivisionDensity(h, after);
    ASSERT_FALSE(again.cells.empty());
    for (const GuideId c : again.cells) expectSliced(h, c);
}

TEST(RootSlices, SlicesLieAsTheirParentWhereTheScalpsAcrossTurns) {
    // The scalp's across direction switches from x to z where the scalp faces along x (more than
    // 0.8): a root drawn across that line keeps its slices' footprints lying as its own (no turn
    // between levels), and they still slice it cleanly.
    GuideHierarchy h;
    const auto scalp = head();
    const Vec3f at = Vec3f{0.8f, 0.6f, 0.0f}.normalized();
    const GuideId root = makeRootTube(h, *scalp, ring(at, 0.3f, 10), {});
    addTubeLevel(h, scalp.get(), {root});
    addTubeLevel(h, scalp.get(), {root});
    h.evaluate(scalp.get());
    const HierarchyGuide* parent = h.guide(root);
    ASSERT_GT(parent->rootAcross.length(), 0.5f);
    std::vector<GuideId> todo = h.childrenOf(root);
    std::size_t checked = 0;
    while (!todo.empty()) {
        const GuideId k = todo.back();
        todo.pop_back();
        const HierarchyGuide* kid = h.guide(k);
        const HierarchyGuide* up = h.guide(kid->parent);
        EXPECT_GT(kid->rootAcross.dot(up->rootAcross), 0.97f) << "a slice's footprint turned against its parent's";
        for (const GuideId c : h.childrenOf(k)) todo.push_back(c);
        ++checked;
    }
    EXPECT_EQ(checked, 6u);
    expectSliced(h, root);
    for (const GuideId k : h.childrenOf(root)) expectSliced(h, k);
}

TEST(RootSlices, SlicesFollowTheirParentsPaintedTube) {
    // Width, flatness and twist painted on a level 1 tube carry to its slices (and theirs), along
    // them; a slice's own edits stay.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    o.segments = 8;
    const GuideId root = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.25f, 10), {}, o);
    addTubeLevel(h, scalp.get(), {root});
    addTubeLevel(h, scalp.get(), {root});
    const std::vector<GuideId> kids = h.childrenOf(root);
    const GuideId grandkid = h.childrenOf(kids[0])[0];
    const float kidTip = h.guide(kids[0])->tube.widthAt(1.0f), kidRoot = h.guide(kids[0])->tube.widthAt(0.0f);
    const float deepTip = h.guide(grandkid)->tube.widthAt(1.0f);
    const float kidTwist = h.guide(kids[0])->tube.twistAt(1.0f);
    TubeProfile painted = h.guide(root)->tube;
    const std::size_t n = h.guide(root)->points.size();
    painted.width.assign(n, painted.widthAt(0.0f));
    painted.twist.assign(n, painted.twistAt(0.0f));
    const float w0 = painted.width[0];
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        painted.width[i] = h.guide(root)->tube.widthAt(t) * (1.0f + t);  // twice as wide at the tip
        painted.twist[i] += 0.5f * t;
    }
    painted.width[0] = w0;
    h.setTube(root, painted);
    h.evaluate(scalp.get());
    EXPECT_NEAR(h.guide(kids[0])->tube.widthAt(1.0f), 2.0f * kidTip, 0.02f * kidTip);
    EXPECT_NEAR(h.guide(kids[0])->tube.widthAt(0.0f), kidRoot, 1e-4f * kidRoot);
    EXPECT_NEAR(h.guide(grandkid)->tube.widthAt(1.0f), 2.0f * deepTip, 0.02f * deepTip);
    EXPECT_NEAR(h.guide(kids[0])->tube.twistAt(1.0f), kidTwist + 0.5f, 1e-3f);
    expectSliced(h, root);
}

TEST(RootSubdivision, EachSubdivisionPaintsItsOwnMap) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId a = bigRoot(h, *scalp);
    bool fresh = false;
    EXPECT_EQ(subdivisionDensityMapFor(h, {a}, {}, &fresh), "tubeSubdivisionDensity01");
    EXPECT_TRUE(fresh);
    EXPECT_EQ(subdivisionDensityMapFor(h, {a}, {"tubeSubdivisionDensity01"}), "tubeSubdivisionDensity02");
    RootSubdivisionOptions o;
    o.map = subdivisionDensityMapName(1);
    const auto cells = subdivideRootTubes(h, *scalp, {a}, o).cells;
    // A cell of it: its map, to paint again; another root: the next.
    EXPECT_EQ(subdivisionDensityMapFor(h, {cells[0]}, {}, &fresh), "tubeSubdivisionDensity01");
    EXPECT_FALSE(fresh);
    RootTubeOptions r;
    const GuideId b = makeRootTube(h, *scalp, ring(Vec3f{0.0f, 0.6f, 0.8f}.normalized(), 0.2f, 8), {}, r);
    EXPECT_EQ(subdivisionDensityMapFor(h, {b}, {}), "tubeSubdivisionDensity02");
    // Saved with it.
    const GuideHierarchy back = GuideHierarchy::fromJson(h.toJson());
    EXPECT_EQ(back.rootSubdivisions().at(0).map, "tubeSubdivisionDensity01");
}

TEST(RootSubdivision, TheMultiplierMakesTheCutDenser) {
    const auto scalp = head();
    GuideHierarchy one, two;
    RootSubdivisionOptions o;
    const std::size_t plain = subdivideRootTubes(one, *scalp, {bigRoot(one, *scalp)}, o).cells.size();
    o.multiplier = 3.0f;
    const std::size_t dense = subdivideRootTubes(two, *scalp, {bigRoot(two, *scalp)}, o).cells.size();
    EXPECT_GT(dense, 2 * plain);
    EXPECT_FLOAT_EQ(two.rootSubdivisions().at(0).multiplier, 3.0f);
    // Applied again at another multiplier: the whole root cut again at it (nothing painted).
    o.multiplier = 1.0f;
    const auto again = applySubdivisionDensity(two, *scalp, {}, o);
    EXPECT_GT(again.removed, 0u);
    EXPECT_LT(rootTubesOf(two).size(), dense);
    EXPECT_FLOAT_EQ(two.rootSubdivisions().at(0).multiplier, 1.0f);
    // The same multiplier and density: nothing.
    EXPECT_TRUE(applySubdivisionDensity(two, *scalp, {}, o).cells.empty());
}

TEST(RootSubdivision, CellsTileTheTubeAllAlongWithNoPenetration) {
    // The cut tube twisted, flattened and tapered: its cells still tile it all along, a hair apart,
    // none reaching into another.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    o.segments = 8;
    const std::vector<Vec3f> path{{0, 1, 0}, {0.05f, 1.3f, 0}, {0.25f, 1.55f, 0.05f}, {0.55f, 1.7f, 0.1f}};
    const GuideId root = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.3f, 12), path, o);
    TubeProfile shaped = h.guide(root)->tube;
    const std::size_t n = h.guide(root)->points.size();
    shaped.width.resize(n), shaped.aspect.resize(n), shaped.twist.resize(n);
    const float w0 = h.guide(root)->tube.widthAt(0.0f), a0 = h.guide(root)->tube.aspectAt(0.0f), t0 = h.guide(root)->tube.twistAt(0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        shaped.width[i] = w0 * (1.0f - 0.5f * t);
        shaped.aspect[i] = a0 * (1.0f - 0.4f * t);
        shaped.twist[i] = t0 + 0.8f * t;
    }
    h.setTube(root, shaped);
    h.evaluate(scalp.get());
    const TubeSource source = tubeSourceOf(h, *scalp, root);
    const TubeSpine pspine = sourceSpine(source, h.scale());
    const std::vector<GuideId> cells = subdivideRootTubes(h, *scalp, {root}).cells;
    ASSERT_GE(cells.size(), 4u);
    EXPECT_TRUE(tubeOverlaps(h, cells).empty()) << "tubeOverlaps finds cells passing into each other";
    std::vector<TubeSpine> spines;
    for (const GuideId c : cells) {
        const HierarchyGuide* g = h.guide(c);
        ASSERT_EQ(g->points.size(), n);
        spines.emplace_back(g->points.data(), n, g->frameUp, g->tube, h.scale());
    }
    for (std::size_t i = 1; i < n; ++i) {
        std::vector<std::vector<SectionPoint>> at(cells.size());
        for (std::size_t k = 0; k < cells.size(); ++k) {
            for (const SectionPoint& q : tubeOutline(h.guide(cells[k])->tube)) at[k].push_back(inSection(pspine, i, spines[k].atSample(i, q.u, q.v)));
        }
        for (std::size_t a = 0; a < cells.size(); ++a) {
            for (std::size_t b = 0; b < cells.size(); ++b) {
                if (a == b) continue;
                for (const SectionPoint& q : at[a]) {
                    EXPECT_FALSE(deepInside(at[b], q, 0.0f)) << "cell " << a << " reaches into cell " << b << " at sample " << i;
                }
            }
        }
    }
}

TEST(RootSubdivision, CornersTooCloseAreMadeOne) {
    // Four sites nearly on one circle: two Voronoi corners a sliver apart, made one.
    const std::vector<CutPoint> square{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::vector<CutPoint> sites{{0.3, 0.3}, {0.7, 0.3}, {0.3, 0.7}, {0.7, 0.71}};
    auto shortest = [&](const std::vector<std::vector<CutPoint>>& cells) {
        double best = 1e9;
        for (const auto& cell : cells) {
            for (std::size_t i = 0; i < cell.size(); ++i) {
                const CutPoint& a = cell[i];
                const CutPoint& b = cell[(i + 1) % cell.size()];
                best = std::min(best, std::hypot(a.x - b.x, a.y - b.y));
            }
        }
        return best;
    };
    EXPECT_LT(shortest(voronoiCells(square, sites)), 0.02);
    const auto merged = voronoiCells(square, sites, 0.05);
    EXPECT_GE(shortest(merged), 0.05);
    double total = 0.0;
    for (const auto& cell : merged) total += area(cell);
    EXPECT_NEAR(total, 1.0, 1e-9);  // (still tiling the square, its corners where they were)
}

TEST(RootSlices, TheRootLoopFollowsTheOutlineDrawn) {
    // A big root on a curved scalp: its tube's root loop runs along the drawn outline's edges as
    // they lie on the scalp (CV to CV), not the flat footprint's.
    GuideHierarchy h;
    const auto scalp = head();
    const std::vector<Vec3f> cvs = ring({0, 1, 0}, 0.45f, 6);
    const GuideId g = makeRootTube(h, *scalp, cvs, {});
    h.evaluate(scalp.get());
    const HierarchyGuide* root = h.guide(g);
    ASSERT_EQ(root->rootRing.size(), tubeOutline(root->tube).size());
    // The drawn edges on the scalp, finely.
    std::vector<Vec3f> drawn;
    for (std::size_t k = 0; k < cvs.size(); ++k) {
        const Vec3f a = root->rootOutline.empty() ? cvs[k] : scalp->position(root->rootOutline[k]);
        const Vec3f b = root->rootOutline.empty() ? cvs[(k + 1) % cvs.size()] : scalp->position(root->rootOutline[(k + 1) % cvs.size()]);
        for (int s = 0; s < 200; ++s) drawn.push_back(scalp->closestPoint(a + (b - a) * (s / 200.0f)).position);
    }
    float off = 0.0f;
    for (const Vec3f& p : root->rootRing) off = std::max(off, nearestOf(p, drawn));
    EXPECT_LT(off, 0.004f) << "the root loop leaves the outline drawn";
}

TEST(RootSubdivision, EachCellsGuideSitsInItsMiddleOnACurvedScalp) {
    // A big root over much of the sphere (its flat footprint far off the scalp at its edge): each
    // cell's guide still roots in its cell's middle, its tube starting where its hair grows.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    o.segments = 3;  // (a guide of few points, as drawn quickly)
    const std::vector<Vec3f> path{{0, 1, 0}, {0.2f, 1.35f, 0.0f}, {0.6f, 1.55f, 0.1f}};
    const GuideId root = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.7f, 10), path, o);
    RootSubdivisionOptions cut;
    cut.multiplier = 2.0f;
    h.evaluate(scalp.get());
    const TubeSpine pspine = sourceSpine(tubeSourceOf(h, *scalp, root), h.scale());
    const std::size_t n = h.guide(root)->points.size();
    const auto cells = subdivideRootTubes(h, *scalp, {root}, cut).cells;
    ASSERT_GE(cells.size(), 6u);
    // And they tile it past the root, none reaching into another.
    for (std::size_t i = 1; i < n; ++i) {
        std::vector<std::vector<SectionPoint>> at;
        for (const GuideId c : cells) {
            const HierarchyGuide* g = h.guide(c);
            ASSERT_EQ(g->points.size(), n);
            const TubeSpine spine(g->points.data(), n, g->frameUp, g->tube, h.scale());
            at.emplace_back();
            for (const SectionPoint& q : tubeOutline(g->tube)) at.back().push_back(inSection(pspine, i, spine.atSample(i, q.u, q.v)));
        }
        for (std::size_t a = 0; a < at.size(); ++a) {
            for (std::size_t b = 0; b < at.size(); ++b) {
                if (a == b) continue;
                for (const SectionPoint& q : at[a]) EXPECT_FALSE(deepInside(at[b], q, 0.0f)) << "cell " << a << " reaches into " << b << " at " << i;
            }
        }
    }
    for (const GuideId c : cells) {
        const std::vector<Vec3f> outline = rootTubeOutline(h, *scalp, c);
        const Vec3f mid = scalpPointNormal(*scalp, outlineCentroid(outline)).point;
        float size = 0.0f;
        for (const Vec3f& q : outline) size = std::max(size, (q - mid).length());
        EXPECT_LT((h.guide(c)->points.front() - mid).length(), 0.35f * size) << "cell " << c << "'s guide off its middle";
        EXPECT_LT(std::abs(h.guide(c)->points.front().length() - 1.0f), 1e-3f) << "on the scalp";
    }
}

TEST(RootSubdivision, EditingACellsRootLeavesItsGuideWithThePack) {
    // A cell's root edited (moved a little): its guide goes on the way it went, never stood up.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    const std::vector<Vec3f> path{{0, 1, 0}, {0.3f, 1.1f, 0.0f}, {0.8f, 1.15f, 0.0f}};  // leaning hard over
    const GuideId root = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.35f, 10), path, o);
    const auto cells = subdivideRootTubes(h, *scalp, {root}).cells;
    ASSERT_FALSE(cells.empty());
    const GuideId c = cells[0];
    const std::vector<Vec3f> before = h.guide(c)->points;
    const Vec3f wayBefore = (before.back() - before.front()).normalized();
    std::vector<Vec3f> outline = rootTubeOutline(h, *scalp, c);
    const Vec3f mid = outlineCentroid(outline);
    outline = carryOutline(*scalp, outline, mid, (mid + Vec3f{0.01f, 0.0f, 0.01f}).normalized());
    editRootTube(h, *scalp, c, outline);
    const std::vector<Vec3f>& after = h.guide(c)->points;
    const Vec3f wayAfter = (after.back() - after.front()).normalized();
    EXPECT_GT(wayBefore.dot(wayAfter), std::cos(5.0f * kPi / 180.0f));
    EXPECT_GT(((before[1] - before[0]).normalized()).dot((after[1] - after[0]).normalized()), std::cos(8.0f * kPi / 180.0f));
}

TEST(RootSubdivision, ApplyCutsAgainAllOverEvenWithNothingChanged) {
    GuideHierarchy h;
    const auto scalp = head();
    RootSubdivisionOptions o;
    o.map = subdivisionDensityMapName(1);
    const auto first = subdivideRootTubes(h, *scalp, {bigRoot(h, *scalp)}, o).cells;
    EXPECT_TRUE(applySubdivisionDensity(h, *scalp, {}, o).cells.empty());  // (nothing changed: nothing)
    o.force = true;
    const auto again = applySubdivisionDensity(h, *scalp, {}, o);
    EXPECT_EQ(again.removed, first.size());
    // (Thrown again, the same density and multiplier: about as many cells.)
    EXPECT_NEAR(static_cast<double>(again.cells.size()), static_cast<double>(first.size()), 0.35 * first.size());
}

TEST(RootSubdivision, ACellCutAgainLeavesItsFirstCut) {
    // A cell of cut 01 cut again (its own cut, map 02): painting 01 again never touches it, its
    // cells or its place (no new cell of 01 reaches into it).
    const ScalpMesh before = painted([](const Vec3f&) { return 0.3f; });
    GuideHierarchy h;
    RootSubdivisionOptions one;
    one.map = "tubeSubdivisionDensity";  // (the painted sphere's map)
    const auto cells = subdivideRootTubes(h, before, {bigRoot(h, before)}, one).cells;
    ASSERT_GE(cells.size(), 4u);
    // The middle-most cell cut again with a map of its own.
    GuideId inner = cells[0];
    for (const GuideId c : cells) {
        if (h.guide(c)->points.front().y > h.guide(inner)->points.front().y) inner = c;
    }
    const std::vector<Vec3f> place = rootTubeOutline(h, before, inner);
    RootSubdivisionOptions two;
    two.map = subdivisionDensityMapName(2);
    two.multiplier = 2.0f;
    const auto sub = subdivideRootTubes(h, before, {inner}, two).cells;
    ASSERT_GE(sub.size(), 2u);
    std::map<GuideId, std::vector<Vec3f>> had;
    for (const GuideId c : sub) had[c] = h.guide(c)->points;
    for (const GuideId c : sub) EXPECT_EQ(h.guide(c)->parent, inner) << "the cells of a cell cut again: its children";
    EXPECT_EQ(h.rootSubdivisions().size(), 1u) << "a destructive cut is not remembered";
    // Cut 01 painted denser everywhere and applied: its new cells stay out of the cell cut again.
    const ScalpMesh after = painted([](const Vec3f&) { return 1.0f; });
    const auto again = applySubdivisionDensity(h, after, {}, one);
    EXPECT_FALSE(again.cells.empty());
    for (const auto& [c, pts] : had) {
        ASSERT_NE(h.guide(c), nullptr) << "a cell of the second cut went";
        for (std::size_t i = 0; i < pts.size(); ++i) EXPECT_LT((h.guide(c)->points[i] - pts[i]).length(), 1e-5f);
    }
    // (In the plane of its place: none of cut 01's new roots' CVs inside it.)
    const Vec3f mid = outlineCentroid(place).normalized();
    const Vec3f a = mid.cross({0, 0, 1}).normalized(), b = mid.cross(a);
    std::vector<SectionPoint> poly;
    for (const Vec3f& q : place) poly.push_back({q.dot(a), q.dot(b)});
    float size = 0.0f;
    for (const SectionPoint& q : poly) size = std::max(size, std::hypot(q.u - poly[0].u, q.v - poly[0].v));
    for (const GuideId c : again.cells) {
        for (const Vec3f& q : rootTubeOutline(h, after, c)) {
            EXPECT_FALSE(deepInside(poly, {q.dot(a), q.dot(b)}, 0.02f * size)) << "a new root of cut 01 inside the cell cut again";
        }
    }
}

TEST(RootSubdivision, TubeOverlapsFindsTubesPassingIntoEachOther) {
    // Two cells, one pushed half its width into the other: the measure says so.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 0.8f;
    const auto cells = subdivideRootTubes(h, *scalp, {makeRootTube(h, *scalp, ring({0, 1, 0}, 0.3f, 10), {}, o)}).cells;
    ASSERT_GE(cells.size(), 2u);
    EXPECT_TRUE(tubeOverlaps(h).empty());
    // The cell nearest the first, moved halfway onto it along the tube.
    GuideId other = cells[1];
    for (const GuideId c : cells) {
        if (c != cells[0] && (h.guide(c)->points.front() - h.guide(cells[0])->points.front()).length() <
                                 (h.guide(other)->points.front() - h.guide(cells[0])->points.front()).length()) {
            other = c;
        }
    }
    std::vector<Vec3f> pts = h.guide(other)->points;
    const Vec3f toward = (h.guide(cells[0])->points.front() - pts.front()) * 0.5f;
    for (std::size_t i = 1; i < pts.size(); ++i) pts[i] = pts[i] + toward;
    h.setPoints(other, pts);
    h.evaluate(scalp.get());
    const auto found = tubeOverlaps(h);
    ASSERT_FALSE(found.empty());
    EXPECT_GT(found.front().depth, 0.1f);
}

TEST(RootSubdivision, ACutWhoseRootsAreAllGoneIsForgotten) {
    // Its cells deleted: the cut forgets them at once (a later guide of the same ID is not its),
    // and the next cut lets the cut go.
    GuideHierarchy h;
    const auto scalp = head();
    RootSubdivisionOptions o;
    o.map = subdivisionDensityMapName(1);
    const auto cells = subdivideRootTubes(h, *scalp, {bigRoot(h, *scalp)}, o).cells;
    deleteRootTubes(h, *scalp, cells);
    ASSERT_EQ(h.rootSubdivisions().size(), 1u);
    for (const GuideId c : h.rootSubdivisions()[0].cells) EXPECT_EQ(c, kNoGuide);
    RootTubeOptions r;
    const GuideId b = makeRootTube(h, *scalp, ring(Vec3f{0.0f, 0.6f, 0.8f}.normalized(), 0.25f, 8), {}, r);
    o.map = subdivisionDensityMapName(2);
    subdivideRootTubes(h, *scalp, {b}, o);
    ASSERT_EQ(h.rootSubdivisions().size(), 1u);
    EXPECT_EQ(h.rootSubdivisions()[0].map, subdivisionDensityMapName(2));
}

TEST(RootSubdivision, ApplyKeepsTheOuterEdgesAsEdited) {
    // A cut root's outer CV dragged out (Edit roots on the cell holding it): Apply fills the outline
    // as the cells' outer edges are now, the CV where it was dragged.
    GuideHierarchy h;
    const auto scalp = head();
    RootSubdivisionOptions o;
    o.map = subdivisionDensityMapName(1);
    const auto cells = subdivideRootTubes(h, *scalp, {bigRoot(h, *scalp)}, o).cells;
    // One of the old outline's CVs held by a single cell.
    std::vector<Vec3f> outline;
    for (const StrandRoot& r : h.rootSubdivisions()[0].outline) outline.push_back(scalp->position(r));
    GuideId owner = kNoGuide;
    std::size_t at = 0;
    Vec3f cv;
    for (const Vec3f& q : outline) {
        std::vector<std::pair<GuideId, std::size_t>> holders;
        for (const GuideId c : cells) {
            const std::vector<Vec3f> cvs = rootTubeOutline(h, *scalp, c);
            for (std::size_t k = 0; k < cvs.size(); ++k) {
                if ((cvs[k] - q).length() < 1e-4f) holders.push_back({c, k});
            }
        }
        if (holders.size() == 1) {
            owner = holders[0].first, at = holders[0].second, cv = q;
            break;
        }
    }
    ASSERT_NE(owner, kNoGuide);
    std::vector<Vec3f> edited = rootTubeOutline(h, *scalp, owner);
    const Vec3f out = (cv - Vec3f{0, 1, 0} * cv.dot({0, 1, 0})).normalized();  // (away from the root's middle)
    const Vec3f moved = (cv + out * 0.06f).normalized();
    edited[at] = moved;
    editRootTube(h, *scalp, owner, edited);
    o.force = true;
    const auto again = applySubdivisionDensity(h, *scalp, {}, o);
    ASSERT_FALSE(again.cells.empty());
    float nearest = 1e9f;
    for (const GuideId c : again.cells) {
        for (const Vec3f& q : rootTubeOutline(h, *scalp, c)) nearest = std::min(nearest, (q - moved).length());
    }
    EXPECT_LT(nearest, 1e-3f) << "the dragged CV is not on the new cut's outline";
    float stayed = 1e9f;
    for (const StrandRoot& r : h.rootSubdivisions()[0].outline) stayed = std::min(stayed, (scalp->position(r) - moved).length());
    EXPECT_LT(stayed, 1e-3f) << "the cut's outline did not take the edit";
}

// Issues #110 and #111: a slice's points under the scalp (its parent's sections dipping into the
// head where the parent leans over) are lifted over it smoothly, not pressed onto it point by
// point, and a slice hanging past the open edge of a scalp stays with its parent.

// The sphere's upper half: an open scalp, its rim the equator (a scalp cut at the neck: the rim's
// normal points out sideways).
std::shared_ptr<const ScalpMesh> openHead() {
    static const std::shared_ptr<const ScalpMesh> cap = [] {
        const auto whole = head();
        std::vector<ScalpMesh::Triangle> kept;
        for (const auto& t : whole->triangles()) {
            const auto y = [&](int c) { return whole->points()[static_cast<std::size_t>(t[c])].y; };
            if (y(0) >= -1e-4f && y(1) >= -1e-4f && y(2) >= -1e-4f) kept.push_back(t);
        }
        return std::make_shared<const ScalpMesh>(std::vector<Vec3f>(whole->points().begin(), whole->points().end()), kept);
    }();
    return cap;
}

// Whether p lies under `scalp` (behind it across the surface, deeper than `slack`).
bool underScalp(const ScalpMesh& scalp, const Vec3f& p, float slack = 1e-4f) {
    const SurfacePoint sp = scalp.closestPoint(p);
    return sp.root.face >= 0 && belowScalpAt(p, sp, -slack);
}

// The largest turn between a guide's consecutive spans, in degrees.
float sharpestTurn(const std::vector<Vec3f>& pts) {
    float worst = 0.0f;
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        const Vec3f a = (pts[i] - pts[i - 1]).normalized(), b = (pts[i + 1] - pts[i]).normalized();
        worst = std::max(worst, std::acos(std::clamp(a.dot(b), -1.0f, 1.0f)) * 180.0f / kPi);
    }
    return worst;
}

TEST(RootSlices, SlicesOfALeaningTubeRiseOverTheScalpSmoothly) {
    // Issue #110: a wide root combed to lean far over: its sections near the root dip into the
    // head on the side it leans to, and so would its slices there. They are lifted over the scalp
    // in a smooth curve, their tubes resting on it, never crumpled onto it nor folded back.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.length = 1.2f;
    o.segments = 16;
    // (An oval long along the way it leans: the sections near the root stand its length on end, its
    // far half down into the head.)
    std::vector<Vec3f> oval;
    for (int k = 0; k < 10; ++k) {
        const float a = 2.0f * kPi * static_cast<float>(k) / 10.0f;
        oval.push_back(Vec3f{0.35f * std::cos(a), 1.0f, 0.12f * std::sin(a)}.normalized());
    }
    const GuideId root = makeRootTube(h, *scalp, oval, {}, o);
    addTubeLevel(h, scalp.get(), {root});
    addTubeLevel(h, scalp.get(), {root});
    // Level 1 combed down to lie along the scalp, just over it.
    std::vector<Vec3f> combed = h.guide(root)->points;
    for (std::size_t i = 1; i < combed.size(); ++i) {
        const float a = 1.4f * static_cast<float>(i) / static_cast<float>(combed.size() - 1);
        const float rise = std::min(1.0f, a / 0.35f);  // (leaving the scalp over its first stretch)
        combed[i] = Vec3f{std::sin(a), std::cos(a), 0.0f} * (1.0f + 0.06f * rise * rise * (3.0f - 2.0f * rise));
    }
    h.setPoints(root, combed);
    h.evaluate(scalp.get());
    std::size_t slices = 0;
    std::vector<GuideId> todo = h.childrenOf(root);
    while (!todo.empty()) {
        const GuideId k = todo.back();
        todo.pop_back();
        for (const GuideId c : h.childrenOf(k)) todo.push_back(c);
        const HierarchyGuide* g = h.guide(k);
        ++slices;
        std::size_t pressed = 0;
        for (std::size_t i = 1; i < g->points.size(); ++i) {
            EXPECT_FALSE(underScalp(*scalp, g->points[i])) << "slice " << k << " point " << i << " under the scalp";
            pressed += (g->points[i] - scalp->closestPoint(g->points[i]).position).length() < 1e-3f;
        }
        EXPECT_EQ(pressed, 0u) << "slice " << k << " pressed onto the scalp point by point (its tube half in it)";
        EXPECT_LT(sharpestTurn(g->points), 60.0f) << "slice " << k << " crumpled or folded back";
    }
    EXPECT_EQ(slices, 6u);
    // (Smoothed once, as split: a brush giving a slice back its own points moves nothing.)
    GuideHierarchy again = h;
    for (const HierarchyGuide& g : h.guides()) {
        if (g.isChild()) again.setPoints(g.id, g.points);
    }
    again.evaluate(scalp.get());
    for (const HierarchyGuide& g : h.guides()) {
        if (!g.isChild()) continue;
        for (std::size_t i = 0; i < g.points.size(); ++i) {
            EXPECT_LT((again.guide(g.id)->points[i] - g.points[i]).length(), 1e-3f) << "slice " << g.id << " point " << i << " moved";
        }
    }
}

TEST(RootSlices, ASliceHangingPastAnOpenScalpsEdgeStaysWithItsParent) {
    // Issue #111: a root near the rim of an open scalp, its tube hanging down past the rim: its
    // slices stay round it all along, none pulled up to the rim (the scalp's point nearest a point
    // below the rim is on the rim, the point off to its side rather than behind the surface).
    GuideHierarchy h;
    const auto scalp = openHead();
    RootTubeOptions o;
    o.segments = 16;
    const Vec3f at = Vec3f{0.94f, 0.34f, 0.0f}.normalized();
    const GuideId root = makeRootTube(h, *scalp, ring(at, 0.12f, 10), {}, o);
    addTubeLevel(h, scalp.get(), {root});
    // Level 1 combed out and down past the rim, hanging just inside the sphere's line below it.
    std::vector<Vec3f> combed = h.guide(root)->points;
    for (std::size_t i = 1; i < combed.size(); ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(combed.size() - 1);
        combed[i] = f < 0.25f ? at + (Vec3f{1.08f, 0.2f, 0.0f} - at) * (f / 0.25f) : Vec3f{1.08f - 0.25f * (f - 0.25f) / 0.75f, 0.2f - 1.6f * (f - 0.25f) / 0.75f, 0.0f};
    }
    h.setPoints(root, combed);
    h.evaluate(scalp.get());
    const HierarchyGuide* parent = h.guide(root);
    const float reach = 3.0f * parent->tube.widthAt(0.0f) * h.scale();  // (the slices lie within its tube)
    const std::vector<GuideId> kids = h.childrenOf(root);
    ASSERT_EQ(kids.size(), 2u);
    for (const GuideId k : kids) {
        const HierarchyGuide* g = h.guide(k);
        ASSERT_EQ(g->points.size(), parent->points.size());
        float farthest = 0.0f;
        for (std::size_t i = 0; i < g->points.size(); ++i) farthest = std::max(farthest, (g->points[i] - parent->points[i]).length());
        EXPECT_LT(farthest, reach) << "slice " << k << " left its parent (pulled to the rim?)";
        EXPECT_LT(g->points.back().y, -1.0f) << "slice " << k << "'s tip taken up to the rim";
    }
}

TEST(RootSlices, SlicesGoIntoTheScalpWithTheirParent) {
    // Issue #111: a parent combed into the head mid-length (Keep off scalp off) takes its slices
    // in with it there: they keep to its tube, never pushed off a scalp it passes through.
    GuideHierarchy h;
    const auto scalp = head();
    RootTubeOptions o;
    o.segments = 16;
    const std::vector<Vec3f> path{{0, 1, 0}, {0.45f, 1.12f, 0}, {0.85f, 0.75f, 0}, {0.8f, 0.3f, 0}, {0.95f, -0.3f, 0}, {1.0f, -0.8f, 0}};
    const GuideId root = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.12f, 10), path, o);
    addTubeLevel(h, scalp.get(), {root});
    // (Combed into the head along its middle, as a brush with Keep off scalp off leaves it.)
    std::vector<Vec3f> into = h.guide(root)->points;
    for (std::size_t i = 0; i < into.size(); ++i) {
        const float w = std::sin(kPi * static_cast<float>(i) / static_cast<float>(into.size() - 1));
        into[i] = into[i] * (1.0f - 0.2f * w * w);
    }
    h.setPoints(root, into);
    h.evaluate(scalp.get());
    const HierarchyGuide* parent = h.guide(root);
    std::size_t inside = 0;
    for (std::size_t i = 0; i < parent->points.size(); ++i) inside += underScalp(*scalp, parent->points[i]) ? 1 : 0;
    ASSERT_GT(inside, 0u) << "the parent goes into the head";
    const float reach = 3.0f * parent->tube.widthAt(0.0f) * h.scale();
    for (const GuideId k : h.childrenOf(root)) {
        const HierarchyGuide* g = h.guide(k);
        ASSERT_EQ(g->points.size(), parent->points.size());
        for (std::size_t i = 0; i < g->points.size(); ++i) {
            EXPECT_LT((g->points[i] - parent->points[i]).length(), reach) << "slice " << k << " point " << i << " pushed off its parent";
        }
        EXPECT_LT(sharpestTurn(g->points), 60.0f) << "slice " << k << " kinked";
    }
}

TEST(Brush, KeepOffScalpLeavesHairHangingPastAnOpenEdge) {
    // Issue #111: the brushes' Keep off scalp (keepOffScalp) slides a guide along the scalp where it
    // goes in, but a guide hanging down past the open rim of a scalp is not in it: left as it is.
    const auto scalp = openHead();
    const std::vector<Vec3f> hanging{{0.94f, 0.34f, 0.0f}, {1.05f, 0.1f, 0.0f}, {0.98f, -0.3f, 0.0f}, {0.95f, -0.8f, 0.0f}, {0.95f, -1.3f, 0.0f}};
    const std::vector<Vec3f> kept = keepOffScalp(hanging, *scalp);
    ASSERT_EQ(kept.size(), hanging.size());
    for (std::size_t i = 0; i < hanging.size(); ++i) EXPECT_LT((kept[i] - hanging[i]).length(), 1e-4f) << "point " << i;
    // Behind the surface it is still taken out.
    const std::vector<Vec3f> into{{0.0f, 1.0f, 0.0f}, {0.0f, 0.8f, 0.0f}, {0.0f, 0.6f, 0.0f}};
    for (const Vec3f& p : keepOffScalp(into, *scalp)) EXPECT_FALSE(underScalp(*scalp, p, 1e-3f));
}

}  // namespace
}  // namespace groomlab
