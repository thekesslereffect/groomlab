// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's map edits (engine/editor), every host's: what an edit of the groom's painted
// maps changes (the maps' text, the graph reading them, the files their images are), worked out
// once; the host sets the texts and keeps the files' states for the step's undo.

#include <functional>
#include <string>
#include <vector>

#include "groomlab/asset_files.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_maps.h"

namespace groomlab {

// Where a groom's map images live and how the host stores their paths.
struct MapFiles {
    std::string mapsFolder;   // the groom's own maps folder: GroomLab's images, renamed with their map
    std::string groomFolder;  // stored paths relative to it (pathFromGroomFolder); "": stored absolute
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

}  // namespace groomlab
