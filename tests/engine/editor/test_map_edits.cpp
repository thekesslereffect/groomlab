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
