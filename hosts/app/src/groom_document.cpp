// SPDX-License-Identifier: Apache-2.0
#include "groom_document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <variant>

#include "groomlab/alembic_io.h"
#include "groomlab/attributes.h"
#include "groomlab/flow.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_io.h"
#include "groomlab/node_library.h"
#include "groomlab/parts.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/random.h"
#include "groomlab/scales.h"
#include "groomlab/operators.h"
#include "groomlab/sculpt.h"
#include "groomlab/subdivision.h"
#include "groomlab/texture.h"
#include "groomlab/transfer.h"
#include "graph_presets.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

// Whether an image is a color one (any texel not gray, or not opaque): a texture map's channels.
int channelsOfImage(const Texture::Image& image) {
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        if (image.rgba[i] != image.rgba[i + 1] || image.rgba[i + 1] != image.rgba[i + 2] || image.rgba[i + 3] != 1.0f) return kColorMap;
    }
    return kGrayMap;
}


std::string forwardSlashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

}  // namespace

std::unique_ptr<GroomDocument> GroomDocument::open(const std::string& path) {
    try {
        GroomFile file = readGroomFile(path);
        if (!file.data.scalp) throw std::runtime_error("'" + path + "' has no scalp: make a new groom on a mesh instead");
        convertUpAxis(file, "Y");  // the app is Y-up, as Maya: a Z-up file (Blender's) turns a quarter about X
        return std::unique_ptr<GroomDocument>(new GroomDocument(std::move(file), path));
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}

std::unique_ptr<GroomDocument> GroomDocument::newOnMesh(MeshPtr scalp, const std::string& meshPath) {
    if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the mesh has no triangles");
    GroomFile file;
    file.data.scalp = std::move(scalp);
    file.data.graphJson = "{}";
    file.source = meshPath;
    return std::unique_ptr<GroomDocument>(new GroomDocument(std::move(file), {}));
}

std::unique_ptr<GroomDocument> GroomDocument::newOnAlembic(const std::string& abcPath, const std::string& object) {
    try {
        AlembicMeshReader reader(abcPath, object);
        double start = 0.0;
        const bool animated = reader.timeRange(&start, nullptr);
        // The scalp as the cache's first sample has it (a still cache: its one mesh).
        const MeshPtr scalp = reader.meshAt(animated ? start : 0.0);
        if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the mesh has no triangles");
        GroomFile file;
        file.data.scalp = scalp;
        file.data.graphJson = "{}";
        file.source = abcPath;
        if (animated) {
            // The file follows the cache frame by frame (GroomScene reads the points of each
            // time on this topology); the path as given, absolute or relative to the cwd, which
            // save keeps (a groom file beside the cache would hold the same path).
            file.scalpCache.file = abcPath;
            file.scalpCache.object = object.empty() ? reader.meshName() : object;
        }
        auto document = std::unique_ptr<GroomDocument>(new GroomDocument(std::move(file), {}));
        if (animated) document->time_ = start;
        return document;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
}

GroomDocument::GroomDocument(GroomFile file, std::string path) : path_(std::move(path)) {
    try {
        scene_ = std::make_unique<GroomScene>(std::move(file));
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    // Every operator's spec once: what the panels show for each node type.
    for (const json& spec : json::parse(operatorSpecsJson())) specs_[spec.at("type").get<std::string>()] = spec;
    refreshLibrary();
    // The graph as the core writes it: every node with every param (defaults filled in), so the
    // panels edit values that are there and the file saved holds the whole look.
    loadGraph(scene_->file().data.graphJson);
    loadMaps();
    // The groups' own colors and the hidden groups as the file keeps them (Maya's attributes' text).
    {
        std::istringstream words(scene_->file().guideGroupColors);
        for (std::string word; words >> word;) {
            const std::size_t colon = word.rfind(':');
            if (colon == std::string::npos || colon == 0 || word.size() != colon + 8 || word[colon + 1] != '#') continue;
            std::string color = word.substr(colon + 1);
            std::transform(color.begin(), color.end(), color.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            groupColors_[word.substr(0, colon)] = color;
        }
    }
    hiddenGroups_ = parseHiddenGuideGroups(scene_->file().hiddenGuideGroups);
    // The caches' first frame is where an animated groom opens (Maya opens at the range's start).
    double first = 0.0;
    if (scene_->timeRange(&first, nullptr)) time_ = first;
    adoptBake();
    simBakes_ = scene_->file().simCaches;  // the scene's session reads them already
}

// ---- Painted maps -----------------------------------------------------------------------------

std::string GroomDocument::resolveMapPath(const std::string& stored) const {
    // As every host resolves it (core's resolveGroomPath: variables, then the groom's folder,
    // where it was read from or saved to since, and the file's search paths).
    const std::string folder = path_.empty() ? std::string() : std::filesystem::u8path(path_).parent_path().u8string();
    return forwardSlashes(resolveGroomPath(scene_->file(), stored, folder));
}

void GroomDocument::loadMaps() {
    const GroomFile& file = scene_->file();
    fileCatalog_ = file.data.maps;
    // The vertex maps from the scalp's color sets, the texture maps' files (scalpMapsFromGroomData,
    // as the Maya host imports them). The catalog says whether a map is color (files written since
    // it did); an older file's map is color when its values are (a corner not gray or not opaque,
    // a texel likewise).
    ScalpMaps maps = scalpMapsFromGroomData(file.data);
    for (auto& [name, entry] : maps.maps) {
        const auto it = file.data.maps.find(name);
        if (entry.kind != "texture" || (it != file.data.maps.end() && it->second.channels == kColorMap)) continue;
        try {
            const TexturePtr texture = loadTexture(resolveMapPath(entry.texture));
            if (texture && texture->image()) entry.channels = channelsOfImage(*texture->image());
        } catch (const std::exception&) {
        }
    }
    // Guide layers stay the hierarchy's; nothing to hand the session for them.
    if (!maps.maps.empty()) session().setScalpMaps(std::move(maps));
}

std::vector<PaintedMap> GroomDocument::maps() const {
    maps_.clear();
    const GroomSession& session = scene_->session();
    for (const auto& [name, entry] : session.scalpMaps().maps) {
        maps_.push_back({name, entry.kind, entry.channels == kColorMap ? kColorMap : kGrayMap, entry.texture});
    }
    if (const HierarchyPtr h = scene_->session().authoredHierarchy()) {
        for (const std::string& layer : h->attributeNames()) {
            if (session.scalpMaps().maps.count(layer)) continue;  // a scalp map of the name wins, as the Map node reads it
            maps_.push_back({layer, "guides", kGrayMap, {}});
        }
    }
    std::sort(maps_.begin(), maps_.end(), [](const PaintedMap& a, const PaintedMap& b) { return a.name < b.name; });
    return maps_;
}

const PaintedMap* GroomDocument::map(const std::string& name) const {
    maps();  // fills maps_, which the pointer points into
    for (const PaintedMap& m : maps_) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

std::string GroomDocument::mapsDirectory() const {
    if (path_.empty()) return {};
    const std::filesystem::path groom = std::filesystem::u8path(path_);
    return forwardSlashes((groom.parent_path() / "masks" / groom.stem()).u8string());
}

std::string GroomDocument::textureMapFile(const std::string& name) const {
    const ScalpMaps& maps = scene_->session().scalpMaps();
    const auto it = maps.maps.find(name);
    if (it == maps.maps.end() || it->second.kind != "texture") return {};
    return resolveMapPath(it->second.texture);
}

void GroomDocument::commitMaps(const std::string& label, const ScalpMaps& maps) {
    StrokeEdit edit;
    edit.mode = label;
    edit.set(StrokeField::ScalpMaps, maps.toJson());
    commitStroke(std::move(edit));
    edits_.back().label = label;
}

std::string GroomDocument::createMap(const std::string& name, const std::string& kind, bool color, float value, int resolution,
                                     const PaintColor& fill) {
    if (!validMapName(name)) return mapNameError(name);  // the hosts' one rule (scalp_maps.h)
    for (const PaintedMap& m : maps()) {
        if (m.name == name) return "the groom already has a map named '" + name + "'";
    }
    const MeshPtr scalp = session().cageScalp();  // vertex maps are stored per vertex of the cage
    if (!scalp) return "no scalp to paint on";
    if (kind == "guides") {
        if (color) return "a guide layer is gray: make a color map on the scalp's vertices or as a texture";
        return editGuides("create map " + name, [&](GuideHierarchy& h) {
            bool any = false;
            for (const HierarchyGuide& g : h.guides()) {
                if (g.parent != kNoGuide) continue;  // children inherit their primary's
                h.setAttribute(g.id, name, value);
                any = true;
            }
            if (!any) throw std::invalid_argument("no primary guides to carry the layer: place some first");
        });
    }
    ScalpMaps maps = session().scalpMaps();
    ScalpMapEntry entry;
    entry.channels = color ? kColorMap : kGrayMap;
    if (kind == "vertices") {
        entry.kind = "vertices";
        const std::size_t n = scalp->pointCount();
        entry.values.resize(n * static_cast<std::size_t>(entry.channels));
        for (std::size_t v = 0; v < n; ++v) {
            if (color) {
                for (std::size_t c = 0; c < 4; ++c) entry.values[v * 4 + c] = fill[c];
            } else {
                entry.values[v] = value;
            }
        }
    } else if (kind == "texture") {
        if (!scalp->hasUVs()) return "the scalp has no UVs to paint a texture map on";
        const std::string folder = mapsDirectory();
        if (folder.empty()) return "save the groom first: a texture map's image goes in a masks folder beside it";
        // masks/<groom>/<name>.png beside the groom file, a free <name>_2.png when that is taken
        // (as the Maya host names them), the file written now so the map reads from the start.
        std::filesystem::path file = std::filesystem::u8path(folder) / (name + ".png");
        for (int k = 2; std::filesystem::exists(file); ++k) file = std::filesystem::u8path(folder) / (name + "_" + std::to_string(k) + ".png");
        try {
            std::filesystem::create_directories(file.parent_path());
            const int size = std::clamp(resolution, 4, 16384);
            const TexturePaint paint = color ? TexturePaint(*scalp, size, size, fill) : TexturePaint(*scalp, size, size, value);
            paint.save(forwardSlashes(file.u8string()));
        } catch (const std::exception& e) {
            return std::string("cannot write the map's image: ") + e.what();
        }
        entry.kind = "texture";
        const std::filesystem::path groomFolder = std::filesystem::u8path(path_).parent_path();
        entry.texture = pathFromGroomFolder(file.u8string(), groomFolder.u8string());  // as every host stores it (paths.h)
    } else {
        return "a map's kind is vertices, texture or guides, not '" + kind + "'";
    }
    maps.maps[name] = std::move(entry);
    commitMaps("create map " + name, maps);
    return {};
}

std::string GroomDocument::renameMap(const std::string& from, const std::string& to) {
    if (!validMapName(to)) return mapNameError(to);
    if (from == to) return {};
    const std::vector<PaintedMap> all = maps();
    const PaintedMap* source = map(from);
    if (!source) return "no map named '" + from + "'";
    for (const PaintedMap& m : all) {
        if (m.name == to) return "the groom already has a map named '" + to + "'";
    }
    const std::string kind = source->kind;
    const std::string label = "rename map " + from;
    if (kind == "guides") {
        const std::string error = editGuides(label, [&](GuideHierarchy& h) { h.renameAttribute(from, to); });
        if (!error.empty()) return error;
    } else {
        ScalpMaps maps = session().scalpMaps();
        ScalpMapEntry entry = maps.maps.at(from);
        maps.maps.erase(from);
        if (entry.kind == "texture" && !entry.texture.empty()) {
            // The file follows when it is named after the map (scalp_maps.h's renamedMapTexture,
            // Maya's rule), to a free name beside it; a file in the way, or one that cannot move, stays.
            const std::filesystem::path full = std::filesystem::u8path(resolveMapPath(entry.texture));
            const std::string renamed = renamedMapTexture(full.generic_u8string(), from, to);
            if (!renamed.empty()) {
                std::filesystem::path target = std::filesystem::u8path(renamed);
                const std::string stem = target.stem().u8string();
                for (int k = 2; std::filesystem::exists(target); ++k) {
                    target = full.parent_path() / (stem + "_" + std::to_string(k) + full.extension().u8string());
                }
                std::error_code ec;
                std::filesystem::rename(full, target, ec);
                if (!ec) {
                    entry.texture = pathFromGroomFolder(target.u8string(), std::filesystem::u8path(path_).parent_path().u8string());
                    paints_.erase(from);
                }
            }
        }
        maps.maps[to] = std::move(entry);
        commitMaps(label, maps);
    }
    // The Map nodes and map params reading the old name read the new one (the core's rename,
    // Maya's too), in the same undo step (the entry holds the maps' texts and the whole graph
    // before and after), so an undo puts both back.
    const json before = graph_;
    bool retargeted = false;
    const std::string graph = retargetMapReaders(graph_.dump(), from, to, &retargeted);
    if (!retargeted) return {};
    graph_ = json::parse(graph);
    const std::string error = applyGraph();
    if (!error.empty()) {
        graph_ = before;
        applyGraph();
        return error;
    }
    Edit& entry = edits_.back();
    entry.whole = true;
    entry.graphBefore = before;
    entry.graphAfter = graph_;
    return {};
}

std::shared_ptr<TexturePaint> GroomDocument::texturePaint(const std::string& name, int channels, int resolution) {
    const std::string full = textureMapFile(name);
    if (full.empty()) throw std::runtime_error("this groom has no texture map named " + name);
    const MeshPtr scalp = session().scalp();
    if (!scalp) throw std::runtime_error("the scalp has no UVs to paint a texture map on");
    requirePaintablePng(full);
    // The paint the last stroke left is painted on while the file is what it wrote (and the
    // scalp and channels the same); otherwise the file is opened as it reads now, or a blank
    // map made when it is not there yet (scalp_paint.h, as the Maya host's brush keeps it).
    try {
        return paints_[name].take(*scalp, full, channels, resolution);
    } catch (...) {
        paints_.erase(name);
        throw;
    }
}

void GroomDocument::writeTexturePaint(const std::string& name) {
    auto it = paints_.find(name);
    if (it == paints_.end() || !it->second.paint) return;
    KeptTexturePaint& opened = it->second;
    std::filesystem::create_directories(std::filesystem::u8path(opened.path).parent_path());
    opened.paint->save(opened.path);
    opened.written();
    // The graph reads the file through the texture cache (by path and stamp): the Map nodes
    // see the new image on the next evaluation.
}

void GroomDocument::applyTextureEdit(const StrokeEdit& edit, bool after) {
    if (edit.textureMap.empty() || edit.texture.empty()) return;
    try {
        const std::shared_ptr<TexturePaint> paint = texturePaint(edit.textureMap, edit.texture.channels, edit.texture.width);
        paint->applyEdit(edit.texture, after);
        writeTexturePaint(edit.textureMap);
    } catch (const std::exception&) {
        // The map went (removed, or its file): nothing to put the tiles into.
    }
}

ScalpMapView GroomDocument::scalpMapView(const std::string& name, const std::string& channel) {
    ScalpMapView view = session().scalpMapView(name, channel);
    if (view.kind != "texture") return view;
    const MeshPtr scalp = session().scalp();
    if (!scalp || !scalp->hasUVs()) {
        view.kind.clear();
        return view;
    }
    // The image (the brush's live one while painting, else the file's) at each vertex's uv:
    // the viewport draws the map per vertex, which is the texture at the scalp's own resolution.
    const std::uint64_t key = hashCombine(hashCombine(view.version, scalp->contentHash()), view.channels);
    if (key != mapViewKey_ || !mapViewValues_) {
        TexturePtr texture;
        if (!view.preview) {
            try {
                texture = loadTexture(view.texture);
            } catch (const std::exception&) {
            }
        }
        const Texture::Image* image = texture ? texture->image() : nullptr;
        if (!view.preview && !image) {
            view.kind.clear();
            return view;
        }
        const std::size_t per = view.channels == 4 ? 4 : 1;
        auto values = std::make_shared<std::vector<float>>(scalp->pointCount() * per, 0.0f);
        std::vector<std::uint8_t> seen(scalp->pointCount(), 0);
        const auto& uvs = scalp->cornerUVs();
        const auto& triangles = scalp->triangles();
        for (std::size_t t = 0; t < triangles.size(); ++t) {
            for (std::size_t k = 0; k < 3; ++k) {
                const auto v = static_cast<std::size_t>(triangles[t][k]);
                if (seen[v]) continue;
                seen[v] = 1;
                const float u = uvs[t * 3 + k][0], w = uvs[t * 3 + k][1];
                std::array<float, 4> c{};
                if (view.preview) {
                    // The preview's rows are bottom-up (v = 0 the bottom row), 8-bit RGBA.
                    const int pw = view.preview->width(), ph = view.preview->height();
                    if (pw <= 0 || ph <= 0) continue;
                    const int x = std::clamp(static_cast<int>(std::floor(u * static_cast<float>(pw))), 0, pw - 1);
                    const int y = std::clamp(static_cast<int>(std::floor(w * static_cast<float>(ph))), 0, ph - 1);
                    const std::vector<std::uint8_t>& rgba = view.preview->rgba();
                    const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x)) * 4;
                    if (at + 3 >= rgba.size()) continue;
                    for (std::size_t i = 0; i < 4; ++i) c[i] = rgba[at + i] / 255.0f;
                    if (per == 1) c[0] = (c[0] + c[1] + c[2]) / 3.0f;  // the preview shows a gray map's value in r g b
                } else {
                    c = image->sample(u, w);
                    if (per == 1) c[0] = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];  // as a gray map reads its file
                }
                for (std::size_t i = 0; i < per; ++i) (*values)[v * per + i] = c[i];
            }
        }
        mapViewValues_ = values;
        mapViewKey_ = key;
    }
    view.kind = "vertices";
    view.values = mapViewValues_;
    view.texture.clear();
    view.preview = nullptr;
    return view;
}

GroomDocument::~GroomDocument() = default;

// ---- Time ----------------------------------------------------------------------------------------

double GroomDocument::framesPerSecond() const {
    const double fps = scene_->framesPerSecond();
    return fps > 0.0 ? fps : 24.0;
}

bool GroomDocument::cacheFrameRange(double* first, double* last) const {
    double start = 0.0, end = 0.0;
    if (!scene_->timeRange(&start, &end)) return false;
    // Frames as Maya numbers them: a sample at 1/24 s is frame 1 (rounded: the samples sit on
    // frames up to the file's precision).
    const double fps = framesPerSecond();
    if (first) *first = std::round(secondsToFrame(start, fps));
    if (last) *last = std::round(secondsToFrame(end, fps));
    return true;
}

void GroomDocument::setFollowScalp(bool on) {
    if (session().followScalp() == on) return;
    session().setFollowScalp(on);
    dirty_ = true;
}

void GroomDocument::setScaleSource(ScaleSource source) {
    if (session().scaleSource() == source) return;
    session().setScaleSource(source);
    dirty_ = true;
}

void GroomDocument::setScaleReference() {
    if (!(session().scaleArea() > 0.0) || session().scaleReference() == session().scaleArea()) return;
    session().setScaleReference(session().scaleArea());
    dirty_ = true;
}

int GroomDocument::scalpSubdivision() const {
    const MeshPtr& scalp = scene_->file().data.scalp;
    return scalp ? std::clamp(scalp->renderSubdivision(), 0, kMaxSubdivisionLevels) : 0;
}

std::string GroomDocument::setScalpSubdivision(int levels) {
    const MeshPtr& scalp = scene_->file().data.scalp;
    if (!scalp) return "the groom has no scalp";
    if (levels < 0 || levels > kMaxSubdivisionLevels) return "Scalp Subdivision is off or 1 to " + std::to_string(kMaxSubdivisionLevels);
    const Edit::Subdivision before{scalp->renderSubdivision(), scalp->renderSubdivisionFrom()};
    const Edit::Subdivision after{levels, levels > 0 ? std::string("Scalp Subdivision") : std::string()};
    if (before.levels == after.levels) return {};
    commitPreview();
    Edit edit;
    edit.side = true;
    edit.label = "scalp subdivision";
    edit.subdivision = std::make_pair(before, after);
    applySide(edit, true);
    record(std::move(edit));
    dirty_ = true;
    return {};
}

int GroomDocument::scalpLevels() const {
    const std::map<std::string, int> levels = session().scalpSubdivisions();
    const auto it = levels.find(kMainScalpName);
    return it == levels.end() ? 0 : it->second;
}

std::size_t GroomDocument::refinedTriangleCount(const ScalpMesh& cage, int levels) {
    levels = std::clamp(levels, 0, kMaxSubdivisionLevels);
    if (levels == 0) return cage.triangleCount();
    // Level 1 makes a quad per corner of each face (a polygon of n triangles has n + 2 corners; a
    // mesh without polygons is its triangles); each further level makes four of each quad.
    std::size_t corners = 0;
    if (cage.polygons().empty()) {
        corners = 3 * cage.triangleCount();
    } else {
        for (const std::int32_t n : cage.polygons()) corners += n > 0 ? static_cast<std::size_t>(n) + 2 : 0;
    }
    std::size_t triangles = 2 * corners;
    for (int l = 1; l < levels; ++l) triangles *= 4;
    return triangles;
}

MeshPtr GroomDocument::refinedFileScalp() const {
    const MeshPtr& scalp = scene_->file().data.scalp;
    const int levels = scalpLevels();
    if (!scalp || levels == 0) return scalp;
    // Kept by the scalp's surface (its render level changes the mesh object, not the surface).
    if (!refinedFrom_ || refinedFrom_->surfaceHash() != scalp->surfaceHash() || refinedFrom_->polygonHash() != scalp->polygonHash()) {
        // The surface alone: points, triangles, polygons and UVs (the views neither show nor pick
        // the color sets).
        auto cage = std::make_shared<ScalpMesh>(scalp->points(), scalp->triangles());
        if (scalp->hasUVs()) cage->setCornerUVs(scalp->cornerUVs());
        cage->setPolygons(scalp->polygons());
        refinedFrom_ = std::move(cage);
        refinedByLevel_.clear();
    }
    MeshPtr& refined = refinedByLevel_[levels];
    if (!refined) refined = subdivideScalp(refinedFrom_, levels);
    return refined;
}

void GroomDocument::loadGraph(const std::string& text) {
    try {
        // The core writes what it evaluates, with the editors' layout (each node's "ui", the
        // graph's "backdrops": graph_io.h), so positions travel with the groom between hosts.
        graph_ = json::parse(saveGraphJson(loadGraphJson(text)));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("graph: ") + e.what());
    }
}

std::vector<GroomDocument::ConnectionInfo> GroomDocument::connections() const {
    std::vector<ConnectionInfo> out;
    if (!graph_.contains("connections")) return out;
    for (const json& c : graph_["connections"]) {
        out.push_back({c.value("from", std::string()), c.value("output", std::string()), c.value("to", std::string()),
                       c.value("input", std::string())});
    }
    return out;
}

bool GroomDocument::nodePosition(const std::string& node, float* x, float* y) const {
    const json* n = nodeJson(node);
    if (!n || !n->contains("ui")) return false;
    const json& ui = (*n)["ui"];
    if (!ui.is_object() || !ui.contains("x") || !ui.contains("y") || !ui["x"].is_number() || !ui["y"].is_number()) return false;
    *x = ui["x"].get<float>();
    *y = ui["y"].get<float>();
    return true;
}

void GroomDocument::setNodePosition(const std::string& node, float x, float y) {
    json* n = nodeJson(node);
    if (!n) return;
    (*n)["ui"] = {{"x", x}, {"y", y}};
    dirty_ = true;
}

std::string GroomDocument::editGraph(const std::string& label, const std::function<void(json&)>& edit,
                                     const std::vector<std::pair<std::string, std::string>>& moved,
                                     const std::map<std::string, std::string>& copied) {
    commitPreview();
    const json before = graph_;
    try {
        edit(graph_);
        prunePins(graph_);  // the pins of links gone, or fed from elsewhere now, go with them
    } catch (const std::exception& e) {
        graph_ = before;
        return e.what();
    }
    const std::string error = applyGraph();
    if (!error.empty()) {
        graph_ = before;
        applyGraph();
        return error;
    }
    Edit entry;
    entry.label = label;
    entry.whole = true;
    entry.graphBefore = before;
    entry.graphAfter = graph_;
    // The Sculpt nodes' edits follow renames, groups, ungroups, copies and removals, in the same step.
    const std::string sculptsBefore = storedText(StrokeField::Sculpts);
    std::string sculpts = sculptsBefore;
    std::string target = session().sculptTarget();
    try {
        sculpts = moveSculptNodes(sculpts, moved);
        for (const auto& [copy, source] : copied) sculpts = copySculptNode(sculpts, source, copy);
        if (graph_.contains("nodes")) {
            std::set<std::string> names;
            for (const json& n : graph_["nodes"]) names.insert(n.value("name", std::string()));
            for (const std::string& node : sculptedNodes(sculpts)) {
                if (!names.count(node.substr(0, node.find('/')))) sculpts = clearSculptNode(sculpts, node);
            }
            for (const auto& [from, to] : moved) {
                if (target == from) target = to;
            }
            if (!target.empty() && !names.count(target)) target.clear();  // gone, or grouped
        }
    } catch (const std::exception&) {
        sculpts = sculptsBefore;  // unreadable: left as it is (the session says why)
    }
    if (sculpts != sculptsBefore) {
        StrokeEdit stroke;
        stroke.mode = label;
        stroke.set(StrokeField::Sculpts, sculpts);
        stroke.text(StrokeField::Sculpts).before = sculptsBefore;
        stroke.text(StrokeField::Sculpts).hasBefore = true;
        stroke.apply(session(), true);
        entry.stroke = std::make_shared<const StrokeEdit>(std::move(stroke));
    }
    if (target != session().sculptTarget()) session().setSculptTarget(target);
    record(std::move(entry));
    dirty_ = true;
    return {};
}

std::vector<std::string> GroomDocument::sculptNodes() const {
    std::vector<std::string> names;
    if (!graph_.contains("nodes")) return names;
    for (const json& n : graph_["nodes"]) {
        if (n.value("type", std::string()) == op::kSculpt) names.push_back(n.value("name", std::string()));
    }
    return names;
}

void GroomDocument::setSculptTarget(const std::string& node) { session().setSculptTarget(node); }

const std::string& GroomDocument::sculptTarget() const { return session().sculptTarget(); }

std::string GroomDocument::clearSculptEdits(const std::string& node) {
    return editStoredText("clear sculpt edits", StrokeField::Sculpts,
                          [&](std::string& text) { text = clearSculptNode(text, node); });
}

std::size_t GroomDocument::sculptedGuides(const std::string& node) const {
    try {
        const SculptEdits* edits = SculptSet::fromJson(storedText(StrokeField::Sculpts)).node(node);
        return edits ? edits->guides.size() : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

std::string GroomDocument::paintParam(const std::string& node, const std::string& name, std::string* mapName, const std::string& kind,
                                      bool* made) {
    if (made) *made = false;
    if (!nodeJson(node)) return "no node '" + node + "'";
    // The map the param reads already (a Map node, a color set, a texture, a guide layer): that one.
    try {
        const ParamMapRef ref = paramMapOf(graph_.dump(), session().scalpMaps(), session().storedHierarchy().get(), node, name);
        if (!ref.map.empty()) {
            if (mapName) *mapName = ref.map;
            return {};
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    if (kind != "vertices" && kind != "texture") return "a setting's map lives on the scalp: vertices or texture, not '" + kind + "'";
    // A new map named for the param (free of the maps and guide layers there), white: the param
    // as it was, black 0.
    const std::string base = sanitizeMapName(node + "_" + name);
    std::string map = base;
    for (int k = 1; this->map(map); ++k) map = base + std::to_string(k);
    std::string error = createMap(map, kind, false, 1.0f, 1024, PaintColor{1.0f, 1.0f, 1.0f, 1.0f});
    if (!error.empty()) return error;
    error = editGraph("map " + node + "." + name,
                      [&](json& g) { g = json::parse(wireParamMap(g.dump(), session().scalpMaps(), node, name, map, kind)); });
    if (!error.empty()) {  // the map goes with the refused wiring
        undo();
        edits_.resize(undoIndex_);
        return error;
    }
    // The map and the wiring as one step: undo takes both back.
    Edit wiring = std::move(edits_.back());
    edits_.pop_back();
    Edit& step = edits_.back();
    step.whole = true;
    step.graphBefore = std::move(wiring.graphBefore);
    step.graphAfter = std::move(wiring.graphAfter);
    step.label = wiring.label;
    undoIndex_ = edits_.size();
    if (mapName) *mapName = map;
    if (made) *made = true;
    return {};
}

std::string GroomDocument::addNode(const std::string& type, float x, float y, std::string* made, const std::string& name) {
    AddOptions options;
    options.name = name;
    return addNode(type, x, y, made, options);
}

namespace {

// GraphModel.insert_on_link (Maya's graph.py, the Blender helper's too): the one link from (its
// output) -> to.input routed through `node`: node reads from on its first input that takes from's
// output, to reads node. Throws GraphError (the edit as it was) when the link is not there or no
// input of node fits.
void insertOnLinkIn(GraphEdit& edit, const GroomDocument::NodeLink& link, const std::string& node) {
    if (!edit.hasNode(node)) throw GraphError("no node '" + node + "'");
    if (node == link.from || node == link.to) throw GraphError(node + " is an end of that link");
    const json g = json::parse(edit.toJson());
    bool linked = false;
    for (const json& c : g.value("connections", json::array())) {
        linked = linked || (c.value("from", std::string()) == link.from && c.value("to", std::string()) == link.to &&
                            c.value("input", std::string()) == link.input && c.value("output", std::string()) == link.output);
    }
    if (!linked) throw GraphError(link.from + " is not connected to " + link.to + "." + link.input);
    std::string why;
    const json spec = json::parse(edit.nodeSpecJson(node));
    for (const json& p : spec.value("inputs", json::array())) {
        GraphEdit trial = edit;
        try {
            trial.connect(link.from, node, p.value("name", std::string()), link.output);
            trial.connect(node, link.to, link.input);
        } catch (const GraphError& e) {
            if (why.empty()) why = e.what();
            continue;
        }
        edit = std::move(trial);
        return;
    }
    throw GraphError(node + " has no input that takes " + link.from + "'s " + edit.outputType(link.from, link.output) + " and feeds " +
                     link.to + "." + link.input + (why.empty() ? "" : " (" + why + ")"));
}

}  // namespace

std::string GroomDocument::addNode(const std::string& type, float x, float y, std::string* made, const AddOptions& options,
                                   std::string* wired, std::string* note) {
    if (wired) wired->clear();
    if (note) note->clear();
    if (!spec(type)) return "no operator '" + type + "'";
    // Named, seeded and placed as every host's editor does it (graph_edit.h): the type's name
    // with its first letter lowered, numbered past the ones there (Maya's: noise, noise1, ...); a
    // library node's after its label.
    std::string chosen = options.name;
    try {
        const std::string base = libraryNodeId(type).empty() ? type : libraryNodeName(spec(type)->value("label", type));
        if (chosen.empty()) chosen = GraphEdit(graph_.dump()).uniqueName(base);
        else if (nodeJson(chosen)) chosen = GraphEdit(graph_.dump()).uniqueName(chosen);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (made) *made = chosen;
    // Its defaults that measure something fitted to the scalp, as a preset's are.
    const MeshPtr& scalp = file().data.scalp;
    // (At the size the params are at: the groom's 1x when it is scaled with its scalp.)
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= session().groomScale();
    std::string after, refused;
    const std::string error = editGraph("add " + chosen, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.addNode(type, chosen, options.params.is_object() ? options.params.dump() : std::string(), x, y, size);
        if (options.onLink) {
            // Tab over a wire: into it, else added unlinked (said).
            try {
                GraphEdit trial = edit;
                insertOnLinkIn(trial, *options.onLink, chosen);
                edit = std::move(trial);
                after = options.onLink->from;
            } catch (const GraphError& e) {
                refused = std::string("Not inserted: ") + e.what();
            }
        } else if (!options.after.empty() && edit.hasNode(options.after)) {
            // After the selected node: into its first link the new node fits in (it goes into the
            // chain, as Maya's Add Node), else fed by it on its first input that takes its output.
            const json now = json::parse(edit.toJson());
            for (const json& c : now.value("connections", json::array())) {
                if (c.value("from", std::string()) != options.after || !after.empty()) continue;
                try {
                    GraphEdit trial = edit;
                    insertOnLinkIn(trial, {options.after, c.value("output", std::string()), c.value("to", std::string()), c.value("input", std::string())},
                                   chosen);
                    edit = std::move(trial);
                    after = options.after;
                } catch (const GraphError&) {
                }
            }
            if (after.empty()) {
                const json spec = json::parse(edit.nodeSpecJson(chosen));
                for (const json& p : spec.value("inputs", json::array())) {
                    try {
                        GraphEdit trial = edit;
                        trial.connect(options.after, chosen, p.value("name", std::string()));
                        edit = std::move(trial);
                        after = options.after;
                        break;
                    } catch (const GraphError&) {
                    }
                }
            }
        }
        g = json::parse(edit.toJson());
    });
    if (error.empty()) {
        if (wired) *wired = after;
        if (note) *note = refused;
    }
    return error;
}

std::string GroomDocument::insertOnLink(const std::string& node, const NodeLink& link) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    return editGraph("insert " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        insertOnLinkIn(edit, link, node);
        g = json::parse(edit.toJson());
    });
}

std::vector<std::string> GroomDocument::shownInnerParams(const std::string& compound) const {
    std::vector<std::string> out;
    const json* n = nodeJson(compound);
    if (!n || n->value("type", std::string()) != "Compound") return out;
    const json params = n->value("params", json::object());
    const json definition = params.is_object() ? params.value("definition", json::object()) : json::object();
    const json expose = definition.is_object() ? definition.value("expose", json::object()) : json::object();
    if (!expose.is_object()) return out;
    for (const auto& [name, target] : expose.items()) {
        if (target.is_string()) out.push_back(target.get<std::string>());
    }
    return out;
}

std::string GroomDocument::showInnerParam(const std::string& compound, const std::string& target, bool shown) {
    const json* n = nodeJson(compound);
    if (!n) return "no node '" + compound + "'";
    if (n->value("type", std::string()) != "Compound") return compound + " is not a Compound";
    const auto dot = target.find('.');
    if (dot == std::string::npos) return "an inner param is \"node.param\" inside the compound";
    const std::string inner = target.substr(0, dot), param = target.substr(dot + 1);
    const std::vector<std::string> now = shownInnerParams(compound);
    if ((std::find(now.begin(), now.end(), target) != now.end()) == shown) return {};
    // Named after the param, or node + Param when the compound has a param of that name (Maya's).
    std::string name = param;
    if (shown) {
        if (const json* s = nodeSpec(compound)) {
            for (const json& p : s->value("params", json::array())) {
                if (p.value("name", std::string()) == param) name = inner + static_cast<char>(std::toupper(static_cast<unsigned char>(param[0]))) + param.substr(1);
            }
        }
    }
    return editGraph((shown ? "show " : "hide ") + target + " on " + compound, [&](json& g) {
        for (json& c : g["nodes"]) {
            if (c.value("name", std::string()) != compound) continue;
            json& params = c["params"];
            if (!params.is_object()) params = json::object();
            json& definition = params["definition"];
            if (!definition.is_object()) definition = json::object();
            json& expose = definition["expose"];
            if (!expose.is_object()) expose = json::object();
            json* innerNode = nullptr;
            if (definition.contains("graph") && definition["graph"].is_object() && definition["graph"].contains("nodes")) {
                for (json& x : definition["graph"]["nodes"]) {
                    if (x.is_object() && x.value("name", std::string()) == inner) innerNode = &x;
                }
            }
            if (shown) {
                bool known = false;
                if (innerNode) {
                    if (const json* s = spec(innerNode->value("type", std::string()))) {
                        for (const json& p : s->value("params", json::array())) known = known || p.value("name", std::string()) == param;
                    }
                }
                if (!known) throw std::invalid_argument("the compound has no param '" + target + "'");
                expose[name] = target;
            } else {
                // Its value goes back inside: the inner node keeps what the compound set last.
                std::vector<std::string> names;
                for (const auto& [key, t] : expose.items()) {
                    if (t.is_string() && t.get<std::string>() == target) names.push_back(key);
                }
                for (const std::string& key : names) {
                    if (params.contains(key)) {
                        if (innerNode) (*innerNode)["params"][param] = params[key];
                        params.erase(key);
                    }
                    expose.erase(key);
                }
            }
        }
    });
}

std::string GroomDocument::addControl(const std::string& node, const std::string& param, std::string* made) {
    const json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    if (!spec("Control")) return "this build has no Control node";
    const json value = paramOrDefault(node, param);
    const double start = value.is_number() ? value.get<double>() : 0.0;
    float x = 0.0f, y = 0.0f;
    nodePosition(node, &x, &y);
    std::string name;
    const std::string error = editGraph("drive " + node + "." + param + " with a slider", [&](json& g) {
        GraphEdit edit(g.dump());
        name = edit.uniqueName(param + "Control");
        // Its slider spans 0 to 1 (or to the value, past 1), starting where the param is.
        const json params = {{"name", param}, {"default", start}, {"min", std::min(0.0, start)}, {"max", std::max(1.0, start)}};
        edit.addNode("Control", name, params.dump(), x - 230.0, y + 160.0);
        g = json::parse(edit.toJson());
        for (json& m : g["nodes"]) {
            if (m.value("name", std::string()) != node) continue;
            json inputs = m.value("paramInputs", json::array());
            if (std::find(inputs.begin(), inputs.end(), json(param)) == inputs.end()) inputs.push_back(param);
            m["paramInputs"] = inputs;
        }
        GraphEdit wired(g.dump());
        wired.connect(name, node, param);
        g = json::parse(wired.toJson());
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::vector<GroomDocument::ControlSlider> GroomDocument::controlSliders() const {
    std::vector<ControlSlider> out;
    const std::map<std::string, float>& values = session().data().controls;
    for (const json& n : graph_.value("nodes", json::array())) {
        if (n.value("type", std::string()) != "Control") continue;
        const json params = n.value("params", json::object());
        ControlSlider c;
        c.node = n.value("name", std::string());
        c.name = params.value("name", std::string("control"));
        const auto number = [&](const char* key, double fallback) {
            const auto it = params.find(key);
            return it != params.end() && it->is_number() ? it->get<double>() : fallback;
        };
        c.min = number("min", 0.0);
        c.max = number("max", 1.0);
        if (!(c.max > c.min)) c.max = c.min + 1.0;
        const auto set = values.find(c.name);
        c.value = set != values.end() ? static_cast<double>(set->second) : number("default", 0.0);
        if (std::none_of(out.begin(), out.end(), [&](const ControlSlider& o) { return o.name == c.name; })) out.push_back(c);
    }
    return out;
}

bool GroomDocument::setControlValue(const std::string& name, double value) {
    const std::vector<ControlSlider> sliders = controlSliders();
    if (std::none_of(sliders.begin(), sliders.end(), [&](const ControlSlider& c) { return c.name == name; })) return false;
    std::map<std::string, float> controls = session().data().controls;
    if (controls.count(name) && controls[name] == static_cast<float>(value)) return true;
    controls[name] = static_cast<float>(value);
    session().setControls(std::move(controls));
    dirty_ = true;
    return true;
}

std::string GroomDocument::bakeNodeToGuides(const std::string& node, const std::string& group, float density, std::size_t* count) {
    if (count) *count = 0;
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (group.empty()) return "bake into a group: it needs a name";
    if (!(density > 0.0f && density <= 1.0f)) return "the density is above 0, at most 1";
    if (!session().scalp()) return "the groom has no scalp";
    // The node as it is now (the last evaluation's inputs), one guide per strand.
    GuideSet baked;
    try {
        const Value value = session().evaluateNode(node, density, time_);
        if (const GuidesPtr* guides = std::get_if<GuidesPtr>(&value); guides && *guides) {
            baked = **guides;
        } else if (const StrandsPtr* strands = std::get_if<StrandsPtr>(&value); strands && *strands) {
            for (std::size_t i = 0; i < (*strands)->strandCount(); ++i) {
                const Vec3f* p = (*strands)->strandPoints(i);
                baked.add((*strands)->ids()[i], std::vector<Vec3f>(p, p + (*strands)->strandPointCount(i)));
            }
        } else {
            return node + " gives no guides or strands (nothing to bake)";
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    if (baked.empty()) return node + " made no guides";
    std::size_t added = 0;
    const std::string error = editGuides("bake " + node, [&](GuideHierarchy& h) {
        GroupId gid = h.findGroup(group);
        if (gid == kNoGroup) gid = h.addGroup(group);
        else if (h.groupDepth(gid) != 0) throw std::invalid_argument("'" + group + "' is a child group: bake into a root group");
        added = h.addGuides(gid, baked).size();
    });
    if (error.empty() && count) *count = added;
    return error;
}

std::string GroomDocument::removeNode(const std::string& node) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (graph_.value("output", std::string()) == node) return "the output node stays";
    return editGraph("remove " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.removeNode(node);  // with its connections
        g = json::parse(edit.toJson());
    });
}

std::string GroomDocument::removeNodes(const std::vector<std::string>& nodes) {
    std::vector<std::string> gone;
    for (const std::string& n : nodes) {
        if (nodeJson(n) && graph_.value("output", std::string()) != n && std::find(gone.begin(), gone.end(), n) == gone.end()) gone.push_back(n);
    }
    if (gone.empty()) return nodes.size() == 1 ? removeNode(nodes.front()) : std::string("the output node stays");
    if (gone.size() == 1) return removeNode(gone.front());
    return editGraph("remove " + std::to_string(gone.size()) + " nodes", [&](json& g) {
        GraphEdit edit(g.dump());
        for (const std::string& n : gone) edit.removeNode(n);
        g = json::parse(edit.toJson());
    });
}

namespace {

constexpr const char* kClipboardKey = "groomlabNodes";  // Maya's graph.py CLIPBOARD_KEY

}  // namespace

std::string GroomDocument::copyNodes(const std::vector<std::string>& nodes) const {
    json data = {{kClipboardKey, 1}, {"nodes", json::array()}, {"connections", json::array()}};
    std::set<std::string> copied;
    // In the graph's order, as Maya's copy_nodes.
    for (const json& n : graph_.value("nodes", json::array())) {
        const std::string name = n.value("name", std::string());
        if (n.value("type", std::string()) == "Output") continue;
        if (std::find(nodes.begin(), nodes.end(), name) == nodes.end()) continue;
        data["nodes"].push_back(n);
        copied.insert(name);
    }
    if (copied.empty()) return {};
    for (const json& c : graph_.value("connections", json::array())) {
        if (copied.count(c.value("from", std::string())) && copied.count(c.value("to", std::string()))) data["connections"].push_back(c);
    }
    return data.dump();
}

std::string GroomDocument::pasteNodes(const std::string& text, float dx, float dy, std::vector<std::string>* made) {
    const json data = json::parse(text, nullptr, false);
    if (!data.is_object() || !data.contains(kClipboardKey) || !data.value("nodes", json()).is_array() || data["nodes"].empty()) {
        return "the clipboard holds no GroomLab nodes";
    }
    for (const json& n : data["nodes"]) {
        if (!n.is_object() || !spec(n.value("type", std::string()))) return "the clipboard names an unknown node type";
    }
    std::map<std::string, std::string> renamed;  // copied name -> pasted name
    std::map<std::string, std::string> copiedSculpts;
    std::vector<std::string> names;
    const std::string error = editGraph("paste " + std::to_string(data["nodes"].size()) + " nodes", [&](json& g) {
        if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
        for (const json& n : data["nodes"]) {
            const std::string from = n.value("name", std::string());
            const std::string name = GraphEdit(g.dump()).uniqueName(from.empty() ? n.value("type", std::string()) : from);
            json node = n;
            node["name"] = name;
            const json ui = n.value("ui", json::object());
            node["ui"] = {{"x", (ui.is_object() ? ui.value("x", 0.0) : 0.0) + dx}, {"y", (ui.is_object() ? ui.value("y", 0.0) : 0.0) + dy}};
            g["nodes"].push_back(node);
            renamed[from] = name;
            names.push_back(name);
            const std::string type = node.value("type", std::string());
            if ((type == op::kSculpt || type == "Compound") && nodeJson(from)) copiedSculpts[name] = from;
        }
        GraphEdit edit(g.dump());
        for (const json& c : data.value("connections", json::array())) {
            const auto a = renamed.find(c.value("from", std::string())), b = renamed.find(c.value("to", std::string()));
            if (a == renamed.end() || b == renamed.end()) continue;
            edit.connect(a->second, b->second, c.value("input", std::string()), c.value("output", std::string()));
        }
        g = json::parse(edit.toJson());
    }, {}, copiedSculpts);
    if (error.empty() && made) *made = names;
    return error;
}

std::string GroomDocument::connectNodes(const std::string& from, const std::string& output, const std::string& to,
                                        const std::string& input) {
    // The core's checks (graph_edit.h, Maya's Graph Editor's too): the nodes and ports there,
    // types that fit, no cycle; what fed the input before is replaced. Drawn by the user: the node
    // set to use the input where a dropdown says (a Clump's centersFrom), in the same undo step.
    return editGraph("connect " + from + " to " + to, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.connectByHand(from, to, input, output);
        g = json::parse(edit.toJson());
    });
}

std::string GroomDocument::disconnectInput(const std::string& to, const std::string& input) {
    return editGraph("disconnect " + to + "." + input, [&](json& g) {
        GraphEdit edit(g.dump());
        if (!edit.disconnect(to, input)) throw std::invalid_argument("nothing feeds " + to + "." + input);
        g = json::parse(edit.toJson());
    });
}

std::string GroomDocument::renameNode(const std::string& node, const std::string& newName) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (newName == node) return {};
    std::vector<std::pair<std::string, std::string>> moved;
    return editGraph("rename " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.renameNode(node, newName);
        g = json::parse(edit.toJson());
        renamePinNode(g, node, newName);
        moved = edit.movedNodes();  // a compound's inner nodes' edits go with it
    }, moved);
}

std::string GroomDocument::duplicateNode(const std::string& node, std::string* made) {
    const json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    if (n->value("type", std::string()) == "Output") return "the output node is one of a kind";
    // A copy beside the node with its params (its own seed when the operator has one: the core
    // picks a free one when the params leave it out), unconnected: the artist wires it.
    json params = n->value("params", json::object());
    params.erase("seed");
    float x = 0.0f, y = 0.0f;
    const bool placed = nodePosition(node, &x, &y);
    // A Sculpt node's copy takes its edits too (a compound's, its inner Sculpt nodes'): its name is
    // made first, so the step copies them.
    const std::string type = n->value("type", std::string());
    const bool sculpted = type == op::kSculpt || type == "Compound";
    std::string chosen = sculpted ? GraphEdit(graph_.dump()).uniqueName(type) : std::string();
    std::map<std::string, std::string> copied;
    if (!chosen.empty()) copied[chosen] = node;
    const std::string error = editGraph("duplicate " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        chosen = edit.addNode(type, chosen, params.dump(), placed ? std::optional<double>(x + 40.0) : std::nullopt,
                              placed ? std::optional<double>(y + 60.0) : std::nullopt);
        g = json::parse(edit.toJson());
    }, {}, copied);
    if (made) *made = chosen;
    return error;
}

std::string GroomDocument::setOutputNode(const std::string& node) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (graph_.value("output", std::string()) == node) return {};
    return editGraph("output " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.setOutputNode(node);
        g = json::parse(edit.toJson());
    });
}

