// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/simulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "groomlab/distance_field.h"
#include "groomlab/field.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/frame_time.h"
#include "groomlab/frames.h"
#include "groomlab/graph_io.h"
#include "groomlab/noise.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "operators/internal.h"

namespace groomlab {

namespace {

// ---- Quaternions (w + xi + yj + zk) ----------------------------------------------------------

struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3f vec() const { return {x, y, z}; }
};

Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Quat conj(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }
float dot4(const Quat& a, const Quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
Quat normalized(const Quat& q) {
    const float n2 = dot4(q, q);
    if (!(n2 > 0.0f)) return Quat{};
    const float k = 1.0f / std::sqrt(n2);
    return {q.w * k, q.x * k, q.y * k, q.z * k};
}
// The third axis of unit q's frame (q e3 q*): a segment's direction.
Vec3f thirdAxis(const Quat& q) {
    return {2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.y * q.z - q.w * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)};
}
// v turned by unit q (q v q*).
Vec3f rotate(const Quat& q, const Vec3f& v) {
    const Vec3f u = q.vec();
    const Vec3f t = u.cross(v) * 2.0f;
    return v + t * q.w + u.cross(t);
}
// The rotation whose matrix has these columns (orthonormal, right-handed).
Quat fromAxes(const Vec3f& c0, const Vec3f& c1, const Vec3f& c2) {
    const float m00 = c0.x, m01 = c1.x, m02 = c2.x;
    const float m10 = c0.y, m11 = c1.y, m12 = c2.y;
    const float m20 = c0.z, m21 = c1.z, m22 = c2.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(std::max(1e-12f, 1.0f + m00 - m11 - m22)) * 2.0f;
        q = {(m21 - m12) / s, 0.25f * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(std::max(1e-12f, 1.0f + m11 - m00 - m22)) * 2.0f;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25f * s, (m12 + m21) / s};
    } else {
        const float s = std::sqrt(std::max(1e-12f, 1.0f + m22 - m00 - m11)) * 2.0f;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s};
    }
    return normalized(q);
}
// For a unit quaternion's vector part of length s (the sine of half its angle) and real part
// c >= 0: half its angle over s, so that times the vector part is the half-angle rotation vector
// (a series for small turns: no atan2 for the many nearly at rest).
float halfAngleOver(float s, float c) {
    if (s < 0.05f * c) {
        const float t = s / c, t2 = t * t;  // atan(t) / s = (t - t^3/3 + t^5/5) / s
        return (1.0f - t2 * (1.0f / 3.0f) + t2 * t2 * 0.2f) / c;
    }
    return pm::atan2(s, c) / s;
}
// The unit quaternion turning by the half-angle rotation vector r (twice its length, about it).
Quat turnOfHalfAngle(const Vec3f& r) {
    const float h2 = r.dot(r);
    if (h2 < 0.0025f) {  // half the angle below 0.05: the series, to float precision
        const float k = 1.0f - h2 * (1.0f / 6.0f) + h2 * h2 * (1.0f / 120.0f);
        return {1.0f - 0.5f * h2 + h2 * h2 * (1.0f / 24.0f), r.x * k, r.y * k, r.z * k};
    }
    const float half = std::sqrt(h2);
    const float k = pm::sin(half) / half;
    return {pm::cos(half), r.x * k, r.y * k, r.z * k};
}
// Normalized linear blend on the shorter arc (small turns: substeps, the inertia's share).
Quat nlerp(const Quat& a, Quat b, float t) {
    if (dot4(a, b) < 0.0f) b = {-b.w, -b.x, -b.y, -b.z};
    return normalized({a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
}
// Spherical blend on the shorter arc (a head turning far in one frame).
Quat slerp(const Quat& a, Quat b, float t) {
    float d = dot4(a, b);
    if (d < 0.0f) {
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    if (d > 0.9995f) return nlerp(a, b, t);
    const float theta = pm::acos(std::min(1.0f, d));
    const float s = pm::sin(theta);
    const float s0 = pm::sin((1.0f - t) * theta) / s, s1 = pm::sin(t * theta) / s;
    return normalized({a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1});
}

// A root's frame: world = origin + rot(local).
struct RootPose {
    Vec3f origin;
    Quat rot;
    Vec3f toWorld(const Vec3f& local) const { return origin + rotate(rot, local); }
    Vec3f toLocal(const Vec3f& world) const { return rotate(conj(rot), world - origin); }
};
RootPose blend(const RootPose& a, const RootPose& b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return {a.origin + (b.origin - a.origin) * t, slerp(a.rot, b.rot, t)};
}

// The bend-twist rows' compliance per unit of the orientation's inverse mass at the reference
// substep (1/96 s): stiffness 1 is rigid (0), toward 0 ever softer; 0 is no constraint at all (not
// called). 0.3 r^2.4 with r = 1/stiffness - 1: with each guide solved whole, this keeps about the
// scale bend had (the tip of a 10 cm guide held out level drops 1.2 of its length at 0.5, two
// thirds at 0.7, a quarter at 0.8 and 4% at 0.9; before: 1.3, a half, a seventh, 4%) and the Fur
// preset's motion (its tips trail a run by half their length and settle within a few frames of a
// stop, as before).
float complianceOf(float stiffness) {
    const float k = std::clamp(stiffness, 1e-4f, 1.0f);
    return 0.3f * pm::pow(1.0f / k - 1.0f, 2.4f);
}
// The stretch row's compliance per unit of a point's inverse mass at the reference substep:
// (1 - stretchStiffness) * 2/3 (none at 1: inextensible), so hair hanging under its weight is
// about as much longer as it was (25 cm at 0.6: 6%, at 0.9: 1%).
float stretchComplianceOf(float stiffness) {
    return (2.0f / 3.0f) * (1.0f - std::clamp(stiffness, 0.0f, 1.0f));
}
constexpr float kReferenceStep = 1.0f / 96.0f;
// The most a joint's rest turn is preloaded (radians, 14 degrees). Beyond, guides whose twist is
// soft drift off their groom (on a skinned bust's bob at twist 0.6: 0.06 cm on average still at
// 25 degrees, 0.006 at 14), and soft hair at a few substeps is not stable beyond 25.
constexpr float kMostPreload = 0.25f;
// The most of a joint's bend stiffness gravity's moment may take away where the hair beyond it is
// above it (standing hair): soft guides standing up off a head at the node's defaults fell over at
// 0.5 and hold at 0.25.
constexpr float kMostBuckle = 0.25f;
// The frame rootStiffnessProfile's share is taken over (a 24 fps frame): the same pull per second at any
// frame rate and substeps.
constexpr float kReferenceFrame = 1.0f / 24.0f;
// A share `a` (0-1) of something taken per `reference` seconds, as the share a step of `h` seconds
// takes: the same in a second whatever the step (1 stays 1, 0 stays 0).
float shareOver(float a, float h, float reference) {
    if (a >= 1.0f) return 1.0f;
    if (!(a > 0.0f)) return 0.0f;
    return 1.0f - pm::pow(1.0f - a, h / reference);
}
// Coulomb's friction coefficient for a `friction` of 0-1: the steepest slope hair rests on without
// sliding, as a share of a right angle (0.3: 27 degrees, mu 0.51; 0.5: 45, mu 1). 0 is no friction;
// 1 and above never slide.
constexpr float kNeverSlides = 1e30f;
float frictionCoefficientOf(float friction) {
    if (!(friction > 0.0f)) return 0.0f;
    if (friction >= 1.0f) return kNeverSlides;
    const float a = friction * 1.5707963f;
    return pm::sin(a) / pm::cos(a);
}
// The most one repulsion pass moves a point, as a share of the radius: a point crowded by many
// others is not thrown out past them.
constexpr float kMostRepulsion = 0.5f;
// Follow the leader's velocity correction (Mueller et al. 2012's s_damping): 1 takes back all
// the motion the length projection added.
constexpr float kLeaderDamping = 0.9f;
// Hair volume's density grid: the world's nodes (whole cells from the origin) in bricks of
// kBrick^3, made where a point adds to one.
class VolumeGrid {
    struct Key {
        std::int64_t x = 0, y = 0, z = 0;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(
                hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(k.x)), static_cast<std::uint64_t>(k.y)),
                            static_cast<std::uint64_t>(k.z)));
        }
    };
    static constexpr std::int64_t kBrick = 8;
    static constexpr std::size_t kNodes = static_cast<std::size_t>(kBrick * kBrick * kBrick);
    static constexpr std::size_t kNone = ~std::size_t{0};
    static std::int64_t brickOf(std::int64_t c) { return c >= 0 ? c / kBrick : -((-c + kBrick - 1) / kBrick); }
    static Key keyOf(std::int64_t x, std::int64_t y, std::int64_t z) { return {brickOf(x), brickOf(y), brickOf(z)}; }
    static std::size_t local(const Key& key, std::int64_t x, std::int64_t y, std::int64_t z) {
        return static_cast<std::size_t>(((z - key.z * kBrick) * kBrick + (y - key.y * kBrick)) * kBrick + (x - key.x * kBrick));
    }

public:
    void clear() {
        index_.clear();
        values_.clear();
        last_ = kNone;
    }
    // w added to node (x, y, z).
    void add(std::int64_t x, std::int64_t y, std::int64_t z, float w) {
        const Key key = keyOf(x, y, z);
        if (last_ == kNone || !(key == lastKey_)) {
            const auto it = index_.emplace(key, static_cast<std::uint32_t>(values_.size() / kNodes)).first;
            if (it->second * kNodes == values_.size()) values_.resize(values_.size() + kNodes, 0.0f);
            last_ = it->second;
            lastKey_ = key;
        }
        values_[last_ * kNodes + local(key, x, y, z)] += w;
    }
    // Reads nodes, each reader keeping the brick it read last (one per thread: the grid is read
    // only while readers live).
    class Reader {
    public:
        explicit Reader(const VolumeGrid& grid) : grid_(grid) {}
        float at(std::int64_t x, std::int64_t y, std::int64_t z) {
            const Key key = keyOf(x, y, z);
            if (!found_ || !(key == key_)) {
                const auto it = grid_.index_.find(key);
                brick_ = it == grid_.index_.end() ? nullptr : grid_.values_.data() + static_cast<std::size_t>(it->second) * kNodes;
                key_ = key;
                found_ = true;
            }
            return brick_ ? brick_[local(key, x, y, z)] : 0.0f;
        }

    private:
        const VolumeGrid& grid_;
        Key key_;
        const float* brick_ = nullptr;
        bool found_ = false;
    };

private:
    std::unordered_map<Key, std::uint32_t, KeyHash> index_;
    std::vector<float> values_;
    std::size_t last_ = kNone;
    Key lastKey_;
};

// ---- Four guides side by side ------------------------------------------------------------------
//
// The rods are solved four guides at a time, one guide per lane of a four-wide float (SSE2 where
// the processor has it, else a loop over the lanes). Each lane does exactly the arithmetic its
// guide alone did, operation for operation and in the same order: an add, multiply, divide or
// square root rounds the same in a lane as alone, and the rare turn that needs a sine or an
// arctangent is taken lane by lane. So a guide's result is the same to the bit whichever guides
// share its lanes (or none: a lane left over repeats the first guide and is thrown away). The
// sweep down a guide is one long chain of operations, each segment waiting for the one before:
// four chains side by side keep the processor busy where one left it waiting.

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_SIM_SSE2 1
#else
#define GROOMLAB_SIM_SSE2 0
#endif

constexpr int kLanes = 4;

#if GROOMLAB_SIM_SSE2
struct F4 {
    __m128 v;
};
struct M4 {  // a lane mask (all bits set: true)
    __m128 v;
};
inline F4 all(float a) { return {_mm_set1_ps(a)}; }
inline F4 lanesOf(float a, float b, float c, float d) { return {_mm_setr_ps(a, b, c, d)}; }
inline void store(const F4& a, float* out) { _mm_storeu_ps(out, a.v); }
inline F4 load(const float* in) { return {_mm_loadu_ps(in)}; }
inline F4 operator+(const F4& a, const F4& b) { return {_mm_add_ps(a.v, b.v)}; }
inline F4 operator-(const F4& a, const F4& b) { return {_mm_sub_ps(a.v, b.v)}; }
inline F4 operator*(const F4& a, const F4& b) { return {_mm_mul_ps(a.v, b.v)}; }
inline F4 operator/(const F4& a, const F4& b) { return {_mm_div_ps(a.v, b.v)}; }
inline F4 operator-(const F4& a) { return {_mm_xor_ps(a.v, _mm_set1_ps(-0.0f))}; }  // the sign flipped, as -x
inline F4 sqrt4(const F4& a) { return {_mm_sqrt_ps(a.v)}; }
inline M4 operator<(const F4& a, const F4& b) { return {_mm_cmplt_ps(a.v, b.v)}; }
inline M4 operator>(const F4& a, const F4& b) { return {_mm_cmpgt_ps(a.v, b.v)}; }
inline M4 operator>=(const F4& a, const F4& b) { return {_mm_cmpge_ps(a.v, b.v)}; }
inline M4 operator==(const F4& a, const F4& b) { return {_mm_cmpeq_ps(a.v, b.v)}; }
inline M4 both(const M4& a, const M4& b) { return {_mm_and_ps(a.v, b.v)}; }
inline F4 select(const M4& m, const F4& a, const F4& b) { return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))}; }
inline int bits(const M4& m) { return _mm_movemask_ps(m.v); }  // lane k's truth at bit k
#else
struct F4 {
    float v[4];
};
struct M4 {
    bool v[4];
};
inline F4 all(float a) { return {{a, a, a, a}}; }
inline F4 lanesOf(float a, float b, float c, float d) { return {{a, b, c, d}}; }
inline void store(const F4& a, float* out) {
    for (int k = 0; k < kLanes; ++k) out[k] = a.v[k];
}
inline F4 load(const float* in) { return {{in[0], in[1], in[2], in[3]}}; }
#define GROOMLAB_SIM_LANES(op)                       \
    F4 out;                                          \
    for (int k = 0; k < kLanes; ++k) out.v[k] = op; \
    return out
