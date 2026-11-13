// SPDX-License-Identifier: Apache-2.0
#include "groomlab/tube.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "groomlab/math.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr std::size_t kShapes = 4;
constexpr std::size_t kOutlinePoints = kTubeOutlinePoints;

// A built-in shape's outline and what is measured on it once.
using ShapeTable = TubeOutline;

// The outline's bounding box made -1 to 1 on both axes, centered.
void normalize(std::vector<SectionPoint>& pts) {
    float lu = std::numeric_limits<float>::max(), hu = -lu, lv = lu, hv = -lu;
    for (const SectionPoint& p : pts) {
        lu = std::min(lu, p.u), hu = std::max(hu, p.u);
        lv = std::min(lv, p.v), hv = std::max(hv, p.v);
    }
    const float cu = 0.5f * (lu + hu), cv = 0.5f * (lv + hv);
    const float su = 2.0f / std::max(hu - lu, 1e-6f), sv = 2.0f / std::max(hv - lv, 1e-6f);
    for (SectionPoint& p : pts) p = {(p.u - cu) * su, (p.v - cv) * sv};
}

std::vector<SectionPoint> makeOutline(TubeShape shape) {
    std::vector<SectionPoint> pts;
    pts.reserve(kOutlinePoints);
    switch (shape) {
        case TubeShape::Round:
            for (std::size_t i = 0; i < kOutlinePoints; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / kOutlinePoints;
                pts.push_back({pm::cos(a), pm::sin(a)});
            }
            return pts;  // the unit circle already
        case TubeShape::Ribbon: {
            // A square with rounded corners (radius 0.3): a ribbon once the aspect flattens it.
            constexpr float r = 0.3f, c = 1.0f - r;
            constexpr std::size_t perCorner = kOutlinePoints / 4;
            const std::array<SectionPoint, 4> centers{{{c, c}, {-c, c}, {-c, -c}, {c, -c}}};
            for (std::size_t k = 0; k < 4; ++k) {
                for (std::size_t i = 0; i < perCorner; ++i) {
                    const float a = 0.5f * kPi * (static_cast<float>(k) + static_cast<float>(i) / (perCorner - 1));
                    pts.push_back({centers[k].u + r * pm::cos(a), centers[k].v + r * pm::sin(a)});
                }
            }
            break;
        }
        case TubeShape::Teardrop:
            // Round at the bottom (-v), pointed at the top (+v): with the section turned to the
            // scalp (v away from it), round against the skin and its point standing away from it.
            for (std::size_t i = 0; i < kOutlinePoints; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / kOutlinePoints;
                pts.push_back({-pm::sin(a) * pm::sin(0.5f * a), pm::cos(a)});
            }
            break;
        case TubeShape::Blade:
            // A lens pointed at both ends (u = -1 and 1), its sides parabolas.
            for (std::size_t i = 0; i < kOutlinePoints; ++i) {
                const float s = 4.0f * static_cast<float>(i) / kOutlinePoints;  // 0..4 round the outline
                const float u = s < 2.0f ? 1.0f - s : s - 3.0f;
                const float side = s < 2.0f ? 1.0f : -1.0f;
                pts.push_back({u, side * (1.0f - u * u)});
            }
            break;
    }
    normalize(pts);
    return pts;
}

// Signed distance from (u, v) to a closed polygon: positive inside (crossing number).
float polygonDistance(const std::vector<SectionPoint>& poly, float u, float v) {
    float best = std::numeric_limits<float>::max();
    bool inside = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const SectionPoint& a = poly[j];
        const SectionPoint& b = poly[i];
        if ((b.v > v) != (a.v > v) && u < (a.u - b.u) * (v - b.v) / (a.v - b.v) + b.u) inside = !inside;
        const float eu = b.u - a.u, ev = b.v - a.v;
        const float l2 = eu * eu + ev * ev;
        const float f = l2 > 0.0f ? std::clamp(((u - a.u) * eu + (v - a.v) * ev) / l2, 0.0f, 1.0f) : 0.0f;
        const float du = u - (a.u + eu * f), dv = v - (a.v + ev * f);
        best = std::min(best, du * du + dv * dv);
    }
    const float d = std::sqrt(best);
    return inside ? d : -d;
}

// The outline's deepest point: on a grid over the domain, then on a finer one around it.
void measureDepth(TubeOutline& t) {
    float best = -1.0f;
    auto search = [&](float cu, float cv, float half, int steps) {
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; j <= steps; ++j) {
                const float u = cu - half + 2.0f * half * static_cast<float>(i) / static_cast<float>(steps);
                const float v = cv - half + 2.0f * half * static_cast<float>(j) / static_cast<float>(steps);
                const float d = polygonDistance(t.points, u, v);
                if (d > best) {
                    best = d;
                    t.deepest = {u, v};
                }
            }
        }
    };
    search(0.0f, 0.0f, 1.0f, 96);
    search(t.deepest.u, t.deepest.v, 2.0f / 96.0f, 16);
    t.depth = std::max(best, 1e-3f);
}

// The farthest crossing of the ray from the center at angle `a` with a closed polygon: how far
// out (0: none), and on which edge (from points[edge] to the next) how far along it.
float farthestCrossing(const std::vector<SectionPoint>& poly, float a, std::size_t* edge, float* along) {
    const float du = pm::cos(a), dv = pm::sin(a);
    const std::size_t n = poly.size();
    float far = 0.0f;
    *edge = 0;
    *along = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& p = poly[i];
        const SectionPoint& q = poly[(i + 1) % n];
        const float eu = q.u - p.u, ev = q.v - p.v;
        const float den = du * ev - dv * eu;  // the ray across the edge
        if (std::abs(den) < 1e-12f) continue;
        const float s = (p.u * ev - p.v * eu) / den;  // along the ray
        const float f = (p.u * dv - p.v * du) / den;  // along the edge
        if (f >= -1e-6f && f <= 1.0f + 1e-6f && s > far) {
            far = s;
            *edge = i;
            *along = std::clamp(f, 0.0f, 1.0f);
        }
    }
    return far;
}

