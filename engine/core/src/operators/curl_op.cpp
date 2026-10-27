// SPDX-License-Identifier: Apache-2.0
// Curl: wraps strands into curls around their current shape.
//
// The curl is a curve of its own, not an offset of each point: around an axis, a smooth curve of
// kAxisSamples samples (the strand's own points at their shares of the strand, or with coherence
// clump the mean of the clump's core strands there, preview_core.h), an offset of radius r (the
// radius param, varied, times the profile) turning at the frequency in the axis's
// rotation-minimizing frames, by the axis's arc length, eased in from 0 at the root. Each point of
// the strand (its own, and any Curl adds) takes the offset at its share of the strand, so the
// same strand at 40 segments or at 256 makes one curl, and more points only sample it more
// finely. Where the points coming in are too few a turn for the tolerance (a coil over a strand of
// 40 segments), Curl first cuts each of the strand's segments into as many as its longest needs at
// the most the curl turns there (its frequency, randomness, frequencyProfile and spacingRandom at
// their most; a zigzag enough to keep its kinks' peaks), up to maxSegments, on the strand taken as a smooth curve
// (each point keeping its share of the strand), then curls those points as its own: the curl
// itself is CurlCS's on the CPU engine (the GPU preview's), the C++ below with the engine off.
//
// A polyline as a smooth curve: each segment a cubic Hermite curve between its points, the slope
// at a point the derivative there of the parabola through it and its neighbours (spaced by their
// distances), so a polyline sampling a smooth curve at any spacing gives that curve back to O(h³).
// The axis's arc length is that curve's (two-point Gauss per segment), its frames carried from the
// root along its samples by double reflection (Wang et al. 2008) with the curve's own tangents,
// seeded by the scalp's normal across the tangent at the root; between samples, by one double
// reflection from the sample before.
//
// The curl turns at a local frequency, the frequency times frequencyProfile (by the share of the
// strand) times a smooth noise of the strand's (spacingRandom): its angle is that integrated
// along the axis's arc (curl_phase.h), through the axis's "profiled arc" (the arc length weighted
// by frequencyProfile: two-point Gauss on each piece of a segment between the ramp's keys, as the
// arc length itself), and the noise's integral in closed form. With the defaults (frequencyProfile
// 1 everywhere, spacingRandom 0) the profiled arc is the arc and the angle 2 pi f s + phase.
//
// Weighted curls (weight): each part of a curl carries the hair hanging below it, so its turns
// spread apart. The load at a point is T = hang * turnsBelow: hang = max(0, the axis's tangent ·
// weightDirection), smoothed a little (two passes of 1-2-1 over the axis's samples), and
// turnsBelow the curl's turns from there to the tip (the integral of the local frequency as
// above: counted in turns, so scale-invariant, and long ringlets carry more than short curls).
// The local pitch stretches to p (1 + weight T / kWeightTurns): the local frequency is divided by
// that stretch, which joins the profiled arc's weight (the stretch at the axis's samples, linear
// between them), so the angle is still that one integral. The radius keeps a turn's wire length
// as a stretched spring does: (2 pi r)² + p² is kept, so r'² = r² u with u = 1 - (q² - 1) / (2 pi f
// r)² for a stretch q at local frequency f, r the curl's own radius (the radius param, varied, before
// the profile and the ease: those then scale r'); an overstretched curl (u 0 or less) goes straight.
// r' = r sqrt(u) down to u = 1/4, then eased to 0 at u = 0 by the cubic matching it there (value
// and slope), so the strand bends smoothly where its curl opens out (sqrt's slope is endless at
// 0). Weight 0 leaves all of it out (the curl as before).
//
// look.slang (CurlCS, CurlAxesCS) repeats this arithmetic in its order for the GPU preview, for
// every strand (weighted: its own axis walked to the tip first for the stretch), at its own points
// and any Curl added: on the CPU engine (look_cpu.h runStage, runStageOnStrands), the C++ here with
// the engine off.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/curl_phase.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_CURL_SSE2 1
#include <emmintrin.h>
#else
#define GROOMLAB_CURL_SSE2 0
#endif

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kPi = 3.14159265359f;
constexpr std::size_t kAxisSamples = 32;  // a curl's axis (look.slang CENTER_SAMPLES)
constexpr std::uint32_t kAlone = 0xffffffffu;
constexpr float kGauss0 = 0.211324865f, kGauss1 = 0.788675135f;  // two-point Gauss on [0, 1]
// Weighted curls: the turns hanging straight below a point that, at weight 1, double its pitch.
constexpr float kWeightTurns = 10.0f;

// ---- A polyline as a smooth curve (look.slang secantOf, slopeFrom, hermiteAt, hermiteDerivative,
// segmentArc)

// v over its length (by its reciprocal: one division), or zero.
Vec3f unit(const Vec3f& v) { return normalizedRecip(v); }

// The unit direction from a to b, its length into h (zero: none).
Vec3f secantOf(const Vec3f& a, const Vec3f& b, float* h) {
    const Vec3f d = b - a;
    *h = d.length();
    return *h > 0.0f ? d * (1.0f / *h) : Vec3f{};
}

// The slope (d point / d arc) of the parabola through three points spaced h0 and h1 apart, the
// unit directions between them s0 and s1: at the first (`which` 0, a curve's root), the middle
// (1) or the last (2, its tip).
Vec3f slopeFrom(const Vec3f& s0, float h0, const Vec3f& s1, float h1, int which) {
    const float w = h0 + h1;
    if (which == 1) return w > 0.0f ? (s0 * h1 + s1 * h0) * (1.0f / w) : Vec3f{};
    if (which == 0) return w > 0.0f ? s0 + (s0 - s1) * (h0 / w) : s0;
    return w > 0.0f ? s1 + (s1 - s0) * (h1 / w) : s1;
}
// Which three points (from `first`) and which of them the slope at point i of n is taken at.
void slopeSpan(std::size_t i, std::size_t n, std::size_t* first, int* which) {
    *first = i == 0 ? 0 : i + 1 == n ? n - 3 : i - 1;
    *which = i == 0 ? 0 : i + 1 == n ? 2 : 1;
}

// The segment from p0 (slope d0) to p1 (slope d1), h long, at u (0 to 1): its point, and its
// derivative in u.
Vec3f hermiteAt(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    const float u2 = u * u;
    return p0 + dd * (u2 * (3.0f - 2.0f * u)) + a * (u * (1.0f - u) * (1.0f - u)) + b * (u2 * (u - 1.0f));
}
Vec3f hermiteDerivative(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    return dd * (6.0f * u * (1.0f - u)) + a * ((1.0f - u) * (1.0f - 3.0f * u)) + b * (u * (3.0f * u - 2.0f));
}
// The segment's arc length from 0 to u (two-point Gauss: hermiteDerivative at u g0 and u g1).
float segmentArc(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    auto speed = [&](float v) {
        return (dd * (6.0f * v * (1.0f - v)) + a * ((1.0f - v) * (1.0f - 3.0f * v)) + b * (v * (3.0f * v - 2.0f))).length();
    };
    return 0.5f * u * (speed(u * kGauss0) + speed(u * kGauss1));
}

// The segment's profiled arc from 0 to u: the integral of its speed times `profile` (0 or more)
// at the share of the axis there (segment k of an axis of `samples` samples), two-point Gauss on
// each piece between the ramp's keys (smooth between them), as segmentArc (look.slang
// profiledSegmentArc); `tangents` the ramp's (Ramp::at).
// Weighted (`stretch`: the stretch at the segment's ends, linear between them; null: none), the
// speed is divided by the stretch too; without a profile (null) it is 1.
float profiledSegmentArc(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u, std::size_t k,
                         std::size_t samples, const Ramp* profile, const float* tangents, const float* stretch = nullptr) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    const float span = static_cast<float>(samples - 1), start = static_cast<float>(k);
    auto weighted = [&](float v) {
        const float speed = (dd * (6.0f * v * (1.0f - v)) + a * ((1.0f - v) * (1.0f - 3.0f * v)) + b * (v * (3.0f * v - 2.0f))).length();
        const float w = profile ? speed * std::max(0.0f, profile->at((start + v) / span, tangents)) : speed;
        return stretch ? w / (stretch[0] + (stretch[1] - stretch[0]) * v) : w;
    };
    auto piece = [&](float lo, float hi) {
        const float w = hi - lo;
        return 0.5f * w * (weighted(lo + w * kGauss0) + weighted(lo + w * kGauss1));
    };
    float sum = 0.0f, lo = 0.0f;
    if (profile) {
        for (const auto& key : profile->keys) {
            const float at = key.first * span - start;
            if (at > lo && at < u) {
                sum += piece(lo, at);
                lo = at;
            }
        }
    }
    return sum + piece(lo, u);
}

