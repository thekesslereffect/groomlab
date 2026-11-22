// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's game exports (engine/editor), every host's: hair cards for Unreal and Unity
// (their maps, the hair cap's, a manifest, the meshes as FBX) and Unity's feathers, from what the
// host evaluated, with one set of options.

#include <map>
#include <string>
#include <vector>

#include "groomlab/card_files.h"
#include "groomlab/engine_cards.h"
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

}  // namespace groomlab
