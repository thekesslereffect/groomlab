// SPDX-License-Identifier: Apache-2.0
#include "groomlab/curve.h"

#include <algorithm>

namespace groomlab {

float arcLength(const Vec3f* points, std::size_t count) {
    float total = 0.0f;
    for (std::size_t i = 1; i < count; ++i) total += (points[i] - points[i - 1]).length();
    return total;
}

void cumulativeArcLength(const Vec3f* points, std::size_t count, float* out) {
    float total = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) total += (points[i] - points[i - 1]).length();
        out[i] = total;
    }
}

void arcParams(const Vec3f* points, std::size_t count, float* out) {
    cumulativeArcLength(points, count, out);
    const float total = count > 0 ? out[count - 1] : 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = total > 1e-12f ? out[i] / total
                                : (count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f);
    }
}

namespace {

// A value placed at arc positions `at` (ascending, n of them) read at t, linear between.
float sampleAt(const float* values, const float* at, std::size_t n, float t) {
    if (n == 0) return 0.0f;
    if (n == 1 || t <= at[0]) return values[0];
    if (t >= at[n - 1]) return values[n - 1];
    const float* hi = std::upper_bound(at, at + n, t);
    const std::size_t i = static_cast<std::size_t>(hi - at);  // at[i - 1] <= t < at[i]
    const float span = at[i] - at[i - 1];
    const float f = span > 0.0f ? (t - at[i - 1]) / span : 0.0f;
    return values[i - 1] * (1.0f - f) + values[i] * f;
}

}  // namespace

void resampleValuesByArcLength(const Vec3f* points, std::size_t count, const float* values, std::size_t valueCount,
                               float* out, std::size_t outCount) {
    thread_local std::vector<float> at;  // scratch, reused across calls
    at.resize(count);
    arcParams(points, count, at.data());
    const std::size_t n = std::min(count, valueCount);
    for (std::size_t k = 0; k < outCount; ++k) {
        const float t = outCount > 1 ? static_cast<float>(k) / static_cast<float>(outCount - 1) : 0.0f;
        out[k] = sampleAt(values, at.data(), n, t);
    }
}

void resampleByArcLengthInto(const Vec3f* points, std::size_t count, Vec3f* out, std::size_t outCount,
                             float lengthFraction) {
    if (outCount == 0) return;
    if (count == 0) {
        std::fill(out, out + outCount, Vec3f{});
        return;
    }
    if (count == 1 || outCount == 1) {
        std::fill(out, out + outCount, points[0]);
        return;
    }

    const float total = arcLength(points, count) * std::clamp(lengthFraction, 0.0f, 1.0f);
    const float step = total / static_cast<float>(outCount - 1);

    std::size_t seg = 0;         // current segment [seg, seg + 1]
    float segStart = 0.0f;       // arc length at points[seg]
    float segLen = (points[1] - points[0]).length();

    for (std::size_t i = 0; i < outCount; ++i) {
        const float target = step * static_cast<float>(i);
        while (seg + 2 < count && segStart + segLen < target) {
            segStart += segLen;
            ++seg;
            segLen = (points[seg + 1] - points[seg]).length();
        }
        const float t = segLen > 0.0f ? std::clamp((target - segStart) / segLen, 0.0f, 1.0f) : 0.0f;
        out[i] = points[seg] + (points[seg + 1] - points[seg]) * t;
    }
}

std::vector<Vec3f> resampleByArcLength(const Vec3f* points, std::size_t count, std::size_t outCount,
                                       float lengthFraction) {
    std::vector<Vec3f> out(outCount);
    resampleByArcLengthInto(points, count, out.data(), outCount, lengthFraction);
    return out;
}

std::vector<Vec3f> smoothPolyline(const Vec3f* points, std::size_t count, int subdivisions) {
    if (count < 3 || subdivisions < 2) return std::vector<Vec3f>(points, points + count);
    std::vector<Vec3f> out;
    out.reserve((count - 1) * static_cast<std::size_t>(subdivisions) + 1);
    // Knot spacing: the square root of the chord (centripetal); never zero.
    auto knot = [](const Vec3f& a, const Vec3f& b) { return std::max(std::sqrt((b - a).length()), 1e-4f); };
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const Vec3f& p1 = points[i];
        const Vec3f& p2 = points[i + 1];
        // Past the ends: the last three points carried on as a parabola, so an end span keeps
        // bending the way the curve does (a straight end chord would leave every end straight).
        const Vec3f p0 = i > 0 ? points[i - 1] : p1 * 3.0f - p2 * 3.0f + points[2];
        const Vec3f p3 = i + 2 < count ? points[i + 2] : p2 * 3.0f - p1 * 3.0f + points[count - 3];
        const float t0 = 0.0f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        // A span starts at its point, exactly (the pyramid's sum of it twice rounds: a guide's root
        // would move with its other points).
        out.push_back(p1);
        for (int k = 1; k < subdivisions; ++k) {
            const float t = t1 + (t2 - t1) * static_cast<float>(k) / static_cast<float>(subdivisions);
            // Barry and Goldman's pyramid.
            const Vec3f a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
            const Vec3f a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
            const Vec3f a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
            const Vec3f b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
            const Vec3f b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
            out.push_back(b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1)));
        }
    }
    out.push_back(points[count - 1]);
    return out;
}

std::vector<Vec3f> resampleSmoothCurve(const Vec3f* points, std::size_t count, std::size_t outCount) {
    if (count < 3 || outCount < 2) return resampleByArcLength(points, count, outCount);
    // Fine enough that evenly spaced samples land on the curve, not on its chords.
    const int perSpan = std::max(8, static_cast<int>((4 * outCount + count - 2) / (count - 1)));
    const std::vector<Vec3f> curve = smoothPolyline(points, count, perSpan);
    return resampleByArcLength(curve.data(), curve.size(), outCount);
}

}  // namespace groomlab
