// SPDX-License-Identifier: Apache-2.0
// Guides fitted to strokes on several photos: screen-space residuals, a rod, gravity, the head and
// the hair's shell, solved per guide by Levenberg-Marquardt on a banded matrix. See guide_fit.h.

#include "groomlab/guide_fit.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

#include "groomlab/parallel.h"

namespace groomlab {

namespace {

// ---- Small doubles ---------------------------------------------------------------------------------

struct D3 {
    double x = 0.0, y = 0.0, z = 0.0;
    D3() = default;
    D3(double a, double b, double c) : x(a), y(b), z(c) {}
    explicit D3(const Vec3f& v) : x(v.x), y(v.y), z(v.z) {}
    Vec3f f() const { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }
    D3 operator+(const D3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    D3 operator-(const D3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    D3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const D3& o) const { return x * o.x + y * o.y + z * o.z; }
    double length() const { return std::sqrt(dot(*this)); }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

double smoothstep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ---- A stroke's curve: a quadratic open-uniform B-spline through it, sampled densely -------------------

struct Curve2 {
    std::vector<std::array<double, 2>> p;  // dense samples, root first
    std::vector<double> s;                 // arc length at each
    double length = 0.0;
};

std::vector<std::array<double, 2>> resampledPx(const std::vector<std::array<float, 2>>& in, double spacing) {
    std::vector<std::array<double, 2>> pts;
    for (const auto& q : in) {
        const std::array<double, 2> d{q[0], q[1]};
        if (pts.empty() || std::hypot(d[0] - pts.back()[0], d[1] - pts.back()[1]) > 1e-9) pts.push_back(d);
    }
    if (pts.size() < 2) return pts;
    std::vector<double> cum(1, 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i) cum.push_back(cum.back() + std::hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
    const double total = cum.back();
    const int count = std::max(3, static_cast<int>(std::round(total / std::max(1e-9, spacing))) + 1);
    std::vector<std::array<double, 2>> out;
    std::size_t k = 0;
    for (int i = 0; i < count; ++i) {
        const double at = total * static_cast<double>(i) / static_cast<double>(count - 1);
        while (k + 2 < cum.size() && cum[k + 1] < at) ++k;
        const double span = cum[k + 1] - cum[k];
        const double t = span > 1e-12 ? std::clamp((at - cum[k]) / span, 0.0, 1.0) : 0.0;
        out.push_back({pts[k][0] + (pts[k + 1][0] - pts[k][0]) * t, pts[k][1] + (pts[k + 1][1] - pts[k][1]) * t});
    }
    return out;
}

Curve2 strokeCurve(const FitStroke& st, float spacingShare) {
    const double side = std::max(st.camera.width, st.camera.height);
    const std::vector<std::array<double, 2>> c = resampledPx(st.pixels, std::max(1.0, static_cast<double>(spacingShare) * side));
    Curve2 out;
    if (c.size() < 2) {
        if (!c.empty()) out.p = {c[0], c[0]};
        out.s = {0.0, 0.0};
        return out;
    }
    const std::size_t n = c.size() - 1, spans = n - 1;
    constexpr int kPerSpan = 8;
    auto mid = [](const std::array<double, 2>& a, const std::array<double, 2>& b) { return std::array<double, 2>{0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1])}; };
    for (std::size_t j = 0; j < spans; ++j) {
        const auto a = j == 0 ? c[0] : mid(c[j], c[j + 1]);
        const auto b = c[j + 1];
        const auto e = j + 1 == spans ? c[n] : mid(c[j + 1], c[j + 2]);
        for (int k = 0; k < kPerSpan; ++k) {
            const double t = static_cast<double>(k) / kPerSpan, s = 1.0 - t;
            out.p.push_back({s * s * a[0] + 2.0 * s * t * b[0] + t * t * e[0], s * s * a[1] + 2.0 * s * t * b[1] + t * t * e[1]});
        }
    }
    out.p.push_back(c[n]);
    out.s.assign(out.p.size(), 0.0);
    for (std::size_t i = 1; i < out.p.size(); ++i) out.s[i] = out.s[i - 1] + std::hypot(out.p[i][0] - out.p[i - 1][0], out.p[i][1] - out.p[i - 1][1]);
    out.length = out.s.back();
    return out;
}

struct Near {
    double dist = 0.0;
    double qx = 0.0, qy = 0.0;  // the nearest place
    double tx = 1.0, ty = 0.0;  // the curve's direction there (root to tip)
    double alpha = 0.0;         // its share of the curve's length
    bool interior = false;      // not one of the curve's ends
};

Near nearest(const Curve2& c, double x, double y) {
    Near best;
    double bestD2 = std::numeric_limits<double>::infinity();
    const std::size_t last = c.p.size() - 1;
    for (std::size_t i = 0; i < last; ++i) {
        const double ax = c.p[i][0], ay = c.p[i][1], bx = c.p[i + 1][0] - ax, by = c.p[i + 1][1] - ay;
        const double l2 = bx * bx + by * by;
        const double t = l2 > 1e-18 ? std::clamp(((x - ax) * bx + (y - ay) * by) / l2, 0.0, 1.0) : 0.0;
        const double qx = ax + bx * t, qy = ay + by * t;
        const double d2 = (x - qx) * (x - qx) + (y - qy) * (y - qy);
        if (d2 < bestD2) {
            bestD2 = d2;
            best.qx = qx;
            best.qy = qy;
            const double l = std::sqrt(l2);
            if (l > 1e-12) {
                best.tx = bx / l;
                best.ty = by / l;
            }
            best.alpha = c.length > 0.0 ? (c.s[i] + t * (c.s[i + 1] - c.s[i])) / c.length : 0.0;
            best.interior = !((i == 0 && t <= 0.0) || (i + 1 == last && t >= 1.0));
        }
    }
    best.dist = std::sqrt(bestD2);
    return best;
}

// ---- Projection and its derivative ------------------------------------------------------------------

struct Proj {
    bool ok = false;
    double u = 0.0, v = 0.0, z = 0.0;
    D3 du, dv;  // d(u, v) / d(point)
};

Proj projected(const RefCamera& cam, const D3& p) {
    Proj out;
    const D3 d = p - D3(cam.pose.position[0], cam.pose.position[1], cam.pose.position[2]);
    const D3 X(cam.pose.rotation[0][0], cam.pose.rotation[1][0], cam.pose.rotation[2][0]);
    const D3 Y(cam.pose.rotation[0][1], cam.pose.rotation[1][1], cam.pose.rotation[2][1]);
    const D3 Z(cam.pose.rotation[0][2], cam.pose.rotation[1][2], cam.pose.rotation[2][2]);
    const double c0 = X.dot(d), c1 = Y.dot(d), z = -Z.dot(d);
    if (z <= 1e-9) return out;
    const double f = cam.focalPx;
    out.ok = true;
    out.z = z;
    out.u = 0.5 * cam.width + cam.offsetU + f * c0 / z;
    out.v = 0.5 * cam.height + cam.offsetV - f * c1 / z;
    out.du = (X + Z * (c0 / z)) * (f / z);
    out.dv = (Y + Z * (c1 / z)) * (-f / z);
    return out;
}

// A photo's pixel as head radii at the head's middle's depth.
double pixelShare(const RefCamera& cam, const Vec3f& center, double R) {
    const Proj p = projected(cam, D3(center));
    const double z = p.ok ? p.z : (D3(center) - D3(cam.eye())).length();
    return z / std::max(1e-9, cam.focalPx) / R;
}

float outwardSign(const ScalpMesh& mesh) {
    if (mesh.pointCount() == 0 || mesh.vertexNormals().size() != mesh.pointCount()) return 1.0f;
    Vec3f center(0, 0, 0);
    for (const Vec3f& q : mesh.points()) center += q;
    center = center / static_cast<float>(mesh.pointCount());
    double out = 0.0;
    for (std::size_t i = 0; i < mesh.pointCount(); ++i) out += static_cast<double>((mesh.points()[i] - center).dot(mesh.vertexNormals()[i]));
    return out >= 0.0 ? 1.0f : -1.0f;
}

// Whether the photo sees the point: in its frame (a little past it is fine) and the head not before it.
bool sees(const RefCamera& cam, const D3& p, const ScalpMesh* head, double R) {
    const Proj q = projected(cam, p);
    if (!q.ok) return false;
    const double margin = 0.02 * std::max(cam.width, cam.height);
    if (q.u < -margin || q.v < -margin || q.u > cam.width + margin || q.v > cam.height + margin) return false;
    if (!head) return true;
    const D3 eye(cam.eye());
    const D3 to = p - eye;
    const double dist = to.length();
    if (dist < 1e-9) return false;
    SurfacePoint hit;
    float t = 0.0f;
    if (!head->intersectRay(eye.f(), (to * (1.0 / dist)).f(), &hit, &t)) return true;
    return static_cast<double>(t) > dist - 0.02 * R;
}

// ---- A banded symmetric matrix and its LDL^T solve ------------------------------------------------------

struct Band {
    int n = 0, b = 0;
    std::vector<double> a;  // A(i, i + k), k = 0..b
    Band(int n_, int b_) : n(n_), b(b_), a(static_cast<std::size_t>(n_) * static_cast<std::size_t>(b_ + 1), 0.0) {}
    double& at(int i, int k) { return a[static_cast<std::size_t>(i) * static_cast<std::size_t>(b + 1) + static_cast<std::size_t>(k)]; }
    double get(int i, int j) const {
        if (j < i) std::swap(i, j);
        return j - i > b ? 0.0 : a[static_cast<std::size_t>(i) * static_cast<std::size_t>(b + 1) + static_cast<std::size_t>(j - i)];
    }
    void add(int i, int j, double v) {
        if (j < i) std::swap(i, j);
        if (j - i <= b) at(i, j - i) += v;
    }
};

// Solves (A + damping) x = rhs in place; false when not positive definite.
bool solveBand(const Band& A, double lambda, std::vector<double>& x) {
    const int n = A.n, b = A.b;
    std::vector<double> L(static_cast<std::size_t>(n) * static_cast<std::size_t>(b + 1), 0.0), D(static_cast<std::size_t>(n), 0.0);
    auto l = [&](int i, int j) -> double& { return L[static_cast<std::size_t>(i) * static_cast<std::size_t>(b + 1) + static_cast<std::size_t>(i - j)]; };
    for (int i = 0; i < n; ++i) {
        for (int j = std::max(0, i - b); j <= i; ++j) {
            double s = A.get(j, i);
            if (j == i) s = s * (1.0 + lambda) + 1e-12;
            for (int k = std::max(0, i - b); k < j; ++k) s -= l(i, k) * l(j, k) * D[static_cast<std::size_t>(k)];
            if (j == i) {
                if (!(s > 0.0)) return false;
                D[static_cast<std::size_t>(i)] = s;
            } else {
                l(i, j) = s / D[static_cast<std::size_t>(j)];
            }
        }
    }
    for (int i = 0; i < n; ++i)
        for (int k = std::max(0, i - b); k < i; ++k) x[static_cast<std::size_t>(i)] -= l(i, k) * x[static_cast<std::size_t>(k)];
    for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] /= D[static_cast<std::size_t>(i)];
    for (int i = n - 1; i >= 0; --i)
        for (int k = i + 1; k <= std::min(n - 1, i + b); ++k) x[static_cast<std::size_t>(i)] -= l(k, i) * x[static_cast<std::size_t>(k)];
    return true;
}

// ---- One guide's problem -------------------------------------------------------------------------------

struct View {
    const FitStroke* stroke = nullptr;
    Curve2 curve;
    double share = 0.0;      // a pixel as head radii at the head's depth
    std::vector<char> seen;  // per point (this round)
};

struct Problem {
    const HairShell* shell = nullptr;
    const ScalpMesh* head = nullptr;
    float headSign = 1.0f;
    GuideFitOptions o;
    double R = 1.0, h = 1.0;
    D3 center, up;
    std::vector<View> views;

