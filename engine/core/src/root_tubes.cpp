// SPDX-License-Identifier: Apache-2.0
// Root-first tubes and the tube hierarchy (root_tubes.h): a port of the Maya Tube tool's Python
// (root_tubes.py) to the core, so every host draws, edits and splits root tubes alike; the section
// fitting is tube_mesh.h's, shared with the modeled tubes' conversions. The math runs in doubles,
// as the Python did.

#include "groomlab/root_tubes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "groomlab/frames.h"
#include "groomlab/tube.h"
#include "groomlab/tube_mesh.h"

namespace groomlab {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxLean = 50.0 * kPi / 180.0;  // a root tube's guide leaves the scalp leaning at most this far off its normal
constexpr double kPathLeave = 0.2;               // the least share of the path over which it bends from the normal into the stroke
constexpr double kPathSmoothing = 0.25;          // a drawn path averaged over this share of its root's size (a Gaussian's sigma)
constexpr double kSpanRoots = 1.0;               // a drawn guide's spans about this many root sizes long ...
constexpr double kSpanTurn = 15.0 * kPi / 180.0;  // ... turning at most this much each ...
constexpr int kSpansMin = 3, kSpansMax = 200;    // ... between these many
constexpr double kClearance = 0.35;              // a drawn guide popped this many root sizes out where it dips into the scalp ...
constexpr double kClearRamp = 2.0;               // ... reached this many root sizes along it from the root ...
constexpr double kClearSpread = 1.5;             // ... the lift spread smoothly over this many root sizes along it
constexpr int kRecenterPasses = 4;               // the guide moved onto its root shape's box middle at most so often

// ---- Doubles -------------------------------------------------------------------------------------------

struct V {
    double x = 0, y = 0, z = 0;
    V() = default;
    V(double a, double b, double c) : x(a), y(b), z(c) {}
    explicit V(const Vec3f& p) : x(p.x), y(p.y), z(p.z) {}
    Vec3f f() const { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }
    V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const V& o) const { return x * o.x + y * o.y + z * o.z; }
    V cross(const V& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(dot(*this)); }
};
using Pts = std::vector<V>;

V unit(const V& v) {
    const double l = v.length();
    return l > 1e-12 ? v * (1.0 / l) : v;
}
double dist(const V& a, const V& b) { return (a - b).length(); }
V lerp(const V& a, const V& b, double t) { return a + (b - a) * t; }
V mean(const Pts& pts) {
    V m;
    for (const V& p : pts) m = m + p;
    return pts.empty() ? m : m * (1.0 / static_cast<double>(pts.size()));
}
double polylineLength(const Pts& pts) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) total += dist(pts[i - 1], pts[i]);
    return total;
}
Pts toV(const std::vector<Vec3f>& pts) {
    Pts out;
    out.reserve(pts.size());
    for (const Vec3f& p : pts) out.emplace_back(p);
    return out;
}
std::vector<Vec3f> toF(const Pts& pts) {
    std::vector<Vec3f> out;
    out.reserve(pts.size());
    for (const V& p : pts) out.push_back(p.f());
    return out;
}
V sideAxis(const V& n) { return std::abs(n.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0); }

using P2 = Vec2d;
using Section = SectionFit;
P2 toPlane(const V& p, const V& c, const V& u, const V& v) {
    const V d = p - c;
    return {d.dot(u), d.dot(v)};
}
V fromPlane(const P2& q, const V& c, const V& u, const V& v) { return c + u * q.x + v * q.y; }

V newellNormal(const Pts& pts) {
    V n;
    const std::size_t count = pts.size();
    for (std::size_t i = 0; i < count; ++i) {
        const V& a = pts[i];
        const V& b = pts[(i + 1) % count];
        n = n + V((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

V centroidOf(const Pts& pts) {
    const std::size_t n = pts.size();
    const V m = mean(pts);
    const V normal = newellNormal(pts);
    if (normal.length() < 1e-12) return m;
    const V nz = unit(normal);
    const V u = unit(nz.cross(sideAxis(nz)));
    const V v = nz.cross(u);
    std::vector<P2> flat;
    for (const V& p : pts) flat.push_back(toPlane(p, m, u, v));
    double area = 0.0, cx = 0.0, cy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const P2 a = flat[i], b = flat[(i + 1) % n];
        const double c = a.x * b.y - b.x * a.y;
        area += c;
        cx += (a.x + b.x) * c;
        cy += (a.y + b.y) * c;
    }
    if (std::abs(area) < 1e-18) return m;
    return fromPlane({cx / (3.0 * area), cy / (3.0 * area)}, m, u, v);
}

Pts resample(const Pts& pts, std::size_t count) {
    if (pts.size() < 2) return pts;
    std::vector<double> along{0.0};
    for (std::size_t i = 1; i < pts.size(); ++i) along.push_back(along.back() + dist(pts[i - 1], pts[i]));
    const double total = along.back();
    if (total <= 1e-12) return Pts(count, pts.front());
    Pts out;
    std::size_t k = 0;
    for (std::size_t j = 0; j < count; ++j) {
        const double s = total * static_cast<double>(j) / static_cast<double>(count - 1);
        while (k < pts.size() - 2 && along[k + 1] < s) ++k;
        const double span = along[k + 1] - along[k];
        const double f = span > 1e-12 ? (s - along[k]) / span : 0.0;
        out.push_back(lerp(pts[k], pts[k + 1], std::clamp(f, 0.0, 1.0)));
    }
    return out;
}

double turning(const Pts& pts) {
    double out = 0.0;
    for (std::size_t i = 2; i < pts.size(); ++i) {
        const V u = pts[i - 1] - pts[i - 2], v = pts[i] - pts[i - 1];
        const double lu = u.length(), lv = v.length();
        if (lu > 1e-12 && lv > 1e-12) out += std::acos(std::clamp(u.dot(v) / (lu * lv), -1.0, 1.0));
    }
    return out;
}

double lean(const V& direction, const V& normal) {
    return std::acos(std::clamp(unit(direction).dot(unit(normal)), -1.0, 1.0));
}

V leaned(const V& direction, const V& normal, double most = kMaxLean) {
    const V n = unit(normal), d = unit(direction);
    if (lean(d, n) <= most) return d;
    V side = d - n * d.dot(n);
    side = side.length() > 1e-9 ? unit(side) : unit(n.cross(sideAxis(n)));
    return n * std::cos(most) + side * std::sin(most);
}

Pts seat(Pts pts, const V& normal, double most = kMaxLean) {
    if (pts.size() < 2) return pts;
    const V first = pts[1] - pts[0];
    pts[1] = pts[0] + leaned(first, normal, most) * first.length();
    return pts;
}

// The straight line the last `m` of `pts` lie along (least squares by index): its point at the
// last index and its step per index.
std::pair<V, V> endLine(const Pts& pts, int m) {
    m = std::max(2, std::min(m, static_cast<int>(pts.size())));
    const std::size_t from = pts.size() - static_cast<std::size_t>(m);
    const double meanI = (m - 1) / 2.0;
    V meanP;
    for (int i = 0; i < m; ++i) meanP = meanP + pts[from + static_cast<std::size_t>(i)];
    meanP = meanP * (1.0 / m);
    double den = 0.0;
    V step;
    for (int i = 0; i < m; ++i) {
        den += (i - meanI) * (i - meanI);
        step = step + (pts[from + static_cast<std::size_t>(i)] - meanP) * (i - meanI);
    }
    step = step * (1.0 / den);
    return {meanP + step * (m - 1 - meanI), step};
}

// `pts` (evenly spaced) averaged with their neighbours, a Gaussian of `sigma` samples, carried on
// past each end along the line its last stretch lies along; the root's shift faded out.
Pts gaussian(const Pts& pts, double sigma) {
    const int n = static_cast<int>(pts.size());
    const int reach = static_cast<int>(3.0 * sigma) + 1;
    std::vector<double> weights;
    for (int k = -reach; k <= reach; ++k) weights.push_back(std::exp(-0.5 * (k / sigma) * (k / sigma)));
    const int m = static_cast<int>(2.0 * sigma) + 2;
    const auto [end, endStep] = endLine(pts, m);
    Pts reversed(pts.rbegin(), pts.rend());
    const auto [start, startStep] = endLine(reversed, m);
    auto at = [&](int i) {
        if (i < 0) return start + startStep * static_cast<double>(-i);
        if (i >= n) return end + endStep * static_cast<double>(i - (n - 1));
        return pts[static_cast<std::size_t>(i)];
    };
    Pts out;
    for (int i = 0; i < n; ++i) {
        V acc;
        double total = 0.0;
        for (int k = -reach; k <= reach; ++k) {
            const double w = weights[static_cast<std::size_t>(k + reach)];
            acc = acc + at(i + k) * w;
            total += w;
        }
        out.push_back(acc * (1.0 / total));
    }
    const V shift = pts[0] - out[0];
    for (int i = 0; i < std::min(reach, n); ++i) {
        const double w = 0.5 * (1.0 + std::cos(kPi * i / static_cast<double>(reach)));
        out[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(i)] + shift * w;
    }
    return out;
}

Pts smoothPath(const Pts& path, const V& root, const V& normal, double size) {
    // A stroke started off the root's middle eased onto it: moved by its start's offset at the root,
    // less and less (a smoothstep) to none halfway along it, so the curve keeps its own way and the
    // guide still leaves the middle of its root (before, only its first point moved: a kink there).
    Pts drawn = path;
    {
        const V offset = root - path[0];
        std::vector<double> along(path.size(), 0.0);
        for (std::size_t i = 1; i < path.size(); ++i) along[i] = along[i - 1] + dist(path[i - 1], path[i]);
        const double fade = 0.5 * along.back();
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            const double x = fade > 0.0 ? std::min(along[i] / fade, 1.0) : (i == 0 ? 0.0 : 1.0);
            const double k = 1.0 - x * x * (3.0 - 2.0 * x);
            drawn[i] = path[i] + offset * k;
        }
        drawn[0] = root;
    }
    if (polylineLength(drawn) <= 1e-9) throw std::invalid_argument("the tube needs a length");
    double sigma = std::max(kPathSmoothing * size, 1e-6);
    // Thinned first: a point nearer the last one kept than half the smoothing dropped.
    Pts pts{drawn[0]};
    for (std::size_t i = 1; i < drawn.size(); ++i) {
        if (dist(drawn[i], pts.back()) >= 0.5 * sigma) pts.push_back(drawn[i]);
    }
    if (dist(drawn.back(), pts.back()) > 1e-12) {
        if (pts.size() > 1) {
            pts.back() = drawn.back();
        } else {
            pts.push_back(drawn.back());
        }
    }
    if (pts.size() < 2) pts = {drawn.front(), drawn.back()};
    double total = polylineLength(pts);
    sigma = std::min(sigma, 0.1 * total);
    const int samples = static_cast<int>(std::min(std::max(4.0 * total / sigma, 50.0), 4000.0));
    pts = resample(pts, static_cast<std::size_t>(samples));
    pts = gaussian(pts, sigma / (total / static_cast<double>(samples - 1)));
    // From the root the guide goes the stroke's way; only a stroke leaving the root leaning more
    // than kMaxLean starts at kMaxLean and bends into the stroke (a quadratic Bezier).
    total = polylineLength(pts);
    const std::size_t probeAt = std::min(pts.size() - 1, std::max<std::size_t>(2, static_cast<std::size_t>(0.05 * pts.size())));
    const V first = pts[probeAt] - pts[0];
    if (lean(first, normal) <= kMaxLean + 1e-6) return pts;
    const V startWay = leaned(first, normal);
    auto corner = [&](std::size_t i, V* out) {
        const V q = pts[i];
        const V way = unit(pts[std::min(i + 2, pts.size() - 1)] - pts[i >= 2 ? i - 2 : 0]);
        const V w0 = pts[0] - q;
        const double b = startWay.dot(way);
        const double den = 1.0 - b * b;
        if (den < 1e-6) {
            *out = lerp(pts[0], q, 0.5);
            return true;
        }
        const double d = startWay.dot(w0), e = way.dot(w0);
        const double t = (b * e - d) / den;
        const double u = (e - b * d) / den;
        if (t <= 0.0 || u >= 0.0) return false;
        *out = lerp(pts[0] + startWay * t, q + way * u, 0.5);
        return true;
    };
    const double least = std::max(kPathLeave * total, 3.0 * size);
    double along = 0.0;
    std::size_t k = 0;
    V mid;
    bool found = false;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        along += dist(pts[i - 1], pts[i]);
        if (along < least && i < pts.size() - 1) continue;
        if (corner(i, &mid)) {
            k = i;
            found = true;
            break;
        }
    }
    if (!found) {
        k = pts.size() - 1;
        mid = pts[0] + startWay * (0.5 * dist(pts[0], pts[k]));
    }
    const V q = pts[k];
    Pts curve;
    for (int i = 0; i < 96; ++i) {
        const double t = i / 96.0;
        curve.push_back(pts[0] * ((1 - t) * (1 - t)) + mid * (2 * t * (1 - t)) + q * (t * t));
    }
    curve.insert(curve.end(), pts.begin() + static_cast<std::ptrdiff_t>(k), pts.end());
    return curve;
}

// `count` points along a polyline, its ends kept, each the same straight distance from the last.
Pts evenChords(const Pts& pts, int count) {
    if (count < 2 || pts.size() < 2) return Pts(static_cast<std::size_t>(std::max(count, 1)), pts.empty() ? V() : pts.front());
    auto walk = [&](double step, Pts* out) {
        out->assign(1, pts[0]);
        std::size_t j = 1;
        for (int c = 0; c < count - 1; ++c) {
            const V here = out->back();
            while (j < pts.size() && dist(pts[j], here) < step) ++j;
            if (j >= pts.size()) return false;
            const V a = pts[j - 1], b = pts[j];
            double lo = 0.0, hi = 1.0;
            for (int it = 0; it < 40; ++it) {
                const double m = 0.5 * (lo + hi);
                if (dist(lerp(a, b, m), here) < step) {
                    lo = m;
                } else {
                    hi = m;
                }
            }
            out->push_back(lerp(a, b, hi));
        }
        return true;
    };
    double lo = 0.0, hi = polylineLength(pts) / static_cast<double>(count - 1);
    Pts best, out;
    bool any = false;
    for (int it = 0; it < 50; ++it) {
        const double step = 0.5 * (lo + hi);
        if (walk(step, &out)) {
            best = out;
            any = true;
            lo = step;
        } else {
            hi = step;
        }
    }
    if (!any) return resample(pts, static_cast<std::size_t>(count));
    best.back() = pts.back();
    return best;
}

int pathSpans(const Pts& points, double size, double density) {
    density = std::max(density, 1e-3);
    const double total = polylineLength(points);
    const double along = total / std::max(kSpanRoots * size / density, 1e-9);
    const std::size_t coarseCount =
        static_cast<std::size_t>(std::min(std::max(total / (0.5 * std::max(size, 1e-9)), 2.0), 2000.0)) + 1;
    const double turns = turning(resample(points, coarseCount)) / (kSpanTurn / density);
    return static_cast<int>(std::min(std::max(std::ceil(std::max(along, turns)), static_cast<double>(kSpansMin)),
                                     static_cast<double>(kSpansMax)));
}

struct Surface {
    const ScalpMesh* scalp = nullptr;
    std::pair<V, V> operator()(const V& p) const {
        const SurfacePoint sp = scalp->closestPoint(p.f());
        if (sp.root.face < 0) return {p, V(0, 1, 0)};
        return {V(sp.position), unit(V(scalp->normal(sp.root)))};
    }
};

// A drawn root's middle on the scalp: the apex of the patch it outlines. The patch is the side of
// the drawn loop's plane (Newell's normal, through its centroid) holding less of the scalp (the
// convex dome the loop goes round: a small patch's bump, or the top of a head the loop circles
// about the ears and neck); its apex the scalp's outermost point that way through the centroid, and
// that way its normal (the root's guide's default way out). Not the scalp point nearest the
// centroid, nor the CVs' mean normal: round a whole head those cancel, or tip it down the neck.
std::pair<V, V> patchApex(const ScalpMesh& scalp, const Pts& outline) {
    const Surface surface{&scalp};
    const V c = centroidOf(outline);
    V n(0, 0, 0);
    double reach = 0.0;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        n = n + outline[i].cross(outline[(i + 1) % outline.size()]);
        reach = std::max(reach, dist(c, outline[i]));
    }
    const double len = n.length();
    if (!(len > 1e-12) || !(reach > 0.0) || scalp.triangleCount() == 0) return surface(c);
    n = n * (1.0 / len);
    // The side holding less of the scalp's area (each triangle by its middle).
    double side[2] = {0.0, 0.0};
    const std::vector<Vec3f>& pts = scalp.points();
    // (A spread of at most about 4000 triangles: which side holds less, not how much.)
    const std::vector<ScalpMesh::Triangle>& tris = scalp.triangles();
    const std::size_t step = std::max<std::size_t>(1, tris.size() / 4000);
    for (std::size_t k = 0; k < tris.size(); k += step) {
        const ScalpMesh::Triangle& t = tris[k];
        const V a(pts[static_cast<std::size_t>(t[0])]), b(pts[static_cast<std::size_t>(t[1])]), d(pts[static_cast<std::size_t>(t[2])]);
        const double area = 0.5 * (b - a).cross(d - a).length();
        side[((a + b + d) * (1.0 / 3.0) - c).dot(n) > 0.0 ? 0 : 1] += area;
    }
    if (side[1] < side[0]) n = n * -1.0;
    // Its outermost point that way, through the centroid (the first hit coming in from outside).
    double extent = reach;
    for (std::size_t k = 0; k < pts.size(); k += std::max<std::size_t>(1, pts.size() / 4000)) extent = std::max(extent, dist(c, V(pts[k])));
    extent *= 1.5;  // (sampled: room to spare)
    SurfacePoint hit;
    float d = 0.0f;
    const V from = c + n * (2.0 * extent + 1e-3);
    if (!scalp.intersectRay(from.f(), (n * -1.0).f(), &hit, &d) || hit.root.face < 0) return {surface(c).first, n};
    return {V(hit.position), n};
}