std::pair<int, int> GroomDocument::nodeVersion(const std::string& node) const {
    const json* n = nodeJson(node);
    if (!n) return {0, 0};
    const json* s = spec(n->value("type", std::string()));
    const int newest = s ? s->value("version", 1) : 1;
    const auto it = n->find("version");
    return {it != n->end() && it->is_number_integer() ? it->get<int>() : newest, newest};
}

std::string GroomDocument::upgradeNode(const std::string& node) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    const auto [saved, newest] = nodeVersion(node);
    if (saved >= newest) return node + " has its operator's newest behaviour";
    return editGraph("upgrade " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.upgradeNode(node);
        g = json::parse(edit.toJson());
    });
}

void GroomDocument::refreshLibrary() {
    static const bool shipped = [] {
        setShippedNodeDir(shippedNodeFolder());
        return true;
    }();
    (void)shipped;
    for (auto it = specs_.begin(); it != specs_.end();) {
        if (it.key().rfind("node:", 0) == 0) it = specs_.erase(it);
        else ++it;
    }
    try {
        const json library = json::parse(libraryNodesJson());
        for (json entry : library["nodes"]) {
            entry["params"] = json::array();
            entry["hidden"] = false;
            entry["outputs"] = json::array({entry.value("output", json{{"name", "out"}, {"type", "Strands"}})});
            const std::string type = entry.value("type", std::string());
            specs_[type] = std::move(entry);
        }
    } catch (const std::exception&) {  // a library that cannot be read never stops the menus
    }
}

