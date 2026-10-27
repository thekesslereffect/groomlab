// SPDX-License-Identifier: Apache-2.0
// SelfCollide (self_collide_op.cpp): hair taken as an incompressible material. Where strands are
// packed tighter than `packing` they are pushed apart (a pressure solve on a grid), so layers that
// passed through one another rest on one another; hair that is not overpacked does not move.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/preview_core.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

StrandsPtr selfCollide(const StrandsPtr& in, const MeshPtr& scalp, const std::map<std::string, ParamValue>& params = {}) {
    auto node = OperatorRegistry::instance().create(op::kSelfCollide);
    for (const auto& [name, value] : params) node->params().set(name, value);
    ExternalInputs ext;
    if (scalp) ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    return valueAs<StrandsPtr>(node->evaluate(ctx, {Value(in)}));
}

// Two sheets of 66 strands (16 points, 2.4 long) crossing in one layer, `scale` times as big: sheet
// A's strands run along z, B's along x, rooted off the 2 x 2 square they cross over, A 0.018 below
// B, so each strand of one passes through every strand of the other. At widths 0.021 the strands of a sheet lie about a
// width and a half apart: two sheets crossing are packed past the default packing, and stacked
// they would need about 0.08 of thickness. `sheets` 1: sheet A alone.
StrandsPtr crossingSheets(float scale = 1.0f, float width = 0.021f, int sheets = 2) {
    auto s = std::make_shared<StrandSet>();
    constexpr int kPerSheet = 66, kPoints = 16;
    std::vector<Vec3f> points(kPoints);
    const std::vector<float> widths(kPoints, width * scale);
    for (int sheet = 0; sheet < sheets; ++sheet) {
        for (int i = 0; i < kPerSheet; ++i) {
            const float across = (-1.0f + (static_cast<float>(i) + 0.5f) * 2.0f / kPerSheet) * scale;
            for (int k = 0; k < kPoints; ++k) {
                const float along = (-1.4f + 2.4f * static_cast<float>(k) / (kPoints - 1)) * scale;
                points[static_cast<std::size_t>(k)] =
                    sheet == 0 ? Vec3f(across, -0.009f * scale, along) : Vec3f(along, 0.009f * scale, across);
            }
            s->addStrand(static_cast<StrandId>(sheet * 1000 + i), points, widths);
        }
    }
    return s;
}

float segmentDistance(const Vec3f& p0, const Vec3f& p1, const Vec3f& q0, const Vec3f& q1) {
    // Closest points of two segments (clamped), as in Ericson's Real-Time Collision Detection.
    const Vec3f d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    float s = 0.0f, t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) return r.length();
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = d1.dot(r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = d1.dot(d2), denom = a * e - b * b;
            s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return ((p0 + d1 * s) - (q0 + d2 * t)).length();
}

// The least distance between a strand of sheet A (IDs under 1000) and one of sheet B (and where),
// of the segments crossing within `inside` of the middle in x and z.
float leastDistanceBetweenSheets(const StrandSet& s, float inside, Vec3f* where = nullptr) {
    float least = 1e30f;
    for (std::size_t a = 0; a < s.strandCount(); ++a) {
        if (s.ids()[a] >= 1000) continue;
        for (std::size_t b = 0; b < s.strandCount(); ++b) {
            if (s.ids()[b] < 1000) continue;
            const Vec3f* p = s.strandPoints(a);
            const Vec3f* q = s.strandPoints(b);
            for (std::size_t i = 0; i + 1 < s.strandPointCount(a); ++i) {
                for (std::size_t j = 0; j + 1 < s.strandPointCount(b); ++j) {
                    const Vec3f mid = (p[i] + p[i + 1] + q[j] + q[j + 1]) * 0.25f;
                    if (std::fabs(mid.x) > inside || std::fabs(mid.z) > inside) continue;
                    const float d = segmentDistance(p[i], p[i + 1], q[j], q[j + 1]);
                    if (d < least) {
                        least = d;
                        if (where) *where = (p[i] + p[i + 1] + q[j] + q[j + 1]) * 0.25f;
                    }
                }
            }
        }
    }
    return least;
}