// The share of its radius r a curl keeps where its pitch (1 / |f|, f the local frequency)
// stretches q-fold (q 1 or more), keeping a turn's wire length sqrt((2 pi r)² + p²): sqrt(u) of
// the share u of r² left, eased to 0 below u = 1/4 (C¹: 1.25 x² - 0.75 x³ of x = 4u); 0 where it
// would go past straight (and where f is 0, the limit: an endless pitch stretched).
float stretchedShare(float r, float q, float f) {
    if (!(q > 1.0f) || r == 0.0f) return 1.0f;
    const float turn = kTwoPi * f * r;
    if (turn == 0.0f) return 0.0f;
    const float u = 1.0f - (q * q - 1.0f) / (turn * turn);
    if (u >= 0.25f) return std::sqrt(u);
    if (!(u > 0.0f)) return 0.0f;
    const float x = 4.0f * u;
    return x * x * (1.25f - 0.75f * x);
}

// A unit tangent along slope d, or `previous` where d is zero.
Vec3f tangentOf(const Vec3f& d, const Vec3f& previous) {
    const float l = d.length();
    return l > 0.0f ? d * (1.0f / l) : previous;
}

// The frame a curl starts from (look.slang curlFirstFrame), at the root where the tangent is
// `tangent`: the scalp's normal (zero: none) across the way the strand leaves (`leave`, unit: to
// its point a 31st of the way along its smooth curve, the same at every resolution), squared to
// the tangent; else (the normal along it) any perpendicular of the tangent. Ill-conditioned where
// the strand leaves close along the normal (a rounding turns it by that rounding over how far
// across the normal reaches; over a 31st of the strand, as the first segment before, rather than
// at the root itself, where a Clump's pull turns strands closer still). No rule starts every strand
// well (a blend toward a fixed perpendicular moves that direction elsewhere, and regular grooms'
// strands find it), so the GPU rounds these as the CPU does.
Frame firstFrame(const Vec3f& normal, const Vec3f& tangent, const Vec3f& leave) {
    Frame f;
    f.tangent = tangent;
    const Vec3f across = normal - leave * normal.dot(leave);
    bool seeded = false;
    if (across.dot(across) > 1e-8f) {
        const Vec3f seed = unit(across);
        const Vec3f square = seed - tangent * seed.dot(tangent);
        if (square.dot(square) > 1e-8f) {
            f.normal = unit(square);
            seeded = true;
        }
    }
    if (!seeded) f.normal = anyPerpendicular(tangent);
    f.binormal = tangent.cross(f.normal);
    return f;
}

// The segment's point at u less its start (hermiteAt's terms without p0: no cancellation for a
// small u, where the frame is carried a short way).
Vec3f hermiteStep(const Vec3f& p0, const Vec3f& p1, const Vec3f& d0, const Vec3f& d1, float h, float u) {
    const Vec3f dd = p1 - p0, a = d0 * h, b = d1 * h;
    const float u2 = u * u;
    return dd * (u2 * (3.0f - 2.0f * u)) + a * (u * (1.0f - u) * (1.0f - u)) + b * (u2 * (u - 1.0f));
}

// Frame f carried along v1, to where the tangent is t1 (double reflection; look.slang
// reflectFrame). A repeated point keeps the frame.
void reflectFrameBy(Frame& f, const Vec3f& v1, const Vec3f& t1) {
    const float c1 = v1.dot(v1);
    if (c1 < 1e-12f) return;
    const float k1 = 2.0f / c1;
    const Vec3f rL = f.normal - v1 * (k1 * v1.dot(f.normal));
    const Vec3f tL = f.tangent - v1 * (k1 * v1.dot(f.tangent));
    const Vec3f v2 = t1 - tL;
    const float c2 = v2.dot(v2);
    const Vec3f r = c2 < 1e-12f ? rL : rL - v2 * (2.0f / c2 * v2.dot(rL));
    f.tangent = t1;
    f.normal = unit(r - t1 * r.dot(t1));
    if (f.normal.dot(f.normal) == 0.0f) f.normal = anyPerpendicular(t1);
    f.binormal = t1.cross(f.normal);
}
// Frame f at x0 carried to x1.
void reflectFrame(const Vec3f& x0, Frame& f, const Vec3f& x1, const Vec3f& t1) { reflectFrameBy(f, x1 - x0, t1); }

// How much of its radius the curl has `arc` along its axis, at share t of the strand: eased in
// from 0 at the root (smoothstep) over its first quarter turn, or the first quarter of the strand
// where that comes first (look.slang curlEase).
float curlEase(float arc, float t, float frequency) {
    const float x = std::min(1.0f, std::max(4.0f * std::fabs(frequency) * arc, 4.0f * t));
    return x * x * (3.0f - 2.0f * x);
}

// A triangle wave with sin's period, peaks and zeros: straight runs between sharp kinks (look.slang
// zigzagWave). Its kinks lie at pi/2 + k pi.
float zigzag(float angle) {
    const float u = angle * (1.0f / kTwoPi) + 0.25f;
    return 1.0f - 4.0f * std::fabs(u - std::floor(u) - 0.5f);
}

// A strand's points as a smooth curve: its segments' lengths and directions and the slope at
// each point.
struct Curve {
    const Vec3f* p = nullptr;
    std::size_t n = 0;
    std::vector<Vec3f> s, d;
    std::vector<float> h;
    void make(const Vec3f* points, std::size_t count) {
        size(points, count);
        for (std::size_t j = 0; j + 1 < n; ++j) s[j] = secantOf(p[j], p[j + 1], &h[j]);
        for (std::size_t i = 0; i < n; ++i) {
            if (n == 2) {
                d[i] = s[0];
            } else if (n > 2) {
                std::size_t a = 0;
                int which = 0;
                slopeSpan(i, n, &a, &which);
                d[i] = slopeFrom(s[a], h[a], s[a + 1], h[a + 1], which);
            }
        }
    }
    // Its points, and room for the rest (make's, or makeFour's).
    void size(const Vec3f* points, std::size_t count) {
        p = points;
        n = count;
        s.resize(n > 1 ? n - 1 : 0);
        h.resize(s.size());
        d.resize(n);
    }
    // Segment j at u (0 < u < 1).
    Vec3f at(std::size_t j, float u) const { return hermiteAt(p[j], p[j + 1], d[j], d[j + 1], h[j], u); }
    // The point at share t of the points (by index: as each node places a strand's points at
    // every resolution: look.slang curvePointAt).
    Vec3f pointAt(float t) const {
        if (n == 0) return {};
        if (n == 1) return p[0];
        const float x = t * static_cast<float>(n - 1);
        const std::size_t i = x <= 0.0f ? 0 : std::min(static_cast<std::size_t>(x), n - 2);
        const float u = x - static_cast<float>(i);
        return u == 0.0f ? p[i] : at(i, u);
    }
};

// One curl's numbers, a strand's (or a clump's): its start (radians), its radius' and
// frequency's variation, the plane a wave or zigzag lies in (an angle about the strand), and its
// spacing noise's key (curl_phase.h).
struct Draw {
    float phase = 0.0f, vary = 1.0f, frequency = 0.0f, plane = 0.0f;
    std::uint32_t spacingKey = 0;
};

