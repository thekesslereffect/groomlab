// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

TEST(Curve, ArcLengthAndResample) {
    const std::vector<Vec3f> pts = {{0, 0, 0}, {0, 1, 0}, {0, 3, 0}};
    EXPECT_FLOAT_EQ(arcLength(pts.data(), pts.size()), 3.0f);

    const auto out = resampleByArcLength(pts.data(), pts.size(), 4);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NEAR(out[1].y, 1.0f, 1e-5f);
    EXPECT_NEAR(out[2].y, 2.0f, 1e-5f);
    EXPECT_NEAR(out[3].y, 3.0f, 1e-5f);
}

TEST(Curve, ResampleFraction) {
    const std::vector<Vec3f> pts = {{0, 0, 0}, {0, 4, 0}};
    const auto out = resampleByArcLength(pts.data(), pts.size(), 3, 0.5f);
    EXPECT_NEAR(out.back().y, 2.0f, 1e-5f);
    EXPECT_NEAR(out[1].y, 1.0f, 1e-5f);
}

TEST(Curve, DegenerateInputs) {
    const std::vector<Vec3f> one = {{1, 2, 3}};
    const auto out = resampleByArcLength(one.data(), one.size(), 3);
    for (const Vec3f& p : out) EXPECT_EQ(p, Vec3f(1, 2, 3));
    EXPECT_TRUE(resampleByArcLength(one.data(), one.size(), 0).empty());
}

TEST(Curve, SmoothPolylinePassesThroughThePointsWithoutOvershoot) {
    // A right-angle bend with a long and a short leg.
    const std::vector<Vec3f> pts = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0.2f, 2, 0}, {0.4f, 2, 0}};
    const auto out = smoothPolyline(pts.data(), pts.size(), 4);
    ASSERT_EQ(out.size(), (pts.size() - 1) * 4 + 1);
    for (std::size_t i = 0; i < pts.size(); ++i) EXPECT_LT((out[i * 4] - pts[i]).length(), 1e-5f);
    for (const Vec3f& p : out) {
        EXPECT_GE(p.x, -0.06f);  // centripetal: a slight bow into the corner, no loop or swing past it
        EXPECT_LE(p.y, 2.02f);
    }
    // A straight line stays straight; below 2 subdivisions or 3 points: as given.
    const std::vector<Vec3f> line = {{0, 0, 0}, {1, 0, 0}, {3, 0, 0}};
    for (const Vec3f& p : smoothPolyline(line.data(), line.size(), 5)) EXPECT_NEAR(p.y, 0.0f, 1e-6f);
    EXPECT_EQ(smoothPolyline(pts.data(), pts.size(), 1).size(), pts.size());
    EXPECT_EQ(smoothPolyline(pts.data(), 2, 4).size(), 2u);
}

TEST(Curve, ResamplingAlongTheSmoothCurveKeepsABendRound) {
    // Six points on a half circle to 40: every new point stays on the circle (straight spans
    // would cut inside it by up to 1 - cos(18 deg) ~ 5%).
    std::vector<Vec3f> arc;
    for (int k = 0; k <= 5; ++k) {
        const float a = 3.14159265f * static_cast<float>(k) / 5.0f;
        arc.push_back({std::cos(a), std::sin(a), 0.0f});
    }
    const std::vector<Vec3f> out = resampleSmoothCurve(arc.data(), arc.size(), 41);
    ASSERT_EQ(out.size(), 41u);
    for (const Vec3f& p : out) EXPECT_NEAR(std::hypot(p.x, p.y), 1.0f, 0.01f);
    EXPECT_LT((out.front() - arc.front()).length(), 1e-5f);
    EXPECT_LT((out.back() - arc.back()).length(), 1e-5f);
    // Even spacing along the curve.
    const float step = (out[1] - out[0]).length();
    for (std::size_t i = 1; i < out.size(); ++i) EXPECT_NEAR((out[i] - out[i - 1]).length(), step, 0.05f * step);
    // A straight line stays straight; two points as resampleByArcLength.
    const std::vector<Vec3f> line = {{0, 0, 0}, {1, 0, 0}, {3, 0, 0}};
    for (const Vec3f& p : resampleSmoothCurve(line.data(), line.size(), 9)) EXPECT_NEAR(p.y, 0.0f, 1e-6f);
    EXPECT_EQ(resampleSmoothCurve(line.data(), 2, 5).size(), 5u);
}