json GroomDocument::libraryInfo(const std::string& node) const {
    const json* n = nodeJson(node);
    const std::string type = n ? n->value("type", std::string()) : std::string();
    if (type != "Compound" && type != "Script") return nullptr;
    const json params = n->value("params", json::object());
    const json definition = params.is_object() ? params.value("definition", json()) : json();
    if (!definition.is_object() || !definition.contains("glnode")) return nullptr;
    try {
        const json spec = json::parse(nodeSpecJson(json{{"type", type}, {"params", {{"definition", definition}}}}.dump()));
        json info = spec.value("library", json::object());
        info["label"] = spec.value("label", std::string());
        info["description"] = spec.value("description", std::string());
        return info;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::string GroomDocument::updateLibraryNode(const std::string& node, const std::string& id) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    const MeshPtr& scalp = file().data.scalp;
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= session().groomScale();
    bool changed = false;
    const std::string error = editGraph("update " + node, [&](json& g) {
        GraphEdit edit(g.dump());
        changed = edit.updateLibraryNode(node, size, id);
        g = json::parse(edit.toJson());
    });
    if (error.empty() && !changed) return node + " has the library's copy already";
    return error;
}

std::string GroomDocument::saveAsNode(const std::string& node, const json& header, std::string* path) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    const MeshPtr& scalp = file().data.scalp;
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= session().groomScale();
    std::string id;
    try {
        const std::string text = nodeFileFromCompound(graph_.dump(), node, header.dump(), size);
        const std::string saved = saveNodeFile(text);
        if (path) *path = saved;
        id = json::parse(text).value("id", std::string());
    } catch (const std::exception& e) {
        return e.what();
    }
    refreshLibrary();
    const std::string error = updateLibraryNode(node, id);
    return error.find("has the library's copy already") != std::string::npos ? std::string() : error;
}