Pts clearOfScalp(Pts pts, const Surface* surface, double size) {
    if (!surface || pts.size() < 3 || size <= 0.0) return pts;
    std::vector<double> along{0.0};
    for (std::size_t i = 1; i < pts.size(); ++i) along.push_back(along.back() + dist(pts[i - 1], pts[i]));
    const double step = along.back() > 0.0 ? along.back() / static_cast<double>(pts.size() - 1) : 1.0;
    const double clearance = kClearance * size;
    std::vector<double> want;
    for (const double d : along) {
        const double x = std::min(d / (kClearRamp * size), 1.0);
        want.push_back(clearance * x * x * (3.0 - 2.0 * x));
    }
    const double sigma = std::max(kClearSpread * size / step, 1.0);
    const int reach = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> kernel;
    for (int k = -reach; k <= reach; ++k) kernel.push_back(std::exp(-0.5 * (k / sigma) * (k / sigma)));
    const double peak = *std::max_element(kernel.begin(), kernel.end());
    for (int pass = 0; pass < 6; ++pass) {
        Pts lifts(pts.size());
        bool any = false;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto [q, n] = (*surface)(pts[i]);
            const double shortBy = want[i] - (pts[i] - q).dot(n);
            if (shortBy > 0.02 * clearance && i > 0) {
                lifts[i] = n * shortBy;
                any = true;
            }
        }
        if (!any) break;
        Pts spread(pts.size());
        for (std::size_t i = 0; i < pts.size(); ++i) {
            V total;
            for (std::size_t k = 0; k < kernel.size(); ++k) {
                const long j = static_cast<long>(i) + static_cast<long>(k) - reach;
                if (j >= 0 && j < static_cast<long>(pts.size())) total = total + lifts[static_cast<std::size_t>(j)] * (kernel[k] / peak);
            }
            spread[i] = total;
        }
        double top = 0.0, need = 0.0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            top = std::max(top, spread[i].length());
            need = std::max(need, lifts[i].length());
        }
        const double scale = top > 0.0 ? std::min(1.0, need / top) : 0.0;
        spread[0] = V();
        for (std::size_t i = 0; i < pts.size(); ++i) pts[i] = pts[i] + spread[i] * scale;
    }
    return pts;
}

Pts guidePath(const V& root, const V& normal, const Pts& path, double length, int segments, double size, double density,
              const Surface* surface) {
    Pts pts;
    if (path.size() >= 2) {
        size = size > 0.0 ? size : 0.05 * polylineLength(path);
        const Pts smooth = smoothPath(path, root, normal, size);
        pts = evenChords(smooth, pathSpans(smooth, size, density) + 1);
        if (surface) pts = evenChords(clearOfScalp(pts, surface, size), static_cast<int>(pts.size()));
    } else {
        segments = std::max(segments, 2);
        if (length <= 1e-9) throw std::invalid_argument("the tube needs a length");
        for (int i = 0; i <= segments; ++i) pts.push_back(root + normal * (length * i / static_cast<double>(segments)));
    }
    return seat(pts, normal);
}

// The least rotation taking unit `a` onto unit `b`, applied to `vec` (frames.h's Rotation).
V turn(const V& vec, const V& a0, const V& b0) {
    const V a = unit(a0), b = unit(b0);
    const double c = a.dot(b);
    if (c < -1.0 + 1e-6) return vec * -1.0;
    const V k = a.cross(b);
    return vec * c + k.cross(vec) + k * (k.dot(vec) / (1.0 + c));
}

// ---- Section fitting (signedArea, shapePoints, fitSection, placeOutline: tube_mesh.h) -------------------

bool solve3(double m[3][3], const double r[3], double out[3]) {
    auto det = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det(m);
    if (std::abs(d) < 1e-18) return false;
    for (int c = 0; c < 3; ++c) {
        double a[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) a[i][j] = m[i][j];
        for (int k = 0; k < 3; ++k) a[k][c] = r[k];
        out[c] = det(a) / d;
    }
    return true;
}

// How far an affine map can take `src` points onto `dst`: the least squares' summed squared residual.
double affineError(const std::vector<P2>& src, const std::vector<P2>& dst) {
    double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const P2& p : src) {
        const double row[3] = {p.x, p.y, 1.0};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) m[i][j] += row[i] * row[j];
    }
    double err = 0.0;
    for (int axis = 0; axis < 2; ++axis) {
        double r[3] = {0, 0, 0};
        for (std::size_t k = 0; k < src.size(); ++k) {
            const double row[3] = {src[k].x, src[k].y, 1.0};
            const double q = axis == 0 ? dst[k].x : dst[k].y;
            for (int i = 0; i < 3; ++i) r[i] += row[i] * q;
        }
        double coef[3];
        if (!solve3(m, r, coef)) return std::numeric_limits<double>::infinity();
        for (std::size_t k = 0; k < src.size(); ++k) {
            const double q = axis == 0 ? dst[k].x : dst[k].y;
            const double e = coef[0] * src[k].x + coef[1] * src[k].y + coef[2] - q;
            err += e * e;
        }
    }
    return err;
}

// The library's points (`stored`, fitted into its box and maybe turned and reordered) matched to the
// `given` points they came from: the cyclic orders an affine map takes onto them as well as the best.
std::vector<std::vector<P2>> candidates(const std::vector<P2>& given, const std::vector<P2>& stored) {
    const std::size_t n = given.size();
    if (stored.size() != n || n == 0) return {};
    std::vector<std::pair<double, std::vector<P2>>> scored;
    for (std::size_t shift = 0; shift < n; ++shift) {
        std::vector<P2> cand(stored.begin() + static_cast<std::ptrdiff_t>(shift), stored.end());
        cand.insert(cand.end(), stored.begin(), stored.begin() + static_cast<std::ptrdiff_t>(shift));
        scored.emplace_back(affineError(cand, given), std::move(cand));
    }
    double least = std::numeric_limits<double>::infinity();
    for (const auto& [e, c] : scored) least = std::min(least, e);
    double size = 0.0;
    for (const P2& p : given) size += p.x * p.x + p.y * p.y;
    if (size == 0.0) size = 1.0;
    std::vector<std::vector<P2>> out;
    for (auto& [e, c] : scored) {
        if (e <= least + 1e-9 * size) out.push_back(std::move(c));
    }
    return out;
}