    // How far a point is out of the head (below 0: inside) and the way out: the head mesh's closest
    // point and normal (within a head radius), else (deep inside, or no mesh) the shell's head along
    // the direction from the middle.
    double headHeight(const D3& p, D3* out = nullptr) const {
        if (head) {
            const SurfacePoint sp = head->closestPoint(p.f(), static_cast<float>(R));
            if (sp.root.face >= 0) {
                const D3 n = D3(sp.normal.normalized() * headSign);
                if (out) *out = n;
                return (p - D3(sp.position)).dot(n);
            }
        }
        const D3 d = p - center;
        const double r = d.length();
        if (out) *out = r > 1e-12 ? d * (1.0 / r) : up;
        return r - static_cast<double>(shell->head(d.f()));
    }

    double shellGap(const D3& p) const {
        const D3 d = p - center;
        const double r = d.length();
        if (r < 1e-9) return -1.0;
        return r - static_cast<double>(shell->outer(d.f()));
    }

    // The energy at x (x[0] fixed); with H and g: the Gauss-Newton matrix J^T J and half the gradient.
    double evaluate(const std::vector<D3>& x, Band* H, std::vector<double>* g) const {
        const int N = static_cast<int>(x.size());
        double E = 0.0;
        struct Entry {
            int vertex;
            D3 j;
        };
        auto residual = [&](double r, std::initializer_list<Entry> entries) {
            E += r * r;
            if (!H) return;
            for (const Entry& a : entries) {
                if (a.vertex <= 0) continue;
                const int ia = 3 * (a.vertex - 1);
                for (int c = 0; c < 3; ++c) (*g)[static_cast<std::size_t>(ia + c)] += a.j[c] * r;
                for (const Entry& bb : entries) {
                    if (bb.vertex <= 0 || bb.vertex < a.vertex) continue;
                    const int ib = 3 * (bb.vertex - 1);
                    for (int c = 0; c < 3; ++c)
                        for (int d = 0; d < 3; ++d) {
                            if (bb.vertex == a.vertex && d < c) continue;
                            H->add(ia + c, ib + d, a.j[c] * bb.j[d]);
                        }
                }
            }
        };
        // stretch
        const double ks = std::sqrt(static_cast<double>(o.stretch));
        for (int i = 0; i + 1 < N; ++i) {
            const D3 e = x[static_cast<std::size_t>(i + 1)] - x[static_cast<std::size_t>(i)];
            const double L = e.length();
            const D3 u = L > 1e-12 ? e * (1.0 / L) : D3(0, 0, 0);
            residual(ks * (L / h - 1.0), {{i + 1, u * (ks / h)}, {i, u * (-ks / h)}});
        }
        // bend
        const double kb = std::sqrt(static_cast<double>(o.bend)) / h;
        for (int i = 1; i + 1 < N; ++i) {
            const D3 s = x[static_cast<std::size_t>(i + 1)] - x[static_cast<std::size_t>(i)] * 2.0 + x[static_cast<std::size_t>(i - 1)];
            for (int c = 0; c < 3; ++c) {
                const D3 e(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0);
                residual(kb * s[c], {{i - 1, e * kb}, {i, e * (-2.0 * kb)}, {i + 1, e * kb}});
            }
        }
        const double gravity = static_cast<double>(o.gravity) / R;
        for (int i = 1; i < N; ++i) {
            const D3& p = x[static_cast<std::size_t>(i)];
            // gravity (linear: its half gradient alone)
            E += gravity * (p - x[0]).dot(up);
            if (H)
                for (int c = 0; c < 3; ++c) (*g)[static_cast<std::size_t>(3 * (i - 1) + c)] += 0.5 * gravity * up[c];
            const double fade = smoothstep01(static_cast<double>(i) * h / (static_cast<double>(o.shellFade) * R));
            // screen
            for (const View& v : views) {
                if (!v.seen[static_cast<std::size_t>(i)]) continue;
                const Proj q = projected(v.stroke->camera, p);
                if (!q.ok) continue;
                const Near nr = nearest(v.curve, q.u, q.v);
                if (!nr.interior) continue;
                const double k = v.share / static_cast<double>(o.screenSigma);
                double nx = q.u - nr.qx, ny = q.v - nr.qy;
                if (nr.dist > 1e-9) {
                    nx /= nr.dist;
                    ny /= nr.dist;
                } else {
                    nx = -nr.ty;
                    ny = nr.tx;
                }
                const D3 j = (q.du * nx + q.dv * ny) * k;
                const double d = k * nr.dist;  // (in screenSigmas)
                const double tau = static_cast<double>(o.screenRobust / o.screenSigma);
                if (tau > 0.0) {
                    // Cauchy: tau^2 ln(1 + (d / tau)^2), as a residual sqrt of it (its gradient exact):
                    // a point far off a stroke (another part of a curly lock, a stroke over part of it)
                    // pulls little
                    const double e = tau * tau * std::log1p((d / tau) * (d / tau));
                    const double r = std::sqrt(e);
                    const double dr = r > 1e-12 ? d / (1.0 + (d / tau) * (d / tau)) / r : 1.0;
                    residual(r, {{i, j * dr}});
                } else {
                    residual(d, {{i, j}});
                }
                // seen: no deeper along the photo's ray than where it first meets the shell (the
                // hair's outside from there), faded in from the root
                if (fade > 0.0) {
                    const D3 eye(v.stroke->camera.eye());
                    const D3 to = p - eye;
                    const double dist = to.length();
                    if (dist > 1e-9) {
                        const D3 dir = to * (1.0 / dist);
                        const double first = static_cast<double>(shell->raycast(eye.f(), dir.f()).t);
                        if (dist > first) {
                            const double kv = fade / (static_cast<double>(o.shellSigma) * R);
                            residual(kv * (dist - first), {{i, dir * kv}});
                        }
                    }
                }
            }
            // head
            {
                D3 n;
                const double height = headHeight(p, &n);
                const double clear = static_cast<double>(o.clearance) * R;
                if (height < clear) {
                    const double k = 1.0 / (static_cast<double>(o.headSigma) * R);
                    residual(k * (height - clear), {{i, n * k}});
                }
            }
            // shell: not out past it
            const double s = shellGap(p);
            if (s > 0.0) {
                D3 grad;
                if (H) {
                    const double eps = 1e-3 * R;
                    double gr[3];
                    for (int c = 0; c < 3; ++c) {
                        const D3 e(c == 0 ? eps : 0.0, c == 1 ? eps : 0.0, c == 2 ? eps : 0.0);
                        gr[c] = (shellGap(p + e) - shellGap(p - e)) / (2.0 * eps);
                    }
                    grad = D3(gr[0], gr[1], gr[2]);
                }
                const double k = 1.0 / (static_cast<double>(o.shellOutSigma) * R);
                residual(k * s, {{i, grad * k}});
            }
        }
        return E;
    }