// The outline's radius each way out from the center (TubeOutline::radius: along each bin's ray
// the farthest crossing of its edges), its perimeter walked evenly from where the +u ray crosses
// it, and where along that each bin's ray crosses (TubeOutline::perimeter, perimeterAt).
void measureRadius(TubeOutline& t) {
    t.radius.assign(kTubeRadialBins, 1.0f);
    t.perimeterAt.assign(kTubeRadialBins, 0.0f);
    t.perimeter.clear();
    const std::vector<SectionPoint>& poly = t.points;
    const std::size_t n = poly.size();
    if (n < 3) return;
    std::vector<float> at(n + 1, 0.0f);  // arc length to each point
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = poly[i];
        const SectionPoint& b = poly[(i + 1) % n];
        at[i + 1] = at[i] + std::hypot(b.u - a.u, b.v - a.v);
    }
    const float total = std::max(at[n], 1e-6f);
    auto arcOf = [&](std::size_t edge, float along) { return at[edge] + (at[edge + 1] - at[edge]) * along; };
    std::size_t edge = 0;
    float along = 0.0f;
    farthestCrossing(poly, 0.0f, &edge, &along);
    const float start = arcOf(edge, along);
    for (std::size_t b = 0; b < kTubeRadialBins; ++b) {
        const float a = 2.0f * kPi * static_cast<float>(b) / static_cast<float>(kTubeRadialBins);
        const float far = farthestCrossing(poly, a, &edge, &along);
        t.radius[b] = far > 1e-4f ? far : 1.0f;
        float s = (arcOf(edge, along) - start) / total;
        s -= std::floor(s);
        t.perimeterAt[b] = s >= 1.0f ? 0.0f : s;
    }
    t.perimeterAt[0] = 0.0f;  // (the start itself)
    t.perimeter.resize(kTubePerimeterPoints);
    std::size_t seg = 0;
    for (std::size_t k = 0; k < kTubePerimeterPoints; ++k) {
        float d = start + total * static_cast<float>(k) / static_cast<float>(kTubePerimeterPoints);
        if (d >= total) d -= total;
        seg = std::upper_bound(at.begin(), at.end(), d) - at.begin();
        seg = std::clamp<std::size_t>(seg, 1, n) - 1;
        const float len = at[seg + 1] - at[seg];
        const float f = len > 0.0f ? std::clamp((d - at[seg]) / len, 0.0f, 1.0f) : 0.0f;
        const SectionPoint& a = poly[seg];
        const SectionPoint& b = poly[(seg + 1) % n];
        t.perimeter[k] = {a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
    }
}

ShapeTable makeTable(TubeShape shape) {
    ShapeTable t;
    t.points = makeOutline(shape);
    // The corners: the outline's farthest point each way the shape turns sharply (or where a
    // blade's sides crest).
    std::vector<SectionPoint> ways;
    switch (shape) {
        case TubeShape::Round: break;
        case TubeShape::Ribbon: ways = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}; break;
        case TubeShape::Teardrop: ways = {{0, 1}}; break;
        case TubeShape::Blade: ways = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; break;
    }
    for (const SectionPoint& way : ways) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < t.points.size(); ++i) {
            const float d = t.points[i].u * way.u + t.points[i].v * way.v;
            if (d > t.points[best].u * way.u + t.points[best].v * way.v + 1e-6f) best = i;
        }
        t.corners.push_back(best);
    }
    measureRadius(t);
    if (shape == TubeShape::Round) return t;  // deepest at the center, 1 deep
    measureDepth(t);
    return t;
}

const ShapeTable& table(TubeShape shape) {
    static const std::array<ShapeTable, kShapes> tables = [] {
        std::array<ShapeTable, kShapes> out;
        for (std::size_t k = 0; k < kShapes; ++k) out[k] = makeTable(static_cast<TubeShape>(k));
        return out;
    }();
    const auto k = static_cast<std::size_t>(shape);
    return tables[k < kShapes ? k : 0];
}

}  // namespace

// ---------------------------------------------------------------------------

const char* tubeShapeName(TubeShape shape) {
    switch (shape) {
        case TubeShape::Round: return "round";
        case TubeShape::Ribbon: return "ribbon";
        case TubeShape::Teardrop: return "teardrop";
        case TubeShape::Blade: return "blade";
    }
    return "round";
}