#define GROOMLAB_SIM_MASK(op)                        \
    M4 out;                                          \
    for (int k = 0; k < kLanes; ++k) out.v[k] = op; \
    return out
inline F4 operator+(const F4& a, const F4& b) { GROOMLAB_SIM_LANES(a.v[k] + b.v[k]); }
inline F4 operator-(const F4& a, const F4& b) { GROOMLAB_SIM_LANES(a.v[k] - b.v[k]); }
inline F4 operator*(const F4& a, const F4& b) { GROOMLAB_SIM_LANES(a.v[k] * b.v[k]); }
inline F4 operator/(const F4& a, const F4& b) { GROOMLAB_SIM_LANES(a.v[k] / b.v[k]); }
inline F4 operator-(const F4& a) { GROOMLAB_SIM_LANES(-a.v[k]); }
inline F4 sqrt4(const F4& a) { GROOMLAB_SIM_LANES(std::sqrt(a.v[k])); }
inline M4 operator<(const F4& a, const F4& b) { GROOMLAB_SIM_MASK(a.v[k] < b.v[k]); }
inline M4 operator>(const F4& a, const F4& b) { GROOMLAB_SIM_MASK(a.v[k] > b.v[k]); }
inline M4 operator>=(const F4& a, const F4& b) { GROOMLAB_SIM_MASK(a.v[k] >= b.v[k]); }
inline M4 operator==(const F4& a, const F4& b) { GROOMLAB_SIM_MASK(a.v[k] == b.v[k]); }
inline M4 both(const M4& a, const M4& b) { GROOMLAB_SIM_MASK(a.v[k] && b.v[k]); }
inline F4 select(const M4& m, const F4& a, const F4& b) { GROOMLAB_SIM_LANES(m.v[k] ? a.v[k] : b.v[k]); }
inline int bits(const M4& m) {
    int out = 0;
    for (int k = 0; k < kLanes; ++k) out |= m.v[k] ? 1 << k : 0;
    return out;
}
#undef GROOMLAB_SIM_LANES
#undef GROOMLAB_SIM_MASK
#endif
constexpr int kAllLanes = (1 << kLanes) - 1;
// std::max(a, b) lane by lane, to the bit (b only where a < b: a NaN or a signed zero as it picks).
inline F4 maxOf(const F4& a, const F4& b) { return select(a < b, b, a); }

// Vec3f and Quat over the lanes, each operation as theirs.
struct V3 {
    F4 x, y, z;
};
inline V3 v3(const Vec3f& a) { return {all(a.x), all(a.y), all(a.z)}; }
inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(const V3& a, const F4& s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator/(const V3& a, const F4& s) { return {a.x / s, a.y / s, a.z / s}; }
inline F4 dot(const V3& a, const V3& o) { return a.x * o.x + a.y * o.y + a.z * o.z; }
inline V3 cross(const V3& a, const V3& o) { return {a.y * o.z - a.z * o.y, a.z * o.x - a.x * o.z, a.x * o.y - a.y * o.x}; }
inline F4 length(const V3& a) { return sqrt4(dot(a, a)); }
inline V3 select(const M4& m, const V3& a, const V3& b) { return {select(m, a.x, b.x), select(m, a.y, b.y), select(m, a.z, b.z)}; }

struct Q4 {
    F4 w, x, y, z;
};
inline Q4 operator*(const Q4& a, const Q4& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Q4 conj(const Q4& q) { return {q.w, -q.x, -q.y, -q.z}; }
inline F4 dot4(const Q4& a, const Q4& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
inline Q4 normalized(const Q4& q) {
    const F4 n2 = dot4(q, q);
    const F4 k = all(1.0f) / sqrt4(n2);
    const M4 some = n2 > all(0.0f);
    return {select(some, q.w * k, all(1.0f)), select(some, q.x * k, all(0.0f)), select(some, q.y * k, all(0.0f)),
            select(some, q.z * k, all(0.0f))};
}
inline V3 vec(const Q4& q) { return {q.x, q.y, q.z}; }
inline V3 thirdAxis(const Q4& q) {
    const F4 two = all(2.0f);
    return {two * (q.x * q.z + q.w * q.y), two * (q.y * q.z - q.w * q.x), all(1.0f) - two * (q.x * q.x + q.y * q.y)};
}
inline V3 rotate(const Q4& q, const V3& v) {
    const V3 u = vec(q);
    const V3 t = cross(u, v) * all(2.0f);
    return v + t * q.w + cross(u, t);
}

// halfAngleOver, lane by lane (`lanes`: the lanes wanted; the others' answer is never read).
F4 halfAngleOver4(const F4& s, const F4& c, int lanes) {
    const F4 t = s / c, t2 = t * t;
    const F4 series = (all(1.0f) - t2 * all(1.0f / 3.0f) + t2 * t2 * all(0.2f)) / c;
    const int far = lanes & ~bits(s < all(0.05f) * c);
    if (far == 0) return series;
    float out[kLanes], sv[kLanes], cv[kLanes];
    store(series, out);
    store(s, sv);
    store(c, cv);
    for (int k = 0; k < kLanes; ++k) {
        if (far & (1 << k)) out[k] = pm::atan2(sv[k], cv[k]) / sv[k];
    }
    return load(out);
}
// turnOfHalfAngle, lane by lane.
Q4 turnOfHalfAngle4(const V3& r) {
    const F4 h2 = dot(r, r);
    const F4 k = all(1.0f) - h2 * all(1.0f / 6.0f) + h2 * h2 * all(1.0f / 120.0f);
    Q4 out{all(1.0f) - all(0.5f) * h2 + h2 * h2 * all(1.0f / 24.0f), r.x * k, r.y * k, r.z * k};
    const int far = kAllLanes & ~bits(h2 < all(0.0025f));
    if (far == 0) return out;
    float w[kLanes], x[kLanes], y[kLanes], z[kLanes], hv[kLanes], rx[kLanes], ry[kLanes], rz[kLanes];
    store(out.w, w);
    store(out.x, x);
    store(out.y, y);
    store(out.z, z);
    store(h2, hv);
    store(r.x, rx);
    store(r.y, ry);
    store(r.z, rz);
    for (int l = 0; l < kLanes; ++l) {
        if (!(far & (1 << l))) continue;
        const float half = std::sqrt(hv[l]);
        const float kk = pm::sin(half) / half;
        w[l] = pm::cos(half);
        x[l] = rx[l] * kk;
        y[l] = ry[l] * kk;
        z[l] = rz[l] * kk;
    }
    return {load(w), load(x), load(y), load(z)};
}

// ---- The rods' direct solve (Deul, Kugelstadt, Weiler and Bender 2018, "Direct Position-Based
// Solver for Stiff Rods") ----------------------------------------------------------------------
//
// A guide of n points has n - 1 segments, each a point (its tip) and an orientation, and two
// constraints of three rows each: stretch-shear (the segment's orientation along its two points)
// and bend-twist (the turn from the segment before, or from the root's frame, to the groomed
// one). Segment j's constraints touch only segments j - 1 and j, so the whole guide's linearized
// system (J M^-1 J^T + compliance) dlambda = -C - compliance lambda is block tridiagonal in 6x6
// blocks: solved exactly in one sweep down the guide and one back up, every point and orientation
// moved by its own inverse mass. Nothing is held while its neighbour moves, so the solve adds no
// energy to the motion, and a stiff guide is held whole in one iteration.

// One segment's settings for a substep (index j: segment j, its tip point j; slot 0 the root's).
struct RodSegment {
    float length = 0.0f;   // as groomed (0: too short to have constraints)
    float w = 0.0f;        // its tip point's inverse mass (0: held)
    float W = 0.0f;        // its orientation's inverse inertia (per radian)
    float stretch = 0.0f;  // the stretch row's compliance, along the segment (0: rigid)
    float bend = -1.0f;    // the bend rows' compliance (negative: none)
    float twist = -1.0f;   // the twist row's
};

struct Block6 {
    F4 m[6][6];
};

// a's lower triangle and diagonal replaced by its Cholesky factor L (a = L L^T, a positive
// definite), the diagonal's inverses in inv.
void cholesky6(Block6& a, F4* inv) {
    for (int j = 0; j < 6; ++j) {
        F4 d = a.m[j][j];
        for (int k = 0; k < j; ++k) d = d - a.m[j][k] * a.m[j][k];
        d = sqrt4(maxOf(d, all(1e-30f)));
        a.m[j][j] = d;
        inv[j] = all(1.0f) / d;
        for (int i = j + 1; i < 6; ++i) {
            F4 s = a.m[i][j];
            for (int k = 0; k < j; ++k) s = s - a.m[i][k] * a.m[j][k];
            a.m[i][j] = s * inv[j];
        }
    }
}
// x = (L L^T)^-1 x, with the factor from cholesky6.
void choleskySolve6(const Block6& l, const F4* inv, F4* x) {
    for (int i = 0; i < 6; ++i) {
        F4 s = x[i];
        for (int k = 0; k < i; ++k) s = s - l.m[i][k] * x[k];
        x[i] = s * inv[i];
    }
    for (int i = 5; i >= 0; --i) {
        F4 s = x[i];
        for (int k = i + 1; k < 6; ++k) s = s - l.m[k][i] * x[k];
        x[i] = s * inv[i];
    }
}
// The same for the six columns of x at once.
void choleskySolve6(const Block6& l, const F4* inv, Block6& x) {
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < i; ++k) {
            for (int c = 0; c < 6; ++c) x.m[i][c] = x.m[i][c] - l.m[i][k] * x.m[k][c];
        }
        for (int c = 0; c < 6; ++c) x.m[i][c] = x.m[i][c] * inv[i];
    }
    for (int i = 5; i >= 0; --i) {
        for (int k = i + 1; k < 6; ++k) {
            for (int c = 0; c < 6; ++c) x.m[i][c] = x.m[i][c] - l.m[k][i] * x.m[k][c];
        }
        for (int c = 0; c < 6; ++c) x.m[i][c] = x.m[i][c] * inv[i];
    }
}

// A unit quaternion's frame: its rotation matrix's columns.
struct Axes3 {
    V3 c[3];
};
Axes3 axesOf(const Q4& q) {
    const F4 zero = all(0.0f), one = all(1.0f);
    return {{rotate(q, {one, zero, zero}), rotate(q, {zero, one, zero}), rotate(q, {zero, zero, one})}};
}

// The bend-twist rows' frame of segment j: the orientation before it (the root's frame for the
// first) times its rest turn, where the groomed shape puts it.
Q4 rodTarget(std::size_t j, const Q4* q, const Q4& rootRot, const Q4* rest) {
    return normalized((j == 1 ? rootRot : q[j - 1]) * rest[j]);
}

// One segment's settings for a substep over the lanes (RodSegment's).
struct RodSegment4 {
    F4 length, w, W, stretch, bend, twist;
};

// Four guides' solve (one per thread): their inputs side by side, the system factored for a
// substep and solved again each iteration.
struct RodWork {
    std::vector<V3> p, tgt;         // the points, and the groomed shape they are pulled toward
    std::vector<Q4> q, rest;        // the orientations (segment j's at j) and rest turns
    std::vector<RodSegment4> seg;   // the segments' settings
    std::vector<F4> attract;        // per point: the pull toward the groomed shape per iteration
    std::vector<F4> lambda;         // 6 per segment: the multipliers
    std::vector<V3> d3;             // each segment's direction, as factored
    std::vector<Axes3> frame;       // each segment's bend-twist frame, as factored
    std::vector<F4> mask;           // 6 per segment: 1 for a row the solve has, 0 for one it has not
    std::vector<Block6> factor;     // D_j, Cholesky factored
    std::vector<F4> inv;            // 6 per segment: its factor's diagonal inverses
    std::vector<Block6> u;          // U_j: segment j's rows against segment j + 1's
    std::vector<Block6> x;          // D_{j-1}^-1 U_{j-1}
    std::vector<F4> rhs;            // 6 per segment: -C - compliance lambda, then dlambda
};

// The guides' systems for a substep, from their orientations then (the Jacobians depend on nothing
// else; they turn little over a substep): A = J M^-1 J^T + compliance, factored down the guide
// (D_j = A_jj - U_{j-1}^T D_{j-1}^-1 U_{j-1}). Orientations q[1..n-1] (segment j's at j).
void rodFactor(std::size_t n, const Q4& rootRot, RodWork& work) {
    const F4 zero = all(0.0f), one = all(1.0f);
    const V3 none{zero, zero, zero};
    const Q4* q = work.q.data();
    const Q4* rest = work.rest.data();
    const RodSegment4* seg = work.seg.data();
    work.d3.assign(n, none);
    work.frame.resize(n);
    work.mask.assign(6 * n, zero);
    work.factor.resize(n);
    work.inv.resize(6 * n);
    work.u.resize(n);
    work.x.resize(n);
    work.rhs.assign(6 * n, zero);
    for (std::size_t j = 1; j < n; ++j) {
        const RodSegment4& s = seg[j];
        // A segment too short has no constraints: its rows stay out of the solve.
        const M4 has = s.length > all(1e-8f);
        work.d3[j] = select(has, thirdAxis(q[j]), none);
        const Axes3 f = axesOf(rodTarget(j, q, rootRot, rest));
        for (int k = 0; k < 3; ++k) work.frame[j].c[k] = select(has, f.c[k], none);
        F4* mask = &work.mask[6 * j];
        mask[0] = mask[1] = mask[2] = select(has, one, zero);
        mask[3] = mask[4] = select(both(has, s.bend >= zero), one, zero);
        mask[5] = select(both(has, s.twist >= zero), one, zero);
    }
    for (std::size_t j = 1; j < n; ++j) {
        const RodSegment4& s = seg[j];
        const F4* mask = &work.mask[6 * j];
        Block6& d = work.factor[j];
        // A_jj. Stretch-shear (rows 0-2, world space): through both points and the orientation,
        // compliant along the segment by `stretch` and all but rigid across it (a trace of
        // compliance keeps the system positive definite where both points are held). Bend-twist
        // (rows 3-5, its frame's axes): through this orientation and the one before (the root's
        // frame holds still). The two coupled through this orientation.
        const F4 l = maxOf(s.length, all(1e-8f));
        const F4 wBefore = j > 1 ? seg[j - 1].w : zero;
        const F4 WBefore = j > 1 ? seg[j - 1].W : zero;
        const V3& d3 = work.d3[j];
        const Axes3& f = work.frame[j];
        const F4 across = (s.w + wBefore) / (l * l) + all(1e-4f) * s.W;
        const F4 dd[3] = {d3.x, d3.y, d3.z};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) d.m[r][c] = (r == c ? across + s.W : zero) + (s.stretch - s.W) * dd[r] * dd[c];
        }
        for (int k = 0; k < 3; ++k) {
            const V3 t = cross(d3, f.c[k]) * (all(0.5f) * s.W);
            d.m[0][3 + k] = d.m[3 + k][0] = t.x;
            d.m[1][3 + k] = d.m[3 + k][1] = t.y;
            d.m[2][3 + k] = d.m[3 + k][2] = t.z;
        }
        const F4 turn = all(0.25f) * (s.W + WBefore);
        const F4 comp[3] = {s.bend, s.bend, s.twist};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) d.m[3 + r][3 + c] = r == c ? turn + maxOf(zero, comp[r]) : zero;
        }
        for (int r = 0; r < 6; ++r) {
            for (int c = 0; c < 6; ++c) d.m[r][c] = d.m[r][c] * (mask[r] * mask[c]);
            d.m[r][r] = select(mask[r] == zero, one, d.m[r][r]);
        }
        if (j > 1) {
            // X_j = D_{j-1}^-1 U_{j-1}, and D_j -= U_{j-1}^T X_j.
            const Block6& u = work.u[j - 1];
            Block6& x = work.x[j];
            x = u;
            choleskySolve6(work.factor[j - 1], &work.inv[6 * (j - 1)], x);
            for (int k = 0; k < 6; ++k) {
                for (int r = 0; r < 6; ++r) {
                    const F4 ukr = u.m[k][r];
                    for (int c = 0; c < 6; ++c) d.m[r][c] = d.m[r][c] - ukr * x.m[k][c];
                }
            }
        }
        cholesky6(d, &work.inv[6 * j]);
        if (j + 1 >= n) continue;
        // U_j: segment j's rows against segment j + 1's, through segment j's point (stretch-shear
        // with stretch-shear) and orientation (stretch-shear and bend-twist with bend-twist).
        Block6& u = work.u[j];
        const RodSegment4& b = seg[j + 1];
        const Axes3& fb = work.frame[j + 1];
        const F4 ss = -s.w / (l * maxOf(b.length, all(1e-8f)));
        for (int r = 0; r < 6; ++r) {
            for (int c = 0; c < 6; ++c) u.m[r][c] = zero;
        }
        for (int r = 0; r < 3; ++r) u.m[r][r] = ss;
        for (int k = 0; k < 3; ++k) {
            const V3 t = cross(d3, fb.c[k]) * (all(-0.5f) * s.W);
            u.m[0][3 + k] = t.x;
            u.m[1][3 + k] = t.y;
            u.m[2][3 + k] = t.z;
            for (int r = 0; r < 3; ++r) u.m[3 + r][3 + k] = all(-0.25f) * s.W * dot(f.c[r], fb.c[k]);
        }
        const F4* next = &work.mask[6 * (j + 1)];
        for (int r = 0; r < 6; ++r) {
            for (int c = 0; c < 6; ++c) u.m[r][c] = u.m[r][c] * (mask[r] * next[c]);
        }
    }
}