// Long hair combed flat over a sphere (radius 10 x scale, the scalp): `count` strands rooted over
// its top (within 60 degrees of the pole, a Fibonacci spread), each `length` long, lying `lift`
// above the skin down its meridian to the equator, then hanging straight down; all of them at one
// height over the skin, so where they gather toward the equator they pass through one another.
struct SphereHair {
    MeshPtr scalp;
    StrandsPtr strands;
};
SphereHair combedOverSphere(std::size_t count, float scale = 1.0f) {
    SphereHair h;
    const float radius = 10.0f * scale, lift = 0.05f * scale, length = 18.0f * scale;
    h.scalp = test::makeSphere(48, 96, radius);
    auto s = std::make_shared<StrandSet>();
    constexpr std::size_t kPoints = 16;
    std::vector<Vec3f> points(kPoints);
    const float r = radius + lift;
    for (std::size_t i = 0; i < count; ++i) {
        // Fibonacci over the cap: cos(theta) from 1 down to cos(60 degrees).
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float theta0 = std::acos(1.0f - u * 0.5f);
        const float phi = static_cast<float>(i) * 2.39996323f;
        const float arc = r * (0.5f * kPi - theta0);  // to the equator
        for (std::size_t k = 0; k < kPoints; ++k) {
            const float along = length * static_cast<float>(k) / static_cast<float>(kPoints - 1);
            if (along <= arc) {
                const float theta = theta0 + along / r;
                const float rr = k == 0 ? radius : r;  // the root on the skin
                points[k] = Vec3f(rr * std::sin(theta) * std::cos(phi), rr * std::cos(theta), rr * std::sin(theta) * std::sin(phi));
            } else {
                points[k] = Vec3f(r * std::cos(phi), -(along - arc), r * std::sin(phi));
            }
        }
        const SurfacePoint root = h.scalp->closestPoint(points[0]);
        s->addStrand(static_cast<StrandId>(i), points, {}, root.root);
    }
    h.strands = s;
    return h;
}

TEST(SelfCollide, CrossingSheetsArePushedApart) {
    // The two sheets passed through each other (their strands 0.018 apart, under a strand's width
    // of 0.021); after, sheet A lies under B, no strand of one within a strand's width of one of
    // the other (but at the sheets' edges, a cell in, where they fill their cells by half).
    const StrandsPtr in = crossingSheets();
    const StrandsPtr out = selfCollide(in, nullptr, {{"profile", Ramp::constant(1.0f)}});
    ASSERT_TRUE(out);
    ASSERT_NE(out, in);
    Vec3f at;
    const float before = leastDistanceBetweenSheets(*in, 0.9f), after = leastDistanceBetweenSheets(*out, 0.9f, &at);
    Vec3f edgeAt;
    const float edge = leastDistanceBetweenSheets(*out, 2.0f, &edgeAt);
    std::printf("[ self-collide ] crossing sheets: least distance %.4f before, %.4f after (width 0.021), at %.3f %.3f %.3f; "
                "%.4f with the sheets' edges, at %.3f %.3f %.3f\n", before, after, at.x, at.y, at.z, edge, edgeAt.x, edgeAt.y, edgeAt.z);
    EXPECT_LT(before, 0.019f);
    EXPECT_GT(after, 0.021f);
    // A went down, B up.
    double a = 0.0, b = 0.0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const double y = out->strandPoints(s)[8].y;
        (out->ids()[s] < 1000 ? a : b) += y;
    }
    EXPECT_LT(a, b);
}

TEST(SelfCollide, RootsStayAndLengthsAreKept) {
    const StrandsPtr in = crossingSheets();
    const StrandsPtr out = selfCollide(in, nullptr);
    ASSERT_TRUE(out && out != in);
    float moved = 0.0f, worst = 0.0f;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const Vec3f* p = in->strandPoints(s);
        const Vec3f* q = out->strandPoints(s);
        EXPECT_EQ(std::memcmp(&p[0], &q[0], sizeof(Vec3f)), 0) << "root of strand " << s;
        for (std::size_t i = 1; i < in->strandPointCount(s); ++i) {
            const float a = (p[i] - p[i - 1]).length(), b = (q[i] - q[i - 1]).length();
            worst = std::max(worst, std::fabs(a - b) / a);
            moved = std::max(moved, (q[i] - p[i]).length());
        }
    }
    EXPECT_GT(moved, 0.01f);
    EXPECT_LT(worst, 1e-4f);
}

TEST(SelfCollide, HairWithRoomIsTheStrandsAsTheyCame) {
    // The sheets of strands a quarter as wide pass through each other, but have room: nothing
    // moves (the very input handed on). One sheet alone of the wide strands has room at a looser
    // packing, and the two crossing at a looser one still.
    const StrandsPtr thin = crossingSheets(1.0f, 0.005f);
    EXPECT_EQ(selfCollide(thin, nullptr), thin);
    const StrandsPtr one = crossingSheets(1.0f, 0.021f, 1);
    EXPECT_EQ(selfCollide(one, nullptr, {{"packing", 0.8}}), one);
    const StrandsPtr both = crossingSheets();
    EXPECT_EQ(selfCollide(both, nullptr, {{"packing", 0.9}}), both);
}

