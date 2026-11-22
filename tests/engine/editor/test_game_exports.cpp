// SPDX-License-Identifier: Apache-2.0
// The groom editor's game exports (engine/editor game_exports.h), every host's options: the base
// name with its LOD, the maps' size kept in range, nothing written without cards; glTF cards with
// a mesh per level of detail and the hair cap (a warning without one).

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/editor/game_exports.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "../core/test_helpers.h"

using namespace groomlab;

TEST(EditorGameExports, OneSetOfOptions) {
    GameExportOptions o;
    EXPECT_EQ(gameExportName(o), "groom");
    o.name = "hair";
    o.lod = "low";
    EXPECT_EQ(gameExportName(o), "hair_low") << "a LOD keeps apart in one folder";
    o.nameGiven = true;
    EXPECT_EQ(gameExportName(o), "hair") << "a name given is the name";
    EXPECT_EQ(gameExportResolution(0), 0) << "0: the Cards nodes' own";
    EXPECT_EQ(gameExportResolution(1), kMinBakeResolution);
    EXPECT_EQ(gameExportResolution(1 << 20), kMaxCardBakeResolution);
    EXPECT_THROW(writeEngineCards(CardBake{}, GroomStats{}, HairLook{}, nullptr, nullptr, "", false, o), std::runtime_error);
}

namespace {

StrandsPtr fur(const MeshPtr& scalp) {
    GraphDocument doc = loadGraphJson(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 6, "strength": 0.8}},
        {"name": "cards", "type": "Cards", "params": {"segments": 4}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "cards", "input": "strands"}, {"from": "cards", "to": "out", "input": "strands"}]})j");
    ExternalInputs externals;
    setGroomExternals(externals, scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    ctx.buildCards = true;
    return doc.graph.evaluateStrands(doc.output, ctx);
}

MeshPtr planeWithUVs() {
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(8));
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& t : plane->triangles()) {
        for (const std::int32_t i : t) uvs.push_back({plane->points()[static_cast<std::size_t>(i)].x, plane->points()[static_cast<std::size_t>(i)].z});
    }
    plane->setCornerUVs(uvs);
    return plane;
}

}  // namespace

TEST(EditorGameExports, GltfCardsAMeshPerLevelAndTheCap) {
    const MeshPtr plane = planeWithUVs();
    const StrandsPtr strands = fur(plane);
    std::vector<std::string> asked;
    GltfCardsSource source;
    source.bake = [&](const std::string& lod, int resolution, float occlusion) {
        asked.push_back(lod);
        EXPECT_EQ(resolution, kMinBakeResolution) << "kept in range";
        EXPECT_FLOAT_EQ(occlusion, 0.5f);
        return bakeCardsInMemory(strands, plane, resolution, occlusion);
    };
    MeshPtr capScalp = plane;
    source.capGroom = [&](MeshPtr* scalp, std::string*) {
        *scalp = capScalp;
        return strands;
    };
    GltfCardsOptions o;
    o.path = (std::filesystem::temp_directory_path() / "groomlab_editor_cards.glb").u8string();
    o.name = "hair";
    o.lods = {"hero", "low"};
    o.resolution = 1;
    o.capResolution = 16;
    o.occlusion = 0.5f;
    const GltfCardsExport done = writeGltfCards(o, HairLook{}, source);
    EXPECT_EQ(asked, (std::vector<std::string>{"hero", "low"}));
    EXPECT_EQ(done.meshes, (std::vector<std::string>{"hair_hero", "hair_low", "hair_scalpCap"}));
    ASSERT_EQ(done.files.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(done.files[0])));
    EXPECT_GT(done.cards, 0u);
    EXPECT_TRUE(done.warnings.empty());

    // No LODs: the cards as they are; a scalp without UVs: the cards still go, a warning says why.
    o.lods.clear();
    capScalp = test::makePlane(8);
    const GltfCardsExport plain = writeGltfCards(o, HairLook{}, source);
    EXPECT_EQ(plain.meshes, (std::vector<std::string>{"hair_cards"}));
    ASSERT_EQ(plain.warnings.size(), 1u);
    EXPECT_EQ(plain.warnings[0].rfind("no hair cap (", 0), 0u);
    source.capGroom = [](MeshPtr*, std::string* whyNot) {
        *whyNot = "the scalp is gone";
        return StrandsPtr();
    };
    EXPECT_EQ(writeGltfCards(o, HairLook{}, source).warnings, (std::vector<std::string>{"no hair cap (the scalp is gone)"}));

    o.alphaMode = "glass";
    EXPECT_THROW(writeGltfCards(o, HairLook{}, source), std::invalid_argument);
    o.alphaMode = "mask";
    o.scale = 0.0f;
    EXPECT_THROW(writeGltfCards(o, HairLook{}, source), std::invalid_argument);
    o.scale = 0.01f;
    source.bake = [](const std::string&, int, float) { return CardBake{}; };
    EXPECT_THROW(writeGltfCards(o, HairLook{}, source), std::runtime_error) << "nothing written without cards";
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(o.path), ec);
}