bool bestSection(const std::vector<P2>& pts, const std::vector<P2>& stored, Section* out) {
    double best = std::numeric_limits<double>::infinity();
    bool any = false;
    for (const std::vector<P2>& fitted : candidates(pts, stored)) {
        const Section s = fitSection(pts, fitted);
        const double off = std::hypot(s.center.x, s.center.y);
        if (!any || off < best - 1e-12) {
            best = off;
            *out = s;
            any = true;
        }
    }
    return any;
}

std::vector<P2> libraryPoints(const GuideHierarchy& h, const std::string& name) {
    std::vector<P2> out;
    const CustomTubeShapePtr shape = h.tubeShape(name);
    if (!shape) return out;
    for (const SectionPoint& p : shape->points) out.push_back({p.u, p.v});
    return out;
}

// ---- A guide's footprint ---------------------------------------------------------------------------------

struct Footprint {
    V center, u, v, n;
};

// The guide's root footprint as the engine lays it: a primary rooted on the scalp, fixed there
// (TubeSpine::setRootOnScalp: across along ScalpMesh::acrossAt, v = n x across); else its root
// section turned flat onto the scalp.
Footprint footprintOf(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id) {
    const HierarchyGuide* g = h.guide(id);
    if (!g || g->points.empty()) throw std::invalid_argument("no guide " + std::to_string(id));
    const Vec3f at = g->points.front();
    if (!g->isChild() && scalp.contains(g->root) && (scalp.position(g->root) - at).length() <= 1e-3f * std::max(1.0f, at.length())) {
        const Vec3f nrm = scalp.normal(g->root);
        const Vec3f across = h.rootAcrossOf(*g, scalp);  // (a cell of a cut: turned as its source)
        if (across.length() > 0.5f) return {V(at), V(across), V(nrm.cross(across)), V(nrm)};
    }
    // Its tube's first section frame, turned onto the scalp's normal there.
    const std::size_t n = g->points.size();
    V u(1, 0, 0), v(0, 0, 1);
    if (n >= 2) {
        const std::vector<Frame> frames = rotationMinimizingFrames(g->points.data(), n, g->frameUp);
        const float roll = g->tube.rollAt(0.0f);
        const float c = pm::cos(roll), s = pm::sin(roll);
        u = unit(V(frames[0].normal * c + frames[0].binormal * s));
        v = V(frames[0].normal * -s + frames[0].binormal * c);
        v = unit(v - u * v.dot(u));
    }
    const V way = unit(u.cross(v));
    const auto [q, nrm] = Surface{&scalp}(V(at));
    (void)q;
    if (way.dot(nrm) < -1.0 + 1e-4) return {V(at), u, v, way};
    return {V(at), turn(u, way, nrm), turn(v, way, nrm), nrm};
}

// The outline in the guide's footprint, kRootTubeOverlap larger about it.
std::vector<P2> rootFit(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const Pts& outline, Footprint* fp) {
    *fp = footprintOf(h, scalp, id);
    const double k = 1.0 + kRootTubeOverlap;
    std::vector<P2> flat;
    for (const V& p : outline) {
        const P2 q = toPlane(p, fp->center, fp->u, fp->v);
        flat.push_back({q.x * k, q.y * k});
    }
    return flat;
}

std::vector<SectionPoint> toSection(const std::vector<P2>& pts) {
    std::vector<SectionPoint> out;
    for (const P2& p : pts) out.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
    return out;
}

// The tube's root section on `outline`: library shape `shape` set to it (fitted in the guide's root
// frame), the section that puts the library's points there.
Section fitShape(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const std::string& shape, const Pts& outline) {
    Footprint fp;
    const std::vector<P2> pts = shapePoints(rootFit(h, scalp, id, outline, &fp));
    h.setOwnedTubeShape(shape, toSection(pts), false);
    Section s;
    if (!bestSection(pts, libraryPoints(h, shape), &s)) {
        throw std::runtime_error("the root outline's shape " + shape + " does not match its CVs");
    }
    return s;
}

double clampAspect(double a) { return std::min(std::max(a, 0.01), 100.0); }
double orOne(double v, double fallback) { return v != 0.0 ? v : fallback; }

void reshape(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const Pts& points) {
    h.setPoints(id, toF(points));
    h.evaluate(&scalp);
}

void setTube(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const TubeProfile& tube) {
    h.setTube(id, tube);
    h.evaluate(&scalp);
}

// The root section fitted again on the guide as it now lies, the whole tube turned and scaled as
// much (`passes` times, or until it holds).
void settle(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const std::string& shape, const Pts& outline, int passes = 2) {
    for (int pass = 0; pass < passes; ++pass) {
        Footprint fp;
        const std::vector<P2> pts = shapePoints(rootFit(h, scalp, id, outline, &fp));
        Section s;
        if (!bestSection(pts, libraryPoints(h, shape), &s)) return;
        TubeProfile tube = h.guide(id)->tube;
        const double w0 = orOne(tube.width.empty() ? 0.0 : tube.width[0], 1e-9);
        const double t0 = tube.twist.empty() ? 0.0 : tube.twist[0];
        const double a0 = orOne(tube.aspect.empty() ? 0.0 : tube.aspect[0], 1.0);
        const double aNew = s.width > 1e-12 ? clampAspect(s.height / s.width) : a0;
        double twist = s.twist + 2.0 * kPi * std::round((t0 - s.twist) / (2.0 * kPi));
        if (std::abs(twist - t0) < 1e-5 && std::abs(s.width / w0 - 1.0) < 1e-5 && std::abs(aNew / a0 - 1.0) < 1e-5) return;
        for (float& x : tube.width) x = static_cast<float>(x * s.width / w0);
        for (float& a : tube.aspect) a = static_cast<float>(clampAspect(a * aNew / a0));
        for (float& t : tube.twist) t = static_cast<float>(t + twist - t0);
        setTube(h, scalp, id, tube);
    }
}

Pts projectAll(const ScalpMesh& scalp, const Pts& pts) {
    Pts out;
    for (const V& p : pts) out.push_back(Surface{&scalp}(p).first);
    return out;
}

// Points taken onto the scalp along `direction` (the nearest hit on the line through each, either
// way), else to the nearest point (a hit far past it: the line grazes the scalp).
Pts projectAlong(const ScalpMesh& scalp, const Pts& pts, const V& direction) {
    const Vec3f d = unit(direction).f();
    Pts out;
    for (const V& p : pts) {
        double best = std::numeric_limits<double>::infinity();
        V hitAt;
        for (const Vec3f& way : {d, d * -1.0f}) {
            SurfacePoint hit;
            float t = 0.0f;
            if (scalp.intersectRay(p.f(), way, &hit, &t) && std::abs(t) < best) {
                best = std::abs(t);
                hitAt = V(hit.position);
            }
        }
        const V near = Surface{&scalp}(p).first;
        out.push_back(std::isinf(best) || best > 4.0 * dist(p, near) + 1e-6 ? near : hitAt);
    }
    return out;
}

std::vector<StrandRoot> bindOutline(const ScalpMesh& scalp, const Pts& pts) {
    std::vector<StrandRoot> out;
    for (const V& p : pts) {
        const SurfacePoint sp = scalp.closestPoint(p.f());
        if (sp.root.face < 0) throw std::invalid_argument("the groom has no scalp to keep a root outline on");
        out.push_back(sp.root);
    }
    return out;
}

std::string newShapeName(const GuideHierarchy& h) {
    for (int n = 1;; ++n) {
        const std::string name = std::string(kOwnedTubeShapePrefix) + std::to_string(n);
        if (!h.tubeShape(name)) return name;
    }
}

GroupId rootGroup(GuideHierarchy& h, const std::string& name) {
    // The group picked when it is a top group (every host's Tube tool), else kRootTubeGroup: a
    // root tube starts a hierarchy of its own, never inside another's levels.
    GroupId found = name.empty() ? kNoGroup : h.findGroup(name);
    if (found != kNoGroup && h.groupDepth(found) == 0) return found;
    const std::string wanted = found != kNoGroup || name.empty() ? std::string(kRootTubeGroup) : name;
    found = h.findGroup(wanted);
    if (found != kNoGroup) return found;
    return h.addGroup(wanted);
}

Pts checkedOutline(const std::vector<Vec3f>& outline) {
    if (outline.size() < 3) throw std::invalid_argument("a root outline needs at least 3 CVs");
    if (outline.size() > kMaxTubeShapePoints) {
        throw std::invalid_argument("a root outline takes at most " + std::to_string(kMaxTubeShapePoints) + " CVs");
    }
    return toV(outline);
}

bool isRootTubeGuide(const HierarchyGuide& g) {
    // A drawn root, or a cell of a subdivided one (a child in the level below, its CVs its own: it
    // is picked, edited and cut again as a root). A slice of the hierarchy (+) is not.
    return (!g.isChild() || g.rootOutline.size() >= 3) && g.tube.custom && g.tube.custom->name.rfind(kOwnedTubeShapePrefix, 0) == 0;
}

}  // namespace

// ---- Path math ---------------------------------------------------------------------------------------------

Vec3f outlineCentroid(const std::vector<Vec3f>& outline) { return centroidOf(toV(outline)).f(); }
std::vector<Vec3f> resamplePolyline(const std::vector<Vec3f>& points, std::size_t count) { return toF(resample(toV(points), count)); }
float polylineTurning(const std::vector<Vec3f>& points) { return static_cast<float>(turning(toV(points))); }
float leanOff(const Vec3f& direction, const Vec3f& normal) { return static_cast<float>(lean(V(direction), V(normal))); }
int rootPathSpans(const std::vector<Vec3f>& points, float size, float density) { return pathSpans(toV(points), size, density); }

std::vector<Vec3f> rootTubeGuidePath(const Vec3f& root, const Vec3f& normal, const std::vector<Vec3f>& path, float length,
                                     int segments, float size, float density, const ScalpMesh* scalp) {
    const Surface surface{scalp};
    return toF(guidePath(V(root), V(normal), toV(path), length, segments, size, density, scalp ? &surface : nullptr));
}

ScalpPointNormal rootOutlineCenter(const ScalpMesh& scalp, const std::vector<Vec3f>& outline) {
    if (outline.empty()) return {};
    const auto [q, n] = patchApex(scalp, toV(outline));
    return {q.f(), n.f()};
}

ScalpPointNormal scalpPointNormal(const ScalpMesh& scalp, const Vec3f& p) {
    const auto [q, n] = Surface{&scalp}(V(p));
    return {q.f(), n.f()};
}

// ---- Root tubes --------------------------------------------------------------------------------------------

GuideId makeRootTube(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<Vec3f>& given, const std::vector<Vec3f>& drawnPath,
                     const RootTubeOptions& o) {
    const Surface surface{&scalp};
    const Pts outline = projectAll(scalp, checkedOutline(given));
    auto [root, normal] = patchApex(scalp, outline);
    double radius = 0.0;
    for (const V& p : outline) radius = std::max(radius, dist(root, p));
    Pts path = toV(drawnPath);
    Pts points = guidePath(root, normal, path, o.length, o.segments, radius, o.density, &surface);
    const GuideId id = h.addGuide(rootGroup(h, o.group), toF(points));
    h.evaluate(&scalp);
    // A placeholder shaped tube first, so the guide's frame lies as it will, then the outline read in it.
    TubeProfile placeholder;
    placeholder.shape = TubeShape::Ribbon;
    placeholder.width = {static_cast<float>(radius), static_cast<float>(radius)};
    placeholder.aspect = {1.0f};
    placeholder.twist = {0.0f};
    placeholder.rollOnScalp = false;
    setTube(h, scalp, id, placeholder);
    const std::string shape = newShapeName(h);
    Section s = fitShape(h, scalp, id, shape, outline);
    // The shape fills its box: the guide moved onto the box's middle and fitted again, till it holds.
    for (int pass = 0; pass < kRecenterPasses; ++pass) {
        if (std::hypot(s.center.x, s.center.y) <= 1e-5 * std::max(s.width, 1e-9)) break;
        Footprint fp;
        rootFit(h, scalp, id, outline, &fp);
        const auto [movedRoot, movedNormal] = surface(fromPlane(s.center, fp.center, fp.u, fp.v));
        if (path.size() >= 2) {  // the drawn curve moved with its root
            const V delta = movedRoot - points[0];
            for (V& p : path) p = p + delta;
        }
        points = guidePath(movedRoot, movedNormal, path, o.length, o.segments, radius, o.density, &surface);
        reshape(h, scalp, id, points);
        s = fitShape(h, scalp, id, shape, outline);
    }
    TubeProfile tube = h.guide(id)->tube;
    tube.custom = h.tubeShape(shape);
    tube.width = {static_cast<float>(s.width), static_cast<float>(s.width * o.taper)};
    tube.aspect = {static_cast<float>(s.width > 1e-12 ? clampAspect(s.height / s.width) : 1.0)};
    tube.twist = {static_cast<float>(s.twist)};
    tube.rollOnScalp = false;
    setTube(h, scalp, id, tube);
    settle(h, scalp, id, shape, outline);
    h.setRootOutline(id, bindOutline(scalp, outline));
    return id;
}

