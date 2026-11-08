// SPDX-License-Identifier: Apache-2.0
#include "groomlab/retriangulate.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/parts.h"
#include "groomlab/transfer.h"

namespace groomlab {

namespace {

float lengthSquared(const Vec3f& a, const Vec3f& b) {
    const Vec3f d{a.x - b.x, a.y - b.y, a.z - b.z};
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Each polygon's vertices, sorted (its triangles' corners).
std::vector<std::vector<std::int32_t>> polygonVertexSets(const ScalpMesh& mesh) {
    std::vector<std::vector<std::int32_t>> out;
    const auto& tris = mesh.triangles();
    std::size_t t = 0;
    for (const std::int32_t count : mesh.polygons()) {
        std::vector<std::int32_t> vertices;
        for (std::int32_t k = 0; k < count && t < tris.size(); ++k, ++t) {
            vertices.insert(vertices.end(), tris[t].begin(), tris[t].end());
        }
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
        out.push_back(std::move(vertices));
    }
    return out;
}

}  // namespace

std::vector<std::array<int, 3>> polygonTriangles(const std::vector<Vec3f>& corners) {
    std::vector<std::array<int, 3>> out;
    const int n = static_cast<int>(corners.size());
    if (n < 3) return out;
    if (n == 4) {
        // Maya's split (getTriangles): across 0-2 only when it is shorter by more than a hair,
        // else across 1-3 (a square, a rectangle: equal diagonals).
        if (lengthSquared(corners[0], corners[2]) < 0.999f * lengthSquared(corners[1], corners[3])) {
            out.push_back({1, 2, 0});
            out.push_back({0, 2, 3});
        } else {
            out.push_back({0, 1, 3});
            out.push_back({3, 1, 2});
        }
        return out;
    }
    for (int k = 1; k + 1 < n; ++k) out.push_back({0, k, k + 1});
    return out;
}

PolygonLoops polygonLoops(const ScalpMesh& mesh) {
    PolygonLoops out;
    const auto& tris = mesh.triangles();
    const std::vector<std::int32_t>& polygons = mesh.polygons();
    if (polygons.empty()) return out;
    std::size_t t = 0;
    for (const std::int32_t count : polygons) {
        // Directed edges of the polygon's triangles; an inner edge comes once each way.
        std::map<std::pair<int, int>, int> edges;  // (from, to) -> the triangle corner at `from`
        for (std::int32_t k = 0; k < count; ++k, ++t) {
            if (t >= tris.size()) return {};
            for (int c = 0; c < 3; ++c) edges[{tris[t][c], tris[t][(c + 1) % 3]}] = static_cast<int>(3 * t) + c;
        }
        std::map<int, std::pair<int, int>> next;  // vertex -> (the next corner's vertex, its triangle corner)
        for (const auto& [edge, corner] : edges) {
            if (edges.count({edge.second, edge.first})) continue;
            if (next.count(edge.first)) return {};  // not a single loop
            next[edge.first] = {edge.second, corner};
        }
        if (next.size() != static_cast<std::size_t>(count) + 2) return {};
        std::vector<int> loop, corners;
        int v = next.begin()->first;
        for (std::size_t k = 0; k < next.size(); ++k) {
            const auto it = next.find(v);
            if (it == next.end()) return {};
            loop.push_back(v);
            corners.push_back(it->second.second);
            v = it->second.first;
        }
        if (v != loop.front()) return {};
        // The corner to start from: the one Maya's split (polygonTriangles: getTriangles) gives the
        // file's triangles from, in their order and corner order (a file Maya or the app wrote:
        // the roots stay on their triangles); none (another host's split): the walk's.
        const std::size_t n = loop.size(), first = t - static_cast<std::size_t>(count);
        for (std::size_t r = 0; r < n; ++r) {
            std::vector<Vec3f> points;
            for (std::size_t k = 0; k < n; ++k) points.push_back(mesh.points()[static_cast<std::size_t>(loop[(r + k) % n])]);
            const std::vector<std::array<int, 3>> split = polygonTriangles(points);
            bool same = split.size() == static_cast<std::size_t>(count);
            for (std::size_t s = 0; same && s < split.size(); ++s) {
                for (int c = 0; same && c < 3; ++c) same = loop[(r + static_cast<std::size_t>(split[s][c])) % n] == tris[first + s][c];
            }
            if (!same) {
                if (r + 1 == n) out.exact = false;
                continue;
            }
            std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(r), loop.end());
            std::rotate(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(r), corners.end());
            break;
        }
        out.vertices.push_back(std::move(loop));
        out.corners.push_back(std::move(corners));
    }
    return out;
}

