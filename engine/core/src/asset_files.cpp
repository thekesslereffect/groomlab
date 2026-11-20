// SPDX-License-Identifier: Apache-2.0
#include "groomlab/asset_files.h"
#include "groomlab/scalp_maps.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "groomlab/card_files.h"
#include "groomlab/paths.h"
#include "groomlab/texture.h"
#include "groomlab/udim.h"

namespace groomlab {

namespace fs = std::filesystem;

namespace {

// A path as the file system compares it here: normal, forward slashes, lower case on Windows.
std::string key(const std::string& path) {
    std::string out = fs::u8path(path).lexically_normal().generic_u8string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
#ifdef _WIN32
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
#endif
    return out;
}

// `path` relative to `folder` when it is inside it ("" otherwise), forward slashes.
std::string inside(const std::string& path, const std::string& folder) {
    if (path.empty() || folder.empty()) return {};
    const std::string p = key(path), f = key(folder);
    if (p.size() <= f.size() + 1 || p.compare(0, f.size(), f) != 0 || p[f.size()] != '/') return {};
    // The original spelling of the rest (only the comparison is case-blind).
    const std::string normal = fs::u8path(path).lexically_normal().generic_u8string();
    return normal.substr(normal.size() - (p.size() - f.size() - 1));
}

std::string bytesOf(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}

bool sameContent(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (fs::file_size(fs::u8path(a), ec) != fs::file_size(fs::u8path(b), ec) || ec) return false;
    return bytesOf(a) == bytesOf(b);
}

bool hasVariable(const std::string& s) { return s.find('$') != std::string::npos || s.find('%') != std::string::npos; }

}  // namespace

std::string fileSafeName(const std::string& text) {
    // As Maya's (groomlab::maya::fileSafeName): leading DAG bars dropped, trailing dots too.
    std::string out;
    std::size_t start = 0;
    while (start < text.size() && text[start] == '|') ++start;
    for (std::size_t i = start; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        out += std::isalnum(c) || c == '_' || c == '.' || c == '-' ? static_cast<char>(c) : '_';
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out.empty() ? std::string("groom") : out;
}

std::string assetFolder(const std::string& base, const std::string& name, const std::string& kind) {
    fs::path p = fs::u8path(base) / "groomlab" / fs::u8path(fileSafeName(name));
    if (!kind.empty()) p /= fs::u8path(kind);
    return p.lexically_normal().generic_u8string();
}

FileFollower::FileFollower(std::string oldBase, std::string oldRoot, std::string newBase, std::string newRoot,
                           std::vector<std::pair<std::string, std::string>> extraRoots)
    : oldBase_(std::move(oldBase)), oldRoot_(std::move(oldRoot)), newBase_(std::move(newBase)), newRoot_(std::move(newRoot)),
      extraRoots_(std::move(extraRoots)) {}

std::string FileFollower::destination(const std::string& source, const std::string& storedPath, bool relative) const {
    // The extra roots first: one may lie inside the old root (a groom's own folder that takes the
    // new name).
    for (const auto& [from, to] : extraRoots_) {
        const std::string rest = inside(source, from);
        if (!rest.empty()) return (fs::u8path(to) / fs::u8path(rest)).lexically_normal().generic_u8string();
    }
    const std::string rest = inside(source, oldRoot_);
    if (!rest.empty()) return (fs::u8path(newRoot_) / fs::u8path(rest)).lexically_normal().generic_u8string();
    if (relative) return (fs::u8path(newBase_) / fs::u8path(storedPath)).lexically_normal().generic_u8string();
    return {};
}

std::string FileFollower::stored(const std::string& path, bool relative) const {
    if (!relative) return path;
    const std::string rest = inside(path, newBase_);
    return rest.empty() ? path : rest;
}

std::string FileFollower::follow(const std::string& storedPath) {
    if (storedPath.empty() || hasVariable(storedPath)) return storedPath;
    const bool relative = !isAbsolutePath(storedPath);
    const std::string source =
        (relative ? fs::u8path(oldBase_) / fs::u8path(storedPath) : fs::u8path(storedPath)).lexically_normal().generic_u8string();
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(source), ec)) {
        // A texture map's tile set (udim.h: "density.png" standing for density.1001.png...): its
        // tiles go together, the map's path kept as it was named.
        if (!isUdimPath(source)) return storedPath;
        std::string target = destination(source, storedPath, relative);
        if (target.empty() || key(target) == key(source)) return relative ? stored(source, true) : storedPath;
        if (fileStamp(target) != 0 && !sameMapImage(source, target)) {
            const std::string free = freeFilePath(target);  // (free of tile sets too)
            warnings_.push_back(target + " holds other content: this scene now uses " + free);
            target = free;
        }
        if (fileStamp(target) == 0) {
            const std::vector<std::string> written = copyMapImage(source, target);
            if (written.empty()) return storedPath;
            copied_.insert(copied_.end(), written.begin(), written.end());
        }
        return stored(target, relative);
    }
    std::string target = destination(source, storedPath, relative);
    if (target.empty() || key(target) == key(source)) return relative ? stored(source, true) : storedPath;
    if (fs::exists(fs::u8path(target), ec) && !sameContent(source, target)) {
        const std::string free = freeFilePath(target);
        warnings_.push_back(target + " holds other content: this scene now uses " + free);
        target = free;
    }
    if (!fs::exists(fs::u8path(target), ec)) {
        fs::create_directories(fs::u8path(target).parent_path(), ec);
        fs::copy_file(fs::u8path(source), fs::u8path(target), fs::copy_options::none, ec);
        if (ec) return storedPath;
        copied_.push_back(target);
    }
    return stored(target, relative);
}