void editRootTube(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const std::vector<Vec3f>& given) {
    const Surface surface{&scalp};
    Pts outline = checkedOutline(given);
    const HierarchyGuide* g = h.guide(id);
    if (!g || !isRootTubeGuide(*g)) throw std::invalid_argument("guide " + std::to_string(id) + " is not a root tube");
    TubeProfile tube = g->tube;
    const std::string shape = tube.custom->name;
    if (tube.rollOnScalp) {  // (a root tube made before it was swept with no roll)
        tube.rollOnScalp = false;
        setTube(h, scalp, id, tube);
    }
    outline = projectAll(scalp, outline);
    const Pts old = toV(h.guide(id)->points);
    auto [root, normal] = patchApex(scalp, outline);
    // (The guide keeps the lean it had: an edit moves its root, it never stands it up.)
    const double had = old.size() >= 2 ? std::max(kMaxLean, lean(old[1] - old[0], surface(old[0]).second)) : kMaxLean;
    V move = root - old[0];
    Pts moved{root};
    for (std::size_t i = 1; i < old.size(); ++i) moved.push_back(old[i] + move);
    moved = seat(moved, normal, had);
    reshape(h, scalp, id, moved);
    Section s = fitShape(h, scalp, id, shape, outline);
    for (int pass = 0; pass < kRecenterPasses; ++pass) {
        if (std::hypot(s.center.x, s.center.y) <= 1e-5 * std::max(s.width, 1e-9)) break;
        Footprint fp;
        rootFit(h, scalp, id, outline, &fp);
        const auto [root2, normal2] = surface(fromPlane(s.center, fp.center, fp.u, fp.v));
        move = root2 - moved[0];
        Pts next{root2};
        for (std::size_t i = 1; i < moved.size(); ++i) next.push_back(moved[i] + move);
        moved = seat(next, normal2, had);
        reshape(h, scalp, id, moved);
        s = fitShape(h, scalp, id, shape, outline);
    }
    TubeProfile now = h.guide(id)->tube;
    const double w0 = orOne(tube.width.empty() ? 0.0 : tube.width[0], 1e-9);
    const double a0 = orOne(tube.aspect.empty() ? 0.0 : tube.aspect[0], 1.0);
    const double t0 = tube.twist.empty() ? 0.0 : tube.twist[0];
    const double aNew = s.width > 1e-12 ? clampAspect(s.height / s.width) : 1.0;
    now.width = tube.width;
    now.aspect = tube.aspect;
    now.twist = tube.twist;
    for (float& x : now.width) x = static_cast<float>(x * s.width / w0);
    for (float& a : now.aspect) a = static_cast<float>(clampAspect(a * aNew / a0));
    for (float& t : now.twist) t = static_cast<float>(t + s.twist - t0);
    setTube(h, scalp, id, now);
    settle(h, scalp, id, shape, outline);
    h.setRootOutline(id, bindOutline(scalp, outline));
}

std::vector<GuideId> rootTubesOf(const GuideHierarchy& h, const std::vector<GuideId>& guides) {
    std::set<GuideId> roots;
    if (guides.empty()) {
        for (const HierarchyGuide& g : h.guides()) {
            if (isRootTubeGuide(g)) roots.insert(g.id);
        }
    } else {
        for (const GuideId id : guides) {
            const HierarchyGuide* g = h.guide(id);
            while (g && g->isChild() && !isRootTubeGuide(*g)) g = h.guide(g->parent);  // (up to a root, or a cell)
            if (g && isRootTubeGuide(*g)) roots.insert(g->id);
        }
    }
    return {roots.begin(), roots.end()};
}

std::vector<Vec3f> rootTubeSectionOutline(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, bool onScalp) {
    const HierarchyGuide* g = h.guide(id);
    if (!g) throw std::invalid_argument("no guide " + std::to_string(id));
    if (!g->tube.custom) throw std::invalid_argument("guide " + std::to_string(id) + " is not a root tube (its shape is " + g->tube.shapeName() + ")");
    const Footprint fp = footprintOf(h, scalp, id);
    const double w = g->tube.width.empty() ? 0.0 : g->tube.width[0];
    const double ht = w * (g->tube.aspect.empty() ? 1.0 : g->tube.aspect[0]);
    std::vector<P2> shape;
    for (const SectionPoint& p : g->tube.custom->points) shape.push_back({p.u, p.v});
    const std::vector<P2> flat = placeOutline(shape, g->tube.twist.empty() ? 0.0 : g->tube.twist[0], w, ht);
    Pts world;
    for (const P2& q : flat) world.push_back(fromPlane(q, fp.center, fp.u, fp.v));
    return toF(onScalp ? projectAlong(scalp, world, fp.n) : world);
}

std::vector<SliceFitSample> sliceFit(const GuideHierarchy& h, GuideId slice, int samples, GuideId within) {
    // Both tubes as they are drawn (display.cpp appendTubeSurface) and their hair grows: the
    // sections, joined from the root (the parent's footprint, the slice's root loop) by
    // (1 - t / join)^2.
    const HierarchyGuide* g = h.guide(slice);
    const HierarchyGuide* p = within != kNoGuide ? h.guide(within) : g && g->isChild() ? h.guide(g->parent) : nullptr;
    if (!g || !p || g->points.size() < 2 || p->points.size() < 2) throw std::invalid_argument("guide " + std::to_string(slice) + " is not a slice");
    TubeSpine own(g->points.data(), g->points.size(), g->frameUp, g->tube, h.scale());
    TubeSpine parent(p->points.data(), p->points.size(), p->frameUp, p->tube, h.scale());
    if (p->rootNormal.length() > 0.5f && p->rootAcross.length() > 0.5f) parent.setRootOnScalp(p->rootNormal, p->rootAcross);
    if (g->rootNormal.length() > 0.5f && g->rootAcross.length() > 0.5f) own.setRootOnScalp(g->rootNormal, g->rootAcross);
    const std::vector<SectionPoint>& outline = tubeOutline(g->tube);
    const bool ring = g->rootRing.size() == outline.size();
    const std::vector<SectionPoint>& pOutline = tubeOutline(p->tube);
    const bool pRing = p->rootRing.size() == pOutline.size() && pOutline.size() >= 3;
    auto weight = [](float t, float join) { return t < join ? (1.0f - t / join) * (1.0f - t / join) : 0.0f; };
    // The parent's drawn point at (t, u, v): c + U u + V v, its terms at t.
    struct Affine {
        Vec3f c, u, v;
    };
    auto parentAt = [&](float t) {
        const float w = weight(t, parent.rootJoin());
        const Vec3f c = parent.at(t, 0.0f, 0.0f), c0 = parent.at(0.0f, 0.0f, 0.0f), f0 = parent.footprintAt(0.0f, 0.0f);
        const Vec3f au = parent.at(t, 1.0f, 0.0f) - c, av = parent.at(t, 0.0f, 1.0f) - c;
        const Vec3f au0 = parent.at(0.0f, 1.0f, 0.0f) - c0, av0 = parent.at(0.0f, 0.0f, 1.0f) - c0;
        const Vec3f fu = parent.footprintAt(1.0f, 0.0f) - f0, fv = parent.footprintAt(0.0f, 1.0f) - f0;
        return Affine{c + (f0 - c0) * w, au + (fu - au0) * w, av + (fv - av0) * w};
    };
    std::vector<SliceFitSample> out;
    for (int k = 0; k < std::max(samples, 2); ++k) {
        SliceFitSample s;
        s.t = static_cast<float>(k) / static_cast<float>(std::max(samples, 2) - 1);
        const float wOwn = weight(s.t, own.rootJoin());
        for (std::size_t q = 0; q < outline.size(); ++q) {
            const SectionPoint& o = outline[q];
            const Vec3f root = ring ? g->rootRing[q] : own.footprintAt(o.u, o.v);
            const Vec3f w = own.at(s.t, o.u, o.v) + (root - own.at(0.0f, o.u, o.v)) * wOwn;
            // The parent's (t, u, v) nearest to holding it: t searched, (u, v) fitted there.
            constexpr int kSteps = 400;
            float bestErr = std::numeric_limits<float>::infinity(), bestT = s.t;
            SectionPoint bestUV;
            for (int j = 0; j <= kSteps; ++j) {
                const float t = static_cast<float>(j) / kSteps;
                const Affine a = parentAt(t);
                const Vec3f d = w - a.c;
                const float uu = a.u.dot(a.u), uv = a.u.dot(a.v), vv = a.v.dot(a.v), det = uu * vv - uv * uv;
                if (det == 0.0f) continue;
                const float du = d.dot(a.u), dv = d.dot(a.v);
                const SectionPoint at{(du * vv - dv * uv) / det, (dv * uu - du * uv) / det};
                const float err = (d - a.u * at.u - a.v * at.v).length();
                if (err < bestErr) bestErr = err, bestT = t, bestUV = at;
            }
            // (The parent drawn from its root loop, not its flat footprint, where it has one: (u, v)
            // refined there, Newton's, by differences.)
            if (pRing) {
                const float w2 = weight(bestT, parent.rootJoin());
                auto drawn = [&](const SectionPoint& at) {
                    return parent.at(bestT, at.u, at.v) + (cageMap(pOutline, p->rootRing, at) - parent.at(0.0f, at.u, at.v)) * w2;
                };
                for (int it = 0; it < 4; ++it) {
                    const Vec3f f = drawn(bestUV) - w;
                    constexpr float hh = 1e-3f;
                    const Vec3f ju = (drawn({bestUV.u + hh, bestUV.v}) - drawn(bestUV)) * (1.0f / hh);
                    const Vec3f jv = (drawn({bestUV.u, bestUV.v + hh}) - drawn(bestUV)) * (1.0f / hh);
                    const float uu = ju.dot(ju), uv = ju.dot(jv), vv = jv.dot(jv), det = uu * vv - uv * uv;
                    if (det == 0.0f) break;
                    const float du = f.dot(ju), dv = f.dot(jv);
                    bestUV.u -= (du * vv - dv * uv) / det;
                    bestUV.v -= (dv * uu - du * uv) / det;
                }
            }
            s.outline.push_back(bestUV);
            s.off.push_back(bestT - s.t);
        }
        {
            const float tp = std::clamp(s.t + (s.off.empty() ? 0.0f : s.off.front()), 0.0f, 1.0f);
            const Vec3f pc = parent.at(tp, 0.0f, 0.0f), pu = parent.at(tp, 1.0f, 0.0f) - pc, pv = parent.at(tp, 0.0f, 1.0f) - pc;
            const Vec3f oc = own.at(s.t, 0.0f, 0.0f), ou = own.at(s.t, 1.0f, 0.0f) - oc;
            const Vec3f axis = pu.cross(pv).normalized();
            const Vec3f a = (pu - axis * pu.dot(axis)).normalized(), b = (ou - axis * ou.dot(axis)).normalized();
            s.turn = std::atan2(axis.dot(a.cross(b)), a.dot(b));
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Vec3f> rootTubeOutline(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id) {
    const HierarchyGuide* g = h.guide(id);
    if (!g) throw std::invalid_argument("no guide " + std::to_string(id));
    if (g->rootOutline.size() >= 3 &&
        std::all_of(g->rootOutline.begin(), g->rootOutline.end(), [&](const StrandRoot& r) { return scalp.contains(r); })) {
        std::vector<Vec3f> out;
        for (const StrandRoot& r : g->rootOutline) out.push_back(scalp.position(r));
        return out;
    }
    return rootTubeSectionOutline(h, scalp, id, true);
}

void keepRootTubeOutline(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const std::vector<Vec3f>& outline) {
    const HierarchyGuide* g = h.guide(id);
    if (!g || !isRootTubeGuide(*g)) throw std::invalid_argument("guide " + std::to_string(id) + " is not a root tube");
    h.setRootOutline(id, bindOutline(scalp, projectAll(scalp, checkedOutline(outline))));
}

ScalpPointNormal rootTubeCenter(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id) {
    return rootOutlineCenter(scalp, rootTubeOutline(h, scalp, id));
}

std::vector<Vec3f> carryOutline(const ScalpMesh& scalp, const std::vector<Vec3f>& outline, const Vec3f& start, const Vec3f& to,
                                const std::set<int>& held) {
    const Surface surface{&scalp};
    const auto [c0, n0] = surface(V(start));
    const auto [c1, n1] = surface(V(to));
    std::vector<Vec3f> out;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        if (held.count(static_cast<int>(i))) {
            out.push_back(outline[i]);
        } else {
            out.push_back(surface(c1 + turn(V(outline[i]) - c0, n0, n1)).first.f());
        }
    }
    return out;
}

std::vector<int> sealedCvs(const std::vector<Vec3f>& outline, const std::vector<std::vector<Vec3f>>& others, float tolerance) {
    const Pts pts = toV(outline);
    double scale = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) scale = std::max(scale, dist(pts[i], pts[(i + 1) % pts.size()]));
    const double tol = tolerance >= 0.0f ? tolerance : 1e-3 * std::max(scale, 1e-6);
    std::vector<int> held;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        bool hit = false;
        for (const std::vector<Vec3f>& cvs : others) {
            const std::size_t n = cvs.size();
            for (std::size_t k = 0; k < n && !hit; ++k) {
                const V a(cvs[k]), b(cvs[(k + 1) % n]);
                const V ab = b - a;
                const double span = ab.dot(ab);
                const double f = span > 1e-18 ? std::clamp((pts[i] - a).dot(ab) / span, 0.0, 1.0) : 0.0;
                if (dist(pts[i], a + ab * f) <= tol) hit = true;
            }
            if (hit) break;
        }
        if (hit) held.push_back(static_cast<int>(i));
    }
    return held;
}

