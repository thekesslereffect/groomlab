// SPDX-License-Identifier: Apache-2.0
#include "groomlab/length_paths.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

// The share of the reach (from its edge) a path's weight eases out over: full within the inner
// 60%, 0 at the edge, smooth in between.
constexpr float kFade = 0.4f;

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The point of segment a-b nearest p, as its share along the segment.
float nearestOnSegment(const Vec3f& a, const Vec3f& b, const Vec3f& p) {
    const Vec3f ab = b - a;
    const float len2 = ab.dot(ab);
    return len2 > 0.0f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
}

// The length between keys i and i+1 at share t: straight, or Fritsch-Carlson's monotone cubic over
// the keys' distances along the path (`along`), so a smooth path never overshoots a key.
float lengthBetween(const std::vector<float>& along, const std::vector<float>& lengths, std::size_t i, float t,
                    bool smooth) {
    const float l0 = lengths[i], l1 = lengths[i + 1];
    if (!smooth || lengths.size() < 3) return smooth ? l0 + (l1 - l0) * smoothstep01(t) : l0 + (l1 - l0) * t;
    const std::size_t n = lengths.size();
    auto slope = [&](std::size_t k) {  // the secant from key k to k + 1
        const float h = along[k + 1] - along[k];
        return h > 0.0f ? (lengths[k + 1] - lengths[k]) / h : 0.0f;
    };
    auto tangent = [&](std::size_t k) {  // at key k
        if (k == 0) return slope(0);
        if (k == n - 1) return slope(n - 2);
        const float a = slope(k - 1), b = slope(k);
        if (a * b <= 0.0f) return 0.0f;  // a peak or a valley: flat there
        return 2.0f / (1.0f / a + 1.0f / b);  // the harmonic mean keeps the curve monotone
    };
    const float h = along[i + 1] - along[i];
    const float m0 = tangent(i) * h, m1 = tangent(i + 1) * h;
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * l0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * l1 + (t3 - t2) * m1;
}

}  // namespace

bool LengthPathSet::empty() const {
    return std::none_of(paths.begin(), paths.end(), [](const LengthPath& p) { return !p.keys.empty(); });
}

std::string LengthPathSet::toJson() const {
    json out = json::array();
    for (const LengthPath& path : paths) {
        json keys = json::array();
        for (const LengthKey& k : path.keys) {
            json key = {{"point", {k.point.x, k.point.y, k.point.z}}, {"length", k.length}, {"reach", k.reach}};
            if (k.root.face >= 0) {
                key["face"] = k.root.face;
                key["u"] = k.root.u;
                key["v"] = k.root.v;
            }
            keys.push_back(std::move(key));
        }
        out.push_back({{"keys", std::move(keys)}, {"smooth", path.smooth}});
    }
    return out.dump();
}

LengthPathSet LengthPathSet::fromJson(const std::string& text) {
    LengthPathSet set;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return set;
    try {
        const json root = json::parse(text);
        if (!root.is_array()) throw std::invalid_argument("length paths: not a list");
        for (const json& p : root) {
            LengthPath path;
            path.smooth = p.value("smooth", true);
            for (const json& k : p.at("keys")) {
                LengthKey key;
                const auto& pt = k.at("point");
                key.point = {pt.at(0).get<float>(), pt.at(1).get<float>(), pt.at(2).get<float>()};
                key.length = std::max(0.0f, k.value("length", 1.0f));
                key.reach = std::max(0.0f, k.value("reach", 1.0f));
                key.root.face = k.value("face", -1);
                key.root.u = k.value("u", 0.0f);
                key.root.v = k.value("v", 0.0f);
                path.keys.push_back(key);
            }
            set.paths.push_back(std::move(path));
        }
    } catch (const json::exception& e) {
        throw std::invalid_argument(std::string("length paths: ") + e.what());
    }
    return set;
}

const char* lengthChangeName(LengthChange change) { return change == LengthChange::Trim ? "trim" : "scale"; }

LengthChange lengthChangeFromName(const std::string& name) {
    if (name == "scale") return LengthChange::Scale;
    if (name == "trim") return LengthChange::Trim;
    throw std::invalid_argument("unknown length change '" + name + "' (scale or trim)");
}

Vec3f lengthKeyPosition(const LengthKey& key, const ScalpMesh* scalp) {
    return scalp && scalp->contains(key.root) ? scalp->position(key.root) : key.point;
}

