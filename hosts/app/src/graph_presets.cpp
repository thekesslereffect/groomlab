// SPDX-License-Identifier: Apache-2.0
#include "graph_presets.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <QCoreApplication>

#include <nlohmann/json.hpp>

#include "groom_document.h"
#include "groomlab/guide_hierarchy.h"

namespace groomlab::app {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxText = 4 * 1024 * 1024;  // characters: a big groom's graph is well under 100 KB

std::string readFile(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<double> scalpSizeOf(const GroomDocument& document) {
    // At the size the params are at: the groom's 1x when it is scaled with its scalp.
    const MeshPtr& scalp = document.file().data.scalp;
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= document.session().groomScale();
    return size;
}

// Text pasted from a chat: a ``` code block's content (```json too), else the text.
std::string unfenced(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    if (text.rfind("```", 0) == 0) {
        const auto newline = text.find('\n');
        if (newline != std::string::npos && text.size() >= 3 && text.compare(text.size() - 3, 3, "```") == 0) {
            text = text.substr(newline + 1, text.size() - newline - 4);
        }
    }
    return text;
}

std::string titleOf(const json& preset, const std::string& fallback) {
    if (preset.contains("about") && preset["about"].is_object() && preset["about"].contains("title") && preset["about"]["title"].is_string()) {
        const std::string title = preset["about"]["title"].get<std::string>();
        if (!title.empty()) return title;
    }
    return fallback;
}

}  // namespace

std::string shippedPresetDir() {
    std::error_code ec;
    const fs::path beside = fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "presets";
    if (fs::is_directory(beside, ec)) return beside.generic_u8string();
#ifdef GROOMLAB_SHIPPED_PRESETS
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_PRESETS), ec)) return GROOMLAB_SHIPPED_PRESETS;
#endif
    return {};
}

std::string shippedAlphaFolder() {
    std::error_code ec;
    const fs::path beside = fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "alphas";
    if (fs::is_directory(beside, ec)) return beside.generic_u8string();
#ifdef GROOMLAB_SHIPPED_ALPHAS
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_ALPHAS), ec)) return GROOMLAB_SHIPPED_ALPHAS;
#endif
    return {};
}

std::string shippedNodeFolder() {
    std::error_code ec;
    const fs::path beside = fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "nodes";
    if (fs::is_directory(beside, ec)) return beside.generic_u8string();
#ifdef GROOMLAB_SHIPPED_NODES
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_NODES), ec)) return GROOMLAB_SHIPPED_NODES;
#endif
    return {};
}

std::string shippedMeshFolder() {
    std::error_code ec;
    const fs::path beside = fs::u8path(QCoreApplication::applicationDirPath().toStdString()) / "meshes";
    if (fs::is_directory(beside, ec)) return beside.generic_u8string();
#ifdef GROOMLAB_SHIPPED_MESHES
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_MESHES), ec)) return GROOMLAB_SHIPPED_MESHES;
#endif
    return {};
}

std::vector<std::string> presetDirectories() { return presetSearchDirs(presetUserDir(), shippedPresetDir()); }

std::vector<PresetInfo> availablePresets() { return listPresets(presetDirectories()); }

std::string applyPresetFile(GroomDocument& document, const std::string& path, PresetApplied* applied) {
    std::string text;
    try {
        text = readFile(path);
    } catch (const std::exception& e) {
        return e.what();
    }
    PresetApplied done;
    const std::string error = applyPresetText(document, text, true, &done);
    if (error.empty() && done.title.empty()) done.title = presetTitle(fs::u8path(path).stem().u8string());
    if (applied) *applied = done;
    return error;
}

namespace {

// The painted maps a preset's graph reads, made as the Maps panel makes them, when the groom has
// none of that name (the Maya host's prepare).
void makeRequiredMaps(GroomDocument& document, const std::string& body, PresetApplied& done) {
    for (const PresetMap& m : presetRequiredMaps(body)) {
        if (document.map(m.name)) continue;
        PaintColor fill{static_cast<float>(m.value), static_cast<float>(m.value), static_cast<float>(m.value), 1.0f};
        const bool color = !m.color.empty();
        if (color) {
            for (std::size_t k = 0; k < std::min<std::size_t>(4, m.color.size()); ++k) fill[k] = static_cast<float>(m.color[k]);
        }
        const std::string error = document.createMap(m.name, m.kind, color, static_cast<float>(m.value), 1024, fill);
        if (error.empty()) done.mapsMade.push_back(m.name);
    }
}

// The guide groups a preset's graph reads, made empty when the groom has none of that name (the
// Maya host's prepare); the one its Draw tool draws into.
void makeRequiredGroups(GroomDocument& document, const std::string& body, PresetApplied& done) {
    for (const PresetGroup& g : presetRequiredGroups(body)) {
        const HierarchyPtr h = document.session().authoredHierarchy();
        const bool had = h && h->findGroup(g.name) != kNoGroup;
        if (!had && document.ensureGuideGroup(g.name).empty()) done.groupsMade.push_back(g.name);
        if (g.draw && done.drawGroup.empty()) done.drawGroup = g.name;
    }
}

// How far the preset's sizes scale to this scalp.
void noteFit(const GroomDocument& document, const json& preset, PresetApplied& done) {
    const std::optional<double> size = scalpSizeOf(document);
    const bool recorded = preset.contains("size") && preset["size"].is_number() && preset["size"].get<double>() > 0.0;
    if (recorded && size) {
        done.fitFactor = *size / preset["size"].get<double>();
        done.fitted = std::abs(done.fitFactor - 1.0) >= 1e-6;
    }
}

}  // namespace

