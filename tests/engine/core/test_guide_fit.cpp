// SPDX-License-Identifier: Apache-2.0
// Guides fitted to strokes on several photos (guide_fit.h), on a synthetic head (a ball) under a
// guessed hair volume, photographed by two synthetic cameras (front and side): a lock drawn in both
// photos comes back in 3D though it lies under the shell's outside (where one photo's lift puts it);
// a stroke only a grazing photo sees stays near the shell, smooth, instead of sinking or zig-zagging;
// strokes of the same lock in the two photos are matched, others left alone.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/guide_fit.h"
#include "groomlab/hair_shell.h"
#include "groomlab/lock_guides.h"

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

RefCamera camera(const Vec3f& direction, float distance = 8.0f, int size = 512, float fovDegrees = 40.0f) {
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

HairShell shellOver(const ScalpMesh& head) {
    HeadFrame f;
    f.center = {0, 0, 0};
    f.radius = 1.0f;
    HairShellOptions o;
    o.volume = 0.4f;
    return HairShell::fromVolume(head, f, o);
}

// A lock: from near the scalp at theta0 down to theta1 (degrees from up), round phi0 (degrees toward
// +x from the front), waving `wave` degrees sideways, at `height` of the way from the head to the
// shell's outside.
std::vector<Vec3f> lock(const HairShell& shell, float theta0, float theta1, float phi0, float wave, float height) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 50; ++i) {
        const float t = static_cast<float>(i) / 50.0f;
        const float theta = (theta0 + (theta1 - theta0) * t) * kPi / 180.0f;
        const float phi = (phi0 + wave * std::sin(3.0f * t)) * kPi / 180.0f;
        const Vec3f d(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
        const float r = shell.head(d) + height * std::min(1.0f, 10.0f * t + 0.1f) * shell.thickness(d);
        pts.push_back(d * r);
    }
    return pts;
}

// The lock drawn on a photo: the points it sees (not behind the head), in its pixels.
FitStroke drawn(const std::vector<Vec3f>& lock, const RefCamera& cam, const ScalpMesh& head, int photo, int stroke) {
    FitStroke s;
    s.camera = cam;
    s.photo = photo;
    s.stroke = stroke;
    for (const Vec3f& p : lock) {
        const Vec3f eye = cam.eye();
        const Vec3f d = (p - eye).normalized();
        SurfacePoint hit;
        float t = 0.0f;
        if (head.intersectRay(eye, d, &hit, &t) && t < (p - eye).length() - 1e-3f) continue;
        float u = 0.0f, v = 0.0f;
        if (cam.project(p, u, v)) s.pixels.push_back({u, v});
    }
    return s;
}

float distanceTo(const std::vector<Vec3f>& curve, const Vec3f& p) {
    float best = 1e9f;
    for (std::size_t k = 0; k + 1 < curve.size(); ++k) {
        const Vec3f a = curve[k], b = curve[k + 1];
        const float s = std::clamp((p - a).dot(b - a) / std::max(1e-12f, (b - a).dot(b - a)), 0.0f, 1.0f);
        best = std::min(best, (p - (a + (b - a) * s)).length());
    }
    return best;
}

// The worst distance to the truth over the guide's points past its first `skip` (head radii of
// arc: the straight run from the root up to the lock).
float worstPast(const std::vector<Vec3f>& guide, const std::vector<Vec3f>& truth, float skip) {
    float s = 0.0f, worst = 0.0f;
    for (std::size_t i = 1; i < guide.size(); ++i) {
        s += (guide[i] - guide[i - 1]).length();
        if (s > skip) worst = std::max(worst, distanceTo(truth, guide[i]));
    }
    return worst;
}

std::vector<Vec3f> lifted(const FitStroke& s, const HairShell& shell, const ScalpMesh& head) {
    return liftStroke(s.camera, s.pixels, shell, head, {}, &head).points;
}

const Vec3f kFront(0.1f, 0.25f, 1.0f), kSide(1.0f, 0.25f, 0.15f);

// The fit's options as guides_from_strokes sets them: rooted again on the head as a stroke's lift is.
GuideFitOptions fitOptions(const ScalpMesh& scalp) {
    GuideFitOptions o;
    o.scalp = &scalp;
    o.rooting = StrokeLiftOptions{}.rooting;
    return o;
}