// One iteration of the direct solve over the guides (rodFactor's systems): points p[0..n-1] (p[0]
// the root, pinned), orientations q[1..n-1], the multipliers (6 per segment, at 6 j).
void rodIteration(std::size_t n, const Q4& rootRot, RodWork& work) {
    const F4 zero = all(0.0f);
    V3* p = work.p.data();
    Q4* q = work.q.data();
    const Q4* rest = work.rest.data();
    const RodSegment4* seg = work.seg.data();
    F4* lambda = work.lambda.data();
    // The errors as they are now. Stretch-shear: (p_j - p_{j-1}) / l - d3. Bend-twist: the exact
    // turn from the groomed frame to the segment's, as a rotation vector of half its angle in that
    // frame (x and y bend, z twist), on the shorter arc: exact for a guide pushed far from its
    // shape. Down the guide with them: g_j = b_j - U_{j-1}^T y_{j-1}, y_j = D_j^-1 g_j (in rhs).
    for (std::size_t j = 1; j < n; ++j) {
        const RodSegment4& s = seg[j];
        const F4* mask = &work.mask[6 * j];
        F4* g = &work.rhs[6 * j];
        const F4* lam = &lambda[6 * j];
        const M4 has = s.length > all(1e-8f);  // (else no constraints: its rows 0)
        const F4 eps = all(1e-4f) * s.W;       // (the trace of compliance across)
        const V3 gamma = (p[j] - p[j - 1]) / s.length - thirdAxis(q[j]);
        const V3& d3 = work.d3[j];
        const F4 along = s.stretch * (d3.x * lam[0] + d3.y * lam[1] + d3.z * lam[2]);
        const F4 g0 = -gamma.x - eps * lam[0] - along * d3.x;
        const F4 g1 = -gamma.y - eps * lam[1] - along * d3.y;
        const F4 g2 = -gamma.z - eps * lam[2] - along * d3.z;
        Q4 e = conj(rodTarget(j, q, rootRot, rest)) * q[j];
        const M4 flip = e.w < zero;
        e = {select(flip, -e.w, e.w), select(flip, -e.x, e.x), select(flip, -e.y, e.y), select(flip, -e.z, e.z)};
        const V3 u = vec(e);
        const V3 c = u * halfAngleOver4(length(u), e.w, bits(has));
        g[0] = select(has, g0, zero);
        g[1] = select(has, g1, zero);
        g[2] = select(has, g2, zero);
        g[3] = select(has, mask[3] * (-c.x - s.bend * lam[3]), zero);
        g[4] = select(has, mask[4] * (-c.y - s.bend * lam[4]), zero);
        g[5] = select(has, mask[5] * (-c.z - s.twist * lam[5]), zero);
        if (j > 1) {
            const Block6& ub = work.u[j - 1];
            const F4* y = &work.rhs[6 * (j - 1)];
            for (int k = 0; k < 6; ++k) {
                for (int r = 0; r < 6; ++r) g[r] = g[r] - ub.m[k][r] * y[k];
            }
        }
        choleskySolve6(work.factor[j], &work.inv[6 * j], g);
    }
    // Back up the guide: dlambda_j = y_j - X_{j+1} dlambda_{j+1}.
    for (std::size_t j = n - 1; j >= 2; --j) {
        const F4* after = &work.rhs[6 * j];
        F4* y = &work.rhs[6 * (j - 1)];
        const Block6& x = work.x[j];
        for (int r = 0; r < 6; ++r) {
            F4 sum = zero;
            for (int c = 0; c < 6; ++c) sum = sum + x.m[r][c] * after[c];
            y[r] = y[r] - sum;
        }
    }
    for (std::size_t k = 6; k < 6 * n; ++k) lambda[k] = lambda[k] + work.rhs[k];
    // The corrections, each point and orientation by its own inverse mass: dp = w J_p^T dlambda,
    // and a turn in world space of W J_theta^T dlambda.
    // Each segment's multipliers move its tip point and turn its orientation, and the segment
    // before by as much the other way: each such share is made once, for both.
    const F4 half = all(0.5f);
    auto ownMove = [&](std::size_t j) {
        const F4* dl = &work.rhs[6 * j];
        return V3{dl[0], dl[1], dl[2]} / maxOf(seg[j].length, all(1e-8f));
    };
    auto ownTurn = [&](std::size_t j) {
        const F4* dl = &work.rhs[6 * j];
        const Axes3& f = work.frame[j];
        return (f.c[0] * dl[3] + f.c[1] * dl[4] + f.c[2] * dl[5]) * half;
    };
    V3 move = ownMove(1), bend = ownTurn(1);
    for (std::size_t j = 1; j < n; ++j) {
        const RodSegment4& s = seg[j];
        const F4* dl = &work.rhs[6 * j];
        const V3 ss{dl[0], dl[1], dl[2]};
        V3 dp = move;
        V3 turn = cross(ss, work.d3[j]) + bend;
        if (j + 1 < n) {
            move = ownMove(j + 1);
            bend = ownTurn(j + 1);
            dp = dp - move;
            turn = turn - bend;
        }
        p[j] = select(s.w > zero, p[j] + dp * s.w, p[j]);
        q[j] = normalized(turnOfHalfAngle4(turn * (half * s.W)) * q[j]);
    }
}

// One guide's rod for a substep (rodSolve): its points (p[0] the root) and the groomed shape they
// are pulled toward, its orientations (segment j's at j, in the solver's array), rest turns, root
// frame, segments and pull per point.
struct RodLane {
    Vec3f* p = nullptr;
    const Vec3f* tgt = nullptr;
    Quat* q = nullptr;
    const Quat* rest = nullptr;
    Quat rootRot;
    const RodSegment* seg = nullptr;
    const float* attract = nullptr;
};

// The rods of `count` guides of n points (1 to 4) over a substep's iterations: each iteration the
// direct solve, then the pull toward the groomed shape (root stiffness) along the length. The
// points and orientations are put back where the lanes came from.
void rodSolve(std::size_t n, const RodLane* lanes, int count, int iterations, RodWork& work) {
    const RodLane* l[kLanes];
    for (int k = 0; k < kLanes; ++k) l[k] = &lanes[k < count ? k : 0];
    auto v3Of = [&](auto point) {
        const Vec3f a = point(*l[0]), b = point(*l[1]), c = point(*l[2]), d = point(*l[3]);
        return V3{lanesOf(a.x, b.x, c.x, d.x), lanesOf(a.y, b.y, c.y, d.y), lanesOf(a.z, b.z, c.z, d.z)};
    };
    auto q4Of = [&](auto quat) {
        const Quat a = quat(*l[0]), b = quat(*l[1]), c = quat(*l[2]), d = quat(*l[3]);
        return Q4{lanesOf(a.w, b.w, c.w, d.w), lanesOf(a.x, b.x, c.x, d.x), lanesOf(a.y, b.y, c.y, d.y),
                  lanesOf(a.z, b.z, c.z, d.z)};
    };
    auto f4Of = [&](auto value) { return lanesOf(value(*l[0]), value(*l[1]), value(*l[2]), value(*l[3])); };
    work.p.resize(n);
    work.tgt.resize(n);
    work.q.resize(n);
    work.rest.resize(n);
    work.seg.resize(n);
    work.attract.resize(n);
    work.lambda.assign(6 * n, all(0.0f));
    for (std::size_t j = 0; j < n; ++j) {
        work.p[j] = v3Of([j](const RodLane& r) { return r.p[j]; });
        work.tgt[j] = v3Of([j](const RodLane& r) { return r.tgt[j]; });
        work.attract[j] = f4Of([j](const RodLane& r) { return r.attract[j]; });
        work.q[j] = q4Of([j](const RodLane& r) { return r.q[j]; });
        work.rest[j] = q4Of([j](const RodLane& r) { return r.rest[j]; });
        RodSegment4& s = work.seg[j];
        s.length = f4Of([j](const RodLane& r) { return r.seg[j].length; });
        s.w = f4Of([j](const RodLane& r) { return r.seg[j].w; });
        s.W = f4Of([j](const RodLane& r) { return r.seg[j].W; });
        s.stretch = f4Of([j](const RodLane& r) { return r.seg[j].stretch; });
        s.bend = f4Of([j](const RodLane& r) { return r.seg[j].bend; });
        s.twist = f4Of([j](const RodLane& r) { return r.seg[j].twist; });
    }
    const Q4 rootRot = q4Of([](const RodLane& r) { return r.rootRot; });
    // The system, factored once for the substep's iterations (it changes with the orientations
    // alone, little over a substep: solving it again each iteration gave the same motion at twice
    // the cost).
    rodFactor(n, rootRot, work);
    const F4 zero = all(0.0f);
    for (int it = 0; it < iterations; ++it) {
        // Stretch-shear and bend-twist over the whole guide at once.
        rodIteration(n, rootRot, work);
        // The pull toward the groomed shape (root stiffness), along the length.
        for (std::size_t j = 1; j < n; ++j) {
            const F4 a = work.attract[j];
            work.p[j] = select(a > zero, work.p[j] + (work.tgt[j] - work.p[j]) * a, work.p[j]);
        }
    }
    for (std::size_t j = 1; j < n; ++j) {
        float px[kLanes], py[kLanes], pz[kLanes], qw[kLanes], qx[kLanes], qy[kLanes], qz[kLanes];
        store(work.p[j].x, px);
        store(work.p[j].y, py);
        store(work.p[j].z, pz);
        store(work.q[j].w, qw);
        store(work.q[j].x, qx);
        store(work.q[j].y, qy);
        store(work.q[j].z, qz);
        for (int k = 0; k < count; ++k) {
            lanes[k].p[j] = {px[k], py[k], pz[k]};
            lanes[k].q[j] = {qw[k], qx[k], qy[k], qz[k]};
        }
    }
}

// How fast a point under water soaks up (seconds to 1/e), and the current's gustiness (as Wind's gust).
constexpr float kSoakTime = 0.25f;
constexpr float kCurrentGust = 0.3f;

