// SPDX-License-Identifier: Apache-2.0
#include "groomlab/pin_morph.h"

#include <algorithm>
#include <cmath>

namespace groomlab {

namespace {

CameraVec3 sub(const CameraVec3& a, const CameraVec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
CameraVec3 add(const CameraVec3& a, const CameraVec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
CameraVec3 scale(const CameraVec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const CameraVec3& a, const CameraVec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const CameraVec3& a) { return std::sqrt(dot(a, a)); }

CameraVec3 column(const CameraMat3& m, int j) { return {m[0][j], m[1][j], m[2][j]}; }

bool usableView(const PinView& v) { return v.focalPx > 0.0 && v.width > 0.0 && v.height > 0.0; }

// The ray through pixel (u, v): its direction in world space, scaled so a step of 1 is a unit of
// depth down the camera's -Z.
CameraVec3 pixelRay(const PinView& view, double u, double v) {
    const double x = (u - 0.5 * view.width) / view.focalPx;
    const double y = -(v - 0.5 * view.height) / view.focalPx;
    const CameraMat3& r = view.pose.rotation;
    return add(add(scale(column(r, 0), x), scale(column(r, 1), y)), scale(column(r, 2), -1.0));
}

// Solves a x = b in place (n x n, `rhs` columns), partial pivoting. False when singular.
bool solveDense(std::vector<double>& a, std::vector<double>& b, std::size_t n, std::size_t rhs) {
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::abs(a[r * n + c]) > std::abs(a[pivot * n + c])) pivot = r;
        if (std::abs(a[pivot * n + c]) < 1e-12) return false;
        if (pivot != c) {
            for (std::size_t k = 0; k < n; ++k) std::swap(a[c * n + k], a[pivot * n + k]);
            for (std::size_t k = 0; k < rhs; ++k) std::swap(b[c * rhs + k], b[pivot * rhs + k]);
        }
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            if (f == 0.0) continue;
            for (std::size_t k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            for (std::size_t k = 0; k < rhs; ++k) b[r * rhs + k] -= f * b[c * rhs + k];
        }
    }
    for (std::size_t c = n; c-- > 0;) {
        for (std::size_t k = 0; k < rhs; ++k) {
            double s = b[c * rhs + k];
            for (std::size_t j = c + 1; j < n; ++j) s -= a[c * n + j] * b[j * rhs + k];
            b[c * rhs + k] = s / a[c * n + c];
        }
    }
    return true;
}

// The radial basis warp: weights per pin and the polynomial part (constant, or affine: 4 terms).
struct Warp {
    std::vector<CameraVec3> centers;  // normalized
    std::vector<double> weights;      // per center x 3
    std::vector<double> poly;         // terms x 3
    std::size_t terms = 0;
    CameraVec3 origin{0, 0, 0};
    double unit = 1.0;

    CameraVec3 norm(const CameraVec3& p) const { return scale(sub(p, origin), 1.0 / unit); }

    CameraVec3 move(const CameraVec3& p) const {
        const CameraVec3 q = norm(p);
        CameraVec3 d{0, 0, 0};
        for (std::size_t i = 0; i < centers.size(); ++i) {
            const double phi = length(sub(q, centers[i]));
            for (int k = 0; k < 3; ++k) d[k] += weights[i * 3 + k] * phi;
        }
        const double basis[4] = {1.0, q[0], q[1], q[2]};
        for (std::size_t t = 0; t < terms; ++t)
            for (int k = 0; k < 3; ++k) d[k] += poly[t * 3 + k] * basis[t];
        return scale(d, unit);  // (the moves were normalized too)
    }
};

bool buildWarp(const std::vector<CameraVec3>& from, const std::vector<CameraVec3>& moves, double stiffness,
               std::size_t terms, Warp& w) {
    const std::size_t n = from.size();
    CameraVec3 lo = from[0], hi = from[0];
    for (const CameraVec3& p : from)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    w.origin = scale(add(lo, hi), 0.5);
    w.unit = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-9});
    w.terms = terms;
    w.centers.clear();
    for (const CameraVec3& p : from) w.centers.push_back(w.norm(p));
    // Stiffness as a ridge on the kernel: 0 interpolates, toward 1 the pins pull ever more softly.
    const double s = std::clamp(stiffness, 0.0, 0.999);
    const double ridge = s / (1.0 - s);
    const std::size_t m = n + terms;
    std::vector<double> a(m * m, 0.0), b(m * 3, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) a[i * m + j] = length(sub(w.centers[i], w.centers[j]));
        // Minus: the kernel |r| is negative definite on the weights the shift leaves free, so a ridge
        // added to it crossed zero somewhere between stiffness 0 and 1 (a near-singular solve: the
        // mesh blew up between the ends); subtracted, it only ever moves away from zero.
        a[i * m + i] -= ridge;
        const double basis[4] = {1.0, w.centers[i][0], w.centers[i][1], w.centers[i][2]};
        for (std::size_t t = 0; t < terms; ++t) a[i * m + n + t] = a[(n + t) * m + i] = basis[t];
        for (int k = 0; k < 3; ++k) b[i * 3 + k] = moves[i][k] / w.unit;
    }
    if (!solveDense(a, b, m, 3)) return false;
    w.weights.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n * 3));
    w.poly.assign(b.begin() + static_cast<std::ptrdiff_t>(n * 3), b.end());
    return true;
}

