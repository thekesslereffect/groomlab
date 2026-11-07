// SPDX-License-Identifier: Apache-2.0
// GroomDocument: what the Groom Tools' lists edit besides the guides themselves (Maya's
// tool_tabs.py GuidesTab and PaintTab, the Blender helper's guides_edit and map_* commands): the
// guide groups' colors and renames, and the painted maps' import, relink, reload, convert,
// channels, fill and removal (with what reads them, and the files GroomLab made). Each edit one
// undo step; the files it writes or deletes are part of the step (undo puts their bytes back).

#include "groom_document.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "groomlab/paths.h"
#include "groomlab/texture.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

std::string forwardSlashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::optional<std::string> readBytes(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) return std::nullopt;
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeBytes(const std::string& path, const std::optional<std::string>& bytes) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(path);
    if (!bytes) {
        std::filesystem::remove(p, ec);
        return;
    }
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
}

// Whether `path` lies inside `folder` (case-insensitive on Windows), as the helper's inFolder.
bool inFolder(const std::string& path, const std::string& folder) {
    if (path.empty() || folder.empty()) return false;
    auto norm = [](const std::string& p) {
        std::string out = std::filesystem::u8path(p).lexically_normal().generic_u8string();
#ifdef _WIN32
        std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
#endif
        return out;
    };
    std::string f = norm(folder);
    while (!f.empty() && f.back() == '/') f.pop_back();
    const std::string p = norm(path);
    return p.size() > f.size() + 1 && p.compare(0, f.size(), f) == 0 && p[f.size()] == '/';
}

bool samePath(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(std::filesystem::u8path(a), std::filesystem::u8path(b), ec)) return true;
    return std::filesystem::u8path(a).lexically_normal() == std::filesystem::u8path(b).lexically_normal();
}

bool pngPath(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return ext == ".png";
}

// An image is gray when every texel's r, g and b agree (the helper's imageIsGray).
bool imageIsGray(const Texture& image) {
    const Texture::Image* im = image.image();
    if (!im) return false;
    for (std::size_t i = 0; i + 2 < im->rgba.size(); i += 4) {
        if (im->rgba[i] != im->rgba[i + 1] || im->rgba[i + 1] != im->rgba[i + 2]) return false;
    }
    return true;
}

