// SPDX-License-Identifier: Apache-2.0
// The hair's outer surface by direction from the head's middle (hair_shell.h).

#include "groomlab/hair_shell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "groomlab/parallel.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

// Solves the 4x4 system m x = b (Gaussian elimination, partial pivoting); false when singular.
bool solve4(std::array<std::array<double, 5>, 4> m, std::array<double, 4>& x) {
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(m[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) > std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(c)])) pivot = r;
        if (std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(c)]) < 1e-12) return false;
        std::swap(m[static_cast<std::size_t>(c)], m[static_cast<std::size_t>(pivot)]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] / m[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)];
            for (int k = c; k < 5; ++k) m[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] -= f * m[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
        }
    }
    for (int c = 0; c < 4; ++c) x[static_cast<std::size_t>(c)] = m[static_cast<std::size_t>(c)][4] / m[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)];
    return true;
}

// The cells within `maxAngle` of cell (i, j) on an nt x np direction grid, each with its cosine.
template <class F>
void forNeighbours(const std::vector<Vec3f>& dirs, int nt, int np, int i, int j, float maxAngle, F&& f) {
    const float dTheta = kPi / static_cast<float>(nt), dPhi = 2.0f * kPi / static_cast<float>(np);
    const float cosMax = std::cos(maxAngle);
    const Vec3f& d0 = dirs[static_cast<std::size_t>(i) * static_cast<std::size_t>(np) + static_cast<std::size_t>(j)];
    const int rows = static_cast<int>(std::ceil(maxAngle / dTheta));
    for (int ii = std::max(0, i - rows); ii <= std::min(nt - 1, i + rows); ++ii) {
        const float s = std::sin((static_cast<float>(ii) + 0.5f) * dTheta);
        int span = s > 1e-3f ? static_cast<int>(std::ceil(maxAngle / (dPhi * s))) + 1 : np;
        span = std::min(span, (np - 1) / 2);
        for (int dj = -span; dj <= span; ++dj) {
            const int jj = ((j + dj) % np + np) % np;
            const std::size_t at = static_cast<std::size_t>(ii) * static_cast<std::size_t>(np) + static_cast<std::size_t>(jj);
            const float c = d0.dot(dirs[at]);
            if (c >= cosMax) f(at, c);
        }
    }
}

}  // namespace

HeadFrame headFrameOf(const ScalpMesh& region, const Vec3f& up, const Vec3f& front) {
    if (region.pointCount() == 0 || region.triangleCount() == 0) throw std::invalid_argument("headFrameOf: the mesh is empty");
    const auto& pts = region.points();
    std::vector<double> weight(pts.size(), 0.0);
    for (const auto& t : region.triangles()) {
        const Vec3f& a = pts[static_cast<std::size_t>(t[0])];
        const double area = 0.5 * static_cast<double>((pts[static_cast<std::size_t>(t[1])] - a).cross(pts[static_cast<std::size_t>(t[2])] - a).length());
        for (int k = 0; k < 3; ++k) weight[static_cast<std::size_t>(t[static_cast<std::size_t>(k)])] += area / 3.0;
    }
    double sw = 0.0;
    double mx = 0.0, my = 0.0, mz = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        sw += weight[i];
        mx += weight[i] * pts[i].x;
        my += weight[i] * pts[i].y;
        mz += weight[i] * pts[i].z;
    }
    if (!(sw > 0.0)) throw std::invalid_argument("headFrameOf: the mesh has no area");
    const Vec3f mean(static_cast<float>(mx / sw), static_cast<float>(my / sw), static_cast<float>(mz / sw));
    // |p|^2 = 2 c.p + k (p from the mean), k = r^2 - |c|^2: linear least squares in (c, k).
    std::array<std::array<double, 5>, 4> m{};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (weight[i] <= 0.0) continue;
        const Vec3f q = pts[i] - mean;
        const double row[4] = {2.0 * q.x, 2.0 * q.y, 2.0 * q.z, 1.0};
        const double b = static_cast<double>(q.dot(q));
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) m[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] += weight[i] * row[r] * row[c];
            m[static_cast<std::size_t>(r)][4] += weight[i] * row[r] * b;
        }
    }
    HeadFrame f;
    f.up = up.normalized();
    Vec3f fr = front - f.up * front.dot(f.up);
    f.front = fr.length() > 1e-6f ? fr.normalized() : Vec3f(0, 0, 1);
    f.left = f.up.cross(f.front).normalized();
    std::array<double, 4> x{};
    double r2 = 0.0;
    if (solve4(m, x)) r2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    if (r2 > 0.0) {
        f.center = mean + Vec3f(static_cast<float>(x[0]), static_cast<float>(x[1]), static_cast<float>(x[2]));
        f.radius = static_cast<float>(std::sqrt(r2));
    } else {  // (flat: the mean and its spread)
        double spread = 0.0;
        for (std::size_t i = 0; i < pts.size(); ++i) spread += weight[i] * static_cast<double>((pts[i] - mean).length());
        f.center = mean;
        f.radius = static_cast<float>(spread / sw);
    }
    // A sphere far bigger than the region (a nearly flat patch): the region's own spread.
    float far = 0.0f;
    for (const Vec3f& p : pts) far = std::max(far, (p - mean).length());
    if (!(f.radius > 0.0f) || f.radius > 4.0f * std::max(far, 1e-6f)) {
        f.center = mean;
        f.radius = std::max(far, 1e-6f);
    }
    return f;
}