double rmsPx(const std::vector<MorphPin>& pins, const std::vector<CameraVec3>& at, const std::vector<PinView>& views) {
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < pins.size(); ++i) {
        for (const PinSight& sight : pins[i].sights) {
            if (sight.view < 0 || static_cast<std::size_t>(sight.view) >= views.size()) continue;
            const PinView& v = views[static_cast<std::size_t>(sight.view)];
            double u = 0.0, vv = 0.0;
            if (!usableView(v) || !projectToImage(v.pose, v.focalPx, v.width, v.height, at[i], u, vv)) continue;
            sum += (u - sight.u) * (u - sight.u) + (vv - sight.v) * (vv - sight.v);
            ++count;
        }
    }
    return count ? std::sqrt(sum / count) : 0.0;
}

}  // namespace

bool pinTarget(const MorphPin& pin, const std::vector<PinView>& views, CameraVec3& target) {
    std::vector<std::pair<CameraVec3, CameraVec3>> rays;  // (origin, direction scaled to unit depth)
    for (const PinSight& sight : pin.sights) {
        if (sight.view < 0 || static_cast<std::size_t>(sight.view) >= views.size()) continue;
        const PinView& v = views[static_cast<std::size_t>(sight.view)];
        if (!usableView(v)) continue;
        rays.push_back({v.pose.position, pixelRay(v, sight.u, sight.v)});
    }
    if (rays.empty()) {
        if (!pin.sights.empty()) return false;
        target = pin.mesh;  // no sight: an anchor where it is
        return true;
    }
    if (rays.size() == 1) {
        // One photo: the pin's own depth down that camera's view, on the photo point's ray.
        const PinView* view = nullptr;
        for (const PinSight& sight : pin.sights)
            if (sight.view >= 0 && static_cast<std::size_t>(sight.view) < views.size() && usableView(views[static_cast<std::size_t>(sight.view)]))
                view = &views[static_cast<std::size_t>(sight.view)];
        const double depth = dot(sub(pin.mesh, view->pose.position), scale(column(view->pose.rotation, 2), -1.0));
        if (!(depth > 0.0)) return false;
        target = add(rays[0].first, scale(rays[0].second, depth));
        return true;
    }
    // Two photos or more: the point nearest every ray, sum (I - d d^T)(p - o) = 0.
    std::vector<double> a(9, 0.0), b(3, 0.0);
    for (const auto& [o, raw] : rays) {
        const CameraVec3 d = scale(raw, 1.0 / length(raw));
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const double m = (r == c ? 1.0 : 0.0) - d[r] * d[c];
                a[r * 3 + c] += m;
                b[r] += m * o[c];
            }
        }
    }
    if (!solveDense(a, b, 3, 1)) return false;
    target = {b[0], b[1], b[2]};
    return true;
}

PinMorphResult morphToPins(const std::vector<CameraVec3>& points, const std::vector<MorphPin>& pins,
                           const std::vector<PinView>& views, const PinMorphOptions& options) {
    PinMorphResult out;
    out.points = points;
    if (pins.empty()) {
        out.error = "no pins: place pins on the mesh and on the photos first";
        return out;
    }
    std::vector<CameraVec3> from, moves;
    out.targets.resize(pins.size());
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (!pinTarget(pins[i], views, out.targets[i])) {
            out.error = "pin " + std::to_string(i + 1) + " has no usable photo point (behind its camera, or its photo not lined up)";
            return out;
        }
        from.push_back(pins[i].mesh);
        moves.push_back(sub(out.targets[i], pins[i].mesh));
    }
    out.rmsBeforePx = rmsPx(pins, from, views);
    if (options.symmetric && options.axis >= 0 && options.axis < 3) {
        // Mirror images, the plane's pins held on it (the reported targets are the given pins').
        const int a = options.axis;
        CameraVec3 lo = from[0], hi = from[0];
        for (const CameraVec3& p : from)
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-9});
        const double onPlane = 0.01 * extent;
        const std::size_t given = from.size();
        for (std::size_t i = 0; i < given; ++i) {
            if (std::abs(from[i][a]) <= onPlane) {
                moves[i][a] = -from[i][a];  // stays on the plane
                continue;
            }
            CameraVec3 mirror = from[i];
            mirror[a] = -mirror[a];
            bool taken = false;
            for (std::size_t j = 0; j < given && !taken; ++j) taken = j != i && length(sub(from[j], mirror)) <= onPlane;
            if (taken) continue;  // the other side has a pin of its own
            CameraVec3 move = moves[i];
            move[a] = -move[a];
            from.push_back(mirror);
            moves.push_back(move);
        }
    }
    // The polynomial part a plain shift: an affine one let stiffness hand the pins' local moves to a
    // whole-mesh stretch and shear (a head distorted at 0.5); with a shift, stiffness eases toward
    // moving the mesh as one piece, its shape kept.
    Warp warp;
    const bool built = buildWarp(from, moves, options.stiffness, 1, warp);
    if (!built) {
        out.error = "the pins could not be solved: two pins on the same mesh point?";
        return out;
    }
    for (CameraVec3& p : out.points) p = add(p, warp.move(p));
    std::vector<CameraVec3> after;
    for (std::size_t i = 0; i < pins.size(); ++i) after.push_back(add(from[i], warp.move(from[i])));
    out.rmsAfterPx = rmsPx(pins, after, views);
    out.ok = true;
    return out;
}

}  // namespace groomlab
