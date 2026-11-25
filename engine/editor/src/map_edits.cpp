// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/map_edits.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/paths.h"
#include "groomlab/stroke.h"
#include "groomlab/texture.h"
#include "groomlab/udim.h"

namespace groomlab {

namespace fs = std::filesystem;

namespace {

std::string sameKey(const std::string& path) { return fs::u8path(path).lexically_normal().generic_u8string(); }

// The image's files moved from -> to (a tile set: each tile), as found and as left; all or nothing.
bool moveImage(const std::vector<std::pair<std::string, std::string>>& moves, std::vector<FileState>* states) {
    std::vector<FileState> found;
    for (const auto& [a, b] : moves) {
        found.push_back({a, readFileState(a), std::nullopt});
        found.push_back({b, readFileState(b), std::nullopt});
    }
    std::vector<std::pair<std::string, std::string>> done;
    std::error_code ec;
    for (const auto& [a, b] : moves) {
        fs::rename(fs::u8path(a), fs::u8path(b), ec);
        if (ec) break;
        done.push_back({a, b});
    }
    if (ec) {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code back;
            fs::rename(fs::u8path(it->second), fs::u8path(it->first), back);
        }
        return false;
    }
    for (FileState& f : found) f.after = readFileState(f.path);
    for (FileState& f : found) states->push_back(std::move(f));
    return true;
}

// A texture map's paint written back into its image, or as the map's own copy (newMapImageFile)
// when another map of the groom or something beside it (files.shared) reads the image: `entry`
// then reads the copy, `*file` its stored path, and true is returned.
bool writeOwnImage(const TexturePaint& paint, const ScalpMaps& maps, const std::string& name, ScalpMapEntry& entry,
                   const MapFiles& files, std::vector<FileState>* states, std::string* file) {
    const auto resolve = [&](const std::string& stored) { return files.resolve ? files.resolve(stored) : stored; };
    const std::string full = resolve(entry.texture);
    bool shared = files.shared && files.shared(full);
    for (const auto& [other, e] : maps.maps) {
        if (other == name || e.kind != "texture" || e.texture.empty()) continue;
        shared = shared || sameFile(resolve(e.texture), full);
    }
    if (!shared || files.mapsFolder.empty()) {
        *states = writeMapImage(paint, full);
        return false;
    }
    const std::string own = newMapImageFile(name, files);
    *states = writeMapImage(paint, own);
    entry.texture = storedMapPath(own, files);
    *file = entry.texture;
    return true;
}

// The image a texture map is to read (importMap, relinkMap): `request.path` in place (a PNG), or
// copied as a PNG into a free file (its files' states in `*states`). The image read in `*image`.
std::string linkedImage(const std::string& name, const MapImageRequest& request, const MapFiles& files, TexturePtr* image,
                        std::vector<FileState>* states) {
    const std::string& path = request.path;
    if (path.empty()) throw std::invalid_argument("give the image's path");
    if (fileStamp(path) == 0) throw std::invalid_argument("no image at " + path);  // (a tile set: any tile)
    try {
        *image = loadTexture(path);
    } catch (const std::exception& e) {
        throw std::invalid_argument(path + " does not read as an image: " + e.what());
    }
    if (!*image || (!(*image)->isUdim() && !(*image)->image())) throw std::invalid_argument(path + " does not read as an image");
    const bool png = isPngPath(path);
    const bool copy = request.copy.value_or(!png);
    if (!copy && !png) throw std::invalid_argument(path + " is not a PNG: painting writes PNG, so another format is copied as one (copy true)");
    if (!copy) return fs::u8path(path).generic_u8string();  // in place: painting edits it
    if (request.copyTo.empty() && files.mapsFolder.empty()) {
        throw std::invalid_argument("save the groom first: a JPEG, TGA, PSD... is copied as a PNG into the groomlab folder beside it");
    }
    if (!request.copyTo.empty() && !isPngPath(request.copyTo)) {
        throw std::invalid_argument("a texture map's file is a PNG (painting writes PNG): copy the image to a .png file");
    }
    const std::string target = request.copyTo.empty() ? newMapImageFile(name, files) : freeFilePath(request.copyTo);
    *states = mapImageStates(target, (*image)->isUdim() ? (*image)->udimTiles() : std::vector<int>{});
    std::error_code ec;
    fs::create_directories(fs::u8path(target).parent_path(), ec);
    if (png) {
        if (copyMapImage(path, target).empty()) throw std::runtime_error("could not copy " + path + " to " + target);
    } else {
        try {
            writeImagePng(**image, target);  // (a tile set: each tile, udimPathFor(target))
        } catch (const std::exception& e) {
            throw std::runtime_error("could not write " + target + ": " + e.what());
        }
    }
    finishFileStates(*states);
    states->erase(std::remove_if(states->begin(), states->end(), [](const FileState& f) { return f.before == f.after; }), states->end());
    return target;
}

// The node of `graph` named `name` (null: none).
const nlohmann::json* graphNode(const nlohmann::json& graph, const std::string& name) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return nullptr;
    for (const nlohmann::json& n : graph["nodes"]) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

bool rgbaChannel(const std::string& c) { return c == "r" || c == "g" || c == "b" || c == "a"; }

// The channel node.param reads of the map it reads: a Map node's output feeding it, or a color
// texture map's component; "" its value (Maya's param_map).
std::string readChannel(const std::string& graphJson, const ScalpMaps& maps, const std::string& node, const std::string& param,
                        const std::string& map) {
    const nlohmann::json g = nlohmann::json::parse(graphJson, nullptr, false);
    if (g.is_object() && g.contains("connections") && g["connections"].is_array()) {
        for (const nlohmann::json& c : g["connections"]) {
            if (!c.is_object() || c.value("to", std::string()) != node || c.value("input", std::string()) != param) continue;
            const nlohmann::json* from = graphNode(g, c.value("from", std::string()));
            const std::string output = c.value("output", std::string());
            return from && from->value("type", std::string()) == "Map" && rgbaChannel(output) ? output : std::string();
        }
    }
    const nlohmann::json* n = graphNode(g, node);
    if (!n || !n->contains("params") || !(*n)["params"].is_object() || !(*n)["params"].contains(param)) return {};
    const nlohmann::json& value = (*n)["params"][param];
    const auto entry = maps.maps.find(map);
    if (!value.is_object() || !value.contains("texture") || entry == maps.maps.end() || entry->second.channels != kColorMap) return {};
    const std::string component = value.value("component", std::string());
    return rgbaChannel(component) ? component : std::string();
}

// node.param fed by channel `channel` of map `map`'s Map node (one per map, made left of the node
// when there is none), the param exposed as an input (graph_controls.h exposeParam).
std::string wireMapChannel(const std::string& graphJson, const std::string& node, const std::string& param, const std::string& map,
                           const std::string& channel) {
    GraphEdit edit(graphJson);
    const nlohmann::json g = nlohmann::json::parse(graphJson, nullptr, false);
    std::string mapNode;
    if (g.is_object() && g.contains("nodes") && g["nodes"].is_array()) {
        for (const nlohmann::json& n : g["nodes"]) {
            if (!n.is_object() || n.value("type", std::string()) != "Map") continue;
            const nlohmann::json params = n.value("params", nlohmann::json::object());
            if (params.is_object() && params.value("name", std::string("mask")) == map) {
                mapNode = n.value("name", std::string());
                break;
            }
        }
    }
    if (mapNode.empty()) {
        std::optional<double> x, y;
        const nlohmann::json* at = graphNode(g, node);
        if (at && at->contains("ui") && (*at)["ui"].is_object()) {
            const nlohmann::json& ui = (*at)["ui"];
            if (ui.contains("x") && ui["x"].is_number() && ui.contains("y") && ui["y"].is_number()) {
                x = ui["x"].get<double>() - 2 * 240.0;  // two of the core's columns (graph_edit.cpp) to its left
                y = ui["y"].get<double>();
            }
        }
        mapNode = edit.addNode("Map", edit.uniqueName(map), nlohmann::json{{"name", map}}.dump(), x, y);
    }
    exposeParam(edit, node, param);
    edit.connect(mapNode, node, param, channel);
    return edit.toJson();
}

}  // namespace