    void markSeen(const std::vector<D3>& x) {
        for (View& v : views) {
            v.seen.assign(x.size(), 0);
            for (std::size_t i = 0; i < x.size(); ++i) v.seen[i] = sees(v.stroke->camera, x[i], head, R) ? 1 : 0;
        }
    }

    // Levenberg-Marquardt from x; the steps taken.
    int solve(std::vector<D3>& x, double* energy) const {
        const int N = static_cast<int>(x.size());
        if (N < 2) return 0;
        const int n = 3 * (N - 1);
        Band H(n, 8);
        std::vector<double> g(static_cast<std::size_t>(n), 0.0);
        double E = evaluate(x, &H, &g);
        double lambda = 1e-3;
        int taken = 0, small = 0;
        const double maxStep = static_cast<double>(o.maxStep) * R;
        for (int it = 0; it < o.iterations; ++it) {
            std::vector<double> step(g.size());
            for (std::size_t k = 0; k < g.size(); ++k) step[k] = -g[k];
            if (!solveBand(H, lambda, step)) {
                lambda *= 10.0;
                if (lambda > 1e8) break;
                continue;
            }
            double longest = 0.0;
            for (int i = 1; i < N; ++i) {
                const std::size_t k = static_cast<std::size_t>(3 * (i - 1));
                longest = std::max(longest, D3(step[k], step[k + 1], step[k + 2]).length());
            }
            const double scale = longest > maxStep ? maxStep / longest : 1.0;
            std::vector<D3> trial = x;
            for (int i = 1; i < N; ++i) {
                const std::size_t k = static_cast<std::size_t>(3 * (i - 1));
                trial[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)] + D3(step[k], step[k + 1], step[k + 2]) * scale;
            }
            Band H2(n, 8);
            std::vector<double> g2(g.size(), 0.0);
            const double E2 = evaluate(trial, &H2, &g2);
            if (E2 < E) {
                const double drop = E - E2;
                x = std::move(trial);
                H = std::move(H2);
                g = std::move(g2);
                ++taken;
                small = drop < 1e-6 * std::max(1.0, std::abs(E)) ? small + 1 : 0;
                E = E2;
                lambda = std::max(1e-7, lambda / 3.0);
                if (small >= 3) break;
            } else {
                lambda *= 4.0;
                if (lambda > 1e8) break;
            }
        }
        if (energy) *energy = E;
        return taken;
    }
};

std::vector<D3> toD(const std::vector<Vec3f>& v) {
    std::vector<D3> out;
    out.reserve(v.size());
    for (const Vec3f& p : v) out.emplace_back(p);
    return out;
}

std::vector<D3> evenlySpaced(const std::vector<D3>& in, double h) {
    if (in.size() < 2) return in;
    std::vector<double> cum(1, 0.0);
    for (std::size_t i = 1; i < in.size(); ++i) cum.push_back(cum.back() + (in[i] - in[i - 1]).length());
    const double total = cum.back();
    const int count = std::max(2, static_cast<int>(std::round(total / h)) + 1);
    std::vector<D3> out;
    std::size_t k = 0;
    for (int i = 0; i < count; ++i) {
        const double at = total * static_cast<double>(i) / static_cast<double>(count - 1);
        while (k + 2 < cum.size() && cum[k + 1] < at) ++k;
        const double span = cum[k + 1] - cum[k];
        const double t = span > 1e-12 ? std::clamp((at - cum[k]) / span, 0.0, 1.0) : 0.0;
        out.push_back(in[k] + (in[k + 1] - in[k]) * t);
    }
    out.front() = in.front();
    return out;
}

double lengthOf(const std::vector<D3>& x) {
    double s = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) s += (x[i] - x[i - 1]).length();
    return s;
}