// A curl's axis as a smooth curve: its `samples` samples `x` (at most kAxisSamples), their slopes,
// the segments' lengths, the frames, arc lengths and profiled arcs at the samples (look.slang
// CurlAxesCS writes the same, CurlCS walks them), and the axis's frame, arc and profiled arc at
// each point of a strand of `count` points (its strand's, or its clump's members' common count).
// The profiled arc (curl_phase.h: the arc weighted by frequencyProfile, `profile`) is the arc
// itself without a profile.
struct Axis {
    std::size_t samples = kAxisSamples;
    std::array<Vec3f, kAxisSamples> x{}, d{};
    std::array<float, kAxisSamples> h{}, arc{}, profiledArc{};
    std::array<Frame, kAxisSamples> f{};
    Draw draw;
    const Ramp* profile = nullptr;  // frequencyProfile (null: 1 everywhere)
    const float* tangents = nullptr;  // its keys' (Ramp::at)
    // Weighted curls: Curl's weight over kWeightTurns (0: none), the unit weightDirection, the
    // spacing noise's amount (spacingRandom: the turns below count it), and the stretch at each
    // sample (1 + load T).
    float load = 0.0f, spacingRandom = 0.0f;
    Vec3f down;
    std::array<float, kAxisSamples> stretch{};
    std::size_t count = 0;
    std::vector<Frame> frames;  // at point i of `count`
    std::vector<float> arcs, profiledArcs;
    // Whether the profiled arc is other than the arc (a profile, or weighted).
    bool spaced() const { return profile || load > 0.0f; }
    const float* stretchOf(std::size_t k) const { return load > 0.0f ? &stretch[k] : nullptr; }
    // The stretch at share t of the axis (1: not weighted).
    float stretchAt(float t) const {
        if (!(load > 0.0f)) return 1.0f;
        const float xs = t * static_cast<float>(samples - 1);
        const std::size_t k = std::min(static_cast<std::size_t>(std::max(xs, 0.0f)), samples - 2);
        const float u = xs - static_cast<float>(k);
        return stretch[k] + (stretch[k + 1] - stretch[k]) * u;
    }
    // The arc length at share t of the axis, its frame there and its profiled arc (look.slang
    // clumpAxisAt, axisWalkAt).
    float arcAt(float t, Frame* frame = nullptr, float* profiled = nullptr) const {
        const float xs = t * static_cast<float>(samples - 1);
        const std::size_t k = std::min(static_cast<std::size_t>(std::max(xs, 0.0f)), samples - 2);
        const float u = xs - static_cast<float>(k);
        if (frame) *frame = f[k];
        if (!(u > 0.0f)) {
            if (profiled) *profiled = spaced() ? profiledArc[k] : arc[k];
            return arc[k];
        }
        if (frame) {
            const Vec3f t1 = tangentOf(hermiteDerivative(x[k], x[k + 1], d[k], d[k + 1], h[k], u), f[k].tangent);
            reflectFrameBy(*frame, hermiteStep(x[k], x[k + 1], d[k], d[k + 1], h[k], u), t1);
        }
        const float a = arc[k] + segmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], u);
        if (profiled) {
            *profiled = spaced() ? profiledArc[k] + profiledSegmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], u, k, samples, profile, tangents, stretchOf(k)) : a;
        }
        return a;
    }
    // At point i of n: the table's, or made.
    float arcAt(std::size_t i, std::size_t n, Frame* frame = nullptr, float* profiled = nullptr) const {
        if (n == count) {
            const bool own = n == samples;  // the strand's own points: its samples'
            if (frame) *frame = own ? f[i] : frames[i];
            const float a = own ? arc[i] : arcs[i];
            if (profiled) *profiled = spaced() ? (own ? profiledArc[i] : profiledArcs[i]) : a;
            return a;
        }
        return arcAt(paramAt(i, n), frame, profiled);
    }
    // The share t at which its arc is `target` (0 to its length): from the share of its segment's
    // length, one Newton step on.
    float shareAtArc(float target) const {
        std::size_t k = 0;
        while (k + 2 < samples && arc[k + 1] < target) ++k;
        const float span = arc[k + 1] - arc[k];
        float u = span > 0.0f ? std::clamp((target - arc[k]) / span, 0.0f, 1.0f) : 0.0f;
        const float speed = hermiteDerivative(x[k], x[k + 1], d[k], d[k + 1], h[k], u).length();
        if (speed > 0.0f) {
            u = std::clamp(u - (arc[k] + segmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], u) - target) / speed, 0.0f, 1.0f);
        }
        return (static_cast<float>(k) + u) / static_cast<float>(samples - 1);
    }
    // From its samples `x`: the rest, its frames starting from `normal` across the way to
    // `leaveTo` (its point a 31st along: firstFrame), the table for `n` points (a strand's own
    // points as its samples: theirs). `same`: a curve through these very samples (its slopes and
    // lengths are theirs).
    void make(const Vec3f& normal, std::size_t n, const Vec3f& leaveTo, const Curve* same = nullptr) {
        shape(same);
        start(normal, leaveTo);
        carry();
        finish(n);
    }
    // make's steps (makeFour and carryFour take some for four axes at once): the slopes and
    // lengths, arcs and profiled arcs; the first frame; the frames carried along the samples;
    // weighted curls and the table.
    void shape(const Curve* same) {
        slopes(same);
        arcLengths();
        profiled();
    }
    void arcLengths() {
        arc[0] = 0.0f;
        for (std::size_t k = 0; k + 1 < samples; ++k) arc[k + 1] = arc[k] + segmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], 1.0f);
    }
    void slopes(const Curve* same) {
        if (same) {
            std::copy_n(same->h.data(), samples - 1, h.data());
            std::copy_n(same->d.data(), samples, d.data());
        } else {
            std::array<Vec3f, kAxisSamples - 1> s;
            for (std::size_t k = 0; k + 1 < samples; ++k) s[k] = secantOf(x[k], x[k + 1], &h[k]);
            for (std::size_t k = 0; k < samples; ++k) {
                if (samples == 2) {
                    d[k] = s[0];
                    continue;
                }
                std::size_t a = 0;
                int which = 0;
                slopeSpan(k, samples, &a, &which);
                d[k] = slopeFrom(s[a], h[a], s[a + 1], h[a + 1], which);
            }
        }
    }
    void profiled() {
        if (profile) {
            profiledArc[0] = 0.0f;
            for (std::size_t k = 0; k + 1 < samples; ++k) {
                profiledArc[k + 1] = profiledArc[k] + profiledSegmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], 1.0f, k, samples, profile, tangents);
            }
        }
    }
    void start(const Vec3f& normal, const Vec3f& leaveTo) {
        const Vec3f t0 = tangentOf(d[0], Vec3f{0, 0, 1});
        f[0] = firstFrame(normal, t0, tangentOf(leaveTo - x[0], t0));
    }
    void carry() {
        for (std::size_t k = 1; k < samples; ++k) {
            f[k] = f[k - 1];
            reflectFrame(x[k - 1], f[k], x[k], tangentOf(d[k], f[k - 1].tangent));
        }
    }
    // `table` false: none for n other than the samples (arcAt then makes each point's arcs, the
    // same, without its frame: the curls' counts).
    void finish(std::size_t n, bool table = true) {
        if (load > 0.0f) weigh();
        count = n == samples || table ? n : 0;
        if (count != n || n == samples) return;  // (arcAt takes them from the samples)
        frames.resize(n);
        arcs.resize(n);
        profiledArcs.resize(spaced() ? n : 0);
        for (std::size_t i = 0; i < n; ++i) arcs[i] = arcAt(paramAt(i, n), &frames[i], spaced() ? &profiledArcs[i] : nullptr);
    }
    // Weighted curls (make's, with its draw set): the stretch at each sample from the load there
    // (how much the axis hangs along `down`, smoothed, times the turns below it, unweighted), then
    // the profiled arc weighted by it.
    void weigh() {
        const std::size_t last = samples - 1;
        std::array<float, kAxisSamples> hang{}, before{};
        for (std::size_t k = 0; k < samples; ++k) hang[k] = std::max(0.0f, f[k].tangent.dot(down));
        for (int pass = 0; pass < 2; ++pass) {
            before = hang;
            for (std::size_t k = 0; k < samples; ++k) {
                hang[k] = 0.25f * (before[k == 0 ? 0 : k - 1] + 2.0f * before[k] + before[std::min(k + 1, last)]);
            }
        }
        auto turnsAt = [&](std::size_t k) {
            return curlTurns(draw.frequency, profile ? profiledArc[k] : arc[k], spacingRandom, draw.spacingKey);
        };
        const float total = turnsAt(last);
        for (std::size_t k = 0; k < samples; ++k) stretch[k] = 1.0f + load * hang[k] * std::fabs(total - turnsAt(k));
        profiledArc[0] = 0.0f;
        for (std::size_t k = 0; k < last; ++k) {
            profiledArc[k + 1] = profiledArc[k] + profiledSegmentArc(x[k], x[k + 1], d[k], d[k + 1], h[k], 1.0f, k, samples, profile, tangents, &stretch[k]);
        }
    }
};

