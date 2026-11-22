// SPDX-License-Identifier: Apache-2.0
// Modeled tubes read off a mesh (tube_mesh.h): open quad tubes read loop by loop (a pointed tip,
// bevels averaged away, shells of one mesh apart, meshes that are not tubes said), root first,
// their shape and center curves, each tube's region value, and the sections fitted to a tube.
// (Maya's test_tube_mesh.py pins the same through its two utilities, on the same tubes.)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/tube_mesh.h"

namespace groomlab {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Mesh {
    std::vector<Vec3d> points;
    std::vector<std::vector<int>> polygons;
};

struct TubeSpec {
    Vec3d root{0.0, 1.0, 0.0};
    double length = 1.2;
    int loops = 10, sides = 8;
    double halfWidth = 0.15, aspect = 0.5, taper = 0.3, twist = kPi / 2.0, bend = 0.25;
    int bevelLoop = -1, bevelRun = -1;  // a second loop just past this loop, a second run just past this run (a crease's bevel)
};

// A modeled tube's rings: from its root along a gentle bend, an ellipse (halfWidth, aspect)
// tapering to `taper` of itself and turning `twist` root to tip.
std::vector<TubeRing> tubeRings(const TubeSpec& s = {}) {
    const Vec3d up = s.root.normalized();
    const Vec3d side = (std::abs(up.z) < 0.9 ? up.cross({0.0, 0.0, 1.0}) : up.cross({1.0, 0.0, 0.0})).normalized();
    std::vector<double> angles;
    for (int i = 0; i < s.sides; ++i) angles.push_back(2.0 * kPi * i / s.sides);
    if (s.bevelRun >= 0) {
        angles.insert(angles.begin() + s.bevelRun + 1, angles[static_cast<std::size_t>(s.bevelRun)] + 0.08 * 2.0 * kPi / s.sides);
    }
    std::vector<double> ts;
    for (int k = 0; k < s.loops; ++k) ts.push_back(k / static_cast<double>(s.loops - 1));
    if (s.bevelLoop >= 0) ts.insert(ts.begin() + s.bevelLoop + 1, ts[static_cast<std::size_t>(s.bevelLoop)] + 0.03 / (s.loops - 1));
    std::vector<TubeRing> rings;
    for (const double t : ts) {
        const Vec3d center = s.root + up * (s.length * t) + side * (s.bend * t * t);
        const Vec3d tangent = (up * s.length + side * (2.0 * s.bend * t)).normalized();
        const Vec3d a = tangent.cross(side.cross(tangent)).normalized();
        const Vec3d b = tangent.cross(a);
        const double w = s.halfWidth * (1.0 + (s.taper - 1.0) * t), h = w * s.aspect, turn = s.twist * t;
        TubeRing ring;
        for (const double angle : angles) {
            const double x0 = w * std::cos(angle), y0 = h * std::sin(angle);
            const double x = x0 * std::cos(turn) - y0 * std::sin(turn), y = x0 * std::sin(turn) + y0 * std::cos(turn);
            ring.push_back(center + a * x + b * y);
        }
        rings.push_back(ring);
    }
    return rings;
}

// Points and quads of an open tube through `rings` (the last closing on its tip with `pointed`).
Mesh grid(const std::vector<TubeRing>& rings, bool pointed = false) {
    Mesh mesh;
    const int sides = static_cast<int>(rings.front().size()), loops = static_cast<int>(rings.size());
    for (const TubeRing& r : rings) mesh.points.insert(mesh.points.end(), r.begin(), r.end());
    for (int k = 0; k + 1 < loops; ++k) {
        for (int i = 0; i < sides; ++i) {
            const int j = (i + 1) % sides;
            mesh.polygons.push_back({k * sides + i, k * sides + j, (k + 1) * sides + j, (k + 1) * sides + i});
        }
    }
    if (pointed) {
        Vec3d tip;
        for (const Vec3d& p : rings.back()) tip = tip + p * (1.0 / sides);
        mesh.points.push_back(tip + (tip - sectionCenter(rings.front())).normalized() * 0.1);
        const int last = (loops - 1) * sides, apex = static_cast<int>(mesh.points.size()) - 1;
        for (int i = 0; i < sides; ++i) mesh.polygons.push_back({last + i, last + (i + 1) % sides, apex});
    }
    return mesh;
}

Mesh joined(const Mesh& a, const Mesh& b) {
    Mesh out = a;
    const int offset = static_cast<int>(a.points.size());
    out.points.insert(out.points.end(), b.points.begin(), b.points.end());
    for (std::vector<int> poly : b.polygons) {
        for (int& v : poly) v += offset;
        out.polygons.push_back(poly);
    }
    return out;
}

std::vector<TubeRing> reversed(std::vector<TubeRing> rings) {
    std::reverse(rings.begin(), rings.end());
    return rings;
}

double distance(const Vec3d& a, const Vec3d& b) { return (a - b).length(); }

std::string whyNot(const Mesh& mesh, const std::string& name = "tube") {
    try {
        readTubes(mesh.points, mesh.polygons, name);
    } catch (const TubeMeshError& e) {
        return e.what();
    }
    return {};
}

// ---- Reading ---------------------------------------------------------------------------------------------

TEST(TubeMesh, AnOpenTubeReadsLoopByLoop) {
    TubeSpec spec;
    spec.loops = 6;
    const std::vector<TubeRing> rings = tubeRings(spec);
    const Mesh mesh = grid(rings);
    const std::vector<ModeledTube> tubes = readTubes(mesh.points, mesh.polygons, "lock");
    ASSERT_EQ(tubes.size(), 1u);
    const ModeledTube& tube = tubes[0];
    EXPECT_EQ(tube.name, "lock");
    EXPECT_EQ(tube.rings.size(), 6u);
    EXPECT_EQ(tube.sides(), 8u);
    EXPECT_FALSE(tube.pointed);
    EXPECT_EQ(tube.readLoops, 6);
    EXPECT_EQ(tube.readSides, 8);
    // Ring 0 the loop of the lowest vertex, every ring the modeled loop, its columns in step.
    for (std::size_t k = 0; k < rings.size(); ++k) {
        std::set<std::size_t> matched;
        for (const Vec3d& p : tube.rings[k]) {
            for (std::size_t i = 0; i < rings[k].size(); ++i) {
                if (p == rings[k][i]) matched.insert(i);
            }
        }
        EXPECT_EQ(matched.size(), 8u) << "ring " << k;
    }
    const std::vector<std::vector<Vec3d>> columns = tube.columns();
    ASSERT_EQ(columns.size(), 8u);
    EXPECT_EQ(columns[0].size(), 6u);
    for (std::size_t k = 0; k < 6; ++k) EXPECT_EQ(columns[3][k], tube.rings[k][3]);
    // Along a column the points are neighbours on the mesh: one modeled edge run.
    const std::size_t first = static_cast<std::size_t>(std::find(rings[0].begin(), rings[0].end(), columns[0][0]) - rings[0].begin());
    for (std::size_t k = 0; k < 6; ++k) EXPECT_EQ(columns[0][k], rings[k][first]);
}

TEST(TubeMesh, AStraightTubesCentersRunUpItsMiddle) {
    TubeSpec spec;
    spec.loops = 5;
    spec.bend = 0.0;
    spec.twist = 0.0;
    spec.aspect = 1.0;
    spec.taper = 1.0;
    const Mesh mesh = grid(tubeRings(spec));
    const ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    const std::vector<Vec3d> centers = tube.centers();
    ASSERT_EQ(centers.size(), 5u);
    for (std::size_t k = 0; k < 5; ++k) {
        EXPECT_NEAR(distance(centers[k], Vec3d{0.0, 1.0 + 1.2 * static_cast<double>(k) / 4.0, 0.0}), 0.0, 1e-12);
        EXPECT_NEAR(sectionRadius(tube.rings[k]), 0.15, 1e-12);
    }
}

TEST(TubeMesh, ABentTubesCentersFollowTheBend) {
    TubeSpec spec;
    const std::vector<TubeRing> rings = tubeRings(spec);
    const Mesh mesh = grid(rings);
    const ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    const Vec3d side = Vec3d{0.0, 1.0, 0.0}.cross({0.0, 0.0, 1.0});
    const std::vector<Vec3d> centers = tube.centers();
    for (std::size_t k = 0; k < centers.size(); ++k) {
        const double t = static_cast<double>(k) / 9.0;
        // An ellipse's centroid is its middle: on the bend's curve.
        EXPECT_NEAR(distance(centers[k], Vec3d{0.0, 1.0 + 1.2 * t, 0.0} + side * (0.25 * t * t)), 0.0, 1e-9) << k;
    }
    EXPECT_GT(std::abs(centers.back().x), 0.2);
}

TEST(TubeMesh, APointedTipAndTheRootAtTheOpenEnd) {
    TubeSpec spec;
    spec.loops = 6;
    spec.sides = 6;
    const Mesh mesh = grid(tubeRings(spec), true);
    ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    EXPECT_TRUE(tube.pointed);
    ASSERT_EQ(tube.rings.size(), 7u);
    for (const Vec3d& p : tube.rings.back()) EXPECT_EQ(p, tube.rings.back()[0]);
    EXPECT_FALSE(orientTube(tube));  // the open end stays the root
    EXPECT_NE(tube.rings[0][0], tube.rings[0][1]);
    EXPECT_NEAR(distance(sectionCenter(tube.rings.back()), tube.rings.back()[0]), 0.0, 1e-12);  // (no area: the points' mean)
    // A scalp says otherwise: the end nearer it first, the tip's point now the root.
    const Vec3d tip = tube.rings.back()[0];
    EXPECT_TRUE(orientTube(tube, [&tip](const Vec3d& p) { return distance(p, tip); }));
    EXPECT_TRUE(tube.flipped);
    EXPECT_TRUE(tube.pointedRoot);
    EXPECT_FALSE(tube.pointed);
    EXPECT_EQ(tube.rings[0][0], tip);
}

TEST(TubeMesh, TheRootIsTheWiderEndWithoutAScalp) {
    TubeSpec spec;
    spec.loops = 5;
    const std::vector<TubeRing> rings = tubeRings(spec);
    const Mesh mesh = grid(reversed(rings));  // modeled tip first
    ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    EXPECT_TRUE(orientTube(tube));
    EXPECT_TRUE(tube.flipped);
    const auto [root, tip] = tube.endSizes();
    EXPECT_GT(root, tip);
    EXPECT_NEAR(distance(sectionCenter(tube.rings[0]), Vec3d{0.0, 1.0, 0.0}), 0.0, 1e-9);
    // With a scalp: the end nearer it, whatever its size.
    ModeledTube again = readTubes(mesh.points, mesh.polygons)[0];
    const Vec3d far{0.0, 3.0, 0.0};
    EXPECT_FALSE(orientTube(again, [&far](const Vec3d& p) { return distance(p, far); }));
    EXPECT_LT(again.endSizes().first, again.endSizes().second);
}

TEST(TubeMesh, BevelsAverageAway) {
    TubeSpec spec;
    spec.loops = 8;
    spec.bevelLoop = 3;
    spec.bevelRun = 2;
    Mesh mesh = grid(tubeRings(spec));
    ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    EXPECT_EQ(tube.rings.size(), 9u);
    EXPECT_EQ(tube.sides(), 9u);
    const TubeRing third = tube.rings[3], fourth = tube.rings[4];
    EXPECT_EQ(tube.mergeBevels(), std::make_pair(1, 1));
    EXPECT_EQ(tube.rings.size(), 8u);
    EXPECT_EQ(tube.sides(), 8u);
    EXPECT_EQ(tube.mergedLoops, 1);
    EXPECT_EQ(tube.mergedRuns, 1);
    EXPECT_EQ(tube.readLoops, 9);  // (as modeled)
    // The merged loop lies between the two it was.
    EXPECT_NEAR(distance(sectionCenter(tube.rings[3]), (sectionCenter(third) + sectionCenter(fourth)) * 0.5), 0.0, 1e-3);
    // A tapering tube's loops drawing closer toward its tip are no bevel.
    spec.bevelLoop = spec.bevelRun = -1;
    mesh = grid(tubeRings(spec));
    ModeledTube plain = readTubes(mesh.points, mesh.polygons)[0];
    EXPECT_EQ(plain.mergeBevels(), std::make_pair(0, 0));
    // A bevel at the root keeps the root's loop where it is.
    spec.bevelLoop = 0;
    mesh = grid(tubeRings(spec));
    ModeledTube rootBevel = readTubes(mesh.points, mesh.polygons)[0];
    const TubeRing rootLoop = rootBevel.rings[0];
    EXPECT_EQ(rootBevel.mergeBevels().first, 1);
    EXPECT_EQ(rootBevel.rings[0], rootLoop);
}

TEST(TubeMesh, ShellsOfOneMeshAndWhatIsNotATube) {
    TubeSpec a, b;
    a.loops = 4;
    b.loops = 5;
    b.root = {1.0, 0.0, 0.0};
    const Mesh both = joined(grid(tubeRings(a)), grid(tubeRings(b)));
    const std::vector<ModeledTube> tubes = readTubes(both.points, both.polygons, "locks");
    ASSERT_EQ(tubes.size(), 2u);
    EXPECT_EQ(tubes[0].name, "locks_1");
    EXPECT_EQ(tubes[1].name, "locks_2");
    EXPECT_EQ(tubes[0].shell, 0);
    EXPECT_EQ(tubes[1].shell, 1);
    EXPECT_EQ(tubes[0].rings.size(), 4u);
    EXPECT_EQ(tubes[1].rings.size(), 5u);

    // A capped tube: said, with what to do.
    TubeSpec small;
    small.loops = 4;
    small.sides = 4;
    Mesh capped = grid(tubeRings(small));
    capped.polygons.push_back({3, 2, 1, 0});
    capped.polygons.push_back({12, 13, 14, 15});
    EXPECT_NE(whyNot(capped, "capped").find("capped: it is closed"), std::string::npos) << whyNot(capped, "capped");
    // One cap: an open end and nothing to walk to.
    Mesh oneCap = grid(tubeRings(small));
    oneCap.polygons.push_back({12, 13, 14, 15});
    EXPECT_FALSE(whyNot(oneCap).empty());
    // A flat grid: one open loop round it, no tube.
    Mesh sheet;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) sheet.points.push_back({static_cast<double>(x), static_cast<double>(y), 0.0});
    }
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) sheet.polygons.push_back({y * 3 + x, y * 3 + x + 1, (y + 1) * 3 + x + 1, (y + 1) * 3 + x});
    }
    EXPECT_NE(whyNot(sheet, "sheet").find("sheet: "), std::string::npos);
    // A tube with a hole cut in its side: three open loops.
    Mesh holed = grid(tubeRings(a));
    holed.polygons.erase(holed.polygons.begin() + 9);
    EXPECT_NE(whyNot(holed).find("3 open edge loops"), std::string::npos) << whyNot(holed);
    // A fin on an edge: non-manifold, the mesh named.
    Mesh fin = grid(tubeRings(a));
    fin.points.push_back({5.0, 5.0, 5.0});
    fin.polygons.push_back({8, 9, static_cast<int>(fin.points.size()) - 1});
    EXPECT_NE(whyNot(fin, "fin").find("fin has edges shared by more than two faces"), std::string::npos);
    // A tube beside a shell that is none: the whole mesh is refused, the shell named.
    const Mesh mixed = joined(grid(tubeRings(a)), sheet);
    EXPECT_NE(whyNot(mixed, "mixed").find("mixed_2: "), std::string::npos) << whyNot(mixed, "mixed");
    // Vertices no face uses are no shell.
    Mesh loose = grid(tubeRings(a));
    loose.points.push_back({9.0, 9.0, 9.0});
    EXPECT_EQ(readTubes(loose.points, loose.polygons).size(), 1u);
    // A face naming a vertex the mesh has not.
    Mesh bad = grid(tubeRings(a));
    bad.polygons[0][0] = 999;
    EXPECT_THROW(readTubes(bad.points, bad.polygons), std::invalid_argument);
}

