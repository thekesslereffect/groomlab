// SPDX-License-Identifier: Apache-2.0
#include "mcp_groom.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>

#include "groomlab/frame_time.h"
#include "groomlab/graph_io.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/node_library.h"
#include "groomlab/presets.h"

namespace groomlab::cli {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// The first of the folders that exists ("" none).
std::string firstFolder(std::initializer_list<fs::path> candidates) {
    for (const fs::path& p : candidates) {
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec)) return fs::weakly_canonical(p, ec).generic_u8string();
    }
    return {};
}

fs::path sourceFolder(const char* path) { return path ? fs::u8path(path) : fs::path(); }

double r4(double v) { return std::round(v * 1e4) / 1e4; }
json vec(const Vec3f& v) { return json::array({r4(v.x), r4(v.y), r4(v.z)}); }

}  // namespace

Workspace::Workspace(std::string root, std::string exeDir) : root_(std::move(root)) {
    const fs::path exe = fs::u8path(exeDir);
    // Installed: the Maya module's GroomLab/bin beside GroomLab/presets (and the Linux build's the
    // same); a build tree: the sources'.
#ifdef GROOMLAB_SHIPPED_PRESETS
    presetDir_ = firstFolder({exe / ".." / "presets", exe / "presets", sourceFolder(GROOMLAB_SHIPPED_PRESETS)});
#else
    presetDir_ = firstFolder({exe / ".." / "presets", exe / "presets"});
#endif
#ifdef GROOMLAB_SHIPPED_NODES
    setShippedNodeDir(firstFolder({exe / ".." / "nodes", exe / "nodes", sourceFolder(GROOMLAB_SHIPPED_NODES)}));
#else
    setShippedNodeDir(firstFolder({exe / ".." / "nodes", exe / "nodes"}));
#endif
#ifdef GROOMLAB_SHIPPED_MESHES
    meshDir_ = firstFolder({exe / ".." / "meshes", exe / "meshes", sourceFolder(GROOMLAB_SHIPPED_MESHES)});
#else
    meshDir_ = firstFolder({exe / ".." / "meshes", exe / "meshes"});
#endif
#ifdef GROOMLAB_DOCS_DIR
    docsDir_ = firstFolder({exe / ".." / "docs" / "pages", sourceFolder(GROOMLAB_DOCS_DIR)});
#else
    docsDir_ = firstFolder({exe / ".." / "docs" / "pages"});
#endif
}

bool Workspace::insideRoot(const std::string& path) const {
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.is_relative()) p = fs::u8path(root_) / p;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p.lexically_normal();
    const fs::path rel = canonical.lexically_relative(fs::u8path(root_));
    if (rel.empty() || rel.is_absolute()) return false;
    const std::string first = rel.begin()->u8string();
    return first != "..";
}

std::string Workspace::resolve(const std::string& path) const {
    if (path.empty()) throw ToolError("no path given");
    if (path.find('\0') != std::string::npos) throw ToolError("the path holds a NUL character");
    if (!insideRoot(path)) {
        throw ToolError("'" + path + "' is outside the folder this server may read and write (" + root_ +
                        "): use a path inside it, relative to it");
    }
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.is_relative()) p = fs::u8path(root_) / p;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p.lexically_normal();
    return canonical.u8string();
}

std::string Workspace::resolveForWrite(const std::string& path) const {
    const std::string full = resolve(path);
    std::error_code ec;
    if (fs::is_directory(fs::u8path(full), ec)) throw ToolError("'" + path + "' is a folder: give a file name");
    fs::create_directories(fs::u8path(full).parent_path(), ec);
    return full;
}

std::string Workspace::shown(const std::string& path) const {
    if (!insideRoot(path)) return path;
    std::error_code ec;
    const fs::path rel = fs::weakly_canonical(fs::u8path(path), ec).lexically_relative(fs::u8path(root_));
    return rel.empty() ? path : rel.generic_u8string();
}

std::vector<std::string> Workspace::presetDirs() const { return presetSearchDirs(presetUserDir(), presetDir_); }

