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

#include <functional>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
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
// A map's name made from an image file's (every host's Import Map): the file name's stem, its
// letters, digits and _ (any other character _), the _ trimmed from both ends ("map" when none is
// left), "map_" before a leading digit, then _2, _3... while `taken` says the name is in use.
std::string mapNameFromFile(const std::string& path, const std::function<bool(const std::string&)>& taken = {});
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
// What reads map `name` in the graph (its JSON), every host's "Read by": the Map nodes of that
// name (a Map node without one reads "mask"), and the (node, param) pairs it feeds: each input a
// Map node of it is linked to, then per node the map params whose guide or colorSet source names
// it and HairColor's map and Interpolate's regionMap naming it. The graph's own nodes: those inside
// compounds are not listed (retargetMapReaders does follow a rename into them).
struct MapReaders {
    std::vector<std::string> nodes;
    std::vector<std::pair<std::string, std::string>> users;
};
MapReaders mapReaders(const std::string& graphJson, const std::string& name);
// The graph with every reader of map `name` (mapReaders) letting go of it, as when the map is
// removed: its Map nodes go with their links (the graph's output cleared when it was one), an
// exposed param they fed loses its port and reads its own value again, and the params naming the
// map go back to their spec's default, inside compounds too (a param naming it there, as a rename
// follows it there; a compound's own Map nodes stay). `unlinked`: the pairs that read it (mapReaders' users);
// `changed`: whether anything read it (the same text when nothing did).
std::string unlinkMapReaders(const std::string& graphJson, const std::string& name, bool* changed = nullptr,
                             std::vector<std::pair<std::string, std::string>>* unlinked = nullptr);
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