TEST(SelfCollide, StrengthZeroIsTheInput) {
    const StrandsPtr in = crossingSheets();
    EXPECT_EQ(selfCollide(in, nullptr, {{"strength", 0.0}}), in);
}

TEST(SelfCollide, ScaleInvariant) {
    // The sheets 100 times as big (widths too) come out the same, 100 times as big.
    const StrandsPtr a = selfCollide(crossingSheets(1.0f), nullptr);
    const StrandsPtr b = selfCollide(crossingSheets(100.0f), nullptr);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->pointCount(), b->pointCount());
    float worst = 0.0f, moved = 0.0f;
    const StrandsPtr in = crossingSheets(1.0f);
    for (std::size_t i = 0; i < a->pointCount(); ++i) {
        worst = std::max(worst, (b->positions()[i] / 100.0f - a->positions()[i]).length());
        moved = std::max(moved, (a->positions()[i] - in->positions()[i]).length());
    }
    EXPECT_GT(moved, 0.01f);
    EXPECT_LT(worst, 1e-4f);
    // And over a sphere, with its scalp.
    const SphereHair small = combedOverSphere(3000, 1.0f), big = combedOverSphere(3000, 100.0f);
    const StrandsPtr c = selfCollide(small.strands, small.scalp), d = selfCollide(big.strands, big.scalp);
    ASSERT_TRUE(c && d);
    worst = 0.0f;
    for (std::size_t i = 0; i < c->pointCount(); ++i) worst = std::max(worst, (d->positions()[i] / 100.0f - c->positions()[i]).length());
    EXPECT_LT(worst, 5e-3f);  // (of a sphere of radius 10: the scalp's field has cells of a power of two)
}