// A flow's velocity at `at` (already scaled by the gusts' frequency and moved with the flow): its
// speed varied by the gusts travelling with it and its direction swayed (Wind's gust field).
Vec3f flowVelocity(const Vec3f& at, const Vec3f& dir, float speed, float gust, std::uint64_t gustSeed,
                   std::uint64_t swaySeed) {
    const float g0 = gust > 0.0f ? fractalNoise(at, gustSeed, 2) : 0.0f;
    Vec3f d = dir;
    if (gust > 0.0f) {
        const Vec3f swayed = dir + vectorNoise(at + Vec3f{17.3f, -4.1f, 9.7f}, swaySeed, 1) * (0.35f * gust);
        if (swayed.length() > 1e-6f) d = swayed.normalized();
    }
    return d * (speed * std::max(0.0f, 1.0f + gust * 1.5f * g0));
}

}  // namespace

// ---- Settings ---------------------------------------------------------------------------------

SimSettings SimSettings::fromParams(const ParamSet& p) {
    SimSettings s;
    auto f = [&](const char* name, float fallback) { return static_cast<float>(p.getDouble(name, fallback)); };
    s.stiffness = f("stretchStiffness", s.stiffness);
    s.bend = f("bend", s.bend);
    s.twist = f("twist", s.twist);
    s.damping = f("damping", s.damping);
    s.drag = f("drag", s.drag);
    s.gravity = f("gravity", s.gravity);
    s.gravityDirection = p.getVec3("gravityDirection", s.gravityDirection);
    const std::string underGravity = p.getString("groomedUnderGravity", "preload");
    s.groomedUnderGravity = underGravity != "off";
    s.preload = underGravity == "preload";
    s.windSpeed = f("windSpeed", s.windSpeed);
    s.windDirection = p.getVec3("windDirection", s.windDirection);
    s.windGust = f("windGust", s.windGust);
    s.windFrequency = f("windFrequency", s.windFrequency);
    s.rootStiffnessProfile = p.getRamp("rootStiffnessProfile", s.rootStiffnessProfile);
    s.inertia = f("inertia", s.inertia);
    s.collideScalp = p.getBool("collideScalp", s.collideScalp);
    s.collisionOffset = f("collisionOffset", s.collisionOffset);
    s.friction = f("friction", s.friction);
    s.repulsion = f("repulsion", s.repulsion);
    s.repulsionRadius = f("repulsionRadius", s.repulsionRadius);
    s.substeps = static_cast<int>(p.getInt("substeps", s.substeps));
    s.iterations = static_cast<int>(p.getInt("iterations", s.iterations));
    s.startFrame = static_cast<int>(p.getInt("startFrame", s.startFrame));
    s.preroll = std::max(0, static_cast<int>(p.getInt("preroll", 0)));
    s.seed = static_cast<std::uint64_t>(p.getInt("seed", 0));
    s.live = p.getString("cacheMode", "live") != "cached";
    s.water = p.getBool("water", s.water);
    s.waterLevel = f("waterLevel", s.waterLevel);
    s.buoyancy = f("buoyancy", s.buoyancy);
    s.waterDrag = f("waterDrag", s.waterDrag);
    s.currentSpeed = f("currentSpeed", s.currentSpeed);
    s.currentDirection = p.getVec3("currentDirection", s.currentDirection);
    s.dryTime = f("dryTime", s.dryTime);
    s.wetWeight = f("wetWeight", s.wetWeight);
    s.rain = f("rain", s.rain);
    s.rootStiffness = f("rootStiffness", s.rootStiffness);
    s.mass = f("mass", s.mass);
    s.strength = f("strength", s.strength);
    s.volume = f("volume", s.volume);
    s.volumeCellSize = f("volumeCellSize", s.volumeCellSize);
    // The maps set on the mappable params (their numbers above are then the specs' defaults).
    std::uint64_t maps = 0;
    for (const std::string& name : simulationMapParams()) {
        if (!p.has(name)) continue;
        const MapRef* map = std::get_if<MapRef>(&p.get(name));
        if (!map) continue;
        std::uint64_t h = hashString(hashCombine(hash64(0x3A95), static_cast<std::uint64_t>(map->source)), name);
        h = hashFloat(hashFloat(hashString(hashCombine(h, static_cast<std::uint64_t>(map->component)), map->name), map->min),
                      map->max);
        for (const auto& [constant, value] : map->constants) h = hashFloat(hashString(h, constant), value);
        maps = hashCombine(maps == 0 ? hash64(0x3A96) : maps, h);
    }
    s.mapsKey = maps;
    return s;
}

std::uint64_t SimSettings::hash() const {
    std::uint64_t h = hash64(0x51A7);
    for (const float v : {stiffness, bend, twist, damping, drag, gravity, gravityDirection.x, gravityDirection.y,
                          gravityDirection.z, windSpeed, windDirection.x, windDirection.y, windDirection.z, windGust,
                          windFrequency, inertia, collisionOffset, friction, repulsion, repulsionRadius}) {
        h = hashFloat(h, v);
    }
    h = hashCombine(h, rootStiffnessProfile.straight ? 1 : 0);
    for (const auto& [t, v] : rootStiffnessProfile.keys) h = hashFloat(hashFloat(h, t), v);
    h = hashCombine(h, collideScalp ? 1 : 0);
    h = hashCombine(hashCombine(hashCombine(h, static_cast<std::uint64_t>(substeps)), static_cast<std::uint64_t>(iterations)),
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(startFrame)));
    h = hashCombine(h, seed);
    // Per point settings and volume, only when set (else the hash made before them).
    if (perPointSettings() || volume > 0.0f || mapsKey != 0) {
        h = hashCombine(h, 0x9E7A);
        for (const float v : {rootStiffness, mass, strength, volume, volume > 0.0f ? volumeCellSize : 0.0f}) {
            h = hashFloat(h, v);
        }
        h = hashCombine(h, mapsKey);
    }
    if (preroll > 0) h = hashCombine(hashCombine(h, 0x9E40), static_cast<std::uint64_t>(preroll));  // only when on
    // Only when on (else the hash made before it).
    if (groomedUnderGravity) h = hashCombine(h, 0x6A7E);
    if (groomedUnderGravity && preload) h = hashCombine(h, 0x9E10);
    // Dry: the hash made before water (the stamps of bakes made then still match).
    if (!wetting()) return h;
    h = hashCombine(h, water ? 0x3A7E : 0x3A7F);
    for (const float v : {waterLevel, buoyancy, waterDrag, currentSpeed, currentDirection.x, currentDirection.y,
                          currentDirection.z, dryTime, wetWeight, rain}) {
        h = hashFloat(h, v);
    }
    return h;
}

std::string simulationExternal(const std::string& node) { return "sim:" + node; }

std::string formatSimulationStamp(std::uint64_t stamp) {
    static const char* kDigits = "0123456789abcdef";
    std::string text(16, '0');
    for (int k = 15; k >= 0; --k, stamp >>= 4) text[static_cast<std::size_t>(k)] = kDigits[stamp & 0xF];
    return text;
}

std::uint64_t parseSimulationStamp(const std::string& text) {
    if (text.empty() || text.size() > 16) return 0;
    std::uint64_t stamp = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) return 0;
        stamp = (stamp << 4) | static_cast<std::uint64_t>(digit);
    }
    return stamp;
}

// ---- Maps -------------------------------------------------------------------------------------

const std::vector<std::string>& simulationMapParams() {
    static const std::vector<std::string> kNames{"stretchStiffness", "bend", "twist", "damping",
                                                 "drag", "rootStiffness", "mass", "strength"};
    return kNames;
}

std::uint64_t SimMaps::hash() const {
    std::uint64_t h = hash64(0x3A97);
    for (const std::vector<float>* values : {&stiffness, &bend, &twist, &damping, &drag, &rootStiffness, &mass, &strength}) {
        h = hashCombine(h, values->size());
        // FNV-1a over the values' bits, two at a time (a hashCombine per value costs more than the maps).
        std::uint64_t f = 1469598103934665603ull;
        const std::size_t n = values->size();
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            std::uint64_t word = 0;
            std::memcpy(&word, values->data() + i, sizeof word);
            f = (f ^ word) * 1099511628211ull;
        }
        if (n % 2 == 1) f = hashFloat(f, values->back());
        h = hashCombine(h, f);
    }
    return h;
}

SimMapsPtr simulationMaps(const ParamSet& params, const GuideSet& guides, const MeshPtr& scalp) {
    const std::vector<std::string>& names = simulationMapParams();
    auto mapped = [&](const std::string& name) {
        return params.field(name) != nullptr || (params.has(name) && std::get_if<MapRef>(&params.get(name)) != nullptr);
    };
    if (std::none_of(names.begin(), names.end(), mapped) || guides.empty()) return nullptr;
    const StrandsPtr strands = guidesAsStrands(guides);
    auto maps = std::make_shared<SimMaps>();
    std::vector<float>* out[] = {&maps->stiffness, &maps->bend, &maps->twist, &maps->damping,
                                 &maps->drag, &maps->rootStiffness, &maps->mass, &maps->strength};
    for (std::size_t k = 0; k < names.size(); ++k) {
        if (!mapped(names[k])) continue;
        // On the guides as strands: their layers are its channels (no guide set to blend from).
        const ScalarField field(params, names[k], scalp.get());
        field.validate(*strands);
        const bool perPoint = field.perPoint();
        std::vector<float>& values = *out[k];
        values.resize(strands->pointCount());
        parallelFor(0, strands->strandCount(), [&](std::size_t g) {
            const std::size_t b = strands->strandBegin(g), n = strands->strandPointCount(g);
            const float atRoot = perPoint ? 0.0f : field.at(*strands, g);
            for (std::size_t j = 0; j < n; ++j) values[b + j] = perPoint ? field.atPoint(*strands, g, j) : atRoot;
        }, 64);
    }
    return maps;
}

// ---- The solver -------------------------------------------------------------------------------

// Per point arrays hold every guide's points one after another (guide g: begin[g] to begin[g+1]).
// A segment's data sits at its tip point's index (segment s of guide g: begin[g] + s + 1), so the
// root's slot holds none.
struct GuideSolver::Impl {
    // One frame's inputs, per guide and point.
    struct Targets {
        std::vector<RootPose> roots;  // each guide's root frame
        std::vector<Vec3f> local;     // each point of the groomed shape in its root's frame
        std::vector<float> lengths;   // each segment's length
        std::vector<Quat> rest;       // each segment's rest Darboux quaternion against the one before (the root's frame)
        std::vector<float> invMassQ;  // each segment's orientation's inverse mass
    };

    SimSettings settings;
    double fps = 24.0;
    int frame = 0;
    bool started = false;
    std::vector<std::uint64_t> ids;
    std::vector<StrandRoot> roots;
    std::vector<std::size_t> begin;  // guides + 1
    std::vector<std::uint32_t> guideOf;  // per point
    std::vector<int> edges;          // each root's face edge its frame's x runs along (-1: not on the scalp)
    std::vector<float> attract;      // per point: the pull toward the groomed shape per iteration
    std::vector<float> rootRamp;     // per point: rootStiffnessProfile there (before its scale and clamp)
    std::vector<Vec3f> x, v;
    // Per point settings (maps, or mass, strength or rootStiffness off their defaults), made
    // for each solve from its inputs; unused (empty) otherwise, the solver then as it was.
    struct PointSettings {
        std::vector<float> stretch;          // the length kept per step (shareOver)
        std::vector<float> stretchCompliance;  // the stretch row's, per step
        std::vector<float> bend;             // 0-1
        std::vector<float> bendCompliance;   // per step (negative: none)
        std::vector<float> twistCompliance;  // per step (negative: none)
        std::vector<float> damp;             // the motion kept per step
        std::vector<float> drag;             // the motion against the air kept per step (heavier: more)
        std::vector<float> attract;          // the pull toward the groomed shape per iteration
        std::vector<float> invMass;          // 1 / mass; 0: held to the input shape (strength 0)
        std::vector<float> perMass;          // 1 / mass (held or not)
        std::vector<float> weight;           // strength, 0-1
        bool blends = false;                 // some weight between 0 and 1: the frames blend
    };
    PointSettings ps;
    bool custom = false;
    std::vector<Vec3f> frameTarget;  // each point's input shape at the last frame's end (blending)
    std::vector<Vec3f> shown;        // the points as the frames show them, while blending (else empty)
    std::vector<Quat> q;
    std::vector<float> wet;  // per point, 0-1, while the settings wet the hair (else empty)
    Targets prev, next;
    std::vector<float> length;  // each guide's length as groomed at the start
    std::vector<Vec3f> restGravity;  // groomedUnderGravity: each guide's gravity in its root's frame at the start
    std::vector<Quat> preload;       // groomedUnderGravity with preload: each segment's turn on its rest turn
    float longest = 0.0f;
    // The scalp's rest pose (collisions through its field) and each root's frame on it.
    MeshPtr restScalp;
    std::uint64_t restKey = 0;
    std::vector<RootPose> restRoots;
    std::vector<char> restRooted;
    // The colliders of the last inputs (a moving one is swept from there over the next substeps).
    std::vector<MeshPtr> lastColliders;

    std::size_t guideCount() const { return ids.size(); }

    // A guide's root frame on `scalp`, or from its own first segment when it has no root there.
    RootPose rootPose(std::size_t g, const Guide& guide, const ScalpMesh* scalp) const {
        if (scalp && edges[g] >= 0 && scalp->contains(guide.root)) {
            const RootFrame f = rootFrameOn(*scalp, guide.root, edges[g]);
            if (f.valid) return {f.origin, fromAxes(f.x, f.y, f.z)};
        }
        if (guide.points.empty()) return {Vec3f{}, Quat{}};  // nothing to stand on: at the origin
        const Vec3f p0 = guide.points.front();
        Vec3f up = guide.points.size() > 1 ? (guide.points[1] - p0).normalized() : Vec3f{0.0f, 1.0f, 0.0f};
        if (up.length() < 0.5f) up = {0.0f, 1.0f, 0.0f};
        const Vec3f across = anyPerpendicular(up);
        return {p0, fromAxes(across, up, across.cross(up))};
    }