MapRename renameMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph,
                    const std::string& from, const std::string& to, const MapFiles& files) {
    const std::vector<std::string> guideLayers = guides ? guides->attributeNames() : std::vector<std::string>{};
    MapRename out;
    out.maps = maps;
    const bool scalpMap = maps.maps.count(from) > 0;
    const bool layer = !scalpMap && std::find(guideLayers.begin(), guideLayers.end(), from) != guideLayers.end();
    if (!scalpMap && !layer) throw std::invalid_argument("no map named '" + from + "'");
    if (from == to) return out;
    if (!validMapName(to)) throw std::invalid_argument(mapNameError(to));
    if (maps.maps.count(to) || std::find(guideLayers.begin(), guideLayers.end(), to) != guideLayers.end()) {
        throw std::invalid_argument("the groom already has a map named '" + to + "'");
    }
    std::string movedFrom;
    if (layer) {
        out.guideLayer = true;
    } else {
        auto node = out.maps.maps.extract(from);
        node.key() = to;
        ScalpMapEntry& entry = node.mapped();
        if (entry.kind == "texture" && !entry.texture.empty()) {
            const std::string full = files.resolve ? files.resolve(entry.texture) : entry.texture;
            const std::string target = renamedMapImage(full, from, to, files.mapsFolder);
            if (!target.empty()) {
                // Shared: another map of this groom stores the same image, or something beside it reads it.
                bool shared = files.shared && files.shared(full);
                for (const auto& [name, other] : maps.maps) {
                    if (name == from || other.kind != "texture" || other.texture.empty()) continue;
                    const std::string otherFull = files.resolve ? files.resolve(other.texture) : other.texture;
                    shared = shared || sameKey(otherFull) == sameKey(full);
                }
                bool done = false;
                if (shared) {
                    std::vector<FileState> written;
                    for (const auto& [a, b] : mapImageMoves(full, target)) written.push_back({b, readFileState(b), std::nullopt});
                    const std::vector<std::string> copied = copyMapImage(full, target);
                    done = !copied.empty();
                    for (FileState& w : written) {
                        w.after = readFileState(w.path);
                        if (w.after != w.before) out.files.push_back(std::move(w));
                    }
                } else {
                    done = moveImage(mapImageMoves(full, target), &out.files);
                }
                if (done) {
                    entry.texture = files.store                 ? files.store(target)
                                    : files.groomFolder.empty() ? fs::u8path(target).generic_u8string()
                                                                : pathFromGroomFolder(target, files.groomFolder);
                    movedFrom = full;
                }
            }
            out.file = entry.texture;
        }
        out.maps.maps.insert(std::move(node));
    }
    bool retargeted = false;
    std::string g = retargetMapReaders(graph, from, to, &retargeted);
    if (!movedFrom.empty()) {
        bool followed = false;  // texture params reading the image read it where it is now
        const std::function<std::string(const std::string&)> resolve =
            files.resolve ? files.resolve : std::function<std::string(const std::string&)>([](const std::string& p) { return p; });
        g = retargetFileReaders(g, movedFrom, out.file, resolve, &followed);
        retargeted = retargeted || followed;
    }
    if (retargeted) out.graph = std::move(g);
    return out;
}

