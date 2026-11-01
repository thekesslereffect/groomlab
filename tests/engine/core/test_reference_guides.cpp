// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos (a prototype): the orientation field on known stripes, the shell
// from known silhouettes, the grower on a known field, and a small end-to-end case drawn from
// known guides.

#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/reference_guides.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;

ScalpMesh sphere(float radius, int rings = 24, int segments = 48) {
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

// A camera on `direction` from the origin at `distance`, looking at it, Y up (Z up for a camera on Y).
RefCamera camera(const Vec3f& direction, float distance, int size, float fovDegrees = 40.0f) {
    const Vec3f z = direction.normalized();
    const Vec3f up = std::abs(z.y) > 0.9f ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0);
    const Vec3f x = up.cross(z).normalized();
    const Vec3f y = z.cross(x);
    RefCamera c;
    for (int i = 0; i < 3; ++i) {
        c.pose.rotation[static_cast<std::size_t>(i)][0] = (i == 0 ? x.x : i == 1 ? x.y : x.z);
        c.pose.rotation[static_cast<std::size_t>(i)][1] = (i == 0 ? y.x : i == 1 ? y.y : y.z);
        c.pose.rotation[static_cast<std::size_t>(i)][2] = (i == 0 ? z.x : i == 1 ? z.y : z.z);
    }
    const Vec3f eye = z * distance;
    c.pose.position = {eye.x, eye.y, eye.z};
    c.width = c.height = size;
    c.focalPx = 0.5 * size / std::tan(0.5 * fovDegrees * kPi / 180.0f);
    return c;
}

RefImage blank(int w, int h, int channels, float value) {
    RefImage im;
    im.width = w;
    im.height = h;
    im.channels = channels;
    im.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(channels), value);
    return im;
}

// A field of one direction everywhere (angle from +u toward +v), full confidence.
std::shared_ptr<OrientationField> constantField(int w, int h, float angle) {
    auto f = std::make_shared<OrientationField>();
    f->width = w;
    f->height = h;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    f->angle.assign(n, angle);
    f->confidence.assign(n, 1.0f);
    f->tensor.resize(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        f->tensor[2 * i] = std::cos(2.0f * angle);
        f->tensor[2 * i + 1] = std::sin(2.0f * angle);
    }
    return f;
}

float angleBetween(float a, float b) {  // directions mod 180 degrees, in degrees
    float d = std::fmod(std::abs(a - b), kPi);
    if (d > 0.5f * kPi) d = kPi - d;
    return d * 180.0f / kPi;
}

TEST(ReferenceGuides, OrientationFieldFindsTheStripesDirection) {
    for (float degrees : {0.0f, 30.0f, 90.0f, 135.0f}) {
        const float a = degrees * kPi / 180.0f;
        RefImage im = blank(96, 96, 1, 0.0f);
        // Stripes running along `a`: the brightness varies across them, with a period of 5 pixels.
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x) {
                const float across = -static_cast<float>(x) * std::sin(a) + static_cast<float>(y) * std::cos(a);
                im.pixels[static_cast<std::size_t>(y * 96 + x)] = 0.5f + 0.4f * std::sin(2.0f * kPi * across / 5.0f);
            }
        const OrientationField f = orientationField(im, RefImage{});
        float du = 0, dv = 0, conf = 0;
        ASSERT_TRUE(f.sample(48.0f, 48.0f, du, dv, conf));
        EXPECT_LT(angleBetween(std::atan2(dv, du), a), 3.0f) << degrees;
        EXPECT_GT(conf, 0.3f) << degrees;
        EXPECT_LT(angleBetween(f.angle[48 * 96 + 48], a), 3.0f) << degrees;
    }
}