bool parseTubeShape(const std::string& name, TubeShape* shape) {
    for (std::size_t k = 0; k < kShapes; ++k) {
        if (name == tubeShapeName(static_cast<TubeShape>(k))) {
            *shape = static_cast<TubeShape>(k);
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& tubeShapeNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (std::size_t k = 0; k < kShapes; ++k) out.emplace_back(tubeShapeName(static_cast<TubeShape>(k)));
        return out;
    }();
    return names;
}

float TubeProfile::widthAt(float t) const { return sampleEven(width, t, 0.1f); }
float TubeProfile::aspectAt(float t) const { return std::clamp(sampleEven(aspect, t, 1.0f), kMinTubeAspect, kMaxTubeAspect); }
float TubeProfile::twistAt(float t) const { return sampleEven(twist, t, 0.0f); }
float TubeProfile::rollAt(float t) const { return sampleEven(roll, t, 0.0f); }
TubeSection TubeProfile::sectionAt(float t) const { return {widthAt(t), aspectAt(t), twistAt(t) + rollAt(t)}; }

std::string TubeProfile::shapeName() const { return custom ? custom->name : tubeShapeName(shape); }

std::string TubeShapeRef::name() const { return custom ? custom->name : tubeShapeName(shape); }

float TubeOutline::radiusAt(float u, float v) const {
    if (radius.empty()) return 1.0f;
    if (u == 0.0f && v == 0.0f) return radius.front();
    float a = pm::atan2(v, u);
    if (a < 0.0f) a += 2.0f * kPi;
    const float x = a / (2.0f * kPi) * static_cast<float>(radius.size());
    const std::size_t i = std::min(static_cast<std::size_t>(x), radius.size() - 1);
    const float f = std::clamp(x - static_cast<float>(i), 0.0f, 1.0f);
    return radius[i] + (radius[(i + 1) % radius.size()] - radius[i]) * f;
}

SectionPoint TubeOutline::perimeterPoint(float s) const {
    if (perimeter.empty()) return {};
    const std::size_t n = perimeter.size();
    float x = (s - std::floor(s)) * static_cast<float>(n);
    const std::size_t i = std::min(static_cast<std::size_t>(x), n - 1);
    const float f = std::clamp(x - static_cast<float>(i), 0.0f, 1.0f);
    const SectionPoint& a = perimeter[i];
    const SectionPoint& b = perimeter[(i + 1) % n];
    return {a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
}

float TubeOutline::perimeterParam(float u, float v) const {
    if (perimeterAt.empty() || (u == 0.0f && v == 0.0f)) return 0.0f;
    float a = pm::atan2(v, u);
    if (a < 0.0f) a += 2.0f * kPi;
    const std::size_t n = perimeterAt.size();
    const float x = a / (2.0f * kPi) * static_cast<float>(n);
    const std::size_t i = std::min(static_cast<std::size_t>(x), n - 1);
    const float f = std::clamp(x - static_cast<float>(i), 0.0f, 1.0f);
    const float s0 = perimeterAt[i];
    float s1 = perimeterAt[(i + 1) % n];
    if (s1 - s0 > 0.5f) s1 -= 1.0f;  // (round the start: the shorter way)
    if (s0 - s1 > 0.5f) s1 += 1.0f;
    const float s = s0 + (s1 - s0) * f;
    return s - std::floor(s);
}

bool tubeShapeByName(const std::string& name, const std::map<std::string, CustomTubeShapePtr>* library, TubeShapeRef* into) {
    TubeShape builtin = TubeShape::Round;
    if (parseTubeShape(name, &builtin)) {
        *into = {builtin, nullptr};
        return true;
    }
    if (library) {
        const auto it = library->find(name);
        if (it != library->end()) {
            *into = {TubeShape::Round, it->second};
            return true;
        }
    }
    return false;
}

namespace {

// From 0 at t a to 1 at t b, smoothstep; a step at b where they meet.
float holdStep(float a, float b, float t) {
    if (b - a <= 1e-6f) return t >= b ? 1.0f : 0.0f;
    const float x = std::clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

}  // namespace

TubeBlendWeights tubeBlendWeights(const TubeBlendTiming& m, float t) {
    if (m.none()) return {};
    const float x = std::clamp(t, 0.0f, 1.0f);
    if (!m.hasMid()) {
        const float s = holdStep(m.rootEnd, m.tipStart, x);
        return {1.0f - s, 0.0f, s};
    }
    if (x <= m.midStart) {
        const float s = holdStep(m.rootEnd, m.midStart, x);
        return {1.0f - s, s, 0.0f};
    }
    if (x <= m.midEnd) return {0.0f, 1.0f, 0.0f};
    const float s = holdStep(m.midEnd, m.tipStart, x);
    return {0.0f, 1.0f - s, s};
}

SectionPoint tubeBlendAt(const TubeBlendPoint& b, float u, float v, float t) {
    if (b.none()) return {u, v};
    const TubeBlendWeights w = tubeBlendWeights(b.timing, t);
    return {w.root * u + w.mid * b.mid.u + w.tip * b.tip.u, w.root * v + w.mid * b.mid.v + w.tip * b.tip.v};
}

bool TubeProfile::blended() const {
    const TubeShapeRef root = rootShape();
    return (midShape && *midShape != root) || (tipShape && *tipShape != root);
}

TubeBlendTiming TubeProfile::blendTiming() const {
    TubeBlendTiming m;
    if (!blended()) return m;
    float rootEnd = std::clamp(rootHold, 0.0f, 1.0f);
    float tipStart = 1.0f - std::clamp(tipHold, 0.0f, 1.0f);
    if (midShape) {
        // The mid's hold centered on its place, the root's and the tip's stopping at it.
        const float at = std::clamp(midAt, 0.0f, 1.0f);
        rootEnd = std::min(rootEnd, at);
        tipStart = std::max(tipStart, at);
        const float half = 0.5f * std::clamp(midHold, 0.0f, 1.0f);
        m.midStart = std::max(at - half, rootEnd);
        m.midEnd = std::min(at + half, tipStart);
    } else if (rootEnd > tipStart) {
        rootEnd = tipStart = 0.5f * (rootEnd + tipStart);  // (overlapping holds: they meet halfway)
    }
    m.rootEnd = rootEnd;
    m.tipStart = tipStart;
    return m;
}

TubeBlendPoint TubeProfile::blendPoint(float u, float v) const {
    TubeBlendPoint b;
    b.timing = blendTiming();
    b.mid = b.tip = {u, v};
    if (b.none()) return b;
    // Where its ray crosses the root's perimeter, and how far out it lies (1 on the outline).
    const TubeOutline& root = tubeShapeOutline(rootShape());
    const float s = root.perimeterParam(u, v);
    const float r = std::hypot(u, v) / std::max(root.radiusAt(u, v), 1e-4f);
    auto place = [&](const std::optional<TubeShapeRef>& profile) -> SectionPoint {
        if (!profile) return {u, v};  // (no tip: the root's)
        const SectionPoint p = tubeShapeOutline(*profile).perimeterPoint(s);
        return {p.u * r, p.v * r};
    };
    b.mid = place(midShape);
    b.tip = place(tipShape);
    return b;
}

SectionPoint TubeProfile::unblend(float u, float v, float t) const {
    const TubeBlendTiming m = blendTiming();
    if (m.none() || (u == 0.0f && v == 0.0f)) return {u, v};
    const TubeBlendWeights w = tubeBlendWeights(m, t);
    if (w.root >= 1.0f) return {u, v};
    // The outline at t (each perimeter point the weights' mix of the profiles'), the ray through
    // (u, v) crossing it: the place along the perimeters and how far out.
    const TubeOutline& root = tubeShapeOutline(rootShape());
    const TubeOutline& mid = tubeShapeOutline(midShape ? *midShape : rootShape());
    const TubeOutline& tip = tubeShapeOutline(tipShape ? *tipShape : rootShape());
    std::vector<SectionPoint> curve(kTubePerimeterPoints);
    for (std::size_t k = 0; k < kTubePerimeterPoints; ++k) {
        const float s = static_cast<float>(k) / static_cast<float>(kTubePerimeterPoints);
        const SectionPoint a = root.perimeterPoint(s), b = mid.perimeterPoint(s), c = tip.perimeterPoint(s);
        curve[k] = {w.root * a.u + w.mid * b.u + w.tip * c.u, w.root * a.v + w.mid * b.v + w.tip * c.v};
    }
    std::size_t edge = 0;
    float along = 0.0f;
    float a = pm::atan2(v, u);
    if (a < 0.0f) a += 2.0f * kPi;
    const float far = farthestCrossing(curve, a, &edge, &along);
    if (!(far > 1e-6f)) return {u, v};
    const float r = std::hypot(u, v) / far;
    const SectionPoint p = root.perimeterPoint((static_cast<float>(edge) + along) / static_cast<float>(kTubePerimeterPoints));
    return {p.u * r, p.v * r};
}

bool TubeProfile::usesShape(const std::string& name) const {
    return (custom && custom->name == name) || (midShape && midShape->custom && midShape->custom->name == name) ||
           (tipShape && tipShape->custom && tipShape->custom->name == name);
}

bool TubeProfile::shaped() const {
    if (custom || shape != TubeShape::Round || blended()) return true;
    for (const float a : aspect) {
        if (a != 1.0f) return true;
    }
    for (const float a : twist) {
        if (a != 0.0f) return true;
    }
    return false;
}

bool TubeProfile::edited() const {
    if (shaped()) return true;
    const TubeProfile plain;
    for (const float w : width) {
        if (w != plain.width.front()) return true;
    }
    return false;
}

void TubeProfile::scaleWidth(float k) {
    for (float& w : width) w *= k;
}

std::uint64_t hashTube(std::uint64_t h, const TubeProfile& tube) {
    if (tube.custom) {
        h = hashCombine(hashString(hashCombine(h, 0xC057u), tube.custom->name), tube.custom->hash);
    } else {
        h = hashCombine(h, static_cast<std::uint64_t>(tube.shape) + 1);
    }
    for (const std::vector<float>* list : {&tube.width, &tube.aspect, &tube.twist}) {
        h = hashCombine(h, list->size());
        for (const float v : *list) h = hashFloat(h, v);
    }
    // The mid's and the tip's profiles where set (a tube of one profile hashes as it always has).
    const std::pair<std::uint64_t, const std::optional<TubeShapeRef>*> keys[] = {{0x3D1Du, &tube.midShape},
                                                                                 {0x7195u, &tube.tipShape}};
    for (const auto& [salt, ref] : keys) {
        if (!*ref) continue;
        h = hashCombine(h, salt);
        const TubeShapeRef& s = **ref;
        h = s.custom ? hashCombine(hashString(h, s.custom->name), s.custom->hash) : hashCombine(h, static_cast<std::uint64_t>(s.shape) + 1);
    }
    if (tube.rootHold != 0.0f || tube.midHold != 0.0f || tube.tipHold != 0.0f || tube.midAt != 0.5f) {
        h = hashFloat(hashFloat(hashFloat(hashFloat(hashCombine(h, 0xB01Du), tube.rootHold), tube.midHold), tube.tipHold), tube.midAt);
    }
    return h;
}

namespace {

float depthIn(const TubeOutline& t, bool round, float u, float v) {
    if (round) return 1.0f - std::sqrt(u * u + v * v);
    if (std::abs(u) > 1.0f + t.depth || std::abs(v) > 1.0f + t.depth) {
        // Far outside the domain: no point in walking the outline for the exact distance.
        return -(std::max(std::abs(u), std::abs(v)) - 1.0f) / t.depth;
    }
    return polygonDistance(t.points, u, v) / t.depth;
}

float outsideDistanceIn(const TubeOutline& t, bool round, float aspect, float u, float v, float limit) {
    const float a = std::max(aspect, 1e-6f);
    const float y = v * a;
    // The outline's box (-1..1 across, -aspect..aspect up): the outline lies inside it, so the
    // distance to the box is never more than to the outline.
    const float dx = std::max(std::abs(u) - 1.0f, 0.0f), dy = std::max(std::abs(y) - a, 0.0f);
    const float box = std::sqrt(dx * dx + dy * dy);
    if (box > limit) return box;
    if (round && a == 1.0f) return std::max(std::sqrt(u * u + v * v) - 1.0f, 0.0f);
    // The outline as drawn at the aspect: the polygon's edges with v scaled.
    const std::vector<SectionPoint>& poly = t.points;
    float best = std::numeric_limits<float>::max();
    bool inside = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float au = poly[j].u, av = poly[j].v * a, bu = poly[i].u, bv = poly[i].v * a;
        if ((bv > y) != (av > y) && u < (au - bu) * (y - bv) / (av - bv) + bu) inside = !inside;
        const float eu = bu - au, ev = bv - av;
        const float l2 = eu * eu + ev * ev;
        const float f = l2 > 0.0f ? std::clamp(((u - au) * eu + (y - av) * ev) / l2, 0.0f, 1.0f) : 0.0f;
        const float du = u - (au + eu * f), dv = y - (av + ev * f);
        best = std::min(best, du * du + dv * dv);
    }
    return inside ? 0.0f : std::sqrt(best);
}

SectionPoint randomPointIn(const TubeOutline& t, bool round, std::uint64_t seed) {
    Rng rng(seed);
    if (round) {
        const float r = std::sqrt(rng.nextFloat());
        const float a = 2.0f * kPi * rng.nextFloat();
        return {r * pm::cos(a), r * pm::sin(a)};
    }
    for (int tries = 0; tries < 64; ++tries) {
        const float u = rng.uniform(-1.0f, 1.0f);
        const float v = rng.uniform(-1.0f, 1.0f);
        if (depthIn(t, false, u, v) >= 0.0f) return {u, v};
    }
    return t.deepest;
}

}  // namespace

const std::vector<SectionPoint>& tubeOutline(TubeShape shape) { return table(shape).points; }

const std::vector<std::size_t>& tubeOutlineCorners(TubeShape shape) { return table(shape).corners; }

bool insideTubeShape(TubeShape shape, float u, float v) { return tubeShapeDepth(shape, u, v) >= 0.0f; }

float tubeShapeDepth(TubeShape shape, float u, float v) { return depthIn(table(shape), shape == TubeShape::Round, u, v); }

float tubeOutsideDistance(TubeShape shape, float aspect, float u, float v, float limit) {
    return outsideDistanceIn(table(shape), shape == TubeShape::Round, aspect, u, v, limit);
}

SectionPoint randomPointInTubeShape(TubeShape shape, std::uint64_t seed) {
    return randomPointIn(table(shape), shape == TubeShape::Round, seed);
}

const TubeOutline& tubeShapeOutline(const TubeProfile& tube) { return tube.custom ? tube.custom->outline : table(tube.shape); }

const TubeOutline& tubeShapeOutline(const TubeShapeRef& shape) { return shape.custom ? shape.custom->outline : table(shape.shape); }

const std::vector<SectionPoint>& tubeOutline(const TubeProfile& tube) { return tubeShapeOutline(tube).points; }

const std::vector<std::size_t>& tubeOutlineCorners(const TubeProfile& tube) { return tubeShapeOutline(tube).corners; }

bool insideTubeShape(const TubeProfile& tube, float u, float v) { return tubeShapeDepth(tube, u, v) >= 0.0f; }

float tubeShapeDepth(const TubeProfile& tube, float u, float v) {
    return depthIn(tubeShapeOutline(tube), tube.roundShape(), u, v);
}

float tubeOutsideDistance(const TubeProfile& tube, float aspect, float u, float v, float limit) {
    return outsideDistanceIn(tubeShapeOutline(tube), tube.roundShape(), aspect, u, v, limit);
}

SectionPoint randomPointInTubeShape(const TubeProfile& tube, std::uint64_t seed) {
    return randomPointIn(tubeShapeOutline(tube), tube.roundShape(), seed);
}

// ---------------------------------------------------------------------------
// Custom shapes

namespace {

// Twice the signed area of a closed polygon: positive counter-clockwise.
float signedArea2(const std::vector<SectionPoint>& p) {
    float a = 0.0f;
    for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += p[j].u * p[i].v - p[i].u * p[j].v;
    return a;
}

float orient(const SectionPoint& a, const SectionPoint& b, const SectionPoint& c) {
    return (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
}

// Segments ab and cd touch or cross (collinear overlaps included).
bool segmentsMeet(const SectionPoint& a, const SectionPoint& b, const SectionPoint& c, const SectionPoint& d) {
    const float d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }
    auto onSegment = [](const SectionPoint& p, const SectionPoint& q, const SectionPoint& r) {  // r on pq, given collinear
        return std::min(p.u, q.u) <= r.u && r.u <= std::max(p.u, q.u) && std::min(p.v, q.v) <= r.v && r.v <= std::max(p.v, q.v);
    };
    return (d1 == 0.0f && onSegment(c, d, a)) || (d2 == 0.0f && onSegment(c, d, b)) || (d3 == 0.0f && onSegment(a, b, c)) ||
           (d4 == 0.0f && onSegment(a, b, d));
}

// No two edges of the closed polygon meet but neighbours at their shared point.
bool simplePolygon(const std::vector<SectionPoint>& p) {
    const std::size_t n = p.size();
    if (n < 3) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = p[i];
        const SectionPoint& b = p[(i + 1) % n];
        // Bounding boxes first: most pairs are far apart.
        const float lu = std::min(a.u, b.u), hu = std::max(a.u, b.u), lv = std::min(a.v, b.v), hv = std::max(a.v, b.v);
        for (std::size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;  // neighbours round the end
            const SectionPoint& c = p[j];
            const SectionPoint& d = p[(j + 1) % n];
            if (std::max(c.u, d.u) < lu || std::min(c.u, d.u) > hu || std::max(c.v, d.v) < lv || std::min(c.v, d.v) > hv) continue;
            if (segmentsMeet(a, b, c, d)) return false;
        }
    }
    return true;
}

// The closed curve through the points, `per` samples per span (from each control point, which
// is the span's first sample, to the next).
std::vector<SectionPoint> denseCurve(const std::vector<SectionPoint>& pts, bool smooth, std::size_t per) {
    const std::size_t n = pts.size();
    std::vector<SectionPoint> out;
    out.reserve(n * per);
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& p0 = pts[(i + n - 1) % n];
        const SectionPoint& p1 = pts[i];
        const SectionPoint& p2 = pts[(i + 1) % n];
        const SectionPoint& p3 = pts[(i + 2) % n];
        for (std::size_t k = 0; k < per; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(per);
            if (!smooth) {
                out.push_back({p1.u + (p2.u - p1.u) * t, p1.v + (p2.v - p1.v) * t});
                continue;
            }
            // Centripetal Catmull-Rom (Barry and Goldman's pyramid, knots spaced by the square
            // root of each span's length): through every point, with no loops or cusps where the
            // points crowd (a point added between two near ones bends the curve, never knots it).
            auto knot = [](const SectionPoint& a, const SectionPoint& b) {
                return std::max(std::sqrt(std::hypot(b.u - a.u, b.v - a.v)), 1e-4f);
            };
            const float k0 = 0.0f, k1 = k0 + knot(p0, p1), k2 = k1 + knot(p1, p2), k3 = k2 + knot(p2, p3);
            const float x = k1 + (k2 - k1) * t;
            auto mix = [](const SectionPoint& a, const SectionPoint& b, float ka, float kb, float at) {
                const float f = (at - ka) / (kb - ka);
                return SectionPoint{a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
            };
            const SectionPoint a1 = mix(p0, p1, k0, k1, x), a2 = mix(p1, p2, k1, k2, x), a3 = mix(p2, p3, k2, k3, x);
            const SectionPoint b1 = mix(a1, a2, k0, k2, x), b2 = mix(a2, a3, k1, k3, x);
            out.push_back(mix(b1, b2, k1, k2, x));
        }
    }
    return out;
}

std::size_t samplesPerSpan(std::size_t points) { return std::clamp<std::size_t>(512 / std::max<std::size_t>(points, 1), 4, 16); }

// `count` points evenly along a closed polyline, starting at its first.
std::vector<SectionPoint> evenlyAlong(const std::vector<SectionPoint>& poly, std::size_t count) {
    const std::size_t n = poly.size();
    std::vector<float> at(n + 1, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = poly[i];
        const SectionPoint& b = poly[(i + 1) % n];
        at[i + 1] = at[i] + std::hypot(b.u - a.u, b.v - a.v);
    }
    std::vector<SectionPoint> out(count);
    std::size_t seg = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const float s = at[n] * static_cast<float>(k) / static_cast<float>(count);
        while (seg + 1 < n && at[seg + 1] <= s) ++seg;
        const float len = at[seg + 1] - at[seg];
        const float f = len > 0.0f ? (s - at[seg]) / len : 0.0f;
        const SectionPoint& a = poly[seg];
        const SectionPoint& b = poly[(seg + 1) % n];
        out[k] = {a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
    }
    return out;
}

}  // namespace

void checkTubeShapeName(const std::string& name) {
    if (name.empty()) throw std::invalid_argument("a tube shape needs a name");
    if (name.size() > 64) throw std::invalid_argument("tube shape names: 64 characters at most");
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) throw std::invalid_argument("tube shape '" + name + "': letters, digits, spaces, '_', '-' and '.' only");
    }
    if (name.front() == ' ' || name.back() == ' ') throw std::invalid_argument("tube shape '" + name + "': no spaces at the ends");
    TubeShape builtin;
    if (parseTubeShape(name, &builtin)) throw std::invalid_argument("'" + name + "' is a built-in tube shape: pick another name");
}

