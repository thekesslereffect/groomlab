// SPDX-License-Identifier: Apache-2.0
#include "groomlab/engine_cards.h"

#include <cmath>
#include <cstdint>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/png.h"

namespace groomlab {

using json = nlohmann::json;

// What the card exports' manifests say about the cards' vertex colors, normals and feather cards
// (Maya's groomlab/cards.py CARD_TINT_NOTE, CARD_NORMALS_NOTE, FEATHER_CARD_NOTE).
const char* const kCardTintNote =
    "Color set groomLabTint (glTF: _TINT, the tint itself): each card's tint, half its value (0.5 none, 1 twice "
    "as light), linear: its own strands' color over the color its shared texture was baked from, root to tip "
    "(textures are shared by shape; the tint gives each card its own color, tips and bands included). "
    "In the material, multiply the Color map's RGB by 2 x the vertex color's RGB (read unconverted, as linear).";
// (Maya's groomlab/game.py AO_WORLD_NOTE.)
const char* const kAoWorldNote =
    "R (gray): ambient occlusion of the whole groom (1 open), linear, in the second UV set (UV1, aoWorld: each card "
    "its own island), so it varies card to card where the shared maps can't";
const char* const kCardNormalsNote =
    "Normals: the strands' own (locked), out of the scalp at the card's roots and leaning along the card as "
    "the groom's lit strands are drawn: shade the cards with them, the same from either side.";
const char* const kFeatherCardNote =
    "A vaned feather's card with featherSpine has three vertices per station, root to tip: the left edge, the "
    "spine (on the rachis, at the baked shaft's U) and the right edge; each half folds out of the vane plane to "
    "follow its vane and has its own normals (the spine's between them). The maps are baked onto the flat card "
    "(each vertex keeps the flat card's UV), with Depth and Tangent in each half's own frame. For wind or "
    "stiffness, the spine is the middle vertex of each station (glTF: TEXCOORD_1 x is 0.5 on it, 0 and 1 at the "
    "edges); down feathers' crossed cards are flat strips of two vertices a station.";

std::string engineCardsManifestJson(bool unity, const EngineCardsInfo& info) {
    json manifest;
    json maps = json::object();
    for (const auto& [map, file] : info.maps) maps[map] = file;
    if (!unity) {
        manifest = {
            {"mesh", info.mesh},
            {"cards", info.cards}, {"triangles", info.triangles}, {"textures", info.textures}, {"feathers", info.feathers},
            {"maps", maps},
            {"channels",
             {{"Depth", "R: distance off the card plane (0.5 on it, higher in front)"},
              {"Coverage", "R: opacity"},
              {"Tangent", "RGB: strand direction in card space (x across U, y root to tip along V, z out), 0.5 + 0.5 * t"},
              {"Attribute", "R: root U, G: root V (scalp UVs), B: seed (random per strand), A: coordU (0 root, 1 tip "
                            "along each strand; on a feather's card along each barb, from the shaft out)"},
              {"Color", "RGB: base color, sRGB"},
              {"AO", "R: ambient occlusion (1 open)"},
              {"AO_world", kAoWorldNote},
              {"Normal", "RGB: the strands' normal as round fibers in tangent space (x across U, y along V, z out), "
                         "DirectX's green (Unreal's), linear"},
              {"ScalpColor", "RGB: the hair's color on the scalp (sRGB), in the scalp's UVs, for the hair cap"},
              {"ScalpCoverage", "R: how much hair covers the skin, in the scalp's UVs"}}},
            {"scalpCap", info.scalpCap.empty() ? json() : json(info.scalpCap)},
            {"uv", "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U. "
                   "UV1 (aoWorld): a unique non-overlapping island per card, for AO_world"},
            {"vertexColor", kCardTintNote},
            {"normals", kCardNormalsNote},
            {"featherCards", kFeatherCardNote}};
    } else {
        const double smoothness = std::round((1.0 - info.roughness) * 1000.0) / 1000.0;
        auto mapOr = [&](const char* m) { return maps.contains(m) ? maps[m] : json(); };
        manifest = {
            {"engine", "Unity"},
            {"mesh", info.mesh},
            {"scalpCap", info.scalpCap.empty() ? json() : json(info.scalpCap)},
            {"units", "meters, Y up (the FBX is converted from the scene's units)"},
            {"cards", info.cards}, {"triangles", info.triangles}, {"textures", info.textures}, {"feathers", info.feathers},
            {"maps", maps},
            {"channels",
             {{"BaseColor", "RGB: base color (sRGB); A: coverage (opacity: alpha clip at 0.5)"},
              {"Mask", "HDRP mask map layout. R: metallic (0); G: ambient occlusion (1 open); "
                       "B: root (0) to tip (1) along the strands; A: smoothness (1 - the hair look's roughness)"},
              {"AO", "R (gray): ambient occlusion (1 open)"},
              {"AO_world", kAoWorldNote},
              {"Tangent", "RGB: strand direction in tangent space (R along +U, G along +V root to tip, B out), "
                          "0.5 + 0.5 * t, linear (import without sRGB)"},
              {"Depth", "R (gray): distance off the card plane (0.5 on it, higher in front), linear"},
              {"Attribute", "R, G: root UV on the scalp; B: seed per strand; A: root (0) to tip (1) along each strand "
                            "(a feather's barbs: from the shaft out), linear"},
              {"Normal", "RGB: the strands' normal as round fibers in tangent space (R along +U, G along +V, B out), "
                         "OpenGL's green, linear (import as a Normal map)"},
              {"ScalpBaseColor", "RGB: the hair's color on the scalp (sRGB); A: how much hair covers the skin"}}},
            {"import",
             {{"textures", "BaseColor and ScalpBaseColor sRGB with Alpha Is Transparency; Mask, AO, AO_world, "
                           "Tangent, Depth and Attribute linear (sRGB off); Normal as Texture Type Normal map"},
              {"model", "Convert Units on (the FBX is in meters, so its scale stays 1)"}}},
            {"materials",
             {{"HDRP",
               {{"shader", "a Shader Graph with the HDRP Hair material (or HDRP/Lit)"},
                {"Base Color Map", mapOr("BaseColor")},
                {"Alpha Clipping", true}, {"Alpha Clip Threshold", 0.5}, {"Double-Sided", true},
                {"Mask Map", mapOr("Mask")},
                {"Normal Map", mapOr("Normal")},
                {"Hair Strand Direction", "Tangent map * 2 - 1 (strands run +V, root to tip)"},
                {"Ambient Occlusion", "AO_world sampled with UV1 (in the Shader Graph), times the Mask map's G"},
                {"Smoothness", smoothness}}},
              {"URP",
               {{"shader", "Universal Render Pipeline/Lit"},
                {"Base Map", mapOr("BaseColor")},
                {"Alpha Clipping", true}, {"Threshold", 0.5}, {"Render Face", "Both"},
                {"Occlusion Map", mapOr("AO")},
                {"Occlusion (whole groom)", "URP/Lit reads occlusion on UV0 only: AO_world needs a Shader Graph sampling it with UV1"},
                {"Normal Map", mapOr("Normal")},
                {"Smoothness", smoothness}}},
              {"scalpCap", "Transparent (alpha blend) with ScalpBaseColor, drawn under the cards"}}},
            {"uv", "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U. "
                   "UV1 (aoWorld): a unique non-overlapping island per card, for AO_world"},
            {"vertexColor", kCardTintNote},
            {"normals", kCardNormalsNote},
            {"featherCards", kFeatherCardNote}};
    }
    return manifest.dump(2);
}

namespace {

std::string outFile(const std::string& directory, const std::string& file) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(directory), ec);
    return (std::filesystem::u8path(directory) / std::filesystem::u8path(file)).generic_u8string();
}

}  // namespace