RootSnap snapRootCv(Vec3f& p, const std::vector<std::vector<Vec3f>>& outlines,
                    const std::function<std::array<float, 2>(const Vec3f&)>& toScreen, float pixels) {
    const std::array<float, 2> s = toScreen(p);
    double best = std::numeric_limits<double>::infinity();
    Vec3f at = p;
    for (const std::vector<Vec3f>& cvs : outlines) {
        for (const Vec3f& q : cvs) {
            const std::array<float, 2> sq = toScreen(q);
            const double d = std::hypot(sq[0] - s[0], sq[1] - s[1]);
            if (d <= pixels && d < best) best = d, at = q;
        }
    }
    if (!std::isinf(best)) {
        p = at;
        return RootSnap::Cv;
    }
    for (const std::vector<Vec3f>& cvs : outlines) {
        const std::size_t n = cvs.size();
        for (std::size_t i = 0; i < n; ++i) {
            const V a(cvs[i]), b(cvs[(i + 1) % n]);
            const V ab = b - a;
            const double span = ab.dot(ab);
            const double f = span > 1e-18 ? std::clamp((V(p) - a).dot(ab) / span, 0.0, 1.0) : 0.0;
            const Vec3f q = (a + ab * f).f();
            const std::array<float, 2> sq = toScreen(q);
            const double d = std::hypot(sq[0] - s[0], sq[1] - s[1]);
            if (d <= pixels && d < best) best = d, at = q;
        }
    }
    if (!std::isinf(best)) {
        p = at;
        return RootSnap::Edge;
    }
    return RootSnap::None;
}

GuideId duplicateRootTube(GuideHierarchy& h, const ScalpMesh& scalp, GuideId id, const Vec3f& to, const Vec3f* startAt) {
    const HierarchyGuide* g = h.guide(id);
    if (!g || !isRootTubeGuide(*g)) throw std::invalid_argument("guide " + std::to_string(id) + " is not a root tube");
    const Surface surface{&scalp};
    const std::vector<Vec3f> outline = rootTubeOutline(h, scalp, id);
    const auto [start, n0] = startAt ? surface(V(*startAt)) : patchApex(scalp, toV(outline));
    const auto [c1, n1] = surface(V(to));
    auto move = [&, start = start, n0 = n0, c1 = c1, n1 = n1](const V& p) { return c1 + turn(p - start, n0, n1); };
    const Pts newOutline = toV(carryOutline(scalp, outline, start.f(), to));
    Pts points;
    for (const Vec3f& p : g->points) points.push_back(move(V(p)));
    points[0] = surface(points[0]).first;
    const TubeProfile tube = g->tube;
    const GroupId group = g->group;
    const GuideHierarchy before = h;  // (the hierarchy under the source, as it was)

    const GuideId copy = h.addGuide(group, toF(points));
    h.evaluate(&scalp);
    TubeProfile placeholder;
    placeholder.shape = TubeShape::Ribbon;
    placeholder.width = {tube.width.empty() ? 0.1f : tube.width[0], tube.width.empty() ? 0.1f : tube.width[0]};
    placeholder.aspect = {1.0f};
    placeholder.twist = {0.0f};
    placeholder.rollOnScalp = false;
    setTube(h, scalp, copy, placeholder);
    const std::string shape = newShapeName(h);
    const Section s = fitShape(h, scalp, copy, shape, newOutline);
    const double w0 = orOne(tube.width.empty() ? 0.0 : tube.width[0], 1e-9);
    const double a0 = orOne(tube.aspect.empty() ? 0.0 : tube.aspect[0], 1.0);
    const double turned = s.twist - (tube.twist.empty() ? 0.0 : tube.twist[0]);
    const double aNew = s.width > 1e-12 ? clampAspect(s.height / s.width) : 1.0;
    TubeProfile made = tube;
    made.custom = h.tubeShape(shape);
    for (float& x : made.width) x = static_cast<float>(x * s.width / w0);
    for (float& a : made.aspect) a = static_cast<float>(clampAspect(a * aNew / a0));
    for (float& t : made.twist) t = static_cast<float>(t + turned);
    made.rollOnScalp = false;
    setTube(h, scalp, copy, made);
    // Its root on its shape's box middle, as Edit root puts it (an outline that fits several ways,
    // a regular polygon, may fit the copy turned another way than its source: its box middle then
    // lies off the carried root), fitted and stored there.
    editRootTube(h, scalp, copy, toF(newOutline));

    // The hierarchy under it: each parent's children made again (its split), each the copy of its
    // counterpart (its shape carried, its tube's lists turned as much), parents first.
    std::unordered_map<GuideId, GuideId> made2{{id, copy}};
    std::vector<GuideId> level{id};
    while (!level.empty()) {
        std::vector<GuideId> below;
        for (const GuideId oldParent : level) {
            std::vector<GuideId> kids = before.childrenOf(oldParent);
            if (kids.empty()) continue;
            std::sort(kids.begin(), kids.end());
            const GroupId kidGroup = before.guide(kids.front())->group;
            const std::string kidGroupName = before.group(kidGroup)->name;
            const GroupId parentGroup = h.guide(made2.at(oldParent))->group;
            const GroupId existing = h.findGroup(kidGroupName);
            std::vector<GuideId> had = existing != kNoGroup ? h.guidesInGroup(existing) : std::vector<GuideId>{};
            const std::set<GuideId> hadSet(had.begin(), had.end());
            ChildGenerationParams params;
            params.count = kids.size();
            params.split = true;
            const std::vector<GuideId> parents{made2.at(oldParent)};
            h.generateChildren(parentGroup, kidGroupName, params, &parents);
            std::vector<GuideId> fresh;
            for (const GuideId k : h.guidesInGroup(h.findGroup(kidGroupName))) {
                if (!hadSet.count(k)) fresh.push_back(k);
            }
            std::sort(fresh.begin(), fresh.end());
            for (std::size_t i = 0; i < kids.size() && i < fresh.size(); ++i) {
                made2[kids[i]] = fresh[i];
                below.push_back(kids[i]);
                const HierarchyGuide* old = before.guide(kids[i]);
                TubeProfile kidTube = h.guide(fresh[i])->tube;
                kidTube.width = old->tube.width;
                kidTube.aspect = old->tube.aspect;
                kidTube.twist = old->tube.twist;
                for (float& t : kidTube.twist) t = static_cast<float>(t + turned);
                kidTube.midShape = old->tube.midShape;
                kidTube.tipShape = old->tube.tipShape;
                if (old->tube.custom && isRootTube(old->tube, 0)) {  // a slice: its half, a shape of its own
                    const std::string name = newShapeName(h);
                    h.setOwnedTubeShape(name, old->tube.custom->points, old->tube.custom->smooth, old->tube.custom->origin);
                    kidTube.custom = h.tubeShape(name);
                    kidTube.rollOnScalp = old->tube.rollOnScalp;
                }
                kidTube.rootHold = old->tube.rootHold;
                kidTube.midHold = old->tube.midHold;
                kidTube.tipHold = old->tube.tipHold;
                kidTube.midAt = old->tube.midAt;
                h.setTube(fresh[i], kidTube);
                Pts kidPoints;
                for (const Vec3f& p : old->points) kidPoints.push_back(move(V(p)));
                h.setPoints(fresh[i], toF(kidPoints));
            }
            h.evaluate(&scalp);
        }
        level = std::move(below);
    }
    return copy;
}

std::size_t deleteRootTubes(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides) {
    if (guides.empty()) return 0;
    const std::size_t before = h.guides().size();
    h.removeGuides(guides, true);
    h.evaluate(&scalp);
    return before - h.guides().size();
}

float rootFootprintError(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id) {
    const Pts outline = toV(rootTubeOutline(h, scalp, id));
    const Footprint fp = footprintOf(h, scalp, id);
    const double k = 1.0 + kRootTubeOverlap;
    std::vector<P2> want, have;
    for (const V& p : outline) {
        const P2 q = toPlane(p, fp.center, fp.u, fp.v);
        want.push_back({q.x * k, q.y * k});
    }
    for (const Vec3f& p : rootTubeSectionOutline(h, scalp, id, false)) have.push_back(toPlane(V(p), fp.center, fp.u, fp.v));
    double size = 0.0;
    for (const P2& p : want) size = std::max(size, std::hypot(p.x, p.y));
    if (size == 0.0) size = 1.0;
    double worst = 0.0;
    for (const P2& a : want) {
        double nearest = std::numeric_limits<double>::infinity();
        for (const P2& b : have) nearest = std::min(nearest, std::hypot(a.x - b.x, a.y - b.y));
        worst = std::max(worst, nearest);
    }
    return static_cast<float>(worst / size);
}

// ---- The tube hierarchy ------------------------------------------------------------------------------------

std::string tubeLevelGroup(const std::string& base, int level) {
    return level <= 1 ? base : base + kTubeLevelSuffix + std::to_string(level);
}

