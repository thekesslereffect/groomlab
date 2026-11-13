// SPDX-License-Identifier: Apache-2.0
// The Card Atlas (card_blocks.h) for a host's tools: the atlas as JSON text in and out, a block's
// root band, guides and graph, its layout and its cards' shapes.

#include "bind_common.h"

#include "groomlab/card_blocks.h"

namespace groomlab::py_bind {

void bindCardBlocks(py::module_& m) {
    m.def("card_block_graph_json", &cardBlockGraphJson, "A new Card Atlas block's graph (JSON).");
    m.def(
        "card_block_guides_json",
        [](const std::string& blockJson, int count) {
            CardBlock block;
            if (!cardBlockFromJson(blockJson, &block, nullptr)) throw py::value_error("not a card block");
            return cardBlockGuidesJson(block, count);
        },
        py::arg("block_json"), py::arg("count") = 0,
        "A new block's guides (GuideHierarchy JSON): straight guides from its root band down the card.");
    m.def(
        "move_card_block_guides",
        [](const std::string& hierarchyJson, const std::string& fromJson, const std::string& toJson) {
            CardBlock from, to;
            if (!cardBlockFromJson(fromJson, &from, nullptr) || !cardBlockFromJson(toJson, &to, nullptr)) {
                throw py::value_error("not a card block");
            }
            return moveCardBlockGuides(hierarchyJson, from, to);
        },
        py::arg("hierarchy_json"), py::arg("from_block_json"), py::arg("to_block_json"),
        "A block's guides after its root band changed: moved, not turned (each root keeps its place on the band).");
    m.def(
        "card_block_mesh",
        [](const std::string& blockJson) {
            CardBlock block;
            if (!cardBlockFromJson(blockJson, &block, nullptr)) throw py::value_error("not a card block");
            const CardBlockMesh mesh = cardBlockMesh(block);
            py::list points;
            for (const Vec3f& p : mesh.points) points.append(py::make_tuple(p.x, p.y, p.z));
            py::dict out;
            out["points"] = points;
            out["face_counts"] = mesh.faceCounts;
            out["face_indices"] = mesh.faceIndices;
            out["uvs"] = mesh.uvs;
            return out;
        },
        py::arg("block_json"),
        "The block's root band: {points, face_counts, face_indices, uvs (2 per point)}.");
    m.def("card_block_json", [](const std::string& atlasJson, const std::string& name) {
            const CardBlockAtlas atlas = cardBlockAtlasFromJson(atlasJson);
            for (const CardBlock& b : atlas.blocks) {
                if (b.name == name) return cardBlockToJson(b, atlas.settings);
            }
            throw py::key_error("no block named " + name);
        },
        py::arg("atlas_json"), py::arg("name"), "The block `name` of an atlas as its groom stores it (cardBlock).");
    m.def(
        "card_block_atlas",
        [](const std::vector<std::pair<std::string, std::string>>& namedBlocks) { return cardBlockAtlasToJson(cardBlockAtlasOf(namedBlocks)); },
        py::arg("named_blocks"), "The atlas (JSON) that [(groom name, its cardBlock text)] make (blank: not a block).");
    m.def(
        "card_block_layout",
        [](const std::string& atlasJson, bool pack) {
            CardBlockAtlas atlas = cardBlockAtlasFromJson(atlasJson);
            if (pack) {
                packCardBlocks(atlas);
            } else {
                updateCardBlockLayout(atlas);
            }
            return cardBlockAtlasToJson(atlas);
        },
        py::arg("atlas_json"), py::arg("pack") = false,
        "The atlas laid out: `pack` every block afresh, else after a change (blocks placed by hand kept).");
    m.def(
        "card_block_next_origin",
        [](const std::string& atlasJson, float width) {
            const Vec3f o = nextCardBlockOrigin(cardBlockAtlasFromJson(atlasJson), width);
            return py::make_tuple(o.x, o.y, o.z);
        },
        py::arg("atlas_json"), py::arg("width"), "Where a new block of `width` goes in the scene.");
    m.def(
        "card_block_shapes",
        [](const std::string& atlasJson, int segments, int across, float bend, float twist, bool spread) {
            CardShapeOptions options;
            options.segments = segments;
            options.across = across;
            options.bend = bend;
            options.twist = twist;
            options.spread = spread;
            py::list out;
            for (const FbxMesh& mesh : cardBlockShapes(cardBlockAtlasFromJson(atlasJson), options)) {
                py::list points;
                for (const Vec3f& p : mesh.points) points.append(py::make_tuple(p.x, p.y, p.z));
                py::dict d;
                d["name"] = mesh.name;
                d["material"] = mesh.material;
                d["points"] = points;
                d["face_counts"] = mesh.faceCounts;
                d["face_indices"] = mesh.faceIndices;
                d["uvs"] = mesh.uvs;
                out.append(d);
            }
            return out;
        },
        py::arg("atlas_json"), py::arg("segments") = 8, py::arg("across") = 1, py::arg("bend") = 0.0f, py::arg("twist") = 0.0f,
        py::arg("spread") = true,
        "The blocks' cards as UV-ready meshes, one per block: [{name, material, points, face_counts, face_indices, "
        "uvs (2 per face corner)}] (write_card_block_shapes writes the same as FBX).");
    m.def(
        "write_card_block_shapes",
        [](const std::string& path, const std::string& atlasJson, int segments, int across, float bend, float twist, bool spread,
           bool meters) {
            CardShapeOptions options;
            options.segments = segments;
            options.across = across;
            options.bend = bend;
            options.twist = twist;
            options.spread = spread;
            writeCardBlockShapes(path, cardBlockAtlasFromJson(atlasJson), options, meters);
        },
        py::arg("path"), py::arg("atlas_json"), py::arg("segments") = 8, py::arg("across") = 1, py::arg("bend") = 0.0f,
        py::arg("twist") = 0.0f, py::arg("spread") = true, py::arg("meters") = false,
        "Writes the blocks' cards as UV-ready meshes (FBX), one per block.");
}

}  // namespace groomlab::py_bind
