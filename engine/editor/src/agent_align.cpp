// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_align.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace groomlab::agent {

namespace {

using Mat3 = CameraMat3;

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

// The rotation by the vector w (its length the angle, radians).
Mat3 rodrigues(const std::array<double, 3>& w) {
    const double angle = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    Mat3 r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (angle < 1e-12) return r;
    const double x = w[0] / angle, y = w[1] / angle, z = w[2] / angle;
    const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    r[0] = {t * x * x + c, t * x * y - s * z, t * x * z + s * y};
    r[1] = {t * x * y + s * z, t * y * y + c, t * y * z - s * x};
    r[2] = {t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    return r;
}

RefCamera moved(const RefCamera& base, const std::array<double, 6>& x) {
    RefCamera c = base;
    c.pose.rotation = mul(rodrigues({x[0], x[1], x[2]}), base.pose.rotation);
    for (int i = 0; i < 3; ++i) c.pose.position[static_cast<std::size_t>(i)] = base.pose.position[static_cast<std::size_t>(i)] + x[3 + i];
    return c;
}

// Distances (pixels) to the nearest marked pixel, two-pass chamfer (3-4), capped at `cap`.
std::vector<float> distanceMap(const std::vector<char>& marked, int w, int h, float cap) {
    std::vector<float> d(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), cap);
    auto at = [&](int x, int y) -> float& { return d[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (marked[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]) at(x, y) = 0.0f;
    const float a = 1.0f, b = 1.41421356f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float& v = at(x, y);
            if (x > 0) v = std::min(v, at(x - 1, y) + a);
            if (y > 0) {
                v = std::min(v, at(x, y - 1) + a);
                if (x > 0) v = std::min(v, at(x - 1, y - 1) + b);
                if (x + 1 < w) v = std::min(v, at(x + 1, y - 1) + b);
            }
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            float& v = at(x, y);
            if (x + 1 < w) v = std::min(v, at(x + 1, y) + a);
            if (y + 1 < h) {
                v = std::min(v, at(x, y + 1) + a);
                if (x + 1 < w) v = std::min(v, at(x + 1, y + 1) + b);
                if (x > 0) v = std::min(v, at(x - 1, y + 1) + b);
            }
        }
    }
    for (float& v : d) v = std::min(v, cap);
    return d;
}

float sampleMap(const std::vector<float>& d, int w, int h, float u, float v, float cap) {
    const float x = u - 0.5f, y = v - 0.5f;
    if (!(x >= 0.0f && y >= 0.0f && x <= static_cast<float>(w - 1) && y <= static_cast<float>(h - 1))) return cap;
    const int x0 = std::min(static_cast<int>(x), w - 2 < 0 ? 0 : w - 2), y0 = std::min(static_cast<int>(y), h - 2 < 0 ? 0 : h - 2);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    auto at = [&](int xx, int yy) { return d[static_cast<std::size_t>(std::min(yy, h - 1)) * static_cast<std::size_t>(w) + static_cast<std::size_t>(std::min(xx, w - 1))]; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// The mesh's coverage of the image through `camera` (1: covered).
std::vector<char> coverage(const ScalpMesh& mesh, const RefCamera& camera, int w, int h) {
    std::vector<char> cov(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    const auto& pts = mesh.points();
    std::vector<std::array<float, 2>> uv(pts.size());
    std::vector<char> ok(pts.size(), 0);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        float u = 0.0f, v = 0.0f, depth = 0.0f;
        if (camera.project(pts[i], u, v, &depth) && depth > 0.0f) uv[i] = {u, v}, ok[i] = 1;
    }
    for (const ScalpMesh::Triangle& t : mesh.triangles()) {
        const std::size_t ia = static_cast<std::size_t>(t[0]), ib = static_cast<std::size_t>(t[1]), ic = static_cast<std::size_t>(t[2]);
        if (!ok[ia] || !ok[ib] || !ok[ic]) continue;
        const auto& A = uv[ia];
        const auto& B = uv[ib];
        const auto& C = uv[ic];
        const float area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
        if (std::abs(area) < 1e-9f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({A[0], B[0], C[0]})))),
                  x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({A[0], B[0], C[0]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({A[1], B[1], C[1]})))),
                  y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({A[1], B[1], C[1]}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float w0 = ((B[0] - px) * (C[1] - py) - (B[1] - py) * (C[0] - px)) / area;
                const float w1 = ((C[0] - px) * (A[1] - py) - (C[1] - py) * (A[0] - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
            }
        }
    }
    return cov;
}

struct Edge {
    std::int32_t a = 0, b = 0;
    std::int32_t f0 = -1, f1 = -1;
};

std::vector<Edge> edgesOf(const ScalpMesh& mesh) {
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> index;
    std::vector<Edge> edges;
    const auto& tris = mesh.triangles();
    for (std::size_t f = 0; f < tris.size(); ++f) {
        for (int k = 0; k < 3; ++k) {
            std::int32_t a = static_cast<std::int32_t>(tris[f][k]), b = static_cast<std::int32_t>(tris[f][(k + 1) % 3]);
            if (a > b) std::swap(a, b);
            const auto it = index.find({a, b});
            if (it == index.end()) {
                index[{a, b}] = edges.size();
                edges.push_back({a, b, static_cast<std::int32_t>(f), -1});
            } else if (edges[it->second].f1 < 0) {
                edges[it->second].f1 = static_cast<std::int32_t>(f);
            }
        }
    }
    return edges;
}

// The mesh's outer outline through `camera`: points on its silhouette edges where the image just
// outside is not covered, and that `usable` accepts.
std::vector<Vec3f> outlineSamples(const ScalpMesh& mesh, const std::vector<Edge>& edges, const RefCamera& camera, int w, int h,
                                  const std::function<bool(const Vec3f&)>& usable, const std::vector<char>& usableFace) {
    const std::vector<char> cov = coverage(mesh, camera, w, h);
    const auto& pts = mesh.points();
    const auto& tris = mesh.triangles();
    const Vec3f eye = camera.eye();
    std::vector<signed char> facing(tris.size(), 0);
    for (std::size_t f = 0; f < tris.size(); ++f) {
        const Vec3f& A = pts[static_cast<std::size_t>(tris[f][0])];
        const Vec3f& B = pts[static_cast<std::size_t>(tris[f][1])];
        const Vec3f& C = pts[static_cast<std::size_t>(tris[f][2])];
        const Vec3f n = (B - A).cross(C - A);
        facing[f] = n.dot(eye - (A + B + C) * (1.0f / 3.0f)) > 0.0f ? 1 : -1;
    }
    auto covered = [&](float u, float v) {
        const int x = static_cast<int>(std::floor(u)), y = static_cast<int>(std::floor(v));
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] != 0;
    };
    std::vector<Vec3f> out;
    for (const Edge& e : edges) {
        if (e.f1 < 0) continue;  // (an open edge: the mesh's cut, not its outline)
        if (facing[static_cast<std::size_t>(e.f0)] == facing[static_cast<std::size_t>(e.f1)]) continue;
        const std::int32_t frontFace = facing[static_cast<std::size_t>(e.f0)] > 0 ? e.f0 : e.f1;
        if (!usableFace.empty() && !usableFace[static_cast<std::size_t>(frontFace)]) continue;
        const Vec3f& A = pts[static_cast<std::size_t>(e.a)];
        const Vec3f& B = pts[static_cast<std::size_t>(e.b)];
        float ua = 0, va = 0, ub = 0, vb = 0;
        if (!camera.project(A, ua, va) || !camera.project(B, ub, vb)) continue;
        // The way out: away from the front face's middle.
        const auto& t = tris[static_cast<std::size_t>(frontFace)];
        const Vec3f mid = (pts[static_cast<std::size_t>(t[0])] + pts[static_cast<std::size_t>(t[1])] + pts[static_cast<std::size_t>(t[2])]) * (1.0f / 3.0f);
        float um = 0, vm = 0;
        if (!camera.project(mid, um, vm)) continue;
        float nx = -(vb - va), ny = ub - ua;
        const float len = std::sqrt(nx * nx + ny * ny);
        if (len < 1e-6f) continue;
        nx /= len, ny /= len;
        const float cu = 0.5f * (ua + ub), cv = 0.5f * (va + vb);
        if ((cu - um) * nx + (cv - vm) * ny < 0.0f) nx = -nx, ny = -ny;
        const int n = std::clamp(static_cast<int>(len / 4.0f), 1, 6);
        for (int k = 0; k < n; ++k) {
            const float s = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
            const float u = ua + (ub - ua) * s, v = va + (vb - va) * s;
            if (covered(u + 2.5f * nx, v + 2.5f * ny)) continue;
            if (u < 1.0f || v < 1.0f || u > static_cast<float>(w - 1) || v > static_cast<float>(h - 1)) continue;
            const Vec3f p = A + (B - A) * s;
            if (usable && !usable(p)) continue;
            out.push_back(p);
        }
    }
    return out;
}