struct Coverage {
    std::vector<char> covered;  // per point: some view's stroke covers it
    std::vector<FitViewResidual> views;
    double extend = 0.0;  // scene units the strokes run on past the guide's tip (most of any view)
    double before = 0.0;  // scene units the strokes start before the guide's first covered point
};

Coverage coverage(const Problem& pb, const std::vector<D3>& x) {
    Coverage c;
    c.covered.assign(x.size(), 0);
    for (const View& v : pb.views) {
        FitViewResidual r;
        r.photo = v.stroke->photo;
        r.stroke = v.stroke->stroke;
        double sum = 0.0, aMin = 1.0, aMax = 0.0, hidden = 0.0;
        int covered = 0, near = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (!v.seen[i]) continue;
            const Proj q = projected(v.stroke->camera, x[i]);
            if (!q.ok) continue;
            ++r.points;
            const Near nr = nearest(v.curve, q.u, q.v);
            if (!nr.interior) continue;
            ++covered;
            sum += nr.dist;
            r.maxPx = std::max(r.maxPx, static_cast<float>(nr.dist));
            // (on the stroke: within 3 screenSigma; what roots, grows and trims the guide)
            if (nr.dist * v.share > 3.0 * static_cast<double>(pb.o.screenSigma) && nr.dist > 2.0) continue;
            c.covered[i] = 1;
            aMin = std::min(aMin, nr.alpha);
            aMax = std::max(aMax, nr.alpha);
            // (how far behind the hair's outside from this photo: where its ray first meets the shell)
            const D3 eye(v.stroke->camera.eye());
            const D3 to = x[i] - eye;
            const double dist = to.length();
            if (dist > 1e-9) hidden += std::max(0.0, dist - static_cast<double>(pb.shell->raycast(eye.f(), (to * (1.0 / dist)).f()).t));
            ++near;
        }
        if (covered > 0) {
            r.meanPx = static_cast<float>(sum / covered);
            r.meanR = static_cast<float>(sum / covered * v.share);
            r.maxR = static_cast<float>(r.maxPx * v.share);
            r.covered = r.points > 0 ? static_cast<float>(covered) / static_cast<float>(r.points) : 0.0f;
            r.strokeCovered = static_cast<float>(std::max(0.0, aMax - aMin));
            r.hidden = near > 0 ? static_cast<float>(hidden / near / pb.R) : 0.0f;
            if (aMax >= aMin) {
                c.extend = std::max(c.extend, (1.0 - aMax) * v.curve.length * v.share * pb.R);
                c.before = std::max(c.before, aMin * v.curve.length * v.share * pb.R);
            }
        }
        c.views.push_back(r);
    }
    return c;
}

