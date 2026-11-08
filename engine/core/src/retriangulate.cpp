// SPDX-License-Identifier: Apache-2.0
#include "groomlab/retriangulate.h"

#include <algorithm>
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
