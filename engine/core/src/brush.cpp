// SPDX-License-Identifier: Apache-2.0
#include "groomlab/brush.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/guide_selection.h"
#include "groomlab/noise.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/scalp_mirror.h"

namespace groomlab {

const char* paintModeName(PaintMode mode) {
    switch (mode) {
        case PaintMode::Replace: return "replace";
        case PaintMode::Add: return "add";
        case PaintMode::Subtract: return "subtract";
        case PaintMode::ClampMaximum: return "clampMaximum";
    }
    return "replace";
}

PaintMode paintModeFromString(const std::string& name) {
    if (name.empty() || name == "replace") return PaintMode::Replace;
    if (name == "add") return PaintMode::Add;
    if (name == "subtract") return PaintMode::Subtract;
    if (name == "clampMaximum" || name == "clamp") return PaintMode::ClampMaximum;
    throw std::invalid_argument("paint mode takes replace, add, subtract or clampMaximum");
}

StrokeSymmetry StrokeSymmetry::of(const BrushSettings& s, const ScalpMesh* scalp) {
    StrokeSymmetry out;
    out.scalp = scalp;
    if (s.mirrorTopology) {
        if (scalp && scalp->triangleCount() > 0) out.mirror = ScalpMirror::of(*scalp);
        return out;
    }
    out.axis = s.mirrorAxis;
    out.center = s.mirrorCenter;
    return out;
}

Vec3f StrokeSymmetry::point(const Vec3f& p) const {
    if (mirror) return mirror->point(*scalp, p);
    return axis >= 0 && axis <= 2 ? mirrorPoint(p, axis, center) : p;
}

Vec3f StrokeSymmetry::vector(const Vec3f& at, const Vec3f& v) const {
    if (mirror) return mirror->vector(*scalp, at, v);
    return axis >= 0 && axis <= 2 ? mirrorVector(v, axis) : v;
}

std::vector<Vec3f> symmetrySeam(const ScalpMesh& scalp, const StrokeSymmetry& sym, float lift) {
    std::vector<Vec3f> out;
    if (!sym.on()) return out;
    const std::vector<Vec3f>& points = scalp.points();
    const auto& triangles = scalp.triangles();
    auto liftOf = [&](const ScalpMesh::Triangle& t) {
        const Vec3f n = (points[t[1]] - points[t[0]]).cross(points[t[2]] - points[t[0]]);
        const float length = n.length();
        return length > 0.0f ? n * (lift / length) : Vec3f();
    };
    auto valid = [&](const ScalpMesh::Triangle& t) {
        for (const std::int32_t v : t) {
            if (v < 0 || static_cast<std::size_t>(v) >= points.size()) return false;
        }
        return true;
    };
    if (sym.topology()) {
        // The edges both of whose vertices are their own partner: the middle loop.
        const std::vector<int>& pairs = sym.mirror->pairs();
        if (!sym.mirror->fits(scalp) || pairs.size() != points.size()) return out;
        std::set<std::pair<std::int32_t, std::int32_t>> seen;
        for (const ScalpMesh::Triangle& t : triangles) {
            if (!valid(t)) continue;
            for (int k = 0; k < 3; ++k) {
                const std::int32_t a = t[k], b = t[(k + 1) % 3];
                if (pairs[a] != a || pairs[b] != b || !seen.insert({std::min(a, b), std::max(a, b)}).second) continue;
                const Vec3f up = liftOf(t);
                out.push_back(points[a] + up);
                out.push_back(points[b] + up);
            }
        }
        return out;
    }
    // A world plane: each triangle it crosses gives the segment between its two edge crossings.
    auto side = [&](const Vec3f& p) {
        const float c = sym.axis == 0 ? p.x : sym.axis == 1 ? p.y : p.z;
        return c - sym.center;
    };
    for (const ScalpMesh::Triangle& t : triangles) {
        if (!valid(t)) continue;
        const float d[3] = {side(points[t[0]]), side(points[t[1]]), side(points[t[2]])};
        Vec3f cut[2];
        int found = 0;
        for (int k = 0; k < 3 && found < 2; ++k) {
            const int j = (k + 1) % 3;
            if ((d[k] >= 0.0f) == (d[j] >= 0.0f)) continue;
            const float f = d[k] / (d[k] - d[j]);
            cut[found++] = points[t[k]] + (points[t[j]] - points[t[k]]) * f;
        }
        if (found != 2) continue;
        const Vec3f up = liftOf(t);
        out.push_back(cut[0] + up);
        out.push_back(cut[1] + up);
    }
    return out;
}

float BrushFalloff::operator()(float distance, float radius) const {
    if (radius <= 0.0f || distance >= radius) return 0.0f;
    const float x = distance / radius;
    if (profile && !profile->keys.empty()) return std::clamp((*profile)(x), 0.0f, 1.0f);
    const float h = std::clamp(hardness, 0.0f, 1.0f);
    if (h >= 1.0f || x <= h) return 1.0f;
    const float t = (x - h) / (1.0f - h);
    const float s = 1.0f - t * t;
    return s * s;
}

float brushFalloff(float distance, float radius, const BrushFalloff& falloff) { return falloff(distance, radius); }

BrushWeightFn sphereWeight(const Vec3f& center, float radius, const BrushFalloff& falloff) {
    return [center, radius, falloff](const Vec3f& p) { return falloff((p - center).length(), radius); };
}

float brushWeight(const Vec3f& p, const Vec3f& center, float radius, const BrushFalloff& falloff, const BrushWeightFn& custom) {
    return custom ? std::clamp(custom(p), 0.0f, 1.0f) : falloff((p - center).length(), radius);
}

namespace {

constexpr float kPerStampRate = 0.05f;  // Length and the Shape brush's width change per stamp at strength 1
constexpr float kFlattenRate = 0.1f;    // Shape on flatness: share of the way to flat (or round) per stamp at strength 1
constexpr float kTwistRate = 0.1f;      // radians per stamp at strength 1
constexpr float kCurlRate = 0.25f;      // radians of bend per stamp over a whole guide at strength 1
constexpr float kAttractRate = 0.1f;    // fraction of the way to the center per stamp, at the tip
constexpr float kDirectionRate = 0.5f;  // share of the turn toward the stroke per stamp at strength 1, full weight
constexpr float kNoiseRate = 0.02f;     // jitter added per stamp at strength 1, as a share of the guide's length
constexpr float kNoiseWaves = 3.0f;     // the jitter's waves along a guide (about)

std::vector<float> segmentLengths(const std::vector<Vec3f>& pts) {
    std::vector<float> len(pts.size(), 0.0f);
    for (std::size_t i = 1; i < pts.size(); ++i) len[i] = (pts[i] - pts[i - 1]).length();
    return len;
}

// Follow-the-leader: rebuild from the root, keeping each segment's original
// length and taking its direction from the moved points.
std::vector<Vec3f> keepLengths(const std::vector<Vec3f>& moved, const std::vector<float>& lengths) {
    std::vector<Vec3f> out(moved.size());
    out[0] = moved[0];
    for (std::size_t i = 1; i < moved.size(); ++i) {
        Vec3f dir = (moved[i] - out[i - 1]).normalized();
        if (dir.length() == 0.0f) dir = (moved[i] - moved[i - 1]).normalized();
        out[i] = out[i - 1] + dir * lengths[i];
    }
    return out;
}

// Weight of one point: the override if given, else the world-space sphere.
struct Weigher {
    Vec3f center;
    float radius;
    BrushWeightFn custom;
    BrushFalloff falloff;
    float operator()(const Vec3f& p) const { return brushWeight(p, center, radius, falloff, custom); }
};

template <class Weigh>
float guideWeight(const std::vector<Vec3f>& pts, const Weigh& weigh) {
    float w = 0.0f;
    for (const Vec3f& p : pts) w = std::max(w, weigh(p));
    return w;
}

bool inHiddenGroup(const GuideHierarchy& h, const HierarchyGuide& g, const BrushSettings& s) {
    if (s.hiddenGroups.empty()) return false;
    const GuideGroup* grp = h.group(g.group);
    return grp && s.hiddenGroups.count(grp->name) > 0;
}

Vec3f rootNormal(const HierarchyGuide& g, const ScalpMesh* scalp) {
    const std::vector<Vec3f>& pts = g.points;
    const bool rooted = scalp && g.root.face >= 0 && static_cast<std::size_t>(g.root.face) < scalp->triangleCount();
    return rooted ? scalp->normal(g.root) : (pts[1] - pts[0]).normalized();
}

// The strength profile at each point of a guide, by its length (root 0, tip 1); empty without a
// profile (every mode then runs as it always has, multiplying by nothing).
std::vector<float> profileAlong(const std::vector<Vec3f>& pts, const BrushSettings& s) {
    if (!s.strengthProfile || s.strengthProfile->keys.empty() || pts.size() < 2) return {};
    const std::vector<float> len = segmentLengths(pts);
    float total = 0.0f;
    for (const float l : len) total += l;
    std::vector<float> out(pts.size());
    float along = 0.0f;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        along += len[i];
        out[i] = (*s.strengthProfile)(total > 0.0f ? along / total : static_cast<float>(i) / static_cast<float>(pts.size() - 1));
    }
    return out;
}

// What one stamp does to one guide.
struct Edit {
    std::vector<Vec3f> points;  // new shape, or empty
    bool tubeEdited = false;    // the Shape brush's: `tube` is the guide's new tube
    TubeProfile tube;
    float attribute = std::numeric_limits<float>::quiet_NaN();
    float attributeWeight = 0.0f;
    std::vector<float> pointWeights;  // pointwise Attribute: how far each point moves toward the value, or empty
    bool erase = false;
};

template <class Weigh>
Edit brushGuide(const HierarchyGuide& g, const ScalpMesh* scalp, const BrushSettings& s, const Weigh& weigh,
                const Vec3f& center, const Vec3f& drag) {
    Edit e;
    const std::vector<Vec3f>& pts = g.points;
    const std::size_t n = pts.size();
    if (n < 2) return e;
    // Root to tip (BrushSettings::strengthProfile): a factor per point, or none.
    const std::vector<float> rootToTip = profileAlong(pts, s);
    const bool profiled = !rootToTip.empty();

    switch (s.mode) {
        case BrushMode::Comb: {
            if (drag.length() == 0.0f) return e;
            std::vector<Vec3f> moved;  // the points, copied once one moves
            for (std::size_t i = 1; i < n; ++i) {
                float w = weigh(pts[i]);
                if (profiled) w *= rootToTip[i];
                if (w <= 0.0f) continue;
                if (moved.empty()) moved = pts;
                moved[i] += drag * (s.strength * w);
            }
            if (moved.empty()) return e;
            // Stretch: the guide takes the length the drag gives it (points respaced evenly);
            // else each segment keeps its length.
            e.points = s.stretch ? resampleByArcLength(moved.data(), n, n) : keepLengths(moved, segmentLengths(pts));
            return e;
        }
        case BrushMode::Smooth: {
            std::vector<Vec3f> moved;  // the points, copied once one moves
            for (std::size_t i = 1; i < n; ++i) {
                // Laplacian step capped at 0.5: larger steps overshoot alternating kinks.
                float w = 0.5f * std::clamp(s.strength, 0.0f, 1.0f) * weigh(pts[i]);
                if (profiled) w *= std::clamp(rootToTip[i], 0.0f, 1.0f);
                if (w <= 0.0f) continue;
                const Vec3f target = i + 1 < n ? (pts[i - 1] + pts[i + 1]) * 0.5f
                                               : (n >= 3 ? pts[i - 1] * 2.0f - pts[i - 2] : pts[i]);
                if (moved.empty()) moved = pts;
                moved[i] = pts[i] + (target - pts[i]) * w;
            }
            if (!moved.empty()) e.points = keepLengths(moved, segmentLengths(pts));
            return e;
        }
        case BrushMode::Lift: {
            const float w = guideWeight(pts, weigh) * std::clamp(s.strength, -1.0f, 1.0f) * 0.5f;
            if (w == 0.0f) return e;
            const Vec3f up = rootNormal(g, scalp);
            std::vector<Vec3f> moved(n);
            moved[0] = pts[0];
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f d = pts[i] - pts[i - 1];
                const float len = d.length();
                Vec3f dir = d.normalized();
                const float wi = profiled ? w * std::clamp(rootToTip[i], 0.0f, 1.0f) : w;
                if (wi > 0.0f) {
                    dir = (dir * (1.0f - wi) + up * wi).normalized();  // toward the normal
                } else {
                    // Lay down: remove part of the normal component.
                    dir = (dir - up * (dir.dot(up) * -wi)).normalized();
                }
                moved[i] = moved[i - 1] + dir * len;
            }
            e.points = std::move(moved);
            return e;
        }
        case BrushMode::Length: {
            const float w = guideWeight(pts, weigh);
            if (w <= 0.0f || s.strength == 0.0f) return e;
            const float factor = std::max(0.05f, 1.0f + s.strength * w * kPerStampRate);
            if (factor < 1.0f) {
                e.points = resampleByArcLength(pts.data(), n, n, factor);
                return e;
            }
            // Grow: extend the tip along its last direction, then respace.
            std::vector<Vec3f> extended = pts;
            const float total = arcLength(pts.data(), n);
            extended.push_back(pts[n - 1] + (pts[n - 1] - pts[n - 2]).normalized() * (total * (factor - 1.0f)));
            e.points = resampleByArcLength(extended.data(), extended.size(), n);
            return e;
        }
        case BrushMode::ShapeTube: {
            // Section by section: the painted list resampled to one value per guide point, then
            // each point's value moved by the brush's weight there (and the root-to-tip profile),
            // so a stroke shapes the tube near the cursor and leaves the rest of it as it was.
            if (s.strength == 0.0f) return e;
            std::vector<float> weights(n, 0.0f);
            bool touched = false;
            for (std::size_t i = 0; i < n; ++i) {
                weights[i] = weigh(pts[i]) * (profiled ? std::max(rootToTip[i], 0.0f) : 1.0f);
                touched = touched || weights[i] > 0.0f;
            }
            if (!touched) return e;
            if (s.smoothTube) {
                // Shift+drag: the channel smoothed along the guide where the brush weighs it, each
                // section toward its neighbours' mean (smoothedTube), the root's and tip's kept.
                const unsigned which = s.tubeChannel == TubeChannel::Width      ? kTubeWidthList
                                       : s.tubeChannel == TubeChannel::Flatness ? kTubeAspectList
                                                                                : kTubeTwistList;
                e.tube = smoothedTube(g.tube, which, n, 1, std::min(1.0f, std::fabs(s.strength)), &weights);
                e.tubeEdited = e.tube != g.tube;
                return e;
            }
            e.tube = g.tube;
            std::vector<float>* list = s.tubeChannel == TubeChannel::Width      ? &e.tube.width
                                       : s.tubeChannel == TubeChannel::Flatness ? &e.tube.aspect
                                                                                : &e.tube.twist;
            std::vector<float> perPoint(n);
            for (std::size_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                perPoint[i] = s.tubeChannel == TubeChannel::Width      ? g.tube.widthAt(t)
                              : s.tubeChannel == TubeChannel::Flatness ? g.tube.aspectAt(t)
                                                                       : g.tube.twistAt(t);
            }
            const float k = std::clamp(s.strength, -1.0f, 1.0f);
            for (std::size_t i = 0; i < n; ++i) {
                const float w = weights[i];
                if (w <= 0.0f) continue;
                float& v = perPoint[i];
                switch (s.tubeChannel) {
                    case TubeChannel::Width: v *= std::max(0.05f, 1.0f + k * w * kPerStampRate); break;
                    case TubeChannel::Flatness: {
                        // Toward kMinTubeAspect (flat), or back toward 1 (as drawn) from either side.
                        const float target = k > 0.0f ? kMinTubeAspect : 1.0f;
                        v += (target - v) * std::min(1.0f, std::fabs(k) * w * kFlattenRate);
                        break;
                    }
                    case TubeChannel::Twist: v += k * w * kTwistRate; break;
                }
            }
            *list = std::move(perPoint);
            e.tubeEdited = *list != (s.tubeChannel == TubeChannel::Width      ? g.tube.width
                                     : s.tubeChannel == TubeChannel::Flatness ? g.tube.aspect
                                                                              : g.tube.twist);
            return e;
        }
        case BrushMode::Attribute: {
            const float strength = std::clamp(s.strength, 0.0f, 1.0f);
            if (s.pointwise) {
                // A band along the guide: each point by the brush's weight there.
                // The layer itself is built in stamp(), which knows the hierarchy.
                bool touched = false;
                std::vector<float> weights(n, 0.0f);
                for (std::size_t i = 0; i < n; ++i) {
                    weights[i] = weigh(pts[i]) * strength;
                    touched = touched || weights[i] > 0.0f;
                }
                if (touched) e.pointWeights = std::move(weights);
                return e;
            }
            const float w = guideWeight(pts, weigh) * strength;
            if (w <= 0.0f) return e;
            e.attributeWeight = w;
            auto it = g.attributes.find(s.attribute);
            const float current = it == g.attributes.end() ? 0.0f : it->second;
            e.attribute = current + (compositePaintValue(current, s.value, s.paintMode) - current) * w;
            return e;
        }
        case BrushMode::Twist: {
            const float angle = guideWeight(pts, weigh) * std::clamp(s.strength, -1.0f, 1.0f) * kTwistRate;
            if (angle == 0.0f) return e;
            const Vec3f axis = rootNormal(g, scalp);
            e.points.resize(n);
            if (!profiled) {
                for (std::size_t i = 0; i < n; ++i) e.points[i] = pts[0] + rotateAxisAngle(pts[i] - pts[0], axis, angle);
                return e;
            }
            // Each point swung by its share along the guide, then the segments their lengths again.
            for (std::size_t i = 0; i < n; ++i) e.points[i] = pts[0] + rotateAxisAngle(pts[i] - pts[0], axis, angle * rootToTip[i]);
            e.points = keepLengths(e.points, segmentLengths(pts));
            return e;
        }
        case BrushMode::Curl: {
            // Bend about the guide's side axis: each point turns the rest of the
            // guide by its share of the curl, so the tip curls most.
            const Vec3f dir0 = (pts[1] - pts[0]).normalized();
            Vec3f axis = dir0.cross(rootNormal(g, scalp));
            if (axis.length() < 1e-3f) axis = dir0.cross(g.frameUp);
            if (axis.length() < 1e-6f) return e;
            axis = axis.normalized();
            const float rate = std::clamp(s.strength, -1.0f, 1.0f) * kCurlRate / static_cast<float>(n - 1);
            std::vector<Vec3f> out;  // the points, copied once one turns
            for (std::size_t i = 1; i + 1 < n; ++i) {
                float angle = rate * weigh(pts[i]);
                if (profiled) angle *= rootToTip[i];
                if (angle == 0.0f) continue;
                if (out.empty()) out = pts;
                for (std::size_t j = i + 1; j < n; ++j) out[j] = out[i] + rotateAxisAngle(out[j] - out[i], axis, angle);
            }
            if (!out.empty()) e.points = std::move(out);
            return e;
        }
        case BrushMode::Slide: {
            // The root moves with the drag, projected onto the scalp; the guide
            // keeps its shape, turned from the old root normal to the new one.
            const float w = weigh(pts[0]) * std::clamp(s.strength, 0.0f, 1.0f);
            if (w <= 0.0f || drag.length() == 0.0f || !scalp || scalp->triangleCount() == 0) return e;
            const SurfacePoint to = scalp->closestPoint(pts[0] + drag * w);
            if (to.root.face < 0) return e;
            const Vec3f n0 = rootNormal(g, scalp).normalized(), n1 = to.normal.normalized();
            e.points.resize(n);
            for (std::size_t i = 0; i < n; ++i) e.points[i] = to.position + rotateBetween(pts[i] - pts[0], n0, n1);
            return e;
        }
        case BrushMode::Straighten: {
            const float k = 0.5f * std::clamp(s.strength, 0.0f, 1.0f);
            const Vec3f dir0 = (pts[1] - pts[0]).normalized();
            std::vector<Vec3f> moved;  // the points, copied once one moves
            float length = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                length += (pts[i] - pts[i - 1]).length();
                float w = k * weigh(pts[i]);
                if (profiled) w *= std::clamp(rootToTip[i], 0.0f, 1.0f);
                if (w <= 0.0f) continue;
                if (moved.empty()) moved = pts;
                moved[i] = pts[i] + (pts[0] + dir0 * length - pts[i]) * w;
            }
            if (!moved.empty()) e.points = keepLengths(moved, segmentLengths(pts));
            return e;
        }
        case BrushMode::Attract: {
            const float k = std::clamp(s.strength, -1.0f, 1.0f) * kAttractRate;
            std::vector<Vec3f> moved;  // the points, copied once one moves
            for (std::size_t i = 1; i < n; ++i) {
                float w = k * weigh(pts[i]) * static_cast<float>(i) / static_cast<float>(n - 1);
                if (profiled) w *= rootToTip[i];
                if (w == 0.0f) continue;
                if (moved.empty()) moved = pts;
                moved[i] = pts[i] + (center - pts[i]) * w;
            }
            if (!moved.empty()) e.points = keepLengths(moved, segmentLengths(pts));
            return e;
        }
        case BrushMode::Direction: {
            // The stroke's direction on the root's tangent plane, and the guide's (root to tip): the
            // guide turns about its root normal by a share of the angle between them.
            if (drag.length() == 0.0f) return e;
            const Vec3f up = rootNormal(g, scalp).normalized();
            Vec3f want = drag - up * drag.dot(up);
            Vec3f have = pts[n - 1] - pts[0];
            have = have - up * have.dot(up);
            if (want.length() < 1e-9f || have.length() < 1e-6f) return e;  // standing up: no direction to turn
            want = (s.strength < 0.0f ? -want : want).normalized();
            have = have.normalized();
            const float full = pm::atan2(have.cross(want).dot(up), have.dot(want));
            const float angle = full * std::min(1.0f, std::fabs(s.strength) * guideWeight(pts, weigh)) * kDirectionRate;
            if (angle == 0.0f) return e;
            e.points.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                e.points[i] = pts[0] + rotateAxisAngle(pts[i] - pts[0], up, profiled ? angle * rootToTip[i] : angle);
            }
            if (profiled) e.points = keepLengths(e.points, segmentLengths(pts));
            return e;
        }
        case BrushMode::Noise: {
            std::vector<Vec3f> moved;  // the points, copied once one moves
            if (s.strength < 0.0f) {
                // Negative (Ctrl): smoothed out, as Smooth does.
                for (std::size_t i = 1; i < n; ++i) {
                    float w = 0.5f * std::min(-s.strength, 1.0f) * weigh(pts[i]);
                    if (profiled) w *= std::clamp(rootToTip[i], 0.0f, 1.0f);
                    if (w <= 0.0f) continue;
                    const Vec3f target = i + 1 < n ? (pts[i - 1] + pts[i + 1]) * 0.5f
                                                   : (n >= 3 ? pts[i - 1] * 2.0f - pts[i - 2] : pts[i]);
                    if (moved.empty()) moved = pts;
                    moved[i] = pts[i] + (target - pts[i]) * w;
                }
                if (!moved.empty()) e.points = keepLengths(moved, segmentLengths(pts));
                return e;
            }
            // A smooth random offset across the guide, the same pattern for a guide every stamp
            // (seeded by its ID), so strokes build it up; calm at the root.
            const float total = arcLength(pts.data(), n);
            if (total <= 0.0f || s.strength == 0.0f) return e;
            const std::uint64_t seed = hashCombine(hash64(0x9015EB2u), static_cast<std::uint64_t>(g.id));
            const float k = std::min(s.strength, 1.0f) * kNoiseRate * total;
            float length = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                length += (pts[i] - pts[i - 1]).length();
                float w = weigh(pts[i]);
                if (profiled) w *= rootToTip[i];
                if (w <= 0.0f) continue;
                const float t = length / total;
                Vec3f offset = vectorNoise(Vec3f(t * kNoiseWaves, 0.5f, 0.25f), seed, 2);
                const Vec3f along = (pts[std::min(i + 1, n - 1)] - pts[i - 1]).normalized();
                offset = offset - along * offset.dot(along);
                if (moved.empty()) moved = pts;
                moved[i] = pts[i] + offset * (k * w * std::min(1.0f, 4.0f * t));
            }
            if (!moved.empty()) e.points = keepLengths(moved, segmentLengths(pts));
            return e;
        }
        case BrushMode::Erase:
            e.erase = guideWeight(pts, weigh) * std::clamp(s.strength, 0.0f, 1.0f) >= 0.5f;
            return e;
        case BrushMode::Cut: {
            // Cut where the brush first reaches the guide from the root; strength
            // lowers the weight needed (strength 1: half way into the falloff).
            const float threshold = 0.5f / std::max(std::clamp(s.strength, 0.0f, 1.0f), 1e-3f);
            float along = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                const float segment = (pts[i] - pts[i - 1]).length();
                if (weigh(pts[i]) >= threshold) {
                    // Back off to where the segment enters the brush, roughly: its middle.
                    const float keep = along + (weigh(pts[i - 1]) >= threshold ? 0.0f : 0.5f * segment);
                    const float total = arcLength(pts.data(), n);
                    const float factor = std::max(keep / total, 0.02f);
                    if (factor < 0.999f) e.points = resampleByArcLength(pts.data(), n, n, factor);
                    return e;
                }
                along += segment;
            }
            return e;
        }
    }
    return e;
}