std::vector<SectionPoint> sampleTubeShapeCurve(const std::vector<SectionPoint>& points, bool smooth) {
    if (points.size() < 3) return points;
    return denseCurve(points, smooth, samplesPerSpan(points.size()));
}

namespace {

CustomTubeShapePtr buildCustomTubeShape(const std::string& name, const std::vector<SectionPoint>& given,
                                        bool smooth);

}  // namespace

CustomTubeShapePtr makeCustomTubeShape(const std::string& name, const std::vector<SectionPoint>& given, bool smooth) {
    // Made once per name, points and smoothness and shared after: a host command reads the groom's
    // guides (their library with them) at every call, and sampling, checking and measuring each
    // shape again made every such call slow on a groom with a few library shapes (a panel polling
    // them lagged the viewport). A shape is never changed once made (a library edit makes a new
    // one), so the same inputs may share it.
    std::string key = name;
    key.push_back(smooth ? 's' : 'c');  // (smooth or corners)
    key.append(reinterpret_cast<const char*>(given.data()), given.size() * sizeof(SectionPoint));
    static std::mutex mutex;
    static std::unordered_map<std::string, CustomTubeShapePtr> made;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = made.find(key);
        if (it != made.end()) return it->second;
    }
    CustomTubeShapePtr shape = buildCustomTubeShape(name, given, smooth);  // (throws: nothing kept)
    const std::lock_guard<std::mutex> lock(mutex);
    if (made.size() >= 512) made.clear();  // (shapes edited many times over: start again)
    made.emplace(std::move(key), shape);
    return shape;
}