// Solves the 6x6 system A x = b (Gaussian elimination, partial pivoting); false when singular.
bool solve6(std::array<std::array<double, 6>, 6> A, std::array<double, 6> b, std::array<double, 6>& x) {
    for (int c = 0; c < 6; ++c) {
        int p = c;
        for (int r = c + 1; r < 6; ++r)
            if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
        if (std::abs(A[p][c]) < 1e-18) return false;
        std::swap(A[p], A[c]);
        std::swap(b[p], b[c]);
        for (int r = c + 1; r < 6; ++r) {
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < 6; ++k) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 5; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < 6; ++k) s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return true;
}

}  // namespace

RefImage subjectMask(const RefImage& photo) {
    RefImage mask;
    if (photo.empty()) return mask;
    const int w = photo.width, h = photo.height, ch = std::max(1, photo.channels);
    const int patch = std::max(2, std::min(w, h) / 40);
    std::array<std::array<float, 3>, 4> corners{};
    const int cx[4] = {0, w - patch, 0, w - patch}, cy[4] = {0, 0, h - patch, h - patch};
    for (int k = 0; k < 4; ++k) {
        for (int c = 0; c < std::min(ch, 3); ++c) {
            float sum = 0.0f;
            for (int y = cy[k]; y < cy[k] + patch; ++y)
                for (int x = cx[k]; x < cx[k] + patch; ++x) sum += photo.at(x, y, c);
            corners[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)] = sum / static_cast<float>(patch * patch);
        }
    }
    std::array<float, 3> back{};
    for (int c = 0; c < 3; ++c) {
        std::array<float, 4> v{corners[0][static_cast<std::size_t>(c)], corners[1][static_cast<std::size_t>(c)], corners[2][static_cast<std::size_t>(c)],
                               corners[3][static_cast<std::size_t>(c)]};
        std::sort(v.begin(), v.end());
        back[static_cast<std::size_t>(c)] = 0.5f * (v[1] + v[2]);
    }
    mask.width = w;
    mask.height = h;
    mask.channels = 1;
    mask.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float d = 0.0f;
            for (int c = 0; c < std::min(ch, 3); ++c) d = std::max(d, std::abs(photo.at(x, y, c) - back[static_cast<std::size_t>(c)]));
            mask.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = d > 0.08f ? 1.0f : 0.0f;
        }
    }
    return mask;
}