namespace {

const GuideGroup* topOf(const GuideHierarchy& h, GroupId id) {
    const GuideGroup* g = h.group(id);
    std::set<GroupId> seen;
    while (g && g->parent != kNoGroup && seen.insert(g->id).second) g = h.group(g->parent);
    return g;
}

std::string baseGroup(const GuideHierarchy& h, const std::vector<GuideId>& guides, const std::string& picked) {
    for (const GuideId id : guides) {
        const HierarchyGuide* g = h.guide(id);
        if (!g) continue;
        if (const GuideGroup* top = topOf(h, g->group)) return top->name;
    }
    if (!picked.empty()) {
        const GroupId id = h.findGroup(picked);
        if (id != kNoGroup) {
            if (const GuideGroup* top = topOf(h, id)) return top->name;
        }
    }
    for (const GuideGroup& g : h.groups()) {
        if (g.parent == kNoGroup && !h.guidesInGroup(g.id).empty()) return g.name;
    }
    return {};
}

// Each level's group ID -> its level (1: the base), and each guide of the hierarchy's children.
struct LevelIndex {
    std::unordered_map<GroupId, int> levelOf;
    std::unordered_map<GuideId, std::vector<GuideId>> children;
    int level(const GuideHierarchy& h, GuideId id) const {
        const HierarchyGuide* g = h.guide(id);
        if (!g) return 0;
        const auto it = levelOf.find(g->group);
        return it == levelOf.end() ? 0 : it->second;
    }
};
// ---- Slicing a drawn root's tube in two (issues 85, 86) ---------------------------------------------

// `poly` clipped to the side of the line through `m` where dot(p - m, n) <= 0 (Sutherland-Hodgman).
std::vector<P2> clipSide(const std::vector<P2>& poly, const P2& m, const P2& n) {
    std::vector<P2> out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const P2& a = poly[i];
        const P2& b = poly[(i + 1) % poly.size()];
        const double da = (a.x - m.x) * n.x + (a.y - m.y) * n.y, db = (b.x - m.x) * n.x + (b.y - m.y) * n.y;
        if (da <= 0.0) out.push_back(a);
        if ((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) {
            const double t = da / (da - db);
            out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
    return out;
}

// At most kMaxTubeShapePoints points: those mattering least to the shape taken out.
std::vector<P2> capPoints(std::vector<P2> poly) {
    while (poly.size() > kMaxTubeShapePoints) {
        std::size_t at = 0;
        double least = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const P2& a = poly[(i + poly.size() - 1) % poly.size()];
            const P2& b = poly[i];
            const P2& c = poly[(i + 1) % poly.size()];
            const double area = std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
            if (area < least) least = area, at = i;
        }
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return poly;
}

// A drawn root's tube (or a slice of one) split into `kids` (two, just made by generateChildren):
// its root shape cut in two across its longer side through its middle, each child that half of
// its tube (sliceTube). So the two fill the parent's tube as if it were sliced lengthwise: they
// meet at the cut (a hair apart) and never pass through each other unless groomed so. Left as
// generateChildren made them when the parent has no shape to cut.
void sliceChildren(GuideHierarchy& h, const ScalpMesh& scalp, GuideId parentId, std::vector<GuideId> kids) {
    const HierarchyGuide* parent = h.guide(parentId);
    if (!parent || kids.size() != 2 || !parent->tube.custom || parent->points.size() < 2 || !scalp.contains(parent->root)) return;
    const TubeSource source = tubeSourceOf(h, scalp, parentId);
    const double w0 = std::max(1e-9, static_cast<double>(source.tube.widthAt(0.0f)));
    const double h0 = w0 * std::max(1e-6, static_cast<double>(source.tube.aspectAt(0.0f)));
    // The root shape in scene units (the section's width across, its height up), its middle and longer way.
    std::vector<P2> shape;
    for (const SectionPoint& q : source.tube.custom->points) shape.push_back({q.u * w0, q.v * h0});
    if (shape.size() < 3) return;
    P2 mid{0.0, 0.0};
    double area = 0.0;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const P2& a = shape[i];
        const P2& b = shape[(i + 1) % shape.size()];
        const double c = a.x * b.y - b.x * a.y;
        area += c;
        mid.x += (a.x + b.x) * c;
        mid.y += (a.y + b.y) * c;
    }
    if (std::abs(area) < 1e-18) return;
    mid = {mid.x / (3.0 * area), mid.y / (3.0 * area)};
    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (const P2& q : shape) {
        sxx += (q.x - mid.x) * (q.x - mid.x), syy += (q.y - mid.y) * (q.y - mid.y), sxy += (q.x - mid.x) * (q.y - mid.y);
    }
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);  // the longer way
    const P2 longer{std::cos(angle), std::sin(angle)};
    const std::vector<P2> halves[2] = {clipSide(shape, mid, longer), clipSide(shape, mid, {-longer.x, -longer.y})};
    std::sort(kids.begin(), kids.end());
    for (int k = 0; k < 2; ++k) {
        std::vector<SectionPoint> half;
        for (const P2& q : halves[k]) half.push_back({static_cast<float>(q.x / w0), static_cast<float>(q.y / h0)});
        sliceTube(h, scalp, source, half, kids[static_cast<std::size_t>(k)]);
    }
    h.evaluate(&scalp);
}

LevelIndex indexLevels(const GuideHierarchy& h, const TubeLevels& info) {
    LevelIndex out;
    for (std::size_t depth = 0; depth < info.levels.size(); ++depth) {
        const GroupId id = h.findGroup(info.levels[depth]);
        if (id != kNoGroup) out.levelOf[id] = static_cast<int>(depth) + 1;
    }
    for (const HierarchyGuide& g : h.guides()) {
        if (g.isChild() && out.levelOf.count(g.group)) out.children[g.parent].push_back(g.id);
    }
    for (auto& [parent, kids] : out.children) std::sort(kids.begin(), kids.end());
    return out;
}

}  // namespace

TubeLevels tubeLevels(const GuideHierarchy& h, const std::vector<GuideId>& guides, const std::string& picked) {
    TubeLevels out;
    out.base = baseGroup(h, guides, picked);
    if (out.base.empty()) return out;
    out.levels.push_back(out.base);
    GroupId above = h.findGroup(out.base);
    for (int level = 2; level <= kMaxTubeLevels; ++level) {
        const GroupId id = h.findGroup(tubeLevelGroup(out.base, level));
        if (id == kNoGroup || h.group(id)->parent != above) break;
        out.levels.push_back(h.group(id)->name);
        above = id;
    }
    return out;
}

namespace {
// A new split's slices with clean roots (issue #110): where a slice's first few parent root
// widths turn sharply (its parent combed down onto the scalp or into it there, its slices
// gathered on the scalp otherwise than in its sections), that stretch smoothed out, fading to
// nothing past it, and the guide bound so (GuideHierarchy::setPoints: its offsets in its parent's
// sections), once, as it is made: what a brush does to it later stays as it is left.
void smoothSliceRoots(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& slices) {
    for (const GuideId id : slices) {
        const HierarchyGuide* g = h.guide(id);
        const HierarchyGuide* parent = g ? h.guide(g->parent) : nullptr;
        if (!parent || g->points.size() < 3) continue;
        std::vector<Vec3f> pts = g->points;
        const std::size_t n = pts.size();
        const float reach = 3.0f * parent->tube.widthAt(0.0f) * std::max(1.0f, parent->tube.aspectAt(0.0f)) * h.scale();
        if (!(reach > 0.0f)) continue;
        std::vector<float> along(n, 0.0f);
        for (std::size_t i = 1; i < n; ++i) along[i] = along[i - 1] + (pts[i] - pts[i - 1]).length();
        const auto zone = [&](std::size_t i) {
            const float k = std::clamp(1.0f - along[i] / reach, 0.0f, 1.0f);
            return k * k * (3.0f - 2.0f * k);
        };
        bool sharp = false;
        for (std::size_t i = 1; i + 1 < n && !sharp && zone(i) > 0.0f; ++i) {
            const Vec3f a = (pts[i] - pts[i - 1]).normalized(), b = (pts[i + 1] - pts[i]).normalized();
            sharp = a.dot(b) < 0.7f;  // (over 45 degrees)
        }
        if (!sharp) continue;
        for (int pass = 0; pass < 4; ++pass) {
            const std::vector<Vec3f> was = pts;
            for (std::size_t i = 1; i + 1 < n; ++i) {
                const float w = 0.5f * zone(i);
                if (w > 0.0f) pts[i] = was[i] + ((was[i - 1] + was[i + 1]) * 0.5f - was[i]) * w;
            }
        }
        h.setPoints(id, std::move(pts));
    }
    h.evaluate(&scalp);
}
}  // namespace

std::size_t addTubeLevel(GuideHierarchy& h, const ScalpMesh* scalp, const std::vector<GuideId>& guides, const std::string& picked) {
    const TubeLevels info = tubeLevels(h, guides, picked);
    if (info.base.empty()) throw std::invalid_argument("the groom has no guides to split");
    const LevelIndex index = indexLevels(h, info);
    std::vector<GuideId> start;
    if (!guides.empty()) {
        for (const GuideId id : guides) {
            if (index.level(h, id)) start.push_back(id);
        }
    } else {
        for (const HierarchyGuide& g : h.guides()) {
            if (index.level(h, g.id) == 1) start.push_back(g.id);
        }
    }
    if (start.empty()) {
        throw std::invalid_argument("no tubes of " + info.base + " to split: select tubes of the hierarchy, or none for all");
    }
    std::vector<GuideId> leaves, todo = start;
    std::set<GuideId> seen;
    while (!todo.empty()) {
        const GuideId g = todo.back();
        todo.pop_back();
        if (!seen.insert(g).second) continue;
        const auto it = index.children.find(g);
        if (it != index.children.end() && !it->second.empty()) {
            todo.insert(todo.end(), it->second.begin(), it->second.end());
        } else {
            leaves.push_back(g);
        }
    }
    std::map<int, std::vector<GuideId>> byLevel;
    for (const GuideId g : leaves) byLevel[index.level(h, g)].push_back(g);
    if (std::all_of(byLevel.begin(), byLevel.end(), [](const auto& e) { return e.first >= kMaxTubeLevels; })) {
        throw std::invalid_argument("the tubes are split " + std::to_string(kMaxTubeLevels) + " levels deep already (the most)");
    }
    std::size_t split = 0;
    for (auto& [level, parents] : byLevel) {
        if (level >= kMaxTubeLevels) continue;
        std::sort(parents.begin(), parents.end());
        const GroupId group = h.guide(parents.front())->group;
        // A plain round tube laid across the scalp first (its aspect a hair off 1), so its halves lie
        // side by side along the scalp.
        for (const GuideId g : parents) {
            TubeProfile tube = h.guide(g)->tube;
            const bool plainRound = !tube.custom && tube.shape == TubeShape::Round &&
                                    std::all_of(tube.aspect.begin(), tube.aspect.end(), [](float a) { return std::abs(a - 1.0f) < 1e-6f; }) &&
                                    std::all_of(tube.twist.begin(), tube.twist.end(), [](float t) { return std::abs(t) < 1e-9f; });
            if (plainRound) {
                tube.aspect = {0.999f};
                h.setTube(g, tube);
            }
        }
        h.evaluate(scalp);
        ChildGenerationParams params;
        params.count = 2;
        params.split = true;
        h.generateChildren(group, tubeLevelGroup(info.base, level + 1), params, &parents);
        split += parents.size();
        // A drawn root's tubes: each split is its parent's root shape sliced in two (sliceChildren).
        if (scalp) {
            h.evaluate(scalp);
            std::vector<GuideId> slices;
            for (const GuideId p : parents) {
                const HierarchyGuide* g = h.guide(p);
                if (!g || !isRootTube(g->tube, 0)) continue;
                const std::vector<GuideId> kids = h.childrenOf(p);
                sliceChildren(h, *scalp, p, kids);
                slices.insert(slices.end(), kids.begin(), kids.end());
            }
            if (!slices.empty()) {
                h.evaluate(scalp);
                smoothSliceRoots(h, *scalp, slices);
            }
        }
    }
    h.evaluate(scalp);
    return split;
}

std::size_t removeTubeLevel(GuideHierarchy& h, const ScalpMesh* scalp, const std::string& base, const std::vector<GuideId>& guides) {
    const TubeLevels info = tubeLevels(h, {}, base);
    if (info.levels.size() <= 1) throw std::invalid_argument(base + " has no level under it to remove");
    if (guides.empty()) {
        h.removeGroup(h.findGroup(info.levels.back()));
        h.evaluate(scalp);
        return 0;
    }
    const LevelIndex index = indexLevels(h, info);
    std::set<GuideId> gone;
    for (const GuideId g : guides) {
        if (!h.guide(g) || !index.level(h, g)) continue;
        const auto it = index.children.find(g);
        if (it != index.children.end() && !it->second.empty()) {
            std::vector<GuideId> below, todo = it->second;
            while (!todo.empty()) {
                const GuideId k = todo.back();
                todo.pop_back();
                const auto kids = index.children.find(k);
                if (kids != index.children.end() && !kids->second.empty()) {
                    todo.insert(todo.end(), kids->second.begin(), kids->second.end());
                } else {
                    below.push_back(k);
                }
            }
            int deepest = 0;
            for (const GuideId k : below) deepest = std::max(deepest, index.level(h, k));
            for (const GuideId k : below) {
                if (index.level(h, k) == deepest) gone.insert(k);
            }
        } else if (index.level(h, g) > 1 && h.guide(g)->isChild()) {
            const auto siblings = index.children.find(h.guide(g)->parent);
            if (siblings != index.children.end()) {
                gone.insert(siblings->second.begin(), siblings->second.end());
            } else {
                gone.insert(g);
            }
        }
    }
    if (gone.empty()) {
        throw std::invalid_argument("no split tubes to remove under the selection: select a split tube, or one that is split");
    }
    h.removeGuides({gone.begin(), gone.end()}, true);
    h.evaluate(scalp);
    return gone.size();
}