namespace {

CustomTubeShapePtr buildCustomTubeShape(const std::string& name, const std::vector<SectionPoint>& given,
                                        bool smooth) {
    checkTubeShapeName(name);
    if (given.size() > kMaxTubeShapePoints) {
        throw std::invalid_argument("tube shape '" + name + "': " + std::to_string(kMaxTubeShapePoints) + " points at most");
    }
    // The points, each a number, those on top of the one before dropped.
    std::vector<SectionPoint> pts;
    float lu = std::numeric_limits<float>::max(), hu = -lu, lv = lu, hv = -lu;
    for (const SectionPoint& p : given) {
        if (!std::isfinite(p.u) || !std::isfinite(p.v)) throw std::invalid_argument("tube shape '" + name + "': points must be numbers");
        lu = std::min(lu, p.u), hu = std::max(hu, p.u), lv = std::min(lv, p.v), hv = std::max(hv, p.v);
    }
    float size = given.empty() ? 0.0f : std::max(hu - lu, hv - lv);
    for (const SectionPoint& p : given) {
        if (!pts.empty() && std::hypot(p.u - pts.back().u, p.v - pts.back().v) <= 1e-4f * size) continue;
        pts.push_back(p);
    }
    while (pts.size() > 1 && std::hypot(pts.front().u - pts.back().u, pts.front().v - pts.back().v) <= 1e-4f * size) pts.pop_back();
    if (pts.size() < 3) throw std::invalid_argument("tube shape '" + name + "': at least 3 points, apart");
    const std::size_t per = samplesPerSpan(pts.size());
    std::vector<SectionPoint> dense;
    // Fitted into the domain: the curve's box -1 to 1 on both axes, centered, the points moved and
    // scaled so (a few rounds: a centripetal curve scaled unevenly is close to the curve of the
    // points scaled, not the same).
    for (int round = 0; round < 3; ++round) {
        dense = denseCurve(pts, smooth, per);
        lu = std::numeric_limits<float>::max(), hu = -lu, lv = lu, hv = -lu;
        for (const SectionPoint& p : dense) lu = std::min(lu, p.u), hu = std::max(hu, p.u), lv = std::min(lv, p.v), hv = std::max(hv, p.v);
        if (!(hu - lu > 1e-3f * size) || !(hv - lv > 1e-3f * size)) {
            throw std::invalid_argument("tube shape '" + name + "': the points lie on a line; give the outline width and height");
        }
        const float cu = 0.5f * (lu + hu), cv = 0.5f * (lv + hv), su = 2.0f / (hu - lu), sv = 2.0f / (hv - lv);
        for (SectionPoint& p : pts) p = {(p.u - cu) * su, (p.v - cv) * sv};
        size = 2.0f;
    }
    if (signedArea2(denseCurve(pts, smooth, per)) < 0.0f) std::reverse(pts.begin(), pts.end());  // counter-clockwise
    dense = denseCurve(pts, smooth, per);
    if (!simplePolygon(dense)) {
        throw std::invalid_argument("tube shape '" + name + "': the outline crosses itself; move the points so its edges don't cross");
    }
    if (polygonDistance(dense, 0.0f, 0.0f) < 0.02f) {
        throw std::invalid_argument("tube shape '" + name +
                                    "': the outline must enclose its center (the guide runs through it); keep the middle of "
                                    "the square inside the shape");
    }
    auto shape = std::make_shared<CustomTubeShape>();
    shape->name = name;
    shape->points = pts;
    shape->smooth = smooth;
    std::uint64_t h = hashCombine(hash64(pts.size()), smooth ? 1 : 2);
    for (const SectionPoint& p : pts) h = hashFloat(hashFloat(h, p.u), p.v);
    shape->hash = h;
    // The outline evenly along the curve; the corners (where the points turn sharply: 20 degrees
    // between straight edges, 60 on a smooth curve) put in place of the nearest outline points.
    TubeOutline& t = shape->outline;
    t.points = evenlyAlong(dense, kOutlinePoints);
    const float sharp = std::cos((smooth ? 60.0f : 20.0f) * kPi / 180.0f);
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = pts[(i + n - 1) % n];
        const SectionPoint& b = pts[i];
        const SectionPoint& c = pts[(i + 1) % n];
        const float iu = b.u - a.u, iv = b.v - a.v, ou = c.u - b.u, ov = c.v - b.v;
        const float li = std::hypot(iu, iv), lo = std::hypot(ou, ov);
        if (!(li > 0.0f && lo > 0.0f) || (iu * ou + iv * ov) / (li * lo) > sharp) continue;
        std::size_t best = 0;
        float bestD = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < t.points.size(); ++k) {
            const float d = std::hypot(t.points[k].u - b.u, t.points[k].v - b.v);
            if (d < bestD) bestD = d, best = k;
        }
        if (std::find(t.corners.begin(), t.corners.end(), best) != t.corners.end()) continue;
        t.points[best] = b;
        t.corners.push_back(best);
    }
    std::sort(t.corners.begin(), t.corners.end());
    measureDepth(t);
    measureRadius(t);
    return shape;
}

}  // namespace