// The custom weight for a mirrored stamp: a point weighs what its mirror image
// weighs in the original. Empty for an empty weight.
BrushWeightFn mirroredWeight(const BrushWeightFn& weight, int axis, float center) {
    if (!weight) return {};
    return [weight, axis, center](const Vec3f& p) { return weight(mirrorPoint(p, axis, center)); };
}

// Scales a weigher's result by a guide's mask weight.
struct MaskedWeigher {
    const Weigher& base;
    float scale;
    float operator()(const Vec3f& p) const { return base(p) * scale; }
};

// The guides a brush edits, each once, with their mask weight (freeze applied).
std::vector<std::pair<GuideId, float>> collectTargets(const GuideHierarchy& h, const BrushSettings& s) {
    const bool freezes = !s.freeze.empty() && h.hasAttributes() &&
                         !(s.mode == BrushMode::Attribute && s.attribute == s.freeze);
    // A guide's weight `m` held back by its freeze value.
    auto unfrozen = [&](const HierarchyGuide& g, float m) {
        float frozen = 0.0f;
        if (freezes && h.effectiveAttribute(g, s.freeze, &frozen)) m *= 1.0f - std::clamp(frozen, 0.0f, 1.0f);
        return m;
    };
    std::vector<std::pair<GuideId, float>> targets;
    std::vector<std::pair<GuideId, float>> frontier;  // targets with their mask weight, for propagate
    std::unordered_set<GuideId> reached;
    for (const GuideId id : guidesAt(h, s.level, s.group)) {
        const HierarchyGuide& g = *h.guide(id);
        if (inHiddenGroup(h, g, s)) continue;
        float m = 1.0f;
        if (s.mask) {
            auto it = s.mask->find(g.id);
            if (it == s.mask->end() || it->second <= 0.0f) continue;
            m = std::min(it->second, 1.0f);
        }
        const float w = unfrozen(g, m);
        if (w <= 0.0f) continue;
        targets.emplace_back(g.id, w);
        frontier.emplace_back(g.id, m);
        reached.insert(g.id);
    }
    if (s.propagate) {
        // Locked descendants do not follow their parents, so brush them too (with
        // their ancestor's mask weight), unless hidden or frozen themselves. A
        // descendant that is a target already is not added again.
        std::unordered_map<GuideId, std::vector<GuideId>> children;  // once, not a scan per guide
        for (const HierarchyGuide& g : h.guides()) {
            if (g.isChild()) children[g.parent].push_back(g.id);
        }
        while (!frontier.empty()) {
            std::vector<std::pair<GuideId, float>> next;
            for (const auto& [id, m] : frontier) {
                auto it = children.find(id);
                if (it == children.end()) continue;
                for (const GuideId child : it->second) {
                    if (!reached.insert(child).second) continue;
                    next.emplace_back(child, m);
                    const HierarchyGuide& g = *h.guide(child);
                    if (!g.locked || inHiddenGroup(h, g, s)) continue;
                    const float w = unfrozen(g, m);
                    if (w > 0.0f) targets.emplace_back(child, w);
                }
            }
            frontier.swap(next);
        }
    }
    return targets;
}

}  // namespace

