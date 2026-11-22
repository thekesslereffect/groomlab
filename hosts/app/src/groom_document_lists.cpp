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

#include "groomlab/asset_files.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/paths.h"
#include "groomlab/texture.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

std::string forwardSlashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::vector<int> tilesOf(const TexturePaint& paint) { return paint.udim() ? paint.udimLayout()->tiles : std::vector<int>{}; }


bool hexColor(const std::string& text) {
    return text.size() == 7 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace

// ---- Side effects of a step: files and group colors -------------------------------------------------

void GroomDocument::attachSide(std::size_t undoBefore, const std::string& label, std::vector<FileState> files,
                               const std::map<std::string, std::string>* colorsBefore) {
    const bool colorsChanged = colorsBefore && *colorsBefore != groupColors_;
    if (files.empty() && !colorsChanged) return;
    if (!history_.recordedSince(undoBefore)) {
        Edit side;
        side.side = true;
        side.label = label;
        record(std::move(side));
    }
    Edit& e = history_.last();
    for (FileState& f : files) e.files.push_back(std::move(f));
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
    restoreFileStates(edit.files, after);
    // The strokes' kept paints and the scalp's sampled texture are of the files as they were.
    paints_.clear();
    mapViewKey_ = 0;
    mapViewValues_.reset();
}

// ---- Guide groups --------------------------------------------------------------------------------

std::string GroomDocument::groupColorsText() const { return guideGroupColorsText(groupColors_); }  // (display.h)

std::string GroomDocument::setGroupColor(const std::string& group, const std::string& color) {
    const HierarchyPtr h = session().authoredHierarchy();
    if (!h || h->findGroup(group) == kNoGroup) return "no group named '" + group + "'";
    std::string c = color;
    std::transform(c.begin(), c.end(), c.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    if (!c.empty() && !hexColor(c)) return "a group's color is #rrggbb";
    const std::map<std::string, std::string> before = groupColors_;
    if (c.empty()) groupColors_.erase(group);
    else groupColors_[group] = c;
    attachSide(history_.undoIndex(), (c.empty() ? "default color of " : "color of ") + group, {}, &before);
    return {};
}

std::string GroomDocument::renameGroup(const std::string& from, const std::string& typed) {
    const std::string to = guideGroupName(typed);  // one word: spaces as '_'
    if (from.empty() || to.empty()) return "a group needs a name";
    if (from == to) return {};
    const std::map<std::string, std::string> before = groupColors_;
    const std::size_t undoBefore = history_.undoIndex();
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
    // The Guides nodes reading or leaving out the group follow it (the core's rename, every
    // host's), in the same undo step: the entry holds the graph before and after too.
    const json graphBefore = graph_;
    bool retargeted = false;
    const std::string graph = renameGroupReaders(graph_.dump(), from, to, &retargeted);
    if (!retargeted) return {};
    graph_ = json::parse(graph);
    if (const std::string failed = applyGraph(); !failed.empty()) {
        graph_ = graphBefore;
        applyGraph();
        return failed;
    }
    Edit& entry = history_.last();
    entry.whole = true;
    entry.graphBefore = graphBefore;
    entry.graphAfter = graph_;
    return {};
}

std::vector<std::pair<std::string, std::string>> GroomDocument::groupUsers(const std::string& group) const {
    return groupReaders(graph_.dump(), group);  // the core's (graph_inserts.h), every host's "Read by"
}

// ---- Maps: what reads them --------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> GroomDocument::mapUsers(const std::string& name) const {
    return mapReaders(graph_.dump(), name).users;  // the core's (scalp_maps.h), every host's "Read by"
}

std::string GroomDocument::missingMapFile(const std::string& name) const {
    const ScalpMaps& maps = scene_->session().scalpMaps();
    const auto it = maps.maps.find(name);
    if (it == maps.maps.end() || it->second.kind != "texture" || it->second.texture.empty()) return {};
    const std::string full = resolveMapPath(it->second.texture);
    return fileStamp(full) != 0 ? std::string() : full;  // (a tile set: any of its tiles)
}

// ---- Maps: files --------------------------------------------------------------------------------

std::string GroomDocument::freeMapFile(const std::string& name) const {
    return freeFilePath((std::filesystem::u8path(mapsDirectory()) / (name + ".png")).u8string());
}

std::string GroomDocument::storedMapPath(const std::string& full) const {
    if (path_.empty()) return forwardSlashes(full);
    return pathFromGroomFolder(full, std::filesystem::u8path(path_).parent_path().u8string());
}

std::string GroomDocument::mapImageFile(const std::string& map, const std::string& path, std::vector<FileState>* copied, int* channels) {
    if (path.empty()) throw std::runtime_error("give the image's path");
    std::error_code ec;
    if (fileStamp(path) == 0) throw std::runtime_error("no image at " + path);  // (a tile set: any tile)
    TexturePtr image;
    try {
        image = loadTexture(path);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + " does not read as an image: " + e.what());
    }
    if (!image || (!image->isUdim() && !image->image())) throw std::runtime_error(path + " does not read as an image");
    if (channels) *channels = isGrayImage(*image) ? kGrayMap : kColorMap;
    if (isPngPath(path)) return forwardSlashes(std::filesystem::u8path(path).u8string());  // in place: painting edits it
    // Another format is copied into the maps folder as a PNG (painting writes PNG).
    if (mapsDirectory().empty()) throw std::runtime_error("save the groom first: a JPEG, TGA, PSD... is copied as a PNG into the groomlab folder beside it");
    const std::string target = freeMapFile(map);
    std::vector<FileState> states = mapImageStates(target, image->isUdim() ? image->udimTiles() : std::vector<int>{});
    std::filesystem::create_directories(std::filesystem::u8path(target).parent_path(), ec);
    try {
        writeImagePng(*image, target);  // (a tile set: each tile, udimPathFor(target))
    } catch (const std::exception& e) {
        throw std::runtime_error("could not write " + target + ": " + e.what());
    }
    std::vector<FileState> made;
    finishFileStates(states);
    made.insert(made.end(), states.begin(), states.end());
    if (copied) {
        for (FileState& state : made) copied->push_back(std::move(state));
    }
    return target;
}

