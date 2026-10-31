// SPDX-License-Identifier: Apache-2.0
// Parting: part lines on the scalp, part-aware Interpolate and Clump, and the Part operator.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "groomlab/brush.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A part down the middle of the unit plane (x = 0.5), running z = 0 .. 1.
PartSet middlePart(const ScalpMesh& plane) {
    PartSet set;
    set.parts.push_back(PartSet::fromPoints("middle", {{0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 1.0f}}, plane));
    return set;
}

TEST(Parts, BindRoundTripAndSides) {
    const auto plane = test::makePlane(8);
    const PartSet set = middlePart(*plane);
    const PartSet back = PartSet::fromJson(set.toJson());
    ASSERT_EQ(back.parts.size(), 1u);
    EXPECT_EQ(back.contentHash(), set.contentHash());
    EXPECT_EQ(back.find("middle")->points.size(), 3u);
    EXPECT_THROW(PartSet::fromPoints("x", {{0, 0, 0}}, *plane), std::invalid_argument);
    EXPECT_TRUE(PartSet::fromJson("").empty());

    const PartLines lines(set, *plane);
    const auto left = lines.sideOf({0.3f, 0.0f, 0.5f});
    const auto right = lines.sideOf({0.7f, 0.0f, 0.5f});
    EXPECT_NE(left.side, 0);
    EXPECT_EQ(left.side, -right.side);
    EXPECT_NEAR(left.distance, 0.2f, 1e-5f);
    EXPECT_TRUE(PartLines::separated(left, right));
    EXPECT_FALSE(PartLines::separated(left, lines.sideOf({0.1f, 0.0f, 0.3f})));
    EXPECT_EQ(lines.sideOf({0.5f, 0.0f, 1.3f}).side, 0);  // past the end: no side
    EXPECT_EQ(lines.sideOf({0.3f, 0.0f, 0.5f}, "other").part, -1);
    // Away from the part: -x on the left, +x on the right (tangent to the plane).
    EXPECT_LT(lines.awayFrom(left, {0, 1, 0}).x, -0.99f);
    EXPECT_GT(lines.awayFrom(right, {0, 1, 0}).x, 0.99f);
}

TEST(Parts, SideOfFindsTheSegmentAScanWould) {
    // Long wavy parts on a sphere, one of them twice (every point near it ties between the two),
    // queried all over: the box trees pick the segment a scan of every segment in order picks.
    const auto sphere = test::makeSphere(40, 80, 1.0f);
    PartSet set;
    for (int k = 0; k < 3; ++k) {
        std::vector<Vec3f> line;
        for (int i = 0; i < 200; ++i) {
            const float x = -0.9f + 1.8f * static_cast<float>(i) / 199.0f;
            line.push_back(Vec3f(x, 1.0f, -0.4f + 0.4f * static_cast<float>(k) + 0.1f * std::sin(9.0f * x)).normalized());
        }
        set.parts.push_back(PartSet::fromPoints("p" + std::to_string(k), line, *sphere));
    }
    set.parts.push_back(set.parts[1]);
    set.parts.back().name = "copy";
    const PartLines lines(set, *sphere);  // no rest pose: the lines are the rest lines
    ASSERT_EQ(lines.lines().size(), 4u);
    auto scan = [&](const Vec3f& q, const std::string& only) {
        PartLines::Side best;
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t p = 0; p < lines.lines().size(); ++p) {
            if (!only.empty() && lines.names()[p] != only) continue;
            const auto& line = lines.lines()[p];
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const Vec3f a = line[i], ab = line[i + 1] - a;
                const float len2 = ab.dot(ab);
                const float t = len2 > 0.0f ? std::clamp((q - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
                const float d = (q - (a + ab * t)).length();
                if (d < bestDistance) {
                    bestDistance = d;
                    best.part = static_cast<int>(p);
                    best.segment = i;
                    best.distance = d;
                }
            }
        }
        return best;
    };
    std::vector<Vec3f> queries = sphere->points();
    for (const auto& line : lines.lines()) queries.insert(queries.end(), line.begin(), line.end());  // on the lines
    std::size_t tied = 0;
    for (const Vec3f& q : queries) {
        for (const std::string& only : {std::string(), std::string("copy"), std::string("p2")}) {
            const PartLines::Side got = lines.sideOf(q, only), want = scan(q, only);
            ASSERT_EQ(got.part, want.part) << q.x << " " << q.y << " " << q.z << " " << only;
            ASSERT_EQ(got.segment, want.segment);
            ASSERT_EQ(got.distance, want.distance);
            tied += only.empty() && want.part == 1 ? 1 : 0;  // the copy ties with it, and loses
        }
    }
    EXPECT_GT(tied, 100u);
}