Groom& Workspace::groom(const std::string& name) {
    if (name.empty()) {
        if (grooms_.size() == 1) return grooms_.begin()->second;
        if (grooms_.empty()) throw ToolError("no groom is open: make one with new_groom or open one with open_groom");
        std::string names;
        for (const auto& [n, g] : grooms_) names += (names.empty() ? "" : ", ") + n;
        throw ToolError("several grooms are open (" + names + "): say which with \"groom\"");
    }
    const auto it = grooms_.find(name);
    if (it == grooms_.end()) {
        std::string names;
        for (const auto& [n, g] : grooms_) names += (names.empty() ? "" : ", ") + n;
        throw ToolError("no groom named '" + name + "'" + (names.empty() ? std::string(" (none is open)") : " (open: " + names + ")"));
    }
    return it->second;
}

Groom& Workspace::add(const std::string& name, std::unique_ptr<GroomScene> scene) {
    Groom& g = grooms_[name];
    g = Groom{};
    g.name = name;
    g.scene = std::move(scene);
    // The groom's own first frame when its caches animate.
    double start = 0.0;
    if (g.scene->timeRange(&start, nullptr)) g.frame = secondsToFrame(start, g.scene->framesPerSecond());
    return g;
}

std::string Workspace::graphText(const Groom& g) {
    return graphOrDefault(g.scene->session().data().graphJson);
}

std::string Workspace::hierarchyText(const Groom& g) { return g.scene->session().data().hierarchyJson; }

std::optional<double> Workspace::fitSize(const Groom& g) {
    const GroomSession& s = g.scene->session();
    const MeshPtr& scalp = s.rawScalp();
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size && s.groomScale() > 0.0) *size /= s.groomScale();
    return size;
}

void Workspace::edit(Groom& g, const std::string& label, const std::function<void(std::string&, std::string&)>& change) {
    const std::string oldGraph = graphText(g), oldHierarchy = hierarchyText(g);
    std::string graph = oldGraph, hierarchy = oldHierarchy;
    change(graph, hierarchy);
    if (graph == oldGraph && hierarchy == oldHierarchy) return;
    GroomSession& s = g.scene->session();
    if (graph != oldGraph) {
        try {
            s.setGraphJson(graph);
        } catch (const std::exception& e) {
            throw ToolError(std::string("the graph would not load after this edit, so nothing changed: ") + e.what());
        }
    }
    if (hierarchy != oldHierarchy) s.setHierarchyJson(hierarchy);
    g.undo.push_back({label, oldGraph, oldHierarchy});
    if (g.undo.size() > kMaxUndo) g.undo.erase(g.undo.begin());
}

std::vector<std::string> Workspace::undo(Groom& g, int steps) {
    std::vector<std::string> labels;
    GroomSession& s = g.scene->session();
    for (int i = 0; i < steps && !g.undo.empty(); ++i) {
        const GroomSnapshot snap = g.undo.back();
        g.undo.pop_back();
        s.setGraphJson(snap.graph);  // it loaded before
        s.setHierarchyJson(snap.hierarchy);
        labels.push_back(snap.label);
    }
    return labels;
}

json Workspace::summary(Groom& g, float density, const std::string& viewNode, bool timings) {
    GroomScene& scene = *g.scene;
    ViewRequest request;
    request.density = density;
    request.viewNode = viewNode;
    request.time = frameToSeconds(g.frame, scene.framesPerSecond());
    request.wantBounds = true;
    const auto t0 = std::chrono::steady_clock::now();
    const ViewResult& view = scene.view(request);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    json out;
    out["strands"] = view.strands ? view.strands->strandCount() : 0;
    out["points"] = view.strands ? view.strands->pointCount() : 0;
    out["guides"] = view.hierarchy ? view.hierarchy->guides().size() : 0;
    if (density < 1.0f) out["density"] = density;
    if (!view.viewedNode.empty()) out["node"] = view.viewedNode;
    if (view.bounds.valid) out["bounds"] = {{"min", vec(view.bounds.lo)}, {"max", vec(view.bounds.hi)}};
    out["errors"] = view.errors;
    json notes = json::object();
    for (const auto& [node, note] : scene.nodeNotes()) notes[node] = note;
    if (!notes.empty()) out["notes"] = notes;
    out["ms"] = static_cast<long long>(ms + 0.5);
    if (timings) {
        auto times = scene.nodeTimes();
        std::sort(times.begin(), times.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
        json slow = json::object();
        for (std::size_t i = 0; i < times.size() && i < 12; ++i) slow[times[i].first] = std::round(times[i].second * 10.0) / 10.0;
        out["nodeMs"] = slow;
    }
    return out;
}

}  // namespace groomlab::cli
