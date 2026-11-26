// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_grooming.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace groomlab::agent {

namespace {

constexpr float kPi = 3.14159265358979f;

float polylineLength(const std::vector<Vec3f>& p) {
    float length = 0.0f;
    for (std::size_t i = 1; i < p.size(); ++i) length += (p[i] - p[i - 1]).length();
    return length;
}

// The landmarks' directions in the frame: (left, up, front).
struct Probe {
    const char* name;
    float left, up, front;
};
const Probe kProbes[] = {
    {"top", 0.0f, 1.0f, 0.0f},
    {"crown", 0.0f, 0.8f, -0.6f},
    {"front_hairline", 0.0f, 0.55f, 0.83f},
    {"nape", 0.0f, -0.55f, -0.83f},
    {"back", 0.0f, 0.0f, -1.0f},
    {"left_temple", 0.75f, 0.35f, 0.56f},
    {"right_temple", -0.75f, 0.35f, 0.56f},
    {"left_ear", 1.0f, -0.05f, -0.1f},
    {"right_ear", -1.0f, -0.05f, -0.1f},
};

}  // namespace

ScalpFrame ScalpFrame::of(const std::string& upAxis) {
    ScalpFrame f;
    if (upAxis == "Z") {
        f.up = {0, 0, 1};
        f.front = {0, -1, 0};
    } else {
        f.up = {0, 1, 0};
        f.front = {0, 0, 1};
    }
    f.left = f.up.cross(f.front);
    return f;
}

const std::vector<std::string>& landmarkNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Probe& p : kProbes) out.push_back(p.name);
        return out;
    }();
    return names;
}

const Landmark* ScalpLandmarks::find(const std::string& name) const {
    for (const Landmark& l : points) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

ScalpLandmarks scalpLandmarks(const ScalpMesh& scalp, const std::string& upAxis) {
    ScalpLandmarks out;
    out.frame = ScalpFrame::of(upAxis);
    const auto& pts = scalp.points();
    if (pts.empty() || scalp.triangleCount() == 0) throw std::invalid_argument("the scalp has no triangles");
    out.lo = out.hi = pts.front();
    for (const Vec3f& p : pts) {
        out.lo = {std::min(out.lo.x, p.x), std::min(out.lo.y, p.y), std::min(out.lo.z, p.z)};
        out.hi = {std::max(out.hi.x, p.x), std::max(out.hi.y, p.y), std::max(out.hi.z, p.z)};
    }
    const Vec3f extent = out.hi - out.lo;
    out.size = std::max({extent.x, extent.y, extent.z});
    // The head as a ball under the top, as wide as the scalp across (a head with its neck, or a
    // cap: the ball's middle below it), the probes on it.
    const Vec3f boxMiddle = (out.lo + out.hi) * 0.5f;
    const float radius = std::max({0.5f * std::abs(extent.dot(out.frame.left)), 0.5f * std::abs(extent.dot(out.frame.front)), 1e-6f});
    const float top = std::max(out.lo.dot(out.frame.up), out.hi.dot(out.frame.up));
    out.middle = boxMiddle + out.frame.up * (top - radius - boxMiddle.dot(out.frame.up));
    for (const Probe& probe : kProbes) {
        const Vec3f d = out.frame.left * probe.left + out.frame.up * probe.up + out.frame.front * probe.front;
        const SurfacePoint at = scalp.closestPoint(out.middle + d.normalized() * radius);
        out.points.push_back({probe.name, at.position, at.normal});
    }
    return out;
}

std::vector<Vec3f> scalpPath(const ScalpMesh& scalp, const Vec3f& middle, const std::vector<Vec3f>& stops, float spacing,
                             float offset) {
    if (stops.size() < 2) throw std::invalid_argument("a path needs two points at least");
    if (!(spacing > 0.0f)) throw std::invalid_argument("the path's spacing must be positive");
    std::vector<Vec3f> out;
    auto put = [&](const Vec3f& p) {
        const SurfacePoint at = scalp.closestPoint(p);
        out.push_back(at.position + at.normal * offset);
    };
    for (std::size_t s = 0; s + 1 < stops.size(); ++s) {
        const Vec3f a = stops[s] - middle, b = stops[s + 1] - middle;
        const float ra = a.length(), rb = b.length();
        const Vec3f da = ra > 1e-9f ? a * (1.0f / ra) : Vec3f(0, 0, 0), db = rb > 1e-9f ? b * (1.0f / rb) : Vec3f(0, 0, 0);
        const float angle = std::acos(std::clamp(da.dot(db), -1.0f, 1.0f));
        // Roughly the arc's length, for the samples' count.
        const float arc = std::max(angle * 0.5f * (ra + rb), (stops[s + 1] - stops[s]).length());
        const int n = std::clamp(static_cast<int>(std::ceil(arc / spacing)), 1, 2000);
        for (int i = s == 0 ? 0 : 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            Vec3f dir;
            if (angle < 1e-4f || angle > kPi - 1e-3f || ra <= 1e-9f || rb <= 1e-9f) {
                dir = (a + (b - a) * t);  // (straight across: no great circle to follow)
                const float len = dir.length();
                dir = len > 1e-9f ? dir * (1.0f / len) : da;
            } else {
                const float sa = std::sin((1.0f - t) * angle) / std::sin(angle), sb = std::sin(t * angle) / std::sin(angle);
                dir = (da * sa + db * sb).normalized();
            }
            put(middle + dir * (ra + (rb - ra) * t));
        }
    }
    return out;
}

std::vector<GuideId> guidesInSphere(const GuideHierarchy& h, const Vec3f& center, float radius, bool rootsOnly, int level,
                                    const std::string& group) {
    std::vector<GuideId> out;
    const float r2 = radius * radius;
    for (const HierarchyGuide& g : h.guides()) {
        if (g.points.empty()) continue;
        if (level >= 0 && h.groupDepth(g.group) != level) continue;
        if (!group.empty()) {
            const GuideGroup* gr = h.group(g.group);
            if (!gr || gr->name != group) continue;
        }
        bool inside = false;
        const std::size_t n = rootsOnly ? 1 : g.points.size();
        for (std::size_t i = 0; i < n && !inside; ++i) inside = (g.points[i] - center).dot(g.points[i] - center) <= r2;
        if (inside) out.push_back(g.id);
    }
    return out;
}

GuideChange guideChange(const GuideHierarchy& before, const GuideHierarchy& after) {
    GuideChange c;
    std::map<GuideId, const HierarchyGuide*> old;
    for (const HierarchyGuide& g : before.guides()) old[g.id] = &g;
    double moved = 0.0;
    std::size_t compared = 0;
    double lengthBefore = 0.0, lengthAfter = 0.0;
    for (const HierarchyGuide& g : after.guides()) {
        const auto it = old.find(g.id);
        if (it == old.end()) {
            ++c.added;
            continue;
        }
        const HierarchyGuide& o = *it->second;
        old.erase(it);
        const std::size_t n = std::min(o.points.size(), g.points.size());
        float most = 0.0f;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const float d = (g.points[i] - o.points[i]).length();
            most = std::max(most, d);
            sum += d;
        }
        if (most <= 1e-6f && o.points.size() == g.points.size()) continue;
        ++c.touched;
        moved += sum;
        compared += n;
        c.maxMove = std::max(c.maxMove, most);
        lengthBefore += polylineLength(o.points);
        lengthAfter += polylineLength(g.points);
    }
    c.removed = old.size();
    if (compared > 0) c.meanMove = static_cast<float>(moved / static_cast<double>(compared));
    if (c.touched > 0) {
        c.lengthBefore = static_cast<float>(lengthBefore / static_cast<double>(c.touched));
        c.lengthAfter = static_cast<float>(lengthAfter / static_cast<double>(c.touched));
    }
    return c;
}