std::string GroomDocument::importNode(const std::string& path, std::string* id) {
    try {
        const std::string saved = importNodeFile(path);
        if (id) *id = readNodeFile(saved).id;
    } catch (const std::exception& e) {
        return e.what();
    }
    refreshLibrary();
    return {};
}

std::string GroomDocument::addScriptNode(float x, float y, std::string* made) {
    json definition = json::parse(scriptNodeTemplate("My Script"));
    const std::string base = definition.value("id", std::string("my.my_script"));
    for (int i = 2; findLibraryNode(definition["id"].get<std::string>()); ++i) definition["id"] = base + std::to_string(i);
    std::string chosen;
    try {
        chosen = GraphEdit(graph_.dump()).uniqueName(libraryNodeName(definition.value("label", std::string("My Script"))));
    } catch (const std::exception& e) {
        return e.what();
    }
    if (made) *made = chosen;
    const MeshPtr& scalp = file().data.scalp;
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= session().groomScale();
    return editGraph("add " + chosen, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.addNode(op::kScript, chosen, json{{"definition", definition}}.dump(), x, y, size);
        g = json::parse(edit.toJson());
    });
}

std::string GroomDocument::setScriptDefinition(const std::string& node, const json& definition) {
    const json* n = nodeJson(node);
    if (!n || n->value("type", std::string()) != "Script") return node + " is not a script node";
    // Checked first: its errors, a line each.
    std::string errors;
    const json checked = json::parse(scriptNodeErrorsJson(definition.dump()));
    for (const json& e : checked["errors"]) {
        if (!errors.empty()) errors += "\n";
        if (e.value("line", 0) > 0) errors += "line " + std::to_string(e["line"].get<int>()) + ", column " + std::to_string(e["column"].get<int>()) + ": ";
        errors += e.value("message", std::string());
    }
    if (!errors.empty()) return errors;
    auto defaults = [](const json& node) {
        std::map<std::string, json> out;
        for (const json& p : json::parse(nodeSpecJson(node.dump())).value("params", json::array())) out[p.value("name", std::string())] = p.value("default", json());
        return out;
    };
    auto names = [](const json& d) {
        std::set<std::string> out;
        for (const json& p : d.value("params", json::array())) {
            if (p.is_object() && p.value("name", json()).is_string()) out.insert(p["name"].get<std::string>());
        }
        return out;
    };
    return editGraph("edit " + node, [&](json& g) {
        for (json& item : g["nodes"]) {
            if (item.value("name", std::string()) != node) continue;
            const std::map<std::string, json> before = defaults(item);
            json& params = item["params"];
            const std::set<std::string> oldNames = names(params.value("definition", json::object())), newNames = names(definition);
            params["definition"] = definition;
            for (const std::string& gone : oldNames) {
                if (newNames.count(gone)) continue;
                params.erase(gone);
                if (item.contains("paramInputs") && item["paramInputs"].is_array()) {
                    json kept = json::array();
                    for (const json& p : item["paramInputs"]) {
                        if (p != gone) kept.push_back(p);
                    }
                    item["paramInputs"] = kept;
                }
                json links = json::array();
                for (const json& c : g.value("connections", json::array())) {
                    if (c.value("to", std::string()) != node || c.value("input", std::string()) != gone) links.push_back(c);
                }
                g["connections"] = links;
            }
            // A param still at its old default takes the new one.
            for (const auto& [name, value] : defaults(item)) {
                if (!newNames.count(name)) continue;
                const auto old = before.find(name);
                if (!params.contains(name) || (old != before.end() && params[name] == old->second)) params[name] = value;
            }
        }
    });
}