std::string storedMapPath(const std::string& full, const MapFiles& files) {
    if (files.store) return files.store(full);
    if (files.groomFolder.empty()) return fs::u8path(full).generic_u8string();
    return pathFromGroomFolder(full, files.groomFolder);
}

std::string newMapImageFile(const std::string& name, const MapFiles& files) {
    return freeFilePath((fs::u8path(files.mapsFolder) / fs::u8path(name + ".png")).generic_u8string());
}

std::vector<FileState> writeMapImage(const TexturePaint& paint, const std::string& file) {
    std::vector<FileState> states = mapImageStates(file, paint.udim() ? paint.udimLayout()->tiles : std::vector<int>{});
    std::error_code ec;
    fs::create_directories(fs::u8path(file).parent_path(), ec);
    try {
        paint.save(file);  // (a tile set: each tile, udimPathFor(file))
    } catch (const std::exception& e) {
        restoreFileStates(states, false);
        throw std::runtime_error(std::string("cannot write the map's image: ") + e.what());
    }
    finishFileStates(states);
    return states;
}

MapConvert convertMapKind(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh& cage, const std::string& name,
                          const std::string& to, int resolution, const std::string& texturePath, const MapFiles& files) {
    if (to != "guides" && to != "vertices" && to != "texture") throw std::invalid_argument("a map's kind is guides, vertices or texture");
    const std::string from = mapKindOf(maps, guides, name);
    if (from.empty()) throw std::invalid_argument("no map named '" + name + "'");
    MapConvert out;
    out.maps = maps;
    if (from == to) return out;
    if (to == "texture" && texturePath.empty() && files.mapsFolder.empty()) {
        throw std::invalid_argument("save the groom first: a texture map's image goes in the groomlab folder beside it");
    }
    if (!texturePath.empty() && !isPngPath(texturePath)) throw std::invalid_argument("a texture map's file is a PNG (painting writes PNG)");
    // The source as its kind holds it.
    MapSource source;
    source.kind = from;
    source.channels = mapChannelsOf(maps, guides, name);
    if (source.channels == 0) source.channels = kGrayMap;
    if (from == "guides") {
        if (!guides) throw std::invalid_argument("the groom has no guides");
    } else if (from == "vertices") {
        source.vertices = maps.maps.at(name).values;
    } else {
        const std::string full = files.resolve ? files.resolve(maps.maps.at(name).texture) : maps.maps.at(name).texture;
        try {
            source.image = loadTexture(full);
        } catch (const std::exception& e) {
            throw std::runtime_error("map " + name + "'s image does not read: " + e.what());
        }
        if (!source.image) throw std::runtime_error("map " + name + "'s image does not read");
    }
    GuideHierarchy h = guides ? *guides : GuideHierarchy{};
    MapConverted converted = convertMap(name, source, to, cage, h, resolution);
    out.colorLost = converted.colorLost;
    if (to == "vertices") {
        ScalpMapEntry entry;
        entry.kind = "vertices";
        entry.values = std::move(converted.vertices);
        entry.channels = source.channels;
        out.maps.maps[name] = std::move(entry);
    } else if (to == "texture") {
        const std::string file = texturePath.empty() ? newMapImageFile(name, files) : freeFilePath(texturePath);
        out.files = writeMapImage(*converted.texture, file);
        ScalpMapEntry entry;
        entry.kind = "texture";
        entry.texture = storedMapPath(file, files);
        entry.channels = source.channels;
        out.file = entry.texture;
        out.maps.maps[name] = std::move(entry);
    } else {
        out.maps.maps.erase(name);  // (a guide layer is gray: a color map keeps its luminance)
    }
    out.removeLayer = from == "guides";
    if (to == "guides") {
        out.setLayer = true;
        for (const HierarchyGuide& g : h.guides()) {
            const auto value = g.attributes.find(name);
            if (value != g.attributes.end()) out.layer[g.id] = value->second;
        }
    }
    return out;
}