std::vector<std::string> writeUnityCardMaps(const CardTextures& t, float roughness, const std::string& directory, const std::string& name) {
    const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
    const auto smoothness = static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - roughness)));
    std::vector<std::uint8_t> baseColor(4 * n), mask(4 * n);
    for (std::size_t i = 0; i < n; ++i) {
        baseColor[4 * i] = t.color[3 * i];
        baseColor[4 * i + 1] = t.color[3 * i + 1];
        baseColor[4 * i + 2] = t.color[3 * i + 2];
        baseColor[4 * i + 3] = t.coverage[i];
        mask[4 * i] = t.iridescence.size() == n ? t.iridescence[i] : 0;  // metallic: none; the iridescence, if any
        mask[4 * i + 1] = t.occlusion[i];                                  // ambient occlusion
        mask[4 * i + 2] = t.attribute[4 * i + 3];                          // root (0) to tip (1)
        mask[4 * i + 3] = smoothness;                                      // 1 - the hair look's roughness
    }
    const std::vector<std::uint8_t> normal = cardNormalOpenGl(t);  // Unity's green: OpenGL's
    std::vector<std::string> paths;
    std::vector<PngImage> images;
    auto png = [&](const char* map, int channels, const std::vector<std::uint8_t>& pixels) {
        paths.push_back(outFile(directory, name + "_" + map + ".png"));
        images.push_back({t.size, t.size, channels, &pixels});
    };
    png("BaseColor", 4, baseColor);
    png("Mask", 4, mask);
    png("AO", 1, t.occlusion);
    png("Tangent", 3, t.tangent);
    png("Depth", 1, t.depth);
    png("Attribute", 4, t.attribute);
    png("Normal", 3, normal);
    writeCompressedPngs(paths, images);
    return paths;
}