TEST(TubeMesh, TheSmoothSurface) {
    TubeSpec spec;
    spec.bend = 0.0;
    spec.twist = 0.0;
    spec.aspect = 1.0;
    spec.taper = 1.0;
    spec.loops = 6;
    const Mesh mesh = grid(tubeRings(spec));
    const std::vector<Vec3d> limit = limitPoints(mesh.points, mesh.polygons);
    ASSERT_EQ(limit.size(), mesh.points.size());
    // A straight round tube of 8 sides: every vertex moves in toward the axis and stays at its height
    // (inner loops), the open ends' loops smooth curves in their own planes.
    for (std::size_t i = 0; i < limit.size(); ++i) {
        const double before = std::hypot(mesh.points[i].x, mesh.points[i].z), after = std::hypot(limit[i].x, limit[i].z);
        EXPECT_LT(after, before);
        EXPECT_GT(after, 0.85 * before);
        EXPECT_NEAR(limit[i].y, mesh.points[i].y, 1e-12);
    }
    // An end loop's limit: the cubic B-spline through the loop, (p[i-1] + 4 p[i] + p[i+1]) / 6.
    const Vec3d expected = (mesh.points[7] + mesh.points[0] * 4.0 + mesh.points[1]) * (1.0 / 6.0);
    EXPECT_NEAR(distance(limit[0], expected), 0.0, 1e-12);

    ModeledTube tube = readTubes(mesh.points, mesh.polygons, "tube", true)[0];
    ASSERT_TRUE(tube.smoothRings.has_value());
    EXPECT_FALSE(tube.smooth());
    const std::vector<TubeRing> cage = tube.rings;
    tube.useSmooth();
    EXPECT_TRUE(tube.smooth());
    EXPECT_FALSE(tube.smoothRings.has_value());
    EXPECT_EQ(*tube.cageRings, cage);
    EXPECT_LT(sectionRadius(tube.rings[2]), sectionRadius(cage[2]));
    // Its sections as drawn: 4 points an edge, the first of each span the vertex's limit point.
    const std::vector<TubeRing> loops = tube.sectionLoops();
    ASSERT_EQ(loops.size(), 6u);
    EXPECT_EQ(loops[2].size(), 32u);
    for (std::size_t k = 0; k < loops.size(); ++k) {
        for (std::size_t i = 0; i < 8; ++i) EXPECT_NEAR(distance(loops[k][4 * i], tube.rings[k][i]), 0.0, 1e-12);
    }
    int per = 0;
    EXPECT_EQ(tubeSurfaceColumns(tube, &per).size(), 32u);
    EXPECT_EQ(per, 4);
    // As modeled: the loops themselves.
    const ModeledTube plain = readTubes(mesh.points, mesh.polygons)[0];
    EXPECT_EQ(plain.sectionLoops(), plain.rings);
    EXPECT_EQ(tubeSurfaceColumns(plain, &per).size(), 8u);
    EXPECT_EQ(per, 1);
}

