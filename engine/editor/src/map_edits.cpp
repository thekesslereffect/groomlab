// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/map_edits.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "groomlab/paths.h"
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

}  // namespace groomlab