std::string applyPresetText(GroomDocument& document, const std::string& text, bool makeMaps, PresetApplied* applied) {
    if (text.size() > kMaxText) return "the text is bigger than any groom's graph";
    const std::string body = unfenced(text);
    json preset = json::parse(body, nullptr, false);
    if (!preset.is_object() || !preset.contains("nodes") || !preset["nodes"].is_array()) {
        return "no GroomLab graph in the text (no \"nodes\")";
    }
    PresetApplied done;
    done.title = titleOf(preset, "the graph");
    if (makeMaps) {
        makeRequiredMaps(document, body, done);
        makeRequiredGroups(document, body, done);
    }
    // The graph fitted to this scalp, its control values set where the graph holds them.
    const std::optional<double> size = scalpSizeOf(document);
    noteFit(document, preset, done);
    std::string graph;
    try {
        graph = applyPresetControlsToGraph(presetGraphJson(body, size), presetControlsJson(body, size));
    } catch (const std::exception& e) {
        return e.what();
    }
    const std::string error = document.editGraph("preset " + done.title, [&](json& g) { g = json::parse(graph); });
    if (!error.empty()) return error;
    done.nodes = document.nodes().size();
    if (applied) *applied = done;
    return {};
}

std::string insertPresetFile(GroomDocument& document, const std::string& path, const std::string& after, PresetApplied* applied) {
    std::string text;
    try {
        text = readFile(path);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (text.size() > kMaxText) return "the preset is bigger than any groom's graph";
    const json preset = json::parse(text, nullptr, false);
    if (!preset.is_object() || !preset.contains("nodes") || !preset["nodes"].is_array()) return "no GroomLab graph in " + path;
    const std::string stem = fs::u8path(path).stem().u8string();
    PresetApplied done;
    done.title = titleOf(preset, presetTitle(stem));
    done.after = after;
    makeRequiredMaps(document, text, done);
    noteFit(document, preset, done);
    const std::optional<double> size = scalpSizeOf(document);
    std::string made;
    const std::string error = document.editGraph("insert " + done.title, [&](json& g) {
        g = json::parse(insertPresetJson(g.dump(), text, size, after, presetNodeName(stem), &made));
    });
    if (!error.empty()) return error;
    done.nodes = document.nodes().size();
    done.nodeMade = made;
    if (applied) *applied = done;
    return {};
}

std::string savePresetFile(const GroomDocument& document, const std::string& name, bool overwrite, std::string* pathOut) {
    if (const std::string error = presetNameError(name); !error.empty()) return error;
    const std::string dir = presetUserDir();
    if (dir.empty()) return "no user preset folder (set GROOMLAB_USER_PRESETS)";
    const fs::path path = fs::u8path(dir) / (name + ".json");
    std::error_code ec;
    if (fs::exists(path, ec) && !overwrite) return "a preset named " + name + " exists";
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return "cannot write " + path.generic_u8string();
    out << makePresetJson(document.graphJson(), scalpSizeOf(document), {}, {}, {}) << "\n";
    if (pathOut) *pathOut = path.generic_u8string();
    return {};
}

std::string graphAsText(const GroomDocument& document) {
    const std::string stem = document.path().empty() ? "groom" : fs::u8path(document.path()).stem().u8string();
    return makePresetJson(document.graphJson(), scalpSizeOf(document), stem, "Copied from " + stem + " (GroomLab)", "Shared");
}

std::string pasteGraphText(GroomDocument& document, const std::string& text, PresetApplied* applied) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return "the clipboard is empty";
    const std::string error = applyPresetText(document, text, false, applied);
    return error.empty() ? error : "cannot paste the graph: " + error;
}

std::string presetMessage(const PresetApplied& a) {
    std::string text = a.title + ": " + std::to_string(a.nodes) + (a.nodes == 1 ? " node" : " nodes");
    if (a.fitted) {
        char factor[32];
        std::snprintf(factor, sizeof factor, "%.3g", a.fitFactor);
        text += ", sizes fitted to this scalp (x" + std::string(factor) + ")";
    }
    if (!a.mapsMade.empty()) {
        text += ", made the map" + std::string(a.mapsMade.size() == 1 ? " " : "s ");
        for (std::size_t i = 0; i < a.mapsMade.size(); ++i) text += (i ? ", " : "") + a.mapsMade[i];
    }
    if (!a.groupsMade.empty()) {
        text += ", made the guide group" + std::string(a.groupsMade.size() == 1 ? " " : "s ");
        for (std::size_t i = 0; i < a.groupsMade.size(); ++i) text += (i ? ", " : "") + a.groupsMade[i];
    }
    return text;
}

}  // namespace groomlab::app