TEST(TubeMesh, PreparedAsTheConversionsTakeIt) {
    TubeSpec spec;
    spec.bevelLoop = 4;
    const std::vector<TubeRing> rings = tubeRings(spec);
    const Mesh mesh = grid(reversed(rings), false);
    ModeledTube tube = readTubes(mesh.points, mesh.polygons, "lock", true)[0];
    prepareTube(tube, true);
    EXPECT_TRUE(tube.flipped);
    EXPECT_TRUE(tube.smooth());
    EXPECT_EQ(tube.rings.size(), 10u);  // 11 loops, the bevel's two as one
    EXPECT_EQ(tube.cageRings->size(), 10u);
    EXPECT_EQ(tube.readLoops, 11);
    EXPECT_GT(tube.endSizes().first, tube.endSizes().second);
    ModeledTube kept = readTubes(mesh.points, mesh.polygons, "lock", false)[0];
    prepareTube(kept, false);
    EXPECT_EQ(kept.rings.size(), 11u);
    EXPECT_FALSE(kept.smooth());
}

// ---- Curves ------------------------------------------------------------------------------------------------

TEST(TubeMesh, VolumeCurvesStayInsideTheTube) {
    EXPECT_TRUE(volumeLayout(1).empty());
    const auto four = volumeLayout(4);
    ASSERT_EQ(four.size(), 3u);
    EXPECT_NEAR(four[0].second, 0.0, 1e-12);
    EXPECT_NEAR(four[1].second, 1.0 / 3.0, 1e-12);
    EXPECT_NEAR(four[2].second, 2.0 / 3.0, 1e-12);
    for (const int count : {2, 4, 7, 10, 20, 50}) {
        const auto layout = volumeLayout(count);
        EXPECT_EQ(layout.size(), static_cast<std::size_t>(count - 1));
        std::set<std::pair<long long, long long>> distinct;
        for (const auto& [radius, turn] : layout) {
            distinct.insert({std::llround(radius * 1e6), std::llround(std::fmod(turn, 1.0) * 1e6)});
            EXPECT_GT(radius, 0.0);
            EXPECT_LE(radius, volumeReach(count) + 1e-9);
        }
        EXPECT_EQ(distinct.size(), layout.size()) << count;
    }
    // A few curves stay near the center; more spread out, never past kTubeVolumeReach.
    EXPECT_NEAR(volumeReach(4), 0.6, 1e-12);
    EXPECT_LT(volumeReach(4), volumeReach(10));
    EXPECT_EQ(volumeReach(100), kTubeVolumeReach);
    // 19 curves: the center, 6 round it and 12 round those, as a honeycomb.
    const auto honeycomb = volumeLayout(19);
    EXPECT_EQ(std::count_if(honeycomb.begin(), honeycomb.end(), [&](const auto& c) { return c.first < 0.5 * volumeReach(19) + 1e-9; }), 6);

    TubeSpec spec;
    spec.bevelLoop = 4;
    const Mesh mesh = grid(tubeRings(spec), true);
    ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    prepareTube(tube, true);
    const std::vector<std::vector<Vec3d>> curves = tube.volumeCurves(10);
    ASSERT_EQ(curves.size(), 10u);
    EXPECT_EQ(curves[0], tube.centers());
    for (const std::vector<Vec3d>& curve : curves) {
        ASSERT_EQ(curve.size(), tube.rings.size());
        for (std::size_t k = 0; k < curve.size(); ++k) {
            EXPECT_LE(distance(curve[k], sectionCenter(tube.rings[k])), sectionRadius(tube.rings[k]) * 0.95 + 1e-9) << k;
        }
    }
    // The way round a section, by length.
    const TubeRing square{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    EXPECT_NEAR(distance(ringPoint(square, 0.125), Vec3d{0.5, 0.0, 0.0}), 0.0, 1e-12);
    EXPECT_NEAR(distance(ringPoint(square, 0.75), Vec3d{0.0, 1.0, 0.0}), 0.0, 1e-12);
    EXPECT_NEAR(distance(ringPoint(square, 1.5), Vec3d{1.0, 1.0, 0.0}), 0.0, 1e-12);
}

TEST(TubeMesh, CurvesRootOnTheScalpAndKeepTheirPlacePastTheRoot) {
    const std::vector<Vec3d> curve{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}};
    const std::vector<Vec3d> rooted = rootedCurve(curve, {0.3, -0.1, 0.0});
    EXPECT_NEAR(distance(rooted[0], Vec3d{0.3, -0.1, 0.0}), 0.0, 1e-12);
    for (std::size_t i = 1; i < curve.size(); ++i) EXPECT_EQ(rooted[i], curve[i]);  // (the fade ends within the first span)
    // A point within the fade moves part of the way.
    const std::vector<Vec3d> fine{{0, 0, 0}, {0, 0.5, 0}, {0, 1, 0}, {0, 4, 0}};
    const std::vector<Vec3d> part = rootedCurve(fine, {0.4, 0.0, 0.0});
    EXPECT_NEAR(part[1].x, 0.2, 1e-12);  // (half way through the fade: half the move)
    EXPECT_EQ(part[2], fine[2]);
    EXPECT_EQ(rootedCurve({{1, 2, 3}}, {0, 0, 0}).front(), (Vec3d{1, 2, 3}));

    // A tube guide: its root on the scalp, its second point straight off it.
    const std::vector<Vec3d> path = tubeGuidePath({{0, 0.1, 0}, {0, 1, 0}, {0, 2, 0}}, {0, 0, 0}, {0, 1, 0});
    ASSERT_EQ(path.size(), 4u);
    EXPECT_EQ(path[0], (Vec3d{0, 0, 0}));
    EXPECT_NEAR(distance(path[1], Vec3d{0, kTubeRootLift, 0}), 0.0, 1e-12);
    EXPECT_EQ(path[2], (Vec3d{0, 1, 0}));
}

