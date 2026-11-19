// SPDX-License-Identifier: Apache-2.0
#include "groomlab/engine_meshes.h"

#include <algorithm>

#include "groomlab/cards.h"
#include "groomlab/feathers.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/retriangulate.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

FbxMesh cardsFbxMesh(const CardMesh& cards, const std::string& name) {
    FbxMesh m;
    m.name = name;
    m.points = cards.positions;
    const std::size_t quads = cards.quads.size() / 4;
    m.faceCounts.assign(quads, 4);
    m.faceIndices.reserve(cards.quads.size());
    // The unique layout: as the bake laid it out, else laid out again (cards that carry their stations).
    const std::size_t n = cards.positions.size();
    std::vector<float> worldU, worldV;
    cardsUniqueUVs(cards, worldU, worldV);  // every host's (cards.h)
    std::vector<float> aoWorld, tint;
    for (const std::uint32_t v : cards.quads) {
        m.faceIndices.push_back(static_cast<int>(v));
        m.uvs.push_back(v < cards.u.size() ? cards.u[v] : 0.0f);
        m.uvs.push_back(v < cards.v.size() ? cards.v[v] : 0.0f);
        if (!worldU.empty()) aoWorld.insert(aoWorld.end(), {v < n ? worldU[v] : 0.0f, v < n ? worldV[v] : 0.0f});
        m.normals.push_back(v < cards.normals.size() ? cards.normals[v] : Vec3f{0.0f, 1.0f, 0.0f});
        const std::array<float, 3> t = v < cards.tints.size() ? cards.tints[v] : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        tint.insert(tint.end(), {0.5f * t[0], 0.5f * t[1], 0.5f * t[2], 1.0f});
    }
    if (!aoWorld.empty()) m.uvSets.emplace_back("aoWorld", std::move(aoWorld));
    m.colors.emplace_back("groomLabTint", std::move(tint));
    return m;
}

FbxMesh scalpCapFbxMesh(const ScalpMesh& scalp, const std::string& name) {
    FbxMesh m;
    m.name = name;
    const float lift = scalpCapLift(scalp);
    const std::vector<Vec3f>& normals = scalp.vertexNormals();
    m.points = scalp.points();
    for (std::size_t i = 0; i < m.points.size() && i < normals.size(); ++i) m.points[i] = m.points[i] + normals[i] * lift;
    // The host's polygons (retriangulate.h polygonLoops: each polygon's corners from its triangles,
    // whatever split they are), else a face of each triangle: every host's cap the same.
    const auto& tris = scalp.triangles();
    const auto& cornerUVs = scalp.cornerUVs();
    const bool hasUVs = scalp.hasUVs();
    auto corner = [&](int vertex, std::size_t triangleCorner) {
        m.faceIndices.push_back(vertex);
        if (hasUVs) {
            m.uvs.push_back(cornerUVs[triangleCorner][0]);
            m.uvs.push_back(cornerUVs[triangleCorner][1]);
        }
    };
    const PolygonLoops loops = polygonLoops(scalp);
    if (loops.vertices.empty()) {
        for (std::size_t t = 0; t < tris.size(); ++t) {
            m.faceCounts.push_back(3);
            for (std::size_t c = 0; c < 3; ++c) corner(static_cast<int>(tris[t][c]), 3 * t + c);
        }
        return m;
    }
    for (std::size_t f = 0; f < loops.vertices.size(); ++f) {
        m.faceCounts.push_back(static_cast<int>(loops.vertices[f].size()));
        for (std::size_t k = 0; k < loops.vertices[f].size(); ++k) corner(loops.vertices[f][k], static_cast<std::size_t>(loops.corners[f][k]));
    }
    return m;
}

FbxMesh scalesFbxMesh(const ScaleMesh& sm, const std::string& name) {
    FbxMesh m;
    m.name = name;
    m.points = sm.points;
    const std::size_t faces = sm.triangleCount();
    m.faceCounts.assign(faces, 3);
    const std::vector<std::uint32_t> owner = sm.triangleOwners();
    std::vector<float> scaleUV, scaleData, color, rootColor;
    for (std::size_t c = 0; c < sm.triangles.size(); ++c) {
        const std::uint32_t v = sm.triangles[c];
        m.faceIndices.push_back(static_cast<int>(v));
        m.uvs.push_back(sm.uvs[v][0]);
        m.uvs.push_back(sm.uvs[v][1]);
        m.normals.push_back(sm.normals[v]);
        color.insert(color.end(), {sm.colors[v][0], sm.colors[v][1], sm.colors[v][2], 1.0f});
        if (sm.scales.empty()) continue;
        const ScaleMesh::Scale& sc = sm.scales[owner[c / 3]];
        scaleUV.insert(scaleUV.end(), {sc.rootUv[0], sc.rootUv[1]});
        scaleData.insert(scaleData.end(), {sc.random, static_cast<float>(sc.variant)});
        rootColor.insert(rootColor.end(), {sc.color[0], sc.color[1], sc.color[2], 1.0f});
    }
    if (!sm.scales.empty()) {
        m.uvSets.emplace_back("scaleUV", std::move(scaleUV));
        m.uvSets.emplace_back("scaleData", std::move(scaleData));
    }
    m.colors.emplace_back("scaleColor", std::move(color));
    if (!sm.scales.empty()) m.colors.emplace_back("scaleRootColor", std::move(rootColor));
    return m;
}

FbxMesh rachisFbxMesh(const RachisMesh& r, const std::string& name) {
    FbxMesh m;
    m.name = name;
    m.points = r.points;
    m.faceCounts.assign(r.faceCounts.begin(), r.faceCounts.end());
    m.faceIndices.assign(r.faceVertices.begin(), r.faceVertices.end());
    if (r.faceUVs.size() == r.faceVertices.size()) {
        m.uvs.reserve(2 * r.faceUVs.size());
        for (const int k : r.faceUVs) {
            const auto i = static_cast<std::size_t>(k);
            m.uvs.push_back(i < r.u.size() ? r.u[i] : 0.0f);
            m.uvs.push_back(i < r.v.size() ? r.v[i] : 0.0f);
        }
    }
    return m;
}

}  // namespace groomlab
