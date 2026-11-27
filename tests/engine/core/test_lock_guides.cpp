// SPDX-License-Identifier: Apache-2.0
// Lock guides (lock_guides.h) round the hair's shell (hair_shell.h), on a synthetic head (a ball)
// photographed by synthetic cameras: the head's frame fitted, the shell carved from known hair
// masks, a known curve on the shell projected into two cameras and lifted back, mirrored guides,
// strokes read off an image, combed guides (root out, off the head, away from the part) and guides
// filled in from keys, cut where the photos show the hair end.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/hair_shell.h"
#include "groomlab/lock_guides.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// A ball of `radius` round `center`, Y up.
ScalpMesh ball(float radius, const Vec3f& center = {}, int rings = 32, int segments = 64, float thetaMax = kPi) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = thetaMax * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            points.push_back(center + Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)) * radius);
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

// A camera on `direction` from the origin at `distance`, looking at it, Y up.
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

HeadFrame unitFrame() {
    HeadFrame f;
    f.center = {0, 0, 0};
    f.radius = 1.0f;
    return f;
}

// The hair: a ball of radius 1.4 over a head of radius 1, down to y = -0.2. A photo's mask: the
// pixels whose ray passes through it.
bool inHair(const Vec3f& p) { return p.length() < 1.4f && p.y > -0.2f; }
ShellPhoto photographed(const Vec3f& dir, int size = 192) {
    ShellPhoto p;
    p.camera = camera(dir, 8.0f, size);
    p.width = p.height = size;
    p.hair.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0);
    const Vec3f eye = p.camera.eye();
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const Vec3f d = p.camera.ray(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            bool hair = false;
            for (float t = 6.0f; t < 10.0f && !hair; t += 0.01f) hair = inHair(eye + d * t);
            p.hair[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = hair ? 1 : 0;
        }
    return p;
}

std::vector<ShellPhoto> aroundTheHead() {
    std::vector<ShellPhoto> photos;
    for (int k = 0; k < 8; ++k) {
        const float a = 2.0f * kPi * static_cast<float>(k) / 8.0f;
        photos.push_back(photographed(Vec3f(std::sin(a), 0.15f, std::cos(a))));
    }
    photos.push_back(photographed(Vec3f(0.05f, 1.0f, 0.1f)));
    return photos;
}

TEST(HairShell, TheHeadsFrameIsTheCraniumsSphere) {
    const Vec3f center(1.0f, 2.0f, 3.0f);
    const ScalpMesh cap = ball(10.0f, center, 16, 48, 0.6f * kPi);  // the hair region: a cap
    const HeadFrame f = headFrameOf(cap, Vec3f(0, 1, 0), Vec3f(0, 0.2f, 1));
    EXPECT_LT((f.center - center).length(), 0.05f);
    EXPECT_NEAR(f.radius, 10.0f, 0.05f);
    EXPECT_NEAR(f.front.dot(f.up), 0.0f, 1e-5f);  // made square to up
    EXPECT_NEAR(f.left.x, 1.0f, 1e-5f);           // up x front
    EXPECT_EQ(headRegion(f, Vec3f(0, 1, 0)), "top");
    EXPECT_EQ(headRegion(f, Vec3f(0, 0.1f, 1)), "front");
    EXPECT_EQ(headRegion(f, Vec3f(1, 0.1f, 0)), "sides");
    EXPECT_EQ(headRegion(f, Vec3f(0, 0.2f, -1)), "back");
    EXPECT_EQ(headRegion(f, Vec3f(0, -0.5f, -1)), "nape");
}