std::string FileFollower::followPrefix(const std::string& storedPrefix, const std::vector<std::string>& suffixes) {
    if (storedPrefix.empty() || hasVariable(storedPrefix)) return storedPrefix;
    const bool relative = !isAbsolutePath(storedPrefix);
    const std::string source = relative ? (fs::u8path(oldBase_) / fs::u8path(storedPrefix)).generic_u8string() : storedPrefix;
    std::error_code ec;
    std::vector<std::string> present;
    for (const std::string& s : suffixes) {
        if (fs::is_regular_file(fs::u8path(source + s), ec)) present.push_back(s);
    }
    if (present.empty()) return storedPrefix;
    // Where the prefix goes: as its first file goes.
    const std::string firstTarget = destination(source + present.front(), storedPrefix + present.front(), relative);
    if (firstTarget.empty()) return storedPrefix;
    std::string target = firstTarget.substr(0, firstTarget.size() - present.front().size());
    if (key(target + present.front()) == key(source + present.front())) return storedPrefix;
    auto conflict = [&](const std::string& prefix) {
        for (const std::string& s : present) {
            if (fs::exists(fs::u8path(prefix + s), ec) && !sameContent(source + s, prefix + s)) return true;
        }
        return false;
    };
    if (conflict(target)) {
        std::string stem = target;
        while (!stem.empty() && stem.back() == '_') stem.pop_back();
        std::string free;
        for (int k = 2;; ++k) {
            free = stem + "_" + std::to_string(k) + "_";
            bool taken = false;
            for (const std::string& s : suffixes) taken = taken || fs::exists(fs::u8path(free + s), ec);
            if (!taken) break;
        }
        warnings_.push_back(target + "* holds other content: this scene now uses " + free + "*");
        target = free;
    }
    for (const std::string& s : present) {
        if (fs::exists(fs::u8path(target + s), ec)) continue;
        fs::create_directories(fs::u8path(target + s).parent_path(), ec);
        fs::copy_file(fs::u8path(source + s), fs::u8path(target + s), fs::copy_options::none, ec);
        if (!ec) copied_.push_back(target + s);
        ec.clear();
    }
    return stored(target, relative);
}