// The group names a Guides node's exclude holds (a list, or names split by commas and spaces).
std::vector<std::string> groupList(const json& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const json& v : value) {
            if (v.is_string() && !v.get<std::string>().empty()) out.push_back(v.get<std::string>());
        }
        return out;
    }
    if (!value.is_string()) return out;
    std::string word;
    for (const char c : value.get<std::string>() + ",") {
        if (c == ',' || c == ' ' || c == ';' || c == '\t') {
            if (!word.empty()) out.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    return out;
}

bool hexColor(const std::string& text) {
    return text.size() == 7 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// The map names HairColor and Interpolate read by a string param.
constexpr std::pair<const char*, const char*> kNamedMapParams[] = {{"HairColor", "map"}, {"Interpolate", "regionMap"}};

}  // namespace

// ---- Side effects of a step: files and group colors -------------------------------------------------

void GroomDocument::attachSide(std::size_t undoBefore, const std::string& label, std::vector<Edit::FileState> files,
                               const std::map<std::string, std::string>* colorsBefore) {
    const bool colorsChanged = colorsBefore && *colorsBefore != groupColors_;
    if (files.empty() && !colorsChanged) return;
    if (!(undoIndex_ > undoBefore && undoIndex_ == edits_.size())) {
        Edit side;
        side.side = true;
        side.label = label;
        record(std::move(side));
    }
    Edit& e = edits_.back();
    for (Edit::FileState& f : files) e.files.push_back(std::move(f));
    if (colorsChanged) {
        if (!e.colors) e.colorsBefore = *colorsBefore;
        e.colors = true;
        e.colorsAfter = groupColors_;
    }
    dirty_ = true;
}

void GroomDocument::applySide(const Edit& edit, bool after) {
    if (edit.colors) groupColors_ = after ? edit.colorsAfter : edit.colorsBefore;
    if (edit.subdivision) {
        // Scalp Subdivision: the file's scalp takes the level, the session the scalp of the time.
        const Edit::Subdivision& s = after ? edit.subdivision->second : edit.subdivision->first;
        scene_->setScalpRenderSubdivision(s.levels, s.from);
        try {
            scene_->setTime(time_);
        } catch (const std::exception&) {
            // (a cache that cannot be read: the next evaluation says so)
        }
    }
    if (edit.files.empty()) return;
    if (after) {
        for (const Edit::FileState& f : edit.files) writeBytes(f.path, f.after);
    } else {
        for (auto f = edit.files.rbegin(); f != edit.files.rend(); ++f) writeBytes(f->path, f->before);
    }
    // The strokes' kept paints and the scalp's sampled texture are of the files as they were.
    paints_.clear();
    mapViewKey_ = 0;
    mapViewValues_.reset();
}

// ---- Guide groups --------------------------------------------------------------------------------

std::string GroomDocument::groupColorsText() const {
    std::string text;
    for (const auto& [name, color] : groupColors_) text += (text.empty() ? "" : " ") + name + ":" + color;
    return text;
}

std::string GroomDocument::setGroupColor(const std::string& group, const std::string& color) {
    const HierarchyPtr h = session().authoredHierarchy();
    if (!h || h->findGroup(group) == kNoGroup) return "no group named '" + group + "'";
    std::string c = color;
    std::transform(c.begin(), c.end(), c.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    if (!c.empty() && !hexColor(c)) return "a group's color is #rrggbb";
    const std::map<std::string, std::string> before = groupColors_;
    if (c.empty()) groupColors_.erase(group);
    else groupColors_[group] = c;
    attachSide(undoIndex_, (c.empty() ? "default color of " : "color of ") + group, {}, &before);
    return {};
}

std::string GroomDocument::renameGroup(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) return "a group needs a name";
    if (from == to) return {};
    const std::map<std::string, std::string> before = groupColors_;
    const std::size_t undoBefore = undoIndex_;
    const std::string error = editGuides("rename group", [&](GuideHierarchy& h) {
        const GroupId id = h.findGroup(from);
        if (id == kNoGroup) throw std::invalid_argument("no group named " + from);
        h.renameGroup(id, to);
    });
    if (!error.empty()) return error;
    // Its color and its hidden state go with it (the color back with an undo).
    const auto color = groupColors_.find(from);
    if (color != groupColors_.end()) {
        groupColors_[to] = color->second;
        groupColors_.erase(from);
    }
    attachSide(undoBefore, "rename group", {}, &before);
    if (hiddenGroups_.erase(from)) hiddenGroups_.insert(to);
    return {};
}

std::vector<std::pair<std::string, std::string>> GroomDocument::groupUsers(const std::string& group) const {
    std::vector<std::pair<std::string, std::string>> users;
    for (const json& n : graph_.value("nodes", json::array())) {
        if (!n.is_object() || n.value("type", std::string()) != "Guides" || !n.contains("params") || !n["params"].is_object()) continue;
        const json& params = n["params"];
        const std::string node = n.value("name", std::string());
        if (params.value("group", json()).is_string() && params["group"].get<std::string>() == group) users.emplace_back(node, "group");
        if (params.contains("exclude")) {
            const std::vector<std::string> names = groupList(params["exclude"]);
            if (std::find(names.begin(), names.end(), group) != names.end()) users.emplace_back(node, "exclude");
        }
    }
    return users;
}

// ---- Maps: what reads them --------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> GroomDocument::mapUsers(const std::string& name) const {
    std::vector<std::pair<std::string, std::string>> users;
    std::set<std::string> readers;
    const json nodes = graph_.value("nodes", json::array());
    for (const json& n : nodes) {
        if (!n.is_object()) continue;
        const json params = n.contains("params") && n["params"].is_object() ? n["params"] : json::object();
        if (n.value("type", std::string()) == "Map" && params.value("name", std::string("mask")) == name) readers.insert(n.value("name", std::string()));
    }
    for (const json& c : graph_.value("connections", json::array())) {
        if (c.is_object() && readers.count(c.value("from", std::string()))) users.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
    }
    for (const json& n : nodes) {
        if (!n.is_object() || !n.contains("params") || !n["params"].is_object()) continue;
        const std::string node = n.value("name", std::string()), type = n.value("type", std::string());
        for (const auto& [param, v] : n["params"].items()) {
            if (v.is_object() && (v.value("guide", std::string()) == name || v.value("colorSet", std::string()) == name)) users.emplace_back(node, param);
        }
        for (const auto& [t, param] : kNamedMapParams) {
            if (type == t && n["params"].value(param, json()).is_string() && n["params"][param].get<std::string>() == name) users.emplace_back(node, param);
        }
    }
    return users;
}