TEST(HairShell, CarvedFromTheMasksTheHairsThickness) {
    const ScalpMesh head = ball(1.0f);
    HairShellOptions o;
    o.maskGrow = 1;
    const HairShell shell = HairShell::fromMasks(head, unitFrame(), aroundTheHead(), o);
    EXPECT_EQ(shell.photoCount(), 9);
    EXPECT_NEAR(shell.head(Vec3f(0, 1, 0)), 1.0f, 0.03f);
    // Over the top and round the sides the hair is 0.4 thick (the photos' hull of a ball: a little more).
    EXPECT_NEAR(shell.thickness(Vec3f(0, 1, 0)), 0.45f, 0.08f);
    EXPECT_NEAR(shell.thickness(Vec3f(1, 0.3f, 0)), 0.4f, 0.12f);
    EXPECT_NEAR(shell.thickness(Vec3f(0, 0.3f, -1)), 0.4f, 0.12f);
    // Below where the hair ends: none.
    EXPECT_LT(shell.thickness(Vec3f(0, -1, 0.2f)), 0.08f);
    EXPECT_LT(shell.thickness(Vec3f(0.7f, -0.7f, 0)), 0.1f);
    EXPECT_TRUE(shell.seenHair(Vec3f(0, 1.2f, 0)));
    EXPECT_TRUE(shell.seenBare(Vec3f(0, 1.7f, 0)));
    EXPECT_TRUE(shell.seenBare(Vec3f(0, -1.2f, 0.3f)));
    EXPECT_FALSE(shell.seenBare(Vec3f(0.0f, 0.0f, 0.0f)));  // (inside the head: no photo sees it)
    // By region, over the hair (the whole ball here).
    const std::vector<ShellRegionStats> stats = shellStats(shell, head);
    ASSERT_EQ(stats.size(), 5u);
    EXPECT_EQ(stats[0].region, "top");
    EXPECT_NEAR(stats[0].mean, 0.45f, 0.1f);
    EXPECT_GT(stats[0].covered, 0.95f);
    EXPECT_EQ(stats[4].region, "nape");
    EXPECT_LT(stats[4].covered, stats[0].covered);  // (below y = -0.2 none)
}

TEST(HairShell, AGuessedVolumeWithoutPhotos) {
    const ScalpMesh head = ball(1.0f);
    HairShellOptions o;
    o.volume = 0.3f;
    const HairShell shell = HairShell::fromVolume(head, unitFrame(), o);
    EXPECT_EQ(shell.photoCount(), 0);
    EXPECT_NEAR(shell.thickness(Vec3f(0, 1, 0)), 0.3f, 0.02f);   // fullest on top
    EXPECT_LT(shell.thickness(Vec3f(0, -1, 0)), 0.1f);           // close underneath
    EXPECT_FALSE(shell.seenBare(Vec3f(0, 3, 0)));
    // A ray through the middle meets it at its outer radius; one past it takes its nearest point.
    const HairShell::Hit hit = shell.raycast(Vec3f(0, 0, 10), Vec3f(0, 0, -1));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.z, shell.outer(Vec3f(0, 0, 1)), 0.01f);
    const HairShell::Hit miss = shell.raycast(Vec3f(0, 3, 10), Vec3f(0, 0, -1));
    EXPECT_FALSE(miss.hit);
    EXPECT_NEAR(miss.point.z, 0.0f, 0.05f);  // (no deeper than the head's middle)
    EXPECT_GT(miss.gap, 0.0f);
}

// A curve on the shell (the points it is outermost at along their directions), root first.
std::vector<Vec3f> curveOnShell(const HairShell& shell) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 40; ++i) {
        const float t = static_cast<float>(i) / 40.0f;
        const float theta = (20.0f + 60.0f * t) * kPi / 180.0f, phi = (10.0f + 25.0f * std::sin(3.0f * t)) * kPi / 180.0f;
        const Vec3f d(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
        pts.push_back(d * shell.outer(d));
    }
    return pts;
}