Problem makeProblem(const std::vector<FitStroke>& strokes, const HairShell& shell, const ScalpMesh* head, const GuideFitOptions& o) {
    Problem pb;
    pb.shell = &shell;
    pb.head = head;
    pb.headSign = head ? outwardSign(*head) : 1.0f;
    pb.o = o;
    const HeadFrame& f = shell.frame();
    pb.R = o.headRadius > 0.0f ? o.headRadius : f.radius;
    pb.center = D3(f.center);
    pb.up = D3(f.up.normalized());
    for (const FitStroke& s : strokes) {
        View v;
        v.stroke = &s;
        v.curve = strokeCurve(s, o.splineSpacing);
        v.share = pixelShare(s.camera, f.center, pb.R);
        pb.views.push_back(std::move(v));
    }
    return pb;
}

}  // namespace

FittedGuide fitGuide(const std::vector<Vec3f>& initial, const std::vector<FitStroke>& strokes, const HairShell& shell, const ScalpMesh* head,
                     const GuideFitOptions& o) {
    if (initial.size() < 2) throw std::invalid_argument("fitGuide: a guide needs 2 points or more");
    if (shell.empty()) throw std::invalid_argument("fitGuide: no shell");
    for (const FitStroke& s : strokes)
        if (s.pixels.size() < 2) throw std::invalid_argument("fitGuide: a stroke needs 2 points or more");
    Problem pb = makeProblem(strokes, shell, head, o);
    std::vector<D3> x = toD(initial);
    FittedGuide out;
    out.lengthBefore = static_cast<float>(lengthOf(x));
    // The rest length: the guide's own spacing (its points as given, evenly spaced).
    pb.h = std::max(1e-6 * pb.R, lengthOf(x) / static_cast<double>(x.size() - 1));
    x = evenlySpaced(x, pb.h);
    pb.h = std::max(1e-6 * pb.R, lengthOf(x) / static_cast<double>(std::max<std::size_t>(1, x.size() - 1)));
    pb.markSeen(x);
    out.energyBefore = static_cast<float>(pb.evaluate(x, nullptr, nullptr));
    double E = out.energyBefore;
    D3 root = x.front();
    const bool reroot = o.scalp != nullptr && !pb.views.empty();
    const int rerootRounds = reroot ? 3 : 0;
    const int rounds = std::max(1, o.lengthRounds) + rerootRounds;
    for (int round = 0; round < rounds; ++round) {
        out.iterations += pb.solve(x, &E);
        if (round + 1 == rounds || pb.views.empty()) break;
        pb.markSeen(x);
        const Coverage c = coverage(pb, x);
        int firstCovered = -1, lastCovered = -1;
        for (int i = 0; i < static_cast<int>(x.size()); ++i)
            if (c.covered[static_cast<std::size_t>(i)]) {
                if (firstCovered < 0) firstCovered = i;
                lastCovered = i;
            }
        bool changed = false;
        if (round < rerootRounds) {
            // The root again, under where the strokes start now (the start's depth was one photo's
            // guess before): the guide from its first covered point, carried back along its
            // direction by what the strokes run on before it, rooted on the scalp.
            const bool needed = firstCovered >= 1 && (round == 0 || c.before > 2.0 * pb.h);
            if (needed && static_cast<int>(x.size()) - firstCovered >= 2) {
                std::vector<D3> tail(x.begin() + firstCovered, x.end());
                D3 back = tail[0] - tail[1];
                const double bl = back.length();
                back = bl > 1e-12 ? back * (1.0 / bl) : D3(0, 0, 0);
                // (no more than half the guide's length, and stopping short of the head)
                const int more = static_cast<int>(std::round(std::min(c.before, 0.5 * lengthOf(tail)) / pb.h));
                const double clear = static_cast<double>(o.clearance) * pb.R;
                for (int k = 1; k <= more; ++k) {
                    const D3 q = tail.front() + back * pb.h;
                    if (pb.headHeight(q) < 2.0 * clear) break;
                    tail.insert(tail.begin(), q);
                }
                // A start well off the head (a lock seen from where it comes out from under the hair):
                // carried on back along the lock, curving down onto the head (the way it came), not
                // dropped straight under (round 0 only).
                const auto gapOf = [&](const D3& q) { return pb.headHeight(q); };
                if (round == 0 && o.carryShare > 0.0f && gapOf(tail.front()) > static_cast<double>(o.carryShare) * pb.R && tail.size() >= 2) {
                    std::vector<D3> carried = tail;
                    D3 dir = carried[0] - carried[1];
                    dir = dir * (1.0 / std::max(1e-12, dir.length()));
                    const double turn = pb.h / (0.4 * pb.R);
                    bool reached = false;
                    for (double s = 0.0; s < static_cast<double>(o.carryLength) * pb.R; s += pb.h) {
                        const D3 q = carried.front();
                        D3 in = pb.center - q;
                        in = in * (1.0 / std::max(1e-12, in.length()));
                        dir = dir + in * turn;
                        dir = dir * (1.0 / std::max(1e-12, dir.length()));
                        const D3 next = q + dir * pb.h;
                        if (gapOf(next) <= 2.0 * static_cast<double>(o.clearance) * pb.R) {
                            reached = true;
                            break;
                        }
                        carried.insert(carried.begin(), next);
                    }
                    if (reached) tail = std::move(carried);
                }
                // (the start out of the head: rooting looks under it toward the middle)
                while (tail.size() > 2 && pb.headHeight(tail.front()) < clear) tail.erase(tail.begin());
                std::vector<Vec3f> pts;
                for (const D3& p : tail) pts.push_back(p.f());
                GuideRootingOptions ro = o.rooting;
                ro.flip = false;
                if (!(ro.headRadius > 0.0f)) ro.headRadius = static_cast<float>(pb.R);
                if (ro.radial) ro.center = shell.frame().center;
                ro.up = shell.frame().up;
                const RootedGuide rooted = rootGuide(pts, *o.scalp, head ? head : o.scalp, ro, guideRootingScale(*o.scalp, ro));
                if (rooted.points.size() >= 2) {
                    x = evenlySpaced(toD(rooted.points), pb.h);
                    root = x.front();
                    changed = true;
                }
            }
            if (!changed) round = rerootRounds - 1;  // (on to the length)
            changed = true;
        } else if (c.extend > 2.0 * pb.h && lastCovered >= static_cast<int>(x.size()) - 3) {
            // The length: grow where a stroke runs on past the tip, trim a tail no stroke covers.
            const double grow = std::min(c.extend, 0.5 * lengthOf(x));
            const int more = static_cast<int>(std::round(grow / pb.h));
            const std::size_t n = x.size();
            D3 dir = x[n - 1] - x[n - 2];
            const double dl = dir.length();
            dir = dl > 1e-12 ? dir * (1.0 / dl) : pb.up * -1.0;
            for (int k = 1; k <= more; ++k) x.push_back(x[n - 1] + dir * (pb.h * k));
            changed = more > 0;
        } else if (lastCovered >= 2 && lastCovered + 1 < static_cast<int>(x.size())) {
            // (the strokes end before the tip: the tip where they end)
            x.resize(static_cast<std::size_t>(lastCovered + 1));
            changed = true;
        }
        if (!changed) break;
        pb.markSeen(x);
    }
    pb.markSeen(x);
    out.energyAfter = static_cast<float>(pb.evaluate(x, nullptr, nullptr));
    out.views = coverage(pb, x).views;
    out.points.reserve(x.size());
    for (const D3& p : x) out.points.push_back(p.f());
    out.points.front() = root.f();
    out.lengthAfter = static_cast<float>(lengthOf(x));
    return out;
}

