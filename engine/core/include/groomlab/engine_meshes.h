// SPDX-License-Identifier: Apache-2.0
#pragma once

// The meshes the game exports hand to an engine, as FBX (fbx_writer.h), the way Maya's and
// Blender's exports make them: the cards (Maya's card mesh), their hair cap, the scales bent onto
// the skin, the feathers' rachis tubes. The Blender helper answers the same meshes to Blender's own
// FBX exporter (pipeline.cpp's cardsMesh, scalpCapMesh, scalesMesh, rachisLayoutMesh).

#include <string>

#include "groomlab/fbx_writer.h"

namespace groomlab {

struct CardMesh;
class ScalpMesh;
struct ScaleMesh;
struct RachisMesh;

// The cards: a quad per card segment, UV0 the atlas, UV1 "aoWorld" (each card its own island, the
// layout AO_world is baked in), the strands' own normals, the tint as color set groomLabTint (half
// its value: 0.5 none), linear.
FbxMesh cardsFbxMesh(const CardMesh& cards, const std::string& name);
// The hair cap: the scalp's polygons pushed out along its normals by scalpCapLift, its uvs.
FbxMesh scalpCapFbxMesh(const ScalpMesh& scalp, const std::string& name);
// The scales: triangles, the plates' uvs, uv sets scaleUV (the skin's uv at each scale's root) and
// scaleData (its random, its variant), color sets scaleColor and scaleRootColor, its normals.
FbxMesh scalesFbxMesh(const ScaleMesh& scales, const std::string& name);
// The feathers' rachis tubes (buildRachisMesh), their uvs.
FbxMesh rachisFbxMesh(const RachisMesh& rachis, const std::string& name);

}  // namespace groomlab