TEST(LockGuides, AStrokeOnTwoPhotosLiftsBackToItsCurve) {
    const ScalpMesh head = ball(1.0f);
    HairShellOptions o;
    o.volume = 0.4f;
    const HairShell shell = HairShell::fromVolume(head, unitFrame(), o);
    const std::vector<Vec3f> truth = curveOnShell(shell);
    for (const Vec3f& dir : {Vec3f(0.1f, 0.2f, 1.0f), Vec3f(0.8f, 0.5f, 0.6f)}) {
        const RefCamera cam = camera(dir, 8.0f, 512);
        std::vector<std::array<float, 2>> pixels;
        for (const Vec3f& p : truth) {
            float u = 0.0f, v = 0.0f;
            ASSERT_TRUE(cam.project(p, u, v));
            pixels.push_back({u, v});
        }
        const LiftedStroke lifted = liftStroke(cam, pixels, shell, head);
        EXPECT_GT(lifted.hits, 9 * lifted.misses);
        // The root on the scalp under the first point; every other point near the curve.
        EXPECT_NEAR(lifted.points.front().length(), 1.0f, 0.02f);
        EXPECT_LT((lifted.points.front().normalized() - truth.front().normalized()).length(), 0.05f);
        float worst = 0.0f;
        std::size_t onShell = 1;  // (past the straight run from the root up to the stroke's first point)
        while (onShell < lifted.points.size() && (lifted.points[onShell] - truth.front()).length() > 0.03f &&
               lifted.points[onShell].length() < truth.front().length() - 0.02f)
            ++onShell;
        EXPECT_LT(onShell, 12u);
        for (std::size_t i = onShell; i < lifted.points.size(); ++i) {
            float nearest = 1e9f;
            for (std::size_t k = 0; k + 1 < truth.size(); ++k) {
                const Vec3f a = truth[k], b = truth[k + 1];
                const float s = std::clamp((lifted.points[i] - a).dot(b - a) / (b - a).dot(b - a), 0.0f, 1.0f);
                nearest = std::min(nearest, (lifted.points[i] - (a + (b - a) * s)).length());
            }
            worst = std::max(worst, nearest);
        }
        EXPECT_LT(worst, 0.04f) << "camera " << dir.x << " " << dir.y << " " << dir.z;
        EXPECT_NEAR(lifted.length, guideLength(lifted.points), 1e-4f);
    }
}

// A stroke drawn tip first (from the end of a lock hanging below the hair region back up to the top
// of the head) is turned round when lifted: its root on the scalp where the same stroke drawn root
// first has its root (guide_rooting.h).
TEST(LockGuides, AStrokeDrawnTipFirstIsTurnedRound) {
    const ScalpMesh head = ball(1.0f), cap = ball(1.0f, Vec3f(0, 0, 0), 16, 64, 0.5f * kPi);
    HairShellOptions o;
    o.volume = 0.4f;
    const HairShell shell = HairShell::fromVolume(head, unitFrame(), o);
    std::vector<Vec3f> truth;
    for (int i = 0; i <= 20; ++i) {
        const float theta = (20.0f + 60.0f * static_cast<float>(i) / 20.0f) * kPi / 180.0f, phi = 0.4f;
        const Vec3f d(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
        truth.push_back(d * shell.outer(d));
    }
    const Vec3f last = truth.back();
    for (int i = 1; i <= 12; ++i) truth.push_back(last + Vec3f(0.0f, -0.12f * static_cast<float>(i), 0.0f));
    const RefCamera cam = camera(Vec3f(0.6f, 0.2f, 1.0f), 8.0f, 512);
    std::vector<std::array<float, 2>> pixels;
    for (const Vec3f& p : truth) {
        float u = 0.0f, v = 0.0f;
        ASSERT_TRUE(cam.project(p, u, v));
        pixels.push_back({u, v});
    }
    const LiftedStroke forward = liftStroke(cam, pixels, shell, cap, {}, &head);
    std::reverse(pixels.begin(), pixels.end());
    const LiftedStroke backward = liftStroke(cam, pixels, shell, cap, {}, &head);
    ASSERT_GE(forward.points.size(), 2u);
    ASSERT_GE(backward.points.size(), 2u);
    EXPECT_FALSE(forward.rooting.flipped);
    EXPECT_TRUE(backward.rooting.flipped);
    EXPECT_LT((backward.points.front() - forward.points.front()).length(), 0.05f);
    EXPECT_LT((backward.points.back() - forward.points.back()).length(), 0.15f);
    EXPECT_LT(backward.points.back().y, -0.5f);  // the tip hangs
}

TEST(LockGuides, MirroredGuidesLandOnTheOtherSide) {
    const ScalpMesh head = ball(1.0f);
    const std::vector<Vec3f> guide{Vec3f(0.6f, 0.8f, 0.0f), Vec3f(0.9f, 0.9f, 0.1f), Vec3f(1.2f, 0.5f, 0.2f)};
    const std::vector<Vec3f> m = mirroredGuide(guide, Vec3f(0, 0, 0), Vec3f(1, 0, 0), &head);
    ASSERT_EQ(m.size(), guide.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
        EXPECT_NEAR(m[i].x, -guide[i].x, 0.02f);
        EXPECT_NEAR(m[i].y, guide[i].y, 0.02f);
        EXPECT_NEAR(m[i].z, guide[i].z, 0.02f);
    }
    EXPECT_NEAR(m.front().length(), 1.0f, 0.01f);  // the root on the scalp
}

TEST(LockGuides, StrokesAreReadOffAnImageRootAtTheDot) {
    // A gray image at twice a 100 x 80 photo's size, offset (6, 4): a red line from (20, 20) to
    // (80, 60) in the photo, a dot at its far end, a blue line besides (another color: not read).
    const float scale = 2.0f;
    const std::array<float, 2> offset{6.0f, 4.0f};
    RefImage im;
    im.width = 212;
    im.height = 168;
    im.channels = 3;
    im.pixels.assign(static_cast<std::size_t>(im.width * im.height * 3), 0.5f);
    auto paint = [&](float px, float py, float r, const std::array<float, 3>& c) {
        const float cx = px * scale + offset[0], cy = py * scale + offset[1];
        for (int y = static_cast<int>(cy - r) - 1; y <= static_cast<int>(cy + r) + 1; ++y)
            for (int x = static_cast<int>(cx - r) - 1; x <= static_cast<int>(cx + r) + 1; ++x) {
                if (x < 0 || y < 0 || x >= im.width || y >= im.height) continue;
                if ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy) > r * r) continue;
                for (int k = 0; k < 3; ++k) im.pixels[static_cast<std::size_t>((y * im.width + x) * 3 + k)] = c[static_cast<std::size_t>(k)];
            }
    };
    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f;
        paint(20.0f + 60.0f * t, 20.0f + 40.0f * t, 2.0f, {0.9f, 0.1f, 0.1f});
        paint(20.0f + 60.0f * t, 70.0f, 2.0f, {0.1f, 0.2f, 0.9f});
    }
    paint(80.0f, 60.0f, 7.0f, {0.9f, 0.1f, 0.1f});
    StrokeImageOptions o;
    o.scale = scale;
    o.offset = offset;
    o.photoWidth = 100;
    o.photoHeight = 80;
    const std::vector<ImageStroke> strokes = strokesFromImage(im, o);
    ASSERT_EQ(strokes.size(), 1u);
    const ImageStroke& s = strokes.front();
    EXPECT_GT(s.rootBlob, s.tipBlob);
    EXPECT_NEAR(s.points.front()[0], 80.0f, 4.0f);  // the dot's end first
    EXPECT_NEAR(s.points.front()[1], 60.0f, 4.0f);
    EXPECT_NEAR(s.points.back()[0], 20.0f, 3.0f);
    EXPECT_NEAR(s.points.back()[1], 20.0f, 3.0f);
    for (const auto& q : s.points) {  // on the line (in the photo's pixels)
        const float d = std::abs(40.0f * (q[0] - 20.0f) - 60.0f * (q[1] - 20.0f)) / std::sqrt(40.0f * 40.0f + 60.0f * 60.0f);
        EXPECT_LT(d, 2.0f);
    }
    o.root = "top";  // the root the upper end
    EXPECT_NEAR(strokesFromImage(im, o).front().points.front()[1], 20.0f, 3.0f);
}