// ---- A stroke's guides and their bounds ------------------------------------------------

struct BrushStrokeCache::State {
    bool filled = false;
    const GuideHierarchy* hierarchy = nullptr;
    std::size_t guideCount = 0;
    std::uint64_t key = 0;
    std::vector<std::pair<GuideId, float>> targets;  // collectTargets, in its order
    // Per target: its points' bounds (lo > hi for a guide without points), and whether they are
    // out of date (the guide or an ancestor was edited).
    std::vector<Vec3f> lo, hi;
    std::vector<char> stale;
    // Attribute modes that offset or clamp a value composite against the value at stroke start.
    // The strongest dab wins, so overlapping dabs never add the amount repeatedly.
    std::vector<float> attributeStart, attributeWeight;
    bool anyStale = true;
    // Targets binned by where their roots were when the cache was filled: a cluster's bounds hold
    // its members', so a stamp skips a whole cluster out of its reach with one test.
    std::vector<std::uint32_t> cluster;  // per target
    std::vector<std::uint32_t> clusterStart, clusterMembers;
    std::vector<Vec3f> clusterLo, clusterHi;
    std::vector<char> clusterStale;
    std::unordered_map<GuideId, std::uint32_t> slot;            // guide -> target index
    std::unordered_map<GuideId, std::vector<GuideId>> children;  // for the descendants of edited guides

    // What picks the guides: the level or group, mask, hidden groups, freeze, propagate.
    static std::uint64_t keyOf(const GuideHierarchy& h, const BrushSettings& s) {
        std::uint64_t k = hashCombine(hash64(0xB2C5A7), static_cast<std::uint64_t>(static_cast<std::int64_t>(s.level)));
        k = hashString(k, s.group);
        k = hashCombine(k, reinterpret_cast<std::uintptr_t>(s.mask));
        k = hashCombine(k, s.mask ? s.mask->size() : 0);
        for (const std::string& g : s.hiddenGroups) k = hashString(k, g);
        k = hashString(k, s.freeze);
        k = hashCombine(k, s.propagate ? 1u : 0u);
        const bool freezes = !s.freeze.empty() && h.hasAttributes() && !(s.mode == BrushMode::Attribute && s.attribute == s.freeze);
        return hashCombine(k, freezes ? 1u : 0u);
    }

    void prepare(const GuideHierarchy& h, const BrushSettings& s) {
        const std::uint64_t k = keyOf(h, s);
        if (filled && hierarchy == &h && guideCount == h.guides().size() && key == k) return;
        filled = true;
        hierarchy = &h;
        guideCount = h.guides().size();
        key = k;
        targets = collectTargets(h, s);
        const std::size_t n = targets.size();
        lo.assign(n, Vec3f{});
        hi.assign(n, Vec3f{});
        stale.assign(n, 1);
        attributeStart.assign(n, 0.0f);
        attributeWeight.assign(n, 0.0f);
        anyStale = true;
        slot.clear();
        for (std::size_t i = 0; i < n; ++i) {
            slot.emplace(targets[i].first, static_cast<std::uint32_t>(i));
            if (const HierarchyGuide* g = h.guide(targets[i].first)) {
                const auto it = g->attributes.find(s.attribute);
                attributeStart[i] = it == g->attributes.end() ? 0.0f : it->second;
            }
        }
        children.clear();
        for (const HierarchyGuide& g : h.guides()) {
            if (g.isChild()) children[g.parent].push_back(g.id);
        }
        // Clusters: a grid of up to 8 x 8 x 8 over the targets' roots.
        const float big = std::numeric_limits<float>::max();
        Vec3f rlo(big, big, big), rhi(-big, -big, -big);
        std::vector<Vec3f> roots(n);
        for (std::size_t i = 0; i < n; ++i) {
            const HierarchyGuide* g = h.guide(targets[i].first);
            roots[i] = g && !g->points.empty() ? g->points.front() : Vec3f{};
            rlo = Vec3f(std::min(rlo.x, roots[i].x), std::min(rlo.y, roots[i].y), std::min(rlo.z, roots[i].z));
            rhi = Vec3f(std::max(rhi.x, roots[i].x), std::max(rhi.y, roots[i].y), std::max(rhi.z, roots[i].z));
        }
        constexpr int kAlong = 8;
        const Vec3f extent = n ? rhi - rlo : Vec3f{};
        auto bin = [&](float v, float l, float e) {
            return e > 0.0f ? std::clamp(static_cast<int>((v - l) / e * kAlong), 0, kAlong - 1) : 0;
        };
        cluster.assign(n, 0);
        const std::size_t clusters = static_cast<std::size_t>(kAlong) * kAlong * kAlong;
        clusterStart.assign(clusters + 1, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const int c = (bin(roots[i].z, rlo.z, extent.z) * kAlong + bin(roots[i].y, rlo.y, extent.y)) * kAlong +
                          bin(roots[i].x, rlo.x, extent.x);
            cluster[i] = static_cast<std::uint32_t>(c);
            ++clusterStart[static_cast<std::size_t>(c) + 1];
        }
        for (std::size_t c = 0; c < clusters; ++c) clusterStart[c + 1] += clusterStart[c];
        clusterMembers.resize(n);
        std::vector<std::uint32_t> fill(clusterStart.begin(), clusterStart.end() - 1);
        for (std::size_t i = 0; i < n; ++i) clusterMembers[fill[cluster[i]]++] = static_cast<std::uint32_t>(i);
        clusterLo.assign(clusters, Vec3f{});
        clusterHi.assign(clusters, Vec3f{});
        clusterStale.assign(clusters, 1);
    }