std::vector<SectionPoint> builtinTubeShapePoints(TubeShape shape, bool* smooth) {
    std::vector<SectionPoint> pts;
    bool round = true;
    switch (shape) {
        case TubeShape::Round:
            for (int i = 0; i < 8; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / 8.0f;
                pts.push_back({pm::cos(a), pm::sin(a)});
            }
            break;
        case TubeShape::Ribbon: {
            // Three points round each rounded corner, one in the middle of each side (so the sides stay straight).
            constexpr float r = 0.3f, c = 1.0f - r;
            const std::array<SectionPoint, 4> centers{{{c, c}, {-c, c}, {-c, -c}, {c, -c}}};
            const std::array<SectionPoint, 4> sides{{{0, 1}, {-1, 0}, {0, -1}, {1, 0}}};
            for (std::size_t k = 0; k < 4; ++k) {
                for (int i = 0; i < 3; ++i) {
                    const float a = 0.5f * kPi * (static_cast<float>(k) + 0.5f * static_cast<float>(i));
                    pts.push_back({centers[k].u + r * pm::cos(a), centers[k].v + r * pm::sin(a)});
                }
                pts.push_back(sides[k]);
            }
            break;
        }
        case TubeShape::Teardrop:
            for (int i = 0; i < 12; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / 12.0f;
                pts.push_back({-pm::sin(a) * pm::sin(0.5f * a), pm::cos(a)});
            }
            break;
        case TubeShape::Blade:
            round = false;  // straight edges: its two points stay sharp
            for (int i = 0; i < 16; ++i) {
                const float s = 4.0f * static_cast<float>(i) / 16.0f;
                const float u = s < 2.0f ? 1.0f - s : s - 3.0f;
                const float side = s < 2.0f ? 1.0f : -1.0f;
                pts.push_back({u, side * (1.0f - u * u)});
            }
            break;
    }
    normalize(pts);
    if (smooth) *smooth = round;
    return pts;
}

Vec3f sectionOffset(const TubeSection& section, float u, float v, float scale) {
    const float w = section.width * scale;
    const float h = w * section.aspect;
    const float c = pm::cos(section.twist), s = pm::sin(section.twist);
    const float x = u * w, y = v * h;
    return {x * c - y * s, x * s + y * c, 0.0f};
}

SectionPoint sectionCoords(const TubeSection& section, float x, float y, float scale) {
    const float c = pm::cos(-section.twist), s = pm::sin(-section.twist);
    const float rx = x * c - y * s, ry = x * s + y * c;
    const float w = std::max(section.width * scale, 1e-6f);
    const float h = std::max(section.width * scale * section.aspect, 1e-6f);
    return {rx / w, ry / h};
}

// ---------------------------------------------------------------------------
// TubeSpine