// ---- Maps: edits ----------------------------------------------------------------------------------

std::string GroomDocument::removeMap(const std::string& name, bool deleteFile, bool* fileDeleted) {
    if (fileDeleted) *fileDeleted = false;
    // Every host's remove (the editor's removeMap, map_edits.h): what reads it lets go of it, and
    // with `deleteFile` the image GroomLab made (nothing else reading it) goes, its files kept in
    // the step so an undo puts them back.
    MapFiles files;
    files.mapsFolder = mapsDirectory();
    files.resolve = [this](const std::string& stored) { return resolveMapPath(stored); };
    MapRemove removed;
    try {
        removed = groomlab::removeMap(session().scalpMaps(), session().storedHierarchy().get(), graph_.dump(), name, deleteFile, files);
    } catch (const std::exception& e) {
        return e.what();
    }
    const std::string label = "remove map " + name;
    const std::size_t undoBefore = history_.undoIndex();
    if (removed.guideLayer) {
        const std::string error = editGuides(label, [&](GuideHierarchy& h) { h.removeAttribute(name); });
        if (!error.empty()) return error;
    } else {
        paints_.erase(name);
        commitMaps(label, removed.maps);
    }
    if (fileDeleted) *fileDeleted = removed.fileDeleted;
    if (!removed.graph.empty() && history_.undoIndex() > undoBefore) {
        const json before = graph_;
        graph_ = json::parse(removed.graph);
        const std::string error = applyGraph();
        if (!error.empty()) {
            graph_ = before;
            applyGraph();
        } else {
            Edit& entry = history_.last();
            entry.whole = true;
            entry.graphBefore = before;
            entry.graphAfter = graph_;
        }
    }
    attachSide(undoBefore, label, std::move(removed.files));
    if (scalpMapName_ == name) scalpMapName_.clear();
    return {};
}

