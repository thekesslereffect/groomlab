// SPDX-License-Identifier: Apache-2.0
// Root-first tubes and the tube hierarchy (root_tubes.h): a port of the Maya Tube tool's Python
// (root_tubes.py, tube_mesh.py's section fitting) to the core, so every host draws, edits and
// splits root tubes alike. The math runs in doubles, as the Python did.

#include "groomlab/root_tubes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "groomlab/frames.h"
#include "groomlab/tube.h"

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

struct P2 {
    double x = 0, y = 0;
};
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

Pts seat(Pts pts, const V& normal) {
    if (pts.size() < 2) return pts;
    const V first = pts[1] - pts[0];
    pts[1] = pts[0] + leaned(first, normal) * first.length();
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
    Pts drawn = path;
    drawn[0] = root;
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

// ---- Section fitting (tube_mesh.py) ---------------------------------------------------------------------

double signedArea(const std::vector<P2>& pts) {
    double a = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const P2& p = pts[i];
        const P2& q = pts[(i + 1) % pts.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

// A section's points as a shape's control points: counter-clockwise, at most kMaxTubeShapePoints.
std::vector<P2> shapePoints(const std::vector<P2>& points) {
    const std::size_t n = points.size(), limit = kMaxTubeShapePoints;
    std::vector<std::size_t> indices(n);
    for (std::size_t i = 0; i < n; ++i) indices[i] = i;
    if (n > limit) {
        std::set<std::size_t> picked;
        for (std::size_t i = 0; i < limit; ++i) {
            picked.insert(static_cast<std::size_t>(std::lround(static_cast<double>(i) * n / static_cast<double>(limit))) % n);
        }
        indices.assign(picked.begin(), picked.end());
    }
    std::vector<P2> pts;
    for (const std::size_t i : indices) pts.push_back(points[i]);
    if (signedArea(pts) < 0.0) std::reverse(pts.begin(), pts.end());
    return pts;
}

struct Fit {
    double n = 0, mu = 0, mv = 0, suu = 0, svv = 0, sx = 0, sy = 0, xu = 0, yu = 0, xv = 0, yv = 0, sxx = 0, syy = 0, sxy = 0;
    Fit(const std::vector<P2>& points, const std::vector<P2>& shape) {
        n = static_cast<double>(points.size());
        for (const P2& p : shape) mu += p.x, mv += p.y;
        mu /= n;
        mv /= n;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double du = shape[i].x - mu, dv = shape[i].y - mv;
            const P2& p = points[i];
            suu += du * du;
            svv += dv * dv;
            sx += p.x;
            sy += p.y;
            xu += p.x * du;
            yu += p.y * du;
            xv += p.x * dv;
            yv += p.y * dv;
            sxx += p.x * p.x;
            syy += p.y * p.y;
            sxy += p.x * p.y;
        }
    }
};

struct FitAt {
    double error, w, h, ox, oy;
};
FitAt fitAt(double angle, const Fit& f) {
    const double c = std::cos(-angle), s = std::sin(-angle);
    const double mqx = (f.sx * c - f.sy * s) / f.n;
    const double mqy = (f.sx * s + f.sy * c) / f.n;
    const double qxx = c * c * f.sxx - 2.0 * c * s * f.sxy + s * s * f.syy - f.n * mqx * mqx;
    const double qyy = s * s * f.sxx + 2.0 * c * s * f.sxy + c * c * f.syy - f.n * mqy * mqy;
    const double cxu = c * f.xu - s * f.yu;
    const double cyv = s * f.xv + c * f.yv;
    const double w = f.suu > 1e-18 ? std::max(cxu / f.suu, 0.0) : 0.0;
    const double h = f.svv > 1e-18 ? std::max(cyv / f.svv, 0.0) : 0.0;
    const double ex = std::max(qxx - w * cxu, 0.0), ey = std::max(qyy - h * cyv, 0.0);
    return {ex + ey, w, h, mqx - w * f.mu, mqy - h * f.mv};
}

struct Section {
    double twist = 0, width = 0, height = 0;
    P2 offset;
};
// The section that best puts `shape`'s points on `points` (each its own, in order), in the
// points' axes at twist 0 (fit_section).
Section fitSection(const std::vector<P2>& points, const std::vector<P2>& shape) {
    double spread = 0.0, mx = 0.0, my = 0.0;
    for (const P2& p : points) {
        spread = std::max(spread, std::hypot(p.x, p.y));
        mx += p.x;
        my += p.y;
    }
    mx /= static_cast<double>(points.size());
    my /= static_cast<double>(points.size());
    double most = 0.0;
    for (const P2& p : points) most = std::max(most, std::hypot(p.x - mx, p.y - my));
    if (most < 1e-9 * std::max(spread, 1.0)) return {0.0, 0.0, 0.0, {mx, my}};
    const Fit fit(points, shape);
    const int steps = 120;
    const double step = 2.0 * kPi / steps;
    double bestAngle = 0.0, bestError = std::numeric_limits<double>::infinity();
    for (int i = 0; i < steps; ++i) {
        const FitAt f = fitAt(step * i, fit);
        if (f.error < bestError) bestError = f.error, bestAngle = step * i;
    }
    double lo = bestAngle - step, hi = bestAngle + step;
    const double golden = 0.5 * (std::sqrt(5.0) - 1.0);
    for (int it = 0; it < 30; ++it) {
        const double a1 = hi - golden * (hi - lo), a2 = lo + golden * (hi - lo);
        if (fitAt(a1, fit).error < fitAt(a2, fit).error) {
            hi = a2;
        } else {
            lo = a1;
        }
    }
    const double angle = 0.5 * (lo + hi);
    const FitAt f = fitAt(angle, fit);
    const double c = std::cos(angle), s = std::sin(angle);
    return {angle, f.w, f.h, {f.ox * c - f.oy * s, f.ox * s + f.oy * c}};
}

std::vector<P2> placeOutline(const std::vector<SectionPoint>& outline, double twist, double width, double height, const P2& center) {
    const double c = std::cos(twist), s = std::sin(twist);
    std::vector<P2> out;
    for (const SectionPoint& p : outline) {
        out.push_back({center.x + p.u * width * c - p.v * height * s, center.y + p.u * width * s + p.v * height * c});
    }
    return out;
}

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
        const double off = std::hypot(s.offset.x, s.offset.y);
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
        const Vec3f across = scalp.acrossAt(g->root);
        if (across.length() > 0.5f) return {V(at), V(across), V(nrm.cross(across)), V(nrm)};
    }
    // Its tube's first section frame, turned onto the scalp's normal there.
    const std::size_t n = g->points.size();
    V u(1, 0, 0), v(0, 0, 1);
    if (n >= 2) {
        const std::vector<Frame> frames = rotationMinimizingFrames(g->points.data(), n, g->frameUp);
        const float roll = g->tube.rollAt(0.0f);
        const float c = std::cos(roll), s = std::sin(roll);
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
    h.setTubeShape(shape, toSection(pts), false);
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
    const std::string wanted = name.empty() ? kRootTubeGroup : name;
    const GroupId found = h.findGroup(wanted);
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
    return !g.isChild() && g.tube.custom && g.tube.custom->name.rfind(kOwnedTubeShapePrefix, 0) == 0;
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

ScalpPointNormal scalpPointNormal(const ScalpMesh& scalp, const Vec3f& p) {
    const auto [q, n] = Surface{&scalp}(V(p));
    return {q.f(), n.f()};
}

// ---- Root tubes --------------------------------------------------------------------------------------------

GuideId makeRootTube(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<Vec3f>& given, const std::vector<Vec3f>& drawnPath,
                     const RootTubeOptions& o) {
    const Surface surface{&scalp};
    const Pts outline = projectAll(scalp, checkedOutline(given));
    auto [root, normal] = surface(centroidOf(outline));
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
        if (std::hypot(s.offset.x, s.offset.y) <= 1e-5 * std::max(s.width, 1e-9)) break;
        Footprint fp;
        rootFit(h, scalp, id, outline, &fp);
        const auto [movedRoot, movedNormal] = surface(fromPlane(s.offset, fp.center, fp.u, fp.v));
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
    auto [root, normal] = surface(centroidOf(outline));
    V move = root - old[0];
    Pts moved{root};
    for (std::size_t i = 1; i < old.size(); ++i) moved.push_back(old[i] + move);
    moved = seat(moved, normal);
    reshape(h, scalp, id, moved);
    Section s = fitShape(h, scalp, id, shape, outline);
    for (int pass = 0; pass < kRecenterPasses; ++pass) {
        if (std::hypot(s.offset.x, s.offset.y) <= 1e-5 * std::max(s.width, 1e-9)) break;
        Footprint fp;
        rootFit(h, scalp, id, outline, &fp);
        const auto [root2, normal2] = surface(fromPlane(s.offset, fp.center, fp.u, fp.v));
        move = root2 - moved[0];
        Pts next{root2};
        for (std::size_t i = 1; i < moved.size(); ++i) next.push_back(moved[i] + move);
        moved = seat(next, normal2);
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
            while (g && g->isChild()) g = h.guide(g->parent);
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
    const std::vector<P2> flat = placeOutline(g->tube.custom->points, g->tube.twist.empty() ? 0.0 : g->tube.twist[0], w, ht, {});
    Pts world;
    for (const P2& q : flat) world.push_back(fromPlane(q, fp.center, fp.u, fp.v));
    return toF(onScalp ? projectAlong(scalp, world, fp.n) : world);
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
    return scalpPointNormal(scalp, outlineCentroid(rootTubeOutline(h, scalp, id)));
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
    const auto [start, n0] = surface(startAt ? V(*startAt) : centroidOf(toV(outline)));
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
                    h.setTubeShape(name, old->tube.custom->points, old->tube.custom->smooth);
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
// its root shape cut in two across its longer side through its middle, each child taking one half
// as a root shape of its own, its guide the path the half's middle takes inside the parent's tube
// (from the parent's footprint on the scalp), its sections the parent's scaled to the half. So the
// two fill the parent's tube as if it were sliced lengthwise: they meet at the cut and never pass
// through each other unless groomed so. Left as generateChildren made them when the parent has no
// shape to cut.
void sliceChildren(GuideHierarchy& h, const ScalpMesh& scalp, GuideId parentId, std::vector<GuideId> kids) {
    const HierarchyGuide* parent = h.guide(parentId);
    if (!parent || kids.size() != 2 || !parent->tube.custom || parent->points.size() < 2 || !scalp.contains(parent->root)) return;
    const TubeProfile ptube = parent->tube;
    const std::vector<Vec3f> ppoints = parent->points;
    const Vec3f pframeUp = parent->frameUp;
    const StrandRoot proot = parent->root;
    const double w0 = std::max(1e-9, static_cast<double>(ptube.widthAt(0.0f)));
    const double h0 = w0 * std::max(1e-6, static_cast<double>(ptube.aspectAt(0.0f)));
    // The root shape in scene units (the section's width across, its height up), its middle and longer way.
    std::vector<P2> shape;
    for (const SectionPoint& q : ptube.custom->points) shape.push_back({q.u * w0, q.v * h0});
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

    TubeSpine spine(ppoints.data(), ppoints.size(), pframeUp, ptube, h.scale());
    spine.setRootOnScalp(scalp.normal(proot), scalp.acrossAt(proot));
    const std::size_t n = ppoints.size();
    std::sort(kids.begin(), kids.end());
    for (int k = 0; k < 2; ++k) {
        const GuideId kid = kids[static_cast<std::size_t>(k)];
        if (halves[k].size() < 3 || !h.guide(kid)) continue;
        // The half in the parent's section coordinates, its box, and its points fitted to it.
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        std::vector<P2> unit;
        for (const P2& q : halves[k]) {
            const P2 uv{q.x / w0, q.y / h0};
            unit.push_back(uv);
            lo[0] = std::min(lo[0], uv.x), lo[1] = std::min(lo[1], uv.y), hi[0] = std::max(hi[0], uv.x), hi[1] = std::max(hi[1], uv.y);
        }
        const double cu = 0.5 * (lo[0] + hi[0]), cv = 0.5 * (lo[1] + hi[1]);
        const double su = std::max(1e-6, 0.5 * (hi[0] - lo[0])), sv = std::max(1e-6, 0.5 * (hi[1] - lo[1]));
        std::vector<SectionPoint> fitted;
        for (const P2& q : capPoints(unit)) fitted.push_back({static_cast<float>((q.x - cu) / su), static_cast<float>((q.y - cv) / sv)});
        const std::string name = newShapeName(h);
        try {
            h.setTubeShape(name, fitted, false);
        } catch (const std::invalid_argument&) {
            continue;  // (a sliver no shape takes: generateChildren's child stays)
        }
        TubeProfile tube = ptube;
        tube.custom = h.tubeShape(name);
        tube.midShape.reset();
        tube.tipShape.reset();
        for (float& x : tube.width) x = static_cast<float>(x * su);
        for (float& a : tube.aspect) a = static_cast<float>(clampAspect(a * sv / su));
        tube.rollOnScalp = false;
        tube.roll.clear();
        h.setTube(kid, tube);
        std::vector<Vec3f> path;
        const float u = static_cast<float>(cu), v = static_cast<float>(cv);
        for (std::size_t i = 0; i < n; ++i) {
            path.push_back(i == 0 ? scalp.closestPoint(spine.footprintAt(u, v)).position
                                  : spine.at(static_cast<float>(i) / static_cast<float>(n - 1), u, v));
        }
        h.setPoints(kid, path);
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
            for (const GuideId p : parents) {
                const HierarchyGuide* g = h.guide(p);
                if (g && isRootTube(g->tube, 0)) sliceChildren(h, *scalp, p, h.childrenOf(p));
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

std::map<std::string, bool> tubeLevelVisibility(const GuideHierarchy& h, const std::string& base, int level) {
    const TubeLevels info = tubeLevels(h, {}, base);
    if (info.levels.empty()) throw std::invalid_argument("no tube hierarchy on " + base);
    level = std::clamp(level, 1, static_cast<int>(info.levels.size()));
    std::map<std::string, bool> out;
    for (std::size_t k = 0; k < info.levels.size(); ++k) out[info.levels[k]] = static_cast<int>(k) + 1 == level;
    return out;
}

}  // namespace groomlab
