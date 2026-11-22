// SPDX-License-Identifier: Apache-2.0
// The groom editor's map edits (engine/editor map_edits.h), every host's: a rename retargets the
// graph's readers, and a texture map's image follows the name (moved, or copied when shared), its
// files' states kept so an undo puts the image back.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/editor/map_edits.h"
#include "groomlab/stroke.h"
#include "../core/test_helpers.h"

using namespace groomlab;
namespace fs = std::filesystem;

namespace {

fs::path freshFolder(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("groomlab_map_edits_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "masks");
    return dir;
}

void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

TEST(MapEdits, RenameMovesTheImageNamedAfterTheMapAndUndoPutsItBack) {
    const fs::path dir = freshFolder("move");
    writeFile(dir / "masks" / "density.png", "png bytes");
    ScalpMaps maps;
    maps.maps["density"] = ScalpMapEntry{"texture", {}, "masks/density.png"};
    // A Map node and a texture param read it: both follow.
    const std::string graph = wireParamMap(wireParamMap("", maps, "noise", "amplitude", "density", "texture"), maps, "length",
                                           "keep", "density", "texture");
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.groomFolder = dir.generic_u8string();
    files.resolve = [&](const std::string& stored) { return (dir / fs::u8path(stored)).generic_u8string(); };
    const MapRename r = renameMap(maps, nullptr, graph, "density", "thickness", files);
    EXPECT_FALSE(r.guideLayer);
    EXPECT_EQ(r.maps.maps.count("density"), 0u);
    ASSERT_EQ(r.maps.maps.count("thickness"), 1u);
    EXPECT_EQ(r.file, "masks/thickness.png");
    EXPECT_EQ(r.maps.maps.at("thickness").texture, "masks/thickness.png");
    EXPECT_FALSE(fs::exists(dir / "masks" / "density.png"));
    EXPECT_EQ(readFile(dir / "masks" / "thickness.png"), "png bytes");
    ASSERT_FALSE(r.graph.empty());
    EXPECT_EQ(r.graph.find("density.png"), std::string::npos) << "the texture params read the moved image";
    // Undone: the files as they were found.
    restoreFileStates(r.files, false);
    EXPECT_EQ(readFile(dir / "masks" / "density.png"), "png bytes");
    EXPECT_FALSE(fs::exists(dir / "masks" / "thickness.png"));
    restoreFileStates(r.files, true);
    EXPECT_TRUE(fs::exists(dir / "masks" / "thickness.png"));
}

TEST(MapEdits, RenameCopiesASharedImageAndRefusesBadNames) {
    const fs::path dir = freshFolder("copy");
    writeFile(dir / "masks" / "fine.png", "shared");
    ScalpMaps maps;
    maps.maps["fine"] = ScalpMapEntry{"texture", {}, "masks/fine.png"};
    maps.maps["other"] = ScalpMapEntry{"texture", {}, "masks/fine.png"};  // (a duplicate's shared image)
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.resolve = [&](const std::string& stored) { return (dir / fs::u8path(stored)).generic_u8string(); };
    const MapRename r = renameMap(maps, nullptr, "", "fine", "coarse", files);
    EXPECT_TRUE(fs::exists(dir / "masks" / "fine.png")) << "the other map keeps its own";
    EXPECT_EQ(readFile(dir / "masks" / "coarse.png"), "shared");
    EXPECT_EQ(r.maps.maps.at("coarse").texture, (dir / "masks" / "coarse.png").generic_u8string()) << "no groom folder: absolute";
    EXPECT_EQ(r.maps.maps.at("other").texture, "masks/fine.png");
    restoreFileStates(r.files, false);
    EXPECT_FALSE(fs::exists(dir / "masks" / "coarse.png"));
    EXPECT_TRUE(fs::exists(dir / "masks" / "fine.png"));
    // Bad names, no such map, a name taken.
    EXPECT_THROW(renameMap(maps, nullptr, "", "fine", "9bad", files), std::invalid_argument);
    EXPECT_THROW(renameMap(maps, nullptr, "", "nope", "x", files), std::invalid_argument);
    EXPECT_THROW(renameMap(maps, nullptr, "", "fine", "other", files), std::invalid_argument);
    // A vertex map: no file; the Map node reading it follows.
    ScalpMaps vertex;
    vertex.maps["mask"] = ScalpMapEntry{"vertices", {1.0f, 0.5f}, ""};
    const std::string graph = R"({"nodes": [{"name": "m", "type": "Map", "params": {"name": "mask"}}], "connections": []})";
    const MapRename v = renameMap(vertex, nullptr, graph, "mask", "fringe", files);
    EXPECT_TRUE(v.files.empty());
    EXPECT_NE(v.graph.find("fringe"), std::string::npos);
}

TEST(MapEdits, RemoveLetsTheReadersGoAndDeletesOnlyAnImageNothingElseReads) {
    const fs::path dir = freshFolder("remove");
    writeFile(dir / "masks" / "dirt.png", "dirt");
    writeFile(dir / "masks" / "both.png", "both");
    ScalpMaps maps;
    maps.maps["dirt"] = ScalpMapEntry{"texture", {}, "masks/dirt.png"};
    maps.maps["both"] = ScalpMapEntry{"texture", {}, "masks/both.png"};
    maps.maps["copy"] = ScalpMapEntry{"texture", {}, "masks/both.png"};
    const std::string graph = R"({"nodes": [{"name": "m", "type": "Map", "params": {"name": "dirt"}}], "connections": []})";
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.resolve = [&](const std::string& stored) { return (dir / fs::u8path(stored)).generic_u8string(); };
    const MapRemove r = removeMap(maps, nullptr, graph, "dirt", true, files);
    EXPECT_EQ(r.maps.maps.count("dirt"), 0u);
    EXPECT_TRUE(r.fileDeleted);
    EXPECT_FALSE(fs::exists(dir / "masks" / "dirt.png"));
    EXPECT_EQ(r.graph.find("\"m\""), std::string::npos) << "its Map node goes";
    restoreFileStates(r.files, false);
    EXPECT_EQ(readFile(dir / "masks" / "dirt.png"), "dirt");
    // Another map reads the image: it stays; without deleteFile nothing is deleted.
    const MapRemove shared = removeMap(maps, nullptr, "", "both", true, files);
    EXPECT_FALSE(shared.fileDeleted);
    EXPECT_TRUE(fs::exists(dir / "masks" / "both.png"));
    EXPECT_FALSE(removeMap(maps, nullptr, "", "dirt", false, files).fileDeleted);
    EXPECT_TRUE(fs::exists(dir / "masks" / "dirt.png"));
    EXPECT_THROW(removeMap(maps, nullptr, "", "nope", true, files), std::invalid_argument);
}

TEST(MapEdits, ConvertResamplesIntoAnotherKindAndWritesATextureImage) {
    const fs::path dir = freshFolder("convert");
    // A plane with UVs: each corner at its point's (x, z).
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(4));
    std::vector<ScalpMesh::UV> uvs;
    for (std::size_t t = 0; t < plane->triangleCount(); ++t) {
        for (const int v : plane->triangles()[t]) uvs.push_back({plane->points()[v].x, plane->points()[v].z});
    }
    plane->setCornerUVs(std::move(uvs));
    const std::shared_ptr<const ScalpMesh> scalp = plane;
    GuideHierarchy h;
    const GroupId group = h.addGroup("g");
    h.addGuide(group, {scalp->points()[0], scalp->points()[0] + Vec3f{0.0f, 0.5f, 0.0f}});
    h.evaluate(scalp.get());
    ScalpMaps maps;
    maps.maps["mask"] = ScalpMapEntry{"vertices", std::vector<float>(scalp->pointCount(), 0.75f), ""};
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.groomFolder = dir.generic_u8string();
    // Onto the guides: the layer's values, the scalp map gone.
    MapConvert onGuides = convertMapKind(maps, &h, *scalp, "mask", "guides", 16, {}, files);
    EXPECT_EQ(onGuides.maps.maps.count("mask"), 0u);
    EXPECT_TRUE(onGuides.setLayer);
    EXPECT_FALSE(onGuides.removeLayer);
    ASSERT_EQ(onGuides.layer.size(), 1u);
    EXPECT_NEAR(onGuides.layer.begin()->second, 0.75f, 1e-3f);
    GuideHierarchy layered = h;
    applyConvertedLayer(layered, "mask", onGuides);
    EXPECT_EQ(layered.attributeNames(), (std::vector<std::string>{"mask"}));
    // Into a texture: an image in the maps folder, stored relative to the groom, undone by its states.
    MapConvert texture = convertMapKind(maps, &h, *scalp, "mask", "texture", 16, {}, files);
    EXPECT_EQ(texture.file, "masks/mask.png");
    EXPECT_EQ(texture.maps.maps.at("mask").kind, "texture");
    EXPECT_TRUE(fs::exists(dir / "masks" / "mask.png"));
    restoreFileStates(texture.files, false);
    EXPECT_FALSE(fs::exists(dir / "masks" / "mask.png"));
    // The same kind: nothing; bad asks refused.
    EXPECT_TRUE(convertMapKind(maps, &h, *scalp, "mask", "vertices", 16, {}, files).files.empty());
    EXPECT_THROW(convertMapKind(maps, &h, *scalp, "mask", "cloud", 16, {}, files), std::invalid_argument);
    EXPECT_THROW(convertMapKind(maps, &h, *scalp, "nope", "texture", 16, {}, files), std::invalid_argument);
    EXPECT_THROW(convertMapKind(maps, &h, *scalp, "mask", "texture", 16, "a.jpg", files), std::invalid_argument);
}

TEST(MapEdits, FillChecksTheAskAndGivesASharedImageItsOwnCopy) {
    const fs::path dir = freshFolder("fill");
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(4));
    std::vector<ScalpMesh::UV> uvs;
    for (std::size_t t = 0; t < plane->triangleCount(); ++t) {
        for (const int v : plane->triangles()[t]) uvs.push_back({plane->points()[v].x, plane->points()[v].z});
    }
    plane->setCornerUVs(std::move(uvs));
    const std::shared_ptr<const ScalpMesh> scalp = plane;
    const std::size_t n = scalp->pointCount();
    GuideHierarchy h;
    const GroupId group = h.addGroup("g");
    const GuideId first = h.addGuide(group, {scalp->points()[0], scalp->points()[0] + Vec3f{0.0f, 0.5f, 0.0f}});
    const GuideId last = h.addGuide(group, {scalp->points()[n - 1], scalp->points()[n - 1] + Vec3f{0.0f, 0.5f, 0.0f}});
    h.setAttribute(first, "lay", 0.2f);
    h.setAttribute(last, "lay", 0.2f);
    h.evaluate(scalp.get());
    ScalpMaps maps;
    maps.maps["mask"] = ScalpMapEntry{"vertices", std::vector<float>(n, 0.0f), ""};
    maps.maps["tint"] = ScalpMapEntry{"vertices", std::vector<float>(n * 4, 0.25f), ""};
    maps.maps["tint"].channels = kColorMap;
    maps.maps["a"] = ScalpMapEntry{"texture", {}, "masks/shared.png"};  // (a duplicate's shared image)
    maps.maps["b"] = ScalpMapEntry{"texture", {}, "masks/shared.png"};
    writeMapImage(newTextureMapPaint(*scalp, kGrayMap, PaintColor{0.0f, 0.0f, 0.0f, 1.0f}, 16), (dir / "masks" / "shared.png").generic_u8string());
    const std::string sharedBytes = readFile(dir / "masks" / "shared.png");
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.groomFolder = dir.generic_u8string();
    files.resolve = [&](const std::string& stored) { return (dir / fs::u8path(stored)).generic_u8string(); };
    // A vertex map: every value; no file.
    MapFillRequest half;
    half.value = 0.5f;
    const MapFill v = fillMap(maps, &h, scalp.get(), "mask", half, files);
    EXPECT_EQ(v.kind, "vertices");
    EXPECT_TRUE(v.mapsChanged);
    EXPECT_TRUE(v.files.empty());
    for (const float x : v.maps.maps.at("mask").values) EXPECT_FLOAT_EQ(x, 0.5f);
    // A color in g only: r, b and a kept.
    MapFillRequest green;
    green.color = PaintColor{0.0f, 1.0f, 0.0f, 0.5f};
    green.mask = 0x2u;
    const MapFill colored = fillMap(maps, &h, scalp.get(), "tint", green, files);
    const std::vector<float>& tint = colored.maps.maps.at("tint").values;
    EXPECT_FLOAT_EQ(tint[0], 0.25f);
    EXPECT_FLOAT_EQ(tint[1], 1.0f);
    EXPECT_FLOAT_EQ(tint[3], 0.25f);
    // Asks a map does not take.
    EXPECT_THROW(fillMap(maps, &h, scalp.get(), "mask", green, files), std::invalid_argument) << "a color on a gray map";
    MapFillRequest selected;
    selected.value = 0.9f;
    selected.selectedOnly = true;
    EXPECT_THROW(fillMap(maps, &h, scalp.get(), "lay", selected, files), std::invalid_argument) << "no guides selected";
    selected.selected = {last};
    EXPECT_THROW(fillMap(maps, &h, scalp.get(), "mask", selected, files), std::invalid_argument) << "Fill Selected on the scalp";
    EXPECT_THROW(fillMap(maps, &h, scalp.get(), "nope", half, files), std::invalid_argument);
    EXPECT_THROW(fillMap(maps, &h, nullptr, "mask", half, files), std::invalid_argument) << "no scalp";
    // Fill Selected: the selected guide's own value, the other's kept.
    const MapFill g = fillMap(maps, &h, nullptr, "lay", selected, files);
    EXPECT_EQ(g.kind, "guides");
    EXPECT_FALSE(g.mapsChanged);
    EXPECT_FLOAT_EQ(g.layer.at(last), 0.9f);
    EXPECT_FLOAT_EQ(g.layer.at(first), 0.2f);
    GuideHierarchy filled = h;
    applyFilledLayer(filled, "lay", g);
    EXPECT_FLOAT_EQ(filled.guide(last)->attributes.at("lay"), 0.9f);
    // A texture map whose image another map reads: written as its own copy, the other's untouched.
    const MapFill own = fillMap(maps, &h, scalp.get(), "a", half, files);
    EXPECT_EQ(own.file, "masks/a.png");
    EXPECT_TRUE(own.mapsChanged);
    EXPECT_EQ(own.maps.maps.at("a").texture, "masks/a.png");
    EXPECT_EQ(own.maps.maps.at("b").texture, "masks/shared.png");
    EXPECT_EQ(readFile(dir / "masks" / "shared.png"), sharedBytes);
    EXPECT_TRUE(fs::exists(dir / "masks" / "a.png"));
    restoreFileStates(own.files, false);
    EXPECT_FALSE(fs::exists(dir / "masks" / "a.png")) << "undone: the copy goes";
    // Its own image: rewritten in place, its bytes before kept for the undo.
    const MapFill inPlace = fillMap(own.maps, &h, scalp.get(), "b", half, files);
    EXPECT_TRUE(inPlace.file.empty());
    EXPECT_FALSE(inPlace.mapsChanged);
    EXPECT_NE(readFile(dir / "masks" / "shared.png"), sharedBytes);
    restoreFileStates(inPlace.files, false);
    EXPECT_EQ(readFile(dir / "masks" / "shared.png"), sharedBytes);
}