TEST(ReferenceLook, ClumpsAreTheBrightnessPeriodAcrossTheFlow) {
    // Vertical hair (the flow along v) in locks 14 pixels apart: brighter in the middle of each,
    // dark gaps between; fine strand-scale noise on top. Deeper gaps read as a larger swing.
    const int n = 200;
    const auto flow = constantField(n, n, 0.5f * kPi);
    float swing[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        RefImage lum = blank(n, n, 1, 0.0f);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float lock = 0.5f + 0.5f * std::cos(2.0f * kPi * static_cast<float>(x) / 14.0f);
                const float strand = ((x * 7 + y / 9) % 5) * 0.01f;
                lum.pixels[static_cast<std::size_t>(y * n + x)] = 0.15f * (1.0f + (k ? 0.9f : 0.3f) * (lock - 0.5f) * 2.0f) + strand;
            }
        const ClumpEstimate c = measureClumps(lum, *flow, RefImage{});
        EXPECT_GT(c.profiles, 50);
        EXPECT_GT(c.periodicShare, 0.8f);
        EXPECT_NEAR(c.spacingPx, 14.0f, 1.0f);
        swing[k] = c.contrast;
    }
    EXPECT_GT(swing[1], 2.0f * swing[0]);
}

TEST(ReferenceLook, StraysAreLinesLeavingTheBody) {
    // A dark round body of hair on a light backdrop; with strays, thin dark lines leaving it.
    const int n = 240;
    const auto flow = constantField(n, n, 0.5f * kPi);
    float density[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        RefImage gray = blank(n, n, 1, 0.8f), mask = blank(n, n, 1, 0.0f);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float dx = static_cast<float>(x) - 120.0f, dy = static_cast<float>(y) - 120.0f;
                if (dx * dx + dy * dy < 70.0f * 70.0f) {
                    gray.pixels[static_cast<std::size_t>(y * n + x)] = 0.25f;
                    mask.pixels[static_cast<std::size_t>(y * n + x)] = 1.0f;
                }
            }
        if (k)
            for (int s = 0; s < 24; ++s) {
                const float a = 2.0f * kPi * static_cast<float>(s) / 24.0f;
                for (float r = 70.0f; r < 95.0f; r += 0.5f) {
                    const int x = static_cast<int>(120.0f + r * std::cos(a)), y = static_cast<int>(120.0f + r * std::sin(a));
                    gray.pixels[static_cast<std::size_t>(y * n + x)] = 0.3f;
                }
            }
        const StrayEstimate s = measureStrays(gray, *flow, mask);
        EXPECT_GT(s.outlinePx, 300.0f);
        density[k] = s.strayDensity;
    }
    EXPECT_LT(density[0], 0.02f);  // the body's own edge is not a stray
    EXPECT_GT(density[1], 0.3f);
}

TEST(ReferenceGuides, ShellIsTheSilhouettesHull) {
    const ScalpMesh scalp = sphere(1.0f);
    const int size = 96;
    std::vector<ReferenceView> views;
    for (const Vec3f& dir : {Vec3f(0, 0, 1), Vec3f(1, 0, 0), Vec3f(0, 1, 0)}) {
        ReferenceView v;
        v.camera = camera(dir, 6.0f, size);
        // The hair: a ball of radius 1.4 around the scalp.
        v.mask = blank(size, size, 1, 0.0f);
        const float silhouette = static_cast<float>(v.camera.focalPx) * 1.4f / std::sqrt(36.0f - 1.96f);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float du = static_cast<float>(x) + 0.5f - 0.5f * size, dv = static_cast<float>(y) + 0.5f - 0.5f * size;
                v.mask.pixels[static_cast<std::size_t>(y * size + x)] = std::sqrt(du * du + dv * dv) < silhouette ? 1.0f : 0.0f;
            }
        v.field = constantField(size, size, 0.5f * kPi);
        views.push_back(v);
    }
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.shellResolution = 48;
    const ReferenceShell shell(scalp, views, options, 1.0f);
    EXPECT_TRUE(shell.inside(Vec3f(0, 1.25f, 0)));
    EXPECT_TRUE(shell.inside(Vec3f(0, 0, -1.25f)));
    EXPECT_TRUE(shell.inside(Vec3f(0.8f, 0.8f, 0.0f)));
    EXPECT_FALSE(shell.inside(Vec3f(1.3f, 1.3f, 0.0f)));    // outside the front and top silhouettes
    EXPECT_FALSE(shell.inside(Vec3f(0.0f, 1.9f, 0.0f)));    // outside the front and side ones
    EXPECT_FALSE(shell.inside(Vec3f(1.2f, 1.2f, 1.2f)));    // outside every one
    EXPECT_GT(shell.fill(), 0.01f);
    EXPECT_LT(shell.fill(), 0.5f);
}