// Strands on the plane; guides at x = 0.3 lean -x, guides at x = 0.7 lean +x.
struct Scene {
    std::shared_ptr<const ScalpMesh> plane = test::makePlane(8);
    GuidesPtr guides;
    Scene() {
        auto g = std::make_shared<GuideSet>();
        GuideId id = 1;
        for (float z = 0.1f; z < 1.0f; z += 0.2f) {
            g->add(id++, test::straightCurve({0.3f, 0, z}, {-0.5f, 0.5f, 0}, 6));
            g->add(id++, test::straightCurve({0.7f, 0, z}, {0.5f, 0.5f, 0}, 6));
        }
        g->rootOnMesh(*plane);
        guides = g;
    }
};

// Mean tip x offset of strands rooted just left of the part.
float leftOfPartLean(const StrandSet& s) {
    float sum = 0.0f;
    int n = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f* p = s.strandPoints(i);
        if (p[0].x > 0.42f && p[0].x < 0.49f && p[0].z > 0.15f && p[0].z < 0.85f) {
            sum += p[s.strandPointCount(i) - 1].x - p[0].x;
            ++n;
        }
    }
    EXPECT_GT(n, 10);
    return sum / static_cast<float>(n);
}

TEST(Parts, InterpolateDoesNotBlendAcrossAPart) {
    Scene scene;
    auto run = [&](bool parts, bool respect) {
        GroomData d;
        d.scalp = scene.plane;
        d.guides = scene.guides;
        if (parts) d.partsJson = middlePart(*scene.plane).toJson();
        std::string graph = defaultGraphJson();
        const std::string interp = "\"type\": \"Interpolate\"";
        graph.replace(graph.find(interp), interp.size(),
                      interp + (respect ? ", \"params\": {\"neighbors\": 6}" : ", \"params\": {\"neighbors\": 6, \"respectParts\": false}"));
        const std::string scatter = "\"type\": \"Scatter\"";
        graph.replace(graph.find(scatter), scatter.size(), scatter + ", \"params\": {\"density\": 3000.0}");
        d.graphJson = graph;
        return Groom(d).evaluate(1.0f, 0.0);
    };
    const float mixed = leftOfPartLean(*run(false, true));
    const float parted = leftOfPartLean(*run(true, true));
    const float ignored = leftOfPartLean(*run(true, false));
    EXPECT_LT(parted, -0.4f);          // all from the left guides
    EXPECT_GT(mixed, parted + 0.1f);   // without the part, right guides pull back
    EXPECT_FLOAT_EQ(ignored, mixed);   // respectParts off: as if there were no part
}

TEST(Parts, PartOperatorOpensTheGap) {
    const auto plane = test::makePlane(8);
    auto strands = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (float x = 0.05f; x < 1.0f; x += 0.1f) {
        const SurfacePoint sp = plane->closestPoint({x, 0, 0.5f});
        strands->addStrand(id++, test::straightCurve(sp.position, {0, 1, 0}, 6), {}, sp.root);
    }
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(plane), plane->contentHash());
    const auto lines = std::make_shared<const PartLines>(middlePart(*plane), *plane);
    ext.set("parts", PartsPtr(lines), lines->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    auto part = OperatorRegistry::instance().create(op::kPart);
    part->params().set("width", 0.2);
    const StrandsPtr out = valueAs<StrandsPtr>(part->evaluate(ctx, {StrandsPtr(strands)}));
    for (std::size_t i = 0; i < out->strandCount(); ++i) {
        const float x = out->strandPoints(i)[0].x;
        const float lean = out->strandPoints(i)[6].x - x;
        // Strongly beside the part, fading toward the edge of its width.
        if (x > 0.4f && x < 0.5f) EXPECT_LT(lean, -0.15f) << x;
        if (x > 0.5f && x < 0.6f) EXPECT_GT(lean, 0.15f) << x;
        if (x > 0.3f && x < 0.4f) EXPECT_LT(lean, 0.0f) << x;
        if (x > 0.6f && x < 0.7f) EXPECT_GT(lean, 0.0f) << x;
        if (x < 0.3f || x > 0.7f) EXPECT_FLOAT_EQ(lean, 0.0f) << x;  // beyond the width
        const float len = (out->strandPoints(i)[6] - out->strandPoints(i)[0]).length();
        EXPECT_GT(len, 0.5f);  // lengths kept: a bent strand's chord is shorter, not longer
    }
    // No parts: unchanged.
    ExternalInputs none;
    none.set("scalp", MeshPtr(plane), plane->contentHash());
    ctx.externals = &none;
    EXPECT_EQ(valueAs<StrandsPtr>(part->evaluate(ctx, {StrandsPtr(strands)}))->positions(), strands->positions());
}