#if GROOMLAB_CURL_SSE2
// Four lanes of Vec3f, each operation the scalar one per lane (in Vec3f's order), so each lane
// gets the scalar code's floats bit for bit (frames.cpp's).
struct V4 {
    __m128 x, y, z;
};
inline V4 operator+(const V4& a, const V4& b) { return {_mm_add_ps(a.x, b.x), _mm_add_ps(a.y, b.y), _mm_add_ps(a.z, b.z)}; }
inline V4 operator-(const V4& a, const V4& b) { return {_mm_sub_ps(a.x, b.x), _mm_sub_ps(a.y, b.y), _mm_sub_ps(a.z, b.z)}; }
inline V4 operator*(const V4& a, __m128 s) { return {_mm_mul_ps(a.x, s), _mm_mul_ps(a.y, s), _mm_mul_ps(a.z, s)}; }
inline __m128 dot(const V4& a, const V4& b) {
    return _mm_add_ps(_mm_add_ps(_mm_mul_ps(a.x, b.x), _mm_mul_ps(a.y, b.y)), _mm_mul_ps(a.z, b.z));
}
inline V4 cross(const V4& a, const V4& b) {
    return {_mm_sub_ps(_mm_mul_ps(a.y, b.z), _mm_mul_ps(a.z, b.y)), _mm_sub_ps(_mm_mul_ps(a.z, b.x), _mm_mul_ps(a.x, b.z)),
            _mm_sub_ps(_mm_mul_ps(a.x, b.y), _mm_mul_ps(a.y, b.x))};
}
inline __m128 pick(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); }
inline V4 pick(__m128 mask, const V4& a, const V4& b) { return {pick(mask, a.x, b.x), pick(mask, a.y, b.y), pick(mask, a.z, b.z)}; }
// unit (and tangentOf): v times the reciprocal of its length where that is above 0, else `other`.
inline V4 unitOr(const V4& v, const V4& other) {
    const __m128 l = _mm_sqrt_ps(dot(v, v));
    return pick(_mm_cmpgt_ps(l, _mm_setzero_ps()), v * _mm_div_ps(_mm_set1_ps(1.0f), l), other);
}
inline V4 lanes(const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& d) {
    return {_mm_setr_ps(a.x, b.x, c.x, d.x), _mm_setr_ps(a.y, b.y, c.y, d.y), _mm_setr_ps(a.z, b.z, c.z, d.z)};
}
inline void unpack(const V4& v, Vec3f out[4]) {
    alignas(16) float x[4], y[4], z[4];
    _mm_store_ps(x, v.x);
    _mm_store_ps(y, v.y);
    _mm_store_ps(z, v.z);
    for (int k = 0; k < 4; ++k) out[k] = {x[k], y[k], z[k]};
}
// anyPerpendicular (frames.cpp): v across the axis least along it, normalized.
inline V4 anyPerpendicularFour(const V4& v) {
    const __m128 sign = _mm_set1_ps(-0.0f), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 ax = _mm_andnot_ps(sign, v.x), ay = _mm_andnot_ps(sign, v.y), az = _mm_andnot_ps(sign, v.z);
    const __m128 onX = _mm_and_ps(_mm_cmple_ps(ax, ay), _mm_cmple_ps(ax, az));
    const __m128 onY = _mm_andnot_ps(onX, _mm_cmple_ps(ay, az));
    const V4 c = cross(v, V4{pick(onX, one, zero), pick(onY, one, zero), pick(_mm_or_ps(onX, onY), zero, one)});
    const __m128 len = _mm_sqrt_ps(dot(c, c));
    return pick(_mm_cmpgt_ps(len, zero), V4{_mm_div_ps(c.x, len), _mm_div_ps(c.y, len), _mm_div_ps(c.z, len)}, V4{zero, zero, zero});
}
// firstFrame: the normal `n` and binormal `b` of the frame of tangent t.
inline void firstFrameFour(const V4& normal, const V4& t, const V4& leave, V4& n, V4& b) {
    const __m128 zero = _mm_setzero_ps(), small = _mm_set1_ps(1e-8f);
    const V4 none{zero, zero, zero};
    const V4 across = normal - leave * dot(normal, leave);
    const V4 seed = unitOr(across, none);
    const V4 square = seed - t * dot(seed, t);
    const __m128 seeded = _mm_and_ps(_mm_cmpgt_ps(dot(across, across), small), _mm_cmpgt_ps(dot(square, square), small));
    n = unitOr(square, none);
    if (_mm_movemask_ps(seeded) != 0xF) n = pick(seeded, n, anyPerpendicularFour(t));
    b = cross(t, n);
}
// reflectFrameBy: the frame (t, n, b) carried along v1, to where the tangent is t1.
inline void reflectFour(V4& t, V4& n, V4& b, const V4& v1, const V4& t1) {
    const __m128 eps = _mm_set1_ps(1e-12f), two = _mm_set1_ps(2.0f), zero = _mm_setzero_ps();
    const __m128 c1 = dot(v1, v1);
    const __m128 keep = _mm_cmplt_ps(c1, eps);  // a repeated point keeps the frame
    const __m128 k1 = _mm_div_ps(two, c1);
    const V4 rL = n - v1 * _mm_mul_ps(k1, dot(v1, n));
    const V4 tL = t - v1 * _mm_mul_ps(k1, dot(v1, t));
    const V4 v2 = t1 - tL;
    const __m128 c2 = dot(v2, v2);
    const V4 r = pick(_mm_cmplt_ps(c2, eps), rL, rL - v2 * _mm_mul_ps(_mm_div_ps(two, c2), dot(v2, rL)));
    V4 next = unitOr(r - t1 * dot(r, t1), V4{zero, zero, zero});
    const __m128 none = _mm_cmpeq_ps(dot(next, next), zero);
    if (_mm_movemask_ps(none)) next = pick(none, anyPerpendicularFour(t1), next);
    const V4 across = cross(t1, next);
    t = pick(keep, t, t1);
    n = pick(keep, n, next);
    b = pick(keep, b, across);
}
inline void storeFrames(Axis* const a[4], std::size_t k, const V4& t, const V4& n, const V4& b) {
    Vec3f tt[4], nn[4], bb[4];
    unpack(t, tt);
    unpack(n, nn);
    unpack(b, bb);
    for (int l = 0; l < 4; ++l) a[l]->f[k] = Frame{tt[l], nn[l], bb[l]};
}
// segmentArc over a whole segment (u = 1): hermiteDerivative's weights at the two Gauss points,
// as segmentArc takes them.
struct WholeArc {
    float w0[2], w1[2], w2[2], half;
    WholeArc() {
        const float u = 1.0f;
        for (int g = 0; g < 2; ++g) {
            const float v = u * (g == 0 ? kGauss0 : kGauss1);
            w0[g] = 6.0f * v * (1.0f - v);
            w1[g] = (1.0f - v) * (1.0f - 3.0f * v);
            w2[g] = v * (3.0f * v - 2.0f);
        }
        half = 0.5f * u;
    }
    __m128 operator()(const V4& p0, const V4& p1, const V4& d0, const V4& d1, __m128 h) const {
        const V4 dd = p1 - p0, a = d0 * h, b = d1 * h;
        __m128 speed[2];
        for (int g = 0; g < 2; ++g) {
            const V4 v = dd * _mm_set1_ps(w0[g]) + a * _mm_set1_ps(w1[g]) + b * _mm_set1_ps(w2[g]);
            speed[g] = _mm_sqrt_ps(dot(v, v));
        }
        return _mm_mul_ps(_mm_set1_ps(half), _mm_add_ps(speed[0], speed[1]));
    }
};
// hermiteDerivative's weights at u, (dd, a, b)'s.
inline V4 derivativeAt(const V4& dd, const V4& a, const V4& b, __m128 u) {
    const __m128 one = _mm_set1_ps(1.0f), three = _mm_set1_ps(3.0f), back = _mm_sub_ps(one, u);
    return dd * _mm_mul_ps(_mm_mul_ps(_mm_set1_ps(6.0f), u), back) +
           a * _mm_mul_ps(back, _mm_sub_ps(one, _mm_mul_ps(three, u))) +
           b * _mm_mul_ps(u, _mm_sub_ps(_mm_mul_ps(three, u), _mm_set1_ps(2.0f)));
}
#endif

// Axis::carry for four axes of as many samples, their frames side by side (SIMD lanes where the
// CPU has them: the same operations in the same order, the branches as per-lane picks). A frame
// carried along a sample is a chain of dependent steps; four at once keep the CPU busy while
// each waits on the last.
void carryFour(Axis* const a[4]) {
#if GROOMLAB_CURL_SSE2
    auto at = [&](const auto& member, std::size_t k) {
        return lanes((a[0]->*member)[k], (a[1]->*member)[k], (a[2]->*member)[k], (a[3]->*member)[k]);
    };
    V4 t = lanes(a[0]->f[0].tangent, a[1]->f[0].tangent, a[2]->f[0].tangent, a[3]->f[0].tangent);
    V4 n = lanes(a[0]->f[0].normal, a[1]->f[0].normal, a[2]->f[0].normal, a[3]->f[0].normal);
    V4 b = lanes(a[0]->f[0].binormal, a[1]->f[0].binormal, a[2]->f[0].binormal, a[3]->f[0].binormal);
    V4 x0 = at(&Axis::x, 0);
    for (std::size_t k = 1; k < a[0]->samples; ++k) {
        const V4 x1 = at(&Axis::x, k);
        reflectFour(t, n, b, x1 - x0, unitOr(at(&Axis::d, k), t));  // (tangentOf(d[k], f[k - 1].tangent))
        storeFrames(a, k, t, n, b);
        x0 = x1;
    }
#else
    for (int l = 0; l < 4; ++l) a[l]->carry();
#endif
}

