// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos (a prototype): the orientation field on known stripes, the shell
// from known silhouettes, the grower on a known field, and a small end-to-end case drawn from
// known guides.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/color.h"
#include "groomlab/curve.h"
#include "groomlab/hair_color.h"
#include "groomlab/reference_guides.h"
#include "groomlab/strand_set.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// A ball, or its cap down to `thetaMax` from the top (+Y).
ScalpMesh sphere(float radius, int rings = 24, int segments = 48, float thetaMax = kPi) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = thetaMax * static_cast<float>(r) / static_cast<float>(rings);
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

// The variance bank (OrientationMethod::Variance): clean stripes' direction within a couple of
// degrees and sure; where two stripe patterns cross, or on noise, the filters' energy spreads over
// many directions and the confidence (1 / Var^2) falls.
TEST(ReferenceGuides, VarianceBankFindsStripesAndDoubtsCrossings) {
    OrientationOptions o;
    o.method = OrientationMethod::Variance;
    o.fillWeak = false;
    o.smoothSigma = 0.0f;
    o.edgeWidth = 0.0f;
    auto stripes = [](RefImage& im, float a, float period, float amplitude) {
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) {
                const float across = -static_cast<float>(x) * std::sin(a) + static_cast<float>(y) * std::cos(a);
                im.pixels[static_cast<std::size_t>(y * im.width + x)] += amplitude * std::sin(2.0f * kPi * across / period);
            }
    };
    auto middle = [](const OrientationField& f, float& angle) {  // the mean confidence over the middle, its angle
        double c = 0.0;
        int n = 0;
        for (int y = 32; y < 64; ++y)
            for (int x = 32; x < 64; ++x) {
                c += f.confidence[static_cast<std::size_t>(y * f.width + x)];
                ++n;
            }
        angle = f.angle[static_cast<std::size_t>(48 * f.width + 48)];
        return static_cast<float>(c / n);
    };
    float stripeConfidence = 1.0f;
    for (float degrees : {0.0f, 30.0f, 90.0f, 135.0f, 163.0f}) {
        for (float period : {4.5f, 5.5f}) {
            const float a = degrees * kPi / 180.0f;
            RefImage im = blank(96, 96, 1, 0.5f);
            stripes(im, a, period, 0.4f);
            const OrientationField f = orientationField(im, RefImage{}, o);
            float angle = 0.0f;
            const float conf = middle(f, angle);
            EXPECT_LT(angleBetween(angle, a), 2.0f) << degrees << " " << period;
            EXPECT_GT(conf, 0.6f) << degrees << " " << period;
            stripeConfidence = std::min(stripeConfidence, conf);
        }
    }
    // Two stripe patterns crossing at right angles.
    RefImage cross = blank(96, 96, 1, 0.5f);
    stripes(cross, 0.3f, 4.8f, 0.2f);
    stripes(cross, 0.3f + 0.5f * kPi, 4.8f, 0.2f);
    float angle = 0.0f;
    const float crossing = middle(orientationField(cross, RefImage{}, o), angle);
    EXPECT_LT(crossing, 0.5f * stripeConfidence);
    // Noise.
    RefImage noise = blank(96, 96, 1, 0.0f);
    std::uint32_t state = 11;
    for (float& v : noise.pixels) {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
    const float noisy = middle(orientationField(noise, RefImage{}, o), angle);
    EXPECT_LT(noisy, 0.3f * stripeConfidence);
}

// Hair hanging down a picture in locks, each strand a dark line on a light backdrop; each lock
// swinging side to side by `amplitude` pixels every `wavelength` (0: straight), its own phase.
RefImage lockedStrands(int size, float wavelength, float amplitude, std::uint32_t seed = 7) {
    RefImage im = blank(size, size, 1, 0.75f);
    std::uint32_t state = seed;
    auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    const int lockWidth = 12;
    for (int lock = 0; lock * lockWidth < size; ++lock) {
        const float phase = 2.0f * kPi * random();
        for (int s = 0; s < 5; ++s) {
            const float x0 = static_cast<float>(lock * lockWidth) + 2.0f + 2.0f * static_cast<float>(s) + 0.6f * random();
            const float shade = 0.1f + 0.15f * random();
            for (float y = 0.0f; y < static_cast<float>(size); y += 0.25f) {
                const float x = x0 + (wavelength > 0.0f ? amplitude * std::sin(2.0f * kPi * y / wavelength + phase) : 0.0f) + 0.3f * (random() - 0.5f);
                const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(y);
                if (xi < 0 || xi >= size || yi >= size) continue;
                im.pixels[static_cast<std::size_t>(yi * size + xi)] = shade;
            }
        }
    }
    return im;
}

