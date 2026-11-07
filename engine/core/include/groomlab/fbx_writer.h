// SPDX-License-Identifier: Apache-2.0
#pragma once

// A binary FBX (7.4) writer for the meshes the game exports hand to an engine (Unreal and Unity
// cards and their hair cap, the scales' mesh, feathers' rachis): what Maya's fbxmaya and Blender's
// FBX exporter write for the other hosts, for the standalone app, which has neither. Each mesh is
// a model with its polygons, per corner its normals, uv sets and color sets, and a material
// (optionally its base color map). Y up, as the groom is.

#include <string>
#include <utility>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

struct FbxMesh {
    std::string name;                                // the model's (and geometry's) name
    std::vector<Vec3f> points;                       // the groom's space and units (centimeters)
    std::vector<int> faceCounts, faceIndices;        // polygons: corners each, then their points
    std::vector<float> uvs;                          // UV0 ("UVMap"): 2 per corner, or none
    std::vector<Vec3f> normals;                      // per corner, or none (the importer smooths)
    std::vector<std::pair<std::string, std::vector<float>>> uvSets;  // more uv sets, 2 per corner
    std::vector<std::pair<std::string, std::vector<float>>> colors;  // color sets, RGBA per corner (linear)
    std::string material;                            // "" none
    std::string baseColorMap;                        // the material's base color image (a path), "" none
};

struct FbxSettings {
    // Meters in the file (Unity's convention, Maya's FBXExportConvertUnitString m): the points
    // scaled by 0.01 and the file's unit a meter; else centimeters (Unreal's, the groom's own).
    bool meters = false;
    std::string creator = "GroomLab";
};

// Writes the meshes to `path` (binary FBX 7.4, Y up). Throws std::runtime_error when it cannot
// write, std::invalid_argument for a mesh whose arrays do not match its corners.
void writeFbx(const std::string& path, const std::vector<FbxMesh>& meshes, const FbxSettings& settings = {});

}  // namespace groomlab