// Curve::make, and Axis::make up to its profiled arcs and table (profiled, finish), for four
// strands of as many points n (3 to kAxisSamples), each its own axis's samples, side by side as
// carryFour; `normals`: the scalp's at their roots. The arcs and the frames walk the strands
// together (the arcs' work goes on while each frame's step waits on the last). `frames` false:
// all but the frames (the curls' counts need only the arcs).
void makeFour(Curve* const c[4], Axis* const a[4], const Vec3f* const points[4], std::size_t n,
              const Vec3f normals[4], bool frames = true) {
    const float leaveAt = 1.0f / static_cast<float>(kAxisSamples - 1);  // (Axis::start's point a 31st along)
    for (int l = 0; l < 4; ++l) {
        c[l]->size(points[l], n);
        a[l]->samples = n;
        std::copy_n(points[l], n, a[l]->x.data());
    }
#if GROOMLAB_CURL_SSE2
    std::array<V4, kAxisSamples> p, d;
    std::array<V4, kAxisSamples - 1> s;
    std::array<__m128, kAxisSamples - 1> h;
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const V4 none{zero, zero, zero};
    Vec3f out[4];
    alignas(16) float values[4];
    for (std::size_t i = 0; i < n; ++i) p[i] = lanes(points[0][i], points[1][i], points[2][i], points[3][i]);
    for (std::size_t j = 0; j + 1 < n; ++j) {  // secantOf
        const V4 v = p[j + 1] - p[j];
        h[j] = _mm_sqrt_ps(dot(v, v));
        s[j] = pick(_mm_cmpgt_ps(h[j], zero), v * _mm_div_ps(one, h[j]), none);
        unpack(s[j], out);
        _mm_store_ps(values, h[j]);
        for (int l = 0; l < 4; ++l) {
            c[l]->s[j] = out[l];
            c[l]->h[j] = a[l]->h[j] = values[l];
        }
    }
    for (std::size_t i = 0; i < n; ++i) {  // slopeFrom
        std::size_t k = 0;
        int which = 0;
        slopeSpan(i, n, &k, &which);
        const V4 &s0 = s[k], &s1 = s[k + 1];
        const __m128 h0 = h[k], h1 = h[k + 1], w = _mm_add_ps(h0, h1), spaced = _mm_cmpgt_ps(w, zero);
        if (which == 1) d[i] = pick(spaced, (s0 * h1 + s1 * h0) * _mm_div_ps(one, w), none);
        else if (which == 0) d[i] = pick(spaced, s0 + (s0 - s1) * _mm_div_ps(h0, w), s0);
        else d[i] = pick(spaced, s1 + (s1 - s0) * _mm_div_ps(h1, w), s1);
        unpack(d[i], out);
        for (int l = 0; l < 4; ++l) c[l]->d[i] = a[l]->d[i] = out[l];
    }
    const WholeArc whole;
    __m128 arc = zero;
    for (int l = 0; l < 4; ++l) a[l]->arc[0] = 0.0f;
    if (!frames) {
        for (std::size_t k = 1; k < n; ++k) {
            arc = _mm_add_ps(arc, whole(p[k - 1], p[k], d[k - 1], d[k], h[k - 1]));
            _mm_store_ps(values, arc);
            for (int l = 0; l < 4; ++l) a[l]->arc[k] = values[l];
        }
        return;
    }
    // Axis::start: from the tangent at the root and the way to the point a 31st along
    // (Curve::pointAt: hermiteAt).
    const float x = leaveAt * static_cast<float>(n - 1);
    const std::size_t i = x <= 0.0f ? 0 : std::min(static_cast<std::size_t>(x), n - 2);
    const float u = x - static_cast<float>(i), u2 = u * u;
    const V4 leaveTo = u == 0.0f ? p[i]
                                 : p[i] + (p[i + 1] - p[i]) * _mm_set1_ps(u2 * (3.0f - 2.0f * u)) +
                                       d[i] * h[i] * _mm_set1_ps(u * (1.0f - u) * (1.0f - u)) +
                                       d[i + 1] * h[i] * _mm_set1_ps(u2 * (u - 1.0f));
    V4 ft = unitOr(d[0], V4{zero, zero, one}), fn, fb;
    firstFrameFour(lanes(normals[0], normals[1], normals[2], normals[3]), ft, unitOr(leaveTo - p[0], ft), fn, fb);
    storeFrames(a, 0, ft, fn, fb);
    // The arcs, and the frames carried along the samples (Axis::arcLengths, carry).
    for (std::size_t k = 1; k < n; ++k) {
        arc = _mm_add_ps(arc, whole(p[k - 1], p[k], d[k - 1], d[k], h[k - 1]));
        _mm_store_ps(values, arc);
        for (int l = 0; l < 4; ++l) a[l]->arc[k] = values[l];
        reflectFour(ft, fn, fb, p[k] - p[k - 1], unitOr(d[k], ft));
        storeFrames(a, k, ft, fn, fb);
    }
#else
    for (int l = 0; l < 4; ++l) {
        c[l]->make(points[l], n);
        a[l]->slopes(c[l]);
        a[l]->arcLengths();
        if (!frames) continue;
        a[l]->start(normals[l], c[l]->pointAt(leaveAt));
        a[l]->carry();
    }
#endif
}

}  // namespace

