// SPDX-License-Identifier: Apache-2.0
// The orientation field between guides (guide_field.h) on a synthetic head (a ball of radius
// 1, its hair a shell over it): guides grown through it start at the scalp root first, follow the
// blend of the guides round them, stay off the head and inside the hull the photos carve, end near
// their neighbours' length and do not blend across a part.

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/hair_shell.h"
#include "groomlab/guide_field.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;

ScalpMesh ball(float radius, int rings = 32, int segments = 64) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            points.push_back(Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)) * radius);
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const int a = r * segments + s, b = r * segments + (s + 1) % segments, c = a + segments, d = b + segments;
            tris.push_back({a, c, d});
            tris.push_back({a, d, b});
        }
    return ScalpMesh(points, tris);
}

HeadFrame unitFrame() {
    HeadFrame f;
    f.radius = 1.0f;
    return f;
}

HairShell volumeShell(const ScalpMesh& head, float volume = 0.4f) {
    HairShellOptions o;
    o.volume = volume;
    return HairShell::fromVolume(head, unitFrame(), o);
}

// A guide from the scalp at `x` over the top toward the back (and `lean` toward +x as it goes), rising
// to `ride` off the head, root first, `length` along the head's angle (radians).
std::vector<Vec3f> overTheTop(float x, float lean, float ride = 0.2f, float from = 0.25f, float sweep = 1.4f) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 40; ++i) {
        const float t = static_cast<float>(i) / 40.0f;
        const float a = from + sweep * t;  // from up toward the back
        const float side = x + lean * t;
        Vec3f d(side, std::cos(a), -std::sin(a));
        d = d.normalized();
        pts.push_back(d * (1.0f + ride * std::min(1.0f, 4.0f * t)));
    }
    return pts;
}

Vec3f rootAt(float x, float a) { return Vec3f(x, std::cos(a), -std::sin(a)).normalized(); }

GuideFieldOptions fineGrid() {
    GuideFieldOptions o;
    o.cell = 0.05f;
    return o;
}

TEST(GuideField, GuidesGrownBetweenTwoGuidesFollowTheirBlend) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = volumeShell(head);
    // Two guides over the top toward the back, one leaning to +x as it goes, one straight.
    const std::vector<FieldGuide> keys{{overTheTop(0.35f, 0.6f), 1.0f}, {overTheTop(-0.35f, 0.0f), 1.0f}};
    const GuideField field(shell, keys, fineGrid(), &head);
    const GuideField::Stats& st = field.stats();
    EXPECT_GT(st.cells, 1000);
    EXPECT_GT(st.guideCells, 50);
    EXPECT_GT(st.ringCells, 100);
    EXPECT_LT(st.residual, 1e-3f);
    // Between them, the field heads back, leaning +x a little (between the two guides' leans).
    const Vec3f mid = Vec3f(0.0f, std::cos(0.9f), -std::sin(0.9f)).normalized() * 1.18f;
    const Vec3f v = field.at(mid);
    EXPECT_GT(v.length(), 0.3f);
    const Vec3f d = v.normalized();
    EXPECT_LT(d.z, -0.4f);  // back
    EXPECT_GT(d.x, 0.0f);   // the leaning guide's share
    const Vec3f lean = field.at(Vec3f(0.35f + 0.6f * 0.45f, std::cos(0.9f), -std::sin(0.9f)).normalized() * 1.18f).normalized();
    EXPECT_GT(lean.x, d.x);  // more lean nearer the leaning guide

    // A guide grown from a root between them: root first, from the root, off the head, back.
    const Vec3f root = rootAt(0.0f, 0.3f);
    const GrownGuide g = field.grow(root, 2.0f);
    ASSERT_GE(g.points.size(), 8u);
    EXPECT_LT((g.points.front() - root).length(), 1e-6f);
    for (const Vec3f& p : g.points) EXPECT_GE(p.length(), 0.995f) << "into the head";
    EXPECT_GT(g.points[2].length(), root.length() + 0.01f);  // leaves the scalp
    // Root out: farther from the root as it goes.
    EXPECT_GT((g.points[g.points.size() * 3 / 4] - root).length(), (g.points[g.points.size() / 4] - root).length());
    EXPECT_LT(g.points.back().z, root.z - 0.6f);
    EXPECT_GT(g.points.back().x, -0.15f);
    EXPECT_LT(g.points.back().x, 0.6f);
    // Inside the shell.
    for (const Vec3f& p : g.points) EXPECT_LE(p.length(), shell.outer(p) + 1.5f * field.cell());
}

TEST(GuideField, GuidesStopWhereTheHullEnds) {
    const ScalpMesh head = ball(1.0f);
    // A thin shell (0.15) and a guide going down the back to well below the head's middle.
    const HairShell shell = volumeShell(head, 0.4f);
    const std::vector<FieldGuide> keys{{overTheTop(0.0f, 0.0f, 0.15f, 0.3f, 2.6f), 1.0f}};
    const GuideField field(shell, keys, fineGrid(), &head);
    // Next to it, unbounded length: it ends where the shell does (the guide's own cells aside) or
    // where its tip's fade dies out, never far past it.
    const GrownGuide g = field.grow(rootAt(0.25f, 0.35f), 10.0f);
    ASSERT_GE(g.points.size(), 8u);
    EXPECT_NE(g.stop, GrowStop::Length);
    const float keyLength = guideLength(keys[0].points);
    EXPECT_LT(g.length, keyLength * 1.4f);
    for (const Vec3f& p : g.points) EXPECT_LE(p.length(), shell.outer(p) + 1.5f * field.cell());
}