std::string GroomDocument::saveScriptNode(const std::string& node, bool asNew, std::string* path) {
    const json* n = nodeJson(node);
    if (!n || n->value("type", std::string()) != "Script") return node + " is not a script node";
    json definition = n->value("params", json::object()).value("definition", json::object());
    const std::string id = definition.value("id", std::string());
    const std::optional<LibraryNode> existing = findLibraryNode(id);
    if (asNew) {
        if (existing) return "the library has a node " + id + " already: give the new node another id";
        definition["version"] = 1;
    } else if (existing) {
        definition["version"] = std::max(definition.value("version", 1), existing->version + 1);
    }
    try {
        const std::string saved = saveNodeFile(definition.dump(2));
        if (path) *path = saved;
    } catch (const std::exception& e) {
        return e.what();
    }
    refreshLibrary();
    if (definition == n->value("params", json::object()).value("definition", json::object())) return {};
    return setScriptDefinition(node, definition);
}

std::string GroomDocument::tidyLayout() {
    return editGraph("tidy layout", [&](json& g) {
        GraphEdit edit(g.dump());
        edit.clearPositions();
        g = json::parse(edit.toJson());
    });
}

std::string GroomDocument::collapseNodes(const std::vector<std::string>& nodes, std::string* made) {
    if (nodes.empty()) return "select the nodes to group";
    std::string name;
    std::vector<std::pair<std::string, std::string>> moved;  // each node into the compound: its sculpt edits follow
    const std::string error = editGraph("group " + std::to_string(nodes.size()) + (nodes.size() == 1 ? " node" : " nodes"), [&](json& g) {
        GraphEdit edit(g.dump());
        name = edit.collapse(nodes);
        g = json::parse(edit.toJson());
        moved = edit.movedNodes();
    }, moved);
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::expandNodes(const std::vector<std::string>& compounds, std::vector<std::string>* made) {
    std::vector<std::string> groups;
    for (const std::string& n : compounds) {
        const json* node = nodeJson(n);
        if (node && node->value("type", std::string()) == "Compound") groups.push_back(n);
    }
    if (groups.empty()) return "select the Compound nodes to ungroup";
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> moved;  // each node out of its compound: its sculpt edits follow
    const std::string error = editGraph(groups.size() == 1 ? "ungroup " + groups.front() : "ungroup " + std::to_string(groups.size()) + " compounds",
                                        [&](json& g) {
                                            GraphEdit edit(g.dump());
                                            for (const std::string& n : groups) {
                                                for (std::string& inner : edit.expand(n)) names.push_back(std::move(inner));
                                            }
                                            g = json::parse(edit.toJson());
                                            moved = edit.movedNodes();
                                        }, moved);
    if (error.empty() && made) *made = names;
    return error;
}

namespace {

json* backdropAt(json& graph, int index) {
    if (!graph.contains("backdrops") || !graph["backdrops"].is_array()) return nullptr;
    json& list = graph["backdrops"];
    if (index < 0 || index >= static_cast<int>(list.size()) || !list[index].is_object()) return nullptr;
    return &list[index];
}

}  // namespace

std::string GroomDocument::addBackdrop(const std::string& title, double x, double y, double w, double h, int* index) {
    int made = -1;
    const std::string error = editGraph("add backdrop", [&](json& g) {
        if (!g.contains("backdrops") || !g["backdrops"].is_array()) g["backdrops"] = json::array();
        g["backdrops"].push_back({{"title", title.empty() ? std::string("Backdrop") : title},
                                  {"x", x},
                                  {"y", y},
                                  {"w", std::max(120.0, w)},
                                  {"h", std::max(80.0, h)},
                                  {"color", json::array({kBackdropColor[0], kBackdropColor[1], kBackdropColor[2]})}});
        made = static_cast<int>(g["backdrops"].size()) - 1;
    });
    if (error.empty() && index) *index = made;
    return error;
}

std::string GroomDocument::setBackdropTitle(int index, const std::string& title) {
    if (!backdropAt(graph_, index)) return "no backdrop " + std::to_string(index);
    return editGraph("rename backdrop", [&](json& g) { (*backdropAt(g, index))["title"] = title; });
}

std::string GroomDocument::setBackdropColor(int index, int r, int g, int b) {
    if (!backdropAt(graph_, index)) return "no backdrop " + std::to_string(index);
    return editGraph("backdrop color", [&](json& graph) {
        (*backdropAt(graph, index))["color"] = json::array({std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)});
    });
}

std::string GroomDocument::removeBackdrop(int index) {
    if (!backdropAt(graph_, index)) return "no backdrop " + std::to_string(index);
    return editGraph("delete backdrop", [&](json& g) {
        g["backdrops"].erase(g["backdrops"].begin() + index);
        if (g["backdrops"].empty()) g.erase("backdrops");
    });
}

void GroomDocument::setBackdropRect(int index, double x, double y, double w, double h) {
    json* b = backdropAt(graph_, index);
    if (!b) return;
    (*b)["x"] = x;
    (*b)["y"] = y;
    (*b)["w"] = std::max(120.0, w);
    (*b)["h"] = std::max(80.0, h);
    dirty_ = true;
}

// ---- Pins (graph_pins.h) ------------------------------------------------------------------------

std::string GroomDocument::addPin(const std::string& to, const std::string& input, double x, double y, int index, std::string* made) {
    std::string pin;
    const std::string error = editGraph("add pin", [&](json& g) {
        pin = groomlab::addPin(g, to, input, x, y, index);
        if (pin.empty()) throw std::invalid_argument("nothing feeds " + to + "." + input + " to pin");
    });
    if (error.empty() && made) *made = pin;
    return error;
}

std::string GroomDocument::removePin(const std::string& pin) {
    return editGraph("remove pin", [&](json& g) {
        if (!groomlab::removePin(g, pin)) throw std::invalid_argument("no pin '" + pin + "'");
    });
}

std::string GroomDocument::mergePins(const std::string& keep, const std::string& drop) {
    return editGraph("join pins", [&](json& g) {
        if (!groomlab::mergePins(g, keep, drop)) throw std::invalid_argument("those pins carry different links' signals");
    });
}

std::string GroomDocument::linkFromPin(const std::string& pin, const std::string& to, const std::string& input) {
    const std::optional<PinSource> source = pinSource(graph_, pin);
    if (!source) return "no pin '" + pin + "'";
    const std::vector<std::string> via = pinRouteTo(graph_, pin);
    return editGraph("connect " + source->from + " to " + to, [&](json& g) {
        GraphEdit edit(g.dump());
        edit.connectByHand(source->from, to, input, source->output);
        g = json::parse(edit.toJson());
        setPinRoute(g, to, input, via);
    });
}

std::string GroomDocument::feedPin(const std::string& pin, const std::string& from, const std::string& output) {
    const std::vector<PinLink> links = pinLinks(graph_, pin);
    if (links.empty()) return "no pin '" + pin + "'";
    return editGraph("feed pin from " + from, [&](json& g) {
        // Each link through the pin from `from` now, on through the pin and the ones after it; one
        // that can't take it throws (the core's checks): nothing changes.
        std::vector<std::pair<PinLink, std::vector<std::string>>> routes;
        for (const PinLink& link : links) {
            std::vector<std::string> via = pinRoute(g, link.to, link.input);
            via.erase(via.begin(), via.begin() + static_cast<std::ptrdiff_t>(std::min(link.index, via.size())));
            routes.emplace_back(link, std::move(via));
        }
        GraphEdit edit(g.dump());
        for (const auto& [link, via] : routes) edit.connectByHand(from, link.to, link.input, output);
        g = json::parse(edit.toJson());
        for (const auto& [link, via] : routes) setPinRoute(g, link.to, link.input, via);
    });
}