    // The inputs as targets: root frames, the groomed shapes in them, lengths, rest Darboux.
    void targetsOf(const SimInputs& in, Targets& t) const {
        const std::size_t count = guideCount(), points = x.size();
        t.roots.resize(count);
        t.local.resize(points);
        t.lengths.assign(points, 0.0f);
        t.rest.assign(points, Quat{});
        t.invMassQ.assign(points, 0.0f);
        const ScalpMesh* scalp = in.scalp.get();
        const auto& guides = in.guides->guides();
        parallelFor(0, count, [&](std::size_t g) {
            const Guide& guide = guides[g];
            const RootPose root = rootPose(g, guide, scalp);
            t.roots[g] = root;
            const std::size_t b = begin[g], n = begin[g + 1] - b;
            for (std::size_t j = 0; j < n; ++j) t.local[b + j] = root.toLocal(guide.points[j]);
            if (n < 2) return;
            // Segment frames transported down the groomed shape from the root's x axis.
            const Vec3f rootX = rotate(root.rot, {1.0f, 0.0f, 0.0f});
            Quat before = root.rot;
            Vec3f d1, d3Before;
            for (std::size_t s = 0; s + 1 < n; ++s) {
                const Vec3f seg = guide.points[s + 1] - guide.points[s];
                const float len = seg.length();
                Vec3f d3 = len > 1e-12f ? seg / len : (s > 0 ? d3Before : rotate(root.rot, {0.0f, 1.0f, 0.0f}));
                if (s == 0) {
                    d1 = rootX - d3 * rootX.dot(d3);
                    if (d1.length() < 1e-4f) d1 = anyPerpendicular(d3);
                } else {
                    d1 = rotateBetween(d1, d3Before, d3);
                    d1 = d1 - d3 * d1.dot(d3);
                    if (d1.length() < 1e-6f) d1 = anyPerpendicular(d3);
                }
                d1 = d1.normalized();
                const Quat frame = fromAxes(d1, d3.cross(d1), d3);
                const std::size_t i = b + s + 1;
                t.lengths[i] = len;
                t.rest[i] = conj(before) * frame;
                const float l = std::max(len, 1e-4f);
                t.invMassQ[i] = 1.0f / (4.0f * l * l);
                before = frame;
                d3Before = d3;
            }
        });
    }

    // Gravity preloading: each segment's rest turn turned so that, in the groomed shape, its bend
    // and twist rows carry the weight of the hair beyond it (the torque of gravity on its points
    // about the segment's first point). The rows (rodIteration) measure the turn from the rest
    // turn to the segment's as a rotation vector of half its angle, r, in the rest turn's frame,
    // and push back along that frame's axes by r / (2 a) for a physical compliance a per row
    // (complianceOf times the reference step squared, over the orientation's 4 l^2; bend across,
    // twist along): the rest turn's own turn is the half-angle vector -2 a t, t the torque in its
    // own frame (a few fixed-point steps: that frame is turned by it). Gravity as a still point
    // feels it per substep: through the drag and damping its velocity takes. From the start's
    // groomed shape and frames (q). Without twist or bend rows (0) nothing keeps a preloaded strand
    // from rolling or folding: none. Two limits on the share of its weight a guide takes (the rest
    // cancelled as without preload: restGravity, scaled): joints turned far off their rest turn
    // are not stable at a few substeps (a guide whose heaviest joint would turn more than
    // kMostPreload takes that share), and hair standing up buckles under a weight its bend cannot
    // hold upright: a joint with the hair beyond above it has gravity's moment there (sum of
    // m (p - joint) against g) taking from its stiffness (l^2 / bendC per radian); at most
    // kMostBuckle of it (the joint's own share: the chain buckles under less).
    void preloadRest() {
        const SimSettings& s = settings;
        const float h = static_cast<float>(1.0 / (fps * s.substeps));
        const Vec3f gravity = s.gravityDirection.length() > 1e-8f ? s.gravityDirection.normalized() * s.gravity : Vec3f{};
        const Vec3f g = gravity * (pm::exp(-10.0f * s.drag * h) * pm::exp(-20.0f * s.damping * h));
        const float bendC = s.bend > 0.0f ? complianceOf(s.bend) * kReferenceStep * kReferenceStep : -1.0f;
        const float twistC = s.twist > 0.0f ? complianceOf(s.twist) * kReferenceStep * kReferenceStep : -1.0f;
        const float mass = std::max(s.mass, 1e-6f);
        preload.assign(x.size(), Quat{});
        restGravity.resize(guideCount());
        // The rest turn's turn (as a half-angle vector) for torque t, in the segment's frame, on a
        // segment l^2 long (rows of compliance 0 hold any torque: none on them).
        auto turnFor = [&](const Vec3f& t, float l2) {
            const Vec3f a{bendC / (4.0f * l2), bendC / (4.0f * l2), twistC / (4.0f * l2)};
            Vec3f r{};
            for (int k = 0; k < 4; ++k) {
                const Vec3f there = rotate(conj(turnOfHalfAngle(r)), t);  // in the rest turn's frame
                r = Vec3f{a.x * there.x, a.y * there.y, a.z * there.z} * -2.0f;
            }
            return r;
        };
        parallelFor(0, guideCount(), [&](std::size_t gi) {
            const std::size_t b = begin[gi], n = begin[gi + 1] - b;
            if (twistC < 0.0f || bendC < 0.0f) {  // (no twist or bend rows: weightless)
                restGravity[gi] = rotate(conj(prev.roots[gi].rot), gravity);
                return;
            }
            thread_local std::vector<Vec3f> torques;
            torques.assign(n, Vec3f{});
            Vec3f moment{};  // the points beyond, weighted: sum of m p
            float weight = 0.0f, most = 0.0f, buckle = 0.0f;
            for (std::size_t j = n; j-- > 1;) {
                const std::size_t i = b + j;
                moment += x[i] * mass;
                weight += mass;
                const Vec3f arm = moment - x[i - 1] * weight;
                torques[j] = rotate(conj(q[i]), arm.cross(g));  // in the segment's frame (z along it)
                const float l2 = std::max(prev.lengths[i] * prev.lengths[i], 1e-8f);
                most = std::max(most, 2.0f * turnFor(torques[j], l2).length());  // the turn's angle
                buckle = std::max(buckle, -arm.dot(g) * bendC / l2);
            }
            float share = most > kMostPreload ? kMostPreload / most : 1.0f;
            if (buckle > kMostBuckle) share = std::min(share, kMostBuckle / buckle);
            for (std::size_t j = 1; j < n; ++j) {
                const std::size_t i = b + j;
                preload[i] = turnOfHalfAngle(turnFor(torques[j] * share, std::max(prev.lengths[i] * prev.lengths[i], 1e-8f)));
            }
            restGravity[gi] = rotate(conj(prev.roots[gi].rot), gravity) * (1.0f - share);
        });
    }
    void preloaded(Targets& t) const {
        if (preload.empty()) return;
        for (std::size_t i = 0; i < t.rest.size(); ++i) t.rest[i] = normalized(t.rest[i] * preload[i]);
    }

    // The segment frames of the targets in world space (start: the rods' first orientations).
    void framesFromTargets(const Targets& t) {
        for (std::size_t g = 0; g < guideCount(); ++g) {
            const std::size_t b = begin[g], n = begin[g + 1] - b;
            Quat before = t.roots[g].rot;
            for (std::size_t s = 0; s + 1 < n; ++s) {
                const std::size_t i = b + s + 1;
                before = normalized(before * t.rest[i]);
                q[i] = before;
            }
        }
    }

    // The pose collisions with the scalp are asked in: its rest pose, else the scalp as it was at
    // the start (a deforming scalp without one: a cache), whose distance field is made once; each
    // point is taken there by its root's motion.
    void rootRestFrames(const MeshPtr& scalp) {
        restScalp.reset();
        restKey = 0;
        if (!scalp || scalp->triangleCount() == 0) return;
        restKey = scalp->hasRest() ? scalp->restPoseHash() : 0;
        restScalp = scalp->hasRest() ? std::make_shared<const ScalpMesh>(scalp->restPose()) : scalp;
        restRoots.assign(guideCount(), RootPose{});
        restRooted.assign(guideCount(), 0);
        for (std::size_t g = 0; g < guideCount(); ++g) {
            if (edges[g] < 0 || !restScalp->contains(roots[g])) continue;
            const RootFrame f = rootFrameOn(*restScalp, roots[g], edges[g]);
            if (!f.valid) continue;
            restRoots[g] = {f.origin, fromAxes(f.x, f.y, f.z)};
            restRooted[g] = 1;
        }
    }

    void solve(const SimInputs& in, int frames, const std::function<void(int)>& frameDone);
};

GuideSolver::GuideSolver() : impl_(std::make_unique<Impl>()) {}
GuideSolver::~GuideSolver() = default;

bool GuideSolver::started() const { return impl_->started; }
int GuideSolver::frame() const { return impl_->frame; }
const std::vector<Vec3f>& GuideSolver::points() const { return impl_->shown.empty() ? impl_->x : impl_->shown; }
const std::vector<float>& GuideSolver::wetness() const { return impl_->wet; }

bool GuideSolver::sameLayout(const GuideSet& guides) const {
    const Impl& m = *impl_;
    if (!m.started || guides.size() != m.guideCount()) return false;
    for (std::size_t g = 0; g < m.guideCount(); ++g) {
        const Guide& guide = guides.guides()[g];
        if (guide.id != m.ids[g] || guide.points.size() != m.begin[g + 1] - m.begin[g]) return false;
    }
    return true;
}

void GuideSolver::start(const SimSettings& settings, double framesPerSecond, int frame, const SimInputs& inputs) {
    Impl& m = *impl_;
    m = Impl{};
    m.settings = settings;
    m.fps = framesPerSecond > 0.0 ? framesPerSecond : 24.0;
    m.frame = frame;
    m.started = true;
    static const GuideSet kNone;
    const GuideSet& guides = inputs.guides ? *inputs.guides : kNone;
    const std::size_t count = guides.size();
    m.ids.resize(count);
    m.roots.resize(count);
    m.begin.assign(count + 1, 0);
    m.edges.assign(count, -1);
    m.length.assign(count, 0.0f);
    for (std::size_t g = 0; g < count; ++g) {
        const Guide& guide = guides.guides()[g];
        m.ids[g] = guide.id;
        m.roots[g] = guide.root;
        m.begin[g + 1] = m.begin[g] + guide.points.size();
        if (inputs.scalp && inputs.scalp->contains(guide.root)) m.edges[g] = longestEdge(*inputs.scalp, guide.root);
    }
    const std::size_t points = m.begin[count];
    m.guideOf.resize(points);
    m.attract.assign(points, 0.0f);
    m.rootRamp.assign(points, 0.0f);
    m.x.resize(points);
    m.v.assign(points, Vec3f{});
    m.q.assign(points, Quat{});
    if (settings.wetting()) m.wet.assign(points, 0.0f);  // dry at the start
    // The pull toward the groomed shape: `rootStiffnessProfile` along the length is the share a 24 fps
    // frame takes (kReferenceFrame), spread over the substeps and iterations a frame at this rate has.
    const float slice = static_cast<float>(1.0 / (m.fps * settings.substeps * settings.iterations));
    for (std::size_t g = 0; g < count; ++g) {
        const Guide& guide = guides.guides()[g];
        const std::size_t b = m.begin[g], n = m.begin[g + 1] - b;
        float length = 0.0f;
        for (std::size_t j = 0; j < n; ++j) {
            m.guideOf[b + j] = static_cast<std::uint32_t>(g);
            m.x[b + j] = guide.points[j];
            if (j > 0) length += (guide.points[j] - guide.points[j - 1]).length();
            const float t = n > 1 ? static_cast<float>(j) / static_cast<float>(n - 1) : 0.0f;
            m.rootRamp[b + j] = settings.rootStiffnessProfile(t);
            const float a = std::clamp(m.rootRamp[b + j], 0.0f, 1.0f);
            m.attract[b + j] = shareOver(a, slice, kReferenceFrame);
        }
        m.length[g] = length;
        m.longest = std::max(m.longest, length);
    }
    if (count > 0) {
        m.targetsOf(inputs, m.prev);
        m.framesFromTargets(m.prev);
        if (settings.groomedUnderGravity && settings.preload) {
            m.preloadRest();
            m.preloaded(m.prev);
        } else if (settings.groomedUnderGravity) {
            const Vec3f gravity =
                settings.gravityDirection.length() > 1e-8f ? settings.gravityDirection.normalized() * settings.gravity : Vec3f{};
            m.restGravity.resize(count);
            for (std::size_t g = 0; g < count; ++g) m.restGravity[g] = rotate(conj(m.prev.roots[g].rot), gravity);
        }
    }
    m.rootRestFrames(inputs.scalp);
    m.lastColliders = inputs.colliders;
}

void GuideSolver::step(const SimInputs& inputs, int frames, const std::function<void(int)>& frameDone) {
    Impl& m = *impl_;
    if (!m.started) throw std::invalid_argument("GuideSolver::step: not started");
    if (!inputs.guides ? m.guideCount() != 0 : !sameLayout(*inputs.guides)) {
        throw std::invalid_argument("GuideSolver::step: other guides than the ones simulated");
    }
    m.solve(inputs, std::max(1, frames), frameDone);
}