std::shared_ptr<ScalpMesh> withTrianglesOf(const ScalpMesh& mesh, const ScalpMesh& split) {
    if (!isRetriangulation(split, mesh)) return nullptr;
    // Each corner of `mesh` by (polygon, vertex).
    std::map<std::pair<std::size_t, int>, std::size_t> cornerOf;
    {
        std::size_t t = 0;
        for (std::size_t p = 0; p < mesh.polygons().size(); ++p) {
            for (std::int32_t k = 0; k < mesh.polygons()[p]; ++k, ++t) {
                for (int c = 0; c < 3; ++c) cornerOf[{p, mesh.triangles()[t][c]}] = 3 * t + static_cast<std::size_t>(c);
            }
        }
    }
    std::vector<std::size_t> from;  // each new triangle corner's corner of `mesh`
    std::size_t t = 0;
    for (std::size_t p = 0; p < split.polygons().size(); ++p) {
        for (std::int32_t k = 0; k < split.polygons()[p]; ++k, ++t) {
            for (int c = 0; c < 3; ++c) from.push_back(cornerOf.at({p, split.triangles()[t][c]}));
        }
    }
    auto out = std::make_shared<ScalpMesh>(mesh.points(), split.triangles());
    out->setPolygons(mesh.polygons());
    if (mesh.hasUVs()) {
        std::vector<ScalpMesh::UV> uvs;
        for (const std::size_t c : from) uvs.push_back(mesh.cornerUVs()[c]);
        out->setCornerUVs(std::move(uvs));
    }
    for (const std::string& set : mesh.colorSetNames()) {
        const std::vector<ScalpMesh::Color>& colors = *mesh.cornerColors(set);
        std::vector<ScalpMesh::Color> moved;
        for (const std::size_t c : from) moved.push_back(colors[c]);
        out->setCornerColors(set, std::move(moved));
    }
    if (mesh.hasRest()) out->setRestPoints(mesh.restPoints());
    return out;
}

bool isRetriangulation(const ScalpMesh& from, const ScalpMesh& to) {
    if (from.pointCount() != to.pointCount() || from.polygons().empty() || to.polygons().empty()) return false;
    if (from.polygons() != to.polygons() || from.triangleCount() != to.triangleCount()) return false;
    if (from.triangles() == to.triangles()) return false;
    return polygonVertexSets(from) == polygonVertexSets(to);
}

void retriangulateGroomData(GroomData& data, const ScalpMesh& from, const ScalpMesh& to) {
    TransferOptions options;
    options.method = TransferMethod::Closest;
    const ScalpMapping mapping(from, to, options);
    if (data.guides) {
        auto guides = std::make_shared<GuideSet>(*data.guides);
        for (Guide& g : guides->guides()) {
            if (g.root.face < 0) continue;
            const MappedPoint m = mapping.map(g.root);
            if (m.target.root.face >= 0) g.root = m.target.root;
        }
        data.guides = guides;
    }
    if (!data.hierarchyJson.empty()) {
        GuideHierarchy h = GuideHierarchy::fromJson(data.hierarchyJson);
        h.transfer(mapping);
        data.hierarchyJson = h.toJson();
    }
    if (!data.partsJson.empty()) {
        const PartSet parts = PartSet::fromJson(data.partsJson);
        if (!parts.empty()) data.partsJson = parts.transferred(mapping).toJson();
    }
    if (!data.flowJson.empty()) {
        const FlowSet flow = FlowSet::fromJson(data.flowJson);
        if (!flow.empty()) data.flowJson = flow.transferred(mapping).toJson();
    }
}

}  // namespace groomlab