std::string GroomDocument::importMap(const std::string& path, std::string* made, const std::string& wanted) {
    std::string name = wanted;
    auto taken = [&](const std::string& n) { return map(n) != nullptr; };
    if (name.empty()) name = mapNameFromFile(path, taken);  // (every host's rule, scalp_maps.h)
    if (!validMapName(name)) return mapNameError(name);
    if (taken(name)) return "the groom already has a map named '" + name + "'";
    const MeshPtr scalp = session().cageScalp();
    if (!scalp || !scalp->hasUVs()) return "the scalp has no UVs for a texture map";
    std::vector<FileState> files;
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
    const std::size_t undoBefore = history_.undoIndex();
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
    std::vector<FileState> files;
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
    if (kind == "texture") {  // texture params reading the map's old file read the new one
        bool followed = false;
        const std::string graph = retargetFileReaders(graph_.dump(), resolveMapPath(maps.maps[name].texture), entry.texture,
                                                      [this](const std::string& stored) { return resolveMapPath(stored); }, &followed);
        if (followed) edit.set(StrokeField::Graph, graph);
    }
    maps.maps[name] = std::move(entry);
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    paints_.erase(name);
    mapViewKey_ = 0;
    const std::size_t undoBefore = history_.undoIndex();
    commitStroke(std::move(edit));
    if (history_.undoIndex() > undoBefore) history_.last().label = label;
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
    if (source->kind == to) return {};
    const MeshPtr scalp = session().cageScalp();
    if (!scalp || scalp->pointCount() == 0) return "no scalp to hold the map";
    // Every host's Convert (the editor's convertMapKind, map_edits.h).
    MapFiles files;
    files.mapsFolder = mapsDirectory();
    files.groomFolder = path_.empty() ? std::string() : std::filesystem::u8path(path_).parent_path().u8string();
    files.resolve = [this](const std::string& stored) { return resolveMapPath(stored); };
    const HierarchyPtr authored = session().authoredHierarchy();
    MapConvert converted;
    try {
        converted = convertMapKind(session().scalpMaps(), authored.get(), *scalp, name, to, resolution, {}, files);
    } catch (const std::exception& e) {
        return e.what();
    }
    const std::string label = "convert map " + name;
    StrokeEdit edit;
    edit.mode = label;
    if (converted.removeLayer || converted.setLayer) {
        GuideHierarchy h = authored ? *authored : GuideHierarchy{};
        applyConvertedLayer(h, name, converted);
        h.evaluate(session().scalp().get());
        edit.set(StrokeField::Guides, h.toJson());
    }
    paints_.erase(name);
    mapViewKey_ = 0;
    edit.set(StrokeField::ScalpMaps, converted.maps.toJson());
    const std::size_t undoBefore = history_.undoIndex();
    commitStroke(std::move(edit));
    if (history_.undoIndex() > undoBefore) history_.last().label = label;
    attachSide(undoBefore, label, std::move(converted.files));
    return {};
}

std::string GroomDocument::setMapChannels(const std::string& name, int wanted) {
    // Every host's Channels (the editor's setMapChannels, map_edits.h): a vertex map's values, a
    // texture map's image rewritten (its bytes before and after kept in the step).
    MapFiles files;
    files.mapsFolder = mapsDirectory();
    files.groomFolder = path_.empty() ? std::string() : std::filesystem::u8path(path_).parent_path().u8string();
    files.resolve = [this](const std::string& stored) { return resolveMapPath(stored); };
    const MeshPtr cage = session().cageScalp();
    MapChannels changed;
    try {
        changed = groomlab::setMapChannels(session().scalpMaps(), session().authoredHierarchy().get(),
                                           cage && cage->pointCount() > 0 ? cage.get() : nullptr, name, wanted, 1024, files);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (!changed.mapsChanged) return {};
    const std::string label = "map channels " + name;
    if (!changed.files.empty()) {
        paints_.erase(name);
        mapViewKey_ = 0;
    }
    const std::size_t undoBefore = history_.undoIndex();
    commitMaps(label, changed.maps);
    attachSide(undoBefore, label, std::move(changed.files));
    return {};
}

std::string GroomDocument::fillMap(const std::string& name, float value, bool selectedOnly, const PaintColor* color, unsigned mask,
                                   PaintMode mode, const std::vector<std::int32_t>& faces) {
    // Every host's Fill (the editor's fillMap, map_edits.h): a guide layer's values, a vertex
    // map's, or a texture map's image rewritten (its bytes before and after kept in the step).
    MapFillRequest fill;
    fill.value = value;
    if (color) fill.color = *color;
    fill.mask = mask;
    fill.mode = mode;
    fill.selectedOnly = selectedOnly;
    if (selectedOnly) fill.selected = selection_;
    fill.faces = faces;
    MapFiles files;
    files.mapsFolder = mapsDirectory();
    files.groomFolder = path_.empty() ? std::string() : std::filesystem::u8path(path_).parent_path().u8string();
    files.resolve = [this](const std::string& stored) { return resolveMapPath(stored); };
    const HierarchyPtr authored = session().authoredHierarchy();
    const MeshPtr cage = session().cageScalp();
    MapFill filled;
    try {
        filled = groomlab::fillMap(session().scalpMaps(), authored.get(), cage && cage->pointCount() > 0 ? cage.get() : nullptr, name,
                                   fill, files);
    } catch (const std::exception& e) {
        return e.what();
    }
    const std::string label = "fill map " + name;
    if (filled.kind == "guides") return editGuides(label, [&](GuideHierarchy& h) { applyFilledLayer(h, name, filled); });
    const std::size_t undoBefore = history_.undoIndex();
    if (filled.mapsChanged) commitMaps(label, filled.maps);
    if (filled.kind == "texture") {
        paints_.erase(name);
        mapViewKey_ = 0;
    }
    attachSide(undoBefore, label, std::move(filled.files));
    return {};
}

}  // namespace groomlab::app