LengthSample sampleLengthPath(const LengthPath& path, const Vec3f& point, const ScalpMesh* scalp) {
    LengthSample out;
    const std::size_t n = path.keys.size();
    if (n == 0) return out;
    std::vector<Vec3f> at(n);
    std::vector<float> lengths(n), along(n, 0.0f);
    for (std::size_t k = 0; k < n; ++k) {
        at[k] = lengthKeyPosition(path.keys[k], scalp);
        lengths[k] = path.keys[k].length;
        if (k) along[k] = along[k - 1] + (at[k] - at[k - 1]).length();
    }
    // The spot of the path the point is deepest inside the reach of: its distance over the reach
    // there (the keys' reaches straight between them) the least.
    float best = std::numeric_limits<float>::max();
    auto consider = [&](float d, float reach, float length) {
        if (!(reach > 0.0f)) return;
        const float share = d / reach;
        if (share < best) {
            best = share;
            out.length = length;
        }
    };
    if (n == 1) {
        consider((point - at[0]).length(), path.keys[0].reach, lengths[0]);
    } else {
        for (std::size_t k = 0; k + 1 < n; ++k) {
            const float t = nearestOnSegment(at[k], at[k + 1], point);
            const float d = (point - (at[k] + (at[k + 1] - at[k]) * t)).length();
            const float reach = path.keys[k].reach + (path.keys[k + 1].reach - path.keys[k].reach) * t;
            consider(d, reach, lengthBetween(along, lengths, k, t, path.smooth));
        }
    }
    if (best == std::numeric_limits<float>::max()) return {};
    out.weight = 1.0f - smoothstep01((best - (1.0f - kFade)) / kFade);
    if (!(out.weight > 0.0f)) out = {};
    return out;
}

LengthSample sampleLengthPaths(const LengthPathSet& paths, const Vec3f& point, const ScalpMesh* scalp) {
    float sum = 0.0f, weights = 0.0f, most = 0.0f;
    for (const LengthPath& path : paths.paths) {
        const LengthSample s = sampleLengthPath(path, point, scalp);
        if (!(s.weight > 0.0f)) continue;
        sum += s.length * s.weight;
        weights += s.weight;
        most = std::max(most, s.weight);
    }
    if (!(weights > 0.0f)) return {};
    return {sum / weights, most};
}

LengthPathSet mirroredLengthPaths(const LengthPathSet& paths, const ScalpMesh* scalp,
                                  const std::function<Vec3f(const Vec3f&)>& mirror) {
    LengthPathSet out = paths;
    for (LengthPath& path : out.paths) {
        for (LengthKey& key : path.keys) {
            const Vec3f p = mirror(lengthKeyPosition(key, scalp));
            key.point = p;
            key.root = StrandRoot{};
            key.root.face = -1;
            if (scalp && scalp->triangleCount() > 0) {
                const SurfacePoint hit = scalp->closestPoint(p);
                if (hit.root.face >= 0) {
                    key.root = hit.root;
                    key.point = hit.position;
                }
            }
        }
    }
    return out;
}

float averageGuideLength(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides, const Vec3f& point,
                         float radius) {
    float sum = 0.0f, weights = 0.0f, nearest = std::numeric_limits<float>::max(), nearestLength = 0.0f;
    for (const GuideId id : guides) {
        const HierarchyGuide* g = hierarchy.guide(id);
        if (!g || g->points.size() < 2) continue;
        const float d = (g->points[0] - point).length();
        const float length = arcLength(g->points);
        if (d < nearest) {
            nearest = d;
            nearestLength = length;
        }
        if (radius > 0.0f && d < radius) {
            const float w = 1.0f - smoothstep01(d / radius);
            sum += length * w;
            weights += w;
        }
    }
    return weights > 0.0f ? sum / weights : nearestLength;
}

std::vector<Vec3f> guideAtLength(const std::vector<Vec3f>& points, float length, LengthChange change) {
    const std::size_t n = points.size();
    if (n < 2 || !(length > 0.0f)) return points;
    const float total = arcLength(points);
    if (!(total > 0.0f)) return points;
    const float factor = length / total;
    if (change == LengthChange::Scale) {
        std::vector<Vec3f> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = points[0] + (points[i] - points[0]) * factor;
        return out;
    }
    // Shorter: cut at the length; longer: the tip extended along its last direction. Either way the
    // points are spaced evenly again, which cuts a little off sharp bends: asked again for what
    // was lost, a few times, so the guide ends at the length.
    auto trimmed = [&](float ask) {
        if (ask < total) return resampleByArcLength(points.data(), n, n, ask / total);
        std::vector<Vec3f> extended = points;
        extended.push_back(points[n - 1] + (points[n - 1] - points[n - 2]).normalized() * (ask - total));
        return resampleByArcLength(extended.data(), extended.size(), n);
    };
    float ask = length;
    std::vector<Vec3f> out = trimmed(ask);
    for (int i = 0; i < 6; ++i) {
        const float got = arcLength(out);
        if (!(got > 0.0f) || std::abs(got - length) <= 1e-5f * length) break;
        ask *= length / got;
        out = trimmed(ask);
    }
    return out;
}

std::vector<LengthResult> applyLengthPaths(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides,
                                           const LengthPathSet& paths, LengthChange change, const ScalpMesh* scalp) {
    std::vector<LengthResult> out;
    if (paths.empty()) return out;
    for (const GuideId id : guides) {
        const HierarchyGuide* g = hierarchy.guide(id);
        if (!g || g->points.size() < 2) continue;
        const LengthSample s = sampleLengthPaths(paths, g->points[0], scalp);
        if (!(s.weight > 0.0f)) continue;
        const float own = arcLength(g->points);
        const float length = own + (s.length - own) * s.weight;
        if (!(own > 0.0f) || std::abs(length - own) <= 1e-6f * own) continue;
        out.push_back({id, guideAtLength(g->points, length, change)});
    }
    return out;
}

}  // namespace groomlab