namespace {

// Every graph param naming a file, `visit(label, value)` (the label "node.param", inside a compound
// "compound/inner.param"; `where` the compounds' prefix).
void visitFileParams(nlohmann::json& graph, const std::function<void(const std::string&, nlohmann::json&)>& visit,
                     const std::string& where = {}) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (nlohmann::json& node : graph["nodes"]) {
        if (!node.is_object() || !node.contains("params") || !node["params"].is_object()) continue;
        const std::string type = node.value("type", std::string());
        const std::string name = node.value("name", std::string());
        for (auto it = node["params"].begin(); it != node["params"].end(); ++it) {
            const std::string& param = it.key();
            nlohmann::json& value = it.value();
            const bool named = param == "path" || param == "texture" || param == "textureRight";
            const bool image = ((param == "map" && type == "HairColor") || (param == "regionMap" && type == "Interpolate")) &&
                               value.is_string() && isImagePath(value.get<std::string>());
            if (value.is_object() && value.contains("texture") && value["texture"].is_string()) {
                visit(where + name + "." + param, value["texture"]);
            } else if ((named || image) && value.is_string()) {
                visit(where + name + "." + param, value);
            }
        }
        if (type == "Compound" && node["params"].contains("definition") && node["params"]["definition"].is_object()) {
            nlohmann::json& definition = node["params"]["definition"];
            if (definition.contains("graph")) visitFileParams(definition["graph"], visit, where + name + "/");
        }
    }
}

}  // namespace

bool insideFolder(const std::string& path, const std::string& folder) { return !inside(path, folder).empty(); }

bool mapImageDeletable(const std::string& file, const std::string& folder, bool readElsewhere) {
    return !file.empty() && !readElsewhere && insideFolder(file, folder) && fileStamp(file) != 0;
}

FollowedGroomFile followSceneGroomFile(const std::string& groomFile, const std::string& oldScene, const std::string& newScene) {
    FollowedGroomFile out;
    std::error_code ec;
    const fs::path full = fs::u8path(groomFile), old = fs::u8path(oldScene), now = fs::u8path(newScene);
    if (groomFile.empty() || oldScene.empty() || newScene.empty() || !fs::is_regular_file(full, ec)) return out;
    if (!sameFile(full.parent_path().u8string(), old.parent_path().u8string())) return out;
    const std::string stem = full.stem().u8string(), oldStem = old.stem().u8string();
    const bool raw = stem == oldStem;
    if (!raw && stem != fileSafeName(oldStem)) return out;  // a groom file of its own name
    const std::string newStem = raw ? now.stem().u8string() : fileSafeName(now.stem().u8string());
    std::string target = (now.parent_path() / fs::u8path(newStem + full.extension().u8string())).generic_u8string();
    if (sameFile(target, full.generic_u8string())) return out;
    if (fs::exists(fs::u8path(target), ec) && !sameMapImage(full.generic_u8string(), target)) {
        target = freeFilePath(target);
        out.renamed = true;
    }
    if (!fs::exists(fs::u8path(target), ec)) out.copied = !copyMapImage(full.generic_u8string(), target).empty();
    out.path = target;
    return out;
}

std::string renamedMapImage(const std::string& full, const std::string& from, const std::string& to, const std::string& folder) {
    if (full.empty() || fileStamp(full) == 0 || !insideFolder(full, folder)) return {};
    const std::string renamed = renamedMapTexture(full, from, to);
    return renamed.empty() ? std::string() : freeFilePath(renamed);
}