void applyConvertedLayer(GuideHierarchy& h, const std::string& name, const MapConvert& converted) {
    if (converted.removeLayer) h.removeAttribute(name);
    if (!converted.setLayer) return;
    for (const auto& [id, value] : converted.layer) {
        if (h.guide(id)) h.setAttribute(id, name, value);
    }
}

MapRemove removeMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph, const std::string& name,
                    bool deleteFile, const MapFiles& files) {
    MapRemove out;
    out.maps = maps;
    const bool scalpMap = maps.maps.count(name) > 0;
    const std::vector<std::string> layers = guides ? guides->attributeNames() : std::vector<std::string>{};
    if (!scalpMap && std::find(layers.begin(), layers.end(), name) == layers.end()) {
        throw std::invalid_argument("no map named '" + name + "'");
    }
    if (!graph.empty()) {
        bool changed = false;
        std::string unlinked = unlinkMapReaders(graph, name, &changed, &out.unlinked);
        if (changed) out.graph = std::move(unlinked);
    }
    if (!scalpMap) {
        out.guideLayer = true;
        return out;
    }
    const ScalpMapEntry entry = out.maps.maps.at(name);
    out.maps.maps.erase(name);
    if (!deleteFile || entry.kind != "texture" || entry.texture.empty()) return out;
    const auto resolve = [&](const std::string& stored) { return files.resolve ? files.resolve(stored) : stored; };
    const std::string file = resolve(entry.texture);
    bool shared = files.shared && files.shared(file);
    for (const auto& [other, e] : out.maps.maps) {
        shared = shared || (e.kind == "texture" && !e.texture.empty() && sameFile(resolve(e.texture), file));
    }
    if (!mapImageDeletable(file, files.mapsFolder, shared)) return out;  // every host's rule (asset_files.h)
    for (FileState& state : mapImageStates(file)) {  // (a tile set: every tile's file)
        if (!state.before) continue;
        std::error_code ec;
        fs::remove(fs::u8path(state.path), ec);
        if (ec) continue;
        state.after = std::nullopt;
        out.files.push_back(std::move(state));
        out.fileDeleted = true;
    }
    return out;
}

