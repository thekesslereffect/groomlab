// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_shoot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace groomlab::agent {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kFilmMm = 36.0;
// What an agent's picks on a mesh of another person's head are good to: 1.5% of the photo's width
// (2 pixels at least).
double defaultPickSigma(double width) { return std::max(2.0, 0.015 * width); }

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? a * (1.0 / l) : a;
}
V3 of(const Vec3f& v) { return {v.x, v.y, v.z}; }

struct Frame {
    V3 c, u, f, l;  // middle, up, front, left
};

// A camera's six numbers: yaw, pitch, roll (radians), log distance, the middle's offset across
// and up (shares of the distance).
using Params = std::array<double, 6>;

RefCamera cameraOf(const Frame& fr, const Params& p, double focalPx, double width, double height) {
    const double yaw = p[0], pitch = p[1], roll = p[2], d = std::exp(p[3]);
    const V3 z = unit(fr.f * (std::cos(yaw) * std::cos(pitch)) + fr.l * (std::sin(yaw) * std::cos(pitch)) + fr.u * std::sin(pitch));
    V3 x0 = cross(fr.u, z);
    if (dot(x0, x0) < 1e-12) x0 = cross(fr.f, z);
    x0 = unit(x0);
    const V3 y0 = cross(z, x0);
    const V3 x = x0 * std::cos(roll) + y0 * std::sin(roll), y = y0 * std::cos(roll) - x0 * std::sin(roll);
    const V3 pos = fr.c + x * (p[4] * d) + y * (p[5] * d) + z * d;
    RefCamera c;
    c.width = width;
    c.height = height;
    c.focalPx = focalPx;
    const V3 axes[3] = {x, y, z};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            const double comp = r == 0 ? axes[k].x : r == 1 ? axes[k].y : axes[k].z;
            c.pose.rotation[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] = comp;
        }
    }
    c.pose.position = {pos.x, pos.y, pos.z};
    return c;
}

Params paramsOf(const Frame& fr, const CameraPose& pose) {
    const V3 x{pose.rotation[0][0], pose.rotation[1][0], pose.rotation[2][0]};
    const V3 z{pose.rotation[0][2], pose.rotation[1][2], pose.rotation[2][2]};
    Params p{};
    p[0] = std::atan2(dot(z, fr.l), dot(z, fr.f));
    p[1] = std::asin(std::clamp(dot(z, fr.u), -1.0, 1.0));
    V3 x0 = cross(fr.u, z);
    if (dot(x0, x0) < 1e-12) x0 = cross(fr.f, z);
    x0 = unit(x0);
    const V3 y0 = cross(z, x0);
    p[2] = std::atan2(dot(x, y0), dot(x, x0));
    const V3 rel = V3{pose.position[0], pose.position[1], pose.position[2]} - fr.c;
    const double d = std::max(1e-6, dot(rel, z));
    p[3] = std::log(d);
    const V3 y = cross(z, x);
    p[4] = dot(rel, x) / d;
    p[5] = dot(rel, y) / d;
    return p;
}

double wrapDegrees(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

// Solves A x = b (n x n, Gaussian elimination with partial pivoting); false when singular.
bool solveLinear(std::vector<double> A, std::vector<double> b, std::size_t n, std::vector<double>& x) {
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t p = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::abs(A[r * n + c]) > std::abs(A[p * n + c])) p = r;
        if (std::abs(A[p * n + c]) < 1e-18) return false;
        if (p != c) {
            for (std::size_t k = 0; k < n; ++k) std::swap(A[p * n + k], A[c * n + k]);
            std::swap(b[p], b[c]);
        }
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            if (f == 0.0) continue;
            for (std::size_t k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0.0);
    for (std::size_t r = n; r-- > 0;) {
        double s = b[r];
        for (std::size_t k = r + 1; k < n; ++k) s -= A[r * n + k] * x[k];
        x[r] = s / A[r * n + r];
    }
    return true;
}

}  // namespace

bool viewYaw(const std::string& view, double& yaw, bool* level) {
    if (level) *level = true;
    if (view == "front") yaw = 0.0;
    else if (view == "back") yaw = 180.0;
    else if (view == "left" || view == "side") yaw = 90.0;
    else if (view == "right") yaw = -90.0;
    else if (view == "three_quarter") {
        yaw = 35.0;
        if (level) *level = false;  // (render_preview's three_quarter looks down 15 degrees)
    } else if (view == "three_quarter_front_left") yaw = 45.0;
    else if (view == "three_quarter_front_right") yaw = -45.0;
    else if (view == "three_quarter_back_left") yaw = 135.0;
    else if (view == "three_quarter_back_right") yaw = -135.0;
    else return false;
    return true;
}

