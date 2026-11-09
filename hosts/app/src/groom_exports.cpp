// SPDX-License-Identifier: Apache-2.0
// GroomDocument's exports and card bake (groom_document.h): what the Maya host's export
// commands and bake do (hosts/maya/src/export_cmds.cpp, groom_shape.cpp), on the session the app
// keeps. Every export evaluates the groom frame by frame through the scene's view (the cached
// scalp, its history and the guide poses of each time, followScalp included), so an export sees
// exactly what the viewport shows at that frame, at the export's density.

#include "groom_document.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "groomlab/alembic_io.h"
#include "groomlab/engine_cards.h"
#include "groomlab/engine_meshes.h"
#include "groomlab/fbx_writer.h"
#include "groomlab/scales.h"
#include "groomlab/asset_files.h"
#include "groomlab/sim_cache.h"
#include "groomlab/simulation.h"
#include "groomlab/card_presets.h"
#include "groomlab/engine_export.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph_edit.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/png.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"

namespace groomlab::app {

namespace fs = std::filesystem;
using nlohmann::json;

std::size_t FrameRange::count() const {
    if (!(step > 0.0) || last < first) return 1;
    return static_cast<std::size_t>(std::floor((last - first) / step + 1e-6)) + 1;
}

namespace {

// The primary guides as curves (what Unreal simulates): the hierarchy's level 0.
StrandSet toCurves(const GuideSet& guides) {
    StrandSet out;
    for (const Guide& g : guides.guides()) {
        if (g.points.size() >= 2) out.addStrand(g.id, g.points, {}, g.root);
    }
    return out;
}

// The export's name: the file's stem (what Maya names the root after the groom's transform).
std::string exportName(const std::string& path) {
    const std::string stem = fs::u8path(path).stem().u8string();
    std::string name;
    for (const char c : stem) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        name += plain ? c : '_';
    }
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) name = "groom" + name;
    return name;
}

bool continueExport(const ExportProgress& progress, std::size_t done, std::size_t count) {
    return !progress || progress(done, count);
}

}  // namespace

StrandsPtr GroomDocument::exportFrame(double seconds, float density, MeshPtr* scalp, bool buildCards, bool gameExport) {
    ViewRequest request;
    request.density = renderOverrides_.densityTimes(density);  // (the render overrides' share too)
    request.time = seconds;
    request.buildCards = buildCards;  // the Cards nodes' cards on the strands (their card layers)
    request.wantBounds = false;
    request.gameExport = gameExport;  // (no Output rootDepth for an engine)
    const ViewResult& view = scene_->view(request);
    if (!view.errors.empty()) throw std::runtime_error(view.errors.front());
    if (!view.strands) throw std::runtime_error("the groom gave no strands");
    if (scalp) *scalp = view.scalp ? view.scalp : scene_->rootScalpAt(seconds);
    return applyRenderOverrides(view.strands, renderOverrides_);  // the widths and lengths a render scales
}