MapFill fillMap(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                const MapFillRequest& fill, const MapFiles& files) {
    MapFill out;
    out.maps = maps;
    out.kind = mapKindOf(maps, guides, name);
    if (out.kind.empty()) throw std::invalid_argument("no map named '" + name + "'");
    const int channels = mapChannelsOf(maps, guides, name);
    if (fill.color && fill.selectedOnly) {
        throw std::invalid_argument("Fill Selected sets a guide layer, which is gray; a color fills a color map");
    }
    if (fill.color && channels != kColorMap) throw std::invalid_argument("a color fills a color map; " + name + " is gray (a value fills it)");
    if (fill.selectedOnly && out.kind != "guides") {
        throw std::invalid_argument("Fill Selected sets a guide layer on the selected guides; " + name + " lives on the scalp");
    }
    if (fill.selectedOnly && !fill.faces.empty()) throw std::invalid_argument("fill the selected guides or the selected faces, not both");
    if (fill.selectedOnly && fill.selected.empty()) throw std::invalid_argument("no guides selected");
    if (!cage && (out.kind != "guides" || !fill.faces.empty())) throw std::invalid_argument("the groom has no scalp mesh");
    const float value = std::clamp(fill.value, 0.0f, 1.0f);
    const PaintColor color = fill.color ? *fill.color : PaintColor{value, value, value, 1.0f};
    const unsigned mask = fill.color ? fill.mask : (fill.mask & 0x7u);  // a value fills a color map's r g b
    if (out.kind == "guides") {
        GuideHierarchy h = guides ? *guides : GuideHierarchy{};
        fillGuideLayer(h, name, value, fill.mode, fill.selectedOnly ? fill.selected : std::vector<GuideId>{}, cage, fill.faces);
        for (const HierarchyGuide& g : h.guides()) {
            const auto own = g.attributes.find(name);
            if (own != g.attributes.end()) out.layer[g.id] = own->second;
        }
        return out;
    }
    ScalpMapEntry& entry = out.maps.maps.at(name);
    if (out.kind == "vertices") {
        fillVertexMap(entry.values, channels, mask, color, fill.mode, *cage, fill.faces);
        out.mapsChanged = true;
        return out;
    }
    // A texture map: its image rewritten, or written as its own copy when something else reads it.
    const std::string full = files.resolve ? files.resolve(entry.texture) : entry.texture;
    requirePaintablePng(full);
    const std::shared_ptr<TexturePaint> paint = openTexturePaint(*cage, full, channels == kColorMap ? kColorMap : kGrayMap, fill.resolution);
    fillTexturePaint(*paint, mask, color, fill.mode, *cage, fill.faces);
    out.mapsChanged = writeOwnImage(*paint, maps, name, entry, files, &out.files, &out.file);
    return out;
}