    // Measures the targets that moved, and their clusters.
    void refresh(const GuideHierarchy& h) {
        if (!anyStale) return;
        anyStale = false;
        const float big = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (!stale[i]) continue;
            stale[i] = 0;
            Vec3f l(big, big, big), u(-big, -big, -big);
            if (const HierarchyGuide* g = h.guide(targets[i].first)) {
                for (const Vec3f& p : g->points) {
                    l = Vec3f(std::min(l.x, p.x), std::min(l.y, p.y), std::min(l.z, p.z));
                    u = Vec3f(std::max(u.x, p.x), std::max(u.y, p.y), std::max(u.z, p.z));
                }
            }
            lo[i] = l;
            hi[i] = u;
        }
        for (std::size_t c = 0; c + 1 < clusterStart.size(); ++c) {
            if (!clusterStale[c]) continue;
            clusterStale[c] = 0;
            Vec3f l(big, big, big), u(-big, -big, -big);
            for (std::uint32_t k = clusterStart[c]; k < clusterStart[c + 1]; ++k) {
                const std::uint32_t i = clusterMembers[k];
                l = Vec3f(std::min(l.x, lo[i].x), std::min(l.y, lo[i].y), std::min(l.z, lo[i].z));
                u = Vec3f(std::max(u.x, hi[i].x), std::max(u.y, hi[i].y), std::max(u.z, hi[i].z));
            }
            clusterLo[c] = l;
            clusterHi[c] = u;
        }
    }

    // The edited guides and their descendants moved (evaluateSubtrees).
    void moved(const std::vector<GuideId>& edited) {
        if (!filled) return;
        std::vector<GuideId> stack(edited.begin(), edited.end());
        std::unordered_set<GuideId> seen;
        while (!stack.empty()) {
            const GuideId id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second) continue;
            const auto at = slot.find(id);
            if (at != slot.end()) {
                stale[at->second] = 1;
                clusterStale[cluster[at->second]] = 1;
                anyStale = true;
            }
            const auto it = children.find(id);
            if (it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
    }
};

BrushStrokeCache::BrushStrokeCache() : state_(std::make_unique<State>()) {}
BrushStrokeCache::~BrushStrokeCache() = default;
BrushStrokeCache::BrushStrokeCache(BrushStrokeCache&&) noexcept = default;
BrushStrokeCache& BrushStrokeCache::operator=(BrushStrokeCache&&) noexcept = default;
void BrushStrokeCache::clear() { *state_ = State{}; }

namespace {

// Whether a box can hold a point of the brush sphere: its nearest point to the center is within
// the radius (with a hair of slack for the rounding of the points' distances).
bool sphereReaches(const Vec3f& center, float radius, const Vec3f& lo, const Vec3f& hi) {
    const float dx = std::max({lo.x - center.x, 0.0f, center.x - hi.x});
    const float dy = std::max({lo.y - center.y, 0.0f, center.y - hi.y});
    const float dz = std::max({lo.z - center.z, 0.0f, center.z - hi.z});
    const float limit = radius * 1.0001f + 1e-6f;
    return dx * dx + dy * dy + dz * dz <= limit * limit;
}

std::size_t stamp(GuideHierarchy& h, const ScalpMesh* scalp, const BrushSettings& s, const Vec3f& center,
                  const Vec3f& drag, const BrushWeightFn& weight, const BrushReachFn& reach, BrushStrokeCache::State* cache,
                  bool threadSafeWeight, std::set<GuideId>* changedIds, const std::set<GuideId>* skip = nullptr,
                  std::set<GuideId>* touched = nullptr) {
    const Weigher weigh{center, s.radius, weight, falloffOf(s)};
    // Collect targets first: edits below must not disturb the iteration.
    std::vector<std::pair<GuideId, float>> collected;
    if (cache) {
        cache->prepare(h, s);
    } else {
        collected = collectTargets(h, s);
    }
    const std::vector<std::pair<GuideId, float>>& targets = cache ? cache->targets : collected;
    // Guides out of the brush's reach weigh 0 at every point, and a guide whose points all weigh
    // 0 is left as it is by every mode: skipped, unweighed. The world sphere's reach is known;
    // a custom weight's comes with it (or nothing is skipped).
    const bool sphere = !weight;
    const bool culls = sphere || static_cast<bool>(reach);
    auto reaches = [&](const Vec3f& lo, const Vec3f& hi) {
        return sphere ? sphereReaches(center, s.radius, lo, hi) : reach(lo, hi);
    };
    std::vector<signed char> clusterReaches;  // per cluster: -1 not tested yet
    if (cache && culls) {
        cache->refresh(h);
        clusterReaches.assign(cache->clusterLo.size(), -1);
    }

    // The guides in reach, in the targets' order.
    std::vector<std::size_t> reached;
    reached.reserve(targets.size());
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const GuideId id = targets[k].first;
        if (skip && skip->count(id)) continue;
        const HierarchyGuide* g = h.guide(id);
        if (!g) continue;
        if (culls) {
            if (g->points.size() < 2) continue;  // no mode changes a guide of fewer points
            if (cache) {
                signed char& c = clusterReaches[cache->cluster[k]];
                if (c < 0) c = reaches(cache->clusterLo[cache->cluster[k]], cache->clusterHi[cache->cluster[k]]) ? 1 : 0;
                if (c == 0 || !reaches(cache->lo[k], cache->hi[k])) continue;
            } else {
                Vec3f lo = g->points.front(), hi = lo;
                for (const Vec3f& p : g->points) {
                    lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                    hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
                }
                if (!reaches(lo, hi)) continue;
            }
        }
        reached.push_back(k);
    }
    // Each guide's edit reads only that guide (and the scalp), as it was before the stamp: made
    // for all of them first (in parallel when the weight may be called from several threads),
    // then applied in the targets' order, as one guide after another gives them.
    const bool collides = s.collide && scalp && scalp->triangleCount() > 0;
    std::vector<Edit> edits(reached.size());
    auto make = [&](std::size_t r) {
        const std::size_t k = reached[r];
        const HierarchyGuide& g = *h.guide(targets[k].first);
        const MaskedWeigher masked{weigh, targets[k].second};
        Edit e = brushGuide(g, scalp, s, masked, center, drag);
        if (!e.erase && !e.tubeEdited && std::isnan(e.attribute) && e.pointWeights.empty() && !e.points.empty() && collides) {
            e.points = keepOffScalp(e.points, *scalp, s.collideOffset);
        }
        edits[r] = std::move(e);
    };
    if ((!weight || threadSafeWeight) && reached.size() >= 32) {
        parallelFor(0, reached.size(), make, 8);
    } else {
        for (std::size_t r = 0; r < reached.size(); ++r) make(r);
    }

    std::size_t changed = 0;
    std::vector<GuideId> edited, erased;
    for (std::size_t r = 0; r < reached.size(); ++r) {
        const GuideId id = targets[reached[r]].first;
        const HierarchyGuide* g = h.guide(id);
        Edit& e = edits[r];
        if (e.erase) {
            erased.push_back(id);
        } else if (e.tubeEdited) {
            h.setTube(id, std::move(e.tube));
            edited.push_back(id);  // children sit inside the tube
        } else if (!std::isnan(e.attribute)) {
            if (cache && s.paintMode != PaintMode::Replace) {
                const std::size_t k = reached[r];
                cache->attributeWeight[k] = std::max(cache->attributeWeight[k], e.attributeWeight);
                const float start = cache->attributeStart[k];
                const float target = compositePaintValue(start, s.value, s.paintMode);
                e.attribute = start + (target - start) * cache->attributeWeight[k];
            }
            h.setAttribute(id, s.attribute, e.attribute);
        } else if (!e.pointWeights.empty()) {
            // The guide's per-point layer (its own or inherited, resampled to its
            // points), started from its scalar attribute where it has none.
            std::vector<float> layer;
            if (!h.effectivePointAttribute(*g, s.attribute, &layer)) {
                float scalar = 0.0f;
                h.effectiveAttribute(*g, s.attribute, &scalar);
                layer.assign(g->points.size(), scalar);
            }
            for (std::size_t i = 0; i < layer.size() && i < e.pointWeights.size(); ++i) {
                layer[i] += (s.value - layer[i]) * e.pointWeights[i];
            }
            h.setPointAttribute(id, s.attribute, std::move(layer));
        } else if (!e.points.empty()) {
            h.setPoints(id, std::move(e.points));
            edited.push_back(id);
        } else {
            continue;
        }
        ++changed;
        if (changedIds) changedIds->insert(id);
        if (touched) touched->insert(id);
    }
    if (!erased.empty()) {
        h.removeGuides(erased);
        if (cache) *cache = BrushStrokeCache::State{};  // other guides now: found again
    }
    if (!edited.empty()) {
        h.evaluateSubtrees(scalp, edited);
        if (cache) cache->moved(edited);
    }
    return changed;
}

// Runs `stampAt(center, drag)` along a path (a stroke): stamps at
// most radius / 4 apart, each with the step to the next; one point: one still stamp.
template <class Stamp>
void alongPath(const BrushSettings& s, const std::vector<Vec3f>& path, const Stamp& stampAt) {
    if (path.size() == 1) {
        stampAt(path[0], Vec3f{});
        return;
    }
    const float spacing = std::max(s.radius * 0.25f, 1e-4f);
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const Vec3f segment = path[i + 1] - path[i];
        const auto steps = std::max(1, static_cast<int>(std::ceil(segment.length() / spacing)));
        const Vec3f step = segment / static_cast<float>(steps);
        for (int k = 0; k < steps; ++k) stampAt(path[i] + step * static_cast<float>(k), step);
    }
}

// A reach for the mirrored stamp: a box is reached when its mirror image is by the original.
BrushReachFn mirroredReach(const BrushReachFn& reach, int axis, float center) {
    if (!reach) return {};
    return [reach, axis, center](const Vec3f& lo, const Vec3f& hi) {
        const Vec3f a = mirrorPoint(lo, axis, center), b = mirrorPoint(hi, axis, center);
        return reach(Vec3f(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)),
                     Vec3f(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)));
    };
}

// The settings the mirrored stamp brushes with: a turn about the guide (Twist, the Shape brush's
// twist) is handed, so its mirror image turns the other way.
BrushSettings mirroredSettings(const BrushSettings& s) {
    BrushSettings out = s;
    if (s.mode == BrushMode::Twist || (s.mode == BrushMode::ShapeTube && s.tubeChannel == TubeChannel::Twist)) {
        out.strength = -s.strength;
    }
    return out;
}

// One stamp, and its mirror image when symmetry is on (skipping guides the
// first one already changed, so seam guides change once). A custom weight is
// mirrored too: the mirrored stamp weighs a point as the original weighs its
// mirror image.
std::size_t symmetricStamp(GuideHierarchy& h, const ScalpMesh* scalp, const BrushSettings& s, const Vec3f& center,
                           const Vec3f& drag, const BrushWeightFn& weight, const BrushReachFn& reach,
                           BrushStrokeCache::State* cache, bool threadSafeWeight, std::set<GuideId>* changedIds) {
    std::set<GuideId> touched;
    std::size_t changed = stamp(h, scalp, s, center, drag, weight, reach, cache, threadSafeWeight, changedIds, nullptr, &touched);
    if (s.mirrorTopology) {
        // At the partner spot across the scalp's topology: a world sphere of the stamp's radius
        // (the cursor's screen circle does not carry over a head in another pose).
        const StrokeSymmetry sym = StrokeSymmetry::of(s, scalp);
        if (!sym.on()) return changed;
        return changed + stamp(h, scalp, mirroredSettings(s), sym.point(center), sym.vector(center, drag), {}, {}, cache,
                               threadSafeWeight, changedIds, &touched, nullptr);
    }
    if (s.mirrorAxis < 0 || s.mirrorAxis > 2) return changed;
    changed += stamp(h, scalp, mirroredSettings(s), mirrorPoint(center, s.mirrorAxis, s.mirrorCenter), mirrorVector(drag, s.mirrorAxis),
                     mirroredWeight(weight, s.mirrorAxis, s.mirrorCenter), mirroredReach(reach, s.mirrorAxis, s.mirrorCenter),
                     cache, threadSafeWeight, changedIds, &touched, nullptr);
    return changed;
}