void GuideSolver::Impl::solve(const SimInputs& in, int frames, const std::function<void(int)>& frameDone) {
    const std::size_t count = guideCount();
    if (count == 0) {
        for (int k = 1; k <= frames; ++k) {
            ++frame;
            if (frameDone) frameDone(k);
        }
        return;
    }
    targetsOf(in, next);
    preloaded(next);
    if (in.scalp && in.scalp->hasRest() && in.scalp->restPoseHash() != restKey) rootRestFrames(in.scalp);

    const SimSettings& s = settings;
    const int substeps = s.substeps * frames;
    const float h = static_cast<float>(1.0 / (fps * s.substeps));
    const Vec3f gravity = s.gravityDirection.length() > 1e-8f ? s.gravityDirection.normalized() * s.gravity : Vec3f{};
    const float dragFactor = pm::exp(-10.0f * s.drag * h);
    const float dampFactor = pm::exp(-20.0f * s.damping * h);
    const float carry = 1.0f - s.inertia;
    const float scale = (kReferenceStep / h) * (kReferenceStep / h);
    const float bendCompliance = s.bend > 0.0f ? complianceOf(s.bend) * scale : -1.0f;
    // Length keeping and repulsion: shares per reference substep (as bend's compliance is), so the
    // look holds at any frame rate and substeps. Friction is Coulomb's (a slide against how deep
    // the surface pushed: the same at any step).
    const float stretchShare = shareOver(s.stiffness, h, kReferenceStep);
    const float stretchCompliance = stretchComplianceOf(s.stiffness) * scale;
    const float frictionMu = frictionCoefficientOf(s.friction);
    const float repulsionShare = shareOver(s.repulsion, h, kReferenceStep);
    const float twistCompliance = s.twist > 0.0f ? complianceOf(s.twist) * scale : -1.0f;
    // The wind: Wind's gust field (a noise travelling with the air), read at each root.
    const Vec3f windDir = s.windDirection.length() > 1e-6f ? s.windDirection.normalized() : Vec3f{1.0f, 0.0f, 0.0f};
    const bool windy = s.windSpeed != 0.0f;
    const std::uint64_t gustSeed = hashCombine(s.seed, 0x6057), swaySeed = hashCombine(s.seed, 0x5A7);
    // Water and wetness, only while the settings wet the hair (dry, the solver runs as it did).
    const bool wetting = !wet.empty();
    const ScalpMesh* waterMesh = s.water && in.water && in.water->triangleCount() > 0 ? in.water.get() : nullptr;
    const float waterDragFactor = pm::exp(-60.0f * s.waterDrag * h);
    const Vec3f lift = gravity * -s.buoyancy;
    const float soak = 1.0f - pm::exp(-h / kSoakTime);
    const float rainSoak = 1.0f - pm::exp(-3.0f * s.rain * h);
    const float drying = 1.0f - pm::exp(-h / s.dryTime);
    const Vec3f currentDir = s.currentDirection.length() > 1e-6f ? s.currentDirection.normalized() : Vec3f{1.0f, 0.0f, 0.0f};
    const std::uint64_t currentGustSeed = hashCombine(s.seed, 0xC0E7), currentSwaySeed = hashCombine(s.seed, 0xC5A7);

    // Per point settings (simulation.h): the maps read on these inputs, or mass, strength or
    // rootStiffness off their defaults. Without any, none of this runs: the solver as it was.
    const SimMaps* maps = in.maps && !in.maps->empty() ? in.maps.get() : nullptr;
    custom = maps != nullptr || s.perPointSettings();
    ps = PointSettings{};
    if (custom) {
        const std::size_t points = x.size();
        auto mapOf = [&](const std::vector<float>& values) {
            return maps && values.size() == points ? values.data() : nullptr;
        };
        const float* mStiffness = maps ? mapOf(maps->stiffness) : nullptr;
        const float* mBend = maps ? mapOf(maps->bend) : nullptr;
        const float* mTwist = maps ? mapOf(maps->twist) : nullptr;
        const float* mDamping = maps ? mapOf(maps->damping) : nullptr;
        const float* mDrag = maps ? mapOf(maps->drag) : nullptr;
        const float* mRoot = maps ? mapOf(maps->rootStiffness) : nullptr;
        const float* mMass = maps ? mapOf(maps->mass) : nullptr;
        const float* mStrength = maps ? mapOf(maps->strength) : nullptr;
        for (std::vector<float>* values : {&ps.stretch, &ps.stretchCompliance, &ps.bend, &ps.bendCompliance, &ps.twistCompliance,
                                           &ps.damp, &ps.drag, &ps.attract, &ps.invMass, &ps.perMass, &ps.weight}) {
            values->resize(points);
        }
        // The pull toward the groomed shape per iteration (as start() spreads it), scaled.
        const float slice = static_cast<float>(1.0 / (fps * s.substeps * s.iterations));
        parallelFor(0, points, [&](std::size_t i) {
            const float stiffness = mStiffness ? std::clamp(mStiffness[i], 0.0f, 1.0f) : s.stiffness;
            const float bend = mBend ? std::clamp(mBend[i], 0.0f, 1.0f) : s.bend;
            const float twist = mTwist ? std::clamp(mTwist[i], 0.0f, 1.0f) : s.twist;
            const float damping = mDamping ? std::max(0.0f, mDamping[i]) : s.damping;
            const float drag = mDrag ? std::max(0.0f, mDrag[i]) : s.drag;
            const float rootScale = std::max(0.0f, mRoot ? mRoot[i] : s.rootStiffness);
            const float mass = std::max(1e-3f, mMass ? mMass[i] : s.mass);
            const float weight = std::clamp(mStrength ? mStrength[i] : s.strength, 0.0f, 1.0f);
            ps.stretch[i] = shareOver(stiffness, h, kReferenceStep);
            ps.stretchCompliance[i] = stretchComplianceOf(stiffness) * scale;
            ps.bend[i] = bend;
            ps.bendCompliance[i] = bend > 0.0f ? complianceOf(bend) * scale : -1.0f;
            ps.twistCompliance[i] = twist > 0.0f ? complianceOf(twist) * scale : -1.0f;
            ps.damp[i] = pm::exp(-20.0f * damping * h);
            ps.drag[i] = pm::exp(-10.0f * drag * h / mass);
            ps.attract[i] = rootScale == 1.0f ? attract[i]
                                              : shareOver(std::clamp(rootRamp[i] * rootScale, 0.0f, 1.0f), slice, kReferenceFrame);
            ps.weight[i] = weight;
            ps.perMass[i] = 1.0f / mass;
            ps.invMass[i] = weight > 0.0f ? ps.perMass[i] : 0.0f;
        }, 4096);
        ps.blends = std::any_of(ps.weight.begin(), ps.weight.end(), [](float w) { return w > 0.0f && w < 1.0f; });
    }
    if (ps.blends) {
        frameTarget.resize(x.size());
        shown.resize(x.size());
    } else {
        frameTarget.clear();
        shown.clear();
    }
    // A point held to its input shape (strength 0): kinematic, nothing moves it.
    auto held = [&](std::size_t i) { return custom && ps.invMass[i] == 0.0f; };

    // Collisions: the scalp through its rest pose's field (or the start frame's), each point taken
    // there by its root's motion; a guide without a root on it against the scalp as it is; the
    // colliders as they are. A scalp or collider as it is asks its field, or when it is deformed
    // away from its rest pose its rest pose's (CollisionSurface: made once for every frame). A
    // guide's points are pushed out from as deep as half its length (searched for that far); the
    // fields are made for the longest guide's (their reach that search rounded up to a power of
    // two: the same fields while guides elsewhere change, unless the longest crosses one).
    const float offset = s.collisionOffset;
    const float fieldDepth = std::max(offset * 2.0f, 0.5f * longest);
    const float fieldSearch = std::max(offset, fieldDepth) * 1.001f + 1e-6f;
    CollisionSurface scalpRest, scalpNow;
    if (s.collideScalp && in.scalp && in.scalp->triangleCount() > 0) {
        if (restScalp) scalpRest = CollisionSurface::of(restScalp, offset, fieldDepth, fieldSearch);
        scalpNow = CollisionSurface::of(in.scalp, offset, fieldDepth, fieldSearch);
    }
    std::vector<CollisionSurface> colliders;
    std::vector<float> colliderFriction;  // each one's friction coefficient (its own, else `friction`'s)
    // A collider that moved since the last inputs (the same mesh, other points) is met by each
    // substep where it is then, on the way from there (its points blended): one sweeping through
    // the hair pushes it along the way instead of appearing past it at the first substep. Those
    // substeps ask the collider's field (its rest pose's, else its own as it is now) through
    // RestPoseCollision, as a deformed collider does.
    struct Sweep {
        std::size_t surface;  // in `colliders`
        MeshPtr from, to;     // `to`: the mesh as it is now, with a rest pose (its own, else these points)
    };
    std::vector<Sweep> sweeps;
    for (std::size_t k = 0; k < in.colliders.size(); ++k) {
        MeshPtr now = in.colliders[k];
        const MeshPtr before = k < lastColliders.size() ? lastColliders[k] : nullptr;
        const bool sweeping = now && before && before != now && before->pointCount() == now->pointCount() &&
                              before->samePointsLayout(*now) && before->points() != now->points();
        if (sweeping && !now->hasRest()) {
            // Its own points as its rest pose: the substeps on the way and the last one ask one
            // field, this frame's.
            auto rested = std::make_shared<ScalpMesh>(*now);
            rested->setRestPoints(now->points());
            now = std::move(rested);
        }
        if (CollisionSurface c = CollisionSurface::of(now, offset, fieldDepth, fieldSearch)) {
            if (sweeping) sweeps.push_back({colliders.size(), before, now});
            colliders.push_back(std::move(c));
            const float own = k < in.colliderFriction.size() ? in.colliderFriction[k] : -1.0f;
            colliderFriction.push_back(own >= 0.0f ? frictionCoefficientOf(own) : frictionMu);
        }
    }
    const std::vector<CollisionSurface> collidersNow = sweeps.empty() ? std::vector<CollisionSurface>{} : colliders;
    // The moving colliders where they are at fraction a of the way (1: as they are).
    auto sweepTo = [&](float a) {
        for (const Sweep& sw : sweeps) {
            if (a >= 1.0f) {
                colliders[sw.surface] = collidersNow[sw.surface];
                continue;
            }
            const std::vector<Vec3f>& p0 = sw.from->points();
            const std::vector<Vec3f>& p1 = sw.to->points();
            std::vector<Vec3f> points(p1.size());
            parallelFor(0, points.size(), [&](std::size_t v) { points[v] = p0[v] + (p1[v] - p0[v]) * a; }, 8192);
            const MeshPtr mesh = std::make_shared<const ScalpMesh>(sw.to->withPoints(std::move(points)));
            CollisionSurface c = CollisionSurface::of(mesh, offset, fieldDepth, fieldSearch);
            if (c) colliders[sw.surface] = std::move(c);
        }
    };

    // A guide's substep under way: what its first part (substepBegin) leaves for the rod solve and
    // the part after it (substepEnd).
    struct GuideStep {
        std::size_t g = 0, b = 0, n = 0;
        RootPose r1;
        std::vector<Vec3f> p, tgt, corr;
        // Each point where the scalp under it carries it over the substep (its root's motion): what
        // friction against the scalp holds it to (guides rooted on the scalp's rest pose only).
        std::vector<Vec3f> onScalp;
        std::vector<float> under;
        std::vector<RodSegment> rod;
    };
    // A substep of guide g, from fraction a0 to a1 of the way between the last inputs and these, up
    // to its rod's solve: false for a guide without segments (done then).
    auto substepBegin = [&](GuideStep& st, std::size_t g, float a0, float a1, double seconds, bool frameEnd) {
        const std::size_t b = begin[g], n = begin[g + 1] - b;
        st.g = g;
        st.b = b;
        st.n = n;
        const float band = std::max(1e-4f, 0.05f * length[g]);  // the surface's band the water fades in over
        const RootPose r0 = blend(prev.roots[g], next.roots[g], a0);
        const RootPose r1 = blend(prev.roots[g], next.roots[g], a1);
        st.r1 = r1;
        auto target = [&](std::size_t j, const RootPose& r, float a) {
            const Vec3f l = prev.local[b + j] + (next.local[b + j] - prev.local[b + j]) * a;
            return r.toWorld(l);
        };
        if (n < 2) {
            if (n == 1) x[b] = target(0, r1, a1);
            if (n == 1 && frameEnd && ps.blends) frameTarget[b] = x[b];
            return false;
        }
        if (scalpRest.mesh && restRooted.size() == count && restRooted[g]) {
            st.onScalp.resize(n);
            for (std::size_t j = 0; j < n; ++j) st.onScalp[j] = r1.toWorld(r0.toLocal(x[b + j]));
        }
        // The share of the root's motion the hair is carried by rigidly (inertia below 1).
        if (carry > 0.0f) {
            const Quat turn = normalized(r1.rot * conj(r0.rot));
            for (std::size_t j = 1; j < n; ++j) {
                const std::size_t i = b + j;
                const Vec3f carried = r1.origin + rotate(turn, x[i] - r0.origin);
                x[i] = x[i] + (carried - x[i]) * carry;
                v[i] = v[i] + (rotate(turn, v[i]) - v[i]) * carry;
                q[i] = nlerp(q[i], turn * q[i], carry);
            }
        }
        std::vector<Vec3f>& p = st.p;
        std::vector<Vec3f>& tgt = st.tgt;
        std::vector<Vec3f>& corr = st.corr;
        p.resize(n);
        tgt.resize(n);
        corr.assign(n + 1, Vec3f{});
        for (std::size_t j = 0; j < n; ++j) tgt[j] = target(j, r1, a1);  // the groomed shape on the root now
        const Vec3f rootVelocity = (tgt[0] - x[b]) / h;
        // Gravity as this guide feels it: with groomedUnderGravity only its change in the root's
        // frame since the start (the groomed shape already holds the rest).
        const Vec3f pull = restGravity.empty() ? gravity : gravity - rotate(r1.rot, restGravity[g]);
        p[0] = tgt[0];
        // The air at this guide: the wind's speed varied by the gusts travelling with it.
        Vec3f air{};
        if (windy) {
            const Vec3f at = x[b] * s.windFrequency - windDir * static_cast<float>(s.windFrequency * s.windSpeed * seconds);
            air = flowVelocity(at, windDir, s.windSpeed, s.windGust, gustSeed, swaySeed);
        }
        // Under water: each point's share (0 above the surface's band, 1 below it), from the plane or
        // the water mesh (the height along its closest point's normal); then its wetness: soaking
        // up under water and in the rain, drying out of the water.
        std::vector<float>& under = st.under;
        Vec3f flow{};
        if (wetting) {
            under.assign(n, 0.0f);
            for (std::size_t j = 0; j < n; ++j) {
                const std::size_t i = b + j;
                if (s.water) {
                    float depth = s.waterLevel - x[i].y;
                    if (waterMesh) {
                        const SurfacePoint sp = waterMesh->closestPoint(x[i]);
                        depth = sp.root.face >= 0 ? (sp.position - x[i]).dot(sp.normal) : -band;
                    }
                    under[j] = smoothstep(-0.5f * band, 0.5f * band, depth);
                }
                float& w = wet[i];
                w += (1.0f - w) * std::min(1.0f, under[j] * soak + rainSoak);
                w -= w * drying * (1.0f - under[j]);
            }
            if (s.water && s.currentSpeed != 0.0f) {
                const Vec3f at = x[b] * s.windFrequency - currentDir * static_cast<float>(s.windFrequency * s.currentSpeed * seconds);
                flow = flowVelocity(at, currentDir, s.currentSpeed, kCurrentGust, currentGustSeed, currentSwaySeed);
            }
        }
        for (std::size_t j = 1; j < n; ++j) {
            const std::size_t i = b + j;
            if (held(i)) {
                p[j] = tgt[j];
                continue;
            }
            const float dragHere = custom ? ps.drag[i] : dragFactor;
            const float dampHere = custom ? ps.damp[i] : dampFactor;
            Vec3f vel;
            if (wetting) {
                // Gravity (heavier while wet above the water), buoyancy, and the water's drag toward its
                // flow taking over from the air's as the point goes under.
                const float u = under[j];
                vel = restGravity.empty() ? v[i] + gravity * (h * (1.0f + s.wetWeight * wet[i] * (1.0f - u))) + lift * (h * u)
                                          : v[i] + pull * h + gravity * (h * s.wetWeight * wet[i] * (1.0f - u)) + lift * (h * u);
                const Vec3f inAir = air + (vel - air) * dragHere;
                const Vec3f inWater = flow + (vel - flow) * waterDragFactor;
                vel = inAir + (inWater - inAir) * u;
            } else {
                vel = v[i] + pull * h;
                vel = air + (vel - air) * dragHere;
            }
            vel = rootVelocity + (vel - rootVelocity) * dampHere;
            p[j] = x[i] + vel * h;
            // The segment's orientation starts where the last substep left it: it keeps no spin of
            // its own (see the top of this file).
        }
        // The rod's segments for this substep: lengths, inverse masses (per point masses; the
        // orientation weighs as its tip point at the segment's length, and keeps no spin: what it
        // resists of a substep's turn is the solve's damping of the hair's swing), the stretch
        // row's compliance (per unit of a point's inverse mass) and the bend-twist rows' (per unit
        // of the orientation's), per reference step; wet hair bends more easily, up to half its bend
        // stiffness gone, by wetWeight.
        std::vector<RodSegment>& rod = st.rod;
        rod.assign(n, RodSegment{});
        const bool bends = custom || bendCompliance >= 0.0f || twistCompliance >= 0.0f;
        for (std::size_t j = 1; j < n; ++j) {
            const std::size_t i = b + j;
            RodSegment& seg = rod[j];
            seg.length = next.lengths[i];
            seg.w = custom ? ps.invMass[i] : 1.0f;
            seg.W = 4.0f * (custom ? next.invMassQ[i] * ps.perMass[i] : next.invMassQ[i]);
            const float l = std::max(seg.length, 1e-4f);
            seg.stretch = (custom ? ps.stretchCompliance[i] : stretchCompliance) / (l * l);
            if (!bends) continue;
            const float bendHere = custom ? ps.bend[i] : s.bend;
            const float bendComplianceHere = custom ? ps.bendCompliance[i] : bendCompliance;
            const float bc = wetting && bendComplianceHere >= 0.0f && wet[i] > 0.0f
                                 ? complianceOf(bendHere * (1.0f - 0.5f * s.wetWeight * wet[i])) * scale
                                 : bendComplianceHere;
            const float tc = custom ? ps.twistCompliance[i] : twistCompliance;
            seg.bend = bc >= 0.0f ? bc * next.invMassQ[i] : -1.0f;
            seg.twist = tc >= 0.0f ? tc * next.invMassQ[i] : -1.0f;
        }
        return true;
    };
    // The rest of guide st.g's substep, after its rod's solve: lengths kept, collisions, velocities.
    auto substepEnd = [&](GuideStep& st, bool frameEnd) {
        const std::size_t g = st.g, b = st.b, n = st.n;
        const float pushDepth = std::max(offset * 2.0f, 0.5f * length[g]);  // (collisions)
        const float pushSearch = std::max(offset, pushDepth) * 1.001f + 1e-6f;
        const RootPose& r1 = st.r1;
        std::vector<Vec3f>& p = st.p;
        const std::vector<Vec3f>& tgt = st.tgt;
        std::vector<Vec3f>& corr = st.corr;
        // Lengths kept: each point at its segment's length from the one before (follow the leader).
        for (std::size_t j = 1; j < n; ++j) {
            const std::size_t i = b + j;
            if (held(i)) continue;  // corr[j] stays 0
            const Vec3f d = p[j] - p[j - 1];
            const float len = d.length();
            const float l = next.lengths[i];
            const Vec3f to = len > 1e-12f ? p[j - 1] + d * (l / len) : p[j - 1] + rotate(q[i], {0.0f, 0.0f, l});
            corr[j] = (to - p[j]) * (custom ? ps.stretch[i] : stretchShare);
            p[j] += corr[j];
        }
        // Out of the scalp and the colliders, with friction against the surface.
        if (scalpNow.mesh || !colliders.empty()) {
            const bool viaRest = scalpRest.mesh && restRooted.size() == count && restRooted[g];
            const RootPose rest = viaRest ? restRoots[g] : RootPose{};
            // Each deformed mesh's walk along the guide (CollisionSurface): the scalp's from the root.
            RestPoseCollision::Cursor scalpCursor = !viaRest && scalpNow ? scalpNow.cursor(roots[g], p[0]) : RestPoseCollision::Cursor{};
            thread_local std::vector<RestPoseCollision::Cursor> colliderCursors;
            colliderCursors.assign(colliders.size(), RestPoseCollision::Cursor{});
            RestPoseCollision::Cursor unused;
            std::size_t firstPushed = n;
            // The walks' cursors before the first point pushed (the pass after the pushes picks
            // them up there: the points before are as they were).
            RestPoseCollision::Cursor scalpCursorThere;
            thread_local std::vector<RestPoseCollision::Cursor> colliderCursorsThere;
            for (std::size_t j = 1; j < n; ++j) {
                const std::size_t i = b + j;
                if (held(i)) continue;
                if (firstPushed == n) {
                    scalpCursorThere = scalpCursor;
                    colliderCursorsThere = colliderCursors;
                }
                const Vec3f before = p[j];
                // The friction of the surfaces that pushed it (the largest; each its own or `friction`).
                float mu = -1.0f;
                bool againstScalp = false;
                if (viaRest) {
                    Vec3f at = rest.toWorld(r1.toLocal(p[j]));
                    if (scalpRest.pushOut(at, offset, pushDepth, pushSearch, unused)) {
                        p[j] = r1.toWorld(rest.toLocal(at));
                        mu = frictionMu;
                        againstScalp = true;
                    }
                } else if (scalpNow.mesh) {
                    if (scalpNow.pushOut(p[j], offset, pushDepth, pushSearch, scalpCursor)) mu = frictionMu;
                }
                for (std::size_t c = 0; c < colliders.size(); ++c) {
                    if (colliders[c].pushOut(p[j], offset, pushDepth, pushSearch, colliderCursors[c])) mu = std::max(mu, colliderFriction[c]);
                }
                if (p[j] == before) continue;
                const Vec3f push = p[j] - before;
                const float depth = push.length();
                const Vec3f normal = push / depth;
                // The slide along the surface over the substep: against the scalp as it moved under
                // the point (its root's motion), else against the world.
                const Vec3f moved = p[j] - (againstScalp ? st.onScalp[j] : x[i]);
                const Vec3f sliding = moved - normal * moved.dot(normal);
                const float slide = sliding.length();
                // Coulomb: a slide within mu times how deep the surface pushed stops (the hair
                // sticks), a longer one loses that much of it (the hair slides, slowed).
                const float pushedMu = mu >= 0.0f ? mu : frictionMu;
                if (slide > 0.0f) p[j] -= sliding * std::min(1.0f, pushedMu * depth / slide);
                firstPushed = std::min(firstPushed, j);
            }
            if (firstPushed < n) {
                // The pushes stretched the segments about the points pushed (2.4% on hair resting
                // on the scalp, 8-20% in fast moves): the lengths kept again from the first point
                // pushed on, the corrections added to the leader's, then the surfaces once more
                // (without friction: the slide was taken once) for any point that went back in.
                for (std::size_t j = firstPushed; j < n; ++j) {
                    const std::size_t i = b + j;
                    if (held(i)) continue;
                    const Vec3f d = p[j] - p[j - 1];
                    const float len = d.length();
                    if (!(len > 1e-12f)) continue;
                    const Vec3f more = (p[j - 1] + d * (next.lengths[i] / len) - p[j]) * (custom ? ps.stretch[i] : stretchShare);
                    p[j] += more;
                    corr[j] += more;
                }
                scalpCursor = scalpCursorThere;
                colliderCursors = colliderCursorsThere;
                for (std::size_t j = firstPushed; j < n; ++j) {
                    const std::size_t i = b + j;
                    if (held(i)) continue;
                    if (viaRest) {
                        Vec3f at = rest.toWorld(r1.toLocal(p[j]));
                        if (scalpRest.pushOut(at, offset, pushDepth, pushSearch, unused)) p[j] = r1.toWorld(rest.toLocal(at));
                    } else if (scalpNow.mesh) {
                        scalpNow.pushOut(p[j], offset, pushDepth, pushSearch, scalpCursor);
                    }
                    for (std::size_t c = 0; c < colliders.size(); ++c) colliders[c].pushOut(p[j], offset, pushDepth, pushSearch, colliderCursors[c]);
                }
            }
        }
        // Velocities, with the leader's correction taken back (the points pushed out of a surface
        // too: without it a point resting on a surface bounces off it).
        for (std::size_t j = 1; j < n; ++j) {
            const std::size_t i = b + j;
            v[i] = held(i) ? (p[j] - x[i]) / h : (p[j] - x[i]) / h - corr[j + 1] * (kLeaderDamping / h);
        }
        for (std::size_t j = 0; j < n; ++j) x[b + j] = p[j];
        // The input shape the frame's blend leans toward (strength between 0 and 1).
        if (frameEnd && ps.blends) {
            for (std::size_t j = 0; j < n; ++j) frameTarget[b + j] = tgt[j];
        }
    };

    // Guide-guide repulsion: points of other guides closer than the radius pushed apart, every
    // push read from the points before the pass (in a fixed order: the same sums on any thread).
    std::vector<Vec3f> push;
    auto repel = [&] {
        const float radius = s.repulsionRadius;
        if (!(repulsionShare > 0.0f) || !(radius > 0.0f)) return;
        const float most = kMostRepulsion * radius;
        const float inv = 1.0f / radius;
        // A cell's index along an axis: 64-bit and clamped (a small radius over a large scene, or a
        // point gone to NaN in a sim that blew up, cast to int was undefined behaviour).
        auto axisCell = [&](float v) -> std::int64_t {
            const double q = std::floor(static_cast<double>(v) * static_cast<double>(inv));
            return std::isfinite(q) ? static_cast<std::int64_t>(std::clamp(q, -1e15, 1e15)) : 0;
        };
        auto cellOf = [&](const Vec3f& p, std::int64_t* c) {
            c[0] = axisCell(p.x);
            c[1] = axisCell(p.y);
            c[2] = axisCell(p.z);
        };
        auto key = [](std::int64_t cx, std::int64_t cy, std::int64_t cz) {
            return hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(cx)), static_cast<std::uint64_t>(cy)),
                               static_cast<std::uint64_t>(cz));
        };
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
        grid.reserve(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (i == begin[guideOf[i]]) continue;  // roots stay
            std::int64_t c[3];
            cellOf(x[i], c);
            grid[key(c[0], c[1], c[2])].push_back(static_cast<std::uint32_t>(i));
        }
        push.assign(x.size(), Vec3f{});
        parallelFor(0, x.size(), [&](std::size_t i) {
            if (i == begin[guideOf[i]]) return;
            std::int64_t c[3];
            cellOf(x[i], c);
            Vec3f sum{};
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto it = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == grid.end()) continue;
                        for (const std::uint32_t o : it->second) {
                            if (guideOf[o] == guideOf[i]) continue;
                            const Vec3f d = x[i] - x[o];
                            const float dist = d.length();
                            if (!(dist < radius) || !(dist > 1e-9f)) continue;
                            sum += d * ((radius - dist) * 0.5f * repulsionShare / dist);
                        }
                    }
                }
            }
            if (custom) sum = sum * ps.invMass[i];  // heavier: pushed less; held: not at all
            const float pushed = sum.length();  // capped after the mass: light hair is never thrown
            push[i] = pushed > most ? sum * (most / pushed) : sum;
        }, 1024);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += push[i];
            v[i] += push[i] / h;
        }
    };

    // Hair volume (Yeti's density gradient): the points counted into the world's grid of
    // volumeCellSize cells (trilinear weights, summed in point order on one thread: the same sums
    // on every run), each point then pushed down the density the others make where it is (its own
    // weights taken back out), relative to that density: hair packed tighter than a cell spreads
    // out, hair already apart is left alone. The grid's nodes are held in bricks, only where the
    // points are (a point flung far out costs a brick, not a grid spanning the space between).
    const float volumeShare = shareOver(s.volume, h, kReferenceStep);
    VolumeGrid volumeGrid;
    std::vector<Vec3f> volumePush;
    auto volumize = [&] {
        const float cell = s.volumeCellSize;
        if (!(volumeShare > 0.0f) || !(cell > 0.0f) || x.empty()) return;
        const float inv = 1.0f / cell;
        for (const Vec3f& p : x) {
            if (!(std::fabs(p.x * inv) < 1e15f && std::fabs(p.y * inv) < 1e15f && std::fabs(p.z * inv) < 1e15f)) return;
        }
        // A point's cell (its lowest corner node, in cells from the origin) and where in it the
        // point is, 0-1 on each axis.
        auto locate = [&](const Vec3f& p, std::int64_t* c, float* t) {
            const float f[3] = {p.x * inv, p.y * inv, p.z * inv};
            for (int a = 0; a < 3; ++a) {
                const float fl = std::floor(f[a]);
                c[a] = static_cast<std::int64_t>(fl);
                t[a] = std::clamp(f[a] - fl, 0.0f, 1.0f);
            }
        };
        volumeGrid.clear();
        for (const Vec3f& p : x) {
            std::int64_t c[3];
            float t[3];
            locate(p, c, t);
            for (int k = 0; k < 8; ++k) {
                const int a = k & 1, bb = (k >> 1) & 1, cc = (k >> 2) & 1;
                volumeGrid.add(c[0] + a, c[1] + bb, c[2] + cc,
                               (a ? t[0] : 1.0f - t[0]) * (bb ? t[1] : 1.0f - t[1]) * (cc ? t[2] : 1.0f - t[2]));
            }
        }
        const float most = kMostRepulsion * cell;
        const float amount = -0.5f * volumeShare * cell;  // times the density's relative change per cell
        volumePush.assign(x.size(), Vec3f{});
        parallelFor(0, x.size(), [&](std::size_t i) {
            if (i == begin[guideOf[i]] || held(i)) return;  // roots and held points stay
            std::int64_t c[3];
            float t[3];
            locate(x[i], c, t);
            VolumeGrid::Reader reader(volumeGrid);
            float rho = 0.0f;
            Vec3f grad{};  // per cell
            for (int k = 0; k < 8; ++k) {
                const int a = k & 1, bb = (k >> 1) & 1, cc = (k >> 2) & 1;
                const float wx = a ? t[0] : 1.0f - t[0], wy = bb ? t[1] : 1.0f - t[1], wz = cc ? t[2] : 1.0f - t[2];
                // The others' count at the node: its own weight there taken out.
                const float d = reader.at(c[0] + a, c[1] + bb, c[2] + cc) - wx * wy * wz;
                rho += d * wx * wy * wz;
                grad.x += d * (a ? 1.0f : -1.0f) * wy * wz;
                grad.y += d * wx * (bb ? 1.0f : -1.0f) * wz;
                grad.z += d * wx * wy * (cc ? 1.0f : -1.0f);
            }
            Vec3f push = grad * (amount / (std::max(0.0f, rho) + 1.0f));
            if (custom) push = push * ps.invMass[i];  // heavier: pushed less
            const float pushed = push.length();  // capped after the mass: light hair is never thrown
            if (pushed > most) push = push * (most / pushed);
            volumePush[i] = push;
        }, 1024);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += volumePush[i];
            v[i] += volumePush[i] / h;
        }
    };

    // The guides in batches of up to four of the same point count, their rods solved side by side
    // (rodSolve): a guide's result is the same in any batch, to the bit.
    std::vector<std::pair<std::size_t, int>> batches;
    for (std::size_t g = 0; g < count;) {
        const std::size_t n = begin[g + 1] - begin[g];
        int lanes = 1;
        while (n >= 2 && lanes < kLanes && g + lanes < count && begin[g + lanes + 1] - begin[g + lanes] == n) ++lanes;
        batches.emplace_back(g, lanes);
        g += static_cast<std::size_t>(lanes);
    }
    // One substep of a batch's guides.
    auto substep = [&](std::size_t batch, float a0, float a1, double seconds, bool frameEnd) {
        thread_local GuideStep steps[kLanes];
        thread_local RodWork work;
        const auto [first, lanes] = batches[batch];
        RodLane rods[kLanes];
        int solving = 0;
        for (int k = 0; k < lanes; ++k) {
            GuideStep& st = steps[solving];
            if (!substepBegin(st, first + static_cast<std::size_t>(k), a0, a1, seconds, frameEnd)) continue;
            RodLane& rod = rods[solving++];
            rod.p = st.p.data();
            rod.tgt = st.tgt.data();
            rod.q = q.data() + st.b;
            rod.rest = next.rest.data() + st.b;
            rod.rootRot = st.r1.rot;
            rod.seg = st.rod.data();
            rod.attract = (custom ? ps.attract.data() : attract.data()) + st.b;
        }
        if (solving == 0) return;
        rodSolve(steps[0].n, rods, solving, s.iterations, work);
        for (int k = 0; k < solving; ++k) substepEnd(steps[k], frameEnd);
    };

    const int base = frame;
    for (int k = 1; k <= substeps; ++k) {
        const float a0 = static_cast<float>(k - 1) / static_cast<float>(substeps);
        const float a1 = static_cast<float>(k) / static_cast<float>(substeps);
        const double seconds = frameToSeconds(base + static_cast<double>(k) / s.substeps, fps);
        const bool frameEnd = k % s.substeps == 0;
        if (!sweeps.empty()) sweepTo(k == substeps ? 1.0f : a1);
        parallelFor(0, batches.size(), [&](std::size_t batch) { substep(batch, a0, a1, seconds, frameEnd); }, 4);
        repel();
        volumize();
        if (frameEnd && ps.blends) {
            // As shown: each point that share of its simulated offset from the input shape.
            parallelFor(0, x.size(), [&](std::size_t i) {
                const float w = ps.weight[i];
                shown[i] = w >= 1.0f ? x[i] : frameTarget[i] + (x[i] - frameTarget[i]) * w;
            }, 4096);
        }
        if (k % s.substeps == 0) {
            ++frame;
            if (frameDone) frameDone(k / s.substeps);
        }
    }
    prev = std::move(next);
    next = Targets{};
    lastColliders = in.colliders;
    roots.clear();
    for (const Guide& guide : in.guides->guides()) roots.push_back(guide.root);
}