void applyFilledLayer(GuideHierarchy& h, const std::string& name, const MapFill& filled) {
    for (const auto& [id, value] : filled.layer) {
        if (h.guide(id)) h.setAttribute(id, name, value);
    }
}

MapChannels setMapChannels(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                           int wanted, int resolution, const MapFiles& files) {
    MapChannels out;
    out.maps = maps;
    const std::string kind = mapKindOf(maps, guides, name);
    if (kind.empty()) throw std::invalid_argument("no map named '" + name + "'");
    if (wanted != kGrayMap && wanted != kColorMap) throw std::invalid_argument("a map is gray (1) or color (4)");
    if (kind == "guides") {
        if (wanted == kColorMap) throw std::invalid_argument("a guide layer is gray: convert " + name + " to vertices or a texture first");
        out.channels = kGrayMap;
        return out;
    }
    const int channels = mapChannelsOf(maps, guides, name) == kColorMap ? kColorMap : kGrayMap;
    out.channels = wanted;
    if (channels == wanted) return out;
    ScalpMapEntry& entry = out.maps.maps.at(name);
    if (kind == "vertices") {
        entry.values = convertMapChannels(entry.values, channels, wanted);
    } else {
        if (!cage) throw std::invalid_argument("the groom has no scalp mesh");
        const std::string full = files.resolve ? files.resolve(entry.texture) : entry.texture;
        requirePaintablePng(full);
        const std::shared_ptr<TexturePaint> paint = openTexturePaint(*cage, full, channels, resolution);
        paint->setChannels(wanted);
        writeOwnImage(*paint, maps, name, entry, files, &out.files, &out.file);
    }
    entry.channels = wanted;
    out.mapsChanged = true;
    return out;
}

MapCreate createMap(const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage, const std::string& name,
                    const MapCreateRequest& create, const MapFiles& files) {
    if (!validMapName(name)) throw std::invalid_argument(mapNameError(name));  // the hosts' one rule (scalp_maps.h)
    if (!mapKindOf(maps, guides, name).empty()) throw std::invalid_argument("the groom already has a map named '" + name + "'");
    const std::string& kind = create.kind;
    if (kind != "guides" && kind != "vertices" && kind != "texture") {
        throw std::invalid_argument("a map's kind is guides, vertices or texture, not '" + kind + "'");
    }
    if (create.color && kind == "guides") {
        throw std::invalid_argument("a guide layer is gray: make a color map on the scalp's vertices or as a texture");
    }
    if (!cage && (kind != "guides" || !create.faces.empty())) throw std::invalid_argument("the groom has no scalp mesh");
    const float value = std::clamp(create.value, 0.0f, 1.0f);
    MapCreate out;
    out.maps = maps;
    if (kind == "guides") {
        GuideHierarchy h = guides ? *guides : GuideHierarchy{};
        if (!addGuideLayer(h, name, value, cage, create.faces)) {
            throw std::invalid_argument("no primary guides to carry the layer: place some first");
        }
        out.guideLayer = true;
        for (const HierarchyGuide& g : h.guides()) {
            const auto own = g.attributes.find(name);
            if (own != g.attributes.end()) out.layer[g.id] = own->second;
        }
        return out;
    }
    ScalpMapEntry entry;
    entry.channels = create.color ? kColorMap : kGrayMap;
    const PaintColor fill = create.color && create.fill ? *create.fill : PaintColor{value, value, value, 1.0f};
    if (kind == "vertices") {
        entry.kind = "vertices";
        entry.values = newVertexMapValues(*cage, entry.channels, fill, create.faces);
    } else {
        if (!cage->hasUVs()) throw std::invalid_argument("the scalp has no UVs to paint a texture map on");
        if (!create.texturePath.empty() && !isPngPath(create.texturePath)) {
            throw std::invalid_argument("a texture map's file is a PNG (painting writes PNG), not " + create.texturePath);
        }
        if (create.texturePath.empty() && files.mapsFolder.empty()) {
            throw std::invalid_argument("save the groom first: a texture map's image goes in the groomlab folder beside it");
        }
        // A new file never replaces one that is there (another groom's, a map removed earlier): a free name.
        const std::string file = create.texturePath.empty() ? newMapImageFile(name, files) : freeFilePath(create.texturePath);
        out.files = writeMapImage(newTextureMapPaint(*cage, entry.channels, fill, create.resolution, create.faces), file);
        entry.kind = "texture";
        entry.texture = storedMapPath(file, files);
        out.file = entry.texture;
    }
    out.maps.maps[name] = std::move(entry);
    return out;
}