std::string GroomDocument::missingMapFile(const std::string& name) const {
    const ScalpMaps& maps = scene_->session().scalpMaps();
    const auto it = maps.maps.find(name);
    if (it == maps.maps.end() || it->second.kind != "texture" || it->second.texture.empty()) return {};
    const std::string full = resolveMapPath(it->second.texture);
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(full), ec) ? std::string() : full;
}

// ---- Maps: files --------------------------------------------------------------------------------

std::string GroomDocument::freeMapFile(const std::string& name) const {
    const std::filesystem::path folder = std::filesystem::u8path(mapsDirectory());
    std::filesystem::path file = folder / (name + ".png");
    std::error_code ec;
    for (int k = 2; std::filesystem::exists(file, ec); ++k) file = folder / (name + "_" + std::to_string(k) + ".png");
    return forwardSlashes(file.u8string());
}

std::string GroomDocument::storedMapPath(const std::string& full) const {
    if (path_.empty()) return forwardSlashes(full);
    return pathFromGroomFolder(full, std::filesystem::u8path(path_).parent_path().u8string());
}

std::string GroomDocument::mapImageFile(const std::string& map, const std::string& path, std::vector<Edit::FileState>* copied, int* channels) {
    if (path.empty()) throw std::runtime_error("give the image's path");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) throw std::runtime_error("no image at " + path);
    TexturePtr image;
    try {
        image = loadTexture(path);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + " does not read as an image: " + e.what());
    }
    if (!image || image->isUdim() || !image->image()) throw std::runtime_error("a map's image is one file over UVs 0-1, not a UDIM tile set");
    if (channels) *channels = imageIsGray(*image) ? kGrayMap : kColorMap;
    if (pngPath(path)) return forwardSlashes(std::filesystem::u8path(path).u8string());  // in place: painting edits it
    // Another format is copied into the maps folder as a PNG (painting writes PNG).
    if (mapsDirectory().empty()) throw std::runtime_error("save the groom first: a JPEG, TGA, PSD... is copied as a PNG into the groomlab folder beside it");
    const std::string target = freeMapFile(map);
    Edit::FileState state{target, readBytes(target), std::nullopt};
    std::filesystem::create_directories(std::filesystem::u8path(target).parent_path(), ec);
    try {
        writeImagePng(*image, target);
    } catch (const std::exception& e) {
        throw std::runtime_error("could not write " + target + ": " + e.what());
    }
    state.after = readBytes(target);
    if (copied) copied->push_back(std::move(state));
    return target;
}

// ---- Maps: edits ----------------------------------------------------------------------------------