TEST(ReferenceGuidesGrow, AVerticalFieldGrowsGuidesDownTheSurface) {
    const ScalpMesh scalp = sphere(1.0f);
    const int size = 128;
    ReferenceView v;
    v.camera = camera(Vec3f(0, 0, 1), 6.0f, size);
    v.mask = blank(size, size, 1, 1.0f);
    v.field = constantField(size, size, 0.5f * kPi);  // vertical lines in the picture
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.maxLength = 1.2f;
    options.lengthTrim = 1.0f;  // the grower's own length (no calibration)
    for (const Vec3f& at : {Vec3f(0, 0.3f, 1), Vec3f(0.4f, 0.4f, 0.8f), Vec3f(-0.5f, 0.2f, 0.8f)}) {
        const SurfacePoint sp = scalp.closestPoint(at.normalized());
        options.roots.push_back(sp.root);
    }
    const ReferenceGuidesResult r = guidesFromReference(scalp, {v}, options);
    ASSERT_EQ(r.guides.size(), 3u);
    for (const ReferenceGuide& g : r.guides) {
        const Vec3f root = g.points.front(), tip = g.points.back();
        // Down, keeping its x (the picture's lines are vertical: the plane through the camera and
        // a vertical line holds x nearly fixed), along the surface (not far off it).
        EXPECT_LT(tip.y, root.y - 0.6f);
        EXPECT_NEAR(tip.x, root.x, 0.25f);
        for (const Vec3f& p : g.points) EXPECT_LT(p.length(), 1.25f);
        float length = 0.0f;
        for (std::size_t i = 1; i < g.points.size(); ++i) length += (g.points[i] - g.points[i - 1]).length();
        EXPECT_NEAR(length, 1.2f, 0.15f);
    }
}

// Hints choose the way: the field alone says "vertical" (and gravity picks down); a stroke drawn
// upward, or a painted overlay whose stroke gets lighter upward, turns the guides up.
TEST(ReferenceGuidesGrow, HintStrokesChooseTheWay) {
    const ScalpMesh scalp = sphere(1.0f);
    const int size = 128;
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.maxLength = 0.8f;
    for (const Vec3f& at : {Vec3f(0, 0.0f, 1), Vec3f(0.3f, 0.1f, 0.9f), Vec3f(-0.3f, -0.1f, 0.9f)})
        options.roots.push_back(scalp.closestPoint(at.normalized()).root);
    auto view = [&] {
        ReferenceView v;
        v.camera = camera(Vec3f(0, 0, 1), 6.0f, size);
        v.mask = blank(size, size, 1, 1.0f);
        v.field = constantField(size, size, 0.5f * kPi);
        return v;
    };
    auto upward = [](const ReferenceGuidesResult& r) {
        int up = 0;
        for (const ReferenceGuide& g : r.guides) up += g.points.back().y > g.points.front().y ? 1 : 0;
        return up;
    };
    const ReferenceGuidesResult plain = guidesFromReference(scalp, {view()}, options);
    ASSERT_EQ(plain.guides.size(), 3u);
    EXPECT_EQ(upward(plain), 0);

    ReferenceView stroked = view();
    for (float u : {44.0f, 64.0f, 84.0f}) stroked.hints.push_back({{{u, 90.0f}, {u, 40.0f}}, true});  // drawn bottom to top
    const ReferenceGuidesResult hinted = guidesFromReference(scalp, {stroked}, options);
    ASSERT_EQ(hinted.guides.size(), 3u);
    EXPECT_EQ(upward(hinted), 3);

    ReferenceView painted = view();
    painted.hintOverlay = blank(size, size, 4, 0.0f);
    for (int y = 30; y < 100; ++y)
        for (int x = 40; x < 90; x += 6)
            for (int dx = 0; dx < 2; ++dx) {
                const std::size_t at = static_cast<std::size_t>((y * size + x + dx) * 4);
                const float light = 1.0f - static_cast<float>(y - 30) / 70.0f;  // dark at the bottom (the root end)
                painted.hintOverlay.pixels[at] = painted.hintOverlay.pixels[at + 1] = painted.hintOverlay.pixels[at + 2] = light;
                painted.hintOverlay.pixels[at + 3] = 1.0f;
            }
    EXPECT_EQ(upward(guidesFromReference(scalp, {painted}, options)), 3);
}