GuideStats measureGuides(const GuideHierarchy& h, const std::vector<GuideId>& ids, const ScalpMesh* scalp) {
    GuideStats s;
    double lengthSum = 0.0, straightSum = 0.0, curlSum = 0.0, rootAngleSum = 0.0;
    std::size_t rootAngles = 0;
    Vec3f dirSum(0, 0, 0), rootSum(0, 0, 0), tipSum(0, 0, 0);
    for (const GuideId id : ids) {
        const HierarchyGuide* g = h.guide(id);
        if (!g || g->points.size() < 2) continue;
        const std::vector<Vec3f>& p = g->points;
        const float length = polylineLength(p);
        if (s.count == 0) {
            s.lengthMin = s.lengthMax = length;
        } else {
            s.lengthMin = std::min(s.lengthMin, length);
            s.lengthMax = std::max(s.lengthMax, length);
        }
        ++s.count;
        lengthSum += length;
        const Vec3f chord = p.back() - p.front();
        if (chord.length() > 1e-9f) dirSum += chord.normalized();
        straightSum += length > 1e-9f ? chord.length() / length : 1.0f;
        float turned = 0.0f;
        for (std::size_t i = 2; i < p.size(); ++i) {
            const Vec3f a = p[i - 1] - p[i - 2], b = p[i] - p[i - 1];
            if (a.length() < 1e-9f || b.length() < 1e-9f) continue;
            turned += std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f));
        }
        curlSum += turned * 180.0f / kPi;
        if (scalp && scalp->triangleCount() > 0) {
            const Vec3f first = p[1] - p[0];
            if (first.length() > 1e-9f) {
                const Vec3f n = scalp->closestPoint(p.front()).normal;
                rootAngleSum += std::acos(std::clamp(first.normalized().dot(n), -1.0f, 1.0f)) * 180.0f / kPi;
                ++rootAngles;
            }
        }
        rootSum += p.front();
        tipSum += p.back();
    }
    if (s.count == 0) return s;
    const float k = 1.0f / static_cast<float>(s.count);
    s.lengthMean = static_cast<float>(lengthSum) * k;
    s.straightness = static_cast<float>(straightSum) * k;
    s.curlDegrees = static_cast<float>(curlSum) * k;
    if (rootAngles > 0) s.rootAngleDegrees = static_cast<float>(rootAngleSum / static_cast<double>(rootAngles));
    s.direction = dirSum.length() > 1e-6f ? dirSum.normalized() : Vec3f(0, 0, 0);
    s.rootMiddle = rootSum * k;
    s.tipMiddle = tipSum * k;
    return s;
}

std::string directionWords(const Vec3f& d, const ScalpFrame& f) {
    if (d.length() < 1e-6f) return "no one way";
    const Vec3f u = d.normalized();
    std::string out;
    auto add = [&](float v, const char* plus, const char* minus) {
        if (std::abs(v) < 0.34f) return;
        out += (out.empty() ? "" : ", ") + std::string(v > 0.0f ? plus : minus);
    };
    // The largest part first.
    struct Part {
        float v;
        const char *plus, *minus;
    };
    Part parts[3] = {{u.dot(f.up), "up", "down"}, {u.dot(f.front), "forward", "back"}, {u.dot(f.left), "to the left", "to the right"}};
    std::sort(std::begin(parts), std::end(parts), [](const Part& a, const Part& b) { return std::abs(a.v) > std::abs(b.v); });
    for (const Part& p : parts) add(p.v, p.plus, p.minus);
    return out.empty() ? "no one way" : out;
}

}  // namespace groomlab::agent