OutlineFit refineWithOutline(const ScalpMesh& mesh, const RefImage& photo, const RefImage& hairMask, const RefCamera& start,
                             const std::vector<CameraCorrespondence>& pairs, const std::function<bool(const Vec3f&)>& usable, int rounds) {
    OutlineFit out;
    out.camera = start;
    if (photo.empty() || mesh.triangleCount() == 0) {
        out.why = "no photo or no mesh";
        return out;
    }
    const int w = photo.width, h = photo.height;
    // The photo's outline that counts: the subject's edge, not where hair is.
    const RefImage subject = subjectMask(photo);
    std::vector<char> edge(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    auto inSubject = [&](int x, int y) { return subject.at(x, y) > 0.5f; };
    const int hairReach = std::max(2, w / 128);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            if (!inSubject(x, y)) continue;
            if (inSubject(x - 1, y) && inSubject(x + 1, y) && inSubject(x, y - 1) && inSubject(x, y + 1)) continue;
            bool hair = false;
            if (!hairMask.empty()) {
                for (int dy = -hairReach; dy <= hairReach && !hair; ++dy)
                    for (int dx = -hairReach; dx <= hairReach && !hair; ++dx) {
                        const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                        const float m = hairMask.width == w && hairMask.height == h
                                            ? hairMask.at(xx, yy)
                                            : hairMask.sample((static_cast<float>(xx) + 0.5f) * static_cast<float>(hairMask.width) / static_cast<float>(w),
                                                              (static_cast<float>(yy) + 0.5f) * static_cast<float>(hairMask.height) / static_cast<float>(h));
                        hair = m > 0.5f;
                    }
            }
            if (hair) continue;
            edge[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
            ++out.edgePixels;
        }
    }
    if (out.edgePixels > w * h / 12) {
        out.edgePixels = 0;
        out.why = "the photo has no plain backdrop to tell the head's outline from (its edge is everywhere)";
        return out;
    }
    if (out.edgePixels < 20) {
        out.why = "the photo shows no outline to match outside the hair (a plain backdrop behind the head is needed)";
        return out;
    }
    // Coarse to fine: the outline pulls from far at first (a rough camera), then only near.
    const float fine = 0.04f * static_cast<float>(w);
    float cap = fine;
    std::vector<float> dist = distanceMap(edge, w, h, cap);
    const std::vector<Edge> edges = edgesOf(mesh);
    std::vector<char> usableFace;
    if (usable) {
        const auto& pts = mesh.points();
        usableFace.resize(mesh.triangleCount(), 0);
        for (std::size_t f = 0; f < mesh.triangleCount(); ++f) {
            const auto& t = mesh.triangles()[f];
            usableFace[f] = usable((pts[static_cast<std::size_t>(t[0])] + pts[static_cast<std::size_t>(t[1])] + pts[static_cast<std::size_t>(t[2])]) *
                                   (1.0f / 3.0f))
                                ? 1
                                : 0;
        }
    }
    auto pairsRms = [&](const RefCamera& c) {
        if (pairs.empty()) return 0.0;
        double sum = 0.0;
        for (const CameraCorrespondence& p : pairs) {
            float u = 0, v = 0;
            if (!c.project(Vec3f(static_cast<float>(p.world[0]), static_cast<float>(p.world[1]), static_cast<float>(p.world[2])), u, v)) {
                sum += static_cast<double>(w) * w;
                continue;
            }
            sum += (u - p.u) * (u - p.u) + (v - p.v) * (v - p.v);
        }
        return std::sqrt(sum / static_cast<double>(pairs.size()));
    };
    auto outlineMean = [&](const RefCamera& c, const std::vector<Vec3f>& samples) {
        if (samples.empty()) return 0.0;
        double sum = 0.0;
        for (const Vec3f& p : samples) {
            float u = 0, v = 0;
            sum += c.project(p, u, v) ? sampleMap(dist, w, h, u, v, cap) : cap;
        }
        return sum / static_cast<double>(samples.size());
    };
    std::vector<Vec3f> samples = outlineSamples(mesh, edges, start, w, h, usable, usableFace);
    if (samples.size() < 10) {
        out.why = "the mesh shows too little outline outside its scalp through this camera";
        return out;
    }
    out.pairsBefore = pairsRms(start);
    out.outlineBefore = outlineMean(start, samples);
    float lo = 1e30f, hi = -1e30f;
    for (const Vec3f& p : mesh.points()) lo = std::min({lo, p.x, p.y, p.z}), hi = std::max({hi, p.x, p.y, p.z});
    const double scale = std::max(1e-6f, hi - lo);
    RefCamera cam = start;
    const std::vector<float> fineDist = dist;
    for (int round = 0; round < rounds; ++round) {
        // (the first rounds with a reach three and two times the final one)
        const float want = round < rounds / 4 ? 3.0f * fine : round < rounds / 2 ? 2.0f * fine : fine;
        if (want != cap) {
            cap = want;
            dist = cap == fine ? fineDist : distanceMap(edge, w, h, cap);
        }
        if (round > 0) samples = outlineSamples(mesh, edges, cam, w, h, usable, usableFace);
        if (samples.size() < 10) break;
        // The outline as much as the pairs (each pair two residuals).
        const double weight = std::sqrt(std::max(2.0, 2.0 * static_cast<double>(pairs.size())) / static_cast<double>(samples.size()));
        auto residuals = [&](const RefCamera& c) {
            std::vector<double> r;
            r.reserve(pairs.size() * 2 + samples.size());
            for (const CameraCorrespondence& p : pairs) {
                float u = 0, v = 0;
                if (!c.project(Vec3f(static_cast<float>(p.world[0]), static_cast<float>(p.world[1]), static_cast<float>(p.world[2])), u, v)) {
                    r.push_back(w), r.push_back(w);
                    continue;
                }
                r.push_back(u - p.u);
                r.push_back(v - p.v);
            }
            for (const Vec3f& p : samples) {
                float u = 0, v = 0;
                r.push_back(weight * (c.project(p, u, v) ? sampleMap(dist, w, h, u, v, cap) : cap));
            }
            return r;
        };
        auto cost = [](const std::vector<double>& r) {
            double s = 0.0;
            for (const double x : r) s += x * x;
            return s;
        };
        double lambda = 1e-3;
        std::vector<double> r0 = residuals(cam);
        double c0 = cost(r0);
        bool moved_any = false;
        for (int it = 0; it < 12; ++it) {
            const std::array<double, 6> steps{1e-4, 1e-4, 1e-4, 1e-4 * scale, 1e-4 * scale, 1e-4 * scale};
            std::vector<std::array<double, 6>> J(r0.size());
            for (int k = 0; k < 6; ++k) {
                std::array<double, 6> x{};
                x[static_cast<std::size_t>(k)] = steps[static_cast<std::size_t>(k)];
                const std::vector<double> r1 = residuals(moved(cam, x));
                for (std::size_t i = 0; i < r0.size(); ++i) J[i][static_cast<std::size_t>(k)] = (r1[i] - r0[i]) / steps[static_cast<std::size_t>(k)];
            }
            std::array<std::array<double, 6>, 6> A{};
            std::array<double, 6> g{};
            for (std::size_t i = 0; i < r0.size(); ++i)
                for (int a = 0; a < 6; ++a) {
                    g[static_cast<std::size_t>(a)] -= J[i][static_cast<std::size_t>(a)] * r0[i];
                    for (int b = 0; b < 6; ++b) A[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] += J[i][static_cast<std::size_t>(a)] * J[i][static_cast<std::size_t>(b)];
                }
            bool better = false;
            for (int tries = 0; tries < 6 && !better; ++tries) {
                auto D = A;
                for (int a = 0; a < 6; ++a) D[static_cast<std::size_t>(a)][static_cast<std::size_t>(a)] *= 1.0 + lambda;
                std::array<double, 6> dx{};
                if (!solve6(D, g, dx)) {
                    lambda *= 10.0;
                    continue;
                }
                const RefCamera next = moved(cam, dx);
                const std::vector<double> r1 = residuals(next);
                const double c1 = cost(r1);
                if (c1 < c0) {
                    cam = next;
                    better = true;
                    moved_any = true;
                    const double gain = c0 - c1;
                    r0 = r1;
                    c0 = c1;
                    lambda = std::max(1e-7, lambda * 0.3);
                    if (gain < 1e-6 * std::max(1.0, c1)) it = 1000;
                } else {
                    lambda *= 10.0;
                }
            }
            if (!better) break;
        }
        if (!moved_any && cap == fine) break;
    }
    cap = fine;
    dist = fineDist;
    samples = outlineSamples(mesh, edges, cam, w, h, usable, usableFace);
    out.ok = true;
    out.camera = cam;
    out.samples = static_cast<int>(samples.size());
    out.pairsAfter = pairsRms(cam);
    out.outlineAfter = outlineMean(cam, samples);
    return out;
}

}  // namespace groomlab::agent
