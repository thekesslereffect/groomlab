// SPDX-License-Identifier: Apache-2.0
#include "groomlab/ramp.h"

#include <algorithm>

#include "groomlab/shared/groom_math.h"

namespace groomlab {

void Ramp::sort() {
    if (!hasHandles()) {
        handles.clear();
        std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return;
    }
    // The handles go with their keys.
    std::vector<std::size_t> order(keys.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a].first < keys[b].first; });
    std::vector<std::pair<float, float>> sortedKeys(keys.size());
    std::vector<RampHandle> sortedHandles(keys.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        sortedKeys[k] = keys[order[k]];
        sortedHandles[k] = handle(order[k]);
    }
    keys = std::move(sortedKeys);
    handles = std::move(sortedHandles);
}

bool Ramp::hasHandles() const {
    for (std::size_t k = 0; k < handles.size() && k < keys.size(); ++k) {
        if (handles[k].set) return true;
    }
    return false;
}

void Ramp::setHandle(std::size_t k, const RampHandle& h) {
    if (k >= keys.size()) return;
    if (handles.size() < keys.size()) handles.resize(keys.size());
    handles[k] = h;
    if (!hasHandles()) handles.clear();
}

bool Ramp::operator==(const Ramp& o) const {
    if (keys != o.keys || straight != o.straight) return false;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        if (handle(k) != o.handle(k)) return false;
    }
    return true;
}

namespace {

// The ramp at t, `in(k)` and `out(k)` key k's slopes into it and out of it (operator(), at).
template <class In, class Out>
float rampAt(const Ramp& ramp, float t, const In& in, const Out& out) {
    const auto& keys = ramp.keys;
    if (keys.empty()) return 1.0f;
    if (!(t > keys.front().first)) return keys.front().second;  // also NaN
    if (t >= keys.back().first) return keys.back().second;
    const auto hi = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const auto& k) { return x < k.first; });
    const auto lo = hi - 1;
    const float span = hi->first - lo->first;
    if (span <= 0.0f) return hi->second;
    const float f = (t - lo->first) / span;
    if (ramp.straight) return lo->second + (hi->second - lo->second) * f;

    // Cubic Hermite: out of the lower key, into the upper one.
    const std::size_t i = static_cast<std::size_t>(lo - keys.begin());
    const float m0 = out(i) * span, m1 = in(i + 1) * span;
    return shared::rampHermite(f, lo->second, m0, hi->second, m1);
}

}  // namespace

float Ramp::operator()(float t) const {
    return rampAt(*this, t, [&](std::size_t k) { return tangentIn(k); }, [&](std::size_t k) { return tangentOut(k); });
}

std::vector<float> Ramp::tangents() const {
    std::vector<float> out(keys.size() * 2);
    for (std::size_t k = 0; k < keys.size(); ++k) {
        out[2 * k] = tangentIn(k);
        out[2 * k + 1] = tangentOut(k);
    }
    return out;
}

float Ramp::at(float t, const float* tangents) const {
    return rampAt(*this, t, [&](std::size_t k) { return tangents[2 * k]; }, [&](std::size_t k) { return tangents[2 * k + 1]; });
}

float Ramp::tangentIn(std::size_t k) const {
    const RampHandle h = handle(k);
    return h.set ? h.in : tangent(k);
}

float Ramp::tangentOut(std::size_t k) const {
    const RampHandle h = handle(k);
    return h.set ? h.out : tangent(k);
}

float Ramp::tangent(std::size_t k) const {
    auto secant = [&](std::size_t s) {  // slope of span s (keys s to s + 1)
        return shared::rampSecant(keys[s].first, keys[s].second, keys[s + 1].first, keys[s + 1].second);
    };
    if (keys.size() < 2) return 0.0f;
    if (k == 0) return secant(0);
    if (k + 1 == keys.size()) return secant(k - 1);
    const float h0 = keys[k].first - keys[k - 1].first, h1 = keys[k + 1].first - keys[k].first;
    return shared::rampInnerTangent(h0, h1, secant(k - 1), secant(k));
}

}  // namespace groomlab