TubeSpine::TubeSpine(const Vec3f* points, std::size_t count, const Vec3f& frameUp, const TubeProfile& tube, float scale)
    : tube_(tube), blended_(tube.blended()) {
    if (count < 2) return;
    const std::vector<Frame> frames = rotationMinimizingFrames(points, count, frameUp);
    centers_.assign(points, points + count);
    axisU_.resize(count);
    axisV_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        const TubeSection sec = tube.sectionAt(t);
        const float w = sec.width * scale, h = w * sec.aspect;
        const float c = pm::cos(sec.twist), s = pm::sin(sec.twist);
        axisU_[i] = frames[i].normal * (c * w) + frames[i].binormal * (s * w);
        axisV_[i] = frames[i].normal * (-s * h) + frames[i].binormal * (c * h);
    }
    footU_ = axisU_.front();
    footV_ = axisV_.front();
    footNormal_ = frames.front().tangent;
    const TubeSection root = tube.sectionAt(0.0f);
    rootWidth_ = root.width * scale;
    rootHeight_ = rootWidth_ * root.aspect;
    // A strand rooted on the footprint (not in the root section) joins the sections over
    // kJoinExtents of the root section's larger half-extent along the guide.
    constexpr float kJoinExtents = 3.0f;
    float length = 0.0f;
    for (std::size_t i = 1; i < count; ++i) length += (points[i] - points[i - 1]).length();
    const float join = kJoinExtents * std::max(rootWidth_, rootHeight_);
    rootJoin_ = length > 0.0f && join < length ? std::max(join / length, 1e-3f) : 1.0f;
}

void TubeSpine::setRootSurface(const Vec3f& normal) {
    if (centers_.empty()) return;
    const float len = normal.length();
    if (!(len > 1e-12f) || !std::isfinite(len)) return;
    const Vec3f n = normal * (1.0f / len);
    const Vec3f tangent = footNormal_;  // the root's tangent until laid
    if (tangent.dot(n) < -1.0f + 1e-4f) return;  // against the guide: no telling which way to lay it
    const Rotation turn(tangent, n);
    footU_ = turn(footU_);
    footV_ = turn(footV_);
    footNormal_ = n;
}

Vec3f TubeSpine::at(float t, float u, float v) const {
    if (blended_) return at(t, u, v, tube_.blendPoint(u, v));
    const std::size_t n = centers_.size();
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(x), n - 2);
    const float f = x - static_cast<float>(i);
    if (f == 0.0f) return atSample(i, u, v);
    return atSample(i, u, v) * (1.0f - f) + atSample(i + 1, u, v) * f;
}

Vec3f TubeSpine::atSample(std::size_t i, float u, float v, const TubeBlendPoint& blend) const {
    if (blend.none()) return centers_[i] + axisU_[i] * u + axisV_[i] * v;
    const std::size_t n = centers_.size();
    const SectionPoint q = tubeBlendAt(blend, u, v, n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f);
    return centers_[i] + axisU_[i] * q.u + axisV_[i] * q.v;
}

Vec3f TubeSpine::at(float t, float u, float v, const TubeBlendPoint& blend) const {
    const std::size_t n = centers_.size();
    const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(x), n - 2);
    const float f = x - static_cast<float>(i);
    if (f == 0.0f) return atSample(i, u, v, blend);
    return atSample(i, u, v, blend) * (1.0f - f) + atSample(i + 1, u, v, blend) * f;
}

SectionPoint TubeSpine::rootCoords(const Vec3f& p, float* along) const {
    const Vec3f d = p - centers_.front();
    const float off = d.dot(footNormal_);
    if (along) *along = off;
    // The footprint's axes are perpendicular (the section's, turned): each coordinate on its own.
    const Vec3f planar = d - footNormal_ * off;
    const float uu = std::max(footU_.dot(footU_), 1e-20f), vv = std::max(footV_.dot(footV_), 1e-20f);
    return {planar.dot(footU_) / uu, planar.dot(footV_) / vv};
}

// ---------------------------------------------------------------------------
// JSON

namespace {

using json = nlohmann::json;

std::vector<float> resampled(const std::vector<float>& values, std::size_t n, float fallback) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = sampleEven(values, n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f, fallback);
    return out;
}

std::vector<float> listOf(const json& j, const char* key) {
    std::vector<float> out;
    if (j.is_number()) {
        out.push_back(j.get<float>());
    } else if (j.is_array()) {
        for (const json& v : j) {
            if (!v.is_number()) throw std::invalid_argument(std::string("tube ") + key + ": numbers only");
            out.push_back(v.get<float>());
        }
    } else {
        throw std::invalid_argument(std::string("tube ") + key + ": a number or a list of numbers");
    }
    if (out.empty()) throw std::invalid_argument(std::string("tube ") + key + ": an empty list");
    for (const float v : out) {
        if (!std::isfinite(v)) throw std::invalid_argument(std::string("tube ") + key + ": not a number");
    }
    return out;
}

}  // namespace

std::string tubeToJson(const TubeProfile& tube, float scale) {
    std::vector<float> width = tube.width;
    for (float& w : width) w *= scale;
    json j = {{"shape", tube.shapeName()}, {"width", width}, {"aspect", tube.aspect}, {"twist", tube.twist}};
    if (tube.midShape) j["midShape"] = tube.midShape->name();
    if (tube.tipShape) j["tipShape"] = tube.tipShape->name();
    if (tube.rootHold != 0.0f) j["rootHold"] = tube.rootHold;
    if (tube.midHold != 0.0f) j["midHold"] = tube.midHold;
    if (tube.tipHold != 0.0f) j["tipHold"] = tube.tipHold;
    if (tube.midAt != 0.5f) j["midAt"] = tube.midAt;
    return j.dump();
}