TEST(ReferenceLook, CurlsAreTheFlowSwingingWithAPeriod) {
    // Locks swinging every 24-48 pixels read as curls of that period, swinging widely (what
    // guidesFromReference calls curly: kCurlShare 0.22 of the flow, over kCurlAmplitude 7.5 degrees);
    // straight locks' flow hardly swings at all.
    const int size = 256;
    auto measure = [&](float wavelength, float amplitude) {
        const RefImage im = lockedStrands(size, wavelength, amplitude);
        const OrientationField fine = orientationField(im, RefImage{});
        const OrientationField coarse = coarseField(fine, RefImage{}, 0.04f * static_cast<float>(size));
        return measureCurl(fine, coarse, RefImage{});
    };
    const CurlEstimate straight = measure(0.0f, 0.0f);
    EXPECT_GT(straight.streamlines, 500);
    EXPECT_LT(straight.amplitude * 180.0f / kPi, 3.0f);
    for (const auto& [wavelength, amplitude] : {std::pair{24.0f, 2.0f}, std::pair{32.0f, 4.0f}, std::pair{48.0f, 4.0f}}) {
        const CurlEstimate c = measure(wavelength, amplitude);
        EXPECT_GT(c.periodicShare, 0.3f) << wavelength;
        EXPECT_GT(c.amplitude * 180.0f / kPi, 10.0f) << wavelength;
        EXPECT_NEAR(c.wavelengthPx, wavelength, 0.15f * wavelength) << wavelength;
    }
}

TEST(ReferenceLook, MelaninOfAppearanceIsHairColorsInverse) {
    // Every melanin and redness back from its appearance (HairColor's model) ...
    for (float m : {0.1f, 0.3f, 0.5f, 0.6f, 0.8f})
        for (float r : {0.0f, 0.3f, 0.7f}) {
            float melanin = -1.0f, redness = -1.0f;
            const float error = melaninOfAppearance(hairAppearanceColor(melaninColor(m, r)), melanin, redness);
            EXPECT_LT(error, 0.02f) << m << " " << r;
            EXPECT_NEAR(melanin, m, 0.021f) << m << " " << r;
            if (m <= 0.6f) EXPECT_NEAR(redness, r, 0.11f) << m << " " << r;  // (darker: redness hardly shows)
        }
    // ... and a dark brown photographed under softer light than the even dome (less of its highlight)
    // as dark as it is, not a lighter brown's.
    const std::array<float, 3> dome = hairAppearanceColor(melaninColor(0.6f, 0.0f));
    std::array<float, 3> soft{};
    for (std::size_t c = 0; c < 3; ++c) soft[c] = dome[c] - 0.6f * kAppearanceSheen;
    float melanin = 0.0f, redness = 0.0f;
    melaninOfAppearance(soft, melanin, redness);
    EXPECT_NEAR(melanin, 0.6f, 0.04f);
}