GuidesPtr GuideSolver::guides() const {
    const Impl& m = *impl_;
    auto out = std::make_shared<GuideSet>();
    const std::vector<Vec3f>& points = m.shown.empty() ? m.x : m.shown;
    // Every guide made at once and filled side by side (one after another, growing the set, was a
    // fifth of a frame's time at 10k guides).
    std::vector<Guide>& guides = out->guides();
    guides.resize(m.guideCount());
    parallelFor(0, m.guideCount(), [&](std::size_t g) {
        const auto b = static_cast<std::ptrdiff_t>(m.begin[g]), e = static_cast<std::ptrdiff_t>(m.begin[g + 1]);
        Guide& guide = guides[g];
        guide.id = m.ids[g];
        guide.root = m.roots[g];
        guide.points.assign(points.begin() + b, points.begin() + e);
        if (!m.wet.empty()) guide.pointAttributes[kWetnessChannel].assign(m.wet.begin() + b, m.wet.begin() + e);
    }, 256);
    return out;
}

// ---- The cache --------------------------------------------------------------------------------

SimCache::SimCache() = default;
SimCache::~SimCache() = default;

void SimCache::configure(const SimSettings& settings, double framesPerSecond) {
    const std::uint64_t hash = settings.hash();
    const double fps = framesPerSecond > 0.0 ? framesPerSecond : 24.0;
    if (hash != settingsHash_ || fps != fps_) clear();
    settings_ = settings;
    settingsHash_ = hash;
    fps_ = fps;
}