// The group new guides go in: created as a root group if missing.
GroupId targetGroup(GuideHierarchy& h, const std::string& name) {
    const std::string group = name.empty() ? "primary" : name;
    const GroupId gid = h.findGroup(group);
    return gid == kNoGroup ? h.addGroup(group) : gid;
}

// For a new guide in a child group: the guide of the parent group whose root is nearest.
GuideId nearestParent(const GuideHierarchy& h, GroupId childGroup, const Vec3f& root) {
    const GroupId parentGroup = h.group(childGroup)->parent;
    GuideId best = kNoGuide;
    float bestD2 = std::numeric_limits<float>::max();
    for (const HierarchyGuide& g : h.guides()) {
        if (g.group != parentGroup || g.points.empty()) continue;
        const float d2 = (g.points.front() - root).dot(g.points.front() - root);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = g.id;
        }
    }
    if (best == kNoGuide) throw std::invalid_argument("no parent guides to attach a child to in the parent group");
    return best;
}

// Adds a guide shaped `pts` to `gid`: a primary in a root group, else a child
// of the nearest guide of the parent group.
GuideId addToGroup(GuideHierarchy& h, GroupId gid, std::vector<Vec3f> pts, const TubeProfile& tube) {
    if (h.groupDepth(gid) == 0) return h.addGuide(gid, std::move(pts), tube);
    const GuideId parent = nearestParent(h, gid, pts.front());
    return h.addChildGuide(gid, parent, std::move(pts), tube);
}

// What a guide placed among others takes from them (neighborMatch).
struct NeighborMatch {
    std::vector<Vec3f> points;  // relative to the new root
    std::map<std::string, float> attributes;  // painted layers (as the neighbors show them), blended
};

// The guides a new guide in group `gid` learns from: its own group's, or, while
// that has none, the other groups' at the same level (a new group starts from
// the hair around it).
std::vector<GroupId> neighborGroups(const GuideHierarchy& h, GroupId gid) {
    auto hasGuides = [&](GroupId g) {
        for (const HierarchyGuide& guide : h.guides()) {
            if (guide.group == g && guide.points.size() >= 2) return true;
        }
        return false;
    };
    if (hasGuides(gid)) return {gid};
    std::vector<GroupId> others;
    const int depth = h.groupDepth(gid);
    for (const HierarchyGuide& guide : h.guides()) {
        if (guide.group == gid || guide.points.size() < 2 || h.groupDepth(guide.group) != depth) continue;
        if (std::find(others.begin(), others.end(), guide.group) == others.end()) others.push_back(guide.group);
    }
    return others;
}

struct Near {
    const HierarchyGuide* g;
    float d2;
};

// The (up to) 3 guides of `groups` nearest `at`, nearest first.
std::vector<Near> nearestOf(const GuideHierarchy& h, const std::vector<GroupId>& groups, const SurfacePoint& at) {
    std::vector<Near> near;
    for (const HierarchyGuide& g : h.guides()) {
        if (g.points.size() < 2 || std::find(groups.begin(), groups.end(), g.group) == groups.end()) continue;
        near.push_back({&g, (g.points.front() - at.position).dot(g.points.front() - at.position)});
    }
    const std::size_t k = std::min<std::size_t>(3, near.size());
    // Equally near (mirrored guides): the lower ID first, the same under every standard library.
    std::partial_sort(near.begin(), near.begin() + k, near.end(),
                      [](const Near& a, const Near& b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.g->id < b.g->id); });
    near.resize(k);
    return near;
}

// The blended shape of the (up to) 3 guides of `groups` nearest `at`, each
// turned from its root normal to at.normal, as offsets from the root. Empty
// when those groups have no guides.
std::vector<Vec3f> neighborShape(const GuideHierarchy& h, const std::vector<GroupId>& groups, const ScalpMesh& scalp,
                                 const SurfacePoint& at, int segments) {
    const std::vector<Near> near = nearestOf(h, groups, at);
    if (near.empty()) return {};
    const std::size_t count = static_cast<std::size_t>(segments) + 1;
    std::vector<Vec3f> blended(count, Vec3f{});
    float wsum = 0.0f, length = 0.0f;
    const Vec3f n1 = at.normal.normalized();
    for (const Near& n : near) {
        const HierarchyGuide& g = *n.g;
        const float w = 1.0f / (n.d2 + 1e-8f);
        const Vec3f n0 = rootNormal(g, &scalp).normalized();
        const std::vector<Vec3f> pts = resampleByArcLength(g.points.data(), g.points.size(), count);
        for (std::size_t j = 0; j < count; ++j) blended[j] += rotateBetween(pts[j] - pts[0], n0, n1) * w;
        length += arcLength(g.points.data(), g.points.size()) * w;
        wsum += w;
    }
    for (Vec3f& p : blended) p = p / wsum;
    length /= wsum;
    // Blending shortens (shapes average out): scale back to the blended length.
    const float blendedLength = arcLength(blended.data(), blended.size());
    if (blendedLength > 1e-6f) {
        for (Vec3f& p : blended) p *= length / blendedLength;
    }
    return blended;
}

// Painted layers of the guides nearest `at`, each blended over the neighbors that
// have it (inherited values count: a child shows its parent's map).
std::map<std::string, float> neighborAttributes(const GuideHierarchy& h, const std::vector<GroupId>& groups,
                                                const SurfacePoint& at) {
    std::map<std::string, std::pair<float, float>> sums;  // name -> (weighted sum, weight)
    for (const Near& n : nearestOf(h, groups, at)) {
        const float w = 1.0f / (n.d2 + 1e-8f);
        for (const auto& [name, value] : h.effectiveAttributes(*n.g)) {
            sums[name].first += value * w;
            sums[name].second += w;
        }
    }
    std::map<std::string, float> out;
    for (const auto& [name, sum] : sums) out[name] = sum.first / sum.second;
    return out;
}

NeighborMatch neighborMatch(const GuideHierarchy& h, const std::vector<GroupId>& groups, const ScalpMesh& scalp,
                            const SurfacePoint& at) {
    NeighborMatch m;
    const std::vector<Near> near = nearestOf(h, groups, at);
    if (near.empty()) return m;
    float wsum = 0.0f, count = 0.0f;
    for (const Near& n : near) {
        const float w = 1.0f / (n.d2 + 1e-8f);
        count += static_cast<float>(n.g->points.size()) * w;
        wsum += w;
    }
    const int segments = std::max(1, static_cast<int>(std::lround(count / wsum)) - 1);
    m.points = neighborShape(h, groups, scalp, at, segments);
    m.attributes = neighborAttributes(h, groups, at);
    return m;
}

// The tube a new guide of `points` points at `at` takes (tube guides): the shape of `own` (the
// placement's: PlaceSettings::tube and tubeShape), and its width, aspect and twist blended from the
// two nearest guides of `groups` by inverse distance, at each of its points (blendedTube). `own`
// as it is when those groups have no guides.
TubeProfile neighborTube(const GuideHierarchy& h, const std::vector<GroupId>& groups, const SurfacePoint& at,
                         std::size_t points, const TubeProfile& own) {
    const std::vector<Near> near = nearestOf(h, groups, at);
    if (near.empty()) return own;
    std::vector<const TubeProfile*> tubes;
    std::vector<float> weights;
    for (std::size_t k = 0; k < std::min<std::size_t>(2, near.size()); ++k) {
        tubes.push_back(&near[k].g->tube);
        weights.push_back(1.0f / std::max(std::sqrt(near[k].d2), 1e-6f));
    }
    TubeProfile out = blendedTube(tubes, weights, std::max<std::size_t>(points, 2));
    out.shape = own.shape;
    out.custom = own.custom;
    return out;
}

// The tube Place and Draw start from: the settings' (widths as shown to as stored), its shape the
// library shape `tubeShape` names when the groom has it.
TubeProfile placementTube(const GuideHierarchy& h, const PlaceSettings& s) {
    TubeProfile own = s.tube;
    for (float& r : own.width) r /= h.scale();
    own.roll.clear();
    if (!s.tubeShape.empty()) {
        TubeShape builtin = TubeShape::Round;
        if (parseTubeShape(s.tubeShape, &builtin)) {
            own.shape = builtin;
            own.custom = nullptr;
        } else if (CustomTubeShapePtr custom = h.tubeShape(s.tubeShape)) {
            own.custom = std::move(custom);
        }
    }
    return own;
}

}  // namespace

BrushMode brushModeFromString(const std::string& name) {
    if (name == "comb") return BrushMode::Comb;
    if (name == "smooth") return BrushMode::Smooth;
    if (name == "lift") return BrushMode::Lift;
    if (name == "length") return BrushMode::Length;
    if (name == "cut") return BrushMode::Cut;
    if (name == "shape") return BrushMode::ShapeTube;
    if (name == "paint" || name == "attribute" || name == "clump") return BrushMode::Attribute;
    if (name == "twist") return BrushMode::Twist;
    if (name == "curl") return BrushMode::Curl;
    if (name == "straighten") return BrushMode::Straighten;
    if (name == "slide" || name == "move") return BrushMode::Slide;
    if (name == "attract") return BrushMode::Attract;
    if (name == "erase") return BrushMode::Erase;
    if (name == "direction") return BrushMode::Direction;
    if (name == "noise") return BrushMode::Noise;
    throw std::invalid_argument("unknown brush mode '" + name + "'");
}

const char* brushModeName(BrushMode mode) {
    switch (mode) {
        case BrushMode::Comb: return "comb";
        case BrushMode::Smooth: return "smooth";
        case BrushMode::Lift: return "lift";
        case BrushMode::Length: return "length";
        case BrushMode::ShapeTube: return "shape";
        case BrushMode::Attribute: return "paint";
        case BrushMode::Twist: return "twist";
        case BrushMode::Curl: return "curl";
        case BrushMode::Straighten: return "straighten";
        case BrushMode::Slide: return "slide";
        case BrushMode::Attract: return "attract";
        case BrushMode::Erase: return "erase";
        case BrushMode::Cut: return "cut";
        case BrushMode::Direction: return "direction";
        case BrushMode::Noise: return "noise";
    }
    return "comb";
}

void useShiftSmoothing(BrushSettings& settings) {
    if (settings.mode == BrushMode::ShapeTube) {
        settings.smoothTube = true;
    } else {
        settings.mode = BrushMode::Smooth;
    }
}

void endShiftSmoothing(BrushSettings& settings, BrushMode mode) {
    settings.mode = mode;
    settings.smoothTube = false;
}

const char* tubeChannelName(TubeChannel channel) {
    switch (channel) {
        case TubeChannel::Width: return "width";
        case TubeChannel::Flatness: return "flatness";
        case TubeChannel::Twist: return "twist";
    }
    return "width";
}

TubeChannel tubeChannelFromString(const std::string& name) {
    if (name == "width") return TubeChannel::Width;
    if (name == "flatness") return TubeChannel::Flatness;
    if (name == "twist") return TubeChannel::Twist;
    throw std::invalid_argument("the Shape brush paints width, flatness or twist, not '" + name + "'");
}