void applyCreatedLayer(GuideHierarchy& h, const std::string& name, const MapCreate& created) {
    for (const auto& [id, value] : created.layer) {
        if (h.guide(id)) h.setAttribute(id, name, value);
    }
}

MapLink importMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name, const MapImageRequest& image,
                  std::optional<bool> color, const MapFiles& files) {
    MapLink out;
    out.maps = maps;
    auto taken = [&](const std::string& n) { return !mapKindOf(maps, guides, n).empty(); };
    out.name = name.empty() ? mapNameFromFile(image.path, taken) : name;  // (every host's rule, scalp_maps.h)
    if (!validMapName(out.name)) throw std::invalid_argument(mapNameError(out.name));
    if (taken(out.name)) throw std::invalid_argument("the groom already has a map named '" + out.name + "'");
    TexturePtr read;
    const std::string full = linkedImage(out.name, image, files, &read, &out.files);
    out.channels = color.value_or(!isGrayImage(*read)) ? kColorMap : kGrayMap;
    ScalpMapEntry entry{"texture", {}, storedMapPath(full, files)};
    entry.channels = out.channels;
    out.file = entry.texture;
    out.maps.maps[out.name] = std::move(entry);
    return out;
}

MapLink relinkMap(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& graph, const std::string& name,
                  const MapImageRequest& image, const MapFiles& files) {
    const std::string kind = mapKindOf(maps, guides, name);
    if (kind.empty()) throw std::invalid_argument("no map named '" + name + "'");
    MapLink out;
    out.name = name;
    out.maps = maps;
    out.channels = mapChannelsOf(maps, guides, name) == kColorMap ? kColorMap : kGrayMap;  // (a guide layer's: gray)
    TexturePtr read;
    const std::string full = linkedImage(name, image, files, &read, &out.files);
    ScalpMapEntry entry{"texture", {}, storedMapPath(full, files)};
    entry.channels = out.channels;
    out.file = entry.texture;
    out.removeLayer = kind == "guides";
    if (kind == "texture" && !graph.empty()) {  // texture params reading the map's old image read the new one
        const std::function<std::string(const std::string&)> resolve =
            files.resolve ? files.resolve : std::function<std::string(const std::string&)>([](const std::string& p) { return p; });
        bool followed = false;
        std::string g = retargetFileReaders(graph, resolve(maps.maps.at(name).texture), out.file, resolve, &followed);
        if (followed) out.graph = std::move(g);
    }
    out.maps.maps[name] = std::move(entry);
    return out;
}

std::vector<std::string> reloadMapFiles(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name,
                                        const MapFiles& files) {
    if (!name.empty()) {
        const std::string kind = mapKindOf(maps, guides, name);
        if (kind.empty()) throw std::invalid_argument("no map named '" + name + "'");
        if (kind != "texture") throw std::invalid_argument(name + " is not a texture map");
    }
    std::vector<std::string> out;
    for (const auto& [map, entry] : maps.maps) {
        if (entry.kind != "texture" || entry.texture.empty() || (!name.empty() && map != name)) continue;
        out.push_back(files.resolve ? files.resolve(entry.texture) : entry.texture);
    }
    return out;
}