TEST(Parts, TravelInGroomFiles) {
    GroomFile file;
    const auto plane = test::makePlane(8);
    file.data.scalp = plane;
    file.data.partsJson = middlePart(*plane).toJson();
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    EXPECT_EQ(PartSet::fromJson(back.data.partsJson).contentHash(), middlePart(*plane).contentHash());
}

TEST(Parts, AddAStrokeWithUniqueNames) {
    const auto head = test::makeSphere(24, 48, 1.0f);
    PartSet set;
    std::vector<Vec3f> stroke;
    for (int i = 0; i <= 300; ++i) {
        const float a = -0.6f + 1.2f * i / 300.0f;
        stroke.push_back(Vec3f{0.0f, std::cos(a), std::sin(a)} * 1.02f);  // just off the surface
    }
    EXPECT_EQ(set.add("crown", stroke, *head), "crown");
    EXPECT_EQ(set.add("crown", stroke, *head), "crown1");
    EXPECT_EQ(set.parts[0].points.size(), 64u);  // resampled evenly
    for (const StrandRoot& r : set.parts[0].points) EXPECT_NEAR(head->position(r).length(), 1.0f, 0.01f);
    EXPECT_THROW(set.add("x", {stroke[0]}, *head), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// The shaping brushes on part lines (applyBrushStampToParts / StrokeToParts)

BrushSettings partBrush(BrushMode mode, float radius, float strength = 1.0f) {
    BrushSettings b;
    b.mode = mode;
    b.radius = radius;
    b.strength = strength;
    return b;
}

// A part along x = `x` with five points, z = 0 .. 1; the middle point pushed to x + `kink`.
PartSet linePart(const ScalpMesh& plane, float x, float kink = 0.0f, const std::string& name = "middle") {
    PartSet set;
    set.parts.push_back(PartSet::fromPoints(
        name, {{x, 0, 0}, {x, 0, 0.25f}, {x + kink, 0, 0.5f}, {x, 0, 0.75f}, {x, 0, 1.0f}}, plane));
    return set;
}

Vec3f partPoint(const PartSet& set, const ScalpMesh& scalp, std::size_t part, std::size_t i) {
    return scalp.position(set.parts[part].points[i]);
}

TEST(PartBrush, CombSlidesTheTouchedPointsAlongTheScalp) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.5f);
    EXPECT_EQ(applyBrushStrokeToParts(set, *plane, partBrush(BrushMode::Comb, 0.2f), {{0.5f, 0, 0.5f}, {0.6f, 0, 0.5f}}), 1u);
    ASSERT_EQ(set.parts.size(), 1u);
    EXPECT_EQ(set.parts[0].name, "middle");
    EXPECT_EQ(set.parts[0].points.size(), 5u);
    EXPECT_NEAR(partPoint(set, *plane, 0, 2).x, 0.6f, 1e-3f);  // rode along with the stroke
    EXPECT_NEAR(partPoint(set, *plane, 0, 2).y, 0.0f, 1e-6f);  // still on the scalp
    EXPECT_NEAR(partPoint(set, *plane, 0, 0).x, 0.5f, 1e-5f);  // out of reach: unchanged
    EXPECT_NEAR(partPoint(set, *plane, 0, 4).x, 0.5f, 1e-5f);
    EXPECT_EQ(applyBrushStampToParts(set, *plane, partBrush(BrushMode::Comb, 0.2f), {0.6f, 0, 0.5f}, {}), 0u);  // no drag
}

