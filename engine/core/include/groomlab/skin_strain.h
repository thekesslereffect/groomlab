// SPDX-License-Identifier: Apache-2.0
#pragma once

// Skin strain: how far a deforming scalp's skin squashed and stretched away from its rest pose,
// per vertex, for nodes that make the hair react to it (SkinResponse).
//
// Each triangle's deformation, rest to posed, has two principal stretches (the lengths a unit
// circle on the rest triangle takes along its two axes once posed: the square roots of the
// eigenvalues of C = F^T F, F the map from the rest triangle's plane to the posed one's). Both are
// divided by the pose's uniform scale (the square root of the median of the triangles' area
// ratios, poseFreeArea's), so a rig scaled at its root reads as no strain; the least one under 1 is
// a squash (1 - it), the greatest one over 1 a stretch (it - 1). Per vertex: the area-weighted
// mean of the triangles around it (rest areas), then relaxed over the mesh's edges `passes` times
// (each vertex toward its neighbours' mean). The principal stretches are frame-free scalars, so the
// means never compare directions measured in different triangles' frames.

#include <cstddef>
#include <vector>

#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"
#include "groomlab/vec3.h"

namespace groomlab {

struct SkinStrain {
    std::vector<float> squash;   // per vertex, >= 0: 1 - the least principal stretch (0: none)
    std::vector<float> stretch;  // per vertex, >= 0: the greatest principal stretch - 1 (0: none)

    // No strain measured: a scalp without a rest pose, at rest, or with no area.
    bool empty() const { return squash.empty(); }
    // A per-vertex value at a root on a mesh of the same triangles (barycentric); 0 off the mesh.
    static float at(const std::vector<float>& values, const ScalpMesh& mesh, const StrandRoot& root);
};

// The strain of `posed` against its rest points (ScalpMesh::restPoints), relaxed `passes` times;
// empty without a rest pose, at rest, or when the rest pose has no area.
SkinStrain skinStrain(const ScalpMesh& posed, int passes);

// The mean length of `mesh`'s triangle edges on its rest pose (the mesh as it is without one); 0
// for no edges.
float meanRestEdge(const ScalpMesh& mesh);
// Relaxing passes for a smoothing radius `radius` (scene units) on `mesh`: a diffusion of n passes
// over edges of mean length h spreads about h * sqrt(n), so n = (radius / h)^2, at most `most`.
int skinStrainPasses(const ScalpMesh& mesh, float radius, int most = 256);

// The surface gradient of a per-vertex field over `mesh` at each vertex: each triangle's (of the
// linear function through its corners' values) averaged over the triangles around the vertex by
// area. A direction in `mesh`'s space, along its surface, per unit of length.
std::vector<Vec3f> vertexGradients(const ScalpMesh& mesh, const std::vector<float>& values);

}  // namespace groomlab