std::string headRegion(const HeadFrame& frame, const Vec3f& d) {
    const Vec3f u = frame.local(d.normalized());
    if (u.y < -0.05f && u.z < 0.0f) return "nape";
    if (u.z > 0.25f) return "front";
    if (std::abs(u.x) > 0.6f && u.y < 0.6f) return "sides";
    if (u.z < -0.35f) return "back";
    return u.y >= 0.3f ? "top" : "sides";
}

const std::vector<std::string>& headRegionNames() {
    static const std::vector<std::string> names{"top", "front", "sides", "back", "nape"};
    return names;
}

Vec3f HairShell::direction(int i, int j) const {
    const float theta = (static_cast<float>(i) + 0.5f) * kPi / static_cast<float>(nt_);
    const float phi = static_cast<float>(j) / static_cast<float>(np_) * 2.0f * kPi - kPi;
    return frame_.front * (std::sin(theta) * std::cos(phi)) + frame_.left * (std::sin(theta) * std::sin(phi)) + frame_.up * std::cos(theta);
}

float HairShell::lookup(const std::vector<float>& grid, const Vec3f& d) const {
    if (grid.empty()) return 0.0f;
    const Vec3f u = frame_.local(d.normalized());
    const float theta = std::acos(std::clamp(u.y, -1.0f, 1.0f));
    const float phi = std::atan2(u.x, u.z);
    const float x = theta / kPi * static_cast<float>(nt_) - 0.5f;
    const float y = (phi + kPi) / (2.0f * kPi) * static_cast<float>(np_);
    const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(i), fy = y - static_cast<float>(j);
    const int i0 = std::clamp(i, 0, nt_ - 1), i1 = std::clamp(i + 1, 0, nt_ - 1);
    const int j0 = ((j % np_) + np_) % np_, j1 = (j0 + 1) % np_;
    return grid[cell(i0, j0)] * (1 - fx) * (1 - fy) + grid[cell(i1, j0)] * fx * (1 - fy) + grid[cell(i0, j1)] * (1 - fx) * fy +
           grid[cell(i1, j1)] * fx * fy;
}

float HairShell::head(const Vec3f& d) const { return lookup(head_, d); }
float HairShell::outer(const Vec3f& d) const { return lookup(outer_, d); }

float HairShell::guessedVolume(const Vec3f& d) const {
    const Vec3f u = frame_.local(d.normalized());
    const float volume = options_.volume > 0.0f ? options_.volume : 0.3f * frame_.radius;
    // (fuller on top, close at the nape, a little narrower up the back)
    const float shape = std::max(0.15f, 0.55f + 0.45f * u.y) * (1.0f - 0.35f * std::max(0.0f, -u.z) * std::max(0.0f, u.y));
    return volume * shape;
}