std::vector<FittedGuide> fitGuides(const std::vector<std::vector<Vec3f>>& initial, const std::vector<std::vector<FitStroke>>& strokes,
                                   const HairShell& shell, const ScalpMesh* head, const GuideFitOptions& o) {
    if (initial.size() != strokes.size()) throw std::invalid_argument("fitGuides: a stroke list per guide");
    if (head && head->triangleCount() > 0) {
        // (the head's search trees built here, before the threads ask them)
        SurfacePoint hit;
        head->intersectRay(shell.frame().center, Vec3f(0, 1, 0), &hit);
        head->closestPoint(shell.frame().center);
    }
    std::vector<FittedGuide> out(initial.size());
    parallelFor(0, initial.size(), [&](std::size_t i) { out[i] = fitGuide(initial[i], strokes[i], shell, head, o); }, 1);
    return out;
}

StrokeMatch matchStroke(const std::vector<Vec3f>& guide, const FitStroke& stroke, const ScalpMesh* head, const Vec3f& center, float headRadius,
                        float splineSpacing) {
    StrokeMatch m;
    if (guide.size() < 2 || stroke.pixels.size() < 2) return m;
    const double R = headRadius > 0.0f ? headRadius : 1.0;
    const Curve2 curve = strokeCurve(stroke, splineSpacing);
    const double share = pixelShare(stroke.camera, center, R);
    std::vector<Proj> q(guide.size());
    std::vector<char> seen(guide.size(), 0);
    for (std::size_t i = 0; i < guide.size(); ++i) {
        q[i] = projected(stroke.camera, D3(guide[i]));
        seen[i] = q[i].ok && sees(stroke.camera, D3(guide[i]), head, R) ? 1 : 0;
    }
    double sum = 0.0, agree = 0.0, aMin = 1.0, aMax = 0.0;
    for (std::size_t i = 0; i < guide.size(); ++i) {
        if (!seen[i]) continue;
        ++m.points;
        const Near nr = nearest(curve, q[i].u, q[i].v);
        if (!nr.interior) continue;
        ++m.coveredPoints;
        sum += nr.dist;
        aMin = std::min(aMin, nr.alpha);
        aMax = std::max(aMax, nr.alpha);
        const std::size_t a = i > 0 ? i - 1 : i, b = i + 1 < guide.size() ? i + 1 : i;
        if (q[a].ok && q[b].ok) {
            const double dx = q[b].u - q[a].u, dy = q[b].v - q[a].v, l = std::hypot(dx, dy);
            if (l > 1e-9) agree += (dx * nr.tx + dy * nr.ty) / l;
        }
    }
    if (m.coveredPoints > 0) {
        m.meanPx = static_cast<float>(sum / m.coveredPoints);
        m.meanR = static_cast<float>(sum / m.coveredPoints * share);
        m.agreement = static_cast<float>(agree / m.coveredPoints);
        m.strokeCovered = static_cast<float>(std::max(0.0, aMax - aMin));
    }
    m.covered = m.points > 0 ? static_cast<float>(m.coveredPoints) / static_cast<float>(m.points) : 0.0f;
    return m;
}