TEST(SelfCollide, DeterministicOnAnyNumberOfThreads) {
    const SphereHair hair = combedOverSphere(20000);
    const StrandsPtr many = selfCollide(hair.strands, hair.scalp);
    ASSERT_TRUE(many && many != hair.strands);
    StrandsPtr one;
#if GROOMLAB_HAS_TBB
    tbb::task_arena single(1);
    single.execute([&] { one = selfCollide(hair.strands, hair.scalp); });
#else
    one = selfCollide(hair.strands, hair.scalp);
#endif
    ASSERT_TRUE(one);
    EXPECT_EQ(std::memcmp(one->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
    const StrandsPtr again = selfCollide(hair.strands, hair.scalp);
    EXPECT_EQ(std::memcmp(again->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
}

TEST(SelfCollide, HairOverASphereGainsVolumeAndStaysOutOfIt) {
    // Hair combed flat over a sphere piles up where it gathers (toward the equator): the layer
    // there thickens outward, no point goes into the sphere, and the crown, holding less hair,
    // thickens less.
    const SphereHair hair = combedOverSphere(20000);
    const StrandsPtr out = selfCollide(hair.strands, hair.scalp);
    ASSERT_TRUE(out && out != hair.strands);
    const float radius = 10.0f;
    float lowest = 1e30f;
    double crown = 0.0;
    std::size_t crownPoints = 0;
    double inBand = 0.0, outBand = 0.0, outBandSq = 0.0;
    std::size_t band = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* p = hair.strands->strandPoints(s);
        const Vec3f* q = out->strandPoints(s);
        // Over the skin: from the sphere above the equator, from its widest circle below.
        auto over = [&](const Vec3f& x) {
            return x.y >= 0.0f ? x.length() - radius : std::sqrt(x.x * x.x + x.z * x.z) - radius;
        };
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            const float off = over(q[i]);
            lowest = std::min(lowest, q[i].length() - radius);
            if (p[i].y > 9.0f) {
                crown += off;
                ++crownPoints;
            }
            if (std::fabs(p[i].y) < 2.0f) {
                inBand += over(p[i]);
                outBand += off;
                outBandSq += static_cast<double>(off) * off;
                ++band;
            }
        }
    }
    ASSERT_GT(band, 1000u);
    const double meanIn = inBand / static_cast<double>(band), meanOut = outBand / static_cast<double>(band);
    const double spread = std::sqrt(std::max(0.0, outBandSq / static_cast<double>(band) - meanOut * meanOut));
    const double crownOut = crown / static_cast<double>(std::max<std::size_t>(crownPoints, 1));
    std::printf("[ self-collide ] over a sphere: height over the skin near the equator %.3f before, %.3f after "
                "(spread %.3f); at the crown %.3f after; lowest point %.4f over the skin\n", meanIn, meanOut, spread, crownOut, lowest);
    EXPECT_GT(lowest, -0.01f);  // (the faceted sphere's faces lie up to 0.005 inside the round one)
    EXPECT_GT(meanOut, meanIn + 0.3);
    EXPECT_GT(spread, 0.2);
    EXPECT_LT(crownOut, 0.75 * meanOut);
}

TEST(SelfCollide, ItsGridAsAHostReadsIt) {
    // hairGridLayout (operator_state.h, the GPU preview's): the strands splatted (at most about
    // 8192, a share of 20000 by ID), each of their segments, the floor's field as tables; and
    // hairGridWalls: the scalp near the world's grid nodes, as wallsOf finds it (its closest point
    // within 1.5 cells). Over a scalp deformed away from its rest pose: none.
    const SphereHair hair = combedOverSphere(20000);
    HairGridLayout l;
    std::string why;
    ASSERT_TRUE(hairGridLayout(op::kSelfCollide, *hair.strands, hair.scalp, &l, &why)) << why;
    EXPECT_GT(l.strands, 4000u);
    EXPECT_LE(l.strands, 8192u);
    EXPECT_EQ(l.weight, 4.0);  // (a quarter of them)
    std::size_t segments = 0;
    std::vector<char> splatted(hair.strands->strandCount(), 0);
    for (std::size_t w = 0; w + 1 < l.segments.size(); w += 2) {
        const std::uint32_t s = l.segments[w + 1];
        ASSERT_LT(s, hair.strands->strandCount());
        EXPECT_GE(l.segments[w], hair.strands->strandBegin(s));
        EXPECT_LT(l.segments[w] + 1, hair.strands->strandBegin(s) + hair.strands->strandPointCount(s));
        splatted[s] = 1;
        ++segments;
    }
    std::size_t expected = 0, strands = 0;
    for (std::size_t s = 0; s < splatted.size(); ++s) {
        if (!splatted[s]) continue;
        expected += hair.strands->strandPointCount(s) - 1;
        ++strands;
    }
    EXPECT_EQ(strands, l.strands);
    EXPECT_EQ(segments, expected);
    EXPECT_GT(l.h, 0.0);
    EXPECT_FALSE(l.coarsened);
    EXPECT_GT(l.blocks, 0u);
    EXPECT_TRUE(l.tables && l.floor > 0.0f && l.searchRadius > l.floor);
    const auto walls = hairGridWalls(*hair.scalp, l.h, l.wallReach);
    ASSERT_TRUE(walls);
    const std::vector<std::uint32_t>& w = *walls;
    const std::int64_t origin[3] = {static_cast<std::int32_t>(w[0]), static_cast<std::int32_t>(w[1]), static_cast<std::int32_t>(w[2])};
    const std::size_t dims[3] = {w[3], w[4], w[5]};
    const std::size_t count = dims[0] * dims[1] * dims[2];
    ASSERT_EQ(w.size(), 8 + count + 4 * std::size_t{w[6]});
    std::size_t near = 0, checked = 0;
    for (std::size_t at = 0; at < count; at += 7) {
        const std::int64_t c[3] = {static_cast<std::int64_t>(at % dims[0]) + origin[0],
                                   static_cast<std::int64_t>((at / dims[0]) % dims[1]) + origin[1],
                                   static_cast<std::int64_t>(at / (dims[0] * dims[1])) + origin[2]};
        const Vec3f node(static_cast<float>(static_cast<double>(c[0]) * l.h), static_cast<float>(static_cast<double>(c[1]) * l.h),
                         static_cast<float>(static_cast<double>(c[2]) * l.h));
        float bound = 0.0f;
        const SurfacePoint sp = hair.scalp->closestPointBounded(node, static_cast<float>(l.wallReach * l.h), &bound);
        const std::uint32_t index = w[8 + at];
        ++checked;
        ASSERT_EQ(index != 0xFFFFFFFFu, sp.root.face >= 0) << at;
        if (index == 0xFFFFFFFFu) continue;
        ++near;
        float height;
        std::memcpy(&height, &w[8 + count + 4 * std::size_t{index}], 4);
        EXPECT_EQ(height, (node - sp.position).dot(sp.normal));
    }
    EXPECT_GT(near, 100u);
    EXPECT_GT(checked, near);
    auto deformed = std::make_shared<ScalpMesh>(*hair.scalp);
    std::vector<Vec3f> rest = deformed->points();
    for (Vec3f& q : rest) q = q * 0.97f;
    deformed->setRestPoints(rest);
    // Over a deformed scalp: the floor through its rest pose (no tables: the CPU engine's walk asks
    // the host, hairGridFloorPush).
    ASSERT_TRUE(hairGridLayout(op::kStatic, *hair.strands, deformed, &l, &why)) << why;
    EXPECT_TRUE(static_cast<bool>(l.floorSurface));
    EXPECT_FALSE(l.tables);
    EXPECT_GT(l.floor, 0.0f);
}

TEST(SelfCollide, APreviewShowsTheRendersStrands) {
    // A preview holds a share of the strands (and the core every preview holds, preview_core.h):
    // SelfCollide's grid is the core's hair alone, so every strand a preview shows comes out as the
    // render's strand of that ID, bit for bit.
    constexpr std::size_t kCount = 40000;
    const SphereHair hair = combedOverSphere(kCount);
    const float core = coreShare(static_cast<double>(kCount));
    ASSERT_LT(core, 1.0f);
    auto held = [&](float keep) {  // the strands a preview at `keep` holds (1: the render's), with their roles
        auto s = std::make_shared<StrandSet>();
        std::vector<float> roles;
        for (std::size_t k = 0; k < hair.strands->strandCount(); ++k) {
            float role = kCoreShown;
            if (!previewRole(hair.strands->ids()[k], keep >= 1.0f ? 2.0f : keep, core, &role)) continue;
            const Vec3f* p = hair.strands->strandPoints(k);
            s->addStrand(hair.strands->ids()[k], std::vector<Vec3f>(p, p + hair.strands->strandPointCount(k)), {},
                         hair.strands->roots()[k]);
            roles.push_back(role);
        }
        s->addStrandChannel(kPreviewChannel, kCoreShown) = roles;
        return StrandsPtr(s);
    };
    const StrandsPtr render = selfCollide(held(1.0f), hair.scalp);
    ASSERT_TRUE(render);
    std::unordered_map<StrandId, std::size_t> at;
    for (std::size_t k = 0; k < render->strandCount(); ++k) at[render->ids()[k]] = k;
    float moved = 0.0f;
    for (std::size_t i = 0; i < render->pointCount(); ++i) moved = std::max(moved, (render->positions()[i] - hair.strands->positions()[i]).length());
    EXPECT_GT(moved, 0.3f);
    for (const float keep : {0.3f, 0.05f}) {
        const StrandsPtr preview = selfCollide(held(keep), hair.scalp);
        ASSERT_TRUE(preview);
        std::size_t different = 0, shown = 0;
        const PreviewCore roles(*preview);
        for (std::size_t k = 0; k < preview->strandCount(); ++k) {
            if (roles.hidden(k)) continue;
            ++shown;
            const std::size_t r = at.at(preview->ids()[k]);
            const std::size_t n = preview->strandPointCount(k);
            if (n != render->strandPointCount(r) || std::memcmp(preview->strandPoints(k), render->strandPoints(r), n * sizeof(Vec3f)) != 0) {
                ++different;
            }
        }
        EXPECT_GT(shown, 1000u);
        EXPECT_EQ(different, 0u) << "at a preview of " << keep;
    }
}

// Not a check: writes the sphere's hair before and after (one strand a line: x y z per point) to
// SELF_COLLIDE_PICTURE's folder, for a picture.
TEST(SelfCollide, Picture) {
    const char* folder = std::getenv("SELF_COLLIDE_PICTURE");
    if (!folder) GTEST_SKIP() << "set SELF_COLLIDE_PICTURE to a folder";
    const SphereHair hair = combedOverSphere(20000);
    const StrandsPtr out = selfCollide(hair.strands, hair.scalp);
    auto write = [&](const StrandSet& s, const std::string& name) {
        std::ofstream file(std::string(folder) + "/" + name);
        for (std::size_t k = 0; k < s.strandCount(); ++k) {
            const Vec3f* p = s.strandPoints(k);
            for (std::size_t i = 0; i < s.strandPointCount(k); ++i) file << p[i].x << ' ' << p[i].y << ' ' << p[i].z << ' ';
            file << '\n';
        }
    };
    write(*hair.strands, "self_collide_before.txt");
    write(*out, "self_collide_after.txt");
}

}  // namespace
}  // namespace groomlab