std::string collectFile(const std::string& path, const std::string& folder, std::vector<std::string>* copied) {
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> tiles;  // (source, name in the folder)
    std::string name = fs::u8path(path).filename().u8string();
    if (udimTokenIn(path)) {
        // A UDIM set (any token, udim.h): every tile there, under the set's own name.
        for (const int tile : Texture(udimPatternOf(path)).udimTiles()) {
            const std::string source = udimTilePath(path, tile);
            if (fs::is_regular_file(fs::u8path(source), ec)) tiles.emplace_back(source, fs::u8path(source).filename().u8string());
        }
    } else if (fs::is_regular_file(fs::u8path(path), ec)) {
        tiles.emplace_back(path, name);
    } else if (isUdimPath(path)) {
        // A texture map's plain path standing for a tile set (udim.h): every tile, under one name
        // (a free one, every tile, when any would land on other content).
        fs::create_directories(fs::u8path(folder), ec);
        std::string target = (fs::u8path(folder) / fs::u8path(name)).generic_u8string();
        if (fileStamp(target) != 0 && !sameMapImage(path, target)) target = freeFilePath(target);
        for (const std::string& written : copyMapImage(path, target)) {
            if (copied) copied->push_back(written);
        }
        return target;
    }
    if (tiles.empty()) return {};
    fs::create_directories(fs::u8path(folder), ec);
    // A free name for the whole set when any of its files would land on other content.
    std::string stem = fs::u8path(name).stem().u8string(), ext = fs::u8path(name).extension().u8string();
    std::string suffix;
    for (int k = 2;; ++k) {
        bool clash = false;
        for (const auto& [source, tileName] : tiles) {
            const fs::path t = fs::u8path(folder) / fs::u8path(suffix.empty() ? tileName : fs::u8path(tileName).stem().u8string() + suffix + fs::u8path(tileName).extension().u8string());
            if (fs::exists(t, ec) && !sameContent(source, t.generic_u8string())) clash = true;
        }
        if (!clash) break;
        suffix = "_" + std::to_string(k);
    }
    for (const auto& [source, tileName] : tiles) {
        const fs::path t = fs::u8path(folder) / fs::u8path(suffix.empty() ? tileName : fs::u8path(tileName).stem().u8string() + suffix + fs::u8path(tileName).extension().u8string());
        if (fs::exists(t, ec)) continue;
        fs::copy_file(fs::u8path(source), t, fs::copy_options::none, ec);
        if (!ec && copied) copied->push_back(t.generic_u8string());
        ec.clear();
    }
    const std::string stored = suffix.empty() ? name : stem + suffix + ext;
    return (fs::u8path(folder) / fs::u8path(stored)).generic_u8string();
}

std::string FileCollector::collect(const std::string& what, const std::string& stored, const std::string& full, const std::string& folder) {
    if (stored.empty()) return stored;
    if (insideFolder(full, dir_)) {
        ++inside_;
        return stored;
    }
    const std::string copy = collectFile(full, folder);
    if (copy.empty()) {
        missing_.emplace_back(what, stored);
        return stored;
    }
    copied_.emplace_back(what, copy);
    return copy;
}

std::string FileCollector::collectCardBake(const std::string& prefix, const std::string& folder) {
    if (prefix.empty() || insideFolder(prefix + "x", dir_)) return prefix;
    FileFollower f(dir_, "", dir_, folder, {{fs::u8path(prefix).parent_path().generic_u8string(), folder}});
    std::vector<std::string> suffixes = cardBakeSuffixes();
    suffixes.push_back("Preview.png");
    const std::string now = f.followPrefix(prefix, suffixes);
    if (now == prefix) return prefix;
    const std::string full = isAbsolutePath(now) ? now : (fs::u8path(dir_) / fs::u8path(now)).generic_u8string();
    copied_.emplace_back("card bake", full);
    return full;
}

std::string followGraphFiles(const std::string& graphJson, const std::function<std::string(const std::string&)>& follow, bool* changed) {
    if (changed) *changed = false;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded()) return graphJson;
    bool any = false;
    visitFileParams(graph, [&](const std::string&, nlohmann::json& value) {
        const std::string stored = value.get<std::string>();
        if (stored.empty()) return;
        const std::string followed = follow(stored);
        if (followed != stored) {
            value = followed;
            any = true;
        }
    });
    if (changed) *changed = any;
    return any ? graph.dump() : graphJson;
}

std::vector<std::pair<std::string, std::string>> graphFileParams(const std::string& graphJson) {
    std::vector<std::pair<std::string, std::string>> out;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded()) return out;
    visitFileParams(graph, [&](const std::string& label, nlohmann::json& value) {
        const std::string stored = value.get<std::string>();
        if (stored.find_first_not_of(" \t\r\n") != std::string::npos) out.emplace_back(label, stored);
    });
    return out;
}

std::string withGraphFileParams(const std::string& graphJson, const std::map<std::string, std::string>& paths, bool* changed) {
    if (changed) *changed = false;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded() || paths.empty()) return graphJson;
    bool any = false;
    visitFileParams(graph, [&](const std::string& label, nlohmann::json& value) {
        const auto it = paths.find(label);
        if (it == paths.end() || value.get<std::string>() == it->second) return;
        value = it->second;
        any = true;
    });
    if (changed) *changed = any;
    return any ? graph.dump() : graphJson;
}

