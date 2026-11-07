// SPDX-License-Identifier: Apache-2.0
#include "groomlab/control_keys.h"

#include <algorithm>
#include <cmath>

namespace groomlab {

namespace {

constexpr double kSameFrame = 1e-6;

}  // namespace

double controlKeyValue(const ControlKeys& keys, double frame) {
    if (keys.empty()) return 0.0;
    if (frame <= keys.front().first || keys.size() == 1) return keys.front().second;
    if (frame >= keys.back().first) return keys.back().second;
    const auto after = std::upper_bound(keys.begin(), keys.end(), frame, [](double f, const std::pair<double, double>& k) { return f < k.first; });
    const std::size_t i = static_cast<std::size_t>(after - keys.begin()) - 1;  // keys[i] <= frame < keys[i + 1]
    const std::size_t n = keys.size();
    // Secant slopes, then Fritsch-Carlson tangents: flat at an extreme, limited so the curve stays
    // between its two keys.
    auto secant = [&](std::size_t k) { return (keys[k + 1].second - keys[k].second) / (keys[k + 1].first - keys[k].first); };
    auto tangent = [&](std::size_t k) {
        if (k == 0) return secant(0);
        if (k == n - 1) return secant(n - 2);
        const double a = secant(k - 1), b = secant(k);
        if (a * b <= 0.0) return 0.0;
        const double h0 = keys[k].first - keys[k - 1].first, h1 = keys[k + 1].first - keys[k].first;
        const double w0 = 2.0 * h1 + h0, w1 = h1 + 2.0 * h0;
        return (w0 + w1) / (w0 / a + w1 / b);  // the weighted harmonic mean
    };
    const double h = keys[i + 1].first - keys[i].first;
    const double t = (frame - keys[i].first) / h;
    double m0 = tangent(i), m1 = tangent(i + 1);
    const double d = secant(i);
    if (d == 0.0) {
        m0 = m1 = 0.0;
    } else {
        const double a = m0 / d, b = m1 / d;
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double tau = 3.0 / std::sqrt(s);
            m0 = tau * a * d;
            m1 = tau * b * d;
        }
    }
    const double t2 = t * t, t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * keys[i].second + (t3 - 2.0 * t2 + t) * h * m0 + (-2.0 * t3 + 3.0 * t2) * keys[i + 1].second +
           (t3 - t2) * h * m1;
}

void setControlKey(ControlKeys& keys, double frame, double value) {
    for (auto& k : keys) {
        if (std::abs(k.first - frame) < kSameFrame) {
            k.second = value;
            return;
        }
    }
    keys.emplace_back(frame, value);
    std::sort(keys.begin(), keys.end());
}

bool removeControlKey(ControlKeys& keys, double frame) {
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const std::pair<double, double>& k) { return std::abs(k.first - frame) < kSameFrame; });
    if (it == keys.end()) return false;
    keys.erase(it);
    return true;
}

bool hasControlKey(const ControlKeys& keys, double frame) {
    return std::any_of(keys.begin(), keys.end(), [&](const std::pair<double, double>& k) { return std::abs(k.first - frame) < kSameFrame; });
}

}  // namespace groomlab