TEST(GuideFit, ALockDrawnInTwoPhotosComesBackIn3D) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = shellOver(head);
    // Under the shell's outside (60% of the way out), where a lift from one photo cannot put it.
    const std::vector<Vec3f> truth = lock(shell, 15.0f, 80.0f, 40.0f, 15.0f, 0.6f);
    const FitStroke front = drawn(truth, camera(kFront), head, 0, 1), side = drawn(truth, camera(kSide), head, 1, 1);
    ASSERT_GT(front.pixels.size(), 40u);
    ASSERT_GT(side.pixels.size(), 40u);
    const std::vector<Vec3f> lift = lifted(front, shell, head);
    ASSERT_GE(lift.size(), 10u);
    const float liftError = worstPast(lift, truth, 0.3f);
    EXPECT_GT(liftError, 0.08f);  // (the lift is on the shell's outside: off by its depth)

    const FittedGuide fit = fitGuide(lift, {front, side}, shell, &head, fitOptions(head));
    const float fitError = worstPast(fit.points, truth, 0.3f);
    EXPECT_LT(fitError, 0.04f) << "lift " << liftError;
    EXPECT_LT(fit.energyAfter, fit.energyBefore);
    ASSERT_EQ(fit.views.size(), 2u);
    for (const FitViewResidual& v : fit.views) {
        EXPECT_LT(v.meanPx, 1.5f) << "photo " << v.photo;
        EXPECT_GT(v.strokeCovered, 0.8f) << "photo " << v.photo;
    }
    // The root on the scalp, nearer the lock's start than the lift's (under its start on the shell's
    // outside); the guide about the lock's length.
    EXPECT_NEAR(fit.points.front().length(), 1.0f, 0.02f);
    const float liftRoot = (lift.front().normalized() - truth.front().normalized()).length();
    EXPECT_LT((fit.points.front().normalized() - truth.front().normalized()).length(), 0.6f * liftRoot) << "lift's " << liftRoot;
    float truthLength = 0.0f;
    for (std::size_t i = 1; i < truth.size(); ++i) truthLength += (truth[i] - truth[i - 1]).length();
    EXPECT_NEAR(fit.lengthAfter, truthLength, 0.15f * truthLength);
    for (const Vec3f& p : fit.points) EXPECT_GT(p.length(), 0.99f);  // (out of the head)
}

// A lock on the side of the head (on the hair's outside, where the front photo sees it) drawn only
// on the front photo, with a hand's wobble: its rays graze the shell, so the lift's depths jump
// between where they first meet the shell and where they pass it, and the smoothing mixes them. The
// fit keeps it near the shell's outside, out of the head and smooth in depth.
TEST(GuideFit, AGrazingStrokeStaysNearTheShellAndSmooth) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = shellOver(head);
    const std::vector<Vec3f> truth = lock(shell, 30.0f, 100.0f, 72.0f, 6.0f, 1.0f);
    FitStroke front = drawn(truth, camera(Vec3f(0.0f, 0.1f, 1.0f)), head, 0, 1);
    ASSERT_GT(front.pixels.size(), 20u);
    std::vector<std::array<float, 2>> wobbly = front.pixels;
    for (std::size_t i = 1; i + 1 < wobbly.size(); ++i) {
        const float du = front.pixels[i + 1][0] - front.pixels[i - 1][0], dv = front.pixels[i + 1][1] - front.pixels[i - 1][1];
        const float l = std::max(1e-6f, std::hypot(du, dv)), w = 3.0f * std::sin(0.9f * static_cast<float>(i));
        wobbly[i] = {front.pixels[i][0] - dv / l * w, front.pixels[i][1] + du / l * w};
    }
    front.pixels = wobbly;
    const std::vector<Vec3f> lift = lifted(front, shell, head);
    ASSERT_GE(lift.size(), 10u);
    const FittedGuide fit = fitGuide(lift, {front}, shell, &head, fitOptions(head));
    ASSERT_EQ(fit.views.size(), 1u);
    EXPECT_LT(fit.views[0].meanPx, 3.0f);
    EXPECT_GT(fit.views[0].strokeCovered, 0.8f);
    // Never out past the shell, nor (its lower half, the part the photo places) far under its outside; out of the head.
    auto depths = [&](const std::vector<Vec3f>& g, float& deepest, float& outest) {
        float s = 0.0f;
        deepest = outest = 0.0f;
        const float half = 0.5f * guideLength(g);
        for (std::size_t i = 1; i < g.size(); ++i) {
            s += (g[i] - g[i - 1]).length();
            const float gap = g[i].length() - shell.outer(g[i]);
            if (s > half) deepest = std::max(deepest, -gap);
            outest = std::max(outest, gap);
        }
    };
    float deepest = 0.0f, outest = 0.0f, liftDeepest = 0.0f, liftOutest = 0.0f;
    depths(fit.points, deepest, outest);
    depths(lift, liftDeepest, liftOutest);
    EXPECT_LT(deepest, 0.1f) << "lift's " << liftDeepest;
    EXPECT_LT(outest, 0.04f) << "lift's " << liftOutest;
    for (const Vec3f& p : fit.points) EXPECT_GT(p.length(), 0.99f);
    // (its start, high on the head, one photo cannot place: the lower half is what it sees well)
    EXPECT_LT(worstPast(fit.points, truth, 0.5f * fit.lengthAfter), 0.2f) << "lift's " << worstPast(lift, truth, 0.5f * guideLength(lift));
    // Smoother than the lift: the summed squared turning (second differences) less.
    auto roughness = [](const std::vector<Vec3f>& g) {
        double r = 0.0;
        for (std::size_t i = 1; i + 1 < g.size(); ++i) {
            const Vec3f d = g[i + 1] - g[i] * 2.0f + g[i - 1];
            r += static_cast<double>(d.dot(d));
        }
        return r;
    };
    EXPECT_LT(roughness(fit.points), roughness(lift));
}

