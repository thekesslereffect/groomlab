// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's map edits (engine/editor), every host's: what an edit of the groom's painted
// maps changes (the maps' text, the graph reading them, the files their images are), worked out
// once; the host sets the texts and keeps the files' states for the step's undo.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/asset_files.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"

namespace groomlab {

// Where a groom's map images live and how the host stores their paths.
struct MapFiles {
    std::string mapsFolder;   // the groom's own maps folder: GroomLab's images, renamed with their map
    std::string groomFolder;  // stored paths relative to it (pathFromGroomFolder); "": stored absolute
    // The host's own rule for storing a path instead (Maya's: relative to the scene or project,
    // portableMayaPath); null: groomFolder's.
    std::function<std::string(const std::string& full)> store;
    // A stored path as a file (relative ones the host resolves: the groom's folder, its search paths).
    std::function<std::string(const std::string&)> resolve;
    // Whether something beside this groom reads the image (Maya: another groom of the scene): a
    // renamed map then copies it instead of moving it, the other keeping its own. Null: nothing does.
    std::function<bool(const std::string& full)> shared;
};

// A map renamed.
struct MapRename {
    bool guideLayer = false;       // a guide layer: the host renames its hierarchy's attribute (renameAttribute)
    ScalpMaps maps;                // the scalp maps after (the entry under its new name; unchanged for a guide layer)
    std::string graph;             // the graph after, its readers of the map (and of the image) retargeted; "" unchanged
    std::vector<FileState> files;  // the image's files moved or copied, as found and as left (the step's undo)
    std::string file;              // a texture map's image as stored after ("" for another kind)
};

// Map `from` renamed `to`, every host's Rename: the Map nodes, map params and texture params
// reading it read the new name (retargetMapReaders, retargetFileReaders). A texture map's image
// named after the map in the groom's maps folder (renamedMapImage) follows the name, to a free one
// when taken: moved (a tile set: each tile), or copied when another map of the groom or something
// beside it (`files.shared`) reads it. A move that fails part way is taken back and the image stays
// where it was, its map renamed all the same. `guides`: the groom's hierarchy, its layers maps too
// (may be null).
// The files are moved or copied here; nothing else is set. Throws std::invalid_argument for a name
// that cannot name a map, no map `from`, or a map named `to` already.
MapRename renameMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph,
                    const std::string& from, const std::string& to, const MapFiles& files);

// A full path as the host stores it (files.store, else relative to files.groomFolder, else absolute).
std::string storedMapPath(const std::string& full, const MapFiles& files);
// A new texture map's image: <maps folder>/<name>.png, or a free name beside it when taken.
std::string newMapImageFile(const std::string& name, const MapFiles& files);
// A texture paint written to `file` (folders made; a tile set: each tile): the files' states as
// found and as left. Throws std::runtime_error when it cannot be written.
std::vector<FileState> writeMapImage(const TexturePaint& paint, const std::string& file);

// A map converted into another kind.
struct MapConvert {
    ScalpMaps maps;                // the scalp maps after
    // The guide layer of the map's name: removed from the hierarchy (it was one), set from
    // `layer` (it is one now: each guide's value), or untouched (neither). applyConvertedLayer.
    bool removeLayer = false, setLayer = false;
    std::map<GuideId, float> layer;
    std::vector<FileState> files;  // the image written (to a texture), as found and as left
    std::string file;              // the new texture map's image as stored ("" for another kind)
    bool colorLost = false;        // a color map onto the guides: its luminance (a guide layer is gray)
};

// Map `name` resampled into kind `to` (guides, vertices or texture), its name kept so the graph
// reads it as before, every host's Convert (scalp_paint.h convertMap; a guide layer read at the
// guides' roots on `cage`). To a texture: `resolution` square, at `texturePath` (a PNG; a free name
// when taken) or newMapImageFile. `guides`: the groom's hierarchy as authored (its layers; null:
// none). Throws std::invalid_argument for no such map, an unknown kind, a guide layer with no
// guides or no maps folder for a texture, std::runtime_error for an image that does not read or
// cannot be written.
MapConvert convertMapKind(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh& cage, const std::string& name,
                          const std::string& to, int resolution, const std::string& texturePath, const MapFiles& files);
// The converted layer onto a hierarchy (the host's, its other edits kept as they are).
void applyConvertedLayer(GuideHierarchy& h, const std::string& name, const MapConvert& converted);

// A map removed.
struct MapRemove {
    bool guideLayer = false;       // a guide layer: the host removes its hierarchy's attribute (removeAttribute)
    ScalpMaps maps;                // the scalp maps after (unchanged for a guide layer)
    std::string graph;             // the graph after, its readers let go of the map; "" unchanged
    std::vector<std::pair<std::string, std::string>> unlinked;  // the (node, param) pairs that read it
    std::vector<FileState> files;  // the image's files deleted, as found and as left (the step's undo)
    bool fileDeleted = false;
};