// The synthetic round trip: a known groom on a ball's cap (the ball a whole head's stand-in, a neck
// under it) photographed from four sides on a light backdrop. Strands from roots over the cap, lifted
// off it (volume), along it away from the top, then hanging down past the ball's middle (short over
// the face: a fringe); curling round their way with `curlWavelength` and `curlRadius` (0: straight).
// The ball and a neck under it (not an occluder: as a photo's neck and shoulders) and the strands
// photographed from four sides on a light backdrop, 256 pixels.
std::vector<ReferenceView> photographed(const std::vector<std::vector<Vec3f>>& truth, float melanin) {
    std::vector<ReferenceView> views;
    std::uint32_t state = 5;
    auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    const std::array<float, 3> hairColor = hairAppearanceColor(melaninColor(melanin, 0.1f));
    const int size = 512;  // drawn at twice the photos' size, then halved (smooth edges, as a photo's)
    for (const Vec3f& dir : {Vec3f(0, 0.25f, 1), Vec3f(1, 0.25f, 0), Vec3f(0, 0.25f, -1), Vec3f(-1, 0.25f, 0)}) {
        ReferenceView v;
        v.camera = camera(dir, 5.0f, size);
        v.image = blank(size, size, 3, 0.75f);
        std::vector<float> depth(static_cast<std::size_t>(size) * size, 1e9f);
        const Vec3f eye = v.camera.eye();
        // The ball and the neck (not an occluder: as a photo's neck and shoulders): skin, lit from
        // the camera's side.
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float du = static_cast<float>(x) + 0.5f - 0.5f * size, dv = static_cast<float>(y) + 0.5f - 0.5f * size;
                const Vec3f ray = (v.camera.axis(0) * du - v.camera.axis(1) * dv - v.camera.axis(2) * static_cast<float>(v.camera.focalPx)).normalized();
                float t = 1e9f;
                Vec3f n;
                const float b = eye.dot(ray), c = eye.dot(eye) - 1.0f, disc = b * b - c;
                if (disc >= 0.0f) {
                    t = -b - std::sqrt(disc);
                    n = (eye + ray * t).normalized();
                }
                const float a2 = ray.x * ray.x + ray.z * ray.z, b2 = eye.x * ray.x + eye.z * ray.z, c2 = eye.x * eye.x + eye.z * eye.z - 0.25f;
                const float disc2 = b2 * b2 - a2 * c2;
                if (disc2 >= 0.0f && a2 > 1e-9f) {
                    const float tc = (-b2 - std::sqrt(disc2)) / a2;
                    const Vec3f q = eye + ray * tc;
                    if (q.y < -0.45f && tc < t) {
                        t = tc;
                        n = Vec3f(q.x, 0.0f, q.z).normalized();
                    }
                }
                if (t >= 1e9f) continue;
                float hu = 0, hv = 0, hz = 0;
                v.camera.project(eye + ray * t, hu, hv, &hz);
                const float lit = 0.35f + 0.65f * std::max(0.0f, -n.dot(ray));
                const float skin[3] = {0.85f, 0.62f, 0.5f};
                for (int k = 0; k < 3; ++k) v.image.pixels[static_cast<std::size_t>((y * size + x) * 3 + k)] = skin[k] * lit;
                depth[static_cast<std::size_t>(y * size + x)] = hz;
            }
        // The strands, each its own shade of the hair's color, two pixels wide, hidden behind the ball.
        state = 5;
        for (const auto& s : truth) {
            const float shade = 0.6f + 0.8f * random();
            for (std::size_t k = 1; k < s.size(); ++k)
                for (int f = 0; f < 2; ++f) {
                    const Vec3f p = s[k - 1] + (s[k] - s[k - 1]) * (static_cast<float>(f) / 2.0f);
                    float u = 0, vv = 0, z = 0;
                    if (!v.camera.project(p, u, vv, &z)) continue;
                    for (int j = 0; j < 2; ++j) {
                        const int x = static_cast<int>(u) + j, y = static_cast<int>(vv);
                        if (x < 0 || y < 0 || x >= size || y >= size || z > depth[static_cast<std::size_t>(y * size + x)] + 0.01f) continue;
                        for (int c = 0; c < 3; ++c)
                            v.image.pixels[static_cast<std::size_t>((y * size + x) * 3 + c)] = linearToSrgb01(hairColor[static_cast<std::size_t>(c)] * shade);
                    }
                }
        }
        v.image = v.image.resized(size / 2, size / 2);
        v.camera = camera(dir, 5.0f, size / 2);
        for (float& p : v.image.pixels) p = std::round(p * 255.0f) / 255.0f;  // (8 bits, as a photo)
        views.push_back(v);
    }
    return views;
}

// With `locks` the strands grow in that many locks, each curling as one (its own phase and
// handedness, its strands about its axis), the locks' axes kept (lockAxes).
struct SyntheticShoot {
    ScalpMesh cap;
    std::shared_ptr<ScalpMesh> head;
    std::vector<ReferenceView> views;
    std::vector<std::vector<Vec3f>> lockAxes;
};

