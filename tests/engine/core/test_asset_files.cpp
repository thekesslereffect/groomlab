// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/asset_files.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_package.h"

using namespace groomlab;
namespace fs = std::filesystem;

namespace {

std::string scratch(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("groomlab_asset_files_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.generic_u8string();
}

void write(const std::string& path, const std::string& text) {
    fs::create_directories(fs::u8path(path).parent_path());
    std::ofstream(fs::u8path(path), std::ios::binary) << text;
}

std::string read(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(AssetFiles, FoldersAndNamesAsMayaKeepsThem) {
    EXPECT_EQ(assetFolder("C:/shots", "shot_v001", "masks"), "C:/shots/groomlab/shot_v001/masks");
    EXPECT_EQ(assetFolder("C:/shots", "shot_v001"), "C:/shots/groomlab/shot_v001");
    EXPECT_EQ(fileSafeName("|charA|hair"), "charA_hair");
    EXPECT_EQ(fileSafeName("charA:hair"), "charA_hair");
    EXPECT_EQ(fileSafeName("hair.v2."), "hair.v2");
    EXPECT_EQ(fileSafeName(""), "groom");
}

// Save As into another folder (and another name): the old scene's files go to the same place in
// the new scene's folders, relative paths stay relative, other absolute paths stay, nothing of the
// old scene changes.
TEST(AssetFiles, SaveAsTakesTheScenesFilesAlong) {
    const std::string root = scratch("follow");
    const std::string oldBase = root + "/old", newBase = root + "/new";
    write(oldBase + "/groomlab/shot/masks/crown.png", "crown");
    write(oldBase + "/groomlab/shot/cards/hair_Color.tga", "color");
    write(oldBase + "/groomlab/shot/cards/hair_Atlas.json", "atlas");
    write(oldBase + "/images/tint.png", "tint");
    write(root + "/library/shared.png", "shared");
    FileFollower f(oldBase, assetFolder(oldBase, "shot"), newBase, assetFolder(newBase, "shot_v2"));
    EXPECT_EQ(f.follow("groomlab/shot/masks/crown.png"), "groomlab/shot_v2/masks/crown.png");
    EXPECT_EQ(read(newBase + "/groomlab/shot_v2/masks/crown.png"), "crown");
    EXPECT_EQ(read(oldBase + "/groomlab/shot/masks/crown.png"), "crown") << "the old scene keeps its own";
    EXPECT_EQ(f.follow("images/tint.png"), "images/tint.png") << "a relative path keeps its place";
    EXPECT_EQ(read(newBase + "/images/tint.png"), "tint");
    EXPECT_EQ(f.follow(root + "/library/shared.png"), root + "/library/shared.png") << "the machine's file stays";
    EXPECT_EQ(f.follow("groomlab/shot/masks/gone.png"), "groomlab/shot/masks/gone.png") << "nothing on disk: as it was";
    EXPECT_EQ(f.follow("$HOME/x.png"), "$HOME/x.png");
    EXPECT_EQ(f.followPrefix("groomlab/shot/cards/hair_", {"Color.tga", "Atlas.json", "Normal.tga"}), "groomlab/shot_v2/cards/hair_");
    EXPECT_EQ(read(newBase + "/groomlab/shot_v2/cards/hair_Color.tga"), "color");
    EXPECT_EQ(read(newBase + "/groomlab/shot_v2/cards/hair_Atlas.json"), "atlas");
    EXPECT_EQ(f.copied().size(), 4u);
    EXPECT_TRUE(f.warnings().empty());
    // Again (a second save there): the same files, nothing copied.
    FileFollower again(oldBase, assetFolder(oldBase, "shot"), newBase, assetFolder(newBase, "shot_v2"));
    EXPECT_EQ(again.follow("groomlab/shot/masks/crown.png"), "groomlab/shot_v2/masks/crown.png");
    EXPECT_TRUE(again.copied().empty());
}

// A file already there with other content is never written over: the scene takes a copy under
// a free name and says so; absolute paths stay absolute (Blender's), an untitled scene's folder
// counts as the old scene's.
TEST(AssetFiles, NeverOverOtherContentAbsoluteAndUntitled) {
    const std::string root = scratch("conflict");
    const std::string oldBase = root + "/a", newBase = root + "/b";
    write(oldBase + "/groomlab/s/masks/m.png", "mine");
    write(newBase + "/groomlab/t/masks/m.png", "theirs");
    write(oldBase + "/groomlab/s/cards/g_Color.tga", "mine");
    write(newBase + "/groomlab/t/cards/g_Color.tga", "theirs");
    write(root + "/temp/groomlab_1_s1maps_0/u.png", "untitled");
    FileFollower f(oldBase, assetFolder(oldBase, "s"), newBase, assetFolder(newBase, "t"),
                   {{root + "/temp/groomlab_1_s1maps_0", assetFolder(newBase, "t", "masks")}});
    EXPECT_EQ(f.follow(oldBase + "/groomlab/s/masks/m.png"), newBase + "/groomlab/t/masks/m_2.png");
    EXPECT_EQ(read(newBase + "/groomlab/t/masks/m.png"), "theirs");
    EXPECT_EQ(read(newBase + "/groomlab/t/masks/m_2.png"), "mine");
    EXPECT_EQ(f.followPrefix("groomlab/s/cards/g_", {"Color.tga"}), "groomlab/t/cards/g_2_");
    EXPECT_EQ(read(newBase + "/groomlab/t/cards/g_2_Color.tga"), "mine");
    EXPECT_EQ(f.follow(root + "/temp/groomlab_1_s1maps_0/u.png"), newBase + "/groomlab/t/masks/u.png");
    EXPECT_EQ(read(newBase + "/groomlab/t/masks/u.png"), "untitled");
    EXPECT_EQ(f.warnings().size(), 2u);
}

// Collect Files: a file from outside copied into the folder (a UDIM set's every tile), the same
// file there used as it is, other content there: a free name.
TEST(AssetFiles, CollectFileCopiesInNeverOver) {
    const std::string root = scratch("collect");
    write(root + "/library/skin.png", "skin");
    write(root + "/library/body.1001.png", "a");
    write(root + "/library/body.1002.png", "b");
    write(root + "/scene/groomlab/s/masks/clash.png", "theirs");
    write(root + "/library/clash.png", "mine");
    const std::string masks = root + "/scene/groomlab/s/masks";
    std::vector<std::string> copied;
    EXPECT_FALSE(insideFolder(root + "/library/skin.png", root + "/scene"));
    EXPECT_TRUE(insideFolder(masks + "/clash.png", root + "/scene"));
    EXPECT_EQ(collectFile(root + "/library/skin.png", masks, &copied), masks + "/skin.png");
    EXPECT_EQ(collectFile(root + "/library/skin.png", masks, &copied), masks + "/skin.png") << "the same file: used as it is";
    EXPECT_EQ(collectFile(root + "/library/body.<UDIM>.png", masks, &copied), masks + "/body.<UDIM>.png");
    EXPECT_EQ(read(masks + "/body.1002.png"), "b");
    EXPECT_EQ(collectFile(root + "/library/clash.png", masks, &copied), masks + "/clash_2.png");
    EXPECT_EQ(read(masks + "/clash.png"), "theirs");
    EXPECT_EQ(collectFile(root + "/library/gone.png", masks, &copied), "");
    EXPECT_EQ(copied.size(), 4u);
}

// A groom's own folder inside the old root takes the new name (the app's masks/<name>): an extra
// root is looked at before the old root.
TEST(AssetFiles, AnExtraRootInsideTheOldRootWins) {
    const std::string root = scratch("extra_first");
    const std::string oldBase = root + "/old", newBase = root + "/new";
    write(oldBase + "/groomlab/shot/masks/shot/crown.png", "crown");
    write(oldBase + "/groomlab/shot/caches/sim.abc", "sim");
    FileFollower f(oldBase, assetFolder(oldBase, "shot"), newBase, assetFolder(newBase, "shot_v2"),
                   {{assetFolder(oldBase, "shot", "masks") + "/shot", assetFolder(newBase, "shot_v2", "masks") + "/shot_v2"}});
    EXPECT_EQ(f.follow("groomlab/shot/masks/shot/crown.png"), "groomlab/shot_v2/masks/shot_v2/crown.png");
    EXPECT_EQ(read(newBase + "/groomlab/shot_v2/masks/shot_v2/crown.png"), "crown");
    EXPECT_EQ(f.follow("groomlab/shot/caches/sim.abc"), "groomlab/shot_v2/caches/sim.abc") << "the rest as the old root";
}

// A texture map's file moved or relinked: the graph's file params naming it (as `resolve` reads
// them; compounds too) name the new one; others stay.
TEST(AssetFiles, FileReadersFollowAMovedMap) {
    const std::string graph = R"({"nodes": [
        {"name": "scatter", "type": "Scatter", "params": {"density": {"texture": "masks/hair/dense.png", "component": "r", "min": 0, "max": 60}}},
        {"name": "width", "type": "Width", "params": {"width": {"texture": "masks/hair/other.png"}}},
        {"name": "c", "type": "Compound", "params": {"definition": {"graph": {"nodes": [
            {"name": "inner", "type": "Clump", "params": {"strength": {"texture": "MASKS/hair/dense.png"}}}]}}}}]})";
    auto resolve = [](const std::string& stored) { return "C:/shot/" + stored; };
    bool changed = false;
    const nlohmann::json out = nlohmann::json::parse(
        retargetFileReaders(graph, "C:/shot/masks/hair/dense.png", "masks/hair/thick.png", resolve, &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(out["nodes"][0]["params"]["density"]["texture"], "masks/hair/thick.png");
    EXPECT_EQ(out["nodes"][0]["params"]["density"]["max"], 60);
    EXPECT_EQ(out["nodes"][1]["params"]["width"]["texture"], "masks/hair/other.png");
    const auto inner = out["nodes"][2]["params"]["definition"]["graph"]["nodes"][0]["params"]["strength"]["texture"];
#ifdef _WIN32
    EXPECT_EQ(inner, "masks/hair/thick.png") << "case-blind on Windows";
#else
    EXPECT_EQ(inner, "MASKS/hair/dense.png");
#endif
    EXPECT_EQ(retargetFileReaders(graph, "C:/shot/masks/hair/none.png", "x.png", resolve, &changed), graph);
    EXPECT_FALSE(changed);
    EXPECT_EQ(retargetFileReaders(graph, "", "x.png", resolve, &changed), graph);
}

// A groom file as every host writes it (groom_package.h): each groom's own files in
// groomlab/<file>/<kind>/<groom>, the params reading a map's image pointed at the copy, paths
// relative to the file; merged by name, the other grooms kept.
TEST(GroomPackage, WritesGroomsWithTheirFilesBesideTheFile) {
    const std::string root = scratch("package");
    // A groom's files somewhere else (a scene's folder), a bake and a Simulate bake.
    write(root + "/scene/groomlab/scene/masks/hair/dense.png", "dense");
    write(root + "/scene/groomlab/scene/cards/hair/hair_Atlas.json", "{}");
    write(root + "/scene/groomlab/scene/cards/hair/hair_Color.tga", "color");
    write(root + "/scene/groomlab/scene/cards/hair/hair_Preview.png", "preview");
    write(root + "/scene/groomlab/scene/caches/hair/hair_sim_sim.abc", "sim");
    GroomFile hair;
    hair.name = "hair";
    hair.data.maps["dense"] = {"texture", root + "/scene/groomlab/scene/masks/hair/dense.png", 1};
    hair.data.graphJson = R"({"nodes": [{"name": "scatter", "type": "Scatter", "params": {"density": {"texture": ")" + root +
                          R"(/scene/groomlab/scene/masks/hair/dense.png", "min": 0, "max": 50}}}]})";
    hair.cardBake = {root + "/scene/groomlab/scene/cards/hair/hair_", "s1"};
    hair.simCaches["sim"] = {root + "/scene/groomlab/scene/caches/hair/hair_sim_sim.abc", "", 7};
    GroomFile beard;
    beard.name = "beard";
    const std::string path = root + "/out/look.groom";
    const GroomWriteReport report = writeGroomsToFile(path, {hair, beard});
    EXPECT_TRUE(report.warnings.empty()) << report.warnings.front();
    EXPECT_EQ(read(root + "/out/groomlab/look/masks/hair/dense.png"), "dense");
    EXPECT_EQ(read(root + "/out/groomlab/look/cards/hair/hair_Color.tga"), "color");
    EXPECT_EQ(read(root + "/out/groomlab/look/cards/hair/hair_Preview.png"), "preview");
    EXPECT_EQ(read(root + "/out/groomlab/look/caches/hair/hair_sim_sim.abc"), "sim");
    const nlohmann::json written = nlohmann::json::parse(read(path));
    EXPECT_EQ(written.at("name"), "hair");
    EXPECT_EQ(written.at("maps").at("dense").at("texture"), "groomlab/look/masks/hair/dense.png");
    EXPECT_EQ(written.at("graph").at("nodes")[0].at("params").at("density").at("texture"), "groomlab/look/masks/hair/dense.png")
        << "the param reading the map's image reads the copy";
    EXPECT_EQ(written.at("cardBake").at("prefix"), "groomlab/look/cards/hair/hair_");
    EXPECT_EQ(written.at("simCaches").at("sim").at("file"), "groomlab/look/caches/hair/hair_sim_sim.abc");
    ASSERT_EQ(written.at("grooms").size(), 1u);
    EXPECT_EQ(written.at("grooms")[0].at("name"), "beard");
    EXPECT_EQ(groomFileFolder(path, "beard", "masks"), root + "/out/groomlab/look/masks/beard");
    EXPECT_EQ(groomFileFolder(path, "", "cards"), root + "/out/groomlab/look/cards/look");

    // Merged: the beard written again replaces its own entry; the hair stays as it was.
    beard.source = "again";
    GroomWriteOptions merge;
    merge.merge = true;
    writeGroomsToFile(path, {beard}, merge);
    std::vector<GroomFile> both = readGroomFiles(path);
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0].name, "hair");
    EXPECT_EQ(both[0].data.maps.at("dense").texture, "groomlab/look/masks/hair/dense.png");
    EXPECT_EQ(both[1].source, "again");
    EXPECT_EQ(groomIndex(both, "beard"), 1);
    EXPECT_EQ(groomIndex(both, "nobody"), -1);
    EXPECT_EQ(groomIndex(both, ""), 0);
    // A groom whose files are in the file's folder already: nothing copied. Without merge: alone.
    GroomFile again = both[0];
    const GroomWriteReport inPlace = writeGroomsToFile(path, {again});
    EXPECT_TRUE(inPlace.copied.empty());
    EXPECT_EQ(readGroomFiles(path).size(), 1u);
    EXPECT_THROW(writeGroomsToFile(path, {beard, beard}), GroomFileError) << "two of one name";
    // A file of one unnamed groom (hosts' files before): the groom merged takes its place.
    GroomFile legacy;
    writeGroomFile(root + "/legacy.groom", legacy);
    writeGroomsToFile(root + "/legacy.groom", {beard}, merge);
    const std::vector<GroomFile> replaced = readGroomFiles(root + "/legacy.groom");
    ASSERT_EQ(replaced.size(), 1u);
    EXPECT_EQ(replaced[0].name, "beard");
}