std::map<std::string, bool> tubeLevelVisibility(const GuideHierarchy& h, const std::string& base, int level, int locked) {
    const TubeLevels info = tubeLevels(h, {}, base);
    if (info.levels.empty()) throw std::invalid_argument("no tube hierarchy on " + base);
    level = std::clamp(level, 1, static_cast<int>(info.levels.size()));
    std::map<std::string, bool> out;
    for (std::size_t k = 0; k < info.levels.size(); ++k) {
        const int at = static_cast<int>(k) + 1;
        out[info.levels[k]] = at == level;
        (void)locked;  // (Lock view keeps its level's tubes in view through the display: tubeViewGroup)
    }
    return out;
}

// ---- Overlaps -------------------------------------------------------------------------------------------

std::vector<TubeOverlap> tubeOverlaps(const GuideHierarchy& h, const std::vector<GuideId>& guides) {
    std::vector<const HierarchyGuide*> tubes;
    if (guides.empty()) {
        for (const HierarchyGuide& g : h.guides()) {
            if (isRootTube(g.tube, 0) && g.points.size() >= 2 && !g.tube.blended()) tubes.push_back(&g);
        }
    } else {
        for (const GuideId id : guides) {
            const HierarchyGuide* g = h.guide(id);
            if (g && isRootTube(g->tube, 0) && g->points.size() >= 2 && !g->tube.blended()) tubes.push_back(g);
        }
    }
    std::vector<TubeSpine> spines;
    std::vector<float> reach;
    for (const HierarchyGuide* g : tubes) {
        spines.emplace_back(g->points.data(), g->points.size(), g->frameUp, g->tube, h.scale());
        if (g->rootNormal.length() > 0.5f && g->rootAcross.length() > 0.5f) spines.back().setRootOnScalp(g->rootNormal, g->rootAcross);
        reach.push_back(std::max(spines.back().rootWidth(), spines.back().rootHeight()));
    }
    // The tubes as they are drawn and their hair grows (display.cpp appendTubeSurface, sliceFit):
    // the sections, joined from the root loop on the scalp (else the footprint) by (1 - t / join)^2.
    // (Near a leaning root the sections alone are not the tube: a cell rooted on the scalp leaves
    // its section there, as its part of the tube does.)
    auto weight = [](const TubeSpine& s, float t) {
        const float join = s.rootJoin();
        return t < join ? (1.0f - t / join) * (1.0f - t / join) : 0.0f;
    };
    auto share = [&](std::size_t k, std::size_t i) {
        return tubes[k]->points.size() > 1 ? static_cast<float>(i) / static_cast<float>(tubes[k]->points.size() - 1) : 0.0f;
    };
    auto drawn = [&](std::size_t k, std::size_t i, const SectionPoint& q) {
        const HierarchyGuide& g = *tubes[k];
        const TubeSpine& s = spines[k];
        const std::vector<SectionPoint>& outline = tubeOutline(g.tube);
        const Vec3f root = g.rootRing.size() == outline.size() && outline.size() >= 3 ? cageMap(outline, g.rootRing, q) : s.footprintAt(q.u, q.v);
        return s.atSample(i, q.u, q.v) + (root - s.at(0.0f, q.u, q.v)) * weight(s, share(k, i));
    };
    // A point's drawn section coordinates at a tube's sample: least squares on the footprint's
    // join, then refined on the root loop's (Newton's, by differences).
    auto coords = [&](std::size_t k, std::size_t i, const Vec3f& p) {
        const TubeSpine& s = spines[k];
        const float w = weight(s, share(k, i));
        const Vec3f c0 = s.at(0.0f, 0.0f, 0.0f), f0 = s.footprintAt(0.0f, 0.0f);
        const Vec3f c = s.center(i) + (f0 - c0) * w;
        const Vec3f a = s.axisU(i) + ((s.footprintAt(1.0f, 0.0f) - f0) - (s.at(0.0f, 1.0f, 0.0f) - c0)) * w;
        const Vec3f b = s.axisV(i) + ((s.footprintAt(0.0f, 1.0f) - f0) - (s.at(0.0f, 0.0f, 1.0f) - c0)) * w;
        const Vec3f d = p - c;
        const float aa = a.dot(a), bb = b.dot(b), ab = a.dot(b), da = d.dot(a), db = d.dot(b);
        const float det = std::max(aa * bb - ab * ab, 1e-30f);
        SectionPoint q{(da * bb - db * ab) / det, (db * aa - da * ab) / det};
        if (w > 0.0f) {
            for (int it = 0; it < 4; ++it) {
                constexpr float e = 1e-3f;
                const Vec3f at = drawn(k, i, q);
                const Vec3f f = at - p;
                const Vec3f ju = (drawn(k, i, {q.u + e, q.v}) - at) * (1.0f / e);
                const Vec3f jv = (drawn(k, i, {q.u, q.v + e}) - at) * (1.0f / e);
                const float uu = ju.dot(ju), uv = ju.dot(jv), vv = jv.dot(jv), dd = uu * vv - uv * uv;
                if (!(dd > 0.0f)) break;
                const float fu = f.dot(ju), fv = f.dot(jv);
                q.u -= (fu * vv - fv * uv) / dd;
                q.v -= (fv * uu - fu * uv) / dd;
            }
        }
        return q;
    };
    // How far inside the outline (unit coordinates) a point lies; below 0: outside.
    auto depthIn = [](const std::vector<SectionPoint>& poly, const SectionPoint& p) {
        bool in = false;
        float edge = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const SectionPoint& a = poly[i];
            const SectionPoint& b = poly[j];
            if ((a.v > p.v) != (b.v > p.v) && p.u < (b.u - a.u) * (p.v - a.v) / (b.v - a.v) + a.u) in = !in;
            const float du = b.u - a.u, dv = b.v - a.v, span = du * du + dv * dv;
            const float f = span > 0.0f ? std::clamp(((p.u - a.u) * du + (p.v - a.v) * dv) / span, 0.0f, 1.0f) : 0.0f;
            edge = std::min(edge, std::hypot(p.u - (a.u + du * f), p.v - (a.v + dv * f)));
        }
        return in ? edge : -edge;
    };
    std::vector<TubeOverlap> out;
    for (std::size_t x = 0; x < tubes.size(); ++x) {
        for (std::size_t y = 0; y < tubes.size(); ++y) {
            if (x == y || tubes[x]->group != tubes[y]->group) continue;
            const float apart = (tubes[x]->points.front() - tubes[y]->points.front()).length();
            if (apart > 2.0f * (reach[x] + reach[y])) continue;  // (not neighbours)
            const HierarchyGuide& into = *tubes[x];
            const HierarchyGuide& from = *tubes[y];
            const std::vector<SectionPoint>& own = tubeOutline(into.tube);
            const std::vector<SectionPoint>& theirs = tubeOutline(from.tube);
            TubeOverlap worst;
            const std::size_t n = into.points.size(), m = from.points.size();
            for (std::size_t i = 1; i < n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n - 1);
                const std::size_t j = std::min(m - 1, static_cast<std::size_t>(std::lround(t * static_cast<float>(m - 1))));
                if (j == 0) continue;
                for (const SectionPoint& q : theirs) {
                    const float d = depthIn(own, coords(x, i, drawn(y, j, q)));
                    if (d > worst.depth) worst = {into.id, from.id, t, d};
                }
            }
            if (worst.depth > 0.0f) out.push_back(worst);
        }
    }
    std::sort(out.begin(), out.end(), [](const TubeOverlap& a, const TubeOverlap& b) { return a.depth > b.depth; });
    return out;
}

// ---- Slices of a tube ----------------------------------------------------------------------------------

Vec3f alongNormal(const ScalpMesh& scalp, const Vec3f& p, const Vec3f& normal) {
    const SurfacePoint near = scalp.closestPoint(p);
    if (normal.length() < 0.5f) return near.position;
    Vec3f at = near.position;
    float best = std::numeric_limits<float>::infinity();
    for (const Vec3f& way : {normal, normal * -1.0f}) {
        SurfacePoint hit;
        float d = 0.0f;
        if (scalp.intersectRay(p, way, &hit, &d) && std::abs(d) < best) best = std::abs(d), at = hit.position;
    }
    if (std::isinf(best) || best > 4.0f * (near.position - p).length() + 1e-6f) return near.position;
    return at;
}

TubeSource tubeSourceOf(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId id) {
    const HierarchyGuide* g = h.guide(id);
    if (!g || g->points.size() < 2) throw std::invalid_argument("no guide " + std::to_string(id) + " to slice");
    TubeSource out;
    out.points = g->points;
    out.frameUp = g->frameUp;
    out.tube = g->tube;
    out.normal = scalp.contains(g->root) ? scalp.normal(g->root) : g->rootNormal;
    out.across = g->rootAcross.length() > 0.5f ? g->rootAcross : h.rootAcrossOf(*g, scalp);
    out.group = g->group;
    if (g->tube.custom && !g->tube.blended() && g->rootRing.size() == tubeOutline(g->tube).size()) {
        out.ring = g->rootRing;
        out.cage = tubeOutline(g->tube);
    }
    return out;
}

TubeSpine sourceSpine(const TubeSource& source, float scale) {
    TubeSpine spine(source.points.data(), source.points.size(), source.frameUp, source.tube, scale);
    if (source.normal.length() > 0.5f && source.across.length() > 0.5f) spine.setRootOnScalp(source.normal, source.across);
    return spine;
}

std::vector<Vec3f> slicePath(const ScalpMesh& scalp, const TubeSource& source, float u, float v, float scale) {
    // Its part of the source's tube as the tube is drawn (display.cpp appendTubeSurface, sliceFit):
    // rooted on the scalp where the tube's root loop (else its flat footprint) has (u, v), then the
    // tube's section point (u, v), joined from that root over the tube's own root join
    // ((1 - t / join)^2, TubeSpine::rootJoin), the whole of the pull. So a cell never leaves its
    // tube nor kinks at its base where the tube leans out of the scalp (issue #110): it runs where
    // that part of the tube runs, and its neighbours beside it as they tile the tube.
    const TubeSpine spine = sourceSpine(source, scale);
    const std::size_t n = source.points.size();
    std::vector<Vec3f> path;
    if (n < 2) return path;
    const bool ring = source.ring.size() >= 3 && source.ring.size() == source.cage.size();
    const Vec3f foot = ring ? cageMap(source.cage, source.ring, {u, v}) : spine.footprintAt(u, v);
    const Vec3f rooted = alongNormal(scalp, foot, source.normal);
    const Vec3f lift = rooted - spine.at(0.0f, u, v);
    const float join = std::clamp(spine.rootJoin(), 1e-3f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float k = t < join ? (1.0f - t / join) * (1.0f - t / join) : 0.0f;
        path.push_back(i == 0 ? rooted : spine.at(t, u, v) + lift * k);
    }
    return path;
}