TEST(LockGuides, CombedGuidesGrowRootOutOffTheHeadAwayFromThePart) {
    const ScalpMesh head = ball(1.0f);
    HairShellOptions so;
    so.volume = 0.3f;
    const HairShell shell = HairShell::fromVolume(head, unitFrame(), so);
    CombOptions o;
    o.part = {true, Vec3f(0.2f, 0, 0), Vec3f(1, 0, 0)};
    // The part's sides on the front of the top: away from it; the back falls.
    struct Case {
        Vec3f root;
        float side;  // +1: should end on +x of its root, -1 on -x, 0 any
    };
    const std::vector<Case> cases{{Vec3f(0.45f, 0.75f, 0.5f).normalized(), 1.0f},
                                  {Vec3f(-0.25f, 0.75f, 0.6f).normalized(), -1.0f},
                                  {Vec3f(0.0f, 0.3f, -1.0f).normalized(), 0.0f},
                                  {Vec3f(0.9f, 0.2f, 0.1f).normalized(), 0.0f}};
    for (std::size_t k = 0; k < cases.size(); ++k) {
        const Vec3f root = cases[k].root;
        const std::vector<Vec3f> g = combGuide(root, 1.6f, shell, o, k);
        ASSERT_GE(g.size(), 10u);
        EXPECT_LT((g.front() - root).length(), 1e-5f);
        EXPECT_NEAR(guideLength(g), 1.6f, 0.5f);
        for (const Vec3f& p : g) EXPECT_GE(p.length(), 0.99f) << "into the head";
        // Root out: it leaves the root (its far half farther than its near half).
        EXPECT_GT((g[g.size() * 3 / 4] - root).length(), (g[g.size() / 4] - root).length());
        if (cases[k].side > 0.0f) EXPECT_GT(g.back().x, root.x + 0.1f);
        if (cases[k].side < 0.0f) EXPECT_LT(g.back().x, root.x - 0.1f);
        // The front and top go back over the head; the back and sides fall.
        if (root.z > 0.3f) EXPECT_LT(g.back().z, root.z);
        else EXPECT_LT(g.back().y, root.y);
    }
    // The comb field lies along the surface.
    for (const Vec3f& p : {Vec3f(0, 1, 0), Vec3f(0.5f, 0.5f, 0.7f), Vec3f(-0.8f, 0.0f, -0.6f)}) {
        const Vec3f d = combDirection(shell.frame(), p * 1.2f, o);
        EXPECT_NEAR(d.length(), 1.0f, 1e-4f);
        EXPECT_LT(std::abs(d.dot(p.normalized())), 1e-3f);
    }
    // Lengths by region, the defaults as shares of the head's radius.
    CombOptions fixed;
    fixed.lengthRandom = 0.0f;
    EXPECT_NEAR(combLength(shell.frame(), Vec3f(0, 0.3f, -1), fixed, 1), 2.3f, 1e-4f);
    fixed.lengths.back = 0.5f;
    EXPECT_NEAR(combLength(shell.frame(), Vec3f(0, 0.3f, -1), fixed, 1), 0.5f, 1e-4f);
}

