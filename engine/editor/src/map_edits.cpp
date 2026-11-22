// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/map_edits.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "groomlab/paths.h"
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
                    entry.texture = files.groomFolder.empty() ? fs::u8path(target).generic_u8string()
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

}  // namespace groomlab