void HairShell::prepare(const ScalpMesh& head, const HeadFrame& frame, const HairShellOptions& options) {
    if (head.triangleCount() == 0) throw std::invalid_argument("HairShell: the head has no triangles");
    frame_ = frame;
    options_ = options;
    nt_ = std::max(8, options.thetaSteps);
    np_ = std::max(16, options.phiSteps);
    const std::size_t n = static_cast<std::size_t>(nt_) * static_cast<std::size_t>(np_);
    std::vector<Vec3f> dirs(n);
    for (int i = 0; i < nt_; ++i)
        for (int j = 0; j < np_; ++j) dirs[cell(i, j)] = direction(i, j);
    // The outermost surface along each direction: a ray from far out, inward.
    float far = 0.0f;
    for (const Vec3f& p : head.points()) far = std::max(far, (p - frame.center).length());
    far = 2.0f * far + frame.radius;
    std::vector<float> raw(n, -1.0f);
    parallelFor(0, n, [&](std::size_t c) {
        SurfacePoint hit;
        float t = 0.0f;
        if (head.intersectRay(frame.center + dirs[c] * far, -dirs[c], &hit, &t)) raw[c] = std::max(0.0f, far - t);
    }, 64);
    // Directions the mesh is open to (a cap's underside): the neighbours' mean, spread in.
    for (int pass = 0; pass < 64; ++pass) {
        bool missing = false;
        std::vector<float> next = raw;
        for (int i = 0; i < nt_; ++i)
            for (int j = 0; j < np_; ++j) {
                const std::size_t c = cell(i, j);
                if (raw[c] >= 0.0f) continue;
                float sum = 0.0f;
                int k = 0;
                const int ni[4] = {i - 1, i + 1, i, i}, nj[4] = {j, j, (j + np_ - 1) % np_, (j + 1) % np_};
                for (int q = 0; q < 4; ++q) {
                    if (ni[q] < 0 || ni[q] >= nt_) continue;
                    const float r = raw[cell(ni[q], nj[q])];
                    if (r >= 0.0f) sum += r, ++k;
                }
                if (k) next[c] = sum / static_cast<float>(k);
                else missing = true;
            }
        raw = std::move(next);
        if (!missing) break;
    }
    for (float& r : raw)
        if (r < 0.0f) r = frame.radius;
    // Bumps evened out: half the largest radius within 12 degrees, half their weighted mean.
    head_.assign(n, 0.0f);
    const float spread = 12.0f * kPi / 180.0f, cosSpread = std::cos(spread);
    parallelFor(0, static_cast<std::size_t>(nt_), [&](std::size_t ii) {
        const int i = static_cast<int>(ii);
        for (int j = 0; j < np_; ++j) {
            float most = 0.0f;
            double sum = 0.0, ws = 0.0;
            forNeighbours(dirs, nt_, np_, i, j, spread, [&](std::size_t at, float c) {
                most = std::max(most, raw[at]);
                const double w = static_cast<double>(c - cosSpread) * static_cast<double>(c - cosSpread) + 1e-9;
                sum += w * raw[at];
                ws += w;
            });
            head_[cell(i, j)] = 0.5f * most + 0.5f * static_cast<float>(sum / ws);
        }
    }, 1);
}

HairShell HairShell::fromVolume(const ScalpMesh& head, const HeadFrame& frame, const HairShellOptions& options) {
    HairShell s;
    s.prepare(head, frame, options);
    s.outer_.resize(s.head_.size());
    s.maxOuter_ = 0.0f;
    for (int i = 0; i < s.nt_; ++i)
        for (int j = 0; j < s.np_; ++j) {
            const std::size_t c = s.cell(i, j);
            s.outer_[c] = s.head_[c] + s.guessedVolume(s.direction(i, j));
            s.maxOuter_ = std::max(s.maxOuter_, s.outer_[c]);
        }
    return s;
}

int HairShell::say(const View& v, const Vec3f& p) const {
    float u = 0.0f, w = 0.0f, z = 0.0f;
    if (!v.camera.project(p, u, w, &z)) return -1;
    if (u < 0.0f || w < 0.0f || u >= static_cast<float>(v.width) || w >= static_cast<float>(v.height)) return -1;
    const std::size_t at = static_cast<std::size_t>(static_cast<int>(w)) * static_cast<std::size_t>(v.width) + static_cast<std::size_t>(static_cast<int>(u));
    const float d = v.depth[at];
    if (d < kInfinity && z > d + 0.01f * d) return -1;  // behind the head
    return v.hair[at] ? 1 : 0;
}

bool HairShell::seenBare(const Vec3f& p) const {
    for (const View& v : views_)
        if (say(v, p) == 0) return true;
    return false;
}

bool HairShell::seenHair(const Vec3f& p) const {
    bool hair = false;
    for (const View& v : views_) {
        const int s = say(v, p);
        if (s == 0) return false;
        hair = hair || s == 1;
    }
    return hair;
}