TEST(TubeMesh, EachTubeAValueOfItsOwn) {
    EXPECT_TRUE(tubeRegionValues(0).empty());
    EXPECT_EQ(tubeRegionValues(1), std::vector<double>{0.5});
    EXPECT_EQ(tubeRegionValues(2), (std::vector<double>{0.3333, 0.6667}));
    const std::vector<double> seven = tubeRegionValues(7);
    EXPECT_EQ(seven, (std::vector<double>{0.125, 0.5, 0.875, 0.375, 0.75, 0.25, 0.625}));
    // Distinct, neighbouring tubes far apart.
    for (const int count : {3, 7, 12, 40}) {
        const std::vector<double> values = tubeRegionValues(count);
        EXPECT_EQ(std::set<double>(values.begin(), values.end()).size(), static_cast<std::size_t>(count));
        for (std::size_t i = 1; i < values.size(); ++i) EXPECT_GT(std::abs(values[i] - values[i - 1]), 0.2) << count;
    }
    // Rounded to 4 places, a half to the even digit (as the Python's round).
    EXPECT_EQ(roundedTubeRegionValue(0.03125), 0.0312);
    EXPECT_EQ(roundedTubeRegionValue(0.09375), 0.0938);
    EXPECT_EQ(roundedTubeRegionValue(0.00625), 0.0063);  // (as a double a little over the half)
    EXPECT_EQ(roundedTubeRegionValue(0.01875), 0.0187);  // (a little under)
    EXPECT_EQ(tubeRegionValues(159)[0], 0.0063);
    EXPECT_EQ(roundedTubeRegionValue(static_cast<double>(0.3333f)), 0.3333);
    // Tubes added to a map: values well clear of the ones its tubes have.
    const std::vector<double> used = tubeRegionValues(2);
    const std::vector<double> more = extraTubeRegionValues(used, 3);
    ASSERT_EQ(more.size(), 3u);
    for (const double v : more) {
        for (const double u : used) EXPECT_GT(std::abs(v - u), 0.05);
    }
    EXPECT_EQ(std::set<double>(more.begin(), more.end()).size(), 3u);
    EXPECT_EQ(extraTubeRegionValues({}, 4), tubeRegionValues(4));

    EXPECT_TRUE(isTubeMapName("tubes"));
    EXPECT_TRUE(isTubeMapName("tubes12"));
    EXPECT_FALSE(isTubeMapName("tubesShape"));
    EXPECT_FALSE(isTubeMapName("tube"));
    EXPECT_FALSE(isTubeMapName(""));
    EXPECT_EQ(unusedName("tubes", {}), "tubes");
    EXPECT_EQ(unusedName("tubes", {"tubes", "tubes2", "other"}), "tubes3");
    EXPECT_EQ(tubeShapeNameFor("lock", {}, {"Round"}), "lock");
    EXPECT_EQ(tubeShapeNameFor(" side|lock:1 ", {}, {}), "side_lock_1");
    EXPECT_EQ(tubeShapeNameFor("round", {}, {"Round", "Ribbon"}), "round_shape");
    EXPECT_EQ(tubeShapeNameFor("lock", {"lock", "lock_2"}, {}), "lock_3");
    EXPECT_EQ(tubeShapeNameFor("  ", {}, {}), "tube");
    EXPECT_EQ(tubeShapeNameFor(std::string(80, 'a'), {}, {}).size(), 56u);
}