std::vector<std::string> writeScalpCapMaps(const ScalpTextures& t, bool unity, const std::string& directory, const std::string& name) {
    std::vector<std::string> paths;
    if (unity) {
        const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
        std::vector<std::uint8_t> baseColor(4 * n);
        for (std::size_t i = 0; i < n; ++i) {
            baseColor[4 * i] = t.color[3 * i];
            baseColor[4 * i + 1] = t.color[3 * i + 1];
            baseColor[4 * i + 2] = t.color[3 * i + 2];
            baseColor[4 * i + 3] = t.coverage[i];
        }
        const std::string p = outFile(directory, name + "_ScalpBaseColor.png");
        writeCompressedPng(p, t.size, t.size, 4, baseColor);
        paths.push_back(p);
    } else {
        const std::string color = outFile(directory, name + "_ScalpColor.tga");
        const std::string coverage = outFile(directory, name + "_ScalpCoverage.tga");
        writeTga(color, t.size, t.size, 3, t.color);
        writeTga(coverage, t.size, t.size, 1, t.coverage);
        paths.push_back(color);
        paths.push_back(coverage);
    }
    return paths;
}


std::string unityFeathersManifestJson(const std::string& objectsJson, const std::string& abc, const std::string& fbx, const std::string& lod,
                                      double barbDensity, std::string* groupsJson) {
    const json objects = json::parse(objectsJson);
    bool uniform = true;
    for (const json& o : objects) uniform = uniform && o["minPoints"] == o["maxPoints"];
    json groups = json::array();
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const json& o = objects[index];
        json instance;
        if (o.value("feathers", false)) {
            instance = {{"Skinning", {{"Roots Attach", true},
                                      {"Roots Attach Target", "the rachis's SkinAttachmentTarget (" +
                                                                  (fbx.empty() ? std::string("no rachis written") : fbx) + ")"}}},
                        {"Physics", {{"Global Position", true}, {"Global Position Influence", 1.0}, {"Global Position Interval", "Per 1ms"},
                                     {"Global Rotation", true}, {"Global Rotation Influence", 1.0}, {"Gravity", 0.0},
                                     {"why", "barbs ride the skinned rachis: held to their groomed shape, not simulated"}}},
                        {"Rendering", {{"Material", "a hair material of its own (the feather design's look)"}}}};
        } else {
            instance = {{"Skinning", {{"Roots Attach", true}, {"Roots Attach Target", "the body's SkinAttachmentTarget (the scalp's skinned mesh)"}}},
                        {"Physics", "the package's defaults (simulated)"},
                        {"Rendering", {{"Material", "the hair material"}}}};
        }
        groups.push_back({{"index", index}, {"object", o["object"]}, {"group", o["group"]}, {"groupId", o["groupId"]},
                          {"feathers", o["feathers"]}, {"strands", o["strands"]},
                          {"pointsPerStrand", o["pointsPerStrand"].is_null() ? o["maxPoints"] : o["pointsPerStrand"]},
                          {"hairInstance", instance}});
    }
    const json manifest = {
        {"engine", "Unity"},
        {"packages", {{"com.unity.demoteam.hair", "https://github.com/Unity-Technologies/com.unity.demoteam.hair.git"},
                      {"com.unity.formats.alembic", "2.2.2 or later (Unity's Package Manager)"},
                      {"com.unity.demoteam.digital-human", "https://github.com/Unity-Technologies/com.unity.demoteam.digital-human.git "
                                                           "(0.2.1-preview or later: roots attach with the mesh's twist)"}}},
        {"strands", abc},
        {"rachis", fbx.empty() ? json() : json(fbx)},
        {"lod", lod.empty() ? json() : json(lod)},
        {"barbDensity", barbDensity},
        {"units", "the .abc in centimeters (the Alembic importer's Scale Factor 0.01 makes meters), widths in "
                  "centimeters; the rachis FBX in meters, Y up: both land in the same place"},
        {"import", {{"strands", {{"Scale Factor", 0.01}, {"Swap Handedness", true}, {"Import Curves", true}}},
                    {"rachis", {{"Convert Units", true}, {"then", "skin it to the character's skeleton (or combine it with the "
                                                                  "body before skinning) and give its SkinnedMeshRenderer a SkinAttachmentTarget"}}}}},
        {"hairAsset", {{"Type", "Alembic"}, {"Alembic Asset", abc}, {"Alembic Asset Groups", "Preserve"},
                       {"Alembic Scale Positions", "Data In Meters"}, {"Alembic Scale Diameters", "Data In Centimeters"},
                       {"Resample Curves", !uniform}, {"Root UV", "Resolve From Curves"}, {"Strand Diameter", "Resolve From Curves"},
                       {"Tip Scale", "Resolve From Curves"},
                       {"why", "Preserve keeps every curves object its own strand group (Combine merges neighbours of the same "
                               "point count); Resample Curves on would give every group one Resample Resolution"}}},
        {"groups", groups},
        {"roots", "Unity attaches a strand group's roots (SkinAttachment, attachment type Mesh) to the nearest vertex "
                  "of the target mesh and the triangles round it: every barb root lies on its own rachis's surface "
                  "(rootOnShaft), so attach the feather groups to the rachis mesh, the hair to the body"}};
    if (groupsJson) *groupsJson = groups.dump();
    return manifest.dump(2);
}

}  // namespace groomlab