namespace {

// `polygon` with each edge off `outline` (both section coordinates) moved `gap` into it, the edges
// along the outline kept: each point where its two edges' lines, so moved, meet.
std::vector<SectionPoint> gapInside(const std::vector<SectionPoint>& polygon, const std::vector<SectionPoint>& outline, float gap) {
    const std::size_t n = polygon.size();
    if (n < 3 || outline.size() < 3 || gap <= 0.0f) return polygon;
    float size = 0.0f;
    for (const SectionPoint& q : outline) size = std::max(size, std::hypot(q.u, q.v));
    const float near = 1e-2f * size;
    auto offOutline = [&](const SectionPoint& p) {
        float best = std::numeric_limits<float>::infinity();
        for (std::size_t k = 0; k < outline.size(); ++k) {
            const SectionPoint& a = outline[k];
            const SectionPoint& b = outline[(k + 1) % outline.size()];
            const float du = b.u - a.u, dv = b.v - a.v, span = du * du + dv * dv;
            const float f = span > 0.0f ? std::clamp(((p.u - a.u) * du + (p.v - a.v) * dv) / span, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, std::hypot(p.u - (a.u + du * f), p.v - (a.v + dv * f)));
        }
        return best;
    };
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = polygon[i];
        const SectionPoint& b = polygon[(i + 1) % n];
        area += static_cast<double>(a.u) * b.v - static_cast<double>(b.u) * a.v;
    }
    const float inward = area >= 0.0 ? 1.0f : -1.0f;  // (counter-clockwise: the inside on the left)
    // Each edge's line moved: a point on it and its way.
    std::vector<SectionPoint> at(n), way(n);
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = polygon[i];
        const SectionPoint& b = polygon[(i + 1) % n];
        const SectionPoint mid{0.5f * (a.u + b.u), 0.5f * (a.v + b.v)};
        float du = b.u - a.u, dv = b.v - a.v;
        const float len = std::hypot(du, dv);
        way[i] = len > 0.0f ? SectionPoint{du / len, dv / len} : SectionPoint{1.0f, 0.0f};
        const bool cut = offOutline(mid) > near || offOutline(a) > near || offOutline(b) > near;
        const float d = cut ? gap : 0.0f;
        any = any || cut;
        at[i] = {a.u - way[i].v * d * inward, a.v + way[i].u * d * inward};
    }
    if (!any) return polygon;
    std::vector<SectionPoint> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e0 = (i + n - 1) % n, e1 = i;  // (the edges before and after point i)
        const float cross = way[e0].u * way[e1].v - way[e0].v * way[e1].u;
        if (std::abs(cross) < 1e-4f) {
            // (Its edges in a line: moved as the edge after it is.)
            out[i] = {polygon[i].u + (at[e1].u - polygon[i].u), polygon[i].v + (at[e1].v - polygon[i].v)};
            continue;
        }
        const float wu = at[e1].u - at[e0].u, wv = at[e1].v - at[e0].v;
        const float s = (wu * way[e1].v - wv * way[e1].u) / cross;
        out[i] = {at[e0].u + way[e0].u * s, at[e0].v + way[e0].v * s};
    }
    // (Kept only when it is still the polygon, a little smaller: each point near where it was, the
    // same way round, most of its area. A sharp corner can throw the meeting point far off, which
    // makes a shape no library takes and loses the slice. Else the whole polygon kSliceGap in, about
    // its middle.)
    double after = 0.0;
    bool close = true;
    for (std::size_t i = 0; i < n; ++i) {
        const SectionPoint& a = out[i];
        const SectionPoint& b = out[(i + 1) % n];
        after += static_cast<double>(a.u) * b.v - static_cast<double>(b.u) * a.v;
        close = close && std::hypot(out[i].u - polygon[i].u, out[i].v - polygon[i].v) <= 4.0f * gap;
    }
    if (close && after * area > 0.0 && std::abs(after) >= 0.25 * std::abs(area)) return out;
    float cu = 0.0f, cv = 0.0f;
    for (const SectionPoint& q : polygon) cu += q.u, cv += q.v;
    cu /= static_cast<float>(n), cv /= static_cast<float>(n);
    float reach = 0.0f;
    for (const SectionPoint& q : polygon) reach = std::max(reach, std::hypot(q.u - cu, q.v - cv));
    const float k = reach > 0.0f ? std::clamp(1.0f - gap / reach, 0.75f, 1.0f) : 1.0f;
    for (std::size_t i = 0; i < n; ++i) out[i] = {cu + (polygon[i].u - cu) * k, cv + (polygon[i].v - cv) * k};
    return out;
}

// Whether (u, v) lies inside `poly` farther than `inset` from its edges.
bool polygonInsideBy(const std::vector<SectionPoint>& poly, float u, float v, float inset) {
    bool in = false;
    float edge = std::numeric_limits<float>::max();
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const SectionPoint& a = poly[i];
        const SectionPoint& b = poly[j];
        if ((a.v > v) != (b.v > v) && u < (b.u - a.u) * (v - a.v) / (b.v - a.v) + a.u) in = !in;
        const float du = b.u - a.u, dv = b.v - a.v, span = du * du + dv * dv;
        const float f = span > 0.0f ? std::clamp(((u - a.u) * du + (v - a.v) * dv) / span, 0.0f, 1.0f) : 0.0f;
        edge = std::min(edge, std::hypot(u - (a.u + du * f), v - (a.v + dv * f)));
    }
    return in && edge > inset;
}

}  // namespace

GuideId sliceTube(GuideHierarchy& h, const ScalpMesh& scalp, const TubeSource& source, const std::vector<SectionPoint>& polygon,
                  GuideId into, const std::vector<Vec3f>& outline, const std::function<Vec3f(float)>& deviation) {
    if (polygon.size() < 3 || source.points.size() < 2) return kNoGuide;
    if (into != kNoGuide && !h.guide(into)) return kNoGuide;
    // The gap: kSliceGap in from each edge where it meets another slice (an edge off the source's
    // outline), none along the source's outline, so the slices keep the source's outer shape at every
    // level (slices of slices never shrink) with a slight gap only between them.
    const std::vector<SectionPoint> gapped = gapInside(polygon, tubeOutline(source.tube), static_cast<float>(kSliceGap));
    // The polygon's middle in the source's section coordinates (its area's centroid, so its guide
    // leaves the middle of its root; its box's for a polygon whose centroid falls outside it) and
    // its reach from there each way, and its points fitted to them as its shape, that middle its
    // origin (CustomTubeShape::origin) (the gapped polygon's, else the polygon's as cut: a slice is
    // never lost to a shape no library takes).
    double cu = 0.0, cv = 0.0, su = 0.0, sv = 0.0;
    std::string name;
    auto fit = [&](const std::vector<SectionPoint>& poly) {
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        std::vector<P2> unit;
        double area = 0.0, au = 0.0, av = 0.0;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const SectionPoint& q = poly[i];
            const SectionPoint& r = poly[(i + 1) % poly.size()];
            unit.push_back({q.u, q.v});
            lo[0] = std::min(lo[0], static_cast<double>(q.u)), lo[1] = std::min(lo[1], static_cast<double>(q.v));
            hi[0] = std::max(hi[0], static_cast<double>(q.u)), hi[1] = std::max(hi[1], static_cast<double>(q.v));
            const double cross = static_cast<double>(q.u) * r.v - static_cast<double>(r.u) * q.v;
            area += cross;
            au += (static_cast<double>(q.u) + r.u) * cross;
            av += (static_cast<double>(q.v) + r.v) * cross;
        }
        if (!(hi[0] - lo[0] > 2e-6) || !(hi[1] - lo[1] > 2e-6)) return false;  // (a sliver)
        cu = 0.5 * (lo[0] + hi[0]), cv = 0.5 * (lo[1] + hi[1]);
        std::vector<SectionPoint> polyF(poly.begin(), poly.end());
        if (std::abs(area) > 1e-12) {
            const double mu = au / (3.0 * area), mv = av / (3.0 * area);
            // (Inside the polygon, well in from its edge: the guide must run through its shape.)
            const float inset = 0.05f * static_cast<float>(std::min(hi[0] - lo[0], hi[1] - lo[1]));
            if (polygonInsideBy(polyF, static_cast<float>(mu), static_cast<float>(mv), inset)) cu = mu, cv = mv;
        }
        su = std::max(hi[0] - cu, cu - lo[0]), sv = std::max(hi[1] - cv, cv - lo[1]);
        std::vector<SectionPoint> fitted;
        for (const P2& q : capPoints(unit)) fitted.push_back({static_cast<float>((q.x - cu) / su), static_cast<float>((q.y - cv) / sv)});
        name = newShapeName(h);
        try {
            h.setOwnedTubeShape(name, fitted, false, true);
        } catch (const std::invalid_argument&) {
            return false;
        }
        return true;
    };
    if (!fit(gapped) && !fit(polygon)) return kNoGuide;
    // Its sections: the source's, scaled to the slice (its gap already in the polygon).
    const double inset = 1.0;
    TubeProfile tube = source.tube;
    tube.joinScale = 1.0f;
    tube.custom = h.tubeShape(name);
    tube.midShape.reset();
    tube.tipShape.reset();
    for (float& x : tube.width) x = static_cast<float>(x * su * inset);
    for (float& a : tube.aspect) a = static_cast<float>(clampAspect(a * sv / su));
    tube.rollOnScalp = false;
    tube.roll.clear();
    // Its guide: a slice of the hierarchy in its parent's sections all along, its root section its
    // part of the parent's, as GuideHierarchy's resolveFollower keeps it; a cell of a cut rooted on
    // the scalp, then its part of the source's tube as the tube is drawn (slicePath).
    std::vector<Vec3f> path;
    if (into != kNoGuide) {
        const TubeSpine spine = sourceSpine(source, h.scale());
        const std::size_t n = source.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            path.push_back(spine.at(static_cast<float>(i) / static_cast<float>(n - 1), static_cast<float>(cu), static_cast<float>(cv)));
        }
    } else {
        path = slicePath(scalp, source, static_cast<float>(cu), static_cast<float>(cv), h.scale());
    }
    if (deviation) {  // (grooming kept: the shape the guides near it were given, on top)
        for (std::size_t i = 1; i < path.size(); ++i) path[i] = path[i] + deviation(static_cast<float>(i) / static_cast<float>(path.size() - 1));
    }
    GuideId id = into;
    if (id == kNoGuide) {
        id = source.parent != kNoGuide ? h.addChildGuide(source.group, source.parent, path, tube) : h.addGuide(source.group, path);
    } else {
        h.setPoints(id, path);
    }
    h.setTube(id, tube);
    if (into != kNoGuide && source.ring.size() >= 3 && source.ring.size() == source.cage.size()) {
        // (A level's slice cuts its root too: its part of the parent's root loop, as cut (no gap: the
        // halves share the cut's CVs and seal), its root's CVs. It is a root of its own then.)
        std::vector<Vec3f> cvs;
        for (const SectionPoint& q : polygon) cvs.push_back(scalp.closestPoint(cageMap(source.cage, source.ring, q)).position);
        if (cvs.size() >= 3) h.setRootOutline(id, bindOutline(scalp, toV(cvs)));
    }
    h.evaluate(&scalp);
    if (into != kNoGuide) return id;  // (a child: its parent lays its footprint and turns its sections)
    // A guide of its own (a cell of a cut): its footprint laid as the source's, its sections turned as
    // the source's (acrossTurn, rollOffsets), its root's CVs kept.
    const HierarchyGuide* g = h.guide(id);
    if (!g || !scalp.contains(g->root)) return id;
    const Vec3f nrm = scalp.normal(g->root);
    const Vec3f own = scalp.acrossAt(g->root);
    Vec3f want = source.across - nrm * source.across.dot(nrm);
    float turn = 0.0f;
    if (own.length() > 0.5f && want.length() > 1e-4f) {
        want = want.normalized();
        turn = std::atan2(nrm.dot(own.cross(want)), own.dot(want));
    }
    h.setRootFrame(id, turn, {});
    h.evaluate(&scalp);
    g = h.guide(id);
    // Its sections against the source's at each point (as a slice matches its parent's), kept as
    // offsets from the roll its footprint lays (so they follow as it is combed).
    const std::vector<float> matched = rollToMatch(g->points, g->frameUp, source.points, source.frameUp, source.tube);
    const float laid = g->tube.roll.empty() ? 0.0f : g->tube.roll[0];
    std::vector<float> offsets;
    for (const float r : matched) offsets.push_back(r - laid);
    h.setRootFrame(id, turn, std::move(offsets));
    if (outline.size() >= 3) h.setRootOutline(id, bindOutline(scalp, toV(outline)));
    h.evaluate(&scalp);
    return id;
}

}  // namespace groomlab
