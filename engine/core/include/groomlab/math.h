// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "groomlab/portable_math.h"
#include "groomlab/shared/groom_math.h"
#include "groomlab/vec3.h"

namespace groomlab {

// v rotated by `angle` radians about the unit vector `axis` (right-handed).
inline Vec3f rotateAxisAngle(const Vec3f& v, const Vec3f& axis, float angle) {
    const float c = pm::cos(angle), s = pm::sin(angle);
    return v * c + axis.cross(v) * s + axis * (axis.dot(v) * (1.0f - c));
}

// Symmetry across the plane where coordinate `axis` (0 x, 1 y, 2 z) equals
// `center`: a point mirrored, and a direction mirrored (its axis component flipped).
inline Vec3f mirrorPoint(const Vec3f& p, int axis, float center) {
    Vec3f out = p;
    if (axis == 0) out.x = 2.0f * center - p.x;
    if (axis == 1) out.y = 2.0f * center - p.y;
    if (axis == 2) out.z = 2.0f * center - p.z;
    return out;
}
inline Vec3f mirrorVector(const Vec3f& v, int axis) {
    Vec3f out = v;
    if (axis == 0) out.x = -v.x;
    if (axis == 1) out.y = -v.y;
    if (axis == 2) out.z = -v.z;
    return out;
}


// v times the reciprocal of its length (one division, not three: not bit-equal to normalized()),
// or zero where the length is `minLength` or less.
inline Vec3f normalizedRecip(const Vec3f& v, float minLength = 0.0f) {
    const float l = v.length();
    return l > minLength ? v * (1.0f / l) : Vec3f{};
}

// A whole number from a float as C's (int) cast makes one (toward zero; NaN: 0), kept within
// ±1e9 (a range a loop can walk).
inline std::int64_t truncToInt(float f) {
    if (!(f == f)) return 0;
    return static_cast<std::int64_t>(std::trunc(std::clamp(f, -1.0e9f, 1.0e9f)));
}

// The blends, steps and curves the GPU preview shares (shared/groom_math.h; noise.h and
// curl_phase.h bring in its noise and curl phase).
using shared::biasOf;
using shared::clamp01;
using shared::fade;
using shared::fitOf;
using shared::gainOf;
using shared::linearstep;
using shared::mix1;
using shared::smoothstepStd;

// mix1 of points.
inline Vec3f mix1(const Vec3f& a, const Vec3f& b, float t) { return a + (b - a) * t; }

// x less its floor: in [0, 1).
inline float fract(float x) { return x - std::floor(x); }

// 0 at e0, 1 at e1 and a smooth S between, held past the ends (reversed
// edges run it backwards; equal edges make a step at e0). As GLSL and the
// expression language define it: shared/groom_math.h smoothstepStd, by its C++ name.
inline float smoothstep(float e0, float e1, float x) { return smoothstepStd(e0, e1, x); }

// SeExpr's gaussstep: 0 at e0 rising to 1 at e1 along a Gaussian, 2^(-8 (1 - t)^2) (reversed edges
// run it backwards; equal edges: a step at e0).
inline float gaussstep(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = 0.0f;
    if (e0 < e1) {
        if (x < e0) return 0.0f;
        if (x >= e1) return 1.0f;
        t = 1.0f - (x - e0) / (e1 - e0);
    } else {
        if (x <= e1) return 1.0f;
        if (x > e0) return 0.0f;
        t = (x - e1) / (e0 - e1);
    }
    return pm::pow(2.0f, -8.0f * t * t);
}

// SeExpr's remap: 1 within `range` of `source`, falling to 0 over `falloff` beyond it, by
// `interp`: 0 linearstep, 1 smoothstep, else gaussstep.
inline float remapFalloff(float x, float source, float range, float falloff, int interp) {
    range = std::abs(range);
    falloff = std::abs(falloff);
    if (falloff == 0.0f) return std::abs(x - source) < range ? 1.0f : 0.0f;
    float a = 0.0f, b = 0.0f;
    if (x > source) {
        a = source + range;
        b = a + falloff;
    } else {
        a = source - range;
        b = a - falloff;
    }
    switch (interp) {
        case 0: return linearstep(b, a, x);
        case 1: return smoothstep(b, a, x);
        default: return gaussstep(b, a, x);
    }
}

// A positive size rounded to a step of a log lattice of `stepsPerDecade` steps per power of ten
// (`up`: to the step at or above it), anchored at 1: a value taken from the whole scalp or all the
// strands, rounded so that an edit changing it a little keeps it. A decade being whole steps, a
// scene scaled by a power of ten (cm to m or mm) rounds to the same step scaled alike: the groom
// looks the same. Not positive or not finite: as it is.
inline float roundToDecadeStep(float value, int stepsPerDecade, bool up = false) {
    if (!(value > 0.0f) || !std::isfinite(value)) return value;
    const double steps = pm::log10(static_cast<double>(value)) * stepsPerDecade;
    // A hair over a step (rounding, as a scaled value's logarithm carries) stays on it.
    const double whole = up ? std::ceil(steps - 1e-9) : std::round(steps);
    return static_cast<float>(pm::pow(10.0, whole / stepsPerDecade));
}

// Linear sample of `values` placed evenly on t in [0, 1] (t clamped); `fallback` when empty. A
// tube's width/aspect/twist lists and a guide's per-point layers and child offsets sample so.
template <class T>
T sampleEven(const std::vector<T>& values, float t, T fallback) {
    if (values.empty()) return fallback;
    if (values.size() == 1) return values[0];
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(values.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(x), values.size() - 2);
    const float f = x - static_cast<float>(i);
    return values[i] * (1.0f - f) + values[i + 1] * f;
}

}  // namespace groomlab