// Drawn guide lines say how long the hair is: short lines over the roots (a beard's) grow short
// guides, about as long as the lines; without them the guides run to the length.
TEST(ReferenceGuidesGrow, DrawnLinesSetTheLength) {
    const ScalpMesh scalp = sphere(1.0f);
    const int size = 128;
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.maxLength = 0.8f;
    for (const Vec3f& at : {Vec3f(0, 0.0f, 1), Vec3f(0.3f, 0.1f, 0.9f), Vec3f(-0.3f, -0.1f, 0.9f)})
        options.roots.push_back(scalp.closestPoint(at.normalized()).root);
    ReferenceView v;
    v.camera = camera(Vec3f(0, 0, 1), 6.0f, size);
    v.mask = blank(size, size, 1, 1.0f);
    v.field = constantField(size, size, 0.5f * kPi);
    auto lengths = [](const ReferenceGuidesResult& r) {
        std::vector<float> out;
        for (const ReferenceGuide& g : r.guides) {
            float l = 0.0f;
            for (std::size_t i = 1; i < g.points.size(); ++i) l += (g.points[i] - g.points[i - 1]).length();
            out.push_back(l);
        }
        return out;
    };
    const std::vector<float> plain = lengths(guidesFromReference(scalp, {v}, options));
    ASSERT_EQ(plain.size(), 3u);
    for (float l : plain) EXPECT_GT(l, 0.6f);
    // Lines down from each root, 0.3 units long at the sphere's front (its depth, 5).
    const float px = 0.3f * static_cast<float>(v.camera.focalPx) / 5.0f;
    ReferenceView drawn = v;
    for (const StrandRoot& r : options.roots) {
        float u = 0, w = 0;
        ASSERT_TRUE(v.camera.project(scalp.position(r), u, w));
        drawn.hints.push_back({{{u, w}, {u, w + px}}, true});
    }
    const std::vector<float> short_ = lengths(guidesFromReference(scalp, {drawn}, options));
    ASSERT_EQ(short_.size(), 3u);
    for (float l : short_) {
        EXPECT_LT(l, 0.45f);
        EXPECT_GT(l, 0.1f);
    }
}

TEST(ReferenceGuidesGrow, MirroredGuidesComeInPairs) {
    const ScalpMesh scalp = sphere(1.0f);
    const int size = 128;
    ReferenceView v;
    v.camera = camera(Vec3f(0.6f, 0, 0.8f), 6.0f, size);
    v.mask = blank(size, size, 1, 1.0f);
    v.field = constantField(size, size, 0.5f * kPi);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::WorldX;
    options.guideCount = 60;
    options.maxLength = 0.8f;
    const ReferenceGuidesResult r = guidesFromReference(scalp, {v}, options);
    int pairs = 0;
    for (const ReferenceGuide& g : r.guides) {
        if (g.mirrorOf < 0) continue;
        ++pairs;
        const ReferenceGuide& o = r.guides[static_cast<std::size_t>(g.mirrorOf)];
        ASSERT_EQ(o.points.size(), g.points.size());
        for (std::size_t i = 0; i < g.points.size(); ++i) {
            EXPECT_NEAR(g.points[i].x, -o.points[i].x, 1e-3f);
            EXPECT_NEAR(g.points[i].y, o.points[i].y, 1e-3f);
        }
    }
    EXPECT_GT(pairs, 5);
    // The camera sees the +x side best; the mirror covers -x roots the view never sees (behind
    // the sphere from it).
    int farSide = 0;
    for (const ReferenceGuide& g : r.guides)
        if (g.points[0].x < -0.6f && g.points[0].z < 0.0f) ++farSide;
    EXPECT_GT(farSide, 0);
}