// Wraps strands into curls around their current shape: a helix (the default), a planar wave or
// zigzag (kinky hair), or a coil, a tight helix a few strand widths across (coily hair). Each
// strand curls on its own, from a phase of its own; with `coherence` clump the strands of a clump
// (Clump's clumpId) curl as one around the clump's middle, with one phase, frequency and plane
// (ringlets).
class CurlOp : public Operator {
public:
    // The scalp's normal at each root seeds the frames the curls turn in.
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::string typeName() const override { return op::kCurl; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"radius", 0.5, "Curl radius, scene units (mappable, per point); a coil's is coilWidths", "length"}
                .soft(0, 2)
                .when("shape", {"helix", "wave", "zigzag"}),
            ParamSpec{"frequency", 0.4, "Turns per unit of length", "perLength"}.soft(0, 2),
            {"frequencyProfile", Ramp::constant(1.0f),
             "Frequency along the length, root to tip, a share of frequency (the tightest): curls loosening or tightening "
             "toward the tip"},
            ParamSpec{"randomness", 0.3, "Per-strand variation of radius and frequency, 0-1", "fraction"}.range(0, 1),
            ParamSpec{"spacingRandom", 0.0,
                      "Smooth random variation of the spacing along each strand (each clump's with coherence clump), 0-1: "
                      "the frequency varies by up to this share every turn or two",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.25f, 1.0f}, {1.0f, 1.0f}},
             "Radius along the length, root to tip (eased in from 0 at the root over the first quarter turn)"},
            {"shape", std::string("helix"),
             "helix: around the strand; wave: a planar sine; zigzag: sharp planar kinks (kinky hair); coil: a tight helix "
             "coilWidths strand widths across (coily hair)", "", {"helix", "wave", "zigzag", "coil"}},
            ParamSpec{"coilWidths", 3.0, "Coil: its radius in strand widths at each point (the widths the strands carry: put "
                                         "a Width node before it)"}
                .soft(0, 10)
                .when("shape", {"coil"}),
            ParamSpec{"phase", 0.0, "Where along a turn every curl starts, in turns (0-1)", "turns"}.soft(0, 1),
            ParamSpec{"phaseRandom", 1.0, "How much of a turn each strand's (or clump's) start varies at random, 0-1",
                      "fraction"}
                .range(0, 1),
            {"coherence", std::string("strand"),
             "strand: each strand curls on its own; clump: the strands of a clump curl together around its middle, with "
             "one phase, frequency and plane (ringlets; after a Clump)", "", {"strand", "clump"}},
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "Coherence clump: the channel naming each strand's clump (Clump writes clumpId)"}
                .adv()
                .when("coherence", {"clump"})
                .readsChannel(),
            ParamSpec{"weight", 0.0,
                      "How much each part of a curl stretches under the hair hanging below it, 0 or more: its turns spread "
                      "apart where more turns hang below them along weightDirection (loose at the root, tight at the tip; "
                      "long ringlets more than short curls, hair lying across it or pointing up barely), each turn keeping "
                      "its length, so the radius narrows (an overstretched curl goes straight). 0: none; 1: 10 turns hanging "
                      "straight below a point double its spacing"}
                .range(0, 10)
                .soft(0, 2),
            ParamSpec{"weightDirection", Vec3f{0.0f, -1.0f, 0.0f},
                      "The way the weight pulls (world space): curls stretch where the strands hang along it"}
                .world(),
            notAnimatable(ParamSpec{"tolerance", 0.5,
                                    "How far a straight segment may stray from the round curl, a share of its radius: where the "
                                    "points coming in are too few a turn for it, Curl cuts each segment evenly along the smooth "
                                    "strand first (0.5: 3 a turn, so no curl aliases into another; 0.3: 4; 0.1: 7; 0.03: 13; a "
                                    "zigzag more than 2 / tolerance, keeping its kinks' peaks). 0: only the points coming in. "
                                    "Points added take the GPU preview off this node: more Resample segments keep it",
                                    "fraction"}
                              .range(0, 1)
                              .soft(0, 0.5))
                .adv(),
            notAnimatable(ParamSpec{"maxSegments", std::int64_t{256}, "The most segments a strand may have with the points Curl adds"}
                              .range(1, 4096)
                              .soft(16, 1024))
                .adv(),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
        };
    }
    // A coil on strands that carry no widths of their own (every width 1, as before any Width); and
    // strands Curl added points to.
    std::string note(const EvalContext&) const override {
        std::string text;
        if (unitWidths_.load(std::memory_order_relaxed)) {
            text = "coil reads the strands' widths, and these have none set (1 everywhere): put a Width node before it";
        }
        if (const std::size_t added = added_.load(std::memory_order_relaxed)) {
            if (!text.empty()) text += "; ";
            text += "added points to " + std::to_string(added) + (added == 1 ? " strand" : " strands") +
                    " too few a turn for the tolerance (more Resample segments keep the GPU preview)";
        }
        return text;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand curls alone: only the strands the input changed in curl again (while Curl adds
    // no points to them). Coherent by clump, a strand's curl follows its clump's middle, which any
    // member moves: in full.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    enum class Shape { Helix, Wave, Zigzag, Coil };

    struct Settings {
        Shape shape = Shape::Helix;
        float frequency = 4.0f, randomness = 0.3f, phase = 0.0f, phaseRandom = 1.0f, coilWidths = 3.0f;
        std::uint64_t seed = 0;
        Ramp frequencyProfile;
        std::vector<float> frequencyTangents;  // its keys' (Ramp::at)
        bool profiled = false;  // frequencyProfile other than 1 everywhere
        float spacingRandom = 0.0f;
        float weight = 0.0f;  // weighted curls (0 without a weightDirection)
        Vec3f weightDirection;  // unit
        // The curl's spacing as it always was: the angle 2 pi f s + phase.
        bool even() const { return !profiled && !(spacingRandom > 0.0f) && !(weight > 0.0f); }
        // An axis's settings for its spacing: the profile, and weighted curls.
        void spacing(Axis& axis) const {
            axis.profile = profiled ? &frequencyProfile : nullptr;
            axis.tangents = profiled ? frequencyTangents.data() : nullptr;
            axis.load = weight / kWeightTurns;
            axis.down = weightDirection;
            axis.spacingRandom = spacingRandom;
        }
    };
    // The draws, in the order the helix always made them (its phase, radius and frequency); the
    // plane after them, used only by a planar shape; the spacing noise's key from all four.
    static Draw drawFor(std::uint64_t key, const Settings& s) {
        Rng rng(hashCombine(key, s.seed ^ 0xC041));
        Draw d;
        const float r0 = rng.nextFloat(), r1 = rng.nextFloat(), r2 = rng.nextFloat(), r3 = rng.nextFloat();
        d.phase = kTwoPi * (s.phase + s.phaseRandom * r0);
        d.vary = 1.0f + s.randomness * (-1.0f + 2.0f * r1);  // (Rng::uniform(-1, 1))
        d.frequency = s.frequency * (1.0f + 0.5f * s.randomness * (-1.0f + 2.0f * r2));
        if (s.shape == Shape::Wave || s.shape == Shape::Zigzag) d.plane = kTwoPi * r3;
        d.spacingKey = curlSpacingKey(r0, r1, r2, r3);
        return d;
    }
    // The offset of a point `r` from the strand at `angle` along its curl, in its frame.
    static Vec3f offsetAt(Shape shape, const Frame& frame, float r, float angle, float plane) {
        if (shape == Shape::Helix || shape == Shape::Coil) return frame.normal * (r * pm::cos(angle)) + frame.binormal * (r * pm::sin(angle));
        const Vec3f across = frame.normal * pm::cos(plane) + frame.binormal * pm::sin(plane);
        return across * (r * (shape == Shape::Wave ? pm::sin(angle) : zigzag(angle)));
    }

    // Each clump's axis (clumps found by the channel), and each strand's clump (kAlone: none,
    // curls on its own).
    struct Clumps {
        std::vector<std::uint32_t> of;
        std::vector<Axis> axes;
    };
    Clumps findClumps(const StrandSet& in, const Settings& set, const ScalpMesh* scalp) const;

    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done. The points it adds first (pointsFor,
    // curlDensified), then each strand curled at its own points (runOwn).
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const;
    bool runOwn(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                StrandChanges* changed) const;
    // The points a strand of `n` points `p` is curled at: its own, or with the tolerance, each
    // segment cut into as many as its longest needs at the most the curl turns (at most maxSegments).
    std::size_t pointsFor(const Vec3f* p, std::size_t n) const;

    mutable std::atomic<bool> unitWidths_{false};
    mutable std::atomic<std::size_t> added_{0};  // strands the last evaluation added points to
};

// The scalp's normal at a root on it; zero without a scalp or for a root off it.
static Vec3f rootNormalOn(const ScalpMesh* scalp, const StrandRoot& root) {
    return scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
}

CurlOp::Clumps CurlOp::findClumps(const StrandSet& in, const Settings& set, const ScalpMesh* scalp) const {
    // Coherent by clump: each clump's middle (the mean of its core strands, preview_core.h, at the
    // same share of their points, as Twist takes it: the same at every preview density), its
    // frames and arc length; every member then takes the clump's curl at its own share of the
    // length, so the clump curls as one. Strands in no clump (no channel value, or a negative one),
    // or in one without a core strand, curl on their own.
    const std::string channel = params().getString("clumpChannel");
    const std::vector<float>* groups = in.strandChannel(channel);
    if (!groups) throw GraphError("Curl: coherence clump needs a Clump before it (no '" + channel + "' channel to find the clumps)");
    Clumps c;
    std::map<long long, std::size_t> slot;
    std::vector<long long> keys;
    std::vector<std::vector<std::size_t>> members;
    c.of.assign(in.strandCount(), kAlone);
    const PreviewCore core(in);
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        const long long key = std::llround((*groups)[s]);
        if (key < 0 || in.strandPointCount(s) < 2) continue;
        auto it = slot.find(key);
        if (it == slot.end()) {
            it = slot.emplace(key, members.size()).first;
            members.emplace_back();
            keys.push_back(key);
        }
        c.of[s] = static_cast<std::uint32_t>(it->second);
        if (core.core(s)) members[it->second].push_back(s);  // the middle's
    }
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        if (c.of[s] != kAlone && members[c.of[s]].empty()) c.of[s] = kAlone;  // no core strand: alone
    }
    c.axes.resize(members.size());
    const std::size_t uniform = in.uniformPointCount();  // members' common count: the clump's curl at its points, made once
    parallelFor(0, members.size(), [&](std::size_t k) {
        if (members[k].empty()) return;
        Axis& axis = c.axes[k];
        // Its samples: its members summed in strand order at each share, on their smooth curves.
        thread_local Curve curve;
        for (const std::size_t s : members[k]) {
            curve.make(in.strandPoints(s), in.strandPointCount(s));
            for (std::size_t i = 0; i < kAxisSamples; ++i) axis.x[i] += curve.pointAt(static_cast<float>(i) / static_cast<float>(kAxisSamples - 1));
        }
        for (std::size_t i = 0; i < kAxisSamples; ++i) axis.x[i] = axis.x[i] / static_cast<float>(members[k].size());
        // Its frames from its members' mean root normal (summed in strand order).
        Vec3f normal;
        for (const std::size_t s : members[k]) normal += rootNormalOn(scalp, in.roots()[s]);
        set.spacing(axis);
        // A clump's draw: from its key, apart from any strand ID's (before its table: weighted
        // curls count its turns).
        axis.draw = drawFor(hashCombine(hash64(static_cast<std::uint64_t>(keys[k])), 0xC1D0C0E5ull), set);
        axis.make(normal.normalized(), uniform, axis.x[1]);
    }, 4);
    return c;
}

// The strands `which` of `in` (all: null), each at `counts[k]` points: its segments each cut into
// as many (counts[k] - 1 over its own segments) on the strand taken as a smooth curve (Curve), its
// own points kept; widths and point channels taken linearly along each segment there.
StrandsPtr curlDensified(const StrandSet& in, const std::vector<std::size_t>& counts, const std::vector<std::uint32_t>* which) {
    const std::size_t made = counts.size();
    auto out = std::make_shared<StrandSet>();
    out->resize(counts, StrandSet::kLeavePositions);
    auto source = [&](std::size_t k) { return which ? static_cast<std::size_t>((*which)[k]) : k; };
    std::vector<StrandId>& ids = out->mutableIds();
    std::vector<StrandRoot>& roots = out->mutableRoots();
    for (std::size_t k = 0; k < made; ++k) {
        ids[k] = in.ids()[source(k)];
        roots[k] = in.roots()[source(k)];
    }
    std::vector<std::vector<float>*> outPoint;
    std::vector<const std::vector<float>*> inPoint;
    for (const std::string& name : in.pointChannelNames()) {
        outPoint.push_back(&out->addPointChannel(name, in.pointChannelDefault(name)));
        inPoint.push_back(in.pointChannel(name));
    }
    for (const std::string& name : in.strandChannelNames()) {
        std::vector<float>& to = out->addStrandChannel(name, in.strandChannelDefault(name));
        const std::vector<float>& from = *in.strandChannel(name);
        for (std::size_t k = 0; k < made; ++k) to[k] = from[source(k)];
    }
    const bool hasWidths = in.widths().size() == in.pointCount();
    std::vector<float>& outWidths = out->mutableWidths();
    parallelFor(0, made, [&](std::size_t k) {
        const std::size_t s = source(k), n = in.strandPointCount(s), m = counts[k];
        const std::size_t begin = in.strandBegin(s), to = out->strandBegin(k);
        const Vec3f* pts = in.strandPoints(s);
        Vec3f* dst = out->strandPoints(k);
        const std::size_t q = m == n || n < 2 ? 1 : (m - 1) / (n - 1);
        auto values = [&](const float* from, float* into) {
            if (q == 1) {
                std::copy_n(from, n, into);
                return;
            }
            for (std::size_t j = 0; j + 1 < n; ++j) {
                for (std::size_t r = 0; r < q; ++r) {
                    const float u = static_cast<float>(r) / static_cast<float>(q);
                    into[j * q + r] = r == 0 ? from[j] : from[j] + (from[j + 1] - from[j]) * u;
                }
            }
            into[m - 1] = from[n - 1];
        };
        if (q == 1) {
            std::copy_n(pts, n, dst);
        } else {
            thread_local Curve curve;
            curve.make(pts, n);
            for (std::size_t j = 0; j + 1 < n; ++j) {
                for (std::size_t r = 0; r < q; ++r) dst[j * q + r] = r == 0 ? pts[j] : curve.at(j, static_cast<float>(r) / static_cast<float>(q));
            }
            dst[m - 1] = pts[n - 1];
        }
        if (hasWidths) values(in.widths().data() + begin, outWidths.data() + to);
        for (std::size_t c = 0; c < outPoint.size(); ++c) values(inPoint[c]->data() + begin, outPoint[c]->data() + to);
    }, 16);
    out->setInstances(in.instances());
    for (const auto& layer : in.cardLayers()) out->addCardLayer(layer);
    for (const auto& layer : in.featherLayers()) out->addFeatherLayer(layer);
    return out;
}