// ---- Sections ----------------------------------------------------------------------------------------------

std::vector<Vec2d> circle(int count, double radius = 1.0) {
    std::vector<Vec2d> out;
    for (int i = 0; i < count; ++i) out.push_back({radius * std::cos(2.0 * kPi * i / count), radius * std::sin(2.0 * kPi * i / count)});
    return out;
}

TEST(TubeMesh, CoverScale) {
    // A round section on a square outline of the same area pokes out at the square's sides: the
    // square drawn wider until at most 5% of the circle lies outside (about 1.10; all of it inside
    // takes 1.13, the circle's radius over the square's half side). A shape matching the section:
    // nothing to add.
    const std::vector<Vec2d> round = circle(96);
    const double half = std::sqrt(kPi) / 2.0;
    const std::vector<Vec2d> square{{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    const double k5 = coverScale(round, square, 0.05), k0 = coverScale(round, square, 0.0);
    EXPECT_GT(k5, 1.03);
    EXPECT_LT(k5, k0);
    EXPECT_NEAR(k0, 1.0 / half, 0.01);
    EXPECT_NEAR(coverScale(round, round, 0.05), 1.0, 1e-6);
    EXPECT_EQ(coverScale(round, circle(96, 0.1), 0.05), 1.35);  // (the cap)
    EXPECT_NEAR(*rayReach(square, 0.0), half, 1e-12);
    EXPECT_FALSE(rayReach({{2, 1}, {3, 1}, {3, 2}}, kPi).has_value());
}

TEST(TubeMesh, SectionFit) {
    std::vector<Vec2d> shape;
    for (int i = 0; i < 8; ++i) shape.push_back({std::cos(2.0 * kPi * i / 8), 0.5 * std::sin(2.0 * kPi * i / 8)});
    const double turn = 0.6, w = 0.3, h = 0.2;
    const std::vector<Vec2d> pts = placeOutline(shape, turn, w, h, {0.1, -0.05});
    const double nearTwist = 0.5;
    const SectionFit fit = fitSection(pts, shape, &nearTwist);
    EXPECT_NEAR(fit.twist, turn, 1e-3);
    EXPECT_NEAR(fit.width, w, 1e-4);
    EXPECT_NEAR(fit.height, h, 1e-4);
    EXPECT_NEAR(fit.center.x, 0.1, 1e-4);
    EXPECT_NEAR(fit.center.y, -0.05, 1e-4);
    // The same fit a turn away, nearest the last section's twist.
    const double far = 0.5 + 4.0 * kPi;
    EXPECT_NEAR(fitSection(pts, shape, &far).twist, turn + 4.0 * kPi, 1e-3);
    // A section with no size (a pointed tip) keeps the last twist.
    const SectionFit none = fitSection(std::vector<Vec2d>(8, Vec2d{0.2, 0.3}), shape, &nearTwist);
    EXPECT_EQ(none.twist, 0.5);
    EXPECT_EQ(none.width, 0.0);
    EXPECT_NEAR(none.center.x, 0.2, 1e-12);
    EXPECT_EQ(fitSection(std::vector<Vec2d>(8, Vec2d{0.2, 0.3}), shape).twist, 0.0);

    // The box that just holds a turned loop, and the outline's nearest point.
    const SectionFit box = sectionBox(placeOutline({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, 0.4, 0.3, 0.1, {0.2, 0.1}), 0.4);
    EXPECT_NEAR(box.width, 0.3, 1e-12);
    EXPECT_NEAR(box.height, 0.1, 1e-12);
    EXPECT_NEAR(box.center.x, 0.2, 1e-12);
    EXPECT_NEAR(box.center.y, 0.1, 1e-12);
    const std::vector<Vec2d> unit{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const auto [at, off] = closestOnOutline({0.5, -3.0}, placeOutline(unit, 0.0, 2.0, 1.0), unit);
    EXPECT_NEAR(at.x, 0.25, 1e-12);
    EXPECT_NEAR(at.y, -1.0, 1e-12);
    EXPECT_NEAR(off, 2.0, 1e-12);
}

TEST(TubeMesh, ShapePointsAndTheLibrarysFit) {
    // Clockwise points come back counter-clockwise, each saying which it was.
    const std::vector<Vec2d> clockwise{{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    std::vector<std::size_t> indices;
    const std::vector<Vec2d> pts = shapePoints(clockwise, &indices);
    EXPECT_GT(signedArea(pts), 0.0);
    EXPECT_EQ(indices, (std::vector<std::size_t>{3, 2, 1, 0}));
    // More than a shape takes: evenly picked.
    const std::vector<Vec2d> many = circle(96);
    const std::vector<Vec2d> picked = shapePoints(many, &indices);
    EXPECT_EQ(picked.size(), 64u);
    EXPECT_EQ(indices[0], 0u);
    EXPECT_EQ(indices[1], 2u);  // (1.5 rounds to the even 2, 3 stays, 4.5 to 4)
    EXPECT_EQ(indices[2], 3u);
    EXPECT_EQ(indices[3], 4u);

    // The library fits a shape into its square, maybe from another start: matched point for point.
    const std::vector<Vec2d> given{{0.3, 0.0}, {0.0, 0.1}, {-0.3, 0.0}, {0.0, -0.2}};
    std::vector<Vec2d> fitted;
    for (const Vec2d& p : given) fitted.push_back({p.x / 0.3, (p.y + 0.05) / 0.15});
    std::rotate(fitted.begin(), fitted.begin() + 1, fitted.end());
    const std::vector<Vec2d> match = matchFitted(given, fitted);
    ASSERT_EQ(match.size(), 4u);
    EXPECT_NEAR(match[0].x, 1.0, 1e-12);
    EXPECT_NEAR(match[3].y, -1.0, 1e-12);
    EXPECT_TRUE(matchFitted(given, {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}).empty());
    EXPECT_TRUE(matchFitted(given, circle(5)).empty());
}

// A guide's frames up the Y axis: u along X, v along Z turned so u x v points along the guide.
TubeFrames straightFrames(double from, double to, int points) {
    TubeFrames frames;
    for (int i = 0; i < points; ++i) {
        frames.points.push_back({0.0, from + (to - from) * i / (points - 1), 0.0});
        frames.u.push_back({1.0, 0.0, 0.0});
        frames.v.push_back({0.0, 0.0, -1.0});
    }
    return frames;
}

TEST(TubeMesh, SectionsFitTheModeledTube) {
    // A straight flat tube tapering to 0.3 of its root and turning a quarter turn root to tip.
    TubeSpec spec;
    spec.bend = 0.0;
    spec.loops = 13;
    spec.sides = 12;
    const Mesh mesh = grid(tubeRings(spec));
    ModeledTube tube = readTubes(mesh.points, mesh.polygons)[0];
    prepareTube(tube, true);
    const TubeFrames frames = straightFrames(1.0, 2.2, 13);
    EXPECT_NEAR(polylineParam(frames.points, sectionCenter(tube.rings[6])), 0.5, 1e-9);
    const TubeFrame mid = frameAt(frames, 0.5);
    EXPECT_NEAR(distance(mid.center, Vec3d{0.0, 1.6, 0.0}), 0.0, 1e-12);

    const TubeRootShape shape = tubeRootShape(tube, frames, 0.0);
    ASSERT_EQ(shape.points.size(), 12u);
    EXPECT_FALSE(shape.smooth);
    EXPECT_GT(signedArea(shape.points), 0.0);
    // The library's fit of it: into the -1..1 box.
    const SectionFit box = sectionBox(shape.points, 0.0);
    std::vector<Vec2d> library;
    for (const Vec2d& p : shape.points) library.push_back({(p.x - box.center.x) / box.width, (p.y - box.center.y) / box.height});
    std::vector<std::size_t> vertices(shape.vertices.size());
    std::vector<Vec2d> shapeUv(shape.vertices.size());
    for (std::size_t j = 0; j < shape.vertices.size(); ++j) vertices[j] = j;
    for (std::size_t j = 0; j < shape.vertices.size(); ++j) shapeUv[shape.vertices[j]] = library[j];

    const std::vector<TubeSectionFit> sections = fitTubeSections(frames, tube, shapeUv, vertices, 0, library, 0.05);
    ASSERT_EQ(sections.size(), 13u);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const double t = static_cast<double>(i) / 12.0;
        const TubeSectionFit& s = sections[i];
        EXPECT_NEAR(s.t, t, 1e-12);
        EXPECT_NEAR(distance(s.center, frames.points[i]), 0.0, 1e-6) << i;
        // Each modeled loop's own size, flatness and turn (the loops lie in the guide's planes).
        const double w = 0.15 * (1.0 + (0.3 - 1.0) * t);
        EXPECT_NEAR(s.width, w, 0.02 * w) << i;
        EXPECT_NEAR(s.height / s.width, 0.5, 0.02) << i;
        EXPECT_NEAR(std::abs(s.twist - sections[0].twist), kPi / 2.0 * t, 0.02) << i;
        EXPECT_NEAR(s.cover, 1.0, 0.02) << i;  // (the shape is the section's all along)
    }
    for (const double a : sectionAspects(sections)) EXPECT_NEAR(a, 0.5, 0.02);
    for (const double k : smoothedCover(sections, 2)) EXPECT_GE(k, 1.0);

    // The root points take the root loop itself; without an outline the sections are as boxed.
    const std::vector<TubeSectionFit> rootHeld = fitTubeSections(frames, tube, shapeUv, vertices, 2);
    EXPECT_NEAR(rootHeld[1].width, rootHeld[0].width, 1e-9);
    EXPECT_EQ(rootHeld[5].cover, 1.0);
    EXPECT_GT(rootHeld[1].width, sections[1].width);

    // Cover factors evened out along the tube, the root's left at 1; a section of no width keeps the last aspect.
    std::vector<TubeSectionFit> made(5);
    made[3].cover = 1.2;
    made[0].cover = 1.3;
    const std::vector<double> even = smoothedCover(made, 1);
    EXPECT_EQ(even[0], 1.0);
    EXPECT_GT(even[2], 1.0);
    EXPECT_LT(even[3], 1.2);
    EXPECT_GT(even[3], even[4]);
    made[0].width = 2.0, made[0].height = 1.0;
    EXPECT_EQ(sectionAspects(made), (std::vector<double>{0.5, 0.5, 0.5, 0.5, 0.5}));
}

TEST(TubeMesh, APointedTubesSectionsKeepTheLastTwist) {
    TubeSpec spec;
    spec.bend = 0.0;
    spec.loops = 6;
    const Mesh mesh = grid(tubeRings(spec), true);
    ModeledTube tube = readTubes(mesh.points, mesh.polygons, "lock", true)[0];
    prepareTube(tube, true);
    ASSERT_TRUE(tube.smooth());
    const double tip = tube.rings.back()[0].y;
    const TubeFrames frames = straightFrames(1.0, tip, 8);
    const TubeRootShape shape = tubeRootShape(tube, frames, 0.0);
    EXPECT_TRUE(shape.smooth);
    std::vector<std::size_t> vertices(shape.vertices.size());
    std::vector<Vec2d> shapeUv(shape.vertices.size());
    const SectionFit box = sectionBox(shape.points, 0.0);
    for (std::size_t j = 0; j < shape.vertices.size(); ++j) {
        vertices[j] = j;
        shapeUv[shape.vertices[j]] = {(shape.points[j].x - box.center.x) / box.width, (shape.points[j].y - box.center.y) / box.height};
    }
    const std::vector<TubeSectionFit> sections = fitTubeSections(frames, tube, shapeUv, vertices);
    ASSERT_EQ(sections.size(), 8u);
    EXPECT_EQ(sections.back().width, 0.0);
    EXPECT_EQ(sections.back().twist, sections[6].twist);
    EXPECT_GT(sections[0].width, sections[6].width);
    EXPECT_THROW(fitTubeSections(frames, tube, shapeUv, {99}), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