HairShell HairShell::fromPhotos(const ScalpMesh& head, const ScalpMesh& hairScalp, const HeadFrame& frame, const std::vector<ReferenceView>& views,
                                const ReferenceGuideOptions& viewOptions, const HairShellOptions& options) {
    std::vector<ShellPhoto> photos;
    for (const ReferenceView& rv : views) {
        ShellPhoto p;
        p.width = rv.image.width;
        p.height = rv.image.height;
        if (p.width <= 0 || p.height <= 0) continue;
        p.camera = rv.camera.width > 0.0 && std::abs(rv.camera.width - p.width) > 0.5 ? rv.camera.scaled(p.width / rv.camera.width) : rv.camera;
        p.hair = photoHairMask(hairScalp, rv, viewOptions);
        photos.push_back(std::move(p));
    }
    return fromMasks(head, frame, photos, options);
}

HairShell HairShell::fromMasks(const ScalpMesh& head, const HeadFrame& frame, const std::vector<ShellPhoto>& photos, const HairShellOptions& options) {
    HairShell s;
    s.prepare(head, frame, options);
    for (const ShellPhoto& photo : photos) {
        View v;
        v.width = photo.width;
        v.height = photo.height;
        if (v.width <= 0 || v.height <= 0 || photo.hair.size() != static_cast<std::size_t>(v.width) * static_cast<std::size_t>(v.height)) {
            throw std::invalid_argument("HairShell: a photo's hair mask is not its size");
        }
        v.camera = photo.camera;
        v.hair = photo.hair;
        // Grown by maskGrow pixels (rows, then columns).
        const int g = std::max(0, options.maskGrow);
        for (int pass = 0; pass < 2 && g > 0; ++pass) {
            std::vector<std::uint8_t> grown(v.hair.size(), 0);
            for (int y = 0; y < v.height; ++y)
                for (int x = 0; x < v.width; ++x) {
                    bool any = false;
                    for (int k = -g; k <= g && !any; ++k) {
                        const int xx = pass == 0 ? x + k : x, yy = pass == 0 ? y : y + k;
                        if (xx < 0 || yy < 0 || xx >= v.width || yy >= v.height) continue;
                        any = v.hair[static_cast<std::size_t>(yy) * static_cast<std::size_t>(v.width) + static_cast<std::size_t>(xx)] != 0;
                    }
                    grown[static_cast<std::size_t>(y) * static_cast<std::size_t>(v.width) + static_cast<std::size_t>(x)] = any ? 1 : 0;
                }
            v.hair = std::move(grown);
        }
        v.depth = meshDepthMap(head, v.camera, v.width, v.height);
        s.views_.push_back(std::move(v));
    }
    const float radius = frame.radius;
    const float reach = options.reach > 0.0f ? options.reach : 2.5f * radius;
    const float step = options.sampleStep > 0.0f ? options.sampleStep : radius / 40.0f;
    const int samples = std::max(1, static_cast<int>(std::ceil(reach / step)));
    const std::size_t n = s.head_.size();
    std::vector<float> thick(n, 0.0f);
    parallelFor(0, static_cast<std::size_t>(s.nt_), [&](std::size_t ii) {
        const int i = static_cast<int>(ii);
        for (int j = 0; j < s.np_; ++j) {
            const std::size_t c = s.cell(i, j);
            const Vec3f d = s.direction(i, j);
            const float guess = s.guessedVolume(d);
            int lastHair = -1, k = 0;
            bool bare = false;
            for (; k < samples; ++k) {
                const Vec3f p = frame.center + d * (s.head_[c] + (static_cast<float>(k) + 0.5f) * step);
                bool hair = false;
                for (const View& v : s.views_) {
                    const int seen = s.say(v, p);
                    if (seen == 0) {
                        bare = true;
                        break;
                    }
                    hair = hair || seen == 1;
                }
                if (bare) break;
                if (hair) lastHair = k;
                else if (lastHair < 0 && static_cast<float>(k + 1) * step > guess) break;  // (no photo sees it: the guess)
            }
            if (lastHair >= 0) thick[c] = static_cast<float>(lastHair + 1) * step;
            else thick[c] = std::min(guess, static_cast<float>(k) * step);
        }
    }, 1);
    // Smoothed over a few degrees.
    std::vector<Vec3f> dirs(n);
    for (int i = 0; i < s.nt_; ++i)
        for (int j = 0; j < s.np_; ++j) dirs[s.cell(i, j)] = s.direction(i, j);
    const float sigma = std::max(0.0f, options.smoothDegrees) * kPi / 180.0f;
    std::vector<float> smooth = thick;
    if (sigma > 0.0f) {
        parallelFor(0, static_cast<std::size_t>(s.nt_), [&](std::size_t ii) {
            const int i = static_cast<int>(ii);
            for (int j = 0; j < s.np_; ++j) {
                double sum = 0.0, ws = 0.0;
                forNeighbours(dirs, s.nt_, s.np_, i, j, 2.5f * sigma, [&](std::size_t at, float c) {
                    const float a = std::acos(std::clamp(c, -1.0f, 1.0f)) / sigma;
                    const double w = std::exp(-0.5 * static_cast<double>(a * a));
                    sum += w * thick[at];
                    ws += w;
                });
                smooth[s.cell(i, j)] = static_cast<float>(sum / ws);
            }
        }, 1);
    }
    s.outer_.resize(n);
    s.maxOuter_ = 0.0f;
    for (std::size_t c = 0; c < n; ++c) {
        s.outer_[c] = s.head_[c] + std::max(0.0f, smooth[c]);
        s.maxOuter_ = std::max(s.maxOuter_, s.outer_[c]);
    }
    return s;
}