TEST(PartBrush, SmoothAndStraightenRelaxAKink) {
    const auto plane = test::makePlane(8);
    PartSet smoothed = linePart(*plane, 0.5f, 0.1f);
    EXPECT_EQ(applyBrushStampToParts(smoothed, *plane, partBrush(BrushMode::Smooth, 0.05f), {0.6f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(partPoint(smoothed, *plane, 0, 2).x, 0.55f, 1e-3f);  // half way to its neighbours' midpoint
    EXPECT_NEAR(partPoint(smoothed, *plane, 0, 2).z, 0.5f, 1e-3f);
    PartSet straightened = linePart(*plane, 0.5f, 0.1f);
    EXPECT_EQ(applyBrushStampToParts(straightened, *plane, partBrush(BrushMode::Straighten, 0.05f), {0.6f, 0, 0.5f}, {}), 1u);
    EXPECT_NEAR(partPoint(straightened, *plane, 0, 2).x, 0.55f, 1e-3f);  // half way to the chord
    // The ends never move.
    for (const PartSet* s : {&smoothed, &straightened}) {
        EXPECT_NEAR(partPoint(*s, *plane, 0, 0).z, 0.0f, 1e-5f);
        EXPECT_NEAR(partPoint(*s, *plane, 0, 4).z, 1.0f, 1e-5f);
    }
}

TEST(PartBrush, AttractPullsPointsToTheCenter) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.5f);
    EXPECT_EQ(applyBrushStampToParts(set, *plane, partBrush(BrushMode::Attract, 0.3f), {0.7f, 0, 0.5f}, {}), 1u);
    const float w = (1.0f - 4.0f / 9.0f) * (1.0f - 4.0f / 9.0f);  // falloff at 2/3 of the radius
    EXPECT_NEAR(partPoint(set, *plane, 0, 2).x, 0.5f + 0.2f * 0.1f * w, 1e-4f);
    PartSet pushed = linePart(*plane, 0.5f);
    applyBrushStampToParts(pushed, *plane, partBrush(BrushMode::Attract, 0.3f, -1.0f), {0.7f, 0, 0.5f}, {});
    EXPECT_LT(partPoint(pushed, *plane, 0, 2).x, 0.5f - 0.005f);
}

TEST(PartBrush, CutSplitsAPartWhereTheBrushTouchesIt) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.5f);
    set.parts.push_back(PartSet::fromPoints("middle1", {{0.9f, 0, 0}, {0.9f, 0, 1}}, *plane));  // the next name is taken
    EXPECT_EQ(applyBrushStampToParts(set, *plane, partBrush(BrushMode::Cut, 0.1f), {0.5f, 0, 0.5f}, {}), 1u);
    ASSERT_EQ(set.parts.size(), 3u);
    EXPECT_EQ(set.parts[0].name, "middle");
    EXPECT_EQ(set.parts[0].points.size(), 2u);
    EXPECT_NEAR(partPoint(set, *plane, 0, 1).z, 0.25f, 1e-5f);
    EXPECT_EQ(set.parts[1].name, "middle2");
    EXPECT_EQ(set.parts[1].points.size(), 2u);
    EXPECT_NEAR(partPoint(set, *plane, 1, 0).z, 0.75f, 1e-5f);
    EXPECT_EQ(set.parts[2].name, "middle1");
    // Cutting an end just shortens; cutting all but one point removes the part.
    PartSet end = linePart(*plane, 0.5f);
    EXPECT_EQ(applyBrushStampToParts(end, *plane, partBrush(BrushMode::Cut, 0.1f), {0.5f, 0, 1.0f}, {}), 1u);
    ASSERT_EQ(end.parts.size(), 1u);
    EXPECT_EQ(end.parts[0].points.size(), 4u);
    PartSet gone = linePart(*plane, 0.5f);
    EXPECT_GE(applyBrushStrokeToParts(gone, *plane, partBrush(BrushMode::Cut, 0.2f), {{0.5f, 0, 0.0f}, {0.5f, 0, 0.8f}}), 1u);
    EXPECT_TRUE(gone.parts.empty());
}

