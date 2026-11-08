// SPDX-License-Identifier: Apache-2.0
#pragma once

// One mesh split into triangles two ways. A groom is rooted on its scalp's triangles (a root's
// face, u, v); a host splits polygons by its own rule (Maya a quad across its shorter diagonal,
// the app's OBJ reader as Maya does: polygonTriangles), so a groom made on a quad mesh in one host
// may meet the same quads split along the other diagonal in another. Its roots move onto the other
// split at the same place on the same surface; its maps (per vertex, per UV) need nothing.

#include <array>
#include <cstdint>
#include <vector>

#include "groomlab/groom.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/vec3.h"

namespace groomlab {

// A polygon's corners (its points in order) split into triangles as Maya's MFnMesh::getTriangles
// splits them (measured in Maya 2022): a quad across its shorter diagonal, corners
// (1, 2, 0), (0, 2, 3) when 0-2 is shorter (its square under 0.999 of 1-3's), else (0, 1, 3),
// (3, 1, 2); a triangle as it is; a larger polygon as a fan from its first corner (Maya's own split
// of those depends on their shape: a host that differs is met by retriangulateGroomData). Local
// corner indices, a triple per triangle (none for fewer than 3 corners).
std::vector<std::array<int, 3>> polygonTriangles(const std::vector<Vec3f>& corners);

// Whether `to` is `from` split otherwise: the same points (count), and both with polygons
// (ScalpMesh::polygons) that match, polygon by polygon, as sets of vertices. False when they are
// the same triangles too (nothing to move) or either has no polygons.
bool isRetriangulation(const ScalpMesh& from, const ScalpMesh& to);

// The groom's roots on `from`'s triangles moved onto `to`'s (isRetriangulation): the flat guides'
// and the hierarchy's roots, the parts and the flow, each to the closest point of the new triangles
// (the same point on a flat polygon), the guides' shapes turned with the surface's normal there (a
// bent quad's two splits differ a little). The scalp and the maps are left to the host (the
// vertex maps' values are per vertex; texture maps read the UVs). Throws std::runtime_error for
// stored data that does not read.
void retriangulateGroomData(GroomData& data, const ScalpMesh& from, const ScalpMesh& to);

}  // namespace groomlab