SyntheticShoot syntheticShoot(float melanin, float curlWavelength = 0.0f, float curlRadius = 0.0f, int locks = 0) {
    // (the cap the ball's own top 9 rings of 24: the same triangles)
    const float capTheta = kPi * 9.0f / 24.0f;
    SyntheticShoot shoot{sphere(1.0f, 9, 48, capTheta), std::make_shared<ScalpMesh>(sphere(1.0f, 24, 48)), {}};
    std::vector<std::vector<Vec3f>> truth;
    std::uint32_t state = 11;
    auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    struct Lock {
        float theta, phi, phase, hand;
    };
    std::vector<Lock> lockList;
    for (int l = 0; l < locks; ++l)
        lockList.push_back({std::acos(1.0f - random() * (1.0f - std::cos(capTheta - 0.12f))), 2.0f * kPi * random(), 2.0f * kPi * random(),
                            random() < 0.5f ? 1.0f : -1.0f});
    for (int i = 0; i < 6000 + (locks ? 1 : 0) * locks; ++i) {
        // (with locks: first each lock's axis, then its strands round it)
        const bool axis = locks && i < locks;
        const Lock* lock = locks ? &lockList[static_cast<std::size_t>(axis ? i : i % locks)] : nullptr;
        float theta0 = std::acos(1.0f - random() * (1.0f - std::cos(capTheta - 0.05f))), phi = 2.0f * kPi * random();
        float phase = 2.0f * kPi * random(), hand = 1.0f;
        // (a lock's strands: its middle strand's path moved a little across the flow and off the
        // scalp, so they curl together as a rope)
        float acrossShift = 0.0f, outShift = 0.0f;
        if (lock) {
            theta0 = lock->theta;
            phi = lock->phi;
            phase = lock->phase;
            hand = lock->hand;
            if (!axis) {
                acrossShift = 0.05f * (2.0f * random() - 1.0f);
                outShift = 0.03f * random();
            }
        }
        std::vector<Vec3f> s;
        float theta = theta0, y = 0.0f;
        bool hanging = false;
        const float length = std::cos(phi) > 0.4f ? 0.55f : 1.2f;
        for (float t = 0.0f; t <= length; t += 0.01f) {
            const float radius = 1.0f + std::min(0.2f, 0.8f * t);
            Vec3f p, across, out;
            if (!hanging) {
                out = Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
                p = out * radius;
                across = Vec3f(std::cos(phi), 0.0f, -std::sin(phi));
                theta += 0.01f / radius;
                if (theta > 0.5f * kPi) {
                    hanging = true;
                    y = p.y;
                }
            } else {
                y -= 0.01f;
                out = Vec3f(std::sin(phi), 0.0f, std::cos(phi));
                p = Vec3f(out.x * radius, y, out.z * radius);
                across = Vec3f(std::cos(phi), 0.0f, -std::sin(phi));
            }
            if (curlWavelength > 0.0f && !axis) {
                const float a = 2.0f * kPi * t / curlWavelength + phase;
                p = p + (across * std::cos(a) + out * std::sin(a) * hand) * (curlRadius * std::min(1.0f, t / 0.1f));
            }
            s.push_back(p + across * acrossShift + out * outShift);
        }
        if (axis) shoot.lockAxes.push_back(s);
        else truth.push_back(s);
    }
    shoot.views = photographed(truth, melanin);
    return shoot;
}

// A groom from guides: a lock of a dozen strands round each, a little apart, coming together at the root.
StrandSet lockStrands(const std::vector<ReferenceGuide>& guides) {
    StrandSet strands;
    StrandId id = 1;
    std::uint32_t state = 3;
    auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    for (const ReferenceGuide& g : guides)
        for (int k = 0; k < 12; ++k) {
            const Vec3f shift(0.12f * (random() - 0.5f), 0.12f * (random() - 0.5f), 0.12f * (random() - 0.5f));
            std::vector<Vec3f> pts;
            for (std::size_t i = 0; i < g.points.size(); ++i) pts.push_back(g.points[i] + shift * (static_cast<float>(i) / static_cast<float>(g.points.size())));
            strands.addStrand(id++, pts);
        }
    return strands;
}

// Grown back from its photos, the known groom's guides (a lock of strands round each) match each
// photo's hair through its camera, and the look reads the hair's color.
TEST(ReferenceGuidesEndToEnd, AKnownGroomGrowsBackFromItsPhotos) {
    const SyntheticShoot shoot = syntheticShoot(0.6f);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 200;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    ASSERT_GT(r.guides.size(), 100u);
    ASSERT_TRUE(r.look.ok);
    EXPECT_NEAR(r.look.melanin, 0.6f, 0.08f);
    EXPECT_FALSE(r.look.curly) << r.look.curlShare << " " << r.look.curlAmplitudeDeg;
    EXPECT_EQ(r.locksTraced + r.locksFilled, 0);  // (straight hair: no locks' curls)
    const StrandSet strands = lockStrands(r.guides);
    double sum = 0.0;
    for (const ReferenceView& v : shoot.views) {
        const PhotoComparison c = compareToPhoto(shoot.cap, strands, v, options);
        EXPECT_GT(c.photoPixels, 2000);
        EXPECT_GT(c.iou, 0.7) << c.missedShare << " " << c.extraShare;
        EXPECT_NEAR(c.photoMelanin, 0.6f, 0.08f);
        sum += c.iou;
    }
    EXPECT_GT(sum / static_cast<double>(shoot.views.size()), 0.74);
}

