// SPDX-License-Identifier: Apache-2.0
#pragma once

// The maps painted on a groom's scalp (Groom Tools > Paint), as the groom stores them in its
// scalpMaps attribute: a vertex map (values per scalp vertex, kept in the scene with the
// groom) or a texture map (an image at the scalp's UVs, kept as a file). Guide layers, the
// third kind of map, live on the guides (guideData). A map is gray (one value) or color
// ("channels": 4, r g b a per vertex, an RGBA PNG); without "channels" it is gray.
//
//   {"crown": {"kind": "vertices", "values": [0, 0.5, ...]},
//    "tint": {"kind": "vertices", "channels": 4, "values": [r, g, b, a, r, g, b, a, ...]},
//    "detail": {"kind": "texture", "texture": "groomlab/shot/masks/hair/detail.png"},
//    "packed": {"kind": "texture", "channels": 4, "texture": "groomlab/shot/masks/hair/packed.png"}}

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/random.h"

namespace groomlab {

struct GroomData;

// random.h's hashString under the name the hosts' map code uses (runtime cache keys only).
inline std::uint64_t hashText(std::uint64_t h, const std::string& s) { return hashString(h, s); }

struct ScalpMapEntry {
    std::string kind;           // "vertices" or "texture"
    std::vector<float> values;  // vertices: `channels` per scalp vertex (0-1), interleaved
    std::string texture;        // texture: the image file, as stored (relative to the scene, resolved when read)
    int channels = 1;           // 1 gray, 4 color (r g b a)
};

struct ScalpMaps {
    std::map<std::string, ScalpMapEntry> maps;

    // "" and "{}" give no maps; throws std::runtime_error on other bad text.
    static ScalpMaps fromJson(const std::string& text);
    std::string toJson() const;  // values rounded to 1/1000
    std::uint64_t hash() const;
    // The maps by name and kind (with a texture map's file) for the graph's Map nodes.
    MapCatalog catalog() const;
};

// The scalp with every vertex map (and the previewed one, `previewValues` of `previewChannels`
// under `previewName`, replacing the stored one) set as a color set of the map's name, so the
// graph's Map nodes read it (a gray map r = g = b, a color map its r g b a). The same mesh when
// there is nothing to add. A map whose count does not fit the scalp is skipped with a message
// in `errors`.
MeshPtr withVertexMaps(const MeshPtr& scalp, const ScalpMaps& maps, const std::string& previewName,
                       const std::vector<float>* previewValues, std::vector<std::string>* errors,
                       int previewChannels = 1);

// A vertex map's values back from the color set withVertexMaps lays (every corner of a vertex
// the same color): one value per vertex when the set is gray (every corner r = g = b, a = 1),
// else r g b a per vertex. `channels` says which (kGrayMap or kColorMap).
std::vector<float> vertexMapFromColorSet(const ScalpMesh& scalp, const std::vector<ScalpMesh::Color>& corners, int* channels);

// ---- Map names, every host's rule ----------------------------------------------------------------
// A map's name is a color set of the scalp, a guide layer and part of a Maya attribute's name:
// letters, digits and _, not starting with a digit.
bool validMapName(const std::string& name);
// Why `name` cannot name a map ("" when it can).
std::string mapNameError(const std::string& name);
// A valid name made from `name`: every other character _, a leading digit after a _ ("map" for "").
std::string sanitizeMapName(const std::string& name);
// The map a param's own map is named (Paint on a setting, New Map > for a setting): "<node>_<param>",
// sanitized. When the groom has a map of that name already (made for the param before, its link
// since gone), every host links that map again, whatever its kind, its painting back, rather than
// making another.
inline std::string paramMapName(const std::string& node, const std::string& param) {
    return sanitizeMapName(node + "_" + param);
}

// The graph (its JSON, graph_io.h) with every reader of map `from` reading `to`: Map nodes by
// name (a Map node without one reads "mask"), HairColor's map and Interpolate's regionMap by name, map params whose guide or
// colorSet source names it, inside compounds' definitions too. Everything else is kept as written; `changed` says
// whether any reader moved (the same text when none did).
std::string retargetMapReaders(const std::string& graphJson, const std::string& from, const std::string& to,
                               bool* changed = nullptr);
// A texture map's file renamed with its map: the stored path with the map's name in the file's
// stem replaced (<map>.png, a numbered <map>_2.png, an older <groom>_<map>.png), "" when the file
// is not named after the map. Where the new name is taken is the host's to resolve (a free name).
std::string renamedMapTexture(const std::string& stored, const std::string& from, const std::string& to);

// A groom file's maps whose names the rule refuses (written by an older standalone app) renamed
// on read, sanitizeMapName made unique among the groom's maps: in the catalog, the scalp's color
// sets, the hierarchy's guide layers and the graph's readers, so the groom reads the same in
// every host. Returns the renames (old -> new); nothing changes for valid names.
std::map<std::string, std::string> sanitizeMapNames(GroomData& data);

// The scalp maps a groom file holds (groom_file.h: the catalog in GroomData::maps, the vertex
// maps as color sets of the scalp): a vertex map's values from the color set of its name, color
// when the catalog or the values say so; a texture map with its file as stored. A vertex map
// without a color set of every corner is left out; guide layers are the hierarchy's.
ScalpMaps scalpMapsFromGroomData(const GroomData& data);

}  // namespace groomlab