// Known guides drawn as dark lines on white from two cameras, then grown back from their roots.
TEST(ReferenceGuidesEndToEnd, TwoDrawnViewsGiveTheGuidesBack) {
    const ScalpMesh scalp = sphere(1.0f);
    // The truth: from roots on the upper front, down along the surface, then hanging.
    std::vector<std::vector<Vec3f>> truth;
    std::vector<StrandRoot> roots;
    for (int i = 0; i < 6; ++i) {
        const float phi = (-0.75f + 1.5f * static_cast<float>(i) / 5.0f);
        const Vec3f root(std::sin(0.85f) * std::sin(phi), std::cos(0.85f), std::sin(0.85f) * std::cos(phi));
        const SurfacePoint sp = scalp.closestPoint(root);
        roots.push_back(sp.root);
        std::vector<Vec3f> g;
        for (int k = 0; k <= 30; ++k) {
            const float theta = 0.85f + (kPi * 0.5f - 0.85f) * static_cast<float>(k) / 30.0f;
            g.push_back(Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)) * 1.03f);
        }
        for (int k = 1; k <= 10; ++k) g.push_back(g.back() + Vec3f(0, -0.05f, 0));
        truth.push_back(g);
    }
    const int size = 192;
    std::vector<ReferenceView> views;
    for (const Vec3f& dir : {Vec3f(0, 0.2f, 1), Vec3f(1, 0.2f, 0.3f)}) {
        ReferenceView v;
        v.camera = camera(dir, 5.0f, size);
        v.image = blank(size, size, 3, 1.0f);
        v.mask = blank(size, size, 1, 0.0f);
        // Dense strands around each guide (a lock), drawn 1 pixel wide.
        for (const auto& g : truth)
            for (int s = -2; s <= 2; ++s) {
                const float shift = 0.04f * static_cast<float>(s);
                for (std::size_t k = static_cast<std::size_t>(4 * std::abs(s)); k + 1 < g.size(); ++k)  // a lock: its strands start staggered
                    for (int t = 0; t < 8; ++t) {
                        const Vec3f p = g[k] + (g[k + 1] - g[k]) * (static_cast<float>(t) / 8.0f) + Vec3f(shift, 0, 0);
                        float u = 0, vv = 0;
                        if (!v.camera.project(p, u, vv)) continue;
                        const int x = static_cast<int>(u), y = static_cast<int>(vv);
                        if (x < 0 || y < 0 || x >= size || y >= size) continue;
                        for (int c = 0; c < 3; ++c) v.image.pixels[static_cast<std::size_t>((y * size + x) * 3 + c)] = 0.15f;
                        for (int j = -2; j <= 2; ++j)
                            for (int i = -2; i <= 2; ++i)
                                if (x + i >= 0 && y + j >= 0 && x + i < size && y + j < size)
                                    v.mask.pixels[static_cast<std::size_t>((y + j) * size + x + i)] = 1.0f;
                    }
            }
        views.push_back(v);
    }
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.roots = roots;
    options.maxLength = 2.0f;
    options.points = 16;
    const ReferenceGuidesResult r = guidesFromReference(scalp, views, options);
    ASSERT_EQ(r.guides.size(), truth.size());
    double distance = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        const auto& g = r.guides[i].points;
        // Each reconstructed point to the nearest truth point (the truth is dense).
        for (const Vec3f& p : g) {
            float best = 1e9f;
            for (const Vec3f& q : truth[i]) best = std::min(best, (p - q).length());
            distance += best;
            ++n;
        }
        EXPECT_LT(g.back().y, g.front().y - 0.5f) << i;
    }
    EXPECT_LT(distance / n, 0.1);  // a tenth of the scalp's radius
}

}  // namespace
}  // namespace groomlab