TEST(Curve, SmoothPolylineEndsKeepBending) {
    // Six points on a half circle: the first and last spans follow the circle too, as a guide
    // combed into a curl keeps curling to its tip.
    std::vector<Vec3f> arc;
    for (int k = 0; k <= 5; ++k) {
        const float a = 3.14159265f * static_cast<float>(k) / 5.0f;
        arc.push_back({std::cos(a), std::sin(a), 0.0f});
    }
    const int sub = 8;
    const auto out = smoothPolyline(arc.data(), arc.size(), sub);
    for (int span : {0, 4}) {
        for (int k = 1; k < sub; ++k) {
            const Vec3f& p = out[static_cast<std::size_t>(span * sub + k)];
            EXPECT_NEAR(std::hypot(p.x, p.y), 1.0f, 0.01f) << "span " << span << " sample " << k;
        }
    }
}

TEST(ScalpMesh, AreaAndNormals) {
    const auto mesh = test::makePlane(4, 2.0f);
    EXPECT_EQ(mesh->triangleCount(), 32u);
    EXPECT_NEAR(mesh->totalArea(), 4.0f, 1e-5f);

    const StrandRoot root{5, 0.25f, 0.25f};
    const Vec3f n = mesh->normal(root);
    EXPECT_NEAR(n.y, 1.0f, 1e-5f);
    EXPECT_NEAR(mesh->position(root).y, 0.0f, 1e-6f);
}

TEST(ScalpMesh, PositionAndNormalOnEveryFace) {
    // position() is the barycentric blend of the face's corners (no bounds check on the hot
    // path: callers validate the face); normal() is unit, and at a corner the same from every
    // face around it (the vertex normal).
    const auto mesh = test::makePlane(4, 2.0f);
    const std::vector<Vec3f>& points = mesh->points();
    std::map<std::int32_t, Vec3f> cornerNormals;
    for (std::size_t f = 0; f < mesh->triangleCount(); ++f) {
        const ScalpMesh::Triangle& tri = mesh->triangles()[f];
        const StrandRoot inside{static_cast<std::int32_t>(f), 0.2f, 0.3f};
        const Vec3f want = points[static_cast<std::size_t>(tri[0])] * 0.5f + points[static_cast<std::size_t>(tri[1])] * 0.2f +
                           points[static_cast<std::size_t>(tri[2])] * 0.3f;
        EXPECT_NEAR((mesh->position(inside) - want).length(), 0.0f, 1e-6f) << "face " << f;
        EXPECT_NEAR(mesh->normal(inside).length(), 1.0f, 1e-5f) << "face " << f;
        const StrandRoot corners[3] = {{static_cast<std::int32_t>(f), 0.0f, 0.0f}, {static_cast<std::int32_t>(f), 1.0f, 0.0f},
                                       {static_cast<std::int32_t>(f), 0.0f, 1.0f}};
        for (int k = 0; k < 3; ++k) {
            const Vec3f n = mesh->normal(corners[k]);
            const auto [it, fresh] = cornerNormals.emplace(tri[static_cast<std::size_t>(k)], n);
            if (!fresh) EXPECT_NEAR((it->second - n).length(), 0.0f, 1e-5f) << "face " << f << " corner " << k;
            EXPECT_NEAR((mesh->position(corners[k]) - points[static_cast<std::size_t>(tri[static_cast<std::size_t>(k)])]).length(), 0.0f, 1e-6f);
        }
    }
    EXPECT_EQ(cornerNormals.size(), points.size());
}

TEST(ScalpMesh, ClosestPointProjectsOntoSurface) {
    const auto mesh = test::makePlane(4, 1.0f);
    const SurfacePoint sp = mesh->closestPoint({0.3f, 0.7f, 0.6f});
    ASSERT_GE(sp.root.face, 0);
    EXPECT_NEAR(sp.position.x, 0.3f, 1e-5f);
    EXPECT_NEAR(sp.position.y, 0.0f, 1e-6f);
    EXPECT_NEAR(sp.position.z, 0.6f, 1e-5f);
    // Round-trips through the stored barycentrics.
    const Vec3f again = mesh->position(sp.root);
    EXPECT_NEAR((again - sp.position).length(), 0.0f, 1e-5f);

    // Outside the square clamps to the nearest edge.
    const SurfacePoint edge = mesh->closestPoint({-1.0f, 0.0f, 0.5f});
    EXPECT_NEAR(edge.position.x, 0.0f, 1e-5f);
}