std::string GroomDocument::removeMap(const std::string& name, bool deleteFile, bool* fileDeleted) {
    if (fileDeleted) *fileDeleted = false;
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    const std::string kind = source->kind;
    const std::string label = "remove map " + name;
    // What reads it lets go of it (Maya's _unlink_map): its Map nodes go with their links (an
    // exposed param's port with them), map params naming it and the map names HairColor and
    // Interpolate read go back to the node's defaults.
    json graph = graph_;
    bool graphChanged = false;
    {
        std::set<std::string> gone;
        for (const json& n : graph.value("nodes", json::array())) {
            if (!n.is_object() || n.value("type", std::string()) != "Map") continue;
            const json params = n.contains("params") && n["params"].is_object() ? n["params"] : json::object();
            if (params.value("name", std::string("mask")) == name) gone.insert(n.value("name", std::string()));
        }
        auto defaultOf = [&](const std::string& type, const std::string& param) {
            const json* s = spec(type);
            if (s && s->contains("params")) {
                for (const json& p : (*s)["params"]) {
                    if (p.value("name", std::string()) == param && p.contains("default")) return p["default"];
                }
            }
            return json();
        };
        json kept = json::array();
        for (json& n : graph["nodes"]) {
            if (!n.is_object()) continue;
            const std::string node = n.value("name", std::string()), type = n.value("type", std::string());
            if (gone.count(node)) {
                graphChanged = true;
                continue;
            }
            if (n.contains("paramInputs") && n["paramInputs"].is_array()) {
                json ports = json::array();
                for (const json& port : n["paramInputs"]) {
                    bool fed = false;
                    for (const json& c : graph.value("connections", json::array())) {
                        if (c.is_object() && gone.count(c.value("from", std::string())) && c.value("to", std::string()) == node && c.value("input", json()) == port) fed = true;
                    }
                    if (!fed) ports.push_back(port);
                }
                if (ports.size() != n["paramInputs"].size()) {
                    if (ports.empty()) n.erase("paramInputs");
                    else n["paramInputs"] = ports;
                }
            }
            if (n.contains("params") && n["params"].is_object()) {
                json& params = n["params"];
                std::vector<std::string> reset;
                for (const auto& [param, v] : params.items()) {
                    if (v.is_object() && (v.value("guide", std::string()) == name || v.value("colorSet", std::string()) == name)) reset.push_back(param);
                }
                for (const auto& [t, param] : kNamedMapParams) {
                    if (type == t && params.value(param, json()).is_string() && params[param].get<std::string>() == name) reset.push_back(param);
                }
                for (const std::string& param : reset) {
                    const json d = defaultOf(type, param);
                    if (d.is_null()) params.erase(param);
                    else params[param] = d;
                    graphChanged = true;
                }
            }
            kept.push_back(std::move(n));
        }
        graph["nodes"] = std::move(kept);
        if (!gone.empty() && graph.contains("connections") && graph["connections"].is_array()) {
            json links = json::array();
            for (const json& c : graph["connections"]) {
                if (c.is_object() && (gone.count(c.value("from", std::string())) || gone.count(c.value("to", std::string())))) continue;
                links.push_back(c);
            }
            graph["connections"] = std::move(links);
        }
    }
    const std::size_t undoBefore = undoIndex_;
    std::vector<Edit::FileState> files;
    if (kind == "guides") {
        const std::string error = editGuides(label, [&](GuideHierarchy& h) { h.removeAttribute(name); });
        if (!error.empty()) return error;
    } else {
        ScalpMaps maps = session().scalpMaps();
        const std::string file = kind == "texture" ? resolveMapPath(maps.maps[name].texture) : std::string();
        maps.maps.erase(name);
        const bool shared = std::any_of(maps.maps.begin(), maps.maps.end(), [&](const auto& m) {
            return m.second.kind == "texture" && !file.empty() && samePath(resolveMapPath(m.second.texture), file);
        });
        std::error_code ec;
        if (deleteFile && !file.empty() && !shared && inFolder(file, mapsDirectory()) && std::filesystem::is_regular_file(std::filesystem::u8path(file), ec)) {
            Edit::FileState state{file, readBytes(file), std::nullopt};
            std::filesystem::remove(std::filesystem::u8path(file), ec);
            if (!ec) {
                files.push_back(std::move(state));
                if (fileDeleted) *fileDeleted = true;
            }
        }
        paints_.erase(name);
        commitMaps(label, maps);
    }
    if (graphChanged && undoIndex_ > undoBefore) {
        const json before = graph_;
        graph_ = std::move(graph);
        const std::string error = applyGraph();
        if (!error.empty()) {
            graph_ = before;
            applyGraph();
        } else {
            Edit& entry = edits_.back();
            entry.whole = true;
            entry.graphBefore = before;
            entry.graphAfter = graph_;
        }
    }
    attachSide(undoBefore, label, std::move(files));
    if (scalpMapName_ == name) scalpMapName_.clear();
    return {};
}

