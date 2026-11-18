// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

// Orthonormal frame at a curve point: tangent along the curve, normal and
// binormal spanning the cross-section (binormal = tangent x normal).
struct Frame {
    Vec3f tangent{0, 0, 1};
    Vec3f normal{1, 0, 0};
    Vec3f binormal{0, 1, 0};

    // Frame-local (x along normal, y along binormal, z along tangent) to world direction.
    Vec3f toWorld(const Vec3f& local) const { return normal * local.x + binormal * local.y + tangent * local.z; }
    Vec3f toLocal(const Vec3f& world) const { return {world.dot(normal), world.dot(binormal), world.dot(tangent)}; }
};

// A unit vector perpendicular to v (deterministic).
Vec3f anyPerpendicular(const Vec3f& v);

// A polyline's unit tangent at point i by central differences: from the point before to the one
// after (an end: from or to itself); zero where those are the same point.
inline Vec3f centralTangent(const Vec3f* points, std::size_t count, std::size_t i) {
    return (points[std::min(i + 1, count - 1)] - points[i > 0 ? i - 1 : 0]).normalized();
}
inline Vec3f centralTangent(const std::vector<Vec3f>& points, std::size_t i) { return centralTangent(points.data(), points.size(), i); }

// Rotation-minimizing frames along a polyline, by the double reflection
// method (Wang et al. 2008). Tangents use central differences. The first
// normal is `initialNormal` made perpendicular to the first tangent (or any
// perpendicular if that degenerates). Frames twist as little as possible
// along the curve, so offsets stored in them survive reshaping the curve.
std::vector<Frame> rotationMinimizingFrames(const Vec3f* points, std::size_t count, const Vec3f& initialNormal);
// The same into `out` (resized to count), reusing its storage.
void rotationMinimizingFramesInto(const Vec3f* points, std::size_t count, const Vec3f& initialNormal,
                                  std::vector<Frame>& out);
// The same for four polylines of `count` points each at once (SIMD lanes where the CPU has them;
// each frame chain waits on the one before, so four side by side run nearly as fast as one):
// out[k] (count frames each) gets exactly what rotationMinimizingFramesInto gives points[k].
void rotationMinimizingFrames4(const Vec3f* const points[4], std::size_t count, const Vec3f initialNormals[4],
                               Frame* const out[4]);

// The minimal rotation taking unit vector `from` onto unit vector `to`, set up
// once so it can be applied to every point of a strand. Opposite vectors turn
// half way round any perpendicular axis.
class Rotation {
public:
    Rotation() = default;  // the identity
    Rotation(const Vec3f& from, const Vec3f& to) {
        const float c = from.dot(to);
        if (c > 1.0f - 1e-6f) return;  // the same: identity
        if (c < -1.0f + 1e-6f) {
            kind_ = Kind::HalfTurn;
            k_ = anyPerpendicular(from);
            return;
        }
        kind_ = Kind::General;
        k_ = from.cross(to);  // |k| = sin(angle)
        cosine_ = c;
        inv_ = 1.0f / (1.0f + c);
    }
    Vec3f operator()(const Vec3f& v) const {
        switch (kind_) {
            case Kind::Identity: return v;
            case Kind::HalfTurn: return k_ * (2.0f * k_.dot(v)) - v;
            case Kind::General: break;
            case Kind::Twisted: break;  // (addTurned's alone)
        }
        return v * cosine_ + k_.cross(v) + k_ * (k_.dot(v) * inv_);
    }
    bool identity() const { return kind_ == Kind::Identity; }
    // A weighted sum of curves each turned onto `to` (Interpolate's blend): with turn_i =
    // Rotation(from[i], to) where turned[i] is non-zero and the identity elsewhere, for j in
    // [0, n): a[j] += turn_0(v[0][j]) * w[0], then += turn_1(v[1][j]) * w[1], and so on over
    // `count`, each curve and `a` held as rows (x at v[i], y at v[i] + stride, z at v[i] + 2 stride).
    // `twists` (null: none): where twists[i] is not 0 (and turned[i] is non-zero), turn_i is
    // TwistedTurn(from[i], to, twists[i]) instead. Every a[j] comes out to the bit as those
    // expressions make it one vector at a time; the rotations are made, and the vectors summed,
    // four side by side in SIMD lanes where the CPU has them.
    static void addTurned(const Vec3f* from, const char* turned, const Vec3f& to, const float* const* v,
                          const float* w, std::size_t count, std::size_t n, std::size_t stride, float* a,
                          const float* twists = nullptr);

private:
    enum class Kind { Identity, HalfTurn, General, Twisted };
    Kind kind_ = Kind::Identity;
    Vec3f k_;
    float cosine_ = 1.0f;
    float inv_ = 1.0f;
};

// Rotates v by the minimal rotation that takes unit vector `from` onto unit
// vector `to` (one Rotation, applied once).
Vec3f rotateBetween(const Vec3f& v, const Vec3f& from, const Vec3f& to);

// Rotation(from, to) followed by `angle` radians about `to` (right-handed), as the matrix whose
// columns are where the axes go: a direction carried along a path that bends, Interpolate's frame
// transfer round a fold (surface_paths.h). Applied as (c0 v.x + c1 v.y) + c2 v.z.
struct TwistedTurn {
    TwistedTurn(const Vec3f& from, const Vec3f& to, float angle);
    Vec3f operator()(const Vec3f& v) const { return (c0 * v.x + c1 * v.y) + c2 * v.z; }
    Vec3f c0, c1, c2;
};

}  // namespace groomlab