// Whether node.param is a color a field colors (a ColorLayers layer's color, colorField): painted as
// a color map whose Map node's rgb output feeds it.
static bool colorFieldParam(const std::string& graphJson, const std::string& node, const std::string& param) {
    const nlohmann::json g = nlohmann::json::parse(graphJson, nullptr, false);
    const nlohmann::json* n = graphNode(g, node);
    if (!n) return false;
    const std::string type = n->value("type", std::string());
    const OperatorRegistry& registry = OperatorRegistry::instance();
    if (!registry.contains(type)) return false;
    for (const ParamSpec& s : registry.create(type)->paramSpecs()) {
        if (s.name == param) return s.colorField;
    }
    return false;
}

ParamPaint paintParam(const std::string& graph, const ScalpMaps& maps, const GuideHierarchy* guides, const ScalpMesh* cage,
                      const std::string& node, const std::string& param, const ParamPaintRequest& paint, const MapFiles& files) {
    const std::string full = graphOrDefault(graph);
    if (!GraphEdit(full).hasNode(node)) throw std::invalid_argument("no node '" + node + "'");
    // A color param (a ColorLayers layer's): a color map, its Map node's rgb output feeding it.
    const bool color = colorFieldParam(full, node, param);
    if (color && paint.kind == "guides") {
        throw std::invalid_argument("a color is painted on the scalp's vertices or a texture, not on the guides");
    }
    // (A color takes its map whole: a color map, listed in the param itself.)
    const std::string channel = color || paint.channel == "luminance" ? std::string() : paint.channel;
    if (!color && !channel.empty() && !rgbaChannel(channel)) {
        throw std::invalid_argument("a map's channel is r, g, b or a, not '" + channel + "'");
    }
    ParamMapRef read;
    try {
        read = paramMapOf(full, maps, guides, node, param);
    } catch (const std::exception& e) {
        throw std::invalid_argument(e.what());
    }
    ParamPaint out;
    out.created.maps = maps;
    if (paint.name.empty() && !read.map.empty()) {  // the map it reads already: that one, as it is
        out.map = read.map;
        out.kind = read.kind;
        out.channel = readChannel(full, maps, node, param, read.map);
        return out;
    }
    out.map = paint.name.empty() ? paramMapName(node, param) : paint.name;
    out.kind = mapKindOf(maps, guides, out.map);
    out.channel = color ? std::string() : channel;  // a color map painted whole
    if (out.kind.empty()) {
        if (paint.kind != "vertices" && paint.kind != "texture" && paint.kind != "guides") {
            throw std::invalid_argument("a setting's map is on the guides, the scalp's vertices or a texture, not '" + paint.kind + "'");
        }
        if (paint.kind == "guides" && (!guides || guides->guides().empty())) {
            throw std::invalid_argument("the groom has no guides to paint on: add guides, or paint on the scalp's vertices");
        }
        if (!channel.empty() && paint.kind == "guides") {
            throw std::invalid_argument("a guide layer is gray: paint a channel of a map on the scalp's vertices or a texture");
        }
        MapCreateRequest create;  // white: the param as it was, black 0
        create.kind = paint.kind;
        create.color = color || !channel.empty();
        create.value = 1.0f;
        create.fill = PaintColor{1.0f, 1.0f, 1.0f, 1.0f};
        create.resolution = paint.resolution;
        create.faces = paint.faces;
        out.created = createMap(maps, guides, cage, out.map, create, files);
        out.made = true;
        out.kind = paint.kind;
    }
    try {
        out.graph = channel.empty() ? wireParamMap(full, out.created.maps, node, param, out.map, out.kind, paint.mapNode)
                                    : wireMapChannel(full, node, param, out.map, channel);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& e) {
        throw std::invalid_argument(e.what());
    }
    return out;
}

}  // namespace groomlab