// Map `name` removed, every host's Remove: what reads it lets go of it (unlinkMapReaders: its Map
// nodes go with their links, map params naming it and the map names HairColor and Interpolate read
// back to the node's defaults). With `deleteFile`, a texture map's image goes too when GroomLab made
// it and nothing else reads it (mapImageDeletable: in the maps folder, no other map of the groom
// and nothing beside it, `files.shared`, reading it; a tile set: every tile). Throws
// std::invalid_argument for no map `name`, std::runtime_error for a graph that does not read.
MapRemove removeMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph, const std::string& name,
                    bool deleteFile, const MapFiles& files);

// A fill of a map, as asked.
struct MapFillRequest {
    float value = 0.0f;                // a gray map's value, a color map's r g b without `color` (clamped 0..1)
    std::optional<PaintColor> color;   // a color map's color, in the channels of `mask`
    unsigned mask = kAllChannels;      // a color map's channels filled (a value: their r g b)
    PaintMode mode = PaintMode::Replace;
    bool selectedOnly = false;         // Fill Selected: a guide layer on `selected` only
    std::vector<GuideId> selected;
    std::vector<std::int32_t> faces;   // only those polygons of the cage (a guide layer: the guides rooted on them)
    int resolution = 1024;             // a texture map whose image is not there yet: a blank one this square
};

// A map filled.
struct MapFill {
    std::string kind;                  // the map's kind (guides, vertices or texture)
    std::map<GuideId, float> layer;    // a guide layer: each guide's own value after (applyFilledLayer)
    ScalpMaps maps;                    // the scalp maps after (a vertex map's values; a texture map's own copy)
    bool mapsChanged = false;
    std::vector<FileState> files;      // the image written, as found and as left (the step's undo)
    std::string file;                  // a texture map's own copy as stored, when it got one ("" otherwise)
};

// Map `name` filled, every host's Fill Map (scalp_paint.h fillGuideLayer, fillVertexMap,
// fillTexturePaint): `fill.value` everywhere, on the selected guides (a guide layer), or on the
// cage's polygons `fill.faces`; a color map takes `fill.color` in the channels of `fill.mask`, or
// the value in its r g b (its alpha kept). A texture map's image is rewritten (a PNG; a tile set:
// each tile); one another map of the groom or something beside it (`files.shared`) reads too is
// written as this map's own copy (newMapImageFile) instead, the other keeping its image. `cage`:
// the scalp the maps are painted on (vertex maps per its vertex; may be null for a guide layer
// filled everywhere). Throws std::invalid_argument for no such map or a fill the map does not take,
// std::runtime_error for an image that cannot be painted or written.
MapFill fillMap(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                const MapFillRequest& fill, const MapFiles& files);
// The filled layer onto a hierarchy (the host's, its other edits kept as they are).
void applyFilledLayer(GuideHierarchy& h, const std::string& name, const MapFill& filled);

// A map made gray or color.
struct MapChannels {
    int channels = 0;                  // the map's channels after (a guide layer: gray)
    ScalpMaps maps;                    // the scalp maps after
    bool mapsChanged = false;
    std::vector<FileState> files;      // the image rewritten, as found and as left (the step's undo)
    std::string file;                  // a texture map's own copy as stored, when it got one ("" otherwise)
};

// Map `name` made gray (1: its luminance) or color (4: the value in r g b, alpha 1), every host's
// Channels (scalp_paint.h convertMapChannels, TexturePaint::setChannels): a vertex map's values, a
// texture map's image rewritten (a PNG; its own copy when another map or something beside it reads
// it, as fillMap). A guide layer stays gray. `cage`: the scalp the maps are painted on (needed for
// a texture map; `resolution`: a blank image this square when its file is not there). Throws
// std::invalid_argument for no such map, channels other than 1 or 4, or color on a guide layer,
// std::runtime_error for an image that cannot be painted or written.
MapChannels setMapChannels(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                           int wanted, int resolution, const MapFiles& files);

// A new map, as asked.
struct MapCreateRequest {
    std::string kind = "vertices";     // guides, vertices or texture
    bool color = false;                // a color map (vertices or texture)
    float value = 0.0f;                // its value (clamped 0..1)
    std::optional<PaintColor> fill;    // a color map's color; none: (value, value, value, 1)
    int resolution = 1024;             // a texture map's image, this square
    std::vector<std::int32_t> faces;   // only those polygons of the cage get it, the rest 0 (a color map: black, alpha 1)
    std::string texturePath;           // a texture map's image (a PNG; a free name beside it when taken); "": newMapImageFile
};

// A map made.
struct MapCreate {
    bool guideLayer = false;           // a guide layer: its values in `layer` (applyCreatedLayer)
    std::map<GuideId, float> layer;    // each guide's own value
    ScalpMaps maps;                    // the scalp maps after (with the new entry; unchanged for a guide layer)
    std::vector<FileState> files;      // a texture map's image written, as found and as left (the step's undo)
    std::string file;                  // a texture map's image as stored ("" for another kind)
};