std::string GroomDocument::importMap(const std::string& path, std::string* made, const std::string& wanted) {
    std::string name = wanted;
    auto taken = [&](const std::string& n) { return map(n) != nullptr; };
    if (name.empty()) {
        // After the file: its letters, digits and _ (a free name).
        std::string stem;
        for (const char c : std::filesystem::u8path(path).stem().u8string()) stem += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
        const auto first = stem.find_first_not_of('_'), last = stem.find_last_not_of('_');
        stem = first == std::string::npos ? std::string("map") : stem.substr(first, last - first + 1);
        if (std::isdigit(static_cast<unsigned char>(stem[0]))) stem = "map_" + stem;
        name = stem;
        for (int n = 2; taken(name); ++n) name = stem + "_" + std::to_string(n);
    }
    if (!validMapName(name)) return mapNameError(name);
    if (taken(name)) return "the groom already has a map named '" + name + "'";
    const MeshPtr scalp = session().cageScalp();
    if (!scalp || !scalp->hasUVs()) return "the scalp has no UVs for a texture map";
    std::vector<Edit::FileState> files;
    int channels = kGrayMap;
    std::string full;
    try {
        full = mapImageFile(name, path, &files, &channels);
    } catch (const std::exception& e) {
        return e.what();
    }
    ScalpMaps maps = session().scalpMaps();
    ScalpMapEntry entry;
    entry.kind = "texture";
    entry.texture = storedMapPath(full);
    entry.channels = channels;
    maps.maps[name] = std::move(entry);
    const std::size_t undoBefore = undoIndex_;
    const std::string label = "import map " + name;
    commitMaps(label, maps);
    attachSide(undoBefore, label, std::move(files));
    if (made) *made = name;
    return {};
}

std::string GroomDocument::relinkMap(const std::string& name, const std::string& path) {
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    const std::string kind = source->kind;
    const int channels = source->channels;
    std::vector<Edit::FileState> files;
    std::string full;
    try {
        full = mapImageFile(name, path, &files, nullptr);
    } catch (const std::exception& e) {
        return e.what();
    }
    const std::string label = "relink map " + name;
    StrokeEdit edit;
    edit.mode = label;
    if (kind == "guides") {
        GuideHierarchy h = session().authoredHierarchy() ? *session().authoredHierarchy() : GuideHierarchy{};
        h.removeAttribute(name);
        try {
            h.evaluate(session().scalp().get());
        } catch (const std::exception& e) {
            return e.what();
        }
        edit.set(StrokeField::Guides, h.toJson());
    }
    ScalpMaps maps = session().scalpMaps();
    ScalpMapEntry entry;
    entry.kind = "texture";
    entry.texture = storedMapPath(full);
    entry.channels = channels;
    maps.maps[name] = std::move(entry);
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    paints_.erase(name);
    mapViewKey_ = 0;
    const std::size_t undoBefore = undoIndex_;
    commitStroke(std::move(edit));
    if (undoIndex_ > undoBefore) edits_.back().label = label;
    attachSide(undoBefore, label, std::move(files));
    return {};
}

std::string GroomDocument::reloadMap(const std::string& name) {
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    if (source->kind != "texture") return name + " is not a texture map";
    paints_.erase(name);
    mapViewKey_ = 0;
    mapViewValues_.reset();
    return {};
}