TEST(ScalpMesh, ClosestBarycentricOnDegenerateTrianglesIsNeverNaN) {
    // Two corners at one point (a welded seam's sliver), or all three on a line: a point on the
    // triangle, never NaN (it was 0/0 there).
    const Vec3f a(0, 0, 0), b(1, 0, 0), c(2, 0, 0);
    struct Case {
        Vec3f p, a, b, c;
    };
    for (const Case& k : {Case{{3, 1, 0}, a, a, b}, Case{{0.5f, 1, 0}, a, a, b}, Case{{-1, 1, 0}, a, b, b},
                          Case{{0.5f, 1, 0}, a, b, c}, Case{{5, 1, 0}, a, b, c}, Case{{1, 1, 0}, a, a, a}}) {
        float u = -1, v = -1;
        closestBarycentric(k.p, k.a, k.b, k.c, u, v);
        ASSERT_TRUE(std::isfinite(u) && std::isfinite(v)) << k.p.x;
        const Vec3f q = k.a + (k.b - k.a) * u + (k.c - k.a) * v;
        EXPECT_TRUE(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z));
        EXPECT_GE(q.x, -1e-6f);
        EXPECT_LE(q.x, 2.0f + 1e-6f);
    }
    // A plain triangle is as before.
    float u = 0, v = 0;
    closestBarycentric({0.25f, 1, 0.25f}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, u, v);
    EXPECT_FLOAT_EQ(u, 0.25f);
    EXPECT_FLOAT_EQ(v, 0.25f);
}

TEST(ScalpMesh, TreeQueryMatchesBruteForce) {
    const auto mesh = test::makeSphere(40, 80, 1.0f);  // 6400 triangles
    Rng rng(7);
    for (int i = 0; i < 3000; ++i) {
        // Points inside, on and outside the sphere.
        const Vec3f p = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() *
                        rng.uniform(0.2f, 2.5f);
        const SurfacePoint fast = mesh->closestPoint(p);
        const SurfacePoint slow = mesh->closestPointBruteForce(p);
        ASSERT_GE(fast.root.face, 0);
        // Ties on shared edges may pick a different face, never a farther point.
        EXPECT_NEAR((fast.position - p).length(), (slow.position - p).length(), 1e-5f) << "query " << i;
        EXPECT_NEAR((mesh->position(fast.root) - fast.position).length(), 0.0f, 1e-5f);
    }
}

TEST(ScalpMesh, BoundedQueryGivesALowerBoundOfTheDistance) {
    // The same point as the plain query, and a distance nothing on the mesh is closer than.
    const auto mesh = test::makeSphere(40, 80, 1.0f);
    Rng rng(11);
    for (int i = 0; i < 3000; ++i) {
        const Vec3f p = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() *
                        rng.uniform(0.2f, 2.5f);
        const float radius = rng.uniform(0.01f, 0.6f);
        float bound = -1.0f;
        const SurfacePoint hit = mesh->closestPointBounded(p, radius, &bound);
        const SurfacePoint plain = mesh->closestPoint(p, radius);
        EXPECT_EQ(hit.root.face, plain.root.face);
        EXPECT_EQ(hit.root.u, plain.root.u);
        EXPECT_EQ(hit.root.v, plain.root.v);
        const float truth = (mesh->closestPointBruteForce(p).position - p).length();
        EXPECT_LE(bound, truth * (1.0f + 1e-5f) + 1e-6f) << "query " << i;
        if (hit.root.face < 0) EXPECT_GE(bound, radius * (1.0f - 1e-6f)) << "query " << i;
    }
}

TEST(ScalpMesh, TreeQueryIsMuchFaster) {
    const auto mesh = test::makeSphere(100, 200, 1.0f);  // 40000 triangles
    std::vector<Vec3f> queries;
    Rng rng(3);
    for (int i = 0; i < 200; ++i) {
        queries.push_back(Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() * 1.1f);
    }
    mesh->closestPoint(queries[0]);  // build the tree outside the timing

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    float sink = 0;
    for (const Vec3f& q : queries) sink += mesh->closestPoint(q).position.x;
    const double fast = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    t0 = clock::now();
    for (const Vec3f& q : queries) sink -= mesh->closestPointBruteForce(q).position.x;
    const double slow = std::chrono::duration<double, std::milli>(clock::now() - t0).count();

    RecordProperty("tree_ms", std::to_string(fast));
    RecordProperty("brute_ms", std::to_string(slow));
    std::printf("[ closest ] 200 queries on 40k triangles: tree %.2f ms, brute force %.2f ms\n", fast, slow);
    EXPECT_NEAR(sink, 0.0f, 1e-3f);
    EXPECT_LT(fast * 20.0, slow);
}

TEST(ScalpMesh, HashTracksContent) {
    EXPECT_EQ(test::makePlane(3)->contentHash(), test::makePlane(3)->contentHash());
    EXPECT_NE(test::makePlane(3)->contentHash(), test::makePlane(4)->contentHash());
}