TEST(GuideFit, StrokesOfOneLockInTwoPhotosAreMatched) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = shellOver(head);
    const RefCamera front = camera(kFront), side = camera(kSide);
    // (on the hair's outside, where the photos that drew them see them)
    const std::vector<Vec3f> a = lock(shell, 15.0f, 80.0f, 40.0f, 15.0f, 0.9f);
    const std::vector<Vec3f> b = lock(shell, 25.0f, 95.0f, 75.0f, 10.0f, 0.95f);
    const std::vector<Vec3f> c = lock(shell, 20.0f, 85.0f, -40.0f, 10.0f, 0.9f);  // (the side photo does not see it)
    // In the order a user might draw them: the front photo's three, then the side's two (b first).
    std::vector<FitStroke> strokes = {drawn(a, front, head, 0, 1), drawn(b, front, head, 0, 2), drawn(c, front, head, 0, 3),
                                      drawn(b, side, head, 1, 1), drawn(a, side, head, 1, 2)};
    std::vector<std::vector<Vec3f>> lifts;
    for (FitStroke& s : strokes) {
        ASSERT_GT(s.pixels.size(), 20u);
        const LiftedStroke l = liftStroke(s.camera, s.pixels, shell, head, {}, &head);
        lifts.push_back(l.points);
        s.rootGap = l.rooting.rootGap;  // (as guides_from_strokes gives it: a group roots from its best rooted stroke)
    }
    const StrokeFitResult r = fitStrokes(strokes, lifts, shell, &head, true, fitOptions(head));
    EXPECT_EQ(r.fused, 2);
    EXPECT_EQ(r.split, 0);
    ASSERT_EQ(r.groups.size(), 3u);
    auto members = [](const StrokeFitGroup& g) {
        std::vector<int> m = g.strokes;
        std::sort(m.begin(), m.end());
        return m;
    };
    EXPECT_EQ(members(r.groups[0]), (std::vector<int>{0, 4}));
    EXPECT_EQ(members(r.groups[1]), (std::vector<int>{1, 3}));
    EXPECT_EQ(members(r.groups[2]), (std::vector<int>{2}));
    // (past their start: carried back from where the strokes begin, down onto the head)
    EXPECT_LT(worstPast(r.groups[0].guide.points, a, 0.3f * r.groups[0].guide.lengthAfter), 0.05f);
    EXPECT_LT(worstPast(r.groups[1].guide.points, b, 0.3f * r.groups[1].guide.lengthAfter), 0.05f);
    // Unfused: a guide a stroke.
    const StrokeFitResult apart = fitStrokes(strokes, lifts, shell, &head, false, fitOptions(head));
    EXPECT_EQ(apart.groups.size(), 5u);
    EXPECT_EQ(apart.fused, 0);
    // Forced: strokes 2 and 3 made one lock (they are not: the joint fit cannot fit both, so it is split back).
    const std::vector<int> forced = {-1, -1, 7, 7, -1};
    const StrokeFitResult made = fitStrokes(strokes, lifts, shell, &head, true, fitOptions(head), {}, &forced);
    EXPECT_EQ(made.split, 2);
}

}  // namespace
}  // namespace groomlab