ExportResult GroomDocument::exportAlembic(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    const FrameRange& range = options.frames;
    const double fps = framesPerSecond();
    ExportResult result;
    try {
        AlembicCurvesWriter writer(path, exportName(path), frameToSeconds(range.first, fps), frameToSeconds(range.step, fps));
        const std::size_t count = range.count();
        for (std::size_t i = 0; i < count; ++i) {
            MeshPtr scalp;
            const StrandsPtr strands = exportFrame(frameToSeconds(range.frame(i), fps), options.density, &scalp);
            writer.writeFrame(*strands, scalp.get());
            result.strands = strands->strandCount();
            result.frames = i + 1;
            if (!continueExport(progress, i + 1, count)) {
                result.cancelled = true;
                break;
            }
        }
        writer.close();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    result.files = {path};
    return result;
}

ExportResult GroomDocument::exportGuides(const std::string& path, const std::string& group, const ExportOptions& options,
                                         const ExportProgress& progress) {
    const FrameRange& range = options.frames;
    const double fps = framesPerSecond();
    ExportResult result;
    try {
        AlembicCurvesWriter writer(path, exportName(path) + "Guides", frameToSeconds(range.first, fps), frameToSeconds(range.step, fps));
        const std::size_t count = range.count();
        for (std::size_t i = 0; i < count; ++i) {
            ViewRequest request;
            request.time = frameToSeconds(range.frame(i), fps);
            request.wantBounds = false;
            const ViewResult& view = scene_->view(request);  // (the guides at that frame: the scalp, the poses)
            const StrandSet curves = toCurves(guidesToSimulate(view.hierarchy, session().data().guides, group));
            writer.writeFrame(curves);
            result.strands = curves.strandCount();
            result.frames = i + 1;
            if (!continueExport(progress, i + 1, count)) {
                result.cancelled = true;
                break;
            }
        }
        writer.close();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    result.files = {path};
    return result;
}

ExportResult GroomDocument::exportUsd(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    const FrameRange& range = options.frames;
    const double fps = framesPerSecond();
    ExportResult result;
    try {
        UsdCurvesWriter::Options usd;
        usd.framesPerSecond = fps;
        usd.metersPerUnit = usdMetersPerUnit(file().units);
        usd.upAxis = "Y";  // the app is Y-up (a Z-up file turned on load)
        usd.comment = path_.empty() ? std::string("GroomLab") : path_;
        // The scales and the Instance nodes' prototypes as every other host writes them: the
        // file's stored meshes (the built-in plates need none).
        const GroomFile& groomFile = file();
        const PrototypeLookup prototypes = [&groomFile](const std::string& name) -> const InstancePrototype* {
            const auto it = groomFile.prototypes.find(name);
            return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
        };
        usd.prototypes = prototypes;
        usd.scales = options.usdScaleInstancer ? UsdCurvesWriter::Options::Scales::Instancer : UsdCurvesWriter::Options::Scales::Mesh;
        ScaleMeshBuilder scales;  // bends only the scales that moved since the last frame
        UsdCurvesWriter writer(path, exportName(path), usd);
        const std::size_t count = range.count();
        for (std::size_t i = 0; i < count; ++i) {
            MeshPtr scalp;
            const double frame = range.frame(i);
            const StrandsPtr strands = exportFrame(frameToSeconds(frame, fps), options.density, &scalp);
            const bool scaled = strands->instances() && strands->instances()->hasMeshes();
            const auto scaleMesh = scaled ? scales.build(*strands, scalp, prototypes) : nullptr;
            writer.writeFrame(frame, *strands, scalp.get(), scaleMesh.get());
            result.strands = strands->strandCount();
            result.frames = i + 1;
            if (!continueExport(progress, i + 1, count)) {
                result.cancelled = true;
                break;
            }
        }
        writer.close();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    result.files = {path};
    return result;
}

ExportResult GroomDocument::exportVrscene(const std::string& path, const ExportOptions& options, const ExportProgress& progress,
                                          MeshPtr* firstScalp) {
    const FrameRange& range = options.frames;
    const double fps = framesPerSecond();
    ExportResult result;
    try {
        VrsceneWriter::Options o;
        o.framesPerSecond = fps;
        o.metersPerUnit = usdMetersPerUnit(file().units);
        o.upAxis = "Y";  // the app is Y-up (a Z-up file turned on load)
        o.comment = path_.empty() ? std::string("GroomLab") : path_;
        VrsceneWriter writer(path, exportName(path), o);
        const std::size_t count = range.count();
        for (std::size_t i = 0; i < count; ++i) {
            MeshPtr scalp;
            const double frame = range.frame(i);
            StrandsPtr strands = exportFrame(frameToSeconds(frame, fps), options.density, &scalp);
            if (options.lod && strands && strands->strandCount() > 0) {
                // Level of detail through the render's camera: far strands thinned, culled ones dropped.
                const LodPlan plan = planLod(*strands, options.lodCamera, *options.lod, scalp.get());
                strands = std::make_shared<StrandSet>(applyLod(*strands, plan));
            }
            if (i == 0) writer.setLobes(hairLobes(scene_->session().hairLook()));  // as evaluated at the first frame
            if (i == 0 && firstScalp) *firstScalp = scalp;
            writer.writeFrame(frame, *strands, scalp.get());
            result.strands = strands->strandCount();
            result.frames = i + 1;
            if (!continueExport(progress, i + 1, count)) {
                result.cancelled = true;
                break;
            }
        }
        writer.close();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    result.files = {path};
    return result;
}

namespace {

// The app's export options as every host's engine writer takes them (engine_export.h).
EngineExportOptions engineOptions(const ExportOptions& options, EngineExportOptions::Engine engine, const std::string& units) {
    EngineExportOptions e;
    e.engine = engine;
    e.groups = options.groups ? std::string(kGroupIdChannel) : std::string();
    e.shafts = options.shafts;
    e.featherGuides = options.featherGuides;
    e.iridescenceAo = options.iridescenceAo;
    e.barbDensity = options.barbDensity;
    e.barbWidthScale = options.barbWidthScale;
    // Unreal: written in its space from the app's (Y up, a Z-up file turned on load; the file's
    // unit), so the importer's defaults line it up with an FBX of the same scene.
    e.upAxis = "Y";
    e.centimetersPerUnit = usdMetersPerUnit(units) * 100.0;
    if (engine == EngineExportOptions::Engine::Unity) {
        e.scale = options.scale;
        e.pointsPerStrand = static_cast<std::size_t>(std::max(0, options.pointsPerStrand));
        e.barbPoints = static_cast<std::size_t>(std::max(0, options.barbPoints));
    }
    return e;
}

}  // namespace

ExportResult GroomDocument::exportEngineGroom(const std::string& path, const ExportOptions& options, bool unity,
                                              const ExportProgress& progress) {
    const FrameRange& range = options.frames;
    const double fps = framesPerSecond();
    ExportResult result;
    const EngineExportOptions engine =
        engineOptions(options, unity ? EngineExportOptions::Engine::Unity : EngineExportOptions::Engine::Unreal, file().units);
    if (options.pointsPerStrand < 0 || options.barbPoints < 0) throw std::runtime_error("points per strand must be 0 or 2 to 4096");
    const std::string problem = engine.problem();
    if (!problem.empty()) throw std::runtime_error(problem);
    try {
        EngineGroomWriter writer(path, exportName(path), frameToSeconds(range.first, fps), frameToSeconds(range.step, fps), engine);
        const std::size_t count = range.count();
        for (std::size_t i = 0; i < count; ++i) {
            MeshPtr scalp;
            const StrandsPtr evaluated = exportFrame(frameToSeconds(range.frame(i), fps), options.density, &scalp, false, true);
            // The hair simulates on the primary guides (the hierarchy's level 0 at this time).
            StrandSet primaries;
            if (!unity && options.guides) {
                if (const HierarchyPtr h = session().hierarchy()) primaries = toCurves(h->flatten(0));
            }
            writer.writeFrame(*evaluated, unity ? nullptr : &primaries, scalp.get());
            result.strands = evaluated->strandCount();
            result.frames = i + 1;
            if (!continueExport(progress, i + 1, count)) {
                result.cancelled = true;
                break;
            }
        }
        writer.close();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    result.files = {path};
    return result;
}

ExportResult GroomDocument::exportUnrealGroom(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    return exportEngineGroom(path, options, false, progress);
}

ExportResult GroomDocument::exportUnityStrands(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    return exportEngineGroom(path, options, true, progress);
}
// ---- Cards -----------------------------------------------------------------------------------------

std::vector<std::string> GroomDocument::cardsNodes() const {
    std::vector<std::string> names;
    for (const NodeInfo& node : nodes()) {
        if (node.type == "Cards") names.push_back(node.name);
    }
    return names;
}

std::string GroomDocument::addCardsNode(std::string* made) {
    // Between what feeds the output and the output: the cards are made from the whole groom.
    const std::string output = graph_.value("output", std::string());
    std::string from, fromOutput;
    for (const ConnectionInfo& c : connections()) {
        if (c.to == output && c.input == "strands") {
            from = c.from;
            fromOutput = c.output;
        }
    }
    float x = 0.0f, y = 0.0f;
    if (!nodePosition(output, &x, &y)) {
        x = 0.0f;
        y = 0.0f;
    }
    std::string name;
    std::string error = addNode("Cards", x - 220.0f, y, &name);
    if (!error.empty()) return error;
    if (made) *made = name;
    if (from.empty() || output.empty()) return {};
    // One undo step for the whole insertion: the add, then the two wires, folded together.
    error = editGraph("add " + name, [&](json& g) {
        GraphEdit edit(g.dump());  // the core's connections (graph_edit.h): checked, what fed the output replaced
        edit.connect(from, name, "strands", fromOutput);
        edit.connect(name, output, "strands");
        g = json::parse(edit.toJson());
    });
    if (error.empty() && edits_.size() >= 2) {
        // The add and its wiring as one step (undo removes the node with its wires).
        Edit wired = std::move(edits_.back());
        edits_.pop_back();
        edits_.back().graphAfter = wired.graphAfter;
        undoIndex_ = edits_.size();
    }
    return error;
}

std::string GroomDocument::applyCardPreset(const std::string& node, const std::string& lod) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    std::vector<std::pair<std::string, ParamValue>> params;
    try {
        params = cardPresetParams(lod, cardNodeKind(graph_.dump(), node));
    } catch (const std::exception& e) {
        return e.what();
    }
    return editGraph(lod + " cards on " + node, [&](json& g) {
        for (json& n : g["nodes"]) {
            if (n.value("name", std::string()) != node) continue;
            for (const auto& [name, value] : params) {
                std::visit(
                    [&](const auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                            n["params"][name] = v;
                        }
                    },
                    value);
            }
        }
    });
}

std::string GroomDocument::applySimulatePreset(const std::string& node, const std::string& preset) {
    const json* spec = nodeJson(node);
    if (!spec) return "no node '" + node + "'";
    if (spec->value("type", std::string()) != "Simulate") return "'" + node + "' is not a Simulate node";
    json params;
    try {
        params = json::parse(simulatePresetParamsJson(preset));
    } catch (const std::exception& e) {
        return e.what();
    }
    std::string title = preset;
    for (const SimPreset& p : simulatePresets()) {
        if (p.name == preset) title = p.title;
    }
    return editGraph(title + " settings on " + node, [&](json& g) {
        for (json& n : g["nodes"]) {
            if (n.value("name", std::string()) != node) continue;
            for (const auto& [name, value] : params.items()) n["params"][name] = value;
        }
    });
}

CardBake GroomDocument::bakeCardsInMemory(int resolution, float occlusion) {
    return bakeCardsInMemoryWith([&](StrandsPtr strands, MeshPtr scalp) {
        return groomlab::bakeCardsInMemory(std::move(strands), std::move(scalp), resolution, occlusion);
    });
}

CardBake GroomDocument::bakeCardsInMemoryWith(const std::function<CardBake(StrandsPtr, MeshPtr)>& bake) {
    if (cardsNodes().empty()) throw std::runtime_error("no cards: add a Cards node to the graph");
    try {
        // The Cards nodes building their cards at full density, on the session's own card
        // document (the view's cache stays), with the inputs of the document's time; the bake
        // as every host makes it (card_files.h).
        ViewRequest request;
        request.density = 1.0f;
        request.time = time_;
        request.wantCards = true;
        request.cardDensity = 1.0f;
        request.wantBounds = false;
        request.gameExport = true;  // cards: a game asset (no Output rootDepth)
        const ViewResult& view = scene_->view(request);
        if (!view.errors.empty()) throw std::runtime_error(view.errors.front());
        return bake(view.cardStrands, view.scalp ? view.scalp : scene_->rootScalpAt(time_));
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}

namespace {

// The record of a bake at `at`: its files (the maps, the atlas, the preview), its size.
CardBakeFiles bakeFilesAt(const CardBakePaths& at, const std::string& stamp, int size) {
    CardBakeFiles files;
    files.prefix = at.prefix;
    files.paths = at.files();
    files.atlas = at.atlas();
    files.preview = at.preview();
    files.paths.push_back(files.preview);
    files.stamp = stamp;
    files.size = size;
    return files;
}

}  // namespace

CardBakeFiles GroomDocument::bakeCards(const std::string& directory, const std::string& name, int resolution, float occlusion) {
    const CardBake bake = bakeCardsInMemory(resolution, occlusion);
    // Never over another groom's bake (Maya's _free_bake_name): this groom's own is rewritten, a
    // bake there that is not its own keeps its files and this one takes name_2, name_3...
    CardBakePaths at = CardBakePaths::in(directory, name);
    for (int k = 2; at.exists() && !(bake_ && bake_->prefix == at.prefix); ++k) at = CardBakePaths::in(directory, name + "_" + std::to_string(k));
    CardBakeFiles files;
    try {
        // Unreal's hair card maps, the atlas and the viewport's map (a PNG the renderer reads as
        // an albedo), as the Maya host writes them.
        files = bakeFilesAt(at, writeCardBake(bake, at, true), bake.textures.size);
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    files.cards = bake.cards->cards.size();
    files.triangles = bake.cards->triangleCount();
    files.textures = bake.cards->textureCount();
    // The session keeps the bake: its atlas gives rebuilt cards their cells, its stamp says
    // which Cards nodes changed since (a new Bake number each time: the maps were rewritten).
    static int bakes = 0;
    session().setCardMaps(formatCardMaps(at.prefix, files.stamp, ++bakes));
    bake_ = files;
    return files;
}

// ---- Simulation ------------------------------------------------------------------------------------

SimRun GroomDocument::simulateTo(double lastFrame, const SimProgress& progress) {
    return session().simulateTo(static_cast<int>(std::lround(lastFrame)), progress);
}

void GroomDocument::invalidateSimulation() {
    for (const auto& [node, ref] : simBakes_) session().setBakedSimulation(node, nullptr);
    if (!simBakes_.empty()) dirty_ = true;
    simBakes_.clear();
    session().invalidateSimulation();
}

std::string GroomDocument::simulationCachePath(const std::string& node) const {
    // Maya's sim.py: the groom's own folder in caches, <caches>/<groom>/<groom>_<node>_sim.abc (an
    // untitled groom's caches: a temp folder of its own, taken along when it is saved).
    const std::string groom = fileSafeName(defaultBakeName());
    return (fs::u8path(assetFolder("caches")) / fs::u8path(groom) / fs::u8path(groom + "_" + node + "_sim.abc")).generic_u8string();
}

std::vector<std::string> GroomDocument::bakeSimulation(double lastFrame, const SimProgress& progress) {
    const std::vector<std::string> nodes = simulateNodes();
    if (nodes.empty()) throw std::runtime_error("the graph has no Simulate node to bake");
    const int last = static_cast<int>(std::lround(lastFrame));
    // The bakes the nodes read now let go first: their files are written again (and a node with a
    // bake has no frames of its own to write until it simulates).
    for (const std::string& node : nodes) session().setBakedSimulation(node, nullptr);
    const SimRun run = session().simulateTo(last, progress);
    if (!run.errors.empty()) throw std::runtime_error(run.errors.front());
    if (run.cancelled) throw std::runtime_error("the simulation stopped before frame " + std::to_string(last) + ": nothing baked");
    std::map<std::string, int> starts;
    for (const SimStatus& s : session().simulationStatus()) starts[s.node] = s.startFrame;
    std::vector<std::string> files;
    try {
        for (const std::string& node : nodes) {
            const std::string path = simulationCachePath(node);
            fs::create_directories(fs::u8path(path).parent_path());
            writeSimulationCache(session(), node, path, starts[node], last);
            const std::uint64_t stamp = session().simulationStamp(node);
            session().setBakedSimulation(node, readSimulationCache(path, {}, stamp));
            simBakes_[node] = {path, {}, stamp};
            files.push_back(path);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    dirty_ = true;
    return files;
}

std::string GroomDocument::defaultBakeDirectory() const {
    // The groom's own folder in cards, as Maya keeps a groom's: groomlab/<name>/cards/<name>
    // (untitled: its own temp folder).
    return assetFolder("cards") + "/" + fileSafeName(defaultBakeName());
}

std::string GroomDocument::defaultBakeName() const { return displayName(); }

void GroomDocument::adoptBake() {
    // The bake the file names (its "cardBake": a groom saved by this app or exported from Maya),
    // else one written beside the groom earlier (bakeCards at the default place): the session
    // takes its atlas and stamp, and the viewport can show its preview map (made from the maps
    // when the bake has none: another host's).
    const CardBakeRef& named = scene_->file().cardBake;
    CardBakePaths at;
    std::string stamp;
    if (!named.empty()) {
        at.prefix = named.prefix;
        std::replace(at.prefix.begin(), at.prefix.end(), '\\', '/');
        stamp = named.strands;
    } else if (!path_.empty()) {
        // The default place (groomlab/<name>/cards/<name>), else where earlier versions baked.
        at = CardBakePaths::foundBesideGroomFile(path_);
    } else {
        return;
    }
    std::error_code ec;
    if (!at.exists()) return;
    if (!fs::exists(fs::u8path(at.preview()), ec)) {
        try {
            writeCardBakePreview(at.prefix);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "GroomLab: the card bake %s has no preview and none could be made: %s\n", at.prefix.c_str(), e.what());
            return;
        }
    }
    // The size from the depth map's TGA header (width at bytes 12-13).
    int size = 0;
    if (std::ifstream in{fs::u8path(at.map("Depth")), std::ios::binary}) {
        unsigned char header[18] = {};
        in.read(reinterpret_cast<char*>(header), 18);
        if (in.gcount() == 18) size = header[12] | (header[13] << 8);
    }
    // The stamp is the file's; a bake found beside the groom has none: every node then reads as
    // stale until the next bake, which is the honest answer (the maps may be from another graph).
    const CardBakeFiles files = bakeFilesAt(at, stamp, size);
    session().setCardMaps(formatCardMaps(at.prefix, stamp, 0));
    bake_ = files;
}

bool GroomDocument::bakeStale() const {
    if (!bake_) return false;
    return scene_->session().stats().cardsStale;
}

ExportResult GroomDocument::exportCards(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    ExportResult result;
    const CardBake bake = bakeCardsInMemory(options.resolution, options.occlusion);
    if (!continueExport(progress, 1, 2)) {
        result.cancelled = true;
        return result;
    }
    try {
        const std::string name = exportName(path);
        GltfDocument doc;
        doc.rootName = name;
        const HairLook look = scene_->session().hairLook();
        CardGltfSettings settings;
        settings.name = name + "_cards";
        settings.scale = file().units == "m" ? 1.0f : file().units == "in" ? 0.0254f : file().units == "mm" ? 0.001f : 0.01f;
        settings.roughness = look.roughness;
        settings.specular = look.specular;
        addCardsToGltf(doc, *bake.cards, bake.textures, settings);
        // The hair cap when the scalp has UVs: the groom baked onto the skin, so low LODs show
        // hair color between cards (as the Maya host's export writes it: gltf_writer.h).
        if (bake.scalp && bake.scalp->hasUVs()) {
            CardGltfSettings cap = settings;
            cap.name = name + "_scalpCap";
            addScalpCapToGltf(doc, *bake.strands, *bake.scalp, 0, options.resolution, cap);
        }
        result.files = writeGltf(path, doc, options.binary);
        result.frames = 1;
        result.strands = bake.cards->cards.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    continueExport(progress, 2, 2);
    return result;
}

std::vector<std::string> GroomDocument::exportEngineCards(const std::string& directory, const std::string& name, bool unity, int resolution,
                                                          float occlusion, std::vector<std::string>* warnings) {
    resolution = resolution > 0 ? std::clamp(resolution, 16, 16384) : 0;
    const CardBake bake = bakeCardsInMemory(resolution, occlusion);
    const GroomStats stats = session().stats();  // the cards' counts, as Maya's card_stats has them
    const HairLook look = session().hairLook();
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    const std::string dir = fs::u8path(directory).generic_u8string();
    std::vector<std::string> maps;
    if (unity) {
        maps = writeUnityCardMaps(bake.textures, look.roughness, dir, name);
    } else {
        const CardBakePaths at = CardBakePaths::in(dir, name);
        writeCardBake(bake, at, false);
        for (const std::string& f : at.files()) {
            if (fs::u8path(f).extension() == ".tga") maps.push_back(f);  // the maps; the atlas record is the groom's own
        }
    }
    // The hair cap's maps: the groom baked onto its scalp (at full density), when the scalp has uvs.
    bool withScalp = bake.scalp && bake.scalp->hasUVs() && bake.strands;
    if (withScalp) {
        try {
            ScalpBakeSettings settings;
            settings.size = resolution > 0 ? resolution : 2048;
            const ScalpTextures t = bakeScalpTextures(*bake.strands, *bake.scalp, settings);
            for (const std::string& p : writeScalpCapMaps(t, unity, dir, name)) maps.push_back(p);
        } catch (const std::exception& e) {
            if (warnings) warnings->push_back(std::string("no scalp maps (") + e.what() + ")");
            withScalp = false;
        }
    } else if (warnings) {
        warnings->push_back("no hair cap: the scalp has no uvs");
    }
    const std::string ext = unity ? ".png" : ".tga";
    std::map<std::string, std::string> byMap;
    for (const std::string& p : maps) {
        const std::string file = fs::u8path(p).filename().u8string();
        const std::string prefix = name + "_";
        if (file.size() > prefix.size() + ext.size() && file.compare(0, prefix.size(), prefix) == 0) {
            byMap[file.substr(prefix.size(), file.size() - prefix.size() - ext.size())] = file;
        }
    }
    const std::string fbx = dir + "/" + name + "_cards.fbx";
    const std::string capFbx = withScalp ? dir + "/" + name + "_scalpCap.fbx" : std::string();
    EngineCardsInfo info;
    info.mesh = name + "_cards.fbx";
    info.scalpCap = withScalp ? name + "_scalpCap.fbx" : std::string();
    info.cards = stats.cards;
    info.triangles = stats.cardTriangles;
    info.textures = stats.cardTextures;
    info.feathers = stats.cardFeathers;
    info.maps = byMap;
    info.roughness = look.roughness;
    const std::string manifest = dir + "/" + name + (unity ? "_unity.json" : "_cards.json");
    {
        std::ofstream f(fs::u8path(manifest), std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + manifest);
        f << engineCardsManifestJson(unity, info);
    }
    // The meshes, as Maya's fbxmaya and Blender's FBX exporter write them (Unity's in meters, with
    // a material on its base color map; Unreal's in centimeters, the materials made in the engine).
    FbxSettings settings;
    settings.meters = unity;
    settings.creator = "GroomLab";
    FbxMesh cards = cardsFbxMesh(*bake.cards, name + "_cards");
    if (unity) {
        cards.material = name + "_cards_mat";
        if (byMap.count("BaseColor")) cards.baseColorMap = dir + "/" + byMap["BaseColor"];
    }
    writeFbx(fbx, {cards}, settings);
    std::vector<std::string> out = maps;
    out.push_back(manifest);
    out.push_back(fbx);
    if (withScalp) {
        FbxMesh cap = scalpCapFbxMesh(*bake.scalp, name + "_scalpCap");
        if (unity) {
            cap.material = name + "_scalpCap_mat";
            if (byMap.count("ScalpBaseColor")) cap.baseColorMap = dir + "/" + byMap["ScalpBaseColor"];
        }
        writeFbx(capFbx, {cap}, settings);
        out.push_back(capFbx);
    }
    return out;
}

std::vector<std::string> GroomDocument::exportUnityFeathers(const std::string& directory, const std::string& name,
                                                            std::vector<std::string>* warnings) {
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    const std::string dir = fs::u8path(directory).generic_u8string();
    EngineExportOptions e;
    e.engine = EngineExportOptions::Engine::Unity;
    e.groups = kGroupIdChannel;
    e.pointsPerStrand = 16;
    e.barbPoints = 6;
    e.maxStrandsPerGroup = kUnityMaxStrandsPerGroup;
    const std::string abc = dir + "/" + name + "_strands.abc";
    MeshPtr scalp;
    const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp, false, true);
    nlohmann::json objects = nlohmann::json::array();
    {
        EngineGroomWriter writer(abc, name, time_, frameToSeconds(1.0, framesPerSecond()), e);
        writer.writeFrame(*strands, nullptr, scalp.get());
        writer.close();
        for (const EngineGroomWriter::Object& o : writer.objects()) {
            objects.push_back({{"object", o.path}, {"group", o.group}, {"groupId", o.groupId}, {"feathers", o.feathers},
                               {"strands", o.strands}, {"pointsPerStrand", o.pointsPerStrand ? nlohmann::json(o.pointsPerStrand) : nlohmann::json()},
                               {"minPoints", o.minPoints}, {"maxPoints", o.maxPoints}});
        }
    }
    std::vector<std::string> out{abc};
    std::string fbx;
    if (hasRachis(strands->featherLayers())) {
        fbx = dir + "/" + name + "_rachis.fbx";
        FbxSettings settings;
        settings.meters = true;  // (Unity's)
        writeFbx(fbx, {rachisFbxMesh(buildRachisMesh(strands->featherLayers()), name + "_rachis")}, settings);
        out.push_back(fbx);
    } else if (warnings && std::any_of(objects.begin(), objects.end(), [](const nlohmann::json& o) { return o.value("feathers", false); })) {
        warnings->push_back("the feathers have no rachis: their shafts go as strands, and there is no mesh for the barbs to ride");
    }
    const std::string manifest = dir + "/" + name + "_unity_feathers.json";
    {
        std::ofstream f(fs::u8path(manifest), std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + manifest);
        f << unityFeathersManifestJson(objects.dump(), name + "_strands.abc", fbx.empty() ? std::string() : name + "_rachis.fbx", "", 1.0);
    }
    out.push_back(manifest);
    return out;
}

std::vector<std::string> GroomDocument::exportScalesFbx(const std::string& path, bool unity) {
    MeshPtr scalp;
    const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp, false, true);
    const GroomFile& groomFile = file();
    const PrototypeLookup prototypes = [&groomFile](const std::string& n) -> const InstancePrototype* {
        const auto it = groomFile.prototypes.find(n);
        return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
    };
    ScaleMeshBuilder builder;
    const auto mesh = builder.build(*strands, scalp, prototypes);
    if (!mesh || mesh->triangles.empty()) throw std::runtime_error("the groom has no scales (a Scales node)");
    FbxSettings settings;
    settings.meters = unity;
    writeFbx(path, {scalesFbxMesh(*mesh, fs::u8path(path).stem().u8string())}, settings);
    return {path};
}

ExportResult GroomDocument::exportCardMaps(const std::string& path, const ExportOptions& options, const ExportProgress& progress) {
    ExportResult result;
    const CardBake bake = bakeCardsInMemoryWith([&](StrandsPtr strands, MeshPtr scalp) {
        return groomlab::bakeCardMapsInMemory(std::move(strands), std::move(scalp), options.mapSettings, options.occlusion);
    });
    if (!continueExport(progress, 1, 2)) {
        result.cancelled = true;
        return result;
    }
    try {
        // <folder>/<name>: the name without an extension a picked file may have.
        const std::filesystem::path out = std::filesystem::u8path(path);
        const std::string name = out.has_extension() ? out.stem().u8string() : out.filename().u8string();
        result.files = writeCardMaps(*bake.maps, options.mapPreset, options.mapSettings, out.parent_path().generic_u8string(),
                                     name.empty() ? exportName(path) : name);
        result.frames = 1;
        result.strands = bake.cards->cards.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    continueExport(progress, 2, 2);
    return result;
}

}  // namespace groomlab::app