bool isNetworkPath(const std::string& path) {
    auto slash = [](char c) { return c == '/' || c == '\\'; };
    return path.size() >= 2 && slash(path[0]) && slash(path[1]);
}

namespace {

// The params visitFileParams marked (their value null, or a texture value's "texture") taken out.
void dropMarkedParams(nlohmann::json& graph) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (nlohmann::json& node : graph["nodes"]) {
        if (!node.is_object() || !node.contains("params") || !node["params"].is_object()) continue;
        nlohmann::json& params = node["params"];
        for (auto it = params.begin(); it != params.end();) {
            const nlohmann::json& v = it.value();
            const bool marked = v.is_null() || (v.is_object() && v.contains("texture") && v["texture"].is_null());
            it = marked ? params.erase(it) : std::next(it);
        }
        if (node.value("type", std::string()) == "Compound" && params.contains("definition") && params["definition"].is_object() &&
            params["definition"].contains("graph")) {
            dropMarkedParams(params["definition"]["graph"]);
        }
    }
}

}  // namespace

std::string withoutNetworkFileParams(const std::string& graphJson, std::map<std::string, std::string>* removed) {
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded()) return graphJson;
    std::map<std::string, std::string> out;
    visitFileParams(graph, [&](const std::string& label, nlohmann::json& value) {
        const std::string stored = value.get<std::string>();
        if (!isNetworkPath(stored)) return;
        out[label] = stored;
        value = nullptr;
    });
    if (removed) *removed = out;
    if (out.empty()) return graphJson;
    dropMarkedParams(graph);
    return graph.dump();
}

std::optional<std::string> readFileState(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(fs::u8path(path), ec)) return std::nullopt;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void putFileState(const std::string& path, const std::optional<std::string>& bytes) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!bytes) {
        fs::remove(p, ec);
        return;
    }
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
}

std::vector<FileState> mapImageStates(const std::string& path, const std::vector<int>& tiles) {
    std::vector<FileState> out;
    for (const std::string& file : mapImageFiles(path, tiles)) out.push_back(FileState{file, readFileState(file), std::nullopt});
    return out;
}

void finishFileStates(std::vector<FileState>& states) {
    for (FileState& state : states) state.after = readFileState(state.path);
}

void restoreFileStates(const std::vector<FileState>& states, bool after) {
    if (after) {
        for (const FileState& state : states) putFileState(state.path, state.after);
    } else {
        for (auto state = states.rbegin(); state != states.rend(); ++state) putFileState(state->path, state->before);
    }
}

bool fileThere(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (fs::is_regular_file(fs::u8path(path), ec)) return true;
    return isImagePath(path) && fileStamp(path) != 0;  // (a tile set: any tile, udim.h)
}

namespace {

// A file name as Find Missing compares names: case-blind on Windows.
std::string nameKey(std::string name) {
#ifdef _WIN32
    std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
#endif
    return name;
}

// A tile's file name split: (its set's name with <udim> in the tile's place, the plain name when
// the tile follows a dot: "dens.1003.png" -> ("dens.<udim>.png", "dens.png")); empty when the name
// is no tile's.
std::pair<std::string, std::string> tileSetNames(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot < 5) return {};
    const std::string stem = name.substr(0, dot), ext = name.substr(dot);
    const std::string digits = stem.substr(stem.size() - 4);
    const char sep = stem[stem.size() - 5];
    if ((sep != '.' && sep != '_') || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return {};
    }
    const int tile = std::stoi(digits);
    if (tile < kFirstUdimTile || tile >= kFirstUdimTile + kUdimTiles) return {};
    const std::string head = stem.substr(0, stem.size() - 5);
    return {head + sep + "<udim>" + ext, sep == '.' ? head + ext : std::string()};
}

}  // namespace

