// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace groomlab {

// Curve over [0, 1], used for "along the length" parameters (clump profile,
// noise falloff, ...). Keys are (position, value) sorted by position; values
// outside the first and last key are held constant.
//
// Between keys the curve is a cubic Hermite through every key. A key's slopes
// (its handles) are automatic unless set: the monotone cubic's (Fritsch-Butland
// tangents), which never overshoot and flatten at peaks and valleys, so profiles
// don't put kinks into the hair; two keys give a straight line. A key's handles
// set (`handles`, issue #102) give it slopes of the artist's: one for both sides
// (a smooth key), or one each side (a corner). `straight` switches to straight
// segments (the handles unused).
//
// A key's handles: its slopes (value per unit of position) into it from the left
// (`in`) and out of it to the right (`out`); `set` false: automatic.
struct RampHandle {
    bool set = false;
    float in = 0.0f, out = 0.0f;
    bool operator==(const RampHandle& o) const { return set == o.set && (!set || (in == o.in && out == o.out)); }
    bool operator!=(const RampHandle& o) const { return !(*this == o); }
};

struct Ramp {
    std::vector<std::pair<float, float>> keys;
    // Each key's handles (as keys, by index); missing ones (shorter, or empty) are automatic.
    // sort() keeps them with their keys.
    std::vector<RampHandle> handles;
    bool straight = false;  // straight segments instead of the smooth curve

    Ramp() = default;
    Ramp(std::initializer_list<std::pair<float, float>> k) : keys(k) { sort(); }

    static Ramp constant(float v) { return Ramp{{0.0f, v}}; }
    static Ramp linear(float root, float tip) { return Ramp{{0.0f, root}, {1.0f, tip}}; }

    void sort();
    float operator()(float t) const;
    // Key k's handles (automatic when none is set), any key's set, and key k's set (an automatic
    // one: RampHandle{}; past the keys: nothing).
    RampHandle handle(std::size_t k) const { return k < handles.size() ? handles[k] : RampHandle{}; }
    bool hasHandles() const;
    void setHandle(std::size_t k, const RampHandle& h);
    // The monotone cubic's tangent (slope) at key k, the automatic one (0 with fewer than two keys).
    float tangent(std::size_t k) const;
    // The slopes the curve takes into key k (from the left) and out of it (to the right): its
    // handles' when set, else its automatic tangent. The GPU preview takes them with the keys.
    float tangentIn(std::size_t k) const;
    float tangentOut(std::size_t k) const;
    // Every key's slopes, in and out (2 per key, as tangentIn and tangentOut give them), and the
    // ramp at t from them: the same value as operator(), for a ramp taken at many places without
    // working them out each time.
    std::vector<float> tangents() const;
    float at(float t, const float* tangents) const;

    bool operator==(const Ramp& o) const;
    bool operator!=(const Ramp& o) const { return !(*this == o); }
};

}  // namespace groomlab
