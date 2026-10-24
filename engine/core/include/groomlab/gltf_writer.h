// SPDX-License-Identifier: Apache-2.0
#pragma once

// glTF 2.0 writer for game cards, with no dependency beyond the JSON library
// the core already uses: a document of meshes (one named node each, under
// an optional root node), PBR metallic-roughness materials and PNG images,
// written as a binary .glb (JSON and buffer chunks, images inside the
// buffer) or a .gltf with its .bin and PNGs beside it.
//
// The writer checks what it writes: attributes as long as the positions,
// indices in range, no NaN or infinite values, material and texture indices
// that exist. Every buffer view starts on a 4-byte boundary, POSITION
// accessors carry min and max, and indices are UNSIGNED_SHORT up to 65535
// vertices and UNSIGNED_INT past that.
//
// The cards helpers turn a CardMesh with its bake (and the hair cap: the scalp
// with its scalp bake) into meshes, materials and textures in glTF's
// conventions: Y up, meters (scaled from the scene's units), UV origin at the
// image's top left (V flipped from Maya's).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/math.h"

namespace groomlab {

class ScalpMesh;
class StrandSet;
struct CardMesh;
struct CardTextures;
struct ScalpTextures;

struct GltfTexture {
    std::string name;
    std::vector<std::uint8_t> png;  // the image file's bytes
};

struct GltfMaterial {
    enum class AlphaMode { Opaque, Mask, Blend };
    std::string name;
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    int baseColorTexture = -1;          // an index into GltfDocument::textures; -1: none
    int metallicRoughnessTexture = -1;  // G roughness, B metallic
    int occlusionTexture = -1;          // R occlusion
    int normalTexture = -1;             // tangent space, OpenGL's green (y up)
    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusionStrength = 1.0f;
    AlphaMode alphaMode = AlphaMode::Mask;
    float alphaCutoff = 0.5f;           // written for Mask only
    bool doubleSided = true;
    std::string extrasJson;             // a JSON object for the material's "extras" ("" : none)
};

struct GltfPrimitive {
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;                  // empty or one per position
    std::vector<std::array<float, 4>> tangents;  // xyz unit, w +1/-1; empty or one per position
    std::vector<std::array<float, 2>> uv0, uv1;  // TEXCOORD_0/1, glTF's orientation (V down)
    std::vector<std::array<float, 4>> colors;    // COLOR_0 (linear RGBA)
    // _TINT (an application attribute, linear RGB): game cards' tint (CardMesh::tints), which a
    // material multiplies the base color by. Not COLOR_0, which glTF viewers multiply the base
    // color by as it is: a tint goes up to 2.
    std::vector<std::array<float, 3>> tints;
    std::vector<std::uint32_t> indices;          // triangles
    int material = -1;
};

struct GltfMesh {
    std::string name;  // also its node's name
    std::vector<GltfPrimitive> primitives;
};

struct GltfDocument {
    std::vector<GltfMesh> meshes;  // one node each
    std::string rootName;          // non-empty: the mesh nodes are children of one node of this name
    std::vector<GltfMaterial> materials;
    std::vector<GltfTexture> textures;
    std::string generator = "GroomLab";
    std::string extrasJson;        // a JSON object for the asset's "extras" ("" : none)
};

// The .glb bytes. Throws std::invalid_argument when the document is inconsistent.
std::vector<std::uint8_t> encodeGlb(const GltfDocument& document);

// Writes `path`: binary, a .glb; else a .gltf whose buffer is <stem>.bin and
// whose images are <stem>_<texture name>.png beside it. Written through
// "<file>.tmp" and moved into place. Returns the files written (the main file
// first). Throws std::invalid_argument (bad document) or std::runtime_error.
std::vector<std::string> writeGltf(const std::string& path, const GltfDocument& document, bool binary = true);

// Per-vertex tangents for a triangle mesh from its positions, normals and a UV
// set given in Maya's orientation (V up): xyz along +U, made perpendicular to
// the normal, w the handedness so that cross(normal, xyz) * w runs along +V.
std::vector<std::array<float, 4>> meshTangents(const std::vector<Vec3f>& positions, const std::vector<Vec3f>& normals,
                                               const std::vector<std::array<float, 2>>& uvs,
                                               const std::vector<std::uint32_t>& indices);

// ---- Cards --------------------------------------------------------------------

struct CardGltfSettings {
    std::string name = "cards";    // mesh / node name; materials and textures are named from it
    float scale = 0.01f;           // scene units to meters (Maya's centimeters: 0.01)
    bool zUp = false;              // the scene is Z up: rotate to glTF's Y up
    bool tangents = true;          // TANGENT: +U across the card, the bitangent root to tip
    // TEXCOORD_1: x across the card 0..1 by column (a feather card's spine, its middle column, at
    // 0.5 wherever the rachis sits: |2x - 1| is 0 on it and 1 at the edges), y root 0 to tip 1.
    bool rootToTip = true;
    GltfMaterial::AlphaMode alphaMode = GltfMaterial::AlphaMode::Mask;
    float alphaCutoff = 0.5f;
    float roughness = 0.5f;        // the hair look's roughness
    float specular = 0.3f;         // the hair look's specular (kept in the material's extras)
};

// Adds the cards as one mesh (one primitive) with a material and two
// textures: <name>_BaseColor (RGBA: the baked color in RGB, sRGB, coverage in
// alpha) and <name>_ORM (R ambient occlusion, G 255 and B 255 so the
// roughness and metallic factors apply as they are). Returns the mesh index.
int addCardsToGltf(GltfDocument& document, const CardMesh& cards, const CardTextures& textures,
                   const CardGltfSettings& settings);

// Adds the hair cap: the scalp (it needs UVs) pushed out along its normals by
// `lift` scene units, as one mesh with a material whose <name>_BaseColor holds
// the scalp bake's color and coverage (alpha). Returns the mesh index.
int addScalpCapToGltf(GltfDocument& document, const ScalpMesh& scalp, const ScalpTextures& textures, float lift,
                      const CardGltfSettings& settings);

// The hair cap of a cards export as every host writes it: the scalp baked from `strands`
// (bakeScalpTextures) at scalpCapResolution, lifted scalpCapLift off the skin, named
// `settings.name`, no root-to-tip coordinates. Throws std::invalid_argument for a scalp without UVs.
int addScalpCapToGltf(GltfDocument& document, const StrandSet& strands, const ScalpMesh& scalp, int capResolution,
                      int resolution, const CardGltfSettings& settings);
// The cap's bake resolution: `capResolution` when above 0 (clamped to 16..16384), else the
// cards' `resolution` when above 0, else 2048.
int scalpCapResolution(int capResolution, int resolution);
// How far the cap sits above the skin: 0.4% of the scalp's size (its points' largest extent).
float scalpCapLift(const ScalpMesh& scalp);

}  // namespace groomlab