TEST(LockGuides, FilledGuidesTakeTheNearKeysShapes) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = HairShell::fromVolume(head, unitFrame());
    FillOptions o;
    o.points = 20;
    o.reach = 0.4f;
    // A key rising and bending to +x from the top.
    std::vector<Vec3f> key;
    for (int i = 0; i < 20; ++i) {
        const float t = static_cast<float>(i) / 19.0f;
        key.push_back(Vec3f(0.5f * t * t, 1.0f + 0.3f * t, 0.0f));
    }
    const std::vector<std::vector<Vec3f>> keys{key};
    std::vector<Vec3f> combed;
    const Vec3f near = Vec3f(0.0f, 1.0f, 0.05f).normalized(), far = Vec3f(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 20; ++i) combed.push_back(near + Vec3f(0, 0, -0.05f * static_cast<float>(i)));
    const FilledGuide close = fillGuide(near, keys, combed, shell, o);
    EXPECT_GT(close.keyShare, 0.95f);
    EXPECT_LT((close.points.back() - (near + (key.back() - key.front()))).length(), 0.02f);  // the key's shape from its own root
    std::vector<Vec3f> combedFar;
    for (int i = 0; i < 20; ++i) combedFar.push_back(far + Vec3f(0, -0.05f * static_cast<float>(i), -0.02f));
    const FilledGuide alone = fillGuide(far, keys, combedFar, shell, o);
    EXPECT_LT(alone.keyShare, 1e-3f);
    EXPECT_LT((alone.points.back() - combedFar.back()).length(), 1e-5f);  // the combed guide where no key is near
}

TEST(LockGuides, GuidesAreCutWhereThePhotosShowTheHairEnd) {
    const ScalpMesh head = ball(1.0f);
    const HairShell shell = HairShell::fromMasks(head, unitFrame(), aroundTheHead());
    // Down the side of the head, just outside it, to well below where the hair ends (y = -0.2).
    std::vector<Vec3f> g;
    for (int i = 0; i <= 40; ++i) {
        const float a = (10.0f + 150.0f * static_cast<float>(i) / 40.0f) * kPi / 180.0f;
        g.push_back(Vec3f(std::sin(a), std::cos(a), 0.0f) * 1.15f);
    }
    const std::vector<Vec3f> cut = clippedToPhotos(g, shell);
    ASSERT_LT(cut.size(), g.size());
    EXPECT_GT(cut.back().y, -0.5f);
    EXPECT_LT(cut.back().y, 0.1f);
    // Without photos: as it was.
    EXPECT_EQ(clippedToPhotos(g, HairShell::fromVolume(head, unitFrame())).size(), g.size());
}

}  // namespace
}  // namespace groomlab