TEST(PartBrush, EraseRemovesPartsUnderTheBrush) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.3f, 0.0f, "left");
    set.parts.push_back(linePart(*plane, 0.7f, 0.0f, "right").parts[0]);
    EXPECT_EQ(applyBrushStrokeToParts(set, *plane, partBrush(BrushMode::Erase, 0.15f), {{0.3f, 0, 0.4f}, {0.3f, 0, 0.6f}}), 1u);
    ASSERT_EQ(set.parts.size(), 1u);
    EXPECT_EQ(set.parts[0].name, "right");
    // Too far, or too weak a brush: nothing goes.
    EXPECT_EQ(applyBrushStampToParts(set, *plane, partBrush(BrushMode::Erase, 0.1f), {0.5f, 0, 0.5f}, {}), 0u);
    EXPECT_EQ(applyBrushStampToParts(set, *plane, partBrush(BrushMode::Erase, 0.1f, 0.2f), {0.7f, 0, 0.5f}, {}), 0u);
    EXPECT_EQ(set.parts.size(), 1u);
}

TEST(PartBrush, SymmetryMirrorsTheStamp) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.3f, 0.0f, "left");
    set.parts.push_back(linePart(*plane, 0.7f, 0.0f, "right").parts[0]);
    BrushSettings b = partBrush(BrushMode::Comb, 0.15f);
    b.mirrorAxis = 0;
    b.mirrorCenter = 0.5f;
    EXPECT_EQ(applyBrushStrokeToParts(set, *plane, b, {{0.3f, 0, 0.5f}, {0.4f, 0, 0.5f}}), 2u);
    EXPECT_NEAR(partPoint(set, *plane, 0, 2).x, 0.4f, 1e-3f);
    EXPECT_NEAR(partPoint(set, *plane, 1, 2).x, 0.6f, 1e-3f);  // its mirror image, the other way
    // A part on the seam changes once: pulled toward the center from either side, the same.
    PartSet seam = linePart(*plane, 0.5f);
    b.mode = BrushMode::Attract;
    b.radius = 0.3f;
    EXPECT_EQ(applyBrushStampToParts(seam, *plane, b, {0.5f, 0, 0.7f}, {}), 1u);
    PartSet plain = linePart(*plane, 0.5f);
    applyBrushStampToParts(plain, *plane, partBrush(BrushMode::Attract, 0.3f), {0.5f, 0, 0.7f}, {});
    EXPECT_EQ(seam.contentHash(), plain.contentHash());
}

TEST(PartBrush, OtherBrushesLeavePartsAlone) {
    const auto plane = test::makePlane(8);
    PartSet set = linePart(*plane, 0.5f);
    const std::uint64_t before = set.contentHash();
    for (const BrushMode mode : {BrushMode::Twist, BrushMode::Length, BrushMode::Lift,
                                 BrushMode::ShapeTube, BrushMode::Attribute, BrushMode::Slide}) {
        EXPECT_EQ(applyBrushStrokeToParts(set, *plane, partBrush(mode, 0.3f), {{0.4f, 0, 0.5f}, {0.6f, 0, 0.5f}}), 0u)
            << brushModeName(mode);
    }
    EXPECT_EQ(set.contentHash(), before);
}

// ---------------------------------------------------------------------------
// Auto parts, found where the hair splits (findAutoParts, Part's partsFrom auto)