TEST(GuideSet, RootOnMeshSnapsFirstPoint) {
    const auto mesh = test::makePlane(4, 1.0f);
    GuideSet guides;
    guides.add(1, test::straightCurve({0.5f, 0.2f, 0.5f}, {0, 1, 0}));
    guides.rootOnMesh(*mesh);

    const Guide& g = guides.guides()[0];
    EXPECT_GE(g.root.face, 0);
    EXPECT_NEAR(g.points.front().y, 0.0f, 1e-6f);
    EXPECT_NEAR(g.points.back().y, 1.0f, 1e-5f);  // moved rigidly: 1.2 - 0.2
}

TEST(Curve, ArcParamsAndValuesResampledByArcLength) {
    // Uneven spacing: 0, 1, 3 along x.
    const std::vector<Vec3f> pts{{0, 0, 0}, {1, 0, 0}, {3, 0, 0}};
    float along[3], at[3];
    cumulativeArcLength(pts.data(), 3, along);
    EXPECT_FLOAT_EQ(along[2], 3.0f);
    EXPECT_FLOAT_EQ(along[2], arcLength(pts));
    arcParams(pts.data(), 3, at);
    EXPECT_FLOAT_EQ(at[1], 1.0f / 3.0f);
    const std::vector<Vec3f> still(3, Vec3f{1, 1, 1});
    arcParams(still.data(), 3, at);  // no length: by index
    EXPECT_FLOAT_EQ(at[1], 0.5f);
    // A value per point (0, 1, 3: the arc length itself) resampled evenly by arc length reads 0, 1, 2, 3.
    const float values[3] = {0.0f, 1.0f, 3.0f};
    float out[4];
    resampleValuesByArcLength(pts.data(), 3, values, 3, out, 4);
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(out[k], static_cast<float>(k), 1e-5f);
}

TEST(Frames, RotationTakesFromOntoTo) {
    const Vec3f up{0, 1, 0};
    for (const Vec3f& to : {Vec3f{0, 1, 0}, Vec3f{0, -1, 0}, Vec3f(1, 1, 0).normalized(), Vec3f(0.2f, -0.9f, 0.4f).normalized()}) {
        const Rotation turn(up, to);
        EXPECT_NEAR((turn(up) - to).length(), 0.0f, 1e-5f);
        const Vec3f v{0.3f, 0.5f, -0.7f};
        EXPECT_NEAR(turn(v).length(), v.length(), 1e-5f);                    // a rotation
        EXPECT_NEAR((turn(v) - rotateBetween(v, up, to)).length(), 0.0f, 1e-6f);  // the one rotateBetween applies
    }
}