namespace {

// Whether one guide fitted to several strokes fits each of them (a fused lock): every photo's mean
// distance within maxFusedResidual, a fair share of each stroke covered, the guide not out past the
// shell. The worst mean (head radii), or -1.
float fusedCost(const FittedGuide& g, const HairShell& shell, float R, const StrokeAssociationOptions& o) {
    float worst = 0.0f;
    for (const FitViewResidual& v : g.views) {
        if (v.points == 0 || v.covered <= 0.0f || v.meanR > o.maxFusedResidual || v.maxR > o.maxFusedOff ||
            v.strokeCovered < o.minStrokeCovered || v.hidden > o.maxHidden)
            return -1.0f;
        worst = std::max(worst, v.meanR);
    }
    const Vec3f c = shell.frame().center;
    for (const Vec3f& p : g.points) {
        const Vec3f d = p - c;
        if (d.length() - shell.outer(d) > o.maxOutside * R) return -1.0f;
    }
    return worst;
}

// The member a group's guide starts from: the best rooted (smallest root gap), then the longest.
bool rootsBetter(const FitStroke& a, float lengthA, const FitStroke& b, float lengthB, float R) {
    if (std::abs(a.rootGap - b.rootGap) > 0.02f * R) return a.rootGap < b.rootGap;
    return lengthA > lengthB;
}

float guideLengthOf(const std::vector<Vec3f>& g) {
    float s = 0.0f;
    for (std::size_t i = 1; i < g.size(); ++i) s += (g[i] - g[i - 1]).length();
    return s;
}

}  // namespace

std::vector<std::vector<int>> associateStrokes(const std::vector<FitStroke>& strokes, const std::vector<std::vector<Vec3f>>& guides,
                                               const HairShell& shell, const ScalpMesh* head, const GuideFitOptions& fit,
                                               const StrokeAssociationOptions& o, const std::vector<int>* forced) {
    const std::size_t n = strokes.size();
    if (guides.size() != n) throw std::invalid_argument("associateStrokes: a guide per stroke");
    if (forced && forced->size() != n) throw std::invalid_argument("associateStrokes: a forced value per stroke");
    const float R = fit.headRadius > 0.0f ? fit.headRadius : shell.frame().radius;
    const Vec3f center = shell.frame().center;
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<std::set<int>> photos(n);
    for (std::size_t i = 0; i < n; ++i) photos[i].insert(strokes[i].photo);
    std::function<int(int)> find = [&](int i) {
        return parent[static_cast<std::size_t>(i)] == i ? i : parent[static_cast<std::size_t>(i)] = find(parent[static_cast<std::size_t>(i)]);
    };
    auto join = [&](int a, int b, bool check) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (check)
            for (int p : photos[static_cast<std::size_t>(b)])
                if (photos[static_cast<std::size_t>(a)].count(p)) return false;
        if (b < a) std::swap(a, b);
        parent[static_cast<std::size_t>(b)] = a;
        photos[static_cast<std::size_t>(a)].insert(photos[static_cast<std::size_t>(b)].begin(), photos[static_cast<std::size_t>(b)].end());
        return true;
    };
    auto isForced = [&](std::size_t i) { return forced && (*forced)[i] >= 0; };
    if (forced) {
        std::map<int, int> first;
        for (std::size_t i = 0; i < n; ++i) {
            if (!isForced(i)) continue;
            const auto it = first.find((*forced)[i]);
            if (it == first.end()) first[(*forced)[i]] = static_cast<int>(i);
            else join(it->second, static_cast<int>(i), false);
        }
    }
    // Candidates: one stroke's guide lies near the other's stroke in its photo, the same way.
    std::vector<std::pair<int, int>> candidates;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b)
            if (strokes[a].photo != strokes[b].photo && !isForced(a) && !isForced(b) && guides[a].size() >= 2 && guides[b].size() >= 2)
                candidates.push_back({static_cast<int>(a), static_cast<int>(b)});
    std::vector<char> near(candidates.size(), 0);
    parallelFor(0, candidates.size(), [&](std::size_t k) {
        const std::size_t a = static_cast<std::size_t>(candidates[k].first), b = static_cast<std::size_t>(candidates[k].second);
        auto close = [&](const StrokeMatch& m) {
            return m.coveredPoints >= 4 && m.covered >= o.minCovered && m.agreement >= o.minAgreement && m.meanR <= o.nearDistance;
        };
        const StrokeMatch ab = matchStroke(guides[a], strokes[b], head, center, R, fit.splineSpacing);
        const StrokeMatch ba = matchStroke(guides[b], strokes[a], head, center, R, fit.splineSpacing);
        near[k] = close(ab) || close(ba);
    }, 4);
    std::vector<std::pair<int, int>> tried;
    for (std::size_t k = 0; k < candidates.size(); ++k)
        if (near[k]) tried.push_back(candidates[k]);
    // Each candidate: one guide fitted to both strokes; a match when it fits both.
    std::vector<std::vector<Vec3f>> init;
    std::vector<std::vector<FitStroke>> pairStrokes;
    for (const auto& [a, b] : tried) {
        const std::size_t ia = static_cast<std::size_t>(a), ib = static_cast<std::size_t>(b);
        const bool aFirst = rootsBetter(strokes[ia], guideLengthOf(guides[ia]), strokes[ib], guideLengthOf(guides[ib]), R);
        init.push_back(aFirst ? guides[ia] : guides[ib]);
        pairStrokes.push_back(aFirst ? std::vector<FitStroke>{strokes[ia], strokes[ib]} : std::vector<FitStroke>{strokes[ib], strokes[ia]});
    }
    const std::vector<FittedGuide> joint = fitGuides(init, pairStrokes, shell, head, fit);
    struct Pair {
        float cost;
        int a, b;
    };
    std::vector<Pair> pairs;
    for (std::size_t k = 0; k < tried.size(); ++k) {
        const float cost = fusedCost(joint[k], shell, R, o);
        if (cost >= 0.0f) pairs.push_back({cost, tried[k].first, tried[k].second});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const Pair& x, const Pair& y) { return x.cost != y.cost ? x.cost < y.cost : (x.a != y.a ? x.a < y.a : x.b < y.b); });
    for (const Pair& p : pairs) join(p.a, p.b, true);
    std::map<int, std::vector<int>> byRoot;
    for (std::size_t i = 0; i < n; ++i) byRoot[find(static_cast<int>(i))].push_back(static_cast<int>(i));
    std::vector<std::vector<int>> groups;
    for (auto& [root, members] : byRoot) groups.push_back(members);
    std::sort(groups.begin(), groups.end(), [](const std::vector<int>& x, const std::vector<int>& y) { return x.front() < y.front(); });
    return groups;
}