std::vector<Vec3f> keepOffScalp(const std::vector<Vec3f>& points, const ScalpMesh& scalp, float offset) {
    if (points.size() < 2 || scalp.triangleCount() == 0) return points;
    // Height above the surface near p, and the surface normal there; false when the surface is far.
    auto below = [&](const Vec3f& p, float reach, SurfacePoint* sp) {
        *sp = scalp.closestPoint(p, reach);
        if (sp->root.face < 0) return false;
        return (p - sp->position).dot(sp->normal.normalized()) < offset - 1e-5f;
    };
    std::vector<Vec3f> out(points.size());
    out[0] = points[0];
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float len = (points[i] - points[i - 1]).length();
        const float reach = len + offset + 1e-4f;
        Vec3f dir = (points[i] - out[i - 1]).normalized();
        if (dir.length() == 0.0f) dir = (points[i] - points[i - 1]).normalized();
        Vec3f p = out[i - 1] + dir * len;
        SurfacePoint sp;
        if (below(p, reach, &sp)) {
            // Slide along the surface: drop the part of the direction going into it.
            const Vec3f n = sp.normal.normalized();
            Vec3f t = dir - n * dir.dot(n);
            if (t.length() < 1e-6f) t = anyPerpendicular(n);
            p = out[i - 1] + t.normalized() * len;
            // Then lift onto the offset where the surface curves up under it.
            for (int pass = 0; pass < 2 && below(p, reach, &sp); ++pass) {
                const Vec3f lifted = sp.position + sp.normal.normalized() * offset;
                const Vec3f d = (lifted - out[i - 1]).normalized();
                if (d.length() == 0.0f) break;
                p = out[i - 1] + d * len;
            }
        }
        out[i] = p;
    }
    return out;
}

std::size_t applyBrushStamp(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                            const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight) {
    return symmetricStamp(hierarchy, scalp, settings, center, drag, weight, {}, nullptr, false, nullptr);
}

std::size_t applyBrushStamp(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                            const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight, const BrushReachFn& reach,
                            BrushStrokeCache* cache, bool threadSafeWeight) {
    return symmetricStamp(hierarchy, scalp, settings, center, drag, weight, reach, cache ? &cache->state() : nullptr,
                          threadSafeWeight, nullptr);
}

std::size_t applyBrushStroke(GuideHierarchy& hierarchy, const ScalpMesh* scalp, const BrushSettings& settings,
                             const std::vector<Vec3f>& path) {
    std::set<GuideId> changed;
    if (path.empty()) return 0;
    BrushStrokeCache cache;  // the stroke's guides and their bounds, for all its stamps
    alongPath(settings, path, [&](const Vec3f& center, const Vec3f& drag) {
        symmetricStamp(hierarchy, scalp, settings, center, drag, {}, {}, &cache.state(), true, &changed);
    });
    return changed.size();
}

std::vector<GuideId> withMirrorGuides(const GuideHierarchy& h, const std::vector<GuideId>& ids, const StrokeSymmetry& sym) {
    if (!sym.on() || ids.empty()) return ids;
    // Each group's roots, indexed once.
    struct Group {
        std::vector<GuideId> ids;
        std::unique_ptr<PointIndex> index;
    };
    std::map<GroupId, Group> groups;
    for (const HierarchyGuide& g : h.guides()) {
        if (!g.points.empty()) groups[g.group].ids.push_back(g.id);
    }
    for (auto& [gid, group] : groups) {
        std::vector<Vec3f> roots;
        for (const GuideId id : group.ids) roots.push_back(h.guide(id)->points.front());
        group.index = std::make_unique<PointIndex>(std::move(roots));
    }
    std::vector<GuideId> out;
    std::unordered_set<GuideId> seen;
    auto add = [&](GuideId id) {
        if (id != kNoGuide && seen.insert(id).second) out.push_back(id);
    };
    for (const GuideId id : ids) {
        add(id);
        const HierarchyGuide* g = h.guide(id);
        if (!g || g->points.empty()) continue;
        const Group& group = groups[g->group];
        const std::size_t k = group.index ? group.index->nearest(sym.point(g->points.front())) : SIZE_MAX;
        if (k < group.ids.size()) add(group.ids[k]);
    }
    return out;
}

GuideId mirrorGuide(const GuideHierarchy& h, GuideId id, const StrokeSymmetry& sym) {
    if (!sym.on() || !h.guide(id)) return kNoGuide;
    const std::vector<GuideId> both = withMirrorGuides(h, {id}, sym);
    return both.size() > 1 ? both[1] : id;
}