TEST(MapEdits, ChannelsMakeAMapGrayOrColorAndGiveASharedImageItsOwnCopy) {
    const fs::path dir = freshFolder("channels");
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(4));
    std::vector<ScalpMesh::UV> uvs;
    for (std::size_t t = 0; t < plane->triangleCount(); ++t) {
        for (const int v : plane->triangles()[t]) uvs.push_back({plane->points()[v].x, plane->points()[v].z});
    }
    plane->setCornerUVs(std::move(uvs));
    const std::shared_ptr<const ScalpMesh> scalp = plane;
    const std::size_t n = scalp->pointCount();
    GuideHierarchy h;
    const GroupId group = h.addGroup("g");
    const GuideId guide = h.addGuide(group, {scalp->points()[0], scalp->points()[0] + Vec3f{0.0f, 0.5f, 0.0f}});
    h.setAttribute(guide, "lay", 0.5f);
    ScalpMaps maps;
    maps.maps["mask"] = ScalpMapEntry{"vertices", std::vector<float>(n, 0.5f), ""};
    maps.maps["a"] = ScalpMapEntry{"texture", {}, "masks/shared.png"};
    maps.maps["b"] = ScalpMapEntry{"texture", {}, "masks/shared.png"};
    writeMapImage(newTextureMapPaint(*scalp, kGrayMap, PaintColor{0.5f, 0.5f, 0.5f, 1.0f}, 16), (dir / "masks" / "shared.png").generic_u8string());
    const std::string sharedBytes = readFile(dir / "masks" / "shared.png");
    MapFiles files;
    files.mapsFolder = (dir / "masks").generic_u8string();
    files.groomFolder = dir.generic_u8string();
    files.resolve = [&](const std::string& stored) { return (dir / fs::u8path(stored)).generic_u8string(); };
    // A vertex map: r g b the value, alpha 1.
    const MapChannels color = setMapChannels(maps, &h, scalp.get(), "mask", kColorMap, 16, files);
    EXPECT_EQ(color.channels, kColorMap);
    EXPECT_TRUE(color.mapsChanged);
    EXPECT_EQ(color.maps.maps.at("mask").channels, kColorMap);
    ASSERT_EQ(color.maps.maps.at("mask").values.size(), n * 4);
    EXPECT_FLOAT_EQ(color.maps.maps.at("mask").values[3], 1.0f);
    EXPECT_FALSE(setMapChannels(maps, &h, scalp.get(), "mask", kGrayMap, 16, files).mapsChanged) << "gray already";
    // A guide layer stays gray.
    EXPECT_EQ(setMapChannels(maps, &h, scalp.get(), "lay", kGrayMap, 16, files).channels, kGrayMap);
    EXPECT_THROW(setMapChannels(maps, &h, scalp.get(), "lay", kColorMap, 16, files), std::invalid_argument);
    EXPECT_THROW(setMapChannels(maps, &h, scalp.get(), "mask", 3, 16, files), std::invalid_argument);
    EXPECT_THROW(setMapChannels(maps, &h, scalp.get(), "nope", kColorMap, 16, files), std::invalid_argument);
    EXPECT_THROW(setMapChannels(maps, &h, nullptr, "a", kColorMap, 16, files), std::invalid_argument) << "no scalp";
    // A texture map whose image another map reads: its own color copy, the other's untouched.
    const MapChannels own = setMapChannels(maps, &h, scalp.get(), "a", kColorMap, 16, files);
    EXPECT_EQ(own.file, "masks/a.png");
    EXPECT_EQ(own.maps.maps.at("a").texture, "masks/a.png");
    EXPECT_EQ(own.maps.maps.at("a").channels, kColorMap);
    EXPECT_EQ(readFile(dir / "masks" / "shared.png"), sharedBytes);
    restoreFileStates(own.files, false);
    EXPECT_FALSE(fs::exists(dir / "masks" / "a.png"));
}