int SimCache::lastFrame() const { return frames_.empty() ? settings_.startFrame - 1 : frames_.rbegin()->first; }

GuidesPtr SimCache::frame(int frame) const {
    const auto it = frames_.find(frame);
    return it == frames_.end() ? nullptr : it->second.guides;
}

std::uint64_t SimCache::frameHash(int frame) const {
    const auto it = frames_.find(frame);
    return it == frames_.end() ? 0 : it->second.hash;
}

std::uint64_t SimCache::inputHash(int frame) const {
    const auto it = frames_.find(frame);
    return it == frames_.end() ? 0 : it->second.input;
}

std::vector<std::pair<int, int>> SimCache::ranges() const {
    std::vector<std::pair<int, int>> out;
    for (const auto& [f, frame] : frames_) {
        if (!out.empty() && out.back().second + 1 == f) {
            out.back().second = f;
        } else {
            out.emplace_back(f, f);
        }
    }
    return out;
}

void SimCache::keep(std::uint64_t inputHash) {
    Frame kept;
    kept.guides = solver_.guides();
    // Each frame made is new (its key in the graph): a serial, not a pass over every point.
    static std::atomic<std::uint64_t> made{0};
    kept.hash = hashCombine(hash64(0xF7A3E), made.fetch_add(1) + 1);
    kept.input = inputHash;
    frames_[solver_.frame()] = std::move(kept);
}

void SimCache::start(const SimInputs& inputs, std::uint64_t inputHash) {
    const auto started = std::chrono::steady_clock::now();
    frames_.clear();
    solver_.start(settings_, fps_, settings_.startFrame, inputs);
    keep(inputHash);
    lastStepMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void SimCache::start(const SimInputs& rest, const SimInputs& inputs, std::uint64_t inputHash) {
    const int preroll = settings_.preroll;
    if (preroll <= 0 || !rest.guides || !inputs.guides) return start(inputs, inputHash);
    const auto started = std::chrono::steady_clock::now();
    frames_.clear();
    solver_.start(settings_, fps_, settings_.startFrame - preroll, rest);
    if (!solver_.sameLayout(*inputs.guides)) return start(inputs, inputHash);
    const int move = (preroll + 1) / 2;
    solver_.step(inputs, move);
    if (preroll > move) solver_.step(inputs, preroll - move);
    keep(inputHash);
    lastStepMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / (preroll + 1);
}

bool SimCache::step(const SimInputs& inputs, std::uint64_t inputHash, int frames) {
    if (!solver_.started() || !inputs.guides || !solver_.sameLayout(*inputs.guides)) {
        clear();
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    frames = std::max(1, frames);
    // Frames between the last and these inputs were stepped toward a blend of the two: what they
    // were stepped toward is not known (0: never checked against a host's inputs).
    solver_.step(inputs, frames, [&](int k) { keep(k == frames ? inputHash : 0); });
    lastStepMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / frames;
    return true;
}

void SimCache::clear() {
    frames_.clear();
    lastStepMs_ = 0.0;
}

std::size_t SimCache::bytes() const {
    std::size_t out = 0;
    for (const auto& [f, frame] : frames_) {
        if (!frame.guides) continue;
        for (const Guide& g : frame.guides->guides()) {
            out += g.points.size() * sizeof(Vec3f) + sizeof(Guide);
            for (const auto& [name, values] : g.pointAttributes) out += values.size() * sizeof(float);
        }
    }
    return out;
}

// ---- Presets ----------------------------------------------------------------------------------

const std::vector<SimPreset>& simulatePresets() {
    static const std::vector<SimPreset> kPresets{
        {"default", "Default", "The Simulate node's defaults: hair that swings freely, held only near the root"},
        {"fur", "Fur",
         "Springy fur that holds its groom: gravity barely changes it, the head's motion shakes it and it springs back "
         "to its groomed shape in a few quick swings"},
    };
    return kPresets;
}

std::string simulatePresetParamsJson(const std::string& name) {
    // Every preset sets the same params (see simulation.h).
    ParamSet params;
    auto set = [&](const char* underGravity, double bend, double twist, double damping, double drag, double rootStiffness,
                   Ramp profile, double inertia, std::int64_t iterations) {
        params.set("groomedUnderGravity", std::string(underGravity));
        params.set("bend", bend);
        params.set("twist", twist);
        params.set("damping", damping);
        params.set("drag", drag);
        params.set("rootStiffness", rootStiffness);
        params.set("rootStiffnessProfile", std::move(profile));
        params.set("inertia", inertia);
        params.set("iterations", iterations);
    };
    if (name == "default") {
        // The Simulate node's defaults (its spec's: a test holds them together).
        set("preload", 0.5, 0.5, 0.1, 0.05, 1.0, Ramp{{0.0f, 1.0f}, {0.2f, 0.0f}}, 1.0, 6);
    } else if (name == "fur") {
        // Tuned on a wolf (centimetres, 30 fps: a fast turn, a run, a hard stop; docs:
        // motion-and-simulation, "Springy fur that holds its groom"). groomedUnderGravity
        // (weightless, as tuned) keeps the groom as it is while still (the typical tip moved under 0.1% of its length a second in,
        // 86% at the node's defaults); bend 0.8 and the hold all along the length (root 1, middle
        // 0.7, tips 0.35) spring it back as a whole; inertia 0.8 takes the edge off the body's
        // motion and damping 0.25 stops it after two or three swings (a third of a second).
        set("weightless", 0.8, 0.7, 0.25, 0.05, 1.0, Ramp{{0.0f, 1.0f}, {0.5f, 0.7f}, {1.0f, 0.35f}}, 0.8, 6);
    } else {
        throw std::invalid_argument("no Simulate preset '" + name + "' (default, fur)");
    }
    return paramsJson(params);
}

}  // namespace groomlab