void soloGuides(GuideHierarchy& h, const std::vector<GuideId>& active, const std::string& freeze) {
    std::unordered_set<GuideId> on;
    std::unordered_map<GuideId, std::vector<GuideId>> children;
    for (const HierarchyGuide& g : h.guides()) {
        if (g.isChild()) children[g.parent].push_back(g.id);
    }
    std::vector<GuideId> stack;
    for (const GuideId id : active) {
        if (h.guide(id)) stack.push_back(id);
    }
    while (!stack.empty()) {
        const GuideId id = stack.back();
        stack.pop_back();
        if (!on.insert(id).second) continue;
        const auto it = children.find(id);
        if (it != children.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
    }
    std::vector<GuideId> ids;
    for (const HierarchyGuide& g : h.guides()) ids.push_back(g.id);
    for (const GuideId id : ids) h.setAttribute(id, freeze, on.count(id) ? 0.0f : 1.0f);
}

bool freezeToSelection(GuideHierarchy& h, const std::vector<GuideId>& selection, const std::string& freeze) {
    if (selection.empty()) return unfreezeAll(h, freeze);
    std::vector<float> before;
    for (const HierarchyGuide& g : h.guides()) {
        const auto it = g.attributes.find(freeze);
        before.push_back(it == g.attributes.end() ? -1.0f : it->second);
    }
    soloGuides(h, selection, freeze);
    std::size_t i = 0;
    for (const HierarchyGuide& g : h.guides()) {
        const auto it = g.attributes.find(freeze);
        if ((it == g.attributes.end() ? -1.0f : it->second) != before[i++]) return true;
    }
    return false;
}

bool unfreezeAll(GuideHierarchy& h, const std::string& freeze) {
    const std::vector<std::string> names = h.attributeNames();
    if (std::find(names.begin(), names.end(), freeze) == names.end()) return false;
    h.removeAttribute(freeze);
    return true;
}

// ---------------------------------------------------------------------------
// Flow arrows and parts

namespace {

// A pin's world direction, and the surface normal at its root.
Vec3f pinDirection(const ScalpMesh& scalp, const FlowPin& pin, Vec3f* normal) {
    Vec3f x, y, z;
    surfaceFrame(scalp, pin.root, x, y, z);
    if (normal) *normal = z;
    return (x * pin.local.x + y * pin.local.y + z * pin.local.z).normalized();
}

// Re-binds a pin at the scalp point nearest `position`, pointing along
// `direction` (laid on the tangent plane there); line and seq kept. False,
// pin unchanged, off the scalp or with a direction along the normal.
bool rebindPin(const ScalpMesh& scalp, FlowPin& pin, const Vec3f& position, const Vec3f& direction) {
    const SurfacePoint at = scalp.closestPoint(position);
    if (at.root.face < 0) return false;
    try {
        FlowPin moved = FlowSet::makePin(scalp, at, direction);
        moved.line = pin.line;
        moved.seq = pin.seq;
        pin = moved;
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

// A pin turned to point along `direction` where it is (line and seq kept).
bool turnPin(const ScalpMesh& scalp, FlowPin& pin, const Vec3f& position, const Vec3f& normal, const Vec3f& direction) {
    try {
        FlowPin turned = FlowSet::makePin(scalp, SurfacePoint{pin.root, position, normal}, direction);
        turned.line = pin.line;
        turned.seq = pin.seq;
        pin = turned;
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

// Unit `d` turned toward unit `target` by `amount` in [0, 1] (a blend, renormalized).
Vec3f turnToward(const Vec3f& d, const Vec3f& target, float amount) {
    const Vec3f turned = d * (1.0f - amount) + target * amount;
    return turned.length() > 1e-6f ? turned.normalized() : d;
}

// What a stamp changed, so the mirrored stamp skips it (indices stay put:
// only Erase removes, and it never skips).
struct FlowTouched {
    std::set<std::size_t> pins, sinks;
    void add(const FlowTouched& o) {
        pins.insert(o.pins.begin(), o.pins.end());
        sinks.insert(o.sinks.begin(), o.sinks.end());
    }
};

// The pins of each stroke line in order along it (indices into flow.pins).
std::map<int, std::vector<std::size_t>> flowLines(const FlowSet& flow) {
    std::map<int, std::vector<std::size_t>> lines;
    for (std::size_t i = 0; i < flow.pins.size(); ++i) {
        if (flow.pins[i].line >= 0) lines[flow.pins[i].line].push_back(i);
    }
    for (auto& [line, members] : lines) {
        std::stable_sort(members.begin(), members.end(),
                         [&](std::size_t a, std::size_t b) { return flow.pins[a].seq < flow.pins[b].seq; });
    }
    return lines;
}

std::size_t flowStamp(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& s, const Vec3f& center,
                      const Vec3f& drag, const BrushWeightFn& weight, const FlowTouched* skip, FlowTouched* touched) {
    const Weigher weigh{center, s.radius, weight, falloffOf(s)};
    const float k = s.strength;
    const float kUnit = std::clamp(k, 0.0f, 1.0f);
    const float kSigned = std::clamp(k, -1.0f, 1.0f);
    constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Where everything is on this scalp, and how much the brush holds it.
    const std::size_t np = flow.pins.size(), ns = flow.sinks.size();
    std::vector<Vec3f> pinPos(np), sinkPos(ns);
    std::vector<float> pinW(np, 0.0f), sinkW(ns, 0.0f);
    for (std::size_t i = 0; i < np; ++i) {
        if (!scalp.contains(flow.pins[i].root)) continue;
        pinPos[i] = scalp.position(flow.pins[i].root);
        if (!(skip && skip->pins.count(i))) pinW[i] = weigh(pinPos[i]);
    }
    for (std::size_t i = 0; i < ns; ++i) {
        if (!scalp.contains(flow.sinks[i].root)) continue;
        sinkPos[i] = scalp.position(flow.sinks[i].root);
        if (!(skip && skip->sinks.count(i))) sinkW[i] = weigh(sinkPos[i]);
    }
    std::size_t changed = 0;
    auto pinChanged = [&](std::size_t i) {
        ++changed;
        if (touched) touched->pins.insert(i);
    };
    auto sinkChanged = [&](std::size_t i) {
        ++changed;
        if (touched) touched->sinks.insert(i);
    };
    // A sink's center slid to `to` (re-bound to the scalp).
    auto moveSink = [&](std::size_t i, const Vec3f& to) {
        const SurfacePoint at = scalp.closestPoint(to);
        if (at.root.face < 0) return;
        flow.sinks[i].root = at.root;
        sinkChanged(i);
    };

    switch (s.mode) {
        case BrushMode::Comb: {
            if (drag.length() == 0.0f) return 0;
            const Vec3f dragDir = drag.normalized();
            const std::map<int, std::vector<std::size_t>> lines = flowLines(flow);
            auto onLine = [&](const FlowPin& pin) {  // part of a stroke of two or more pins
                const auto it = pin.line >= 0 ? lines.find(pin.line) : lines.end();
                return it != lines.end() && it->second.size() >= 2;
            };
            std::set<int> movedLines;
            for (std::size_t i = 0; i < np; ++i) {
                const float w = pinW[i];
                if (w <= 0.0f) continue;
                Vec3f dir = pinDirection(scalp, flow.pins[i], nullptr);
                // A pin on its own is laid along the stroke by the comb; a stroke's pins keep
                // their direction for now: it follows the stroke's new shape below.
                const bool lined = onLine(flow.pins[i]);
                if (!lined) dir = turnToward(dir, dragDir, kUnit * w);
                if (rebindPin(scalp, flow.pins[i], pinPos[i] + drag * (k * w), dir)) {
                    pinChanged(i);
                    if (lined) movedLines.insert(flow.pins[i].line);
                }
            }
            // A stroke line that moved: each of its pins points along the line as it now runs
            // (from the pin before to the one after), the way it was drawn. Pins not on this
            // scalp (bound to another mesh) are left out.
            if (!movedLines.empty()) {
                for (const auto& [line, all] : lines) {
                    if (!movedLines.count(line)) continue;
                    std::vector<std::size_t> members;
                    for (const std::size_t i : all) {
                        if (scalp.contains(flow.pins[i].root)) members.push_back(i);
                    }
                    if (members.size() < 2) continue;
                    std::vector<Vec3f> along(members.size());
                    for (std::size_t j = 0; j < members.size(); ++j) along[j] = scalp.position(flow.pins[members[j]].root);
                    for (std::size_t j = 0; j < members.size(); ++j) {
                        const Vec3f dir = along[std::min(j + 1, members.size() - 1)] - along[j > 0 ? j - 1 : 0];
                        if (dir.length() <= 1e-9f) continue;
                        const std::size_t i = members[j];
                        if (rebindPin(scalp, flow.pins[i], along[j], dir) && !(touched && touched->pins.count(i))) pinChanged(i);
                    }
                }
            }
            for (std::size_t i = 0; i < ns; ++i) {
                if (sinkW[i] > 0.0f) moveSink(i, sinkPos[i] + drag * (k * sinkW[i]));
            }
            return changed;
        }
        case BrushMode::Smooth: {
            // Each pin's direction toward its nearest pins' average, and its place
            // toward the midpoint of its neighbours along its stroke line.
            std::vector<Vec3f> positions;
            std::vector<std::size_t> owner;
            for (std::size_t i = 0; i < np; ++i) {
                if (!scalp.contains(flow.pins[i].root)) continue;
                positions.push_back(pinPos[i]);
                owner.push_back(i);
            }
            const PointIndex index(positions);
            std::vector<std::size_t> prev(np, npos), next(np, npos);
            for (const auto& [line, members] : flowLines(flow)) {
                for (std::size_t j = 0; j < members.size(); ++j) {
                    if (j > 0) prev[members[j]] = members[j - 1];
                    if (j + 1 < members.size()) next[members[j]] = members[j + 1];
                }
            }
            std::vector<FlowPin> out = flow.pins;  // from the state before the stamp
            for (std::size_t i = 0; i < np; ++i) {
                const float w = 0.5f * kUnit * pinW[i];
                if (w <= 0.0f) continue;
                Vec3f normal;
                const Vec3f dir = pinDirection(scalp, flow.pins[i], &normal);
                constexpr std::size_t kNear = 7;  // itself and six others
                std::array<std::size_t, kNear> near{};
                std::array<float, kNear> d2{};
                const std::size_t found = index.kNearest(pinPos[i], kNear, near.data(), d2.data());
                Vec3f sum;
                for (std::size_t n = 0; n < found; ++n) {
                    const std::size_t j = owner[near[n]];
                    if (j == i) continue;
                    Vec3f dj = pinDirection(scalp, flow.pins[j], nullptr);
                    dj = dj - normal * dj.dot(normal);  // on this pin's tangent plane
                    if (dj.length() > 1e-6f) sum += dj.normalized();
                }
                const Vec3f newDir = sum.length() > 1e-6f ? turnToward(dir, sum.normalized(), w) : dir;
                Vec3f target = pinPos[i];
                if (prev[i] != npos && next[i] != npos && scalp.contains(flow.pins[prev[i]].root) &&
                    scalp.contains(flow.pins[next[i]].root)) {
                    target = (pinPos[prev[i]] + pinPos[next[i]]) * 0.5f;
                }
                if (rebindPin(scalp, out[i], pinPos[i] + (target - pinPos[i]) * w, newDir)) pinChanged(i);
            }
            flow.pins = std::move(out);
            return changed;
        }
        case BrushMode::Twist: {
            for (std::size_t i = 0; i < np; ++i) {
                const float angle = kSigned * kTwistRate * pinW[i];
                if (angle == 0.0f) continue;
                Vec3f normal;
                const Vec3f dir = pinDirection(scalp, flow.pins[i], &normal);
                if (turnPin(scalp, flow.pins[i], pinPos[i], normal, rotateAxisAngle(dir, normal, angle))) pinChanged(i);
            }
            for (std::size_t i = 0; i < ns; ++i) {
                const float angle = kSigned * kTwistRate * sinkW[i];
                if (angle == 0.0f) continue;
                flow.sinks[i].angle += angle * 57.29578f;  // degrees, the same turn
                sinkChanged(i);
            }
            return changed;
        }
        case BrushMode::Length: {
            for (std::size_t i = 0; i < ns; ++i) {
                if (sinkW[i] <= 0.0f || k == 0.0f) continue;
                flow.sinks[i].radius *= std::max(0.05f, 1.0f + k * sinkW[i] * kPerStampRate);
                sinkChanged(i);
            }
            return changed;
        }
        case BrushMode::Attract: {
            for (std::size_t i = 0; i < np; ++i) {
                const float w = kSigned * kAttractRate * pinW[i];
                if (w == 0.0f) continue;
                const Vec3f dir = pinDirection(scalp, flow.pins[i], nullptr);
                if (rebindPin(scalp, flow.pins[i], pinPos[i] + (center - pinPos[i]) * w, dir)) pinChanged(i);
            }
            for (std::size_t i = 0; i < ns; ++i) {
                const float w = kSigned * kAttractRate * sinkW[i];
                if (w != 0.0f) moveSink(i, sinkPos[i] + (center - sinkPos[i]) * w);
            }
            return changed;
        }
        case BrushMode::Straighten: {
            // Pins of a stroke line turn toward the line's overall direction (its ends).
            for (const auto& [line, members] : flowLines(flow)) {
                if (members.size() < 2) continue;
                const std::size_t a = members.front(), b = members.back();
                if (!scalp.contains(flow.pins[a].root) || !scalp.contains(flow.pins[b].root)) continue;
                const Vec3f overall = pinPos[b] - pinPos[a];
                if (overall.length() < 1e-6f) continue;
                for (const std::size_t i : members) {
                    const float w = 0.5f * kUnit * pinW[i];
                    if (w <= 0.0f) continue;
                    Vec3f normal;
                    const Vec3f dir = pinDirection(scalp, flow.pins[i], &normal);
                    const Vec3f target = overall - normal * overall.dot(normal);
                    if (target.length() < 1e-6f) continue;
                    if (turnPin(scalp, flow.pins[i], pinPos[i], normal, turnToward(dir, target.normalized(), w))) pinChanged(i);
                }
            }
            return changed;
        }
        case BrushMode::Erase: {
            std::vector<FlowPin> pins;
            for (std::size_t i = 0; i < np; ++i) {
                if (pinW[i] * kUnit >= 0.5f) ++changed;
                else pins.push_back(flow.pins[i]);
            }
            std::vector<FlowSink> sinks;
            for (std::size_t i = 0; i < ns; ++i) {
                if (sinkW[i] * kUnit >= 0.5f) ++changed;
                else sinks.push_back(flow.sinks[i]);
            }
            flow.pins = std::move(pins);
            flow.sinks = std::move(sinks);
            return changed;
        }
        case BrushMode::Lift:
        case BrushMode::Curl:
        case BrushMode::ShapeTube:
        case BrushMode::Attribute:
        case BrushMode::Slide:
        case BrushMode::Cut:
        case BrushMode::Direction:
        case BrushMode::Noise:
            return 0;
    }
    return 0;
}

std::size_t symmetricFlowStamp(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& s, const Vec3f& center,
                               const Vec3f& drag, const BrushWeightFn& weight, FlowTouched* touched) {
    FlowTouched first;
    std::size_t changed = flowStamp(flow, scalp, s, center, drag, weight, nullptr, &first);
    if (s.mirrorTopology) {
        const StrokeSymmetry sym = StrokeSymmetry::of(s, &scalp);
        FlowTouched second;
        if (sym.on()) {
            changed += flowStamp(flow, scalp, mirroredSettings(s), sym.point(center), sym.vector(center, drag), {}, &first,
                                 &second);
        }
        first.add(second);
    } else if (s.mirrorAxis >= 0 && s.mirrorAxis <= 2) {
        FlowTouched second;
        changed += flowStamp(flow, scalp, mirroredSettings(s), mirrorPoint(center, s.mirrorAxis, s.mirrorCenter),
                             mirrorVector(drag, s.mirrorAxis), mirroredWeight(weight, s.mirrorAxis, s.mirrorCenter),
                             &first, &second);
        first.add(second);
    }
    if (touched) touched->add(first);
    return changed;
}

// The part points (part, point) and the parts a stamp changed.
struct PartTouched {
    std::set<std::pair<std::size_t, std::size_t>> points;
    std::set<std::size_t> parts;
    void add(const PartTouched& o) {
        points.insert(o.points.begin(), o.points.end());
        parts.insert(o.parts.begin(), o.parts.end());
    }
};

std::size_t partStamp(PartSet& set, const ScalpMesh& scalp, const BrushSettings& s, const Vec3f& center,
                      const Vec3f& drag, const BrushWeightFn& weight, const PartTouched* skip, PartTouched* touched) {
    const Weigher weigh{center, s.radius, weight, falloffOf(s)};
    const float k = s.strength;
    const float kUnit = std::clamp(k, 0.0f, 1.0f);
    const float kSigned = std::clamp(k, -1.0f, 1.0f);
    std::size_t changed = 0;

    if (s.mode == BrushMode::Erase) {
        std::vector<Part> kept;
        for (Part& part : set.parts) {
            const bool under = std::any_of(part.points.begin(), part.points.end(), [&](const StrandRoot& r) {
                return scalp.contains(r) && weigh(scalp.position(r)) * kUnit >= 0.5f;
            });
            if (under) ++changed;
            else kept.push_back(std::move(part));
        }
        set.parts = std::move(kept);
        return changed;
    }
    if (s.mode == BrushMode::Cut) {
        // The touched points go; each run of points left is a part: the first
        // keeps the name, the others take name1, name2... (unused ones).
        const float threshold = 0.5f / std::max(kUnit, 1e-3f);
        std::set<std::string> taken;
        for (const Part& part : set.parts) taken.insert(part.name);
        auto unique = [&](const std::string& base) {
            std::string name = base;
            for (int i = 1; taken.count(name); ++i) name = base + std::to_string(i);
            taken.insert(name);
            return name;
        };
        std::vector<Part> out;
        for (Part& part : set.parts) {
            std::vector<std::vector<StrandRoot>> pieces(1);
            bool hit = false;
            for (const StrandRoot& r : part.points) {
                if (scalp.contains(r) && weigh(scalp.position(r)) >= threshold) {
                    hit = true;
                    if (!pieces.back().empty()) pieces.emplace_back();
                } else {
                    pieces.back().push_back(r);
                }
            }
            if (!hit) {
                out.push_back(std::move(part));
                continue;
            }
            ++changed;
            bool first = true;
            for (auto& piece : pieces) {
                if (piece.size() < 2) continue;
                Part p;
                p.name = first ? part.name : unique(part.name);
                first = false;
                p.points = std::move(piece);
                out.push_back(std::move(p));
            }
        }
        set.parts = std::move(out);
        return changed;
    }

    for (std::size_t pi = 0; pi < set.parts.size(); ++pi) {
        Part& part = set.parts[pi];
        const std::size_t n = part.points.size();
        if (n < 2) continue;
        std::vector<Vec3f> pos(n);
        std::vector<bool> valid(n);
        for (std::size_t i = 0; i < n; ++i) {
            valid[i] = scalp.contains(part.points[i]);
            if (valid[i]) pos[i] = scalp.position(part.points[i]);
        }
        auto weightAt = [&](std::size_t i) {
            if (!valid[i] || (skip && skip->points.count({pi, i}))) return 0.0f;
            return weigh(pos[i]);
        };
        std::vector<Vec3f> moved(n);
        std::vector<bool> move(n, false);
        switch (s.mode) {
            case BrushMode::Comb: {
                if (drag.length() == 0.0f) return 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const float w = k * weightAt(i);
                    if (w == 0.0f) continue;
                    moved[i] = pos[i] + drag * w;
                    move[i] = true;
                }
                break;
            }
            case BrushMode::Smooth: {
                for (std::size_t i = 1; i + 1 < n; ++i) {
                    const float w = 0.5f * kUnit * weightAt(i);
                    if (w <= 0.0f || !valid[i - 1] || !valid[i + 1]) continue;
                    moved[i] = pos[i] + ((pos[i - 1] + pos[i + 1]) * 0.5f - pos[i]) * w;
                    move[i] = true;
                }
                break;
            }
            case BrushMode::Straighten: {
                if (!valid[0] || !valid[n - 1]) break;
                const Vec3f a = pos[0], ab = pos[n - 1] - a;
                const float len2 = ab.dot(ab);
                if (len2 <= 0.0f) break;
                for (std::size_t i = 1; i + 1 < n; ++i) {
                    const float w = 0.5f * kUnit * weightAt(i);
                    if (w <= 0.0f) continue;
                    const float t = std::clamp((pos[i] - a).dot(ab) / len2, 0.0f, 1.0f);
                    moved[i] = pos[i] + (a + ab * t - pos[i]) * w;
                    move[i] = true;
                }
                break;
            }
            case BrushMode::Attract: {
                for (std::size_t i = 0; i < n; ++i) {
                    const float w = kSigned * kAttractRate * weightAt(i);
                    if (w == 0.0f) continue;
                    moved[i] = pos[i] + (center - pos[i]) * w;
                    move[i] = true;
                }
                break;
            }
            default:
                return 0;  // Lift, Curl, ShapeTube, Attribute, Slide, Twist, Length: not for parts
        }
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!move[i]) continue;
            const SurfacePoint at = scalp.closestPoint(moved[i]);
            if (at.root.face < 0) continue;
            part.points[i] = at.root;
            any = true;
            if (touched) touched->points.insert({pi, i});
        }
        if (any) {
            ++changed;
            if (touched) touched->parts.insert(pi);
        }
    }
    return changed;
}

std::size_t symmetricPartStamp(PartSet& set, const ScalpMesh& scalp, const BrushSettings& s, const Vec3f& center,
                               const Vec3f& drag, const BrushWeightFn& weight, PartTouched* touched) {
    PartTouched first;
    std::size_t changed = partStamp(set, scalp, s, center, drag, weight, nullptr, &first);
    if (s.mirrorTopology) {
        const StrokeSymmetry sym = StrokeSymmetry::of(s, &scalp);
        PartTouched second;
        if (sym.on()) changed += partStamp(set, scalp, s, sym.point(center), sym.vector(center, drag), {}, &first, &second);
        first.add(second);
    } else if (s.mirrorAxis >= 0 && s.mirrorAxis <= 2) {
        PartTouched second;
        changed += partStamp(set, scalp, s, mirrorPoint(center, s.mirrorAxis, s.mirrorCenter),
                             mirrorVector(drag, s.mirrorAxis), mirroredWeight(weight, s.mirrorAxis, s.mirrorCenter),
                             &first, &second);
        first.add(second);
    }
    if (touched) touched->add(first);
    return changed;
}

}  // namespace

std::size_t applyBrushStampToFlow(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& settings,
                                  const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight) {
    return symmetricFlowStamp(flow, scalp, settings, center, drag, weight, nullptr);
}

std::size_t applyBrushStrokeToFlow(FlowSet& flow, const ScalpMesh& scalp, const BrushSettings& settings,
                                   const std::vector<Vec3f>& path) {
    if (path.empty()) return 0;
    FlowTouched all;
    std::size_t removed = 0;
    alongPath(settings, path, [&](const Vec3f& center, const Vec3f& drag) {
        removed += symmetricFlowStamp(flow, scalp, settings, center, drag, {}, &all);
    });
    return settings.mode == BrushMode::Erase ? removed : all.pins.size() + all.sinks.size();
}

std::size_t applyBrushStampToParts(PartSet& parts, const ScalpMesh& scalp, const BrushSettings& settings,
                                   const Vec3f& center, const Vec3f& drag, const BrushWeightFn& weight) {
    return symmetricPartStamp(parts, scalp, settings, center, drag, weight, nullptr);
}

std::size_t applyBrushStrokeToParts(PartSet& parts, const ScalpMesh& scalp, const BrushSettings& settings,
                                    const std::vector<Vec3f>& path) {
    if (path.empty()) return 0;
    PartTouched all;
    std::size_t counted = 0;
    alongPath(settings, path, [&](const Vec3f& center, const Vec3f& drag) {
        counted += symmetricPartStamp(parts, scalp, settings, center, drag, {}, &all);
    });
    // Erase and Cut renumber the parts: their stamps' counts add up.
    return settings.mode == BrushMode::Erase || settings.mode == BrushMode::Cut ? counted : all.parts.size();
}

// ---------------------------------------------------------------------------
// Place and draw

std::string placementTubeJson(const PlaceSettings& s) {
    nlohmann::json j = nlohmann::json::parse(tubeToJson(s.tube));
    if (!s.tubeShape.empty()) j["shape"] = s.tubeShape;
    return j.dump();
}

void setPlacementTube(PlaceSettings& s, const std::string& json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(std::string("placement tube: ") + e.what());
    }
    if (!j.is_object()) throw std::invalid_argument("placement tube: a JSON object");
    std::string shape;
    if (j.contains("shape")) {
        if (!j["shape"].is_string() || j["shape"].get<std::string>().empty()) {
            throw std::invalid_argument("placement tube: shape is a name");
        }
        shape = j["shape"].get<std::string>();
        j.erase("shape");
    }
    TubeProfile tube = editedTube(s.tube, j.dump());
    if (!shape.empty()) {
        TubeShape builtin = TubeShape::Round;
        if (parseTubeShape(shape, &builtin)) {
            tube.shape = builtin;
            s.tubeShape.clear();
        } else {
            checkTubeShapeName(shape);
            tube.shape = TubeShape::Round;
            s.tubeShape = shape;
        }
    }
    tube.custom = nullptr;
    s.tube = std::move(tube);
}

GuideId placeGuide(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& s, const SurfacePoint& at) {
    if (at.root.face < 0) throw std::invalid_argument("placeGuide: not a point on the scalp");
    const GroupId gid = targetGroup(hierarchy, s.group);
    const int segments = std::max(s.segments, 1);
    std::vector<Vec3f> pts;
    NeighborMatch match;
    bool matched = false;
    if (s.matchNeighbors) {
        // Interpolate the nearby guides of the group, or of the groups at its level
        // while it has none; for a child group with neither, the shape of the
        // nearby parents (not their tube or layers).
        match = neighborMatch(hierarchy, neighborGroups(hierarchy, gid), scalp, at);
        matched = !match.points.empty();
        if (!matched && hierarchy.groupDepth(gid) > 0) {
            match.points = neighborShape(hierarchy, {hierarchy.group(gid)->parent}, scalp, at, segments);
        }
        pts = std::move(match.points);
        for (Vec3f& p : pts) p += at.position;
        if (!pts.empty()) pts = keepOffScalp(pts, scalp);
    }
    if (pts.empty()) {
        for (int i = 0; i <= segments; ++i) pts.push_back(at.position + at.normal * (s.length * i / segments));
    }
    // The placement's shape, its sections the two nearest guides' blended (else the placement's own).
    const TubeProfile own = placementTube(hierarchy, s);
    const TubeProfile tube = matched ? neighborTube(hierarchy, neighborGroups(hierarchy, gid), at, pts.size(), own) : own;
    const GuideId id = addToGroup(hierarchy, gid, std::move(pts), tube);
    // Painted layers: a root guide takes them from its neighbors (children inherit their parent's).
    if (matched && hierarchy.groupDepth(gid) == 0) {
        for (const auto& [name, value] : match.attributes) hierarchy.setAttribute(id, name, value);
    }
    hierarchy.evaluateSubtrees(&scalp, {id});
    return id;
}

std::vector<GuideId> placeGuides(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& s,
                                 const std::vector<Vec3f>& path) {
    std::vector<GuideId> placed;
    std::vector<Vec3f> roots;
    const GroupId existing = hierarchy.findGroup(s.group.empty() ? "primary" : s.group);
    for (const HierarchyGuide& g : hierarchy.guides()) {
        if (g.group == existing && !g.points.empty()) roots.push_back(g.points.front());
    }
    const float spacing2 = s.spacing * s.spacing;
    for (const Vec3f& p : path) {
        const SurfacePoint at = scalp.closestPoint(p);
        if (at.root.face < 0) continue;
        const bool crowded = std::any_of(roots.begin(), roots.end(), [&](const Vec3f& r) {
            return (r - at.position).dot(r - at.position) < spacing2;
        });
        if (crowded) continue;
        placed.push_back(placeGuide(hierarchy, scalp, s, at));
        roots.push_back(at.position);
    }
    return placed;
}

GuideId drawGuide(GuideHierarchy& hierarchy, const ScalpMesh& scalp, const PlaceSettings& s,
                  const std::vector<Vec3f>& path) {
    if (path.size() < 2 || arcLength(path.data(), path.size()) < 1e-4f) {
        throw std::invalid_argument("drawGuide: draw a longer stroke");
    }
    const SurfacePoint root = scalp.closestPoint(path.front());
    if (root.root.face < 0) throw std::invalid_argument("drawGuide: the stroke does not start on the scalp");
    std::vector<Vec3f> pts = path;
    pts.front() = root.position;
    pts = resampleByArcLength(pts.data(), pts.size(), static_cast<std::size_t>(std::max(s.segments, 1)) + 1);
    pts = keepOffScalp(pts, scalp);
    const GroupId gid = targetGroup(hierarchy, s.group);
    // The shape is drawn; painted layers and the tube come from the neighbors, as for Place.
    std::map<std::string, float> layers;
    if (s.matchNeighbors && hierarchy.groupDepth(gid) == 0) layers = neighborAttributes(hierarchy, neighborGroups(hierarchy, gid), root);
    const TubeProfile own = placementTube(hierarchy, s);
    const TubeProfile tube = s.matchNeighbors ? neighborTube(hierarchy, neighborGroups(hierarchy, gid), root, pts.size(), own) : own;
    const GuideId id = addToGroup(hierarchy, gid, std::move(pts), tube);
    for (const auto& [name, value] : layers) hierarchy.setAttribute(id, name, value);
    hierarchy.evaluateSubtrees(&scalp, {id});
    return id;
}

}  // namespace groomlab
