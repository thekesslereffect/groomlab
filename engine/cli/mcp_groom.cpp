// SPDX-License-Identifier: Apache-2.0
#include "mcp_groom.h"

#include <filesystem>
#include <initializer_list>
#include <stdexcept>

#include "cli_export.h"
#include "groomlab/alembic_io.h"
#include "groomlab/frame_time.h"
#include "groomlab/node_library.h"
#include "mesh_files.h"

namespace groomlab::cli {

namespace {

namespace fs = std::filesystem;
using agent::ToolError;

// The first of the folders that exists ("" none).
std::string firstFolder(std::initializer_list<fs::path> candidates) {
    for (const fs::path& p : candidates) {
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec)) return fs::weakly_canonical(p, ec).generic_u8string();
    }
    return {};
}

fs::path sourceFolder(const char* path) { return path ? fs::u8path(path) : fs::path(); }

}  // namespace

// ---- A groom -----------------------------------------------------------------------------------------

HeadlessGroom::HeadlessGroom(std::string name, std::unique_ptr<GroomScene> scene)
    : AgentGroom(std::move(name)), scene_(std::move(scene)) {}

const ViewResult& HeadlessGroom::view(const ViewRequest& request) {
    try {
        return scene_->view(request);
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
}

GroomFile HeadlessGroom::toFile() {
    const GroomSession& s = scene_->session();
    GroomFile file = scene_->file();
    file.data = s.data();
    file.data.scalpHistory = nullptr;
    file.framesPerSecond = s.framesPerSecond();
    return file;
}

// ---- The target --------------------------------------------------------------------------------------

HeadlessTarget::HeadlessTarget(std::string root, const std::string& exeDir) {
    folders_.root = std::move(root);
    const fs::path exe = fs::u8path(exeDir);
    // Installed: the Maya module's GroomLab/bin beside GroomLab/presets (and the Linux build's the
    // same); a build tree: the sources'.
#ifdef GROOMLAB_SHIPPED_PRESETS
    folders_.presetDir = firstFolder({exe / ".." / "presets", exe / "presets", sourceFolder(GROOMLAB_SHIPPED_PRESETS)});
#else
    folders_.presetDir = firstFolder({exe / ".." / "presets", exe / "presets"});
#endif
#ifdef GROOMLAB_SHIPPED_NODES
    setShippedNodeDir(firstFolder({exe / ".." / "nodes", exe / "nodes", sourceFolder(GROOMLAB_SHIPPED_NODES)}));
#else
    setShippedNodeDir(firstFolder({exe / ".." / "nodes", exe / "nodes"}));
#endif
#ifdef GROOMLAB_SHIPPED_MESHES
    folders_.meshDir = firstFolder({exe / ".." / "meshes", exe / "meshes", sourceFolder(GROOMLAB_SHIPPED_MESHES)});
#else
    folders_.meshDir = firstFolder({exe / ".." / "meshes", exe / "meshes"});
#endif
#ifdef GROOMLAB_DOCS_DIR
    folders_.docsDir = firstFolder({exe / ".." / "docs" / "pages", sourceFolder(GROOMLAB_DOCS_DIR)});
#else
    folders_.docsDir = firstFolder({exe / ".." / "docs" / "pages"});
#endif
}

std::vector<agent::AgentGroom*> HeadlessTarget::grooms() {
    std::vector<agent::AgentGroom*> out;
    for (auto& [name, g] : grooms_) out.push_back(g.get());
    return out;
}

agent::AgentGroom& HeadlessTarget::open(const std::string& name, GroomFile file) {
    std::unique_ptr<GroomScene> scene;
    try {
        scene = std::make_unique<GroomScene>(std::move(file));
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
    auto g = std::make_unique<HeadlessGroom>(name, std::move(scene));
    // The groom's own first frame when its caches animate.
    double start = 0.0;
    if (g->scene().timeRange(&start, nullptr)) g->frame = secondsToFrame(start, g->scene().framesPerSecond());
    HeadlessGroom& made = *g;
    grooms_[name] = std::move(g);
    return made;
}

void HeadlessTarget::close(const std::string& name) { grooms_.erase(name); }

std::shared_ptr<const ScalpMesh> HeadlessTarget::readMesh(const std::string& path, const std::string& object) {
    try {
        return readMeshFile(path, object);
    } catch (const MeshFileError& e) {
        throw ToolError(e.what());
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
}

agent::ExportReport HeadlessTarget::exportCurves(agent::AgentGroom& groom, const agent::ExportRequest& request) {
    ExportOptions o;
    o.out = request.path;
    o.start = request.start;
    o.end = request.end;
    o.step = request.step;
    o.density = request.density;
    o.scales = request.scales;
    ExportReport report;
    try {
        report = exportStrands(static_cast<HeadlessGroom&>(groom).scene(), o, nullptr);
    } catch (const std::invalid_argument& e) {
        throw ToolError(e.what());
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
    agent::ExportReport out;
    out.frames = report.frames;
    out.strandTotal = report.strandTotal;
    out.milliseconds = report.milliseconds;
    out.format = isVrscenePath(o.out) ? "vrscene" : isUsdPath(o.out) ? "usd" : "alembic";
    return out;
}

}  // namespace groomlab::cli