HairShell::Hit HairShell::raycast(const Vec3f& origin, const Vec3f& dIn) const {
    Hit out;
    const Vec3f d = dIn.normalized();
    const Vec3f& c = frame_.center;
    const float tc = (c - origin).dot(d);
    const float reach = maxOuter_ * 1.05f;
    const float t0 = std::max(0.0f, tc - reach), t1 = tc + reach;
    const float step = std::max(reach / 300.0f, 1e-6f);
    auto gapAt = [&](float t) {
        const Vec3f q = origin + d * t - c;
        return q.length() - outer(q);
    };
    float best = kInfinity, bestT = tc;
    float prevT = t0;
    for (float t = t0; t <= t1; t += step) {
        const float g = gapAt(t);
        if (g <= 0.0f) {
            float lo = prevT, hi = t;
            for (int k = 0; k < 20; ++k) {
                const float mid = 0.5f * (lo + hi);
                if (gapAt(mid) <= 0.0f) hi = mid;
                else lo = mid;
            }
            out.t = hi;
            out.point = origin + d * hi;
            out.hit = true;
            out.gap = 0.0f;
            return out;
        }
        if (g < best) {
            best = g;
            bestT = t;
        }
        prevT = t;
    }
    out.t = std::min(bestT, tc);
    out.point = origin + d * out.t;
    out.gap = gapAt(out.t);
    return out;
}

std::vector<ShellRegionStats> shellStats(const HairShell& shell, const ScalpMesh& hairScalp) {
    const HeadFrame& f = shell.frame();
    std::vector<ShellRegionStats> out;
    std::vector<std::vector<float>> values(headRegionNames().size());
    for (int i = 0; i < shell.thetaSteps(); ++i)
        for (int j = 0; j < shell.phiSteps(); ++j) {
            const Vec3f d = shell.direction(i, j);
            const Vec3f at = f.center + d * shell.headAt(i, j);
            const SurfacePoint sp = hairScalp.closestPoint(at, 0.1f * f.radius);
            if (sp.root.face < 0) continue;
            const std::string region = headRegion(f, d);
            const auto& names = headRegionNames();
            const std::size_t r = static_cast<std::size_t>(std::find(names.begin(), names.end(), region) - names.begin());
            values[r].push_back(shell.outerAt(i, j) - shell.headAt(i, j));
        }
    for (std::size_t r = 0; r < values.size(); ++r) {
        ShellRegionStats s;
        s.region = headRegionNames()[r];
        std::vector<float>& v = values[r];
        s.directions = static_cast<int>(v.size());
        if (!v.empty()) {
            double sum = 0.0;
            int covered = 0;
            for (const float t : v) {
                sum += t;
                s.max = std::max(s.max, t);
                if (t > f.radius / 20.0f) ++covered;
            }
            s.mean = static_cast<float>(sum / static_cast<double>(v.size()));
            s.covered = static_cast<float>(covered) / static_cast<float>(v.size());
            const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(0.9 * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
            s.p90 = v[k];
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace groomlab
