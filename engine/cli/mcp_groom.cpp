// SPDX-License-Identifier: Apache-2.0
#include "mcp_groom.h"

#include <filesystem>
#include <initializer_list>
#include <stdexcept>

#include "cli_export.h"
#include "groomlab/alembic_io.h"
#include "groomlab/frame_time.h"
#include "groomlab/node_library.h"
#include "groomlab/sim_cache.h"
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
    const MeshPtr fileScalp = file.data.scalp;
    file.data = s.data();
    // An animated scalp's frames ride on the file's scalp (its topology and rest): not the frame shown.
    if (!file.scalpCache.empty() && fileScalp) file.data.scalp = fileScalp;
    file.data.scalpHistory = nullptr;
    file.framesPerSecond = s.framesPerSecond();
    file.referencePhotos = referencePhotos;
    file.simCaches = bakes;
    return file;
}

void HeadlessGroom::reopen(const agent::AnimationInputs& inputs) {
    GroomFile file = toFile();
    const ScalpMaps maps = scene_->session().scalpMaps();  // (the session's own, not the file's data)
    if (inputs.scalpCache) file.scalpCache = *inputs.scalpCache;
    if (inputs.guideCache) file.guideCache = *inputs.guideCache;
    std::unique_ptr<GroomScene> scene;
    try {
        scene = std::make_unique<GroomScene>(std::move(file));
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
    if (!maps.maps.empty()) scene->session().setScalpMaps(maps);
    scene_ = std::move(scene);
    dropStrokeHost();
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
    g->referencePhotos = g->scene().file().referencePhotos;
    g->bakes = g->scene().file().simCaches;
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

void HeadlessTarget::setAnimation(agent::AgentGroom& groom, const agent::AnimationInputs& inputs) {
    static_cast<HeadlessGroom&>(groom).reopen(inputs);
}

void HeadlessTarget::writeMeshCache(const std::string& path, const std::string& name, const std::vector<std::shared_ptr<const ScalpMesh>>& frames,
                                    double startSeconds, double secondsPerFrame) {
    if (frames.empty()) throw ToolError("no frames to write");
    try {
        AlembicMeshWriter writer(path, name, startSeconds, secondsPerFrame);
        for (const auto& mesh : frames) writer.writeFrame(*mesh);
        writer.close();
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
}

std::size_t HeadlessTarget::bakeSimulation(agent::AgentGroom& groom, const std::string& node, const std::string& path, int first, int last) {
    HeadlessGroom& g = static_cast<HeadlessGroom&>(groom);
    GroomSession& s = g.session();
    s.setBakedSimulation(node, nullptr);  // (its frames simulated again, not read from an old bake)
    const SimRun run = s.simulateTo(last);
    if (!run.errors.empty()) throw ToolError("simulate: " + run.errors.front());
    std::size_t frames = 0;
    try {
        frames = writeSimulationCache(s, node, path, first, last);
        const std::uint64_t stamp = s.simulationStamp(node);
        s.setBakedSimulation(node, readSimulationCache(path, {}, stamp));
        g.bakes[node] = {path, {}, stamp};
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    } catch (const std::runtime_error& e) {
        throw ToolError(e.what());
    }
    return frames;
}

void HeadlessTarget::loadSimulationCache(agent::AgentGroom& groom, const std::string& node, const std::string& path, const std::string& object) {
    HeadlessGroom& g = static_cast<HeadlessGroom&>(groom);
    GroomSession& s = g.session();
    const std::uint64_t stamp = s.simulationStamp(node);
    try {
        s.setBakedSimulation(node, readSimulationCache(path, object, stamp));
    } catch (const AlembicError& e) {
        throw ToolError(std::string("alembic: ") + e.what());
    }
    g.bakes[node] = {path, object, stamp};
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