std::size_t CurlOp::pointsFor(const Vec3f* p, std::size_t n) const {
    const float tolerance = std::clamp(static_cast<float>(params().getDouble("tolerance")), 0.0f, 1.0f);
    const auto most = static_cast<std::size_t>(std::max<std::int64_t>(1, params().getInt("maxSegments")));
    if (!(tolerance > 0.0f) || n < 2 || most <= n - 1) return n;
    // A chord strays r (1 - cos(pi / m)) from a turn of m points: within tolerance r at
    // m = pi / acos(1 - tolerance) a turn. A zigzag's kinks: the point nearest one at most half a
    // spacing off loses about 2 / m of its peak, so more than 2 / tolerance a turn.
    int perTurn = std::max(3, static_cast<int>(std::ceil(kPi / pm::acos(1.0f - tolerance) - 1e-4f)));
    if (params().getString("shape") == "zigzag") perTurn = std::max(perTurn, static_cast<int>(std::ceil(2.0f / tolerance)) + 1);
    // The most the curl turns per unit of length: its frequency at its randomness's most, times the
    // profile's most and the spacing noise's.
    float profileMost = 0.0f;
    for (const auto& key : params().getRamp("frequencyProfile").keys) profileMost = std::max(profileMost, key.second);
    const float randomness = static_cast<float>(params().getDouble("randomness"));
    const float spacing = std::clamp(static_cast<float>(params().getDouble("spacingRandom")), 0.0f, 1.0f);
    const float turns = std::fabs(static_cast<float>(params().getDouble("frequency"))) * (1.0f + 0.5f * std::fabs(randomness)) * profileMost *
                        (1.0f + spacing);
    float longest = 0.0f;
    for (std::size_t j = 1; j < n; ++j) longest = std::max(longest, (p[j] - p[j - 1]).length());
    const float wanted = std::ceil(longest * turns * static_cast<float>(perTurn) - 1e-3f);
    if (!(wanted > 1.0f)) return n;
    const std::size_t q = std::min(static_cast<std::size_t>(std::min(wanted, 1e6f)), most / (n - 1));
    return q <= 1 ? n : n + (n - 1) * (q - 1);
}

bool CurlOp::run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                 StrandChanges* changed) const {
    const StrandsPtr in = requireStrands(inputs, 0, op::kCurl);
    const std::size_t strands = in->strandCount();
    auto withStrands = [&](StrandsPtr given) {
        std::vector<Value> again = inputs;
        again[0] = Value(std::move(given));
        return again;
    };
    if (incremental) {
        const StrandChanges* changes = someStrandsChanged(*incremental, 0);
        const StrandsPtr last = changes ? valueAs<StrandsPtr>(incremental->previous) : nullptr;
        if (!last || last->strandCount() != strands) return false;
        std::atomic<bool> other{false};
        std::vector<std::size_t> counts(changes->strands.size());
        parallelFor(0, counts.size(), [&](std::size_t k) {
            const std::size_t s = changes->strands[k];
            counts[k] = pointsFor(in->strandPoints(s), in->strandPointCount(s));
            if (counts[k] != last->strandPointCount(s)) other = true;  // (another layout: in full)
        }, 256);
        if (other) return false;
        if (last->pointCount() == in->pointCount()) return runOwn(ctx, inputs, incremental, result, changed);  // (none added)
        // With the points it added: the changed strands made apart (as in full), written over the
        // last result's, while each strand's curl reads it alone (alone, its radius the same
        // everywhere).
        if (params().getString("coherence") == "clump" || !ScalarField(params(), "radius", scalpOf(ctx)).isConstant()) return false;
        if (changes->none()) {
            *result = last;
            *changed = *changes;
            return true;
        }
        Value made;
        if (!runOwn(ctx, withStrands(curlDensified(*in, counts, &changes->strands)), nullptr, &made, nullptr)) return false;
        auto out = copyOfLast(last, *incremental);
        writeStrands(*out, *valueAs<StrandsPtr>(made), changes->strands);
        *result = StrandsPtr(out);
        *changed = *changes;
        return true;
    }
    std::vector<std::size_t> counts(strands);
    std::atomic<std::size_t> added{0};
    parallelFor(0, strands, [&](std::size_t s) {
        counts[s] = pointsFor(in->strandPoints(s), in->strandPointCount(s));
        if (counts[s] != in->strandPointCount(s)) added.fetch_add(1, std::memory_order_relaxed);
    }, 256);
    if (added == 0) {
        added_.store(0, std::memory_order_relaxed);
        return runOwn(ctx, inputs, nullptr, result, nullptr);
    }
    if (!runOwn(ctx, withStrands(curlDensified(*in, counts, nullptr)), nullptr, result, nullptr)) return false;
    added_.store(added, std::memory_order_relaxed);
    return true;
}