// The same groom curling: its photos read as curly, with about the curls' period.
TEST(ReferenceGuidesEndToEnd, CurlyHairReadsAsCurly) {
    const float wavelength = 0.25f;
    const SyntheticShoot shoot = syntheticShoot(0.3f, wavelength, 0.04f);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 100;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    ASSERT_TRUE(r.look.ok);
    EXPECT_TRUE(r.look.curly) << r.look.curlShare << " " << r.look.curlAmplitudeDeg;
    EXPECT_NEAR(r.look.curlFrequency, 1.0f / wavelength, 0.3f / wavelength);
}

// Curly locks: traced back one guide a lock, each guide curling as its lock, near its axis.
TEST(ReferenceGuidesEndToEnd, CurlyLocksAreTracedOneGuideALock) {
    const float wavelength = 0.3f, radius = 0.05f;
    const SyntheticShoot shoot = syntheticShoot(0.6f, wavelength, radius, 150);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 80;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    ASSERT_TRUE(r.look.curly) << r.look.curlShare << " " << r.look.curlAmplitudeDeg;
    EXPECT_GT(r.locksTraced, static_cast<int>(r.guides.size()) / 4);
    EXPECT_EQ(r.locksTraced + r.locksFilled, static_cast<int>(r.guides.size()));
    EXPECT_NEAR(r.lockRadius, radius, 0.25f * radius);
    EXPECT_GT(r.locksDetected, 20);
    EXPECT_GT(r.lockRoots, 10);
    EXPECT_NEAR(r.lockWavelength, wavelength, 0.25f * wavelength);
    // Each traced guide runs near a true lock (its axis, within the lock's width and the curl).
    double near = 0.0;
    int n = 0;
    for (const ReferenceGuide& g : r.guides) {
        if (g.lock != 1) continue;
        double best = 1e9;
        for (const auto& axis : shoot.lockAxes) {
            double sum = 0.0;
            for (const Vec3f& p : g.points) {
                float d = 1e9f;
                for (const Vec3f& q : axis) d = std::min(d, (p - q).length());
                sum += d;
            }
            best = std::min(best, sum / static_cast<double>(g.points.size()));
        }
        near += best;
        ++n;
    }
    ASSERT_GT(n, 0);
    EXPECT_LT(near / n, 0.16);
    // A lock of strands round each guide curls with the photos' locks: full-size curls not quite in
    // their place cost a little of the flow's agreement against smooth guides, no more.
    ReferenceGuideOptions plain = options;
    plain.lockCurls = false;
    const ReferenceGuidesResult straight = guidesFromReference(shoot.cap, shoot.views, plain);
    double curled = 0.0, uncurled = 0.0;
    for (const ReferenceView& v : shoot.views) {
        const PhotoComparison a = compareToPhoto(shoot.cap, lockStrands(r.guides), v, options);
        const PhotoComparison b = compareToPhoto(shoot.cap, lockStrands(straight.guides), v, options);
        curled += a.flowAgreement;
        uncurled += b.flowAgreement;
    }
    EXPECT_GT(curled, uncurled - 0.06 * static_cast<double>(shoot.views.size()));
}

// Long swept-back waves: locks from roots over the cap running back over the head (lifted off it),
// then falling down the back past the ball's middle, each lock waving side to side as one (a
// `wavelength` long, `amplitude` wide), its strands a rope about its middle (lockAxes: the middles).
SyntheticShoot sweptBackShoot(float melanin, int locks, float wavelength, float amplitude) {
    const float capTheta = kPi * 9.0f / 24.0f;
    SyntheticShoot shoot{sphere(1.0f, 9, 48, capTheta), std::make_shared<ScalpMesh>(sphere(1.0f, 24, 48)), {}};
    std::uint32_t state = 21;
    auto random = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    };
    std::vector<std::vector<Vec3f>> truth;
    const float step = 0.01f, length = 2.2f;
    for (int l = 0; l < locks; ++l) {
        const float theta0 = std::acos(1.0f - random() * (1.0f - std::cos(capTheta - 0.12f))), phi0 = 2.0f * kPi * random();
        const float phase = 2.0f * kPi * random();
        // The middle: over the head toward the back (and a little down), lifted, then down the back.
        std::vector<Vec3f> middle, side;
        Vec3f p = Vec3f(std::sin(theta0) * std::sin(phi0), std::cos(theta0), std::sin(theta0) * std::cos(phi0));
        for (float t = 0.0f; t <= length; t += step) {
            const float lift = 1.0f + std::min(0.15f, 0.6f * t);
            Vec3f n = p.normalized();
            const bool falling = p.z < -0.55f || p.y < 0.2f;
            Vec3f want = falling ? Vec3f(0.0f, -1.0f, -0.15f) : Vec3f(0.0f, -0.25f, -1.0f);
            Vec3f tangent = want - n * want.dot(n);
            if (falling) tangent = want;
            tangent = tangent.normalized();
            const Vec3f across = n.cross(tangent).normalized();
            middle.push_back(n * lift + across * (amplitude * std::sin(2.0f * kPi * t / wavelength + phase) * std::min(1.0f, t / 0.2f)));
            side.push_back(across);
            p = falling ? p + tangent * step : (n + tangent * step).normalized();
            if (p.y < -1.4f) break;
        }
        shoot.lockAxes.push_back(middle);
        for (int k = 0; k < 40; ++k) {
            const float shift = 0.05f * (2.0f * random() - 1.0f), out = 0.03f * random();
            std::vector<Vec3f> strand;
            for (std::size_t i = 0; i < middle.size(); ++i) strand.push_back(middle[i] + side[i] * shift + middle[i].normalized() * out);
            truth.push_back(strand);
        }
    }
    shoot.views = photographed(truth, melanin);
    return shoot;
}

