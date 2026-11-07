// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Unreal and Unity card exports' manifests (<name>_cards.json, <name>_unity.json): what each map
// holds, how to import and shade the cards, as Maya's game.py writes them; every host writes the
// same (the Blender helper, the app).

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace groomlab {

struct CardTextures;
struct ScalpTextures;

// What the card exports' manifests (and glTF's) say about the cards' vertex colors, the AO_world
// map, the normals and feather cards (Maya's CARD_TINT_NOTE, AO_WORLD_NOTE, CARD_NORMALS_NOTE,
// FEATHER_CARD_NOTE).
extern const char* const kCardTintNote;
extern const char* const kAoWorldNote;
extern const char* const kCardNormalsNote;
extern const char* const kFeatherCardNote;

struct EngineCardsInfo {
    std::string mesh;      // the cards' FBX (its file name)
    std::string scalpCap;  // the hair cap's FBX, "" none
    std::size_t cards = 0, triangles = 0, textures = 0, feathers = 0;
    std::map<std::string, std::string> maps;  // map -> its file name
    double roughness = 0.5;                   // the hair look's (Unity's smoothness: 1 - it)
};

// The manifest as JSON text: Unreal's (`unity` false) or Unity's.
std::string engineCardsManifestJson(bool unity, const EngineCardsInfo& info);

// Unity's card maps from a bake (HDRP's layout): <name>_BaseColor (RGB, coverage in A), _Mask
// (metallic, AO, root to tip, smoothness = 1 - roughness), _AO, _Tangent, _Depth, _Attribute and
// _Normal (OpenGL's green) PNGs in `directory`. Returns the files.
std::vector<std::string> writeUnityCardMaps(const CardTextures& textures, float roughness, const std::string& directory,
                                            const std::string& name);
// The hair cap's maps (bakeScalpTextures): Unreal's <name>_ScalpColor and _ScalpCoverage TGAs, or
// Unity's <name>_ScalpBaseColor PNG (coverage in A). Returns the files.
std::vector<std::string> writeScalpCapMaps(const ScalpTextures& textures, bool unity, const std::string& directory,
                                           const std::string& name);

// Unity Feathers' manifest (Maya's export_unity_feathers, <name>_unity_feathers.json): the strands
// .abc's groups (`objectsJson`: [{object, group, groupId, feathers, strands, pointsPerStrand, minPoints,
// maxPoints}], EngineGroomWriter's objects), the rachis FBX (`fbx`, "" none), the LOD and barb
// density; how to import them with Unity's hair package. `groupsJson` gets the groups' part.
std::string unityFeathersManifestJson(const std::string& objectsJson, const std::string& abc, const std::string& fbx, const std::string& lod,
                                      double barbDensity, std::string* groupsJson = nullptr);

}  // namespace groomlab