// A sphere of `radius` welded at its seam and poles, the poles on the x axis: with an even
// `rings`, a ring of vertices lies on x = 0 (a symmetric head's center loop).
std::shared_ptr<const ScalpMesh> weldedSphere(int rings, int segments, float radius) {
    std::vector<Vec3f> points{{-radius, 0, 0}};
    for (int r = 1; r < rings; ++r) {
        const float theta = 3.14159265f * static_cast<float>(r) / rings;
        for (int s = 0; s < segments; ++s) {
            const float phi = 6.2831853f * static_cast<float>(s) / segments;
            points.emplace_back(-radius * std::cos(theta), radius * std::sin(theta) * std::cos(phi),
                                radius * std::sin(theta) * std::sin(phi));
        }
    }
    points.emplace_back(radius, 0.0f, 0.0f);
    const int last = static_cast<int>(points.size()) - 1;
    auto at = [segments](int r, int s) { return 1 + (r - 1) * segments + (s % segments); };
    std::vector<ScalpMesh::Triangle> tris;
    for (int s = 0; s < segments; ++s) {
        tris.push_back({0, at(1, s + 1), at(1, s)});
        tris.push_back({last, at(rings - 1, s), at(rings - 1, s + 1)});
        for (int r = 1; r + 1 < rings; ++r) {
            tris.push_back({at(r, s), at(r, s + 1), at(r + 1, s)});
            tris.push_back({at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(tris));
}

// Strands on the cap of a sphere of `scale` (y above 0.3 of it: a head's scalp), roots spread
// evenly (a Fibonacci spiral), each straight along comb(point on the unit sphere) laid on the
// surface and lifted a little, 0.3 of the radius long.
template <typename Comb>
std::shared_ptr<StrandSet> combedStrands(const ScalpMesh& sphere, float scale, const Comb& comb, int count = 6000) {
    auto s = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (int i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        if (y < 0.3f) continue;
        const float r = std::sqrt(1.0f - y * y), a = 2.39996323f * static_cast<float>(i);
        const SurfacePoint sp = sphere.closestPoint(Vec3f(r * std::cos(a), y, r * std::sin(a)) * scale);
        const Vec3f n = sp.position.normalized();
        Vec3f d = comb(n);
        d = (d - n * n.dot(d)).normalized();
        s->addStrand(id++, test::straightCurve(sp.position, (d + n * 0.3f).normalized() * (0.3f * scale), 6), {}, sp.root);
    }
    return s;
}

// Combed apart at x = `x0`, sideways and a little back.
auto partedAt(float x0) {
    return [x0](const Vec3f& p) { return Vec3f(p.x < x0 ? -1.0f : 1.0f, 0.0f, 0.3f); };
}

std::vector<Vec3f> partPoints(const PartSet& set, const ScalpMesh& scalp, std::size_t part) {
    std::vector<Vec3f> out;
    for (const StrandRoot& r : set.parts[part].points) out.push_back(scalp.position(r));
    return out;
}

TEST(FlowParts, FindsTheLineTheHairIsCombedAwayFrom) {
    const auto head = weldedSphere(40, 80, 1.0f);
    const PartSet found = findAutoParts(*combedStrands(*head, 1.0f, partedAt(0.0f)), *head, 90.0f, 0.15f);
    ASSERT_EQ(found.parts.size(), 1u);
    EXPECT_EQ(found.parts[0].name, "auto1");
    const std::vector<Vec3f> line = partPoints(found, *head, 0);
    for (const Vec3f& p : line) {
        EXPECT_LT(std::abs(p.x), 0.04f) << p.x << " " << p.y << " " << p.z;  // on the center line
        EXPECT_GT(p.y, 0.15f);  // on the cap (rim at 0.3), or within R / 2 of its hair
    }
    // Front to back over the top (the cap's rim is at z = +-0.95).
    const auto [lo, hi] = std::minmax_element(line.begin(), line.end(), [](const Vec3f& a, const Vec3f& b) { return a.z < b.z; });
    EXPECT_LT(lo->z, -0.7f);
    EXPECT_GT(hi->z, 0.7f);
}

TEST(FlowParts, HairCombedOneWayOrGatheredHasNoPart) {
    const auto head = weldedSphere(40, 80, 1.0f);
    auto none = [&](const auto& comb) { return findAutoParts(*combedStrands(*head, 1.0f, comb), *head, 90.0f, 0.15f).parts.size(); };
    EXPECT_EQ(none([](const Vec3f&) { return Vec3f(1.0f, 0.0f, 0.3f); }), 0u);  // all one way
    EXPECT_EQ(none([](const Vec3f& p) { return Vec3f(p.x < 0.0f ? 1.0f : -1.0f, 0.0f, 0.3f); }), 0u);  // meeting at a line
    // A soft split (62 degrees between the sides, back over the head and a little apart, all along
    // the line) is a part only below that splitAngle.
    auto soft = [](const Vec3f& n) {
        const Vec3f across = (Vec3f(1, 0, 0) - n * n.x).normalized(), back = (Vec3f(0, 0, 1) - n * n.z).normalized();
        return across * (n.x < 0.0f ? -0.6f : 0.6f) + back;
    };
    EXPECT_EQ(none(soft), 0u);
    EXPECT_EQ(findAutoParts(*combedStrands(*head, 1.0f, soft), *head, 45.0f, 0.15f).parts.size(), 1u);
}

TEST(FlowParts, AWhorlIsNoPartAndACrownEndsOne) {
    const auto head = weldedSphere(40, 80, 1.0f);
    const Vec3f crown(0.0f, 0.8f, 0.6f);  // up and back
    auto whorl = [crown](const Vec3f& p) {
        const Vec3f out = p - crown, around = crown.cross(out);
        return out.normalized() * std::cos(1.22f) + around.normalized() * std::sin(1.22f);  // spiralling out at 70 degrees
    };
    EXPECT_TRUE(findAutoParts(*combedStrands(*head, 1.0f, whorl), *head, 90.0f, 0.15f).empty());
    // A center part in front running into the whorl: one part, from the front to near the crown.
    auto crowned = [&](const Vec3f& p) {
        const float w = std::clamp(((p - crown).length() - 0.25f) / 0.3f, 0.0f, 1.0f);  // the whorl's within 0.25
        const Vec3f parted = p.z < crown.z ? Vec3f(p.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.3f) : Vec3f(0.0f, 0.0f, 1.0f);
        return whorl(p).normalized() * (1.0f - w) + parted.normalized() * w;
    };
    const PartSet found = findAutoParts(*combedStrands(*head, 1.0f, crowned), *head, 90.0f, 0.15f);
    ASSERT_EQ(found.parts.size(), 1u);
    for (const Vec3f& p : partPoints(found, *head, 0)) {
        EXPECT_LT(p.z, crown.z);  // in front of the crown
        if ((p - crown).length() > 0.55f) EXPECT_LT(std::abs(p.x), 0.06f);  // on the center line, curling into the whorl
    }
}

TEST(FlowParts, ScaleInvariantAndDeterministic) {
    const auto head = weldedSphere(40, 80, 1.0f);
    const auto big = weldedSphere(40, 80, 100.0f);
    auto side = partedAt(-0.35f);  // a side part
    const PartSet found = findAutoParts(*combedStrands(*head, 1.0f, side), *head, 90.0f, 0.15f);
    const PartSet again = findAutoParts(*combedStrands(*head, 1.0f, side), *head, 90.0f, 0.15f);
    const PartSet scaled = findAutoParts(*combedStrands(*big, 100.0f, side), *big, 90.0f, 0.15f);
    ASSERT_EQ(found.parts.size(), 1u);
    EXPECT_EQ(again.contentHash(), found.contentHash());
    ASSERT_EQ(scaled.parts.size(), 1u);
    const auto a = partPoints(found, *head, 0), b = partPoints(scaled, *big, 0);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_LT((a[i] * 100.0f - b[i]).length(), 0.1f) << i;
    for (const Vec3f& p : a) EXPECT_NEAR(p.x, -0.35f, 0.05f);
}

// The farthest a point of `b` is from the polyline `a`.
float farthestFrom(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float worst = 0.0f;
    for (const Vec3f& q : b) {
        float best = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i + 1 < a.size(); ++i) {
            const Vec3f ab = a[i + 1] - a[i];
            const float t = std::clamp((q - a[i]).dot(ab) / std::max(ab.dot(ab), 1e-12f), 0.0f, 1.0f);
            best = std::min(best, (q - (a[i] + ab * t)).length());
        }
        worst = std::max(worst, best);
    }
    return worst;
}

TEST(FlowParts, ASmallNudgeMovesThePartALittle) {
    const auto head = weldedSphere(40, 80, 1.0f);
    const auto strands = combedStrands(*head, 1.0f, partedAt(0.0f));
    const PartSet before = findAutoParts(*strands, *head, 90.0f, 0.15f);
    ASSERT_EQ(before.parts.size(), 1u);
    const auto line = partPoints(before, *head, 0);
    // The split combed 0.02 to the side: the part follows, about as far, all along.
    const PartSet moved = findAutoParts(*combedStrands(*head, 1.0f, partedAt(0.02f)), *head, 90.0f, 0.15f);
    ASSERT_EQ(moved.parts.size(), 1u);
    const auto shifted = partPoints(moved, *head, 0);
    float meanX = 0.0f;
    for (const Vec3f& p : shifted) meanX += p.x / static_cast<float>(shifted.size());
    float meanBefore = 0.0f;
    for (const Vec3f& p : line) meanBefore += p.x / static_cast<float>(line.size());
    EXPECT_NEAR(meanX - meanBefore, 0.02f, 0.012f);
    // A few strands at the part turned the other way: the part barely moves (no pop).
    auto nudged = std::make_shared<StrandSet>(*strands);
    int turned = 0;
    for (std::size_t s = 0; s < nudged->strandCount() && turned < 3; ++s) {
        Vec3f* p = nudged->strandPoints(s);
        if (p[0].x > 0.0f && p[0].x < 0.03f && std::abs(p[0].z) < 0.3f) {
            for (std::size_t i = 1; i < nudged->strandPointCount(s); ++i) p[i].x = p[0].x - (p[i].x - p[0].x);
            ++turned;
        }
    }
    ASSERT_EQ(turned, 3);
    const PartSet after = findAutoParts(*nudged, *head, 90.0f, 0.15f);
    ASSERT_EQ(after.parts.size(), 1u);
    EXPECT_LT(farthestFrom(line, partPoints(after, *head, 0)), 0.02f);
}

TEST(FlowParts, PartNodeOpensFlowPartsAsPaintedOnes) {
    const auto head = weldedSphere(40, 80, 1.0f);
    const auto strands = combedStrands(*head, 1.0f, partedAt(0.0f));
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(head), head->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    auto part = OperatorRegistry::instance().create(op::kPart);
    part->params().set("width", 0.2);
    EXPECT_EQ(part->params().getString("partsFrom"), "painted");
    EXPECT_EQ(part->externalInputs(), (std::vector<std::string>{"scalp", "parts"}));
    // Painted, with nothing drawn: the strands pass through.
    EXPECT_EQ(valueAs<StrandsPtr>(part->evaluate(ctx, {StrandsPtr(strands)}))->positions(), strands->positions());
    part->params().set("partsFrom", std::string("auto"));
    EXPECT_EQ(part->externalInputs(), (std::vector<std::string>{"scalp"}));
    const StrandsPtr out = valueAs<StrandsPtr>(part->evaluate(ctx, {StrandsPtr(strands)}));
    // The same as the found parts painted: one push, one implementation.
    const PartSet found = findAutoParts(*strands, *head, 90.0f, 0.15f);
    ASSERT_EQ(found.parts.size(), 1u);
    const auto lines = std::make_shared<const PartLines>(found, *head);
    ext.set("parts", PartsPtr(lines), lines->contentHash());
    auto painted = OperatorRegistry::instance().create(op::kPart);
    painted->params().set("width", 0.2);
    EXPECT_EQ(valueAs<StrandsPtr>(painted->evaluate(ctx, {StrandsPtr(strands)}))->positions(), out->positions());
    EXPECT_EQ(valueAs<StrandsPtr>(part->evaluate(ctx, {StrandsPtr(strands)}))->positions(), out->positions());  // painted ignored
    // Strands by the part were turned, ones far from it not.
    std::size_t turned = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f root = out->strandPoints(s)[0];
        const bool moved = out->strandPoints(s)[6] != strands->strandPoints(s)[6];
        if (std::abs(root.x) > 0.25f) EXPECT_FALSE(moved) << root.x;
        turned += std::abs(root.x) < 0.1f && moved ? 1 : 0;
    }
    EXPECT_GT(turned, 50u);
    // What a host draws for the node: the same lines.
    const PartsPtr drawn = partLinesFor(part->params(), *strands, *head, nullptr);
    ASSERT_TRUE(drawn);
    EXPECT_EQ(drawn->contentHash(), lines->contentHash());
}

}  // namespace
}  // namespace groomlab