// Grown back from its photos, long swept-back waves: guides near the true locks' middles, most of
// their length, following their waves at least as well as the same guides smoothed straight.
TEST(ReferenceGuidesEndToEnd, LongSweptBackWavesGrowBackFullLength) {
    const SyntheticShoot shoot = sweptBackShoot(0.6f, 150, 0.8f, 0.06f);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 80;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    ASSERT_GT(r.guides.size(), 40u);
    EXPECT_FALSE(r.look.curly) << r.look.curlShare << " " << r.look.curlAmplitudeDeg;
    double truthLength = 0.0;
    for (const auto& axis : shoot.lockAxes) truthLength += arcLength(axis);
    truthLength /= static_cast<double>(shoot.lockAxes.size());
    double grown = 0.0, near = 0.0;
    std::vector<ReferenceGuide> smooth = r.guides;
    for (std::size_t i = 0; i < r.guides.size(); ++i) {
        const ReferenceGuide& g = r.guides[i];
        grown += arcLength(g.points);
        double best = 1e9;
        for (const auto& axis : shoot.lockAxes) {
            double sum = 0.0;
            for (const Vec3f& p : g.points) {
                float d = 1e9f;
                for (const Vec3f& q : axis) d = std::min(d, (p - q).length());
                sum += d;
            }
            best = std::min(best, sum / static_cast<double>(g.points.size()));
        }
        near += best;
        // (the same guide, its waves smoothed away)
        std::vector<Vec3f> pts = resampleByArcLength(g.points.data(), g.points.size(), 64);
        for (int it = 0; it < 200; ++it) {
            std::vector<Vec3f> next = pts;
            for (std::size_t k = 1; k + 1 < pts.size(); ++k) next[k] = pts[k] * 0.5f + (pts[k - 1] + pts[k + 1]) * 0.25f;
            pts.swap(next);
        }
        smooth[i].points = pts;
    }
    grown /= static_cast<double>(r.guides.size());
    near /= static_cast<double>(r.guides.size());
    // (as long as the locks, within a fifth: the hair hanging over the neck and down the back is
    // hair, and a photo whose hair runs past its crop is worked on wider)
    EXPECT_GT(grown, 0.8 * truthLength);
    EXPECT_LT(grown, 1.2 * truthLength);
    EXPECT_LT(near, 0.2);
    double following = 0.0, smoothed = 0.0, iou = 0.0;
    for (const ReferenceView& v : shoot.views) {
        const PhotoComparison c = compareToPhoto(shoot.cap, lockStrands(r.guides), v, options);
        following += c.flowAgreement;
        iou += c.iou;
        smoothed += compareToPhoto(shoot.cap, lockStrands(smooth), v, options).flowAgreement;
    }
    EXPECT_GE(following, smoothed);
    EXPECT_GT(iou / 4.0, 0.6);
}