bool CurlOp::runOwn(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
                    StrandChanges* changed) const {
    const StrandsPtr in = requireStrands(inputs, 0, op::kCurl);
    const ScalpMesh* scalp = scalpOf(ctx);
    const ScalarField radiusField(params(), "radius", scalp);
    radiusField.validate(*in);
    const bool perPoint = radiusField.perPoint();
    Settings set;
    const std::string shapeName = params().getString("shape");
    set.shape = shapeName == "wave" ? Shape::Wave : shapeName == "zigzag" ? Shape::Zigzag : shapeName == "coil" ? Shape::Coil : Shape::Helix;
    set.frequency = static_cast<float>(params().getDouble("frequency"));
    set.randomness = static_cast<float>(params().getDouble("randomness"));
    set.phase = static_cast<float>(params().getDouble("phase"));
    set.phaseRandom = static_cast<float>(params().getDouble("phaseRandom"));
    set.coilWidths = static_cast<float>(params().getDouble("coilWidths"));
    set.seed = static_cast<std::uint64_t>(params().getInt("seed"));
    const Ramp profile = params().getRamp("profile");
    set.frequencyProfile = params().getRamp("frequencyProfile");
    set.frequencyTangents = set.frequencyProfile.tangents();
    set.profiled = std::any_of(set.frequencyProfile.keys.begin(), set.frequencyProfile.keys.end(), [](const auto& k) { return k.second != 1.0f; });
    set.spacingRandom = std::clamp(static_cast<float>(params().getDouble("spacingRandom")), 0.0f, 1.0f);
    const Vec3f weightDirection = params().getVec3("weightDirection");
    if (weightDirection.length() > 0.0f) {
        set.weightDirection = weightDirection.normalized();
        set.weight = std::max(0.0f, static_cast<float>(params().getDouble("weight")));
    }
    const bool byClump = params().getString("coherence") == "clump";
    const bool coil = set.shape == Shape::Coil;
    if (coil) {
        const std::vector<float>& w = in->widths();
        unitWidths_.store(!w.empty() && std::all_of(w.begin(), w.end(), [](float v) { return v == 1.0f; }), std::memory_order_relaxed);
    } else {
        unitWidths_.store(false, std::memory_order_relaxed);
    }

    const StrandChanges* changes = nullptr;
    StrandsPtr last;  // the last result, to make again only the strands the input changed in
    if (incremental) {
        changes = someStrandsChanged(*incremental, 0);
        // (Laid out as its input, or with the points Curl added to some strands.)
        last = changes ? valueAs<StrandsPtr>(incremental->previous) : nullptr;
        if (!last || last->strandCount() != in->strandCount() || last->pointCount() < in->pointCount() || byClump ||
            !radiusField.strandLocal()) {
            return false;
        }
        if (changes->none()) {  // the same strands in: the same result
            *result = last;
            *changed = *changes;
            return true;
        }
    }
    const std::size_t strandCount = in->strandCount();
    // On the CPU engine (look.slang CurlCS: one source with the GPU preview's) where Curl adds no
    // points: at tolerance 0 here, else once the strands' counts say it adds none (below).
    auto onEngine = [&] {
        gpu::StageInputs inputs;
        if (byClump && !(inputs.curl = gpu::curlInputs(*in, scalp, params().getString("clumpChannel"), set.seed))) return false;
        StrandsPtr made = gpu::runStage(gpu::StageKind::Curl, params(), in, scalp, std::move(inputs));
        if (!made) return false;
        added_.store(0, std::memory_order_relaxed);
        *result = std::move(made);
        return true;
    };
    if (!incremental && gpu::cpuEngineOn() && onEngine()) return true;
    const Clumps clumps = byClump ? findClumps(*in, set, scalp) : Clumps{};
    const RampAlong along(profile, *in);
    const float* widths = coil && in->widths().size() == in->pointCount() ? in->widths().data() : nullptr;
    // The radius at point i of strand s, before the draw's variation and the profile: a coil's,
    // the param's per point, or the strand's own (`strandRadius`).
    auto radiusAt = [&](std::size_t s, std::size_t i, float strandRadius) {
        if (coil) return widths ? set.coilWidths * widths[in->strandBegin(s) + i] : set.coilWidths;
        return perPoint ? radiusField.atPoint(*in, s, i) : strandRadius;
    };
    auto strandRadiusOf = [&](std::size_t s) { return perPoint || coil ? 0.0f : radiusField.at(*in, s); };

    // Strand s's curve and the axis it curls about: its clump's, or made from it into `own`: its
    // own points, or kAxisSamples on the curve through more (finer detail than that, a kink
    // where a Clump's center bends, would make the curl's arc length depend on the points).
    // `beginAxis` makes them up to the axis's first frame (returning its clump's axis, or null for
    // its own, in `own`), `prepare` all of it. `prepareFour` makes four strands' from `first` into
    // `axes` (null for a strand of fewer than 2 points): the frames of four strands curling alone
    // on as many samples side by side (carryFour), and four strands of as many points, each its
    // own axis's samples, side by side all through (makeFour).
    auto beginAxis = [&](std::size_t s, Curve& curve, Axis& own) -> const Axis* {
        const std::size_t n = in->strandPointCount(s);
        curve.make(in->strandPoints(s), n);
        const std::uint32_t c = byClump ? clumps.of[s] : kAlone;
        if (c != kAlone) return &clumps.axes[c];
        own.samples = std::min(n, kAxisSamples);
        set.spacing(own);
        own.draw = drawFor(in->ids()[s], set);  // (before its table: weighted curls count its turns)
        for (std::size_t k = 0; k < own.samples; ++k) {
            own.x[k] = own.samples == n ? curve.p[k] : curve.pointAt(static_cast<float>(k) / static_cast<float>(own.samples - 1));
        }
        own.shape(own.samples == n ? &curve : nullptr);
        own.start(rootNormalOn(scalp, in->roots()[s]), curve.pointAt(1.0f / static_cast<float>(kAxisSamples - 1)));
        return nullptr;
    };
    auto prepare = [&](std::size_t s, Curve& curve, Axis& own) -> const Axis& {
        if (const Axis* clump = beginAxis(s, curve, own)) return *clump;
        own.carry();
        own.finish(in->strandPointCount(s));
        return own;
    };
    // `frames` false: all a strand's count needs (plan's arcs), its frames left out where nothing
    // else needs them (weighted curls do).
    auto prepareFour = [&](std::size_t first, Curve* curves, Axis* owns, const Axis** axes, bool frames = true) {
        frames = frames || set.weight > 0.0f;
        const std::size_t count = std::min<std::size_t>(4, strandCount - first);
        const std::size_t n = in->strandPointCount(first);
        bool same = count == 4 && !byClump && n >= 3 && n <= kAxisSamples;
        for (std::size_t k = 1; k < count && same; ++k) same = in->strandPointCount(first + k) == n;
        if (same) {
            Curve* const c[4] = {&curves[0], &curves[1], &curves[2], &curves[3]};
            Axis* const a[4] = {&owns[0], &owns[1], &owns[2], &owns[3]};
            const Vec3f* points[4];
            Vec3f normals[4];
            for (std::size_t k = 0; k < 4; ++k) {
                points[k] = in->strandPoints(first + k);
                normals[k] = rootNormalOn(scalp, in->roots()[first + k]);
                set.spacing(owns[k]);
                owns[k].draw = drawFor(in->ids()[first + k], set);
            }
            makeFour(c, a, points, n, normals, frames);
            for (std::size_t k = 0; k < 4; ++k) {
                owns[k].profiled();
                owns[k].finish(n);
                axes[k] = &owns[k];
            }
            return;
        }
        bool four = count == 4;
        for (std::size_t k = 0; k < count; ++k) {
            axes[k] = nullptr;
            if (in->strandPointCount(first + k) < 2) {
                four = false;
                continue;
            }
            if ((axes[k] = beginAxis(first + k, curves[k], owns[k])) != nullptr || owns[k].samples != owns[0].samples) four = false;
        }
        if (four && frames) {
            Axis* const all[4] = {&owns[0], &owns[1], &owns[2], &owns[3]};
            carryFour(all);
        }
        for (std::size_t k = 0; k < count; ++k) {
            if (axes[k] || in->strandPointCount(first + k) < 2) continue;
            if (!four && frames) owns[k].carry();
            owns[k].finish(in->strandPointCount(first + k), frames);
            axes[k] = &owns[k];
        }
    };

    // Strand s curled at its own points into `to`.
    auto curl = [&](std::size_t s, const Curve& curve, const Axis& axis, Vec3f* to) {
        const std::size_t n = curve.n;
        const Draw& d = axis.draw;
        const float strandRadius = strandRadiusOf(s);
        to[0] = curve.p[0];  // the root stays
        for (std::size_t i = 1; i < n; ++i) {
            const float t = paramAt(i, n);
            Frame frame;
            float w = 0.0f;  // the profiled arc: eased in over the first quarter turn by it, turned by it
            axis.arcAt(i, n, &frame, &w);
            const float base = radiusAt(s, i, strandRadius);
            float r = base * d.vary * along(i, n) * curlEase(w, t, d.frequency);
            if (axis.load > 0.0f) {
                // Weighted: the share of the curl's own radius a turn's wire keeps at the stretched
                // pitch.
                const float local = curlLocalFrequency(d.frequency, set.profiled ? set.frequencyProfile.at(t, set.frequencyTangents.data()) : 1.0f,
                                                       w, set.spacingRandom, d.spacingKey);
                r *= stretchedShare(base * d.vary, axis.stretchAt(t), local);
            }
            const float angle = curlAngle(d.frequency, w, d.phase, set.spacingRandom, d.spacingKey);
            to[i] = curve.p[i] + offsetAt(set.shape, frame, r, angle, d.plane);
        }
    };

    if (last) {
        // The strands the input changed in, into the last result's copy (laid out as the input).
        const std::vector<std::uint32_t>& list = changes->strands;
        auto out = std::make_shared<StrandSet>(*in);
        positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        if (!(gpu::cpuEngineOn() && gpu::runStageOnStrands(gpu::StageKind::Curl, params(), in, list, scalp, *out))) {
            forEachListed(list, [&](std::size_t s) {
                thread_local Curve curve;
                thread_local Axis own;
                if (in->strandPointCount(s) < 2) {
                    copyStrandPoints(*out, *in, s);
                    return;
                }
                curl(s, curve, prepare(s, curve, own), out->strandPoints(s));
            });
        }
        *result = StrandsPtr(out);
        *changed = *changes;
        return true;
    }

    // Each strand curled at its own points.
    auto out = std::make_shared<StrandSet>(*in);
    out->overwritePositions();  // every point written below
    parallelFor(0, (strandCount + 3) / 4, [&](std::size_t g) {
        thread_local std::array<Curve, 4> curves;
        thread_local std::array<Axis, 4> owns;
        const Axis* axes[4];
        const std::size_t first = 4 * g;
        prepareFour(first, curves.data(), owns.data(), axes, true);
        for (std::size_t s = first; s < std::min(first + 4, strandCount); ++s) {
            if (in->strandPointCount(s) < 2) {
                copyStrandPoints(*out, *in, s);
                continue;
            }
            curl(s, curves[s - first], *axes[s - first], out->strandPoints(s));
        }
    }, 16);
    *result = StrandsPtr(out);
    return true;
}

void registerCurlOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCurl,
                          {"look", "Helical curls around each strand.", "coil wave helix kinky afro"},
                          [] { return std::make_unique<CurlOp>(); });
}

}  // namespace groomlab