TEST(Frames, FourAtOnceGiveWhatOneAtATimeGives) {
    // Random wiggly polylines, and the cases the scalar code branches on: repeated points
    // (the frame carried over), a strand doubling back on itself (a reversed tangent), an
    // initial normal along the first segment, and a zero-length strand.
    Rng rng(21);
    auto wiggly = [&](std::size_t n) {
        std::vector<Vec3f> p(n);
        Vec3f at(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        for (auto& q : p) {
            q = at;
            at += Vec3f(rng.uniform(-0.3f, 0.3f), rng.uniform(0.0f, 0.5f), rng.uniform(-0.3f, 0.3f));
        }
        return p;
    };
    for (std::size_t n : {2u, 3u, 9u, 16u}) {
        for (int round = 0; round < 50; ++round) {
            std::vector<Vec3f> strands[4];
            Vec3f normals[4];
            for (int k = 0; k < 4; ++k) {
                strands[k] = wiggly(n);
                normals[k] = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
            }
            const int odd = round % 5;
            if (odd == 1 && n > 2) strands[1][n / 2] = strands[1][n / 2 - 1];  // a repeated point
            if (odd == 2 && n > 2) strands[2][n - 1] = strands[2][n - 3];      // doubling back
            if (odd == 3) normals[3] = strands[3][1] - strands[3][0];          // normal along the strand
            if (odd == 4) std::fill(strands[0].begin(), strands[0].end(), strands[0][0]);  // all one point
            const Vec3f* pts[4];
            std::vector<Frame> four[4];
            Frame* outs[4];
            for (int k = 0; k < 4; ++k) {
                pts[k] = strands[k].data();
                four[k].resize(n);
                outs[k] = four[k].data();
            }
            rotationMinimizingFrames4(pts, n, normals, outs);
            for (int k = 0; k < 4; ++k) {
                const std::vector<Frame> one = rotationMinimizingFrames(pts[k], n, normals[k]);
                for (std::size_t i = 0; i < n; ++i) {
                    // Bit for bit: compared as floats would let -0 equal 0 (and fail NaN).
                    ASSERT_EQ(std::memcmp(&one[i], &four[k][i], sizeof(Frame)), 0)
                        << "n " << n << " round " << round << " lane " << k << " point " << i;
                }
            }
        }
    }
}

TEST(Frames, TurnedSumsGiveWhatOneVectorAtATimeGives) {
    // Rotation::addTurned (rotations made and vectors summed in lanes) against Rotation's
    // operator() one vector at a time, summed curve after curve: random directions, and the
    // cases the constructor branches on (the same direction: identity; opposite: a half turn;
    // nearly so), curves left unturned, more curves than a group of 16, and lengths that are
    // not a multiple of 4; every curve turned some general way (a loop of its own); and with some
    // curves twisted on about `to` (TwistedTurn's matrix: Interpolate round a fold).
    Rng rng(31);
    auto direction = [&] { return Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized(); };
    auto check = [&](bool mixed, bool twisted, std::size_t n, std::size_t count) {
        const std::size_t stride = (n + 3) & ~std::size_t{3};
        const Vec3f to = direction();
        std::vector<Vec3f> from(count);
        std::vector<char> turned(count);
        std::vector<float> w(count), twists(count, 0.0f);
        std::vector<std::vector<float>> curves(count, std::vector<float>(3 * stride, 0.0f));
        std::vector<const float*> rows(count);
        for (std::size_t i = 0; i < count; ++i) {
            switch (mixed ? i % 6 : 5) {
                case 0: from[i] = to; break;                                             // identity
                case 1: from[i] = -to; break;                                            // half turn
                case 2: from[i] = (to + Vec3f(1e-4f, 0.0f, 0.0f)).normalized(); break;   // nearly the same
                case 3: from[i] = (-to + Vec3f(0.0f, 2e-4f, 0.0f)).normalized(); break;  // nearly opposite
                default: from[i] = direction(); break;
            }
            turned[i] = mixed && i % 5 == 4 ? 0 : 1;
            w[i] = rng.uniform(0.0f, 2.0f);
            if (twisted && i % 3 != 1) twists[i] = i % 3 == 0 ? 3.1415927f : rng.uniform(-3.0f, 3.0f);
            for (std::size_t j = 0; j < n; ++j) {
                curves[i][j] = rng.uniform(-3, 3);
                curves[i][stride + j] = j % 7 == 3 ? -0.0f : rng.uniform(-3, 3);
                curves[i][2 * stride + j] = rng.uniform(-3, 3);
            }
            rows[i] = curves[i].data();
        }
        std::vector<float> sums(3 * stride);
        for (std::size_t j = 0; j < n; ++j) {
            sums[j] = rng.uniform(-1, 1);
            sums[stride + j] = -0.0f;
            sums[2 * stride + j] = rng.uniform(-1, 1);
        }
        std::vector<Vec3f> expected(n);
        for (std::size_t j = 0; j < n; ++j) expected[j] = Vec3f(sums[j], sums[stride + j], sums[2 * stride + j]);
        for (std::size_t i = 0; i < count; ++i) {
            const Rotation turn = turned[i] ? Rotation(from[i], to) : Rotation();
            const TwistedTurn twist(from[i], to, twists[i]);
            for (std::size_t j = 0; j < n; ++j) {
                const Vec3f v(rows[i][j], rows[i][stride + j], rows[i][2 * stride + j]);
                expected[j] += (turned[i] && twists[i] != 0.0f ? twist(v) : turn(v)) * w[i];
            }
        }
        Rotation::addTurned(from.data(), turned.data(), to, rows.data(), w.data(), count, n, stride, sums.data(),
                            twisted ? twists.data() : nullptr);
        for (std::size_t j = 0; j < n; ++j) {
            const Vec3f got(sums[j], sums[stride + j], sums[2 * stride + j]);
            // Bit for bit: compared as floats would let -0 equal 0.
            ASSERT_EQ(std::memcmp(&got, &expected[j], sizeof(Vec3f)), 0)
                << (mixed ? "mixed" : "general") << (twisted ? " twisted" : "") << " n " << n << " count " << count << " point " << j;
        }
    };
    for (const bool twisted : {false, true}) {
        for (const bool mixed : {true, false}) {
            for (std::size_t n : {1u, 4u, 7u, 32u, 33u}) {
                for (std::size_t count : {1u, 3u, 6u, 16u, 21u}) check(mixed, twisted, n, count);
            }
        }
    }
}

}  // namespace
}  // namespace groomlab