TEST(GuideField, NotBlendedAcrossThePart) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = volumeShell(head);
    // A part at x = 0 over the top front: the guides on each side sweep away from it.
    auto sweep = [](float x0, float sign) {
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 30; ++i) {
            const float t = static_cast<float>(i) / 30.0f;
            const float a = sign * (0.12f + 1.3f * t);  // from near the top toward the side
            Vec3f d(std::sin(a) + x0, std::cos(a), 0.35f);
            pts.push_back(d.normalized() * (1.0f + 0.2f * std::min(1.0f, 4.0f * t)));
        }
        return pts;
    };
    std::vector<FieldGuide> keys{{sweep(0.0f, 1.0f), 1.0f}, {sweep(0.0f, -1.0f), 1.0f}, {sweep(0.15f, 1.0f), 1.0f}, {sweep(-0.15f, -1.0f), 1.0f}};
    std::vector<std::vector<Vec3f>> lines;
    for (const auto& k : keys) lines.push_back(k.points);
    int votes = 0;
    const PartLine found = partFromGuides(unitFrame(), lines, &votes);
    EXPECT_EQ(votes, 4);
    ASSERT_TRUE(found.on);
    EXPECT_LT(std::abs((found.point - Vec3f()).dot(found.normal)), 0.12f);
    EXPECT_GT(std::abs(found.normal.x), 0.99f);

    GuideFieldOptions withPart = fineGrid();
    withPart.part = {true, Vec3f(0, 0, 0), Vec3f(1, 0, 0)};
    const GuideField parted(shell, keys, withPart, &head);
    const GuideField blended(shell, keys, fineGrid(), &head);
    EXPECT_GT(parted.stats().partSeams, 10);
    // Roots right by the part grow away from it on their own side.
    for (const float x : {0.04f, -0.04f}) {
        const Vec3f root = Vec3f(x, 1.0f, 0.3f).normalized();
        const GrownGuide g = parted.grow(root, 1.2f);
        ASSERT_GE(g.points.size(), 6u);
        if (x > 0.0f) EXPECT_GT(g.points.back().x, root.x + 0.3f);
        else EXPECT_LT(g.points.back().x, root.x - 0.3f);
    }
    // Blended across, the field by the part is weaker (the two sides cancel).
    const Vec3f byPart = Vec3f(0.03f, 1.0f, 0.3f).normalized() * 1.12f;
    EXPECT_GT(parted.at(byPart).length(), blended.at(byPart).length() + 0.1f);
}

TEST(GuideField, FilledGuidesTakeTheirNeighboursLengths) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = volumeShell(head);
    const std::vector<FieldGuide> keys{{overTheTop(0.3f, 0.0f, 0.2f, 0.25f, 0.8f), 1.0f}, {overTheTop(-0.3f, 0.0f, 0.2f, 0.25f, 0.8f), 1.0f}};
    const float keyLength = guideLength(keys[0].points);
    std::vector<Vec3f> roots;
    for (int i = 0; i < 12; ++i) roots.push_back(rootAt(-0.25f + 0.5f * static_cast<float>(i) / 11.0f, 0.25f + 0.02f * static_cast<float>(i % 3)));
    FieldFillOptions o;
    o.field = fineGrid();
    const FieldFill fill = fieldFill(roots, keys, shell, o, &head);
    ASSERT_EQ(fill.guides.size(), roots.size());
    EXPECT_EQ(fill.byLength + fill.byFade + fill.byHull + fill.byTurn + fill.empty, static_cast<int>(roots.size()));
    EXPECT_EQ(fill.empty, 0);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        const auto& g = fill.guides[i];
        ASSERT_GE(g.size(), 2u);
        EXPECT_LT((g.front() - roots[i]).length(), 1e-6f);
        const float len = guideLength(g);
        EXPECT_LE(len, keyLength * (1.0f + o.lengthSlack) * (1.0f + o.lengthRandom) + 0.06f);
        EXPECT_GT(len, 0.5f * keyLength);  // (the field carries on to about their tips)
        // Riding about as high as the keys (0.2 off the head) past their first stretch, not along the scalp.
        EXPECT_GT(g[g.size() * 2 / 3].length(), 1.1f);
    }
    // No keys: refused.
    EXPECT_THROW(fieldFill(roots, {}, shell, o), std::invalid_argument);
}

TEST(GuideField, AWaveOnTopKeepsTheRootAndLength) {
    const HeadFrame f = unitFrame();
    const std::vector<Vec3f> g = overTheTop(0.0f, 0.0f);
    CombOptions o;
    o.flip = 0.0f;
    const std::vector<Vec3f> w = wavedGuide(g, f, o, 3);
    ASSERT_EQ(w.size(), g.size());
    EXPECT_EQ(w.front(), g.front());
    EXPECT_NEAR(guideLength(w), guideLength(g), 0.25f * guideLength(g));
    float moved = 0.0f;
    for (std::size_t i = 0; i < g.size(); ++i) moved = std::max(moved, (w[i] - g[i]).length());
    EXPECT_GT(moved, 0.03f);  // waved
    EXPECT_LT(moved, 0.15f);
}

}  // namespace
}  // namespace groomlab