FileIndex indexFolder(const std::string& folder, bool recursive) {
    FileIndex index;
    std::error_code ec;
    const fs::path root = fs::absolute(fs::u8path(folder), ec);
    auto note = [&](const fs::directory_entry& e) {
        std::error_code err;
        if (!e.is_regular_file(err)) return;
        const std::string name = e.path().filename().u8string();
        const std::string path = e.path().generic_u8string();
        index.files[nameKey(name)].push_back(path);
        const auto [setName, plainName] = tileSetNames(name);
        const std::string dir = e.path().parent_path().generic_u8string();
        if (!setName.empty()) index.sets[nameKey(setName)].insert(dir);
        if (!plainName.empty()) index.sets[nameKey(plainName)].insert(dir);
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            note(*it);
        }
    } else {
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) note(*it);
    }
    for (auto& [name, paths] : index.files) std::sort(paths.begin(), paths.end());
    return index;
}

std::string matchMissingFile(const FileIndex& index, const std::string& stored, std::vector<std::string>* candidatesOut) {
    std::vector<std::string> parts;
    for (const fs::path& part : fs::u8path(stored).lexically_normal()) {
        const std::string p = part.generic_u8string();
        if (!p.empty() && p != "." && p != "/") parts.push_back(p);
    }
    if (parts.empty()) return {};
    const std::string base = parts.back();
    std::vector<std::string> candidates;
    auto fromSets = [&](const std::string& key) {
        const auto it = index.sets.find(nameKey(key));
        if (it == index.sets.end()) return;
        for (const std::string& dir : it->second) candidates.push_back(dir + "/" + base);
    };
    if (const char* token = udimTokenIn(base)) {
        std::string key = base;
        key.replace(key.find(token), std::string(token).size(), "<udim>");
        fromSets(key);  // a set by its token, the token kept
    } else {
        const auto it = index.files.find(nameKey(base));
        if (it != index.files.end()) candidates = it->second;
        if (candidates.empty()) fromSets(base);  // a texture map's plain path standing for a set
    }
    if (candidates.size() == 1) return candidates.front();
    if (candidates.empty()) return {};
    // Several: the one whose folders end as the stored path's do, by at least two parts and more
    // than any other.
    auto tail = [&](const std::string& path) {
        std::vector<std::string> theirs;
        for (const fs::path& part : fs::u8path(path)) theirs.push_back(part.generic_u8string());
        std::size_t n = 0;
        while (n < parts.size() && n < theirs.size() &&
               nameKey(parts[parts.size() - 1 - n]) == nameKey(theirs[theirs.size() - 1 - n])) {
            ++n;
        }
        return n;
    };
    std::vector<std::pair<std::size_t, std::string>> scores;
    for (const std::string& c : candidates) scores.emplace_back(tail(c), c);
    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    if (scores[0].first > 1 && scores[0].first > scores[1].first) return scores[0].second;
    if (candidatesOut) {
        std::sort(candidates.begin(), candidates.end());
        *candidatesOut = candidates;
    }
    return {};
}

bool samePathText(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && key(a) == key(b); }

bool sameFile(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::equivalent(fs::u8path(a), fs::u8path(b), ec)) return true;
    // Not both there: canonical as far as they exist (a folder's links), then as samePathText.
    auto canonical = [](const std::string& p) {
        std::error_code e;
        const fs::path c = fs::weakly_canonical(fs::u8path(p), e);
        return e ? p : c.generic_u8string();
    };
    return samePathText(canonical(a), canonical(b));
}

std::string freeFilePath(const std::string& path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    fs::path out = p;
    // (Taken: a file, or an image's tile set of that name: "density.1001.png"... for "density.png".)
    auto taken = [&](const fs::path& at) { return fs::exists(at, ec) || (isImagePath(at.u8string()) && fileStamp(at.generic_u8string()) != 0); };
    for (int k = 2; taken(out); ++k) out = p.parent_path() / (p.stem().u8string() + "_" + std::to_string(k) + p.extension().u8string());
    return out.generic_u8string();
}

std::string retargetFileReaders(const std::string& graphJson, const std::string& from, const std::string& to,
                                const std::function<std::string(const std::string&)>& resolve, bool* changed) {
    if (changed) *changed = false;
    if (from.empty() || to.empty()) return graphJson;
    return followGraphFiles(
        graphJson, [&](const std::string& stored) { return samePathText(resolve ? resolve(stored) : stored, from) ? to : stored; },
        changed);
}

}  // namespace groomlab