// Long hair in deep waves (each lock swinging side to side by about its own width): its crests run
// across the locks for a moment, so the fine directions read sideways bands down the back. With the
// locks' paths (lockPaths: traced along the coarse flow from their roots, holding their way through
// the crests, matched across the views) the guides fall down the back where the hair hangs, clearly
// more than without, about as long as the locks.
TEST(ReferenceGuidesEndToEnd, DeepWavesFallThroughTheirCrests) {
    const SyntheticShoot shoot = sweptBackShoot(0.6f, 150, 0.45f, 0.1f);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 80;
    double truthLength = 0.0;
    for (const auto& axis : shoot.lockAxes) truthLength += arcLength(axis);
    truthLength /= static_cast<double>(shoot.lockAxes.size());
    // (how long the guides are, and below the ball's middle how much of their way past the curls falls)
    auto measure = [](const ReferenceGuidesResult& r, double& length, double& fall) {
        double down = 0.0, along = 0.0;
        length = 0.0;
        for (const ReferenceGuide& g : r.guides) {
            length += arcLength(g.points);
            std::vector<Vec3f> pts = resampleByArcLength(g.points.data(), g.points.size(), 64);
            for (int it = 0; it < 100; ++it) {
                std::vector<Vec3f> next = pts;
                for (std::size_t k = 1; k + 1 < pts.size(); ++k) next[k] = pts[k] * 0.5f + (pts[k - 1] + pts[k + 1]) * 0.25f;
                pts.swap(next);
            }
            for (std::size_t i = 1; i < pts.size(); ++i) {
                if (pts[i].y > -0.2f) continue;
                const Vec3f d = pts[i] - pts[i - 1];
                down += -d.y;
                along += d.length();
            }
        }
        length /= static_cast<double>(std::max<std::size_t>(1, r.guides.size()));
        fall = along > 0.0 ? down / along : 0.0;
    };
    options.lockPaths = 1;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    options.lockPaths = 0;
    const ReferenceGuidesResult plain = guidesFromReference(shoot.cap, shoot.views, options);
    ASSERT_GT(r.guides.size(), 40u);
    ASSERT_EQ(r.lockViews.size(), shoot.views.size());
    int detected = 0, matched = 0, triangulated = 0;
    for (const auto& c : r.lockViews) {
        detected += c.detected;
        matched += c.matched;
        triangulated += c.triangulated;
        EXPECT_EQ(c.triangulated + c.singleView, c.detected);
    }
    EXPECT_GT(detected, 20);
    EXPECT_GT(matched, detected / 2);
    EXPECT_GT(triangulated, 5);
    EXPECT_GT(r.lockGuides, 5);
    EXPECT_TRUE(plain.lockViews.empty());
    double length = 0.0, fall = 0.0, plainLength = 0.0, plainFall = 0.0;
    measure(r, length, fall);
    measure(plain, plainLength, plainFall);
    EXPECT_GT(length, 0.8 * truthLength);
    EXPECT_LT(length, 1.2 * truthLength);
    EXPECT_GT(fall, 0.7);
    EXPECT_GT(fall, plainFall + 0.1);
}

