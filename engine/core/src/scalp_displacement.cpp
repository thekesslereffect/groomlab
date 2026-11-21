// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_displacement.h"

#include <algorithm>
#include <cmath>

#include "groomlab/random.h"
#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace {

// The points of `mesh` a displacement moves: its piece's, or all of them for the groom's own
// scalp on a mesh of one scalp. False: none (another scalp's name the mesh has no piece of).
bool pointsOf(const ScalpMesh& mesh, const std::string& scalp, std::size_t* first, std::size_t* count) {
    if (const ScalpPiece* piece = mesh.pieces().empty() ? nullptr : mesh.piece(scalp)) {
        *first = piece->firstPoint;
        *count = piece->pointCount;
        return true;
    }
    if (mesh.pieces().empty() && (scalp.empty() || scalp == kMainScalpName)) {
        *first = 0;
        *count = mesh.pointCount();
        return true;
    }
    return false;
}

}  // namespace

std::uint64_t displacementKey(const std::vector<ScalpDisplacement>& displacements) {
    std::uint64_t key = hash64(0xD15B7ACEull);
    for (const ScalpDisplacement& d : displacements) {
        key = hashString(key, d.scalp);
        key = hashString(key, d.path);
        key = hashFloat(hashFloat(key, d.height), d.zero);
        key = hashCombine(key, fileStamp(resolvePath(d.path)));
    }
    return key;
}

std::shared_ptr<const ScalpMesh> displaceScalp(const std::shared_ptr<const ScalpMesh>& scalp,
                                               const std::vector<ScalpDisplacement>& displacements, std::string* error) {
    if (!scalp || scalp->pointCount() == 0 || !scalp->hasUVs()) return nullptr;
    const std::size_t points = scalp->pointCount();
    // Each point's displacement: the map at each of its corners' UVs, averaged.
    std::vector<float> sum(points, 0.0f);
    std::vector<int> corners(points, 0);
    bool any = false;
    const auto& triangles = scalp->triangles();
    const auto& uvs = scalp->cornerUVs();
    for (const ScalpDisplacement& d : displacements) {
        if (d.path.empty() || d.height == 0.0f) continue;
        std::size_t first = 0, count = 0;
        if (!pointsOf(*scalp, d.scalp, &first, &count)) continue;
        TexturePtr texture;
        try {
            texture = loadTexture(resolvePath(d.path));
        } catch (const std::exception& e) {
            if (error) *error = "Scalp displacement: " + std::string(e.what());
            continue;
        }
        if (!texture) {
            if (error) *error = "Scalp displacement: cannot read '" + d.path + "'";
            continue;
        }
        any = true;
        for (std::size_t t = 0; t < triangles.size(); ++t) {
            for (int c = 0; c < 3; ++c) {
                const auto v = static_cast<std::size_t>(triangles[t][static_cast<std::size_t>(c)]);
                if (v < first || v >= first + count) continue;
                const auto& uv = uvs[t * 3 + static_cast<std::size_t>(c)];
                const float value = texture->sample(uv[0], uv[1])[0];
                sum[v] += (value - d.zero) * d.height;
                ++corners[v];
            }
        }
        if (!texture->error().empty() && error) *error = "Scalp displacement: " + texture->error();
    }
    if (!any) return nullptr;
    std::vector<float> offset(points, 0.0f);
    for (std::size_t v = 0; v < points; ++v) offset[v] = corners[v] > 0 ? sum[v] / static_cast<float>(corners[v]) : 0.0f;

    // Along each point's normal, and the rest pose's along its own (a displaced scalp at rest is
    // still at rest).
    auto moved = [&](const std::vector<Vec3f>& from, const std::vector<Vec3f>& normals) {
        std::vector<Vec3f> out(from);
        parallelFor(0, points, [&](std::size_t v) {
            if (offset[v] != 0.0f && v < normals.size()) out[v] = from[v] + normals[v] * offset[v];
        }, 4096);
        return out;
    };
    auto displaced = std::make_shared<ScalpMesh>(scalp->withPoints(moved(scalp->points(), scalp->vertexNormals())));
    if (scalp->hasRest()) {
        const ScalpMesh rest = scalp->restPose();
        displaced->setRestPoints(moved(rest.points(), rest.vertexNormals()));
    }
    return displaced;
}

}  // namespace groomlab