ShootSolve solveShoot(const std::vector<ShootView>& views, const ShootOptions& options) {
    // How good the picks are: what the pairs alone fit to (when they are enough to say).
    if (!options.pickSigma) {
        std::size_t residuals = 0, dof = options.focalMm ? 0 : 1;
        double width = 0.0;
        for (const ShootView& v : views) {
            if (v.fixed) continue;
            residuals += 2 * v.pairs.size();
            dof += 6;
            width = std::max(width, v.width);
        }
        ShootOptions o = options;
        o.pickSigma = defaultPickSigma(width);
        if (residuals >= dof + 4 && options.priorWeight > 0.0) {
            ShootOptions alone = options;
            alone.priorWeight = 0.0;
            alone.pickSigma = 1.0;
            alone.lensFallback = false;
            const ShootSolve a = solveShoot(views, alone);
            if (a.ok) {
                const double sigma = a.rms * std::sqrt(static_cast<double>(residuals) / static_cast<double>(residuals - dof));
                o.pickSigma = std::clamp(sigma, 0.1, 2.0 * defaultPickSigma(width));
            }
        }
        return solveShoot(views, o);
    }
    const double sigmaPx = *options.pickSigma;
    auto pickSigma = [&](double) { return sigmaPx; };
    ShootSolve out;
    Frame fr{of(options.center), unit(of(options.up)), unit(of(options.front)), V3{}};
    fr.l = unit(cross(fr.u, fr.f));
    std::vector<std::size_t> free;
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (!views[i].fixed) free.push_back(i);
    }
    if (free.empty()) {
        out.why = "no photo to solve";
        return out;
    }
    std::size_t pairCount = 0;
    for (const std::size_t i : free) {
        if (views[i].pairs.size() < 3) {
            out.why = "every photo solved needs 3 pairs or more (4 or more alone)";
            return out;
        }
        pairCount += views[i].pairs.size();
    }
    const bool freeFocal = !options.focalMm;
    const double w0 = options.priorWeight;
    // The fixed cameras' distances (the shoot's, as a prior).
    std::vector<double> fixedLogD;
    for (const ShootView& v : views) {
        if (v.fixed) fixedLogD.push_back(paramsOf(fr, v.fixed->pose)[3]);
    }
    const std::size_t n = free.size() * 6 + (freeFocal ? 1 : 0);
    auto unpackFocal = [&](const std::vector<double>& x) { return freeFocal ? std::exp(x[n - 1]) : *options.focalMm; };
    auto residuals = [&](const std::vector<double>& x, std::vector<double>& r, bool withPriors) {
        r.clear();
        const double fmm = unpackFocal(x);
        double meanLogD = 0.0;
        int counted = 0;
        for (std::size_t k = 0; k < free.size(); ++k) meanLogD += x[k * 6 + 3], ++counted;
        for (const double d : fixedLogD) meanLogD += d, ++counted;
        meanLogD /= std::max(1, counted);
        if (options.distance) meanLogD = std::log(*options.distance);
        for (std::size_t k = 0; k < free.size(); ++k) {
            const ShootView& v = views[free[k]];
            Params p;
            for (int j = 0; j < 6; ++j) p[static_cast<std::size_t>(j)] = x[k * 6 + static_cast<std::size_t>(j)];
            const RefCamera cam = cameraOf(fr, p, fmm / kFilmMm * v.width, v.width, v.height);
            for (const CameraCorrespondence& c : v.pairs) {
                float u = 0, vv = 0, depth = 0;
                if (!cam.project(Vec3f(static_cast<float>(c.world[0]), static_cast<float>(c.world[1]), static_cast<float>(c.world[2])), u, vv, &depth) ||
                    depth <= 0.0f) {
                    r.push_back(v.width / pickSigma(v.width)), r.push_back(v.width / pickSigma(v.width));
                    continue;
                }
                r.push_back((u - c.u) / pickSigma(v.width));
                r.push_back((vv - c.v) / pickSigma(v.width));
            }
            if (!withPriors || w0 <= 0.0) continue;
            if (v.yawDegrees) r.push_back(w0 * wrapDegrees(p[0] / kDeg - *v.yawDegrees) / options.yawSigmaDegrees);
            if (v.level) {
                r.push_back(w0 * (p[1] / kDeg) / options.pitchSigmaDegrees);
                r.push_back(w0 * (p[2] / kDeg) / options.rollSigmaDegrees);
            }
            if (counted > 1 || options.distance) r.push_back(w0 * (p[3] - meanLogD) / options.distanceSigma);
            r.push_back(0.5 * w0 * p[4] / 0.3);
            r.push_back(0.5 * w0 * p[5] / 0.3);
        }
        if (withPriors && freeFocal && w0 > 0.0) r.push_back(w0 * std::log(fmm / options.preferredFocalMm) / std::log(options.focalSigma));
    };
    auto cost = [](const std::vector<double>& r) {
        double s = 0.0;
        for (const double v : r) s += v * v;
        return s;
    };
    // The starts: a few focal lengths (the one given), each view at its prior angle (else where a
    // solve of its own pairs puts it), as far as its pairs' spread says.
    std::vector<double> focals = freeFocal ? std::vector<double>{60.0, 100.0, 150.0, 220.0} : std::vector<double>{*options.focalMm};
    double bestCost = std::numeric_limits<double>::infinity();
    std::vector<double> best;
    for (const double f0 : focals) {
        std::vector<double> x(n, 0.0);
        if (freeFocal) x[n - 1] = std::log(f0);
        bool usable = true;
        for (std::size_t k = 0; k < free.size(); ++k) {
            const ShootView& v = views[free[k]];
            const double fpx = f0 / kFilmMm * v.width;
            // How far: the points' spread over their pixels' spread.
            V3 c3{};
            double cu = 0, cv = 0;
            for (const CameraCorrespondence& c : v.pairs) c3 = c3 + V3{c.world[0], c.world[1], c.world[2]}, cu += c.u, cv += c.v;
            const double m = static_cast<double>(v.pairs.size());
            c3 = c3 * (1.0 / m), cu /= m, cv /= m;
            double s3 = 0, s2 = 0;
            for (const CameraCorrespondence& c : v.pairs) {
                const V3 d = V3{c.world[0], c.world[1], c.world[2]} - c3;
                s3 += dot(d, d);
                s2 += (c.u - cu) * (c.u - cu) + (c.v - cv) * (c.v - cv);
            }
            const double distance = std::max(1e-3, fpx * std::sqrt(s3 / std::max(1e-9, s2)));
            Params p{};
            if (v.yawDegrees) {
                p[0] = *v.yawDegrees * kDeg;
            } else {
                CameraSolveOptions o;
                o.imageWidth = v.width;
                o.imageHeight = v.height;
                o.focalPx = fpx;
                o.fixFocal = true;
                const CameraSolveResult alone = v.pairs.size() >= 4 ? solveCamera(v.pairs, o) : CameraSolveResult{};
                if (alone.ok) p = paramsOf(fr, alone.pose);
                else usable = usable && v.pairs.size() >= 4;
            }
            if (v.yawDegrees || !(p[3] != 0.0)) {
                p[3] = std::log(distance);
                // The pairs' middle where the camera sees it: the head's middle offset to match.
                p[4] = -(cu - 0.5 * v.width) / fpx;
                p[5] = (cv - 0.5 * v.height) / fpx;
            }
            for (int j = 0; j < 6; ++j) x[k * 6 + static_cast<std::size_t>(j)] = p[static_cast<std::size_t>(j)];
        }
        if (!usable) continue;
        // Levenberg-Marquardt.
        std::vector<double> r0, r1;
        residuals(x, r0, true);
        double c0 = cost(r0), lambda = 1e-3;
        for (int it = 0; it < 150; ++it) {
            const std::size_t m = r0.size();
            std::vector<double> J(m * n);
            for (std::size_t j = 0; j < n; ++j) {
                std::vector<double> xs = x;
                const double h = j + 1 == n && freeFocal ? 1e-5 : (j % 6 == 3 ? 1e-5 : 1e-6);
                xs[j] += h;
                residuals(xs, r1, true);
                for (std::size_t i = 0; i < m; ++i) J[i * n + j] = (r1[i] - r0[i]) / h;
            }
            std::vector<double> A(n * n, 0.0), g(n, 0.0);
            for (std::size_t i = 0; i < m; ++i) {
                for (std::size_t a = 0; a < n; ++a) {
                    const double ja = J[i * n + a];
                    if (ja == 0.0) continue;
                    g[a] -= ja * r0[i];
                    for (std::size_t b = 0; b < n; ++b) A[a * n + b] += ja * J[i * n + b];
                }
            }
            bool better = false;
            for (int tries = 0; tries < 8 && !better; ++tries) {
                std::vector<double> D = A, dx;
                for (std::size_t a = 0; a < n; ++a) D[a * n + a] += lambda * std::max(1e-9, A[a * n + a]);
                if (!solveLinear(D, g, n, dx)) {
                    lambda *= 10.0;
                    continue;
                }
                std::vector<double> xn = x;
                for (std::size_t a = 0; a < n; ++a) xn[a] += dx[a];
                residuals(xn, r1, true);
                const double c1 = cost(r1);
                if (c1 < c0) {
                    const double gain = c0 - c1;
                    x = xn, r0 = r1, c0 = c1;
                    lambda = std::max(1e-9, lambda * 0.3);
                    better = true;
                    if (gain < 1e-9 * std::max(1.0, c1)) it = 1000;
                } else {
                    lambda *= 10.0;
                }
            }
            if (!better) break;
        }
        if (c0 < bestCost) bestCost = c0, best = x;
    }
    if (best.empty()) {
        out.why = "the cameras could not be solved";
        return out;
    }
    double fmm = unpackFocal(best);
    // A short focal the pairs leaned to: tried at the preferred one, kept there unless clearly worse.
    if (freeFocal && fmm < 70.0 && options.lensFallback) {
        ShootOptions fixed = options;
        fixed.focalMm = options.preferredFocalMm;
        const ShootSolve longer = solveShoot(views, fixed);
        ShootOptions same = options;
        same.focalMm = fmm;
        const ShootSolve shorter = solveShoot(views, same);
        if (longer.ok && shorter.ok && longer.rms <= shorter.rms + 0.5 * sigmaPx) {
            ShootSolve kept = longer;
            kept.focalNote = "the pairs leaned to a " + std::to_string(static_cast<int>(std::lround(fmm))) +
                             " mm lens but fit a portrait lens nearly as well (" + std::to_string(shorter.rms).substr(0, 4) + " -> " +
                             std::to_string(longer.rms).substr(0, 4) + " px): kept " +
                             std::to_string(static_cast<int>(std::lround(options.preferredFocalMm))) + " mm; give focal_mm if the lens is known";
            return kept;
        }
    }
    out.ok = true;
    out.focalMm = fmm;
    std::vector<double> rmsList;
    double all = 0.0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < views.size(); ++i) {
        const ShootView& v = views[i];
        ShootCamera sc;
        if (v.fixed) {
            sc.camera = *v.fixed;
        } else {
            Params p;
            for (int j = 0; j < 6; ++j) p[static_cast<std::size_t>(j)] = best[k * 6 + static_cast<std::size_t>(j)];
            sc.camera = cameraOf(fr, p, fmm / kFilmMm * v.width, v.width, v.height);
            ++k;
        }
        const Params q = paramsOf(fr, sc.camera.pose);
        sc.yaw = q[0] / kDeg, sc.pitch = q[1] / kDeg, sc.roll = q[2] / kDeg, sc.distance = std::exp(q[3]);
        if (v.yawDegrees) {
            const V3 want = fr.f * std::cos(*v.yawDegrees * kDeg) + fr.l * std::sin(*v.yawDegrees * kDeg);
            const V3 z{sc.camera.pose.rotation[0][2], sc.camera.pose.rotation[1][2], sc.camera.pose.rotation[2][2]};
            sc.fromPrior = std::acos(std::clamp(dot(unit(z), want), -1.0, 1.0)) / kDeg;
        }
        double sum = 0.0;
        for (const CameraCorrespondence& c : v.pairs) {
            float u = 0, vv = 0;
            double e = v.width;
            if (sc.camera.project(Vec3f(static_cast<float>(c.world[0]), static_cast<float>(c.world[1]), static_cast<float>(c.world[2])), u, vv)) {
                e = std::sqrt((u - c.u) * (u - c.u) + (vv - c.v) * (vv - c.v));
            }
            sc.errors.push_back(e);
            sum += e * e;
            all += e * e;
        }
        sc.rms = v.pairs.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(v.pairs.size()));
        if (!v.fixed) rmsList.push_back(sc.rms);
        out.cameras.push_back(sc);
    }
    out.rms = pairCount ? std::sqrt(all / static_cast<double>(pairCount)) : 0.0;
    // A photo whose pairs fight the rest: its error far over the shoot's usual.
    if (rmsList.size() >= 3) {
        std::vector<double> sorted = rmsList;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        for (std::size_t i = 0; i < views.size(); ++i) {
            ShootCamera& sc = out.cameras[i];
            if (!views[i].fixed && sc.rms > std::max({2.5 * median, 0.01 * views[i].width, 3.0})) sc.fights = true;
        }
    }
    return out;
}

}  // namespace groomlab::agent