// The same deep waves from three photos (front, side, back), the traced locks lifted first and
// merged in 3D (LockJoin::Merge): every line a piece, lifted and fitted; pieces joined; chains
// rooted on the scalp, scored, and some taking roots' places. The guides still fall down the back,
// rooted, about as long as the locks; each step's counts add up.
TEST(ReferenceGuidesEndToEnd, TracedLocksMergedIn3D) {
    SyntheticShoot shoot = sweptBackShoot(0.6f, 150, 0.45f, 0.1f);
    shoot.views.resize(3);
    ReferenceGuideOptions options;
    options.symmetry = ReferenceSymmetry::None;
    options.occluders.push_back(shoot.head);
    options.guideCount = 80;
    options.lockPaths = 1;
    options.lockJoin = LockJoin::Merge;
    const ReferenceGuidesResult r = guidesFromReference(shoot.cap, shoot.views, options);
    const auto& m = r.lockMerge;
    ASSERT_TRUE(m.on);
    ASSERT_EQ(r.lockViews.size(), 3u);
    int detected = 0;
    for (const auto& c : r.lockViews) {
        EXPECT_GT(c.detected, 0);
        detected += c.detected;
    }
    EXPECT_EQ(detected, m.pieces);
    EXPECT_GT(m.pieces, 20);
    EXPECT_GT(m.lifted, m.pieces / 2);
    EXPECT_LE(m.fitted, m.lifted);
    EXPECT_EQ(m.chains + m.merges, m.fitted - m.tooShort - m.duplicates);
    EXPECT_EQ(m.kept + m.gatedOut + m.rootDropped, m.chains);
    EXPECT_FALSE(m.mergesPerPass.empty());
    EXPECT_GT(m.kept, 5);
    EXPECT_GE(r.lockGuides, 3);
    EXPECT_GT(m.meanScore, options.lockMerge.minScore);
    // (every guide on the scalp, root first: the cap is the ball's top, radius 1)
    double truthLength = 0.0;
    for (const auto& axis : shoot.lockAxes) truthLength += arcLength(axis);
    truthLength /= static_cast<double>(shoot.lockAxes.size());
    double length = 0.0;
    for (const ReferenceGuide& g : r.guides) {
        ASSERT_GE(g.points.size(), 2u);
        EXPECT_LT(std::abs(g.points.front().length() - 1.0f), 0.05f);
        length += arcLength(g.points);
    }
    length /= static_cast<double>(std::max<std::size_t>(1, r.guides.size()));
    EXPECT_GT(length, 0.7 * truthLength);
    EXPECT_LT(length, 1.3 * truthLength);
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

// The scorer (PhotoScorer): known locks drawn from three cameras; each true guide scores above the
// same guide swung sideways, moved off its lock, or run across it; what the head hides from a
// camera behind it is not counted.
TEST(ReferenceGuidesScore, TheTrueGuideScoresAboveItsPerturbations) {
    const ScalpMesh scalp = sphere(1.0f);
    std::vector<std::vector<Vec3f>> truth;
    for (int i = 0; i < 5; ++i) {
        const float phi = (-0.6f + 1.2f * static_cast<float>(i) / 4.0f);
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
    for (const Vec3f& dir : {Vec3f(0, 0.2f, 1), Vec3f(1, 0.2f, 0.4f), Vec3f(-1, 0.2f, 0.4f), Vec3f(0, 0.2f, -1)}) {
        ReferenceView v;
        v.camera = camera(dir, 5.0f, size);
        v.image = blank(size, size, 3, 1.0f);
        v.mask = blank(size, size, 1, 0.0f);
        const Vec3f eye = v.camera.eye();
        for (const auto& g : truth)
            for (int s = -2; s <= 2; ++s) {
                const float shift = 0.03f * static_cast<float>(s);
                for (std::size_t k = 0; k + 1 < g.size(); ++k)
                    for (int t = 0; t < 8; ++t) {
                        const Vec3f p = g[k] + (g[k + 1] - g[k]) * (static_cast<float>(t) / 8.0f) + Vec3f(shift * std::cos(0.0f), 0, 0);
                        // (hidden behind the ball: not drawn)
                        const Vec3f d = (p - eye).normalized();
                        const float b = (eye).dot(d), c = eye.dot(eye) - 1.0f, disc = b * b - c;
                        if (disc > 0.0f && -b - std::sqrt(disc) < (p - eye).length() - 1e-3f) continue;
                        float u = 0, vv = 0;
                        if (!v.camera.project(p, u, vv)) continue;
                        const int x = static_cast<int>(u), y = static_cast<int>(vv);
                        if (x < 0 || y < 0 || x >= size || y >= size) continue;
                        for (int ch = 0; ch < 3; ++ch) v.image.pixels[static_cast<std::size_t>((y * size + x) * 3 + ch)] = 0.15f;
                        for (int j = -2; j <= 2; ++j)
                            for (int i = -2; i <= 2; ++i)
                                if (x + i >= 0 && y + j >= 0 && x + i < size && y + j < size)
                                    v.mask.pixels[static_cast<std::size_t>((y + j) * size + x + i)] = 1.0f;
                    }
            }
        views.push_back(v);
    }
    const PhotoScorer scorer(scalp, views);
    ASSERT_EQ(scorer.viewCount(), 4u);
    for (std::size_t i = 0; i < truth.size(); ++i) {
        const std::vector<Vec3f>& g = truth[i];
        std::vector<Vec3f> swung = g, moved = g, across;
        for (std::size_t k = 0; k < g.size(); ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(g.size() - 1);
            swung[k] = g[k] + Vec3f(0.6f * t * t, 0, 0);
            moved[k] = g[k] + Vec3f(0.12f, -0.05f, 0.05f);
            across.push_back(g.front() + Vec3f(0.8f * t, 0.05f * t, 0.0f));
        }
        const GuideScore right = scorer.score(g), a = scorer.score(swung), b = scorer.score(moved), c = scorer.score(across);
        EXPECT_GT(right.score, 0.5f) << i;
        EXPECT_GT(right.score, a.score + 0.05f) << i;
        EXPECT_GT(right.score, b.score + 0.05f) << i;
        EXPECT_GT(right.score, c.score + 0.05f) << i;
        EXPECT_LT(right.photos[0].coarseError, 15.0f) << i;
        // The camera behind the ball sees none of it: its points are hidden, not counted.
        EXPECT_EQ(right.photos[3].samples, 0) << i;
        EXPECT_GT(right.photos[3].hidden, 0) << i;
        EXPECT_EQ(right.views, 3) << i;
    }
    // In parallel: the same.
    const std::vector<GuideScore> all = scorer.score(truth);
    ASSERT_EQ(all.size(), truth.size());
    EXPECT_NEAR(all[2].score, scorer.score(truth[2]).score, 1e-6f);
}

}  // namespace
}  // namespace groomlab