std::string GroomDocument::convertMap(const std::string& name, const std::string& to, int resolution) {
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    if (to != "guides" && to != "vertices" && to != "texture") return "a map's kind is guides, vertices or texture";
    const std::string from = source->kind;
    if (from == to) return {};
    const MeshPtr scalp = session().cageScalp();
    if (!scalp || scalp->pointCount() == 0) return "no scalp to hold the map";
    const int channels = source->channels;
    const int size = std::clamp(resolution, 16, 8192);
    ScalpMaps maps = session().scalpMaps();
    std::vector<float> vertices;
    TexturePtr image;
    const HierarchyPtr authored = session().authoredHierarchy();
    GuideSet flat;
    std::vector<Edit::FileState> files;
    const std::string label = "convert map " + name;
    StrokeEdit edit;
    edit.mode = label;
    try {
        if (from == "guides") {
            if (!authored) return "the groom has no guides";
            flat = authored->flatten();
        } else if (from == "vertices") {
            vertices = maps.maps[name].values;
            if (vertices.size() != scalp->pointCount() * static_cast<std::size_t>(channels)) return "map " + name + " does not fit this scalp";
        } else {
            try {
                image = loadTexture(resolveMapPath(maps.maps[name].texture));
            } catch (const std::exception& e) {
                return "map " + name + "'s image does not read: " + e.what();
            }
            if (!image || image->isUdim()) return "a map's image is one file over UVs 0-1, not a UDIM tile set";
        }
        if (to == "vertices") {
            ScalpMapEntry entry;
            entry.kind = "vertices";
            entry.values = from == "guides" ? vertexMapFromGuides(flat, name, *scalp) : vertexMapFromTexture(*image, *scalp, channels);
            entry.channels = channels;
            maps.maps[name] = std::move(entry);
        } else if (to == "texture") {
            if (!scalp->hasUVs()) return "the scalp has no UVs for a texture map";
            if (mapsDirectory().empty()) return "save the groom first: a texture map's image goes in the groomlab folder beside it";
            const std::string file = freeMapFile(name);
            const TexturePaint paint = from == "guides" ? textureFromGuides(flat, name, *scalp, size, size)
                                                        : textureFromVertexMap(vertices, channels, *scalp, size, size);
            Edit::FileState state{file, readBytes(file), std::nullopt};
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(file).parent_path(), ec);
            paint.save(file);
            state.after = readBytes(file);
            files.push_back(std::move(state));
            ScalpMapEntry entry;
            entry.kind = "texture";
            entry.texture = storedMapPath(file);
            entry.channels = channels;
            maps.maps[name] = std::move(entry);
        } else {
            if (channels == kColorMap && from == "vertices") vertices = convertMapChannels(vertices, kColorMap, kGrayMap);  // a guide layer is gray
            maps.maps.erase(name);
        }
        if (to == "guides" || from == "guides") {
            GuideHierarchy h = authored ? *authored : GuideHierarchy{};
            if (from == "guides") h.removeAttribute(name);
            if (to == "guides") {
                // Each guide's root as it lies on the cage (where vertex maps are painted).
                GuideHierarchy onCage = h;
                onCage.evaluate(scalp.get());
                std::vector<StrandRoot> roots;
                for (const HierarchyGuide& g : onCage.guides()) roots.push_back(g.root);
                const std::vector<float> values =
                    from == "vertices" ? rootValuesFromVertexMap(vertices, *scalp, roots) : rootValuesFromTexture(*image, *scalp, roots);
                std::size_t i = 0;
                for (const HierarchyGuide& g : onCage.guides()) h.setAttribute(g.id, name, values[i++]);
            }
            h.evaluate(session().scalp().get());
            edit.set(StrokeField::Guides, h.toJson());
        }
    } catch (const std::exception& e) {
        for (const Edit::FileState& f : files) writeBytes(f.path, f.before);
        return e.what();
    }
    paints_.erase(name);
    mapViewKey_ = 0;
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    const std::size_t undoBefore = undoIndex_;
    commitStroke(std::move(edit));
    if (undoIndex_ > undoBefore) edits_.back().label = label;
    attachSide(undoBefore, label, std::move(files));
    return {};
}