// Map `name` made, every host's New Map (scalp_paint.h addGuideLayer, newVertexMapValues,
// newTextureMapPaint): a guide layer on the primary guides (their children inheriting), a vertex
// map on the cage's vertices, or a texture map whose image is written now (stored as the host
// stores paths). `cage`: the scalp the maps are painted on (may be null for a guide layer made
// everywhere). Throws std::invalid_argument for a name that cannot name a map or is taken, an
// unknown kind, a color guide layer, no scalp, no UVs, no maps folder or a texture that is not a
// PNG, no primary guides to carry a layer; std::runtime_error for an image that cannot be written.
MapCreate createMap(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                    const MapCreateRequest& create, const MapFiles& files);
// The made layer onto a hierarchy (the host's, its other edits kept as they are).
void applyCreatedLayer(GuideHierarchy& h, const std::string& name, const MapCreate& created);

// An image for a texture map to read (Import, Relink Image).
struct MapImageRequest {
    std::string path;                  // the image, a full path (a tile set: its pattern or a tile)
    std::optional<bool> copy;          // copied into the maps folder as a PNG; none: only another format (painting writes PNG)
    std::string copyTo;                // the copy's file (a PNG; a free name beside it when taken); "": newMapImageFile
};

// A map linked to an image.
struct MapLink {
    std::string name;                  // the map (an import's: as given, else after the file)
    ScalpMaps maps;                    // the scalp maps after
    bool removeLayer = false;          // a guide layer reads the image now: the host removes its hierarchy's attribute
    std::string graph;                 // the graph after, texture params reading the old image reading the new; "" unchanged
    int channels = 1;                  // the map's channels
    std::vector<FileState> files;      // the copy's files, as found and as left (the step's undo)
    std::string file;                  // the image as stored
};

// A new texture map reading the image, every host's Import: named `name`, else after the file
// (mapNameFromFile: a free name), color when the image has color unless `color` says; the image
// in place (a PNG) or copied as a PNG (`image.copy`; a free name). Throws std::invalid_argument for
// no image or one that does not read, a name that cannot name a map or is taken, another format not
// to be copied, no maps folder for a copy; std::runtime_error for a copy that cannot be written.
MapLink importMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name, const MapImageRequest& image,
                  std::optional<bool> color, const MapFiles& files);
// Map `name` (any kind) reading the image as a texture map, its channels kept (a guide layer: gray,
// its layer removed), every host's Relink Image: the image's rules as importMap's; texture params
// reading the map's old image read the new one (retargetFileReaders). Throws as importMap, and for
// no map `name`.
MapLink relinkMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph, const std::string& name,
                  const MapImageRequest& image, const MapFiles& files);
// The images Reload reads again: texture map `name`'s (every texture map's for ""), as full paths.
// Throws std::invalid_argument for no map `name` or one that is not a texture map.
std::vector<std::string> reloadMapFiles(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name,
                                        const MapFiles& files);

// Paint on a setting, as asked.
struct ParamPaintRequest {
    std::string kind = "vertices";     // a map made here: guides, vertices or texture
    std::string name;                  // the map; "": the one the param reads, else its own (paramMapName)
    std::string channel;               // "r", "g", "b" or "a": that channel of a color map, read through its Map node; "": its value
    std::vector<std::int32_t> faces;   // a map made here: white on those polygons of the cage only, black elsewhere
    int resolution = 1024;             // a texture map made here, this square
};

// What Paint on a setting does.
struct ParamPaint {
    std::string map, kind;             // the map the brush paints, and where it lives
    std::string channel;               // the channel the brush paints ("": the map's value)
    bool made = false;                 // the map made here (`created`)
    MapCreate created;                 // the map made: its scalp maps after, a guide layer's values, its files
    std::string graph;                 // the graph after, the param reading the map; "" when it read it already
};

// Paint on a setting, every host's (+ New Map > for a setting, a param's Paint): the map node.param
// reads already (paramMapOf; an expression, a channel or an image no map holds is refused), else the
// map `name` (default its own, paramMapName: one there already, its link since gone, is read again,
// its painting back), made white when missing (createMap: the look stays as it is) and read by the
// param as every host links it (wireParamMap: the map's Map node wired into the param, its 0-1
// multiplying the param's own value, so white is the param as it was; a param naming a map takes
// the name), with a channel through that output of the Map node (masks packed in one color map,
// made color).
// Throws std::invalid_argument for no such node, a kind or channel it cannot be, no guides for a
// guide layer, a gray guide layer for a channel, and what createMap refuses.
ParamPaint paintParam(const std::string& graph, const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage,
                      const std::string& node, const std::string& param, const ParamPaintRequest& paint, const MapFiles& files);

}  // namespace groomlab