void GroomDocument::movePin(const std::string& pin, double x, double y) {
    if (groomlab::movePin(graph_, pin, x, y)) dirty_ = true;
}

// ---- The stored texts (parts, flow, correctives) ------------------------------------------------

std::string GroomDocument::storedText(StrokeField field) const {
    SessionStrokeHost reader(const_cast<GroomSession&>(session()));
    return reader.storedText(field);
}

std::string GroomDocument::editStoredText(const std::string& label, StrokeField field, const std::function<void(std::string&)>& edit) {
    std::string text = storedText(field);
    const std::string before = text;
    try {
        edit(text);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (text == before) return {};
    // As a stroke's edit of the field: the text before recorded, the session takes the text
    // after, one undo step named `label`.
    StrokeEdit stroke;
    stroke.mode = label;
    stroke.set(field, std::move(text));
    commitStroke(std::move(stroke));
    return {};
}

std::string GroomDocument::transferToScalp(MeshPtr scalp, bool fit, bool maps, std::vector<std::string>* warnings) {
    TransferOptions options;
    options.method = fit ? TransferMethod::Bounds : TransferMethod::Closest;
    return transferToScalp(std::move(scalp), options, maps, warnings);
}

std::string GroomDocument::transferToScalp(MeshPtr scalp, const TransferOptions& given, bool maps,
                                           std::vector<std::string>* warnings) {
    if (!scalp || scalp->triangleCount() == 0) return "the new scalp has no triangles";
    const MeshPtr from = scene_->file().data.scalp;
    if (!from || from->triangleCount() == 0) return "the groom has no scalp to move from";
    TransferOptions options = given;
    // Fitting the size: the groom's scale grows the guides about their roots; the mapping only turns them.
    if (options.fitSize && session().scaleWithScalp()) options.keepLengths = true;
    if (usesMarkers(options.method) && options.markers.empty()) options.markers = transferMarkers();
    std::unique_ptr<ScalpMapping> mapping;
    try {
        if (!options.markers.empty()) options.markers = snapTransferMarkers(options.markers, *from, *scalp);
        mapping = std::make_unique<ScalpMapping>(*from, *scalp, options, warnings);
    } catch (const std::exception& e) {
        return e.what();
    }
    commitPreview();
    GroomFile file = currentFile();
    if (!options.markers.empty()) file.transferMarkers = options.markers;
    ScalpMaps projected = session().scalpMaps();
    try {
        // The guides, parts and flow re-rooted (the core's transfer, Maya's groomLabTransfer's).
        if (!file.data.hierarchyJson.empty()) {
            GuideHierarchy h = GuideHierarchy::fromJson(file.data.hierarchyJson);
            h.setScale(static_cast<float>(session().groomScale()));  // moved as shown: the groom keeps its scale
            h.transfer(*mapping);
            file.data.hierarchyJson = h.toJson();
        }
        if (!file.data.partsJson.empty()) {
            const PartSet parts = PartSet::fromJson(file.data.partsJson);
            const PartSet out = parts.transferred(*mapping);
            if (out.parts.size() < parts.parts.size() && warnings) warnings->push_back("some parts fell apart on the new scalp and were dropped");
            file.data.partsJson = out.empty() ? std::string() : out.toJson();
        }
        if (!file.data.flowJson.empty()) {
            const FlowSet flow = FlowSet::fromJson(file.data.flowJson);
            const FlowSet out = flow.transferred(*mapping);
            if ((out.pins.size() < flow.pins.size() || out.sinks.size() < flow.sinks.size()) && warnings) {
                warnings->push_back("some flow arrows did not fit the new scalp and were dropped");
            }
            file.data.flowJson = out.empty() ? std::string() : out.toJson();
        }
        if (mapping->uvMisses() > 0 && warnings) {
            warnings->push_back(std::to_string(mapping->uvMisses()) +
                                " roots found no matching UV on the new scalp and went to the closest point");
        }
        // The maps through the same mapping: a texture map's new image beside the groom (the groom saved).
        if (maps) {
            MapProjectionFiles files;
            files.resolve = [this](const std::string& stored) { return resolveMapPath(stored); };
            const std::string folder = mapsDirectory();
            if (!folder.empty()) {
                files.write = [this, folder](const std::string& name, const TexturePaint& paint) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
                    std::string target = folder + "/" + name + ".png";
                    for (int k = 2; std::filesystem::exists(std::filesystem::u8path(target), ec); ++k) {
                        target = folder + "/" + name + "_" + std::to_string(k) + ".png";
                    }
                    paint.save(target);
                    return pathFromGroomFolder(target, std::filesystem::u8path(path_).parent_path().u8string());
                };
            }
            projectScalpMaps(projected, {}, *mapping, files, warnings);
        } else {
            // Not projected: the vertex maps cannot fit another scalp, so they go; textures stay.
            for (auto it = projected.maps.begin(); it != projected.maps.end();) {
                if (it->second.kind == "vertices") {
                    if (warnings) warnings->push_back("the vertex map " + it->first + " was dropped (not projected)");
                    it = projected.maps.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    // The new scalp with the projected vertex maps as its color sets, as the file keeps them.
    auto mesh = std::make_shared<ScalpMesh>(scalp->points(), scalp->triangles());
    if (scalp->hasUVs()) mesh->setCornerUVs(scalp->cornerUVs());
    mesh->setPolygons(scalp->polygons());  // what Catmull-Clark refines
    // Scalp Subdivision as it was (the new scalp's own level when the old one had none).
    if (from->renderSubdivision() > 0) mesh->setRenderSubdivision(from->renderSubdivision(), from->renderSubdivisionFrom());
    else mesh->setRenderSubdivision(scalp->renderSubdivision(), scalp->renderSubdivisionFrom());
    for (const std::string& set : scalp->colorSetNames()) {
        if (const auto* colors = scalp->cornerColors(set)) mesh->setCornerColors(set, *colors);
    }
    for (const auto& [name, entry] : projected.maps) {
        if (entry.kind != "vertices") continue;
        const std::size_t per = entry.channels == kColorMap ? 4 : 1;
        if (entry.values.size() != mesh->pointCount() * per) continue;
        mesh->setCornerColors(name, cornerColorsFromVertexMap(entry.values, *mesh, static_cast<int>(per)));
    }
    MapCatalog catalog;
    for (const auto& [name, info] : file.data.maps) {
        if (info.kind == "guides") catalog[name] = info;
    }
    for (const auto& [name, info] : projected.catalog()) catalog[name] = info;
    file.data.maps = catalog;
    file.data.scalp = mesh;
    file.scalpCache = {};  // another topology: the caches cannot follow
    file.guideCache = {};
    file.simCaches.clear();  // nor the simulation's bakes (made on the other scalp)
    file.data.followScalp = false;
    // The groom keeps the scale it had (the guides moved as shown): the new scalp is k times its 1x.
    // Fitting the size: k grows with the area (the new scalp the old one resized).
    double target = session().groomScale();
    if (options.fitSize && session().scaleWithScalp()) {
        const double before = restSurfaceArea(*from), after = restSurfaceArea(*mesh);
        if (before > 0.0 && after > 0.0) target *= std::sqrt(after / before);
    }
    file.data.scaleReference = restSurfaceArea(*mesh) / (target * target);
    file.data.scalpFingerprint = mesh->topologyFingerprint();  // the groom is on this scalp now
    try {
        scene_ = std::make_unique<GroomScene>(std::move(file));
    } catch (const std::exception& e) {
        return e.what();
    }
    transferMarkers_.reset();  // in the new scene's file
    referencePhotos_.reset();
    loadGraph(scene_->file().data.graphJson);
    loadMaps();
    paints_.clear();
    mapViewValues_.reset();
    mapViewKey_ = 0;
    selection_.clear();
    ++selectionVersion_;
    edits_.clear();  // the scalp itself changed: nothing before it can come back
    undoIndex_ = 0;
    dirty_ = true;
    return {};
}
const std::vector<TransferMarker>& GroomDocument::transferMarkers() const {
    return transferMarkers_ ? *transferMarkers_ : scene_->file().transferMarkers;
}

void GroomDocument::setTransferMarkers(std::vector<TransferMarker> markers) {
    transferMarkers_ = std::move(markers);
    dirty_ = true;
}

const std::string& GroomDocument::referencePhotos() const {
    return referencePhotos_ ? *referencePhotos_ : scene_->file().referencePhotos;
}

void GroomDocument::setReferencePhotos(std::string json) {
    if (json == referencePhotos()) return;
    referencePhotos_ = std::move(json);
    dirty_ = true;
}

std::vector<GroomDocument::NodeInfo> GroomDocument::nodes() const {
    std::vector<NodeInfo> out;
    if (!graph_.contains("nodes")) return out;
    for (const json& node : graph_["nodes"]) {
        NodeInfo info;
        info.name = node.value("name", std::string());
        info.type = node.value("type", std::string());
        info.bypass = node.value("bypass", false);
        info.evaluation = node.value("evaluation", std::string("always"));
        out.push_back(std::move(info));
    }
    return out;
}

json* GroomDocument::nodeJson(const std::string& node) {
    if (!graph_.contains("nodes")) return nullptr;
    for (json& n : graph_["nodes"]) {
        if (n.value("name", std::string()) == node) return &n;
    }
    return nullptr;
}

const json* GroomDocument::nodeJson(const std::string& node) const {
    return const_cast<GroomDocument*>(this)->nodeJson(node);
}

json GroomDocument::param(const std::string& node, const std::string& name) const {
    if (preview_.held && preview_.node == node && preview_.name == name) return preview_.value;  // a drag's value, previewed elsewhere
    const json* n = nodeJson(node);
    if (!n || !n->contains("params")) return nullptr;
    const json& params = (*n)["params"];
    return params.contains(name) ? params[name] : json(nullptr);
}

json GroomDocument::paramOrDefault(const std::string& node, const std::string& name) const {
    json value = param(node, name);
    if (!value.is_null()) return value;
    if (const json* spec = nodeSpec(node)) {
        for (const json& p : spec->value("params", json::array())) {
            if (p.value("name", std::string()) == name) return p.value("default", json(nullptr));
        }
    }
    return nullptr;
}

const json* GroomDocument::spec(const std::string& type) const {
    const auto it = specs_.find(type);
    return it == specs_.end() ? nullptr : &*it;
}

const json* GroomDocument::nodeSpec(const std::string& node) const {
    const json* n = nodeJson(node);
    if (!n) return nullptr;
    const std::string type = n->value("type", std::string());
    const bool exposes = n->contains("paramInputs") && (*n)["paramInputs"].is_array() && !(*n)["paramInputs"].empty();
    if (type != "Compound" && type != "Script" && type != "Input" && type != "Value" && type != "Expression" && !exposes) {
        return spec(type);
    }
    // A Compound's params are the ones its definition exposes (an Input's output its type, a
    // Value's and an Expression's output by their params; exposed params add ports): the core
    // resolves them from the node's own JSON (graph_edit.h's nodeSpecJson), cached by that JSON,
    // so an edit of the node gives a fresh spec.
    const std::string key = n->dump();
    auto it = resolvedSpecs_.find(key);
    if (it == resolvedSpecs_.end()) {
        if (resolvedSpecs_.size() > 64) resolvedSpecs_.clear();  // a small cache: the nodes shown lately
        try {
            it = resolvedSpecs_.emplace(key, json::parse(groomlab::nodeSpecJson(key))).first;
        } catch (const std::exception&) {
            return spec(type);
        }
    }
    return &it->second;
}

std::vector<std::string> GroomDocument::operatorTypes() const {
    std::vector<std::string> types;
    for (const auto& [type, spec] : specs_.items()) types.push_back(type);
    std::sort(types.begin(), types.end());
    return types;
}

std::string GroomDocument::paramInputSource(const std::string& node, const std::string& name) const {
    for (const ConnectionInfo& c : connections()) {
        if (c.to == node && c.input == name) return c.from;
    }
    return {};
}

bool GroomDocument::paramExposed(const std::string& node, const std::string& name) const {
    const json* n = nodeJson(node);
    if (!n || !n->contains("paramInputs") || !(*n)["paramInputs"].is_array()) return false;
    for (const json& p : (*n)["paramInputs"]) {
        if (p.is_string() && p.get<std::string>() == name) return true;
    }
    return false;
}

std::string GroomDocument::exposeParam(const std::string& node, const std::string& name, bool exposed) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (paramExposed(node, name) == exposed) return {};
    return exposeParams(node, {name}, exposed);
}

std::string GroomDocument::exposeParams(const std::string& node, const std::vector<std::string>& names, bool exposed) {
    if (!nodeJson(node)) return "no node '" + node + "'";
    if (names.empty()) return {};
    const std::set<std::string> changed(names.begin(), names.end());
    std::map<std::string, std::size_t> rank;
    if (const json* spec = nodeSpec(node)) {
        std::size_t i = 0;
        for (const json& p : spec->value("params", json::array())) rank[p.value("name", std::string())] = i++;
    }
    return editGraph((exposed ? "expose inputs on " : "collapse inputs on ") + node, [&](json& g) {
        for (json& n : g["nodes"]) {
            if (n.value("name", std::string()) != node) continue;
            json inputs = json::array();
            for (const json& p : n.value("paramInputs", json::array())) {
                if (!(p.is_string() && changed.count(p.get<std::string>()))) inputs.push_back(p);
            }
            if (exposed) {
                for (const std::string& name : names) inputs.push_back(name);
                std::stable_sort(inputs.begin(), inputs.end(), [&](const json& a, const json& b) {
                    const auto ai = rank.find(a.get<std::string>()), bi = rank.find(b.get<std::string>());
                    return (ai == rank.end() ? rank.size() : ai->second) < (bi == rank.end() ? rank.size() : bi->second);
                });
            }
            if (inputs.empty()) n.erase("paramInputs");
            else n["paramInputs"] = inputs;
        }
        if (!exposed) {
            // The port goes with what fed it.
            json connections = json::array();
            for (const json& c : g.value("connections", json::array())) {
                if (!(c.value("to", std::string()) == node && changed.count(c.value("input", std::string())))) connections.push_back(c);
            }
            g["connections"] = connections;
        }
    });
}

std::vector<std::string> GroomDocument::colorSetNames() const {
    std::vector<std::string> names;
    const MeshPtr& scalp = scene_->file().data.scalp;
    if (!scalp) return names;
    const ScalpMaps& maps = scene_->session().scalpMaps();
    for (const std::string& set : scalp->colorSetNames()) {
        if (!maps.maps.count(set)) names.push_back(set);
    }
    return names;
}

std::vector<std::string> GroomDocument::strandChannelNames() const {
    const ViewResult& view = lastView();
    return view.strands ? view.strands->strandChannelNames() : std::vector<std::string>{};
}

std::string GroomDocument::applyGraph() {
    try {
        session().setGraphJson(graph_.dump());
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

std::string GroomDocument::replaceGraph(const std::string& graphJson) {
    commitPreview();
    const json before = graph_;
    try {
        loadGraph(graphJson);
    } catch (const std::exception& e) {
        graph_ = before;
        return e.what();
    }
    const std::string error = applyGraph();
    if (!error.empty()) {
        graph_ = before;
        applyGraph();
        return error;
    }
    edits_.clear();
    undoIndex_ = 0;
    dirty_ = true;
    return {};
}

std::string GroomDocument::setParam(const std::string& node, const std::string& name, const json& value, bool record) {
    json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    // The value's kind against the spec's: the graph's loader takes a text where a number is
    // expected (an expression map), so a panel's slip would silently turn a number into a map.
    if (const json* spec = nodeSpec(node)) {
        for (const json& p : spec->value("params", json::array())) {
            if (p.value("name", std::string()) != name) continue;
            const std::string kind = p.value("kind", std::string());
            // A mappable number may hold a map instead (an object with its source: map_ref.h).
            const bool mapped = p.value("mappable", false) && value.is_object();
            const bool fits = (kind == "bool" && value.is_boolean()) || (kind == "int" && (value.is_number_integer() || mapped)) ||
                              (kind == "float" && (value.is_number() || mapped)) || (kind == "string" && value.is_string()) ||
                              (kind == "vec3" && value.is_array() && value.size() == 3) || kind == "ramp" || kind == "map";
            if (!fits) return node + "." + name + " takes a " + kind + (p.value("mappable", false) ? " or a map" : "");
        }
    }
    // A drag's step the host previews elsewhere: held, the graph untouched. A drag of another
    // param, or an edit of this one that is not such a step, applies what was held first.
    if (preview_.held && !(!record && preview_.node == node && preview_.name == name)) commitPreview();
    if (!record && livePreview && (value.is_number() || value.is_array() || value.is_object()) && livePreview(node, name, value)) {
        preview_ = Preview{true, node, name, value};
        dirty_ = true;
        return {};
    }
    if (preview_.held) {
        // The same param, previewed until now, taken back by the host: applied on the CPU from here.
        preview_ = Preview{};
        if (livePreviewEnded) livePreviewEnded();
    }
    return applyParam(node, name, value, record);
}

void GroomDocument::commitPreview() {
    if (!preview_.held) return;
    const Preview p = preview_;
    preview_ = Preview{};
    if (livePreviewEnded) livePreviewEnded();
    applyParam(p.node, p.name, p.value, false);
}

std::string GroomDocument::applyParam(const std::string& node, const std::string& name, const json& value, bool record) {
    json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    json& params = (*n)["params"];
    if (!params.is_object()) params = json::object();
    const json before = params.contains(name) ? params[name] : json(nullptr);
    if (before == value) return {};
    params[name] = value;
    const std::string error = applyGraph();
    if (!error.empty()) {
        if (before.is_null()) {
            params.erase(name);
        } else {
            params[name] = before;
        }
        applyGraph();
        return error;
    }
    dirty_ = true;
    if (record) recordEdit(node, name, before, value);
    return {};
}

void GroomDocument::record(Edit edit) {
    edits_.resize(undoIndex_);  // what could be redone is gone
    edits_.push_back(std::move(edit));
    undoIndex_ = edits_.size();
}

void GroomDocument::recordEdit(const std::string& node, const std::string& name, const json& before, const json& after) {
    commitPreview();  // a drag previewed elsewhere ends here: its value applied for real
    if (before == after) return;
    record({node, name, before, after, false, nullptr, {}});
}

void GroomDocument::commitStroke(StrokeEdit edit) {
    if (edit.empty()) return;
    // An edit that rewires the graph (core paintParamMap: a setting's map made and linked): the
    // document's graph takes it, the step undoing both.
    std::optional<json> graphAfter;
    if (edit.text(StrokeField::Graph).set) {
        json after = json::parse(edit.text(StrokeField::Graph).after, nullptr, false);
        edit.text(StrokeField::Graph) = StrokeEdit::Text{};
        if (!after.is_discarded() && after.is_object()) graphAfter = std::move(after);
    }
    if (graphAfter) commitPreview();
    // The texts before, read as the session's host reads them (guides handed over as a
    // hierarchy have no text of their own), so undo puts back exactly what was there.
    SessionStrokeHost reader(session());
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        StrokeEdit::Text& t = edit.texts[i];
        if (!t.set) continue;
        t.before = reader.storedText(static_cast<StrokeField>(i));
        t.hasBefore = true;
    }
    edit.apply(session(), true);
    // A texture map's stroke painted its tiles into the paint already: the file takes them now
    // (the graph reads the file, so the strands follow on the next evaluation).
    if (!edit.textureMap.empty()) writeTexturePaint(edit.textureMap);
    Edit entry;
    entry.label = edit.mode.empty() ? "stroke" : edit.mode + " stroke";
    if (graphAfter) {
        entry.whole = true;
        entry.graphBefore = graph_;
        entry.graphAfter = *graphAfter;
        graph_ = std::move(*graphAfter);
        applyGraph();
    }
    entry.stroke = std::make_shared<const StrokeEdit>(std::move(edit));
    record(std::move(entry));
    dirty_ = true;
}

std::string GroomDocument::editGuides(const std::string& label, const std::function<void(GuideHierarchy&)>& edit) {
    const HierarchyPtr authored = session().authoredHierarchy();
    GuideHierarchy h = authored ? *authored : GuideHierarchy{};
    try {
        edit(h);
        // New guides take their shapes from their parents, moved roots bind again; what was
        // untouched is rebuilt from its root frame to the same points.
        h.evaluate(session().scalp().get());
    } catch (const std::exception& e) {
        return e.what();
    }
    StrokeEdit stroke;
    stroke.mode = label;
    stroke.set(StrokeField::Guides, h.toJson());
    // Guides that went take their selection with them.
    if (!selection_.empty()) {
        std::vector<GuideId> kept;
        for (const GuideId id : selection_) {
            if (h.guide(id)) kept.push_back(id);
        }
        if (kept.size() != selection_.size()) setSelection(std::move(kept));
    }
    commitStroke(std::move(stroke));
    edits_.back().label = label;
    return {};
}

void GroomDocument::setSelection(std::vector<GuideId> ids) {
    if (ids == selection_) return;
    selection_ = std::move(ids);
    ++selectionVersion_;
}

void GroomDocument::setGroupHidden(const std::string& name, bool hidden) {
    // Saved with the groom ("hiddenGuideGroups"), as Maya keeps it on the groom.
    const bool changed = hidden ? hiddenGroups_.insert(name).second : hiddenGroups_.erase(name) > 0;
    if (changed) dirty_ = true;
}

std::string GroomDocument::hiddenGroupsText() const {
    std::string text;
    for (const std::string& name : hiddenGroups_) text += (text.empty() ? "" : ",") + name;
    return text;
}

std::string GroomDocument::setBypass(const std::string& node, bool bypass) {
    commitPreview();
    json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    const bool before = n->value("bypass", false);
    if (before == bypass) return {};
    (*n)["bypass"] = bypass;
    const std::string error = applyGraph();
    if (!error.empty()) {
        (*n)["bypass"] = before;
        applyGraph();
        return error;
    }
    dirty_ = true;
    edits_.resize(undoIndex_);
    edits_.push_back({node, "bypass", before, bypass, true});
    undoIndex_ = edits_.size();
    return {};
}

std::string GroomDocument::setEvaluation(const std::string& node, const std::string& mode) {
    try {
        evaluationModeFromName(mode);
    } catch (const GraphError& e) {
        return e.what();
    }
    const json* n = nodeJson(node);
    if (!n) return "no node '" + node + "'";
    if (n->value("evaluation", std::string("always")) == mode) return {};
    const char* labels[] = {"Evaluate Always", "Evaluate in the Viewport Only", "Evaluate in Renders Only"};
    const auto which = static_cast<std::size_t>(evaluationModeFromName(mode));
    return editGraph(node + ": " + labels[which], [&](json& graph) {
        for (json& entry : graph["nodes"]) {
            if (entry.value("name", std::string()) != node) continue;
            if (mode == "always") {
                entry.erase("evaluation");
            } else {
                entry["evaluation"] = mode;
            }
        }
    });
}

std::string GroomDocument::editName(const Edit& edit) {
    return edit.stroke || edit.whole || edit.side ? edit.label : edit.node + "." + edit.name;
}

std::string GroomDocument::undoText() const { return canUndo() ? "Undo " + editName(edits_[undoIndex_ - 1]) : std::string(); }

std::string GroomDocument::redoText() const { return canRedo() ? "Redo " + editName(edits_[undoIndex_]) : std::string(); }

std::string GroomDocument::undo() {
    commitPreview();
    if (!canUndo()) return {};
    const Edit& edit = edits_[undoIndex_ - 1];
    std::string error;
    if (edit.stroke) {
        edit.stroke->apply(session(), false);
        applyTextureEdit(*edit.stroke, false);
    }
    if (edit.stroke && !edit.whole) {
        // nothing more: the stroke's texts are back
    } else if (edit.whole) {
        graph_ = edit.graphBefore;
        error = applyGraph();
    } else if (edit.bypass) {
        if (json* n = nodeJson(edit.node)) {
            (*n)["bypass"] = edit.before.get<bool>();
            error = applyGraph();
        }
    } else if (json* n = nodeJson(edit.node)) {
        json& params = (*n)["params"];
        if (edit.before.is_null()) {
            params.erase(edit.name);
        } else {
            params[edit.name] = edit.before;
        }
        error = applyGraph();
    }
    applySide(edit, false);  // its files and group colors
    --undoIndex_;
    dirty_ = true;
    return error;
}

std::string GroomDocument::redo() {
    commitPreview();
    if (!canRedo()) return {};
    const Edit& edit = edits_[undoIndex_];
    std::string error;
    if (edit.stroke) {
        edit.stroke->apply(session(), true);
        applyTextureEdit(*edit.stroke, true);
    }
    if (edit.stroke && !edit.whole) {
        // nothing more: the stroke's texts are back
    } else if (edit.whole) {
        graph_ = edit.graphAfter;
        error = applyGraph();
    } else if (json* n = nodeJson(edit.node)) {
        if (edit.bypass) {
            (*n)["bypass"] = edit.after.get<bool>();
        } else {
            (*n)["params"][edit.name] = edit.after;
        }
        error = applyGraph();
    }
    applySide(edit, true);
    ++undoIndex_;
    dirty_ = true;
    return error;
}

void GroomDocument::setScalpMapDisplay(const std::string& name, const std::string& channel) {
    scalpMapName_ = name;
    scalpMapChannel_ = channel.empty() ? "rgb" : channel;
}

const DisplayFrame& GroomDocument::updateDisplay(const DisplaySettings& settings) {
    // The strand lines come as StrandLines the renderer writes straight into its buffers (whole,
    // or the changed strands' ranges), not as vectors filled here: the builder's share of a
    // dab or a tweak is then the items' bookkeeping alone.
    display_.setWriteLines(true);
    DisplayInputs inputs = DisplayInputs::fromView(lastView(), session().hasPreview());
    if (!scalpMapName_.empty()) inputs.scalpMap = scalpMapView(scalpMapName_, scalpMapChannel_);
    if (!selection_.empty()) {
        // A hard selection: every selected guide weighs 1 (asked only when it changed).
        const std::vector<GuideId>& ids = selection_;
        inputs.selection = [&ids] {
            std::map<GuideId, float> weights;
            for (const GuideId id : ids) weights[id] = 1.0f;
            return weights;
        };
        inputs.selectionHash = selectionVersion_;
    }
    const bool ownColors = !groupColors_.empty() && settings.guideGroupColors.empty();
    const std::string seed = settings.guideColorSeed.empty() ? guideColorSeed() : std::string();
    if (ownColors || !seed.empty()) {
        // The groups' own colors (the rest automatic: their level's, from the groom's own hue).
        DisplaySettings colored = settings;
        if (ownColors) colored.guideGroupColors = groupColorsText();
        if (!seed.empty()) colored.guideColorSeed = seed;
        return display_.update(inputs, colored);
    }
    return display_.update(inputs, settings);
}

std::string GroomDocument::guideColorSeed() const {
    // The file's name without its extension: Maya's groom "hair" exported as hair.groom starts its
    // guide colors from the same hue in the app ("" untitled: the theme's own colors).
    return path_.empty() ? std::string() : std::filesystem::path(path_).stem().string();
}

Bounds GroomDocument::sceneBounds() const {
    Bounds b = lastView().bounds;
    const MeshPtr& scalp = scene_->file().data.scalp;
    if (scalp && scalp->pointCount() > 0) {
        for (const Vec3f& p : scalp->points()) {
            if (!b.valid) {
                b.lo = b.hi = p;
                b.valid = true;
            } else {
                b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
                b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
            }
        }
    }
    return b;
}

GroomFile GroomDocument::currentFile() const {
    GroomFile file = scene_->file();
    if (transferMarkers_) file.transferMarkers = *transferMarkers_;
    if (referencePhotos_) file.referencePhotos = *referencePhotos_;
    // The groups' own colors and the hidden groups, in Maya's attributes' form (space separated).
    file.guideGroupColors = groupColorsText();
    file.hiddenGuideGroups.clear();
    for (const std::string& name : hiddenGroups_) file.hiddenGuideGroups += (file.hiddenGuideGroups.empty() ? "" : " ") + name;
    file.data.graphJson = graph_.dump();
    // The guides and overlays as the session holds them now: the strokes' and edits' results.
    const GroomData& data = session().data();
    file.data.controls = data.controls;  // the Controls sliders' values
    file.data.hierarchyJson = data.hierarchyJson;
    file.data.partsJson = data.partsJson;
    file.data.flowJson = data.flowJson;
    file.data.correctivesJson = data.correctivesJson;
    file.data.sculptsJson = data.sculptsJson;
    // The painted maps as the file keeps them: the catalog (the guide layers' entries as read,
    // the scalp maps as they are now) and the vertex maps as color sets of the scalp, a removed
    // map's color set gone with it (a color set that was never a map stays: the graph may read it).
    const ScalpMaps& maps = session().scalpMaps();
    MapCatalog catalog;
    for (const auto& [name, info] : fileCatalog_) {
        if (info.kind == "guides") catalog[name] = info;
    }
    for (const auto& [name, info] : maps.catalog()) catalog[name] = info;
    file.data.maps = catalog;
    if (file.data.scalp) {
        const ScalpMesh& was = *file.data.scalp;
        auto scalp = std::make_shared<ScalpMesh>(was.points(), was.triangles());
        if (was.hasUVs()) scalp->setCornerUVs(was.cornerUVs());
        if (was.hasRest()) scalp->setRestPoints(was.restPoints());
        scalp->setPolygons(was.polygons());  // what Catmull-Clark refines, and the render's level
        scalp->setRenderSubdivision(was.renderSubdivision(), was.renderSubdivisionFrom());
        for (const std::string& set : was.colorSetNames()) {
            const auto it = fileCatalog_.find(set);
            const bool wasMap = it != fileCatalog_.end() && it->second.kind == "vertices";
            if (wasMap && !maps.maps.count(set)) continue;
            if (const auto* colors = was.cornerColors(set)) scalp->setCornerColors(set, *colors);
        }
        // With other scalps (scalp_pieces.h) the vertex maps are per vertex of the scalps joined:
        // laid on the joined mesh, each scalp keeps its own share as its color sets.
        const MeshPtr joined = combineScalps(scalp, file.data.scalps);
        std::shared_ptr<ScalpMesh> mapped = joined == scalp ? scalp : std::make_shared<ScalpMesh>(*joined);
        for (const auto& [name, entry] : maps.maps) {
            if (entry.kind != "vertices") continue;
            const std::size_t per = entry.channels == kColorMap ? 4 : 1;
            if (entry.values.size() != mapped->pointCount() * per) continue;
            mapped->setCornerColors(name, cornerColorsFromVertexMap(entry.values, *mapped, static_cast<int>(per)));
        }
        if (mapped != scalp) {
            const auto pieces = scalpPieces(mapped);
            auto own = [&](const std::string& name, const MeshPtr& as) -> MeshPtr {
                const auto it = pieces.find(name);
                if (it == pieces.end()) return as;
                auto mesh = std::make_shared<ScalpMesh>(*as);
                for (const std::string& set : it->second->colorSetNames()) mesh->setCornerColors(set, *it->second->cornerColors(set));
                return mesh;
            };
            file.data.scalp = own(kMainScalpName, scalp);
            for (auto& [name, mesh] : file.data.scalps) mesh = own(name, mesh);
        } else {
            file.data.scalp = scalp;
        }
    }
    file.data.followScalp = data.followScalp;
    // Scaling with the scalp: the reference in use (the first area measured when the file had none).
    file.data.scaleSource = data.scaleSource;
    file.data.scaleReference = session().scaleReference();
    file.data.scaleTransformReference = data.scaleTransformReference;
    file.simCaches = simBakes_;
    return file;
}

void GroomDocument::save(const std::string& path) {
    GroomFile file = currentFile();
    MapCatalog catalog = file.data.maps;
    // The last bake (written or adopted), relative to the groom when it can be: another host
    // (Maya's import) takes its atlas and stamp with the groom.
    file.cardBake = {};
    if (bake_ && !bake_->prefix.empty()) {
        // As every host stores a path relative to a groom file (paths.h; the prefix as a file's).
        file.cardBake = {pathFromGroomFolder(bake_->prefix, std::filesystem::u8path(path).parent_path().u8string()), bake_->stamp};
    }
    // The simulation bakes the same way, relative to the groom.
    for (auto& [node, ref] : file.simCaches) ref.file = pathFromGroomFolder(ref.file, std::filesystem::u8path(path).parent_path().u8string());
    try {
        writeGroomFile(path, file);
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    const bool moved = path != path_;
    path_ = path;
    dirty_ = false;
    fileCatalog_ = catalog;
    if (moved) {
        // Relative paths (texture maps) resolve against the new folder first, then where the
        // groom came from and its search paths.
        std::vector<std::string> dirs{std::filesystem::u8path(path_).parent_path().u8string()};
        if (!scene_->file().directory.empty()) dirs.push_back(scene_->file().directory);
        dirs.insert(dirs.end(), scene_->file().searchPaths.begin(), scene_->file().searchPaths.end());
        session().setPathResolver(searchPathResolver(std::move(dirs)));
        paints_.clear();
    }
}

void GroomDocument::writeRenderGroom(const std::string& path, MeshPtr* scalp) {
    // The scalp as the hair grows on it: refined as the groom refines it (Scalp Subdivision).
    if (scalp) *scalp = session().refineAsScalp(scene_->rootScalpAt(time_));
    GroomFile file = currentFile();
    const std::string folder = std::filesystem::u8path(path).parent_path().u8string();
    file.cardBake = {};
    for (auto& [node, ref] : file.simCaches) ref.file = pathFromGroomFolder(ref.file, folder);
    // Relative paths (texture maps) as the document resolves them: its folder, then the file's.
    std::vector<std::string> dirs;
    if (!path_.empty()) dirs.push_back(std::filesystem::u8path(path_).parent_path().u8string());
    if (!scene_->file().directory.empty()) dirs.push_back(scene_->file().directory);
    for (const std::string& dir : dirs) {
        if (std::find(file.searchPaths.begin(), file.searchPaths.end(), dir) == file.searchPaths.end()) file.searchPaths.push_back(dir);
    }
    writeGroomFile(path, file);
}

std::size_t GroomDocument::exportAlembic(const std::string& path) {
    ExportOptions options;
    options.frames.first = options.frames.last = frame();
    return exportAlembic(path, options).strands;
}

std::vector<std::pair<std::string, std::string>> GroomDocument::nodeNotes() const {
    std::map<std::string, std::string> notes;
    for (const auto& [name, note] : scene_->nodeNotes()) notes[name] = note;
    // An Instance or Scales naming a mesh the groom file does not hold (the built-in plates always are).
    const auto& stored = file().prototypes;
    if (graph_.contains("nodes") && graph_["nodes"].is_array()) {
        for (const json& n : graph_["nodes"]) {
            const std::string type = n.value("type", std::string());
            if (type != "Instance" && type != "Scales") continue;
            const json params = n.value("params", json::object());
            const std::vector<std::string> missing = missingInstancePrototypes(
                params.value("prototype", std::string("instance")), [&](const std::string& mesh) {
                    const auto it = stored.find(mesh);
                    return it != stored.end() && it->second.found;
                });
            if (missing.empty()) continue;
            std::string names;
            for (const std::string& m : missing) names += (names.empty() ? "" : ", ") + m;
            std::string& note = notes[n.value("name", std::string())];
            note = "no mesh named " + names + " in the groom: its instances do not draw or render" +
                   (note.empty() ? "" : "; " + note);
        }
    }
    const json attributes = channels();
    for (const auto& [name, info] : attributes.items()) {
        for (const json& read : info.value("reads", json::array())) {
            if (read.value("reaches", false) || read.value("optional", false)) continue;
            std::string& note = notes[name];
            note += (note.empty() ? "" : "; ") + std::string("reads the attribute ") + read.value("name", std::string()) + " (" +
                    read.value("param", std::string()) + "), which no node upstream writes: it is ignored";
        }
    }
    return {notes.begin(), notes.end()};
}

std::vector<std::pair<std::string, std::string>> GroomDocument::nodeErrors() const {
    // As Maya's Graph Editor reads them: "node 'name': why" (the outermost node named: a
    // compound's inner node's error is the compound's).
    std::vector<std::pair<std::string, std::string>> errors;
    for (const std::string& line : lastView().errors) {
        const std::size_t at = line.find("node '");
        if (at == std::string::npos) continue;
        const std::size_t end = line.find("': ", at + 6);
        if (end == std::string::npos) continue;
        std::string name = line.substr(at + 6, end - at - 6);
        if (std::any_of(errors.begin(), errors.end(), [&](const auto& e) { return e.first == name; })) continue;
        errors.emplace_back(std::move(name), line.substr(end + 3));
    }
    return errors;
}

json GroomDocument::channels() const {
    std::vector<std::string> guideMaps;
    if (const HierarchyPtr h = scene_->session().authoredHierarchy()) guideMaps = h->attributeNames();
    try {
        return json::parse(graphChannelsJson(graph_.dump(), guideMaps));
    } catch (const std::exception&) {
        return json::object();
    }
}

}  // namespace groomlab::app
