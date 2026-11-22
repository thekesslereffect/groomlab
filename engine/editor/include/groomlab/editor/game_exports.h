// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's game exports (engine/editor), every host's: hair cards for Unreal and Unity
// (their maps, the hair cap's, a manifest, the meshes as FBX) and Unity's feathers, from what the
// host evaluated, with one set of options.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "groomlab/card_files.h"
#include "groomlab/engine_cards.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/cards.h"
#include "groomlab/groom_session.h"
#include "groomlab/texture.h"

namespace groomlab {

// A game export, as asked (every host's dialog and scripting).
struct GameExportOptions {
    std::string directory;           // where it goes (made when missing)
    std::string name = "groom";      // the files' base name
    std::string lod;                 // a level of detail: "<name>_<lod>" (unless `nameGiven`)
    bool nameGiven = false;          // the name as given, the LOD not added to it
    int resolution = 0;              // the maps' size (0: the Cards nodes' atlasSize; kMinBakeResolution..kMaxCardBakeResolution)
    float occlusion = 1.0f;          // the cards' ambient occlusion
    bool scalp = true;               // the hair cap (the groom baked onto its scalp) with the cards
    bool writeMeshes = true;         // the meshes written as FBX (the core's writer); false: the host writes them
    // Unity's feathers: points per strand and per barb, the strands' density.
    int points = 16, barbPoints = 6;
    float density = 1.0f;
};

// The base name an export writes under: the name, with "_<lod>" unless the name was given.
std::string gameExportName(const GameExportOptions& o);
// The maps' size asked, kept in range (0 stays 0: the Cards nodes').
int gameExportResolution(int resolution);

// Hair cards written for an engine.
struct EngineCardsExport {
    std::string name;                         // the base name written under
    std::vector<std::string> maps;            // the cards' and the hair cap's maps
    std::map<std::string, std::string> byMap; // each map's file by its role (BaseColor, Depth...)
    std::string manifest;                     // <name>_cards.json (Unreal) or <name>_unity.json
    std::string fbx, scalpCapFbx;             // the meshes' files (written or for the host to write; "" no cap)
    std::vector<std::string> warnings;        // a hair cap left out, and why
    std::vector<std::string> files() const;   // every file written, the maps first
};

// The hair cards of `bake` written for Unreal (TGA maps, Unreal's layout, centimeters) or Unity (PNG
// maps for its materials, meters): the maps; the hair cap's (`cap`: the groom baked onto `capScalp`,
// the host's bake; null: none, `capWhyNot` said); the manifest (engineCardsManifestJson, the cards'
// counts from `stats`, the roughness from `look`); the meshes' FBX (when o.writeMeshes: the cards,
// and the cap on its scalp; Unity's with a material on its base color map). Throws std::runtime_error
// for a file it cannot write.
EngineCardsExport writeEngineCards(const CardBake& bake, const GroomStats& stats, const HairLook& look, const ScalpTextures* cap,
                                   const ScalpMesh* capScalp, const std::string& capWhyNot, bool unity, const GameExportOptions& o);

// Hair cards as glTF 2.0 (every host's): asked as each host's dialog and scripting take it.
struct GltfCardsOptions {
    std::string path;                 // the .glb (binary) or .gltf (its .bin and PNG maps beside it)
    std::string name = "groom";       // the root node; meshes <name>_cards (or _<lod>) and <name>_scalpCap
    bool binary = true;
    std::vector<std::string> lods;    // a mesh per level of detail (the host's bake of each); none: the cards as they are
    int resolution = 0;               // the cards' maps (gameExportResolution)
    int capResolution = 0;            // the hair cap's (0: scalpCapResolution's)
    float occlusion = 1.0f;
    bool cap = true;                  // the hair cap (the groom baked onto its scalp; a scalp without UVs: a warning)
    std::string alphaMode = "mask";   // gltfAlphaModeNamed: mask, blend, opaque
    float alphaCutoff = 0.5f;
    float scale = 0.01f;              // scene units to meters
    bool zUp = false;                 // the scene is Z up
    bool rootToTip = true;
    bool tangents = true;
    std::array<float, 9> colorToRec709 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::string generator;            // the asset's generator ("": the writer's)
};

// A glTF cards export written.
struct GltfCardsExport {
    std::vector<std::string> files;     // the main file first
    std::vector<std::string> meshes;    // the meshes' names, the cap's last
    std::size_t cards = 0;              // the last level's cards
    std::vector<std::string> warnings;  // a hair cap left out, and why
};

// What the host evaluates for it: the cards baked at a level of detail ("": the groom's as they
// are; resolution and occlusion as asked), and the groom at full density with its scalp for the
// hair cap (null with `whyNot` said: no cap, a warning). What either throws goes to the caller.
struct GltfCardsSource {
    std::function<CardBake(const std::string& lod, int resolution, float occlusion)> bake;
    std::function<StrandsPtr(MeshPtr* scalp, std::string* whyNot)> capGroom;
};

// The cards (a mesh per level of detail), the hair cap and their materials (the hair look's
// roughness and specular) written as glTF. Throws std::invalid_argument for options out of range
// (an alpha mode, a scale), std::runtime_error for a file it cannot write.
GltfCardsExport writeGltfCards(const GltfCardsOptions& o, const HairLook& look, const GltfCardsSource& source);

}  // namespace groomlab
