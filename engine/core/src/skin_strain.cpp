// SPDX-License-Identifier: Apache-2.0
#include "groomlab/skin_strain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "groomlab/parallel.h"

namespace groomlab {

namespace {

bool validTriangle(const ScalpMesh::Triangle& tri, std::size_t points) {
    for (const std::int32_t i : tri) {
        if (i < 0 || static_cast<std::size_t>(i) >= points) return false;
    }
    return true;
}

// Each vertex's neighbours over the triangles' edges (CSR: vertex v's are at [start[v], start[v + 1])),
// sorted and without repeats.
struct Neighbours {
    std::vector<std::size_t> start;
    std::vector<std::int32_t> of;
};

Neighbours neighboursOf(const ScalpMesh& mesh) {
    const std::size_t n = mesh.pointCount();
    std::vector<std::vector<std::int32_t>> lists(n);
    for (const ScalpMesh::Triangle& tri : mesh.triangles()) {
        if (!validTriangle(tri, n)) continue;
        for (int k = 0; k < 3; ++k) {
            const std::int32_t a = tri[static_cast<std::size_t>(k)], b = tri[static_cast<std::size_t>((k + 1) % 3)];
            if (a == b) continue;
            lists[static_cast<std::size_t>(a)].push_back(b);
            lists[static_cast<std::size_t>(b)].push_back(a);
        }
    }
    Neighbours out;
    out.start.resize(n + 1, 0);
    for (std::size_t v = 0; v < n; ++v) {
        std::vector<std::int32_t>& list = lists[v];
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        out.start[v + 1] = out.start[v] + list.size();
    }
    out.of.reserve(out.start[n]);
    for (const auto& list : lists) out.of.insert(out.of.end(), list.begin(), list.end());
    return out;
}

// Each vertex halfway toward its neighbours' mean, `passes` times (a vertex without neighbours stays).
void relax(std::vector<float>& values, const Neighbours& around, int passes) {
    if (passes <= 0 || values.empty()) return;
    std::vector<float> next(values.size());
    for (int pass = 0; pass < passes; ++pass) {
        parallelFor(0, values.size(), [&](std::size_t v) {
            const std::size_t begin = around.start[v], end = around.start[v + 1];
            if (begin == end) {
                next[v] = values[v];
                return;
            }
            double sum = 0.0;
            for (std::size_t k = begin; k < end; ++k) sum += values[static_cast<std::size_t>(around.of[k])];
            next[v] = static_cast<float>(0.5 * values[v] + 0.5 * sum / static_cast<double>(end - begin));
        });
        values.swap(next);
    }
}

}  // namespace

float SkinStrain::at(const std::vector<float>& values, const ScalpMesh& mesh, const StrandRoot& root) {
    if (!mesh.contains(root)) return 0.0f;
    const ScalpMesh::Triangle& tri = mesh.triangles()[static_cast<std::size_t>(root.face)];
    if (!validTriangle(tri, values.size())) return 0.0f;
    const float w = 1.0f - root.u - root.v;
    return values[static_cast<std::size_t>(tri[0])] * w + values[static_cast<std::size_t>(tri[1])] * root.u +
           values[static_cast<std::size_t>(tri[2])] * root.v;
}

SkinStrain skinStrain(const ScalpMesh& posed, int passes) {
    SkinStrain out;
    if (!posed.hasRest() || posed.atRest()) return out;
    const std::vector<Vec3f>& rest = posed.restPoints();
    const std::vector<Vec3f>& now = posed.points();
    if (rest.size() != now.size() || rest.empty()) return out;
    const double restArea = restSurfaceArea(posed);
    if (!(restArea > 0.0)) return out;
    // The pose's uniform scale (a rig scaled at its root): no strain.
    const double scale = std::sqrt(poseFreeArea(posed, now) / restArea);
    if (!(scale > 0.0)) return out;

    // Per triangle: its principal stretches, from the eigenvalues of G^-1 H (G, H the Gram matrices of
    // its two edges from corner 0 at rest and posed: C's in the edges' basis, the same eigenvalues).
    const auto& triangles = posed.triangles();
    const std::size_t count = triangles.size();
    std::vector<float> least(count, 1.0f), most(count, 1.0f), weight(count, 0.0f);
    parallelFor(0, count, [&](std::size_t t) {
        const ScalpMesh::Triangle& tri = triangles[t];
        if (!validTriangle(tri, rest.size())) return;
        const auto a = static_cast<std::size_t>(tri[0]), b = static_cast<std::size_t>(tri[1]), c = static_cast<std::size_t>(tri[2]);
        const Vec3f r1 = rest[b] - rest[a], r2 = rest[c] - rest[a];
        const Vec3f p1 = now[b] - now[a], p2 = now[c] - now[a];
        const double g11 = r1.dot(r1), g12 = r1.dot(r2), g22 = r2.dot(r2);
        const double h11 = p1.dot(p1), h12 = p1.dot(p2), h22 = p2.dot(p2);
        const double detG = g11 * g22 - g12 * g12;
        if (!(detG > 1e-12 * g11 * g22)) return;  // a sliver at rest: no say
        const double trace = (g22 * h11 - 2.0 * g12 * h12 + g11 * h22) / detG;
        const double det = std::max(0.0, (h11 * h22 - h12 * h12) / detG);
        const double root = std::sqrt(std::max(0.0, trace * trace * 0.25 - det));
        const double big = trace * 0.5 + root, small = std::max(0.0, trace * 0.5 - root);
        most[t] = static_cast<float>(std::sqrt(big) / scale);
        least[t] = static_cast<float>(std::sqrt(small) / scale);
        weight[t] = posed.restTriangleArea(t);
    });

    // Per vertex: the triangles around it, by rest area (in triangle order: the same sums every time).
    const std::size_t n = now.size();
    std::vector<double> squashSum(n, 0.0), stretchSum(n, 0.0), weightSum(n, 0.0);
    for (std::size_t t = 0; t < count; ++t) {
        if (!(weight[t] > 0.0f)) continue;
        const double sq = std::clamp(1.0 - static_cast<double>(least[t]), 0.0, 1.0);
        const double st = std::max(0.0, static_cast<double>(most[t]) - 1.0);
        for (const std::int32_t v : triangles[t]) {
            const auto i = static_cast<std::size_t>(v);
            squashSum[i] += sq * weight[t];
            stretchSum[i] += st * weight[t];
            weightSum[i] += weight[t];
        }
    }
    out.squash.assign(n, 0.0f);
    out.stretch.assign(n, 0.0f);
    for (std::size_t v = 0; v < n; ++v) {
        if (!(weightSum[v] > 0.0)) continue;
        out.squash[v] = static_cast<float>(squashSum[v] / weightSum[v]);
        out.stretch[v] = static_cast<float>(stretchSum[v] / weightSum[v]);
    }
    if (passes > 0) {
        const Neighbours around = neighboursOf(posed);
        relax(out.squash, around, passes);
        relax(out.stretch, around, passes);
    }
    return out;
}

float meanRestEdge(const ScalpMesh& mesh) {
    const std::vector<Vec3f>& points = mesh.hasRest() ? mesh.restPoints() : mesh.points();
    double sum = 0.0;
    std::size_t edges = 0;
    for (const ScalpMesh::Triangle& tri : mesh.triangles()) {
        if (!validTriangle(tri, points.size())) continue;
        for (int k = 0; k < 3; ++k) {
            sum += (points[static_cast<std::size_t>(tri[static_cast<std::size_t>((k + 1) % 3)])] -
                    points[static_cast<std::size_t>(tri[static_cast<std::size_t>(k)])])
                       .length();
            ++edges;
        }
    }
    return edges == 0 ? 0.0f : static_cast<float>(sum / static_cast<double>(edges));
}

int skinStrainPasses(const ScalpMesh& mesh, float radius, int most) {
    if (!(radius > 0.0f)) return 0;
    const double h = meanRestEdge(mesh);
    if (!(h > 0.0)) return 0;
    const double passes = std::ceil((static_cast<double>(radius) / h) * (static_cast<double>(radius) / h));
    return static_cast<int>(std::clamp(passes, 0.0, static_cast<double>(std::max(most, 0))));
}

std::vector<Vec3f> vertexGradients(const ScalpMesh& mesh, const std::vector<float>& values) {
    const std::vector<Vec3f>& points = mesh.points();
    std::vector<Vec3f> out(points.size());
    if (values.size() != points.size()) return out;
    std::vector<double> gx(points.size(), 0.0), gy(points.size(), 0.0), gz(points.size(), 0.0), area(points.size(), 0.0);
    for (const ScalpMesh::Triangle& tri : mesh.triangles()) {
        if (!validTriangle(tri, points.size())) continue;
        const auto a = static_cast<std::size_t>(tri[0]), b = static_cast<std::size_t>(tri[1]), c = static_cast<std::size_t>(tri[2]);
        const Vec3f n = (points[b] - points[a]).cross(points[c] - points[a]);
        const float twice = n.length();
        if (!(twice > 0.0f)) continue;
        const Vec3f unit = n / twice;
        // The gradient of the linear function through the corners' values: sum of value_i (n x e_i) / 2A,
        // e_i the edge opposite corner i, counterclockwise about n.
        const Vec3f g = (unit.cross(points[c] - points[b]) * values[a] + unit.cross(points[a] - points[c]) * values[b] +
                         unit.cross(points[b] - points[a]) * values[c]) /
                        twice;
        const double w = 0.5 * static_cast<double>(twice);
        for (const std::size_t v : {a, b, c}) {
            gx[v] += g.x * w;
            gy[v] += g.y * w;
            gz[v] += g.z * w;
            area[v] += w;
        }
    }
    for (std::size_t v = 0; v < points.size(); ++v) {
        if (area[v] > 0.0) {
            out[v] = {static_cast<float>(gx[v] / area[v]), static_cast<float>(gy[v] / area[v]), static_cast<float>(gz[v] / area[v])};
        }
    }
    return out;
}

}  // namespace groomlab