TubeProfile editedTube(const TubeProfile& base, const std::string& editJson, float scale, std::size_t points,
                       const TubeShapeLibrary* library) {
    json j;
    try {
        j = json::parse(editJson);
    } catch (const json::exception& e) {
        throw std::invalid_argument(std::string("tube: ") + e.what());
    }
    if (!j.is_object()) throw std::invalid_argument("tube: a JSON object");
    TubeProfile tube = base;
    if (j.contains("shape")) {
        const std::string name = j["shape"].is_string() ? j["shape"].get<std::string>() : "";
        TubeShape builtin = TubeShape::Round;
        if (parseTubeShape(name, &builtin)) {
            tube.shape = builtin;
            tube.custom = nullptr;
        } else if (library && library->count(name)) {
            tube.shape = TubeShape::Round;
            tube.custom = library->at(name);
        } else {
            std::string known = "round, ribbon, teardrop, blade";
            if (library) {
                for (const auto& entry : *library) known += ", " + entry.first;
            }
            throw std::invalid_argument("tube shape '" + name + "': one of " + known);
        }
    }
    // The mid's and the tip's profiles: a shape's name, or null / "" to unset.
    for (const auto& [key, into] : {std::pair<const char*, std::optional<TubeShapeRef>*>{"midShape", &tube.midShape},
                                    std::pair<const char*, std::optional<TubeShapeRef>*>{"tipShape", &tube.tipShape}}) {
        if (!j.contains(key)) continue;
        const json& value = j[key];
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
            into->reset();
            continue;
        }
        const std::string name = value.is_string() ? value.get<std::string>() : "";
        TubeShapeRef ref;
        if (!tubeShapeByName(name, library, &ref)) {
            std::string known = "round, ribbon, teardrop, blade";
            if (library) {
                for (const auto& entry : *library) known += ", " + entry.first;
            }
            throw std::invalid_argument(std::string("tube ") + key + " '" + name + "': one of " + known + ", or none");
        }
        *into = ref;
    }
    for (const auto& [key, into] : {std::pair<const char*, float*>{"rootHold", &tube.rootHold},
                                    std::pair<const char*, float*>{"midHold", &tube.midHold},
                                    std::pair<const char*, float*>{"tipHold", &tube.tipHold},
                                    std::pair<const char*, float*>{"midAt", &tube.midAt}}) {
        if (!j.contains(key)) continue;
        if (!j[key].is_number() || !(j[key].get<double>() >= 0.0 && j[key].get<double>() <= 1.0)) {
            throw std::invalid_argument(std::string("tube ") + key + ": a number, 0 to 1");
        }
        *into = j[key].get<float>();
    }
    if (j.contains("width")) {
        tube.width = listOf(j["width"], "width");
        for (float& w : tube.width) {
            if (w < 0.0f) throw std::invalid_argument("tube width: 0 or more");
            w /= scale > 0.0f ? scale : 1.0f;
        }
    }
    if (j.contains("aspect")) {
        tube.aspect = listOf(j["aspect"], "aspect");
        for (const float a : tube.aspect) {
            if (a <= 0.0f) throw std::invalid_argument("tube aspect: above 0");
        }
    }
    if (j.contains("twist")) tube.twist = listOf(j["twist"], "twist");
    if (j.contains("scaleWidth")) {
        if (!j["scaleWidth"].is_number() || !(j["scaleWidth"].get<double>() >= 0.0)) {
            throw std::invalid_argument("tube scaleWidth: a number, 0 or more");
        }
        tube.scaleWidth(j["scaleWidth"].get<float>());
    }
    if (j.contains("samples")) {
        if (!j["samples"].is_number_integer() || j["samples"].get<int>() < 2 || j["samples"].get<int>() > 1024) {
            throw std::invalid_argument("tube samples: 2 to 1024");
        }
        const auto n = static_cast<std::size_t>(j["samples"].get<int>());
        tube.width = resampled(tube.width, n, 0.1f);
        tube.aspect = resampled(tube.aspect, n, 1.0f);
        tube.twist = resampled(tube.twist, n, 0.0f);
    }
    if (j.contains("smooth")) {
        const std::string which = j["smooth"].is_string() ? j["smooth"].get<std::string>() : "";
        const unsigned lists = which == "width" ? kTubeWidthList : which == "aspect" ? kTubeAspectList
                               : which == "twist" ? kTubeTwistList : which == "all" ? kTubeAllLists : 0u;
        if (lists == 0) throw std::invalid_argument("tube smooth: width, aspect, twist or all");
        int passes = 3;
        if (j.contains("passes")) {
            if (!j["passes"].is_number_integer() || j["passes"].get<int>() < 1 || j["passes"].get<int>() > 100) {
                throw std::invalid_argument("tube passes: 1 to 100");
            }
            passes = j["passes"].get<int>();
        }
        std::size_t n = std::max<std::size_t>(points, 2);
        for (const std::vector<float>* list : {&tube.width, &tube.aspect, &tube.twist}) n = std::max(n, list->size());
        tube = smoothedTube(tube, lists, n, passes);
    }
    return tube;
}

TubeProfile smoothedTube(const TubeProfile& tube, unsigned lists, std::size_t samples, int passes, float amount,
                         const std::vector<float>* weights) {
    TubeProfile out = tube;
    const std::size_t n = std::max<std::size_t>(samples, 2);
    const float k = std::clamp(amount, 0.0f, 1.0f);
    const bool weighed = weights && weights->size() == n;
    const std::array<std::pair<unsigned, std::vector<float>*>, 3> all{
        {{kTubeWidthList, &out.width}, {kTubeAspectList, &out.aspect}, {kTubeTwistList, &out.twist}}};
    for (const auto& [bit, list] : all) {
        if (!(lists & bit)) continue;
        const float fallback = bit == kTubeWidthList ? 0.1f : bit == kTubeAspectList ? 1.0f : 0.0f;
        std::vector<float> v = resampled(*list, n, fallback);
        std::vector<float> next = v;
        for (int pass = 0; pass < passes; ++pass) {
            for (std::size_t i = 1; i + 1 < n; ++i) {
                const float w = k * (weighed ? std::clamp((*weights)[i], 0.0f, 1.0f) : 1.0f);
                next[i] = v[i] + (0.5f * (v[i - 1] + v[i + 1]) - v[i]) * w;
            }
            v.swap(next);
            next = v;
        }
        *list = std::move(v);
    }
    return out;
}

TubeProfile blendedTube(const std::vector<const TubeProfile*>& tubes, const std::vector<float>& weights, std::size_t samples) {
    TubeProfile out;
    if (tubes.empty()) return out;
    out.shape = tubes.front()->shape;
    out.custom = tubes.front()->custom;
    out.midShape = tubes.front()->midShape;
    out.tipShape = tubes.front()->tipShape;
    out.rootHold = tubes.front()->rootHold;
    out.midHold = tubes.front()->midHold;
    out.tipHold = tubes.front()->tipHold;
    out.midAt = tubes.front()->midAt;
    const std::size_t n = std::max<std::size_t>(samples, 2);
    float total = 0.0f;
    for (std::size_t k = 0; k < tubes.size(); ++k) total += k < weights.size() ? std::max(weights[k], 0.0f) : 0.0f;
    auto blend = [&](float (TubeProfile::*at)(float) const) {
        std::vector<float> v(n, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            if (!(total > 0.0f)) {
                v[i] = (tubes.front()->*at)(t);
                continue;
            }
            for (std::size_t k = 0; k < tubes.size(); ++k) {
                const float w = k < weights.size() ? std::max(weights[k], 0.0f) : 0.0f;
                v[i] += (tubes[k]->*at)(t) * (w / total);
            }
        }
        // As few values as say the same: one, or root and tip on a line.
        const float scaleOf = std::max({std::abs(v.front()), std::abs(v.back()), 1e-6f});
        bool constant = true, linear = true;
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            constant = constant && std::abs(v[i] - v[0]) <= 1e-5f * scaleOf;
            linear = linear && std::abs(v[i] - (v.front() * (1.0f - t) + v.back() * t)) <= 1e-5f * scaleOf;
        }
        if (constant) return std::vector<float>{v[0]};
        if (linear) return std::vector<float>{v.front(), v.back()};
        return v;
    };
    out.width = blend(&TubeProfile::widthAt);
    out.aspect = blend(&TubeProfile::aspectAt);
    out.twist = blend(&TubeProfile::twistAt);
    return out;
}

}  // namespace groomlab
