// SPDX-License-Identifier: Apache-2.0
#include "groomlab/length_field.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/surface_paths.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
// Where the scale eases back to x1: from the keys' spacing out to twice it.
constexpr float kFadeOver = 1.0f;
// The fronts' limit: the scalp's extent times this (every vertex of a piece reached).
constexpr float kLimitExtents = 4.0f;

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

}  // namespace

bool LengthKeySet::scaled() const {
    return std::any_of(keys.begin(), keys.end(), [](const LengthKey& k) { return k.scale != 1.0f; });
}

std::string LengthKeySet::toJson() const {
    json out = json::array();
    for (const LengthKey& k : keys) {
        json key = {{"point", {k.point.x, k.point.y, k.point.z}}, {"scale", k.scale}};
        if (k.root.face >= 0) {
            key["face"] = k.root.face;
            key["u"] = k.root.u;
            key["v"] = k.root.v;
        }
        out.push_back(std::move(key));
    }
    return out.dump();
}

LengthKeySet LengthKeySet::fromJson(const std::string& text) {
    LengthKeySet set;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return set;
    try {
        const json root = json::parse(text);
        if (!root.is_array()) throw std::invalid_argument("length keys: not a list");
        for (const json& k : root) {
            LengthKey key;
            const auto& pt = k.at("point");
            key.point = {pt.at(0).get<float>(), pt.at(1).get<float>(), pt.at(2).get<float>()};
            key.scale = k.value("scale", 1.0f);
            if (!(key.scale > 0.0f)) key.scale = 1.0f;
            key.root.face = k.value("face", -1);
            key.root.u = k.value("u", 0.0f);
            key.root.v = k.value("v", 0.0f);
            set.keys.push_back(key);
        }
    } catch (const json::exception& e) {
        throw std::invalid_argument(std::string("length keys: ") + e.what());
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

// ---- The field ------------------------------------------------------------------------------------

// A point the field is asked at: where it is now, and its root and rest position on the scalp
// (face -1: not on it).
struct LengthField::End {
    Vec3f now, rest;
    StrandRoot root;
};

LengthField::LengthField(const LengthKeySet& keys, const ScalpMesh* scalp) : scalp_(scalp) {
    const std::size_t n = keys.keys.size();
    if (n == 0) return;
    if (scalp && scalp->triangleCount() > 0) graph_ = sharedSurfaceGraph(*scalp);
    std::vector<StrandRoot> roots(n);
    std::vector<float> limits(n, -1.0f);
    float limit = 0.0f;
    if (graph_ && graph_->vertexCount() > 0) {
        Vec3f lo = graph_->position(0), hi = lo;
        for (std::uint32_t v = 1; v < graph_->vertexCount(); ++v) {
            const Vec3f& p = graph_->position(v);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        limit = (hi - lo).length() * kLimitExtents;
    }
    for (const LengthKey& key : keys.keys) {
        const std::size_t i = logScales_.size();
        logScales_.push_back(std::log(std::max(key.scale, 1e-6f)));
        points_.push_back(lengthKeyPosition(key, scalp));
        roots[i].face = -1;
        if (graph_ && graph_->contains(key.root)) {
            roots[i] = key.root;
            limits[i] = limit;
            rest_.push_back(graph_->rootPosition(key.root));
        } else {
            rest_.push_back(points_.back());
        }
    }
    if (graph_) fronts_ = std::make_unique<SurfaceFronts>(graph_, roots, limits, nullptr);
    // The spacing: the largest distance from a key to its nearest other.
    spacing_ = n < 2 ? kInf : 0.0f;
    for (std::size_t i = 0; i < n && n >= 2; ++i) {
        End end{points_[i], rest_[i], roots[i]};
        float nearest = kInf;
        for (std::size_t j = 0; j < n; ++j) {
            if (j != i) nearest = std::min(nearest, distance(j, end));
        }
        spacing_ = std::max(spacing_, nearest);
    }
}

LengthField::~LengthField() = default;

LengthField::End LengthField::endAt(const Vec3f& point) const {
    End end{point, point, StrandRoot{}};
    end.root.face = -1;
    if (!graph_) return end;
    const SurfacePoint hit = scalp_->closestPoint(point);
    if (!graph_->contains(hit.root)) return end;
    end.root = hit.root;
    end.rest = graph_->rootPosition(hit.root);
    return end;
}

float LengthField::distance(std::size_t key, const End& end) const {
    const float straight = (end.now - points_[key]).length();
    if (!fronts_ || end.root.face < 0 || !fronts_->front(key)) return straight;
    const Vec3f d = end.rest - rest_[key];
    const float straight2 = d.dot(d);
    const SurfacePath path = fronts_->path(key, SurfacePathEnd(end.root, end.rest), straight2);
    if (path.kind == SurfacePath::Kind::Straight) return std::sqrt(straight2);
    if (path.kind == SurfacePath::Kind::Bent) return path.distance;
    return straight;  // another piece of the scalp
}

float LengthField::scaleAt(const Vec3f& point) const {
    if (logScales_.empty()) return 1.0f;
    const End end = endAt(point);
    double sum = 0.0, weights = 0.0;
    float nearest = kInf;
    for (std::size_t k = 0; k < logScales_.size(); ++k) {
        const float d = distance(k, end);
        if (!(d > 0.0f)) return std::exp(logScales_[k]);  // on a key: its scale
        nearest = std::min(nearest, d);
        const double w = 1.0 / (static_cast<double>(d) * d);
        sum += w * logScales_[k];
        weights += w;
    }
    float fade = 1.0f;
    if (std::isfinite(spacing_) && spacing_ > 0.0f && nearest > spacing_) {
        fade = 1.0f - smoothstep01((nearest - spacing_) / (kFadeOver * spacing_));
    }
    return std::exp(static_cast<float>(sum / weights) * fade);
}

LengthKeySet mirroredLengthKeys(const LengthKeySet& keys, const ScalpMesh* scalp,
                                const std::function<Vec3f(const Vec3f&)>& mirror) {
    LengthKeySet out = keys;
    for (LengthKey& key : out.keys) {
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
    return out;
}

float nearestGuideLength(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides, const Vec3f& point) {
    float nearest = kInf, length = 0.0f;
    for (const GuideId id : guides) {
        const HierarchyGuide* g = hierarchy.guide(id);
        if (!g || g->points.size() < 2) continue;
        const Vec3f d = g->points[0] - point;
        if (d.dot(d) < nearest) {
            nearest = d.dot(d);
            length = arcLength(g->points);
        }
    }
    return length;
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

std::vector<LengthResult> applyLengthField(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides,
                                           const LengthField& field, LengthChange change) {
    std::vector<LengthResult> out;
    for (const GuideId id : guides) {
        const HierarchyGuide* g = hierarchy.guide(id);
        if (!g || g->points.size() < 2) continue;
        const float scale = field.scaleAt(g->points[0]);
        if (!(scale > 0.0f) || std::abs(scale - 1.0f) <= 1e-6f) continue;
        const float own = arcLength(g->points);
        if (!(own > 0.0f)) continue;
        out.push_back({id, guideAtLength(g->points, own * scale, change)});
    }
    return out;
}

}  // namespace groomlab