StrokeFitResult fitStrokes(const std::vector<FitStroke>& strokes, const std::vector<std::vector<Vec3f>>& lifts, const HairShell& shell,
                           const ScalpMesh* head, bool fuse, const GuideFitOptions& o, const StrokeAssociationOptions& ao,
                           const std::vector<int>* forced) {
    if (lifts.size() != strokes.size()) throw std::invalid_argument("fitStrokes: a lift per stroke");
    StrokeFitResult result;
    // Each stroke alone.
    std::vector<std::size_t> live;
    for (std::size_t i = 0; i < strokes.size(); ++i)
        if (lifts[i].size() >= 2) live.push_back(i);
    std::vector<std::vector<Vec3f>> init;
    std::vector<std::vector<FitStroke>> own;
    for (std::size_t i : live) {
        init.push_back(lifts[i]);
        own.push_back({strokes[i]});
    }
    const std::vector<FittedGuide> lone = fitGuides(init, own, shell, head, o);
    std::vector<FittedGuide> single(strokes.size());
    std::vector<std::vector<Vec3f>> singlePoints(strokes.size());
    for (std::size_t k = 0; k < live.size(); ++k) {
        single[live[k]] = lone[k];
        singlePoints[live[k]] = lone[k].points;
    }
    const float R = o.headRadius > 0.0f ? o.headRadius : shell.frame().radius;
    std::vector<std::vector<int>> groups;
    if (fuse) {
        groups = associateStrokes(strokes, singlePoints, shell, head, o, ao, forced);
    } else {
        for (std::size_t i = 0; i < strokes.size(); ++i) groups.push_back({static_cast<int>(i)});
    }
    // (strokes whose lift was dropped have no guide)
    for (auto& g : groups)
        g.erase(std::remove_if(g.begin(), g.end(), [&](int i) { return lifts[static_cast<std::size_t>(i)].size() < 2; }), g.end());
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const std::vector<int>& g) { return g.empty(); }), groups.end());
    // Each group of several: from its best rooted member's lone fit, to all of its strokes.
    std::vector<std::size_t> multi;
    std::vector<std::vector<Vec3f>> jointInit;
    std::vector<std::vector<FitStroke>> jointStrokes;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        std::vector<int>& g = groups[gi];
        if (g.size() < 2) continue;
        std::stable_sort(g.begin(), g.end(), [&](int a, int b) {
            const std::size_t ia = static_cast<std::size_t>(a), ib = static_cast<std::size_t>(b);
            return rootsBetter(strokes[ia], single[ia].lengthAfter, strokes[ib], single[ib].lengthAfter, R);
        });
        multi.push_back(gi);
        jointInit.push_back(single[static_cast<std::size_t>(g.front())].points);
        std::vector<FitStroke> ss;
        for (int i : g) ss.push_back(strokes[static_cast<std::size_t>(i)]);
        jointStrokes.push_back(std::move(ss));
    }
    const std::vector<FittedGuide> joint = fitGuides(jointInit, jointStrokes, shell, head, o);
    std::vector<int> jointIndex(groups.size(), -1);
    for (std::size_t k = 0; k < multi.size(); ++k) jointIndex[multi[k]] = static_cast<int>(k);
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const std::vector<int>& g = groups[gi];
        if (jointIndex[gi] < 0) {
            result.groups.push_back({g, single[static_cast<std::size_t>(g.front())], false});
            continue;
        }
        const FittedGuide& fg = joint[static_cast<std::size_t>(jointIndex[gi])];
        if (fusedCost(fg, shell, R, ao) >= 0.0f) {
            result.groups.push_back({g, fg, false});
            ++result.fused;
        } else {
            for (int i : g) {
                result.groups.push_back({{i}, single[static_cast<std::size_t>(i)], true});
                ++result.split;
            }
        }
    }
    std::stable_sort(result.groups.begin(), result.groups.end(), [](const StrokeFitGroup& a, const StrokeFitGroup& b) {
        return *std::min_element(a.strokes.begin(), a.strokes.end()) < *std::min_element(b.strokes.begin(), b.strokes.end());
    });
    return result;
}

}  // namespace groomlab
