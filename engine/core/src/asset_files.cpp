// SPDX-License-Identifier: Apache-2.0
#include "groomlab/asset_files.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "groomlab/paths.h"
#include "groomlab/texture.h"

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

// file -> file_2, file_3... (before the extension) until one is free.
std::string freeName(const std::string& path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    fs::path out = p;
    for (int k = 2; fs::exists(out, ec); ++k) out = p.parent_path() / (p.stem().u8string() + "_" + std::to_string(k) + p.extension().u8string());
    return out.generic_u8string();
}

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
    if (!fs::is_regular_file(fs::u8path(source), ec)) return storedPath;
    std::string target = destination(source, storedPath, relative);
    if (target.empty() || key(target) == key(source)) return relative ? stored(source, true) : storedPath;
    if (fs::exists(fs::u8path(target), ec) && !sameContent(source, target)) {
        const std::string free = freeName(target);
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

void visitFileParams(nlohmann::json& graph, const std::function<void(nlohmann::json&)>& visit) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (nlohmann::json& node : graph["nodes"]) {
        if (!node.is_object() || !node.contains("params") || !node["params"].is_object()) continue;
        const std::string type = node.value("type", std::string());
        for (auto it = node["params"].begin(); it != node["params"].end(); ++it) {
            const std::string& param = it.key();
            nlohmann::json& value = it.value();
            const bool named = param == "path" || param == "texture" || param == "textureRight";
            const bool image = ((param == "map" && type == "HairColor") || (param == "regionMap" && type == "Interpolate")) &&
                               value.is_string() && isImagePath(value.get<std::string>());
            if (value.is_object() && value.contains("texture") && value["texture"].is_string()) {
                visit(value["texture"]);
            } else if ((named || image) && value.is_string()) {
                visit(value);
            }
        }
        if (type == "Compound" && node["params"].contains("definition") && node["params"]["definition"].is_object()) {
            nlohmann::json& definition = node["params"]["definition"];
            if (definition.contains("graph")) visitFileParams(definition["graph"], visit);
        }
    }
}

}  // namespace

bool insideFolder(const std::string& path, const std::string& folder) { return !inside(path, folder).empty(); }

std::string collectFile(const std::string& path, const std::string& folder, std::vector<std::string>* copied) {
    std::error_code ec;
    const std::string token = "<UDIM>";
    const std::size_t at = path.find(token);
    std::vector<std::pair<std::string, std::string>> tiles;  // (source, name in the folder)
    std::string name = fs::u8path(path).filename().u8string();
    if (at != std::string::npos) {
        // A UDIM set: every tile 1001-1999 there, under the set's own name.
        for (int tile = 1001; tile <= 1999; ++tile) {
            std::string source = path;
            source.replace(at, token.size(), std::to_string(tile));
            if (fs::is_regular_file(fs::u8path(source), ec)) tiles.emplace_back(source, fs::u8path(source).filename().u8string());
        }
    } else if (fs::is_regular_file(fs::u8path(path), ec)) {
        tiles.emplace_back(path, name);
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

std::string followGraphFiles(const std::string& graphJson, const std::function<std::string(const std::string&)>& follow, bool* changed) {
    if (changed) *changed = false;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded()) return graphJson;
    bool any = false;
    visitFileParams(graph, [&](nlohmann::json& value) {
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

bool samePathText(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && key(a) == key(b); }

std::string retargetFileReaders(const std::string& graphJson, const std::string& from, const std::string& to,
                                const std::function<std::string(const std::string&)>& resolve, bool* changed) {
    if (changed) *changed = false;
    if (from.empty() || to.empty()) return graphJson;
    return followGraphFiles(
        graphJson, [&](const std::string& stored) { return samePathText(resolve ? resolve(stored) : stored, from) ? to : stored; },
        changed);
}

}  // namespace groomlab