std::string GroomDocument::setMapChannels(const std::string& name, int wanted) {
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    if (wanted != kGrayMap && wanted != kColorMap) return "a map is gray (1) or color (4)";
    if (source->kind == "guides") return wanted == kColorMap ? "a guide layer is gray: convert " + name + " to vertices or a texture first" : std::string();
    if (source->channels == wanted) return {};
    ScalpMaps maps = session().scalpMaps();
    ScalpMapEntry& entry = maps.maps[name];
    std::vector<Edit::FileState> files;
    const std::string label = "map channels " + name;
    if (entry.kind == "vertices") {
        entry.values = convertMapChannels(entry.values, entry.channels == kColorMap ? kColorMap : kGrayMap, wanted);
    } else {
        const std::string full = resolveMapPath(entry.texture);
        const MeshPtr scalp = session().scalp();
        if (!scalp) return "no scalp";
        try {
            requirePaintablePng(full);
            const std::shared_ptr<TexturePaint> paint = openTexturePaint(*scalp, full, entry.channels == kColorMap ? kColorMap : kGrayMap, 1024);
            paint->setChannels(wanted);
            Edit::FileState state{full, readBytes(full), std::nullopt};
            paint->save(full);
            state.after = readBytes(full);
            files.push_back(std::move(state));
        } catch (const std::exception& e) {
            return e.what();
        }
        paints_.erase(name);
        mapViewKey_ = 0;
    }
    entry.channels = wanted;
    const std::size_t undoBefore = undoIndex_;
    commitMaps(label, maps);
    attachSide(undoBefore, label, std::move(files));
    return {};
}

std::string GroomDocument::fillMap(const std::string& name, float value, bool selectedOnly, const PaintColor* color, unsigned mask,
                                   PaintMode mode) {
    const PaintedMap* source = map(name);
    if (!source) return "no map named '" + name + "'";
    const std::string kind = source->kind;
    const int channels = source->channels;
    value = std::clamp(value, 0.0f, 1.0f);
    if (color && channels != kColorMap) return "a color fills a color map; " + name + " is gray (a value fills it)";
    if (selectedOnly && kind != "guides") return "Fill Selected sets a guide layer on the selected guides; " + name + " lives on the scalp";
    PaintColor fill = color ? *color : PaintColor{value, value, value, 1.0f};
    if (!color) mask &= 0x7u;  // a value fills a color map's r g b
    const std::string label = "fill map " + name;
    if (kind == "guides") {
        const std::vector<GuideId> picked = selection_;
        if (selectedOnly && picked.empty()) return "no guides selected";
        return editGuides(label, [&](GuideHierarchy& h) {
            if (selectedOnly) {
                for (const GuideId id : picked) {
                    if (const HierarchyGuide* g = h.guide(id)) {
                        float current = 0.0f;
                        h.effectiveAttribute(*g, name, &current);
                        h.setAttribute(id, name, compositePaintValue(current, value, mode));
                    }
                }
            } else {
                for (const HierarchyGuide& g : h.guides()) {
                    float current = 0.0f;
                    h.effectiveAttribute(g, name, &current);
                    h.setAttribute(g.id, name, compositePaintValue(current, value, mode));
                }
            }
        });
    }
    if (kind == "vertices") {
        const MeshPtr scalp = session().cageScalp();
        if (!scalp) return "no scalp";
        ScalpMaps maps = session().scalpMaps();
        ScalpMapEntry& entry = maps.maps[name];
        const auto per = static_cast<std::size_t>(channels);
        if (entry.values.size() != scalp->pointCount() * per) {
            entry.values.assign(scalp->pointCount() * per, 0.0f);
            if (per == 4) {
                for (std::size_t i = 3; i < entry.values.size(); i += 4) entry.values[i] = 1.0f;
            }
        }
        fillMapValues(entry.values, channels, mask, fill, mode);
        commitMaps(label, maps);
        return {};
    }
    // A texture map: the file rewritten, its bytes before and after the step.
    const std::string full = textureMapFile(name);
    const MeshPtr scalp = session().scalp();
    if (!scalp) return "no scalp";
    std::vector<Edit::FileState> files;
    try {
        requirePaintablePng(full);
        const std::shared_ptr<TexturePaint> paint = openTexturePaint(*scalp, full, channels, 1024);
        if (channels == kColorMap) paint->fill(fill, mask, mode);
        else paint->fill(value, mode);
        Edit::FileState state{full, readBytes(full), std::nullopt};
        paint->save(full);
        state.after = readBytes(full);
        files.push_back(std::move(state));
    } catch (const std::exception& e) {
        return e.what();
    }
    paints_.erase(name);
    mapViewKey_ = 0;
    attachSide(undoIndex_, label, std::move(files));
    return {};
}

}  // namespace groomlab::app
