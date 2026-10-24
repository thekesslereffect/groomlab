// SPDX-License-Identifier: Apache-2.0
// The pipeline's commands (pipeline.h; docs/BLENDER.md "Pipeline"): what Maya's groomlab.game,
// exports, cards, sim, collision, extra_scalps and transfer modules do, over the helper's session.
// Files are written here except FBX, which the host writes from the meshes answered (Maya writes
// them through Maya's FBX exporter; Blender's add-on through Blender's).

#include "pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <utility>

#include "groomlab/card_files.h"
#include "groomlab/card_presets.h"
#include "groomlab/cards.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/flow.h"
#include "groomlab/frame_time.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/png.h"
#include "groomlab/random.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/simulation.h"
#include "groomlab/strand_geometry.h"
#include "groomlab/transfer.h"
#include "groomlab/transfer_markers_edit.h"

#include <variant>

#if GROOMLAB_HAS_ALEMBIC
#include "groomlab/engine_export.h"
#include "groomlab/sim_cache.h"
#endif

#include "request.h"
#include "session.h"
#include "tools.h"

namespace groomlab::helper {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- Meshes answered to the host ----------------------------------------------------------------

// A mesh the host makes (and writes as FBX): polygons, and per face corner its uvs, normals, more
// uv sets and colors, in groom space until answered.
struct MeshOut {
    std::string key;
    std::vector<Vec3f> points;
    std::vector<std::int32_t> faceCounts, faceIndices;
    std::vector<float> uvs;                              // 2 per corner, or none
    std::vector<Vec3f> normals;                          // per corner, or none
    std::map<std::string, std::vector<float>> uvSets;    // 2 per corner
    std::map<std::string, std::vector<float>> colors;    // 4 per corner (linear RGBA)
    json info = json::object();
};

// The meshes into the session's meshes arena, points in the host's space (or groom space): each
// {"points", "faceCounts", "faceIndices", "uvs"?, "normals"?, "uvSets", "colors"} plus its info.
json putMeshes(Session& s, const std::vector<MeshOut>& meshes, bool groomSpace) {
    std::size_t bytes = 0;
    for (const MeshOut& m : meshes) {
        bytes += Arena::reserve(m.points.size() * 12) + Arena::reserve(m.faceCounts.size() * 4) +
                 Arena::reserve(m.faceIndices.size() * 4) + Arena::reserve(m.uvs.size() * 4) + Arena::reserve(m.normals.size() * 12);
        for (const auto& [name, v] : m.uvSets) bytes += Arena::reserve(v.size() * 4);
        for (const auto& [name, v] : m.colors) bytes += Arena::reserve(v.size() * 4);
    }
    s.meshes.begin(bytes);
    json out = json::object();
    for (const MeshOut& m : meshes) {
        json a = m.info;
        a["points"] = putPoints(s.meshes, m.points, s.space, groomSpace);
        a["faceCounts"] = put(s.meshes, m.faceCounts.data(), m.faceCounts.size(), 1);
        a["faceIndices"] = put(s.meshes, m.faceIndices.data(), m.faceIndices.size(), 1);
        a["corners"] = m.faceIndices.size();
        if (!m.uvs.empty()) a["uvs"] = put(s.meshes, m.uvs.data(), m.uvs.size() / 2, 2);
        if (!m.normals.empty()) {
            std::vector<float> n(m.normals.size() * 3);
            for (std::size_t i = 0; i < m.normals.size(); ++i) {
                if (groomSpace) {
                    n[3 * i] = m.normals[i].x;
                    n[3 * i + 1] = m.normals[i].y;
                    n[3 * i + 2] = m.normals[i].z;
                } else {
                    s.space.directionToHost(m.normals[i], &n[3 * i]);
                }
            }
            a["normals"] = put(s.meshes, n.data(), m.normals.size(), 3);
        }
        json sets = json::object();
        for (const auto& [name, v] : m.uvSets) sets[name] = put(s.meshes, v.data(), v.size() / 2, 2);
        a["uvSets"] = sets;
        json colors = json::object();
        for (const auto& [name, v] : m.colors) colors[name] = put(s.meshes, v.data(), v.size() / 4, 4);
        a["colors"] = colors;
        out[m.key] = std::move(a);
    }
    return out;
}

bool groomSpaceOf(const json& args) {
    const std::string space = optString(args, "space", "host");
    if (space != "host" && space != "groom") fail("bad_request", "space is \"host\" or \"groom\"");
    return space == "groom";
}

// The cards as Maya's card mesh has them: a quad per card segment, the atlas UVs, the strands'
// own normals, the tint as color set groomLabTint (half its value: 0.5 none), linear.
MeshOut cardsMesh(const CardMesh& cards, const std::string& key) {
    MeshOut m;
    m.key = key;
    m.points = cards.positions;
    const std::size_t quads = cards.quads.size() / 4;
    m.faceCounts.assign(quads, 4);
    m.faceIndices.reserve(cards.quads.size());
    std::vector<float>& tint = m.colors["groomLabTint"];
    for (const std::uint32_t v : cards.quads) {
        m.faceIndices.push_back(static_cast<std::int32_t>(v));
        m.uvs.push_back(v < cards.u.size() ? cards.u[v] : 0.0f);
        m.uvs.push_back(v < cards.v.size() ? cards.v[v] : 0.0f);
        m.normals.push_back(v < cards.normals.size() ? cards.normals[v] : Vec3f{0.0f, 1.0f, 0.0f});
        const std::array<float, 3> t = v < cards.tints.size() ? cards.tints[v] : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        tint.insert(tint.end(), {0.5f * t[0], 0.5f * t[1], 0.5f * t[2], 1.0f});
    }
    m.info = {{"cards", cards.cards.size()}, {"triangles", 2 * quads}};
    return m;
}

// The hair cap: the scalp's polygons pushed out along its normals by scalpCapLift (0.4% of its
// size), its UVs (Maya's scalp_cap: a polyMoveVertex on the scalp mesh).
MeshOut scalpCapMesh(const ScalpMesh& scalp, const std::string& key) {
    MeshOut m;
    m.key = key;
    const float lift = scalpCapLift(scalp);
    const std::vector<Vec3f>& normals = scalp.vertexNormals();
    m.points = scalp.points();
    for (std::size_t i = 0; i < m.points.size() && i < normals.size(); ++i) m.points[i] = m.points[i] + normals[i] * lift;
    bool exact = true;
    hostMeshFaces(scalp, m.faceCounts, m.faceIndices, m.uvs, exact);
    m.info = {{"lift", lift}};
    return m;
}

// A polygon mesh of RachisMesh's layout (rachis tubes, strands as a mesh): its UVs per corner.
MeshOut rachisLayoutMesh(const RachisMesh& r, const std::string& key) {
    MeshOut m;
    m.key = key;
    m.points = r.points;
    m.faceCounts.assign(r.faceCounts.begin(), r.faceCounts.end());
    m.faceIndices.assign(r.faceVertices.begin(), r.faceVertices.end());
    if (r.faceUVs.size() == r.faceVertices.size()) {
        m.uvs.reserve(2 * r.faceUVs.size());
        for (const int k : r.faceUVs) {
            const auto i = static_cast<std::size_t>(k);
            m.uvs.push_back(i < r.u.size() ? r.u[i] : 0.0f);
            m.uvs.push_back(i < r.v.size() ? r.v[i] : 0.0f);
        }
    }
    return m;
}

// The scales bent onto the skin as Maya's groomLabScaleMesh makes them: triangles, the plates'
// uvs, uv sets scaleUV (the skin's uv at each scale's root) and scaleData (its random, its
// variant), color sets scaleColor (per vertex) and scaleRootColor (per scale), its own normals.
MeshOut scalesMesh(const ScaleMesh& sm, const std::string& key) {
    MeshOut m;
    m.key = key;
    m.points = sm.points;
    const std::size_t faces = sm.triangleCount();
    m.faceCounts.assign(faces, 3);
    std::vector<std::uint32_t> owner(faces, 0);
    for (std::size_t k = 0; k < sm.scales.size(); ++k) {
        const ScaleMesh::Scale& sc = sm.scales[k];
        for (std::uint32_t f = sc.firstTriangle; f < sc.firstTriangle + sc.triangleCount && f < faces; ++f) owner[f] = static_cast<std::uint32_t>(k);
    }
    std::vector<float>& scaleUV = m.uvSets["scaleUV"];
    std::vector<float>& scaleData = m.uvSets["scaleData"];
    std::vector<float>& color = m.colors["scaleColor"];
    std::vector<float>& rootColor = m.colors["scaleRootColor"];
    for (std::size_t c = 0; c < sm.triangles.size(); ++c) {
        const std::uint32_t v = sm.triangles[c];
        m.faceIndices.push_back(static_cast<std::int32_t>(v));
        m.uvs.push_back(sm.uvs[v][0]);
        m.uvs.push_back(sm.uvs[v][1]);
        m.normals.push_back(sm.normals[v]);
        color.insert(color.end(), {sm.colors[v][0], sm.colors[v][1], sm.colors[v][2], 1.0f});
        if (sm.scales.empty()) continue;
        const ScaleMesh::Scale& sc = sm.scales[owner[c / 3]];
        scaleUV.insert(scaleUV.end(), {sc.rootUv[0], sc.rootUv[1]});
        scaleData.insert(scaleData.end(), {sc.random, static_cast<float>(sc.variant)});
        rootColor.insert(rootColor.end(), {sc.color[0], sc.color[1], sc.color[2], 1.0f});
    }
    if (sm.scales.empty()) {
        m.uvSets.clear();
        m.colors.erase("scaleRootColor");
    }
    m.info = {{"scales", sm.scales.size()}, {"triangles", faces}};
    return m;
}

// ---- Evaluations ------------------------------------------------------------------------------------

// The groom at full density at the session's time, as exports evaluate it (renderOnly nodes on),
// with the Cards nodes' cards when `cards`.
const ViewResult& evaluateNow(Session& s, bool cards) {
    ViewRequest request;
    request.density = 1.0f;
    request.time = s.seconds;
    request.wantBounds = false;
    request.wantCards = cards;
    request.cardDensity = 1.0f;
    feedSessionTime(s);
    const ViewResult& view = s.groom->evaluate(request);
    if (!view.errors.empty()) fail("evaluation", view.errors.front());
    if (!view.strands) fail("evaluation", "the groom gave no strands");
    return view;
}

MeshPtr scalpOf(Session& s, const ViewResult& view) { return view.scalp ? view.scalp : s.groom->scalp(); }

CardBake bakeCardsNow(Session& s, int resolution, float occlusion) {
    const ViewResult& view = evaluateNow(s, true);
    if (!view.cardStrands) fail("state", "the graph has no Cards node: add one first");
    try {
        return bakeCardsInMemory(view.cardStrands, scalpOf(s, view), resolution, occlusion);
    } catch (const std::exception& e) {
        fail("export", e.what());
    }
}

// The graph's own Cards nodes (not those inside compounds), in graph order.
std::vector<std::string> cardsNodes(const json& graph) {
    std::vector<std::string> names;
    for (const json& n : graph.value("nodes", json::array())) {
        if (n.is_object() && n.value("type", std::string()) == "Cards") names.push_back(n.value("name", std::string()));
    }
    return names;
}

// A copy of the graph with every Cards node set for level of detail `lod` (Maya's cards_preset
// into a copy: hero, mid, low; each node by what it cards, card_presets.h).
std::string graphAtLod(const std::string& graphText, const std::string& lod) {
    json graph = json::parse(graphText);
    const std::vector<std::string> nodes = cardsNodes(graph);
    if (nodes.empty()) fail("state", "the graph has no Cards node: add one first");
    for (const std::string& node : nodes) {
        std::vector<std::pair<std::string, ParamValue>> params;
        try {
            params = cardPresetParams(lod, cardNodeKind(graphText, node));
        } catch (const std::invalid_argument& e) {
            fail("bad_request", e.what());
        }
        for (json& n : graph["nodes"]) {
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
    }
    return graph.dump();
}

// cards_preset: the session's Cards nodes (or `node`) set for a level of detail, each by what it
// cards (Maya's cards_preset: hero, mid, low; `triangles` overrides the budget). Not an undo step
// (the host keeps the graph, as an edit_graph op).
json cardsPreset(Session& s, const json& args) {
    const std::string lod = needString(args, "lod");
    const std::string text = s.groom->data().graphJson;
    json graph = json::parse(text);
    std::vector<std::string> nodes = cardsNodes(graph);
    const std::string only = optString(args, "node", "");
    if (!only.empty()) {
        if (std::find(nodes.begin(), nodes.end(), only) == nodes.end()) fail("not_found", "no Cards node '" + only + "'");
        nodes = {only};
    }
    if (nodes.empty()) fail("state", "the graph has no Cards node: add one first");
    std::optional<int> triangles;
    if (args.contains("triangles") && args["triangles"].is_number()) triangles = args["triangles"].get<int>();
    json set = json::object();
    for (const std::string& node : nodes) {
        std::vector<std::pair<std::string, ParamValue>> params;
        try {
            params = cardPresetParams(lod, cardNodeKind(text, node), triangles);
        } catch (const std::invalid_argument& e) {
            fail("bad_request", e.what());
        }
        set = json::object();
        for (json& n : graph["nodes"]) {
            if (n.value("name", std::string()) != node) continue;
            if (!n.contains("params") || !n["params"].is_object()) n["params"] = json::object();
            for (const auto& [name, value] : params) {
                std::visit(
                    [&](const auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                                      std::is_same_v<T, std::string>) {
                            n["params"][name] = v;
                            set[name] = v;
                        }
                    },
                    value);
            }
        }
    }
    s.groom->setGraphJson(graph.dump());
    return {{"nodes", nodes}, {"params", set}};
}

// The session's graph swapped for another while an export evaluates, put back after.
class GraphSwap {
public:
    GraphSwap(Session& s, const std::string& graph) : s_(s), before_(s.groom->data().graphJson) {
        if (!s.groom->graphError().empty()) fail("graph", "the graph does not load: " + s.groom->graphError());
        s.groom->setGraphJson(graph);
    }
    ~GraphSwap() {
        try {
            s_.groom->setGraphJson(before_);
        } catch (...) {
        }
    }
    GraphSwap(const GraphSwap&) = delete;
    GraphSwap& operator=(const GraphSwap&) = delete;

private:
    Session& s_;
    std::string before_;
};

std::string graphTextOf(const Session& s) {
    const std::string& text = s.groom->data().graphJson;
    return text.find_first_not_of(" \t\r\n{}") == std::string::npos ? defaultGraphJson() : text;
}

// <directory>/<name>: the folder made.
std::string outFile(const std::string& directory, const std::string& file) {
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    return (fs::u8path(directory) / fs::u8path(file)).generic_u8string();
}

void writeJsonFile(const std::string& path, const json& value) {
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) fail("io", "cannot write '" + path + "'");
    f << value.dump(2) << "\n";
    if (!f) fail("io", "cannot write '" + path + "'");
}

std::string baseName(const std::string& path) { return fs::u8path(path).filename().u8string(); }

// What the card exports' manifests say about the cards' vertex colors, normals and feather cards
// (Maya's groomlab/cards.py CARD_TINT_NOTE, CARD_NORMALS_NOTE, FEATHER_CARD_NOTE).
const char* const kCardTintNote =
    "Color set groomLabTint (glTF: _TINT, the tint itself): each card's tint, half its value (0.5 none, 1 twice "
    "as light), linear: its own strands' color over the color its shared texture was baked from, root to tip "
    "(textures are shared by shape; the tint gives each card its own color, tips and bands included). "
    "In the material, multiply the Color map's RGB by 2 x the vertex color's RGB (read unconverted, as linear).";
const char* const kCardNormalsNote =
    "Normals: the strands' own (locked), out of the scalp at the card's roots and leaning along the card as "
    "the groom's lit strands are drawn: shade the cards with them, the same from either side.";
const char* const kFeatherCardNote =
    "A vaned feather's card with featherSpine has three vertices per station, root to tip: the left edge, the "
    "spine (on the rachis, at the baked shaft's U) and the right edge; each half folds out of the vane plane to "
    "follow its vane and has its own normals (the spine's between them). The maps are baked onto the flat card "
    "(each vertex keeps the flat card's UV), with Depth and Tangent in each half's own frame. For wind or "
    "stiffness, the spine is the middle vertex of each station (glTF: TEXCOORD_1 x is 0.5 on it, 0 and 1 at the "
    "edges); down feathers' crossed cards are flat strips of two vertices a station.";

// ---- glTF cards (Maya's export_cards_gltf, groomLabExportGltf) ----------------------------------

json exportGltfCards(Session& s, const json& args) {
    const std::string path = needString(args, "path");
    const std::string ext = fs::u8path(path).extension().u8string();
    const bool binary = optBool(args, "binary", ext != ".gltf");
    std::string name = optString(args, "name", fs::u8path(path).stem().u8string());
    if (name.empty()) name = "groom";
    int resolution = static_cast<int>(optNumber(args, "resolution", 0.0));
    resolution = resolution > 0 ? std::clamp(resolution, 16, 16384) : 0;  // 0: the Cards nodes' atlasSize
    const int capResolution = static_cast<int>(optNumber(args, "capResolution", 0.0));
    CardGltfSettings settings;
    const std::string alpha = optString(args, "alphaMode", "mask");
    if (alpha == "mask") {
        settings.alphaMode = GltfMaterial::AlphaMode::Mask;
    } else if (alpha == "blend") {
        settings.alphaMode = GltfMaterial::AlphaMode::Blend;
    } else if (alpha == "opaque") {
        settings.alphaMode = GltfMaterial::AlphaMode::Opaque;
    } else {
        fail("bad_request", "alphaMode is mask, blend or opaque");
    }
    settings.alphaCutoff = static_cast<float>(optNumber(args, "alphaCutoff", 0.5));
    settings.scale = static_cast<float>(optNumber(args, "scale", 0.01));  // groom space's centimeters to glTF's meters
    if (!(settings.scale > 0.0f)) fail("bad_request", "scale must be above 0");
    settings.zUp = false;  // groom space is Y up, as glTF
    settings.rootToTip = optBool(args, "rootToTip", true);
    settings.tangents = optBool(args, "tangents", true);
    const float occlusion = static_cast<float>(optNumber(args, "occlusion", 1.0));
    std::vector<std::string> lods;
    if (args.contains("lods") && !args["lods"].is_null()) {
        if (!args["lods"].is_array()) fail("bad_request", "lods is a list of levels of detail (hero, mid, low)");
        for (const json& l : args["lods"]) {
            if (!l.is_string() || !findCardLodPreset(l.get<std::string>())) fail("bad_request", "a level of detail is hero, mid or low");
            lods.push_back(l.get<std::string>());
        }
    }
    const HairLook look = s.groom->hairLook();
    settings.roughness = look.roughness;
    settings.specular = look.specular;
    GltfDocument doc;
    doc.rootName = name;
    doc.generator = "GroomLab for Blender";
    json meshes = json::array();
    std::vector<std::string> warnings;
    auto addLevel = [&](const std::string& lod) {
        const CardBake bake = bakeCardsNow(s, resolution, occlusion);
        settings.name = name + (lod.empty() ? "_cards" : "_" + lod);
        addCardsToGltf(doc, *bake.cards, bake.textures, settings);
        meshes.push_back(settings.name);
    };
    if (lods.empty()) {
        addLevel("");
    } else {
        const std::string graph = graphTextOf(s);
        for (const std::string& lod : lods) {
            GraphSwap swap(s, graphAtLod(graph, lod));
            addLevel(lod);
        }
    }
    if (optBool(args, "cap", true)) {
        try {
            const ViewResult& view = evaluateNow(s, false);
            const MeshPtr scalp = scalpOf(s, view);
            if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the groom has no scalp");
            CardGltfSettings cap = settings;
            cap.name = name + "_scalpCap";
            addScalpCapToGltf(doc, *view.strands, *scalp, capResolution, resolution, cap);
            meshes.push_back(cap.name);
        } catch (const RequestError&) {
            throw;
        } catch (const std::exception& e) {  // a scalp without UVs: the cards still go
            warnings.push_back(std::string("no hair cap (") + e.what() + ")");
        }
    }
    try {
        return {{"files", writeGltf(path, doc, binary)}, {"meshes", meshes}, {"warnings", warnings}};
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
}

// ---- Unreal and Unity cards (Maya's export_cards_unreal, export_cards_unity) ----------------------

std::vector<std::string> writeUnityCardMaps(const CardTextures& t, float roughness, const std::string& directory, const std::string& name) {
    const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
    const auto smoothness = static_cast<std::uint8_t>(std::lround(255.0f * (1.0f - roughness)));
    std::vector<std::uint8_t> baseColor(4 * n), mask(4 * n);
    for (std::size_t i = 0; i < n; ++i) {
        baseColor[4 * i] = t.color[3 * i];
        baseColor[4 * i + 1] = t.color[3 * i + 1];
        baseColor[4 * i + 2] = t.color[3 * i + 2];
        baseColor[4 * i + 3] = t.coverage[i];
        mask[4 * i] = t.iridescence.size() == n ? t.iridescence[i] : 0;  // metallic: none; the iridescence, if any
        mask[4 * i + 1] = t.occlusion[i];                                  // ambient occlusion
        mask[4 * i + 2] = t.attribute[4 * i + 3];                          // root (0) to tip (1)
        mask[4 * i + 3] = smoothness;                                      // 1 - the hair look's roughness
    }
    const std::vector<std::uint8_t> normal = cardNormalOpenGl(t);  // Unity's green: OpenGL's
    std::vector<std::string> paths;
    std::vector<PngImage> images;
    auto png = [&](const char* map, int channels, const std::vector<std::uint8_t>& pixels) {
        paths.push_back(outFile(directory, name + "_" + map + ".png"));
        images.push_back({t.size, t.size, channels, &pixels});
    };
    png("BaseColor", 4, baseColor);
    png("Mask", 4, mask);
    png("AO", 1, t.occlusion);
    png("Tangent", 3, t.tangent);
    png("Depth", 1, t.depth);
    png("Attribute", 4, t.attribute);
    png("Normal", 3, normal);
    writeCompressedPngs(paths, images);
    return paths;
}

json exportEngineCards(Session& s, const json& args, bool unity) {
    std::string directory = needString(args, "directory");
    std::string name = optString(args, "name", "groom");
    const std::string lod = optString(args, "lod", "");
    if (!lod.empty() && !args.contains("name")) name += "_" + lod;
    int resolution = static_cast<int>(optNumber(args, "resolution", 0.0));
    resolution = resolution > 0 ? std::clamp(resolution, 16, 16384) : 0;
    const float occlusion = static_cast<float>(optNumber(args, "occlusion", 1.0));
    bool withScalp = optBool(args, "scalp", true);
    const CardBake bake = bakeCardsNow(s, resolution, occlusion);
    const GroomStats stats = s.groom->stats();  // the cards' counts, as Maya's card_stats has them
    const HairLook look = s.groom->hairLook();
    std::vector<std::string> paths;
    std::map<std::string, std::string> byMap;
    json warnings = json::array();
    try {
        if (unity) {
            paths = writeUnityCardMaps(bake.textures, look.roughness, directory, name);
        } else {
            const CardBakePaths at = CardBakePaths::in(fs::u8path(directory).generic_u8string(), name);
            std::error_code ec;
            fs::create_directories(fs::u8path(directory), ec);
            writeCardBake(bake, at, false);
            for (const std::string& f : at.files()) {
                if (fs::u8path(f).extension() == ".tga") paths.push_back(f);  // the maps; the atlas record beside them is the groom's own
            }
        }
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    MeshPtr scalp;
    if (withScalp) {
        // The groom baked onto its scalp (the hair cap's maps), at full density.
        try {
            const ViewResult& view = evaluateNow(s, false);
            scalp = s.groom->rawScalp();
            if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the groom has no scalp");
            ScalpBakeSettings settings;
            settings.size = resolution > 0 ? resolution : 2048;
            const ScalpTextures t = bakeScalpTextures(*view.strands, *scalpOf(s, view), settings);
            if (unity) {
                const std::size_t n = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
                std::vector<std::uint8_t> baseColor(4 * n);
                for (std::size_t i = 0; i < n; ++i) {
                    baseColor[4 * i] = t.color[3 * i];
                    baseColor[4 * i + 1] = t.color[3 * i + 1];
                    baseColor[4 * i + 2] = t.color[3 * i + 2];
                    baseColor[4 * i + 3] = t.coverage[i];
                }
                const std::string p = outFile(directory, name + "_ScalpBaseColor.png");
                writeCompressedPng(p, t.size, t.size, 4, baseColor);
                paths.push_back(p);
            } else {
                const std::string color = outFile(directory, name + "_ScalpColor.tga");
                const std::string coverage = outFile(directory, name + "_ScalpCoverage.tga");
                writeTga(color, t.size, t.size, 3, t.color);
                writeTga(coverage, t.size, t.size, 1, t.coverage);
                paths.push_back(color);
                paths.push_back(coverage);
            }
        } catch (const RequestError&) {
            throw;
        } catch (const std::exception& e) {  // e.g. a scalp without UVs: the cards still go
            warnings.push_back(std::string("no scalp maps (") + e.what() + ")");
            withScalp = false;
        }
    }
    const std::string ext = unity ? ".png" : ".tga";
    for (const std::string& p : paths) {
        const std::string file = baseName(p);
        const std::string prefix = name + "_";
        if (file.size() > prefix.size() + ext.size() && file.compare(0, prefix.size(), prefix) == 0) {
            byMap[file.substr(prefix.size(), file.size() - prefix.size() - ext.size())] = file;
        }
    }
    const std::string fbx = outFile(directory, name + "_cards.fbx");
    const std::string capFbx = withScalp ? outFile(directory, name + "_scalpCap.fbx") : std::string();
    json manifest;
    json maps = json::object();
    for (const auto& [map, file] : byMap) maps[map] = file;
    if (!unity) {
        manifest = {
            {"mesh", baseName(fbx)},
            {"cards", stats.cards}, {"triangles", stats.cardTriangles}, {"textures", stats.cardTextures}, {"feathers", stats.cardFeathers},
            {"maps", maps},
            {"channels",
             {{"Depth", "R: distance off the card plane (0.5 on it, higher in front)"},
              {"Coverage", "R: opacity"},
              {"Tangent", "RGB: strand direction in card space (x across U, y root to tip along V, z out), 0.5 + 0.5 * t"},
              {"Attribute", "R: root U, G: root V (scalp UVs), B: seed (random per strand), A: coordU (0 root, 1 tip "
                            "along each strand; on a feather's card along each barb, from the shaft out)"},
              {"Color", "RGB: base color, sRGB"},
              {"AO", "R: ambient occlusion (1 open)"},
              {"Normal", "RGB: the strands' normal as round fibers in tangent space (x across U, y along V, z out), "
                         "DirectX's green (Unreal's), linear"},
              {"ScalpColor", "RGB: the hair's color on the scalp (sRGB), in the scalp's UVs, for the hair cap"},
              {"ScalpCoverage", "R: how much hair covers the skin, in the scalp's UVs"}}},
            {"scalpCap", withScalp ? json(baseName(capFbx)) : json()},
            {"uv", "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U"},
            {"vertexColor", kCardTintNote},
            {"normals", kCardNormalsNote},
            {"featherCards", kFeatherCardNote}};
    } else {
        const double smoothness = std::round((1.0 - look.roughness) * 1000.0) / 1000.0;
        auto mapOr = [&](const char* m) { return maps.contains(m) ? maps[m] : json(); };
        manifest = {
            {"engine", "Unity"},
            {"mesh", baseName(fbx)},
            {"scalpCap", withScalp ? json(baseName(capFbx)) : json()},
            {"units", "meters, Y up (the FBX is converted from the scene's units)"},
            {"cards", stats.cards}, {"triangles", stats.cardTriangles}, {"textures", stats.cardTextures}, {"feathers", stats.cardFeathers},
            {"maps", maps},
            {"channels",
             {{"BaseColor", "RGB: base color (sRGB); A: coverage (opacity: alpha clip at 0.5)"},
              {"Mask", "HDRP mask map layout. R: metallic (0); G: ambient occlusion (1 open); "
                       "B: root (0) to tip (1) along the strands; A: smoothness (1 - the hair look's roughness)"},
              {"AO", "R (gray): ambient occlusion (1 open)"},
              {"Tangent", "RGB: strand direction in tangent space (R along +U, G along +V root to tip, B out), "
                          "0.5 + 0.5 * t, linear (import without sRGB)"},
              {"Depth", "R (gray): distance off the card plane (0.5 on it, higher in front), linear"},
              {"Attribute", "R, G: root UV on the scalp; B: seed per strand; A: root (0) to tip (1) along each strand "
                            "(a feather's barbs: from the shaft out), linear"},
              {"Normal", "RGB: the strands' normal as round fibers in tangent space (R along +U, G along +V, B out), "
                         "OpenGL's green, linear (import as a Normal map)"},
              {"ScalpBaseColor", "RGB: the hair's color on the scalp (sRGB); A: how much hair covers the skin"}}},
            {"import",
             {{"textures", "BaseColor and ScalpBaseColor sRGB with Alpha Is Transparency; Mask, AO, Tangent, "
                           "Depth and Attribute linear (sRGB off); Normal as Texture Type Normal map"},
              {"model", "Convert Units on (the FBX is in meters, so its scale stays 1)"}}},
            {"materials",
             {{"HDRP",
               {{"shader", "a Shader Graph with the HDRP Hair material (or HDRP/Lit)"},
                {"Base Color Map", mapOr("BaseColor")},
                {"Alpha Clipping", true}, {"Alpha Clip Threshold", 0.5}, {"Double-Sided", true},
                {"Mask Map", mapOr("Mask")},
                {"Normal Map", mapOr("Normal")},
                {"Hair Strand Direction", "Tangent map * 2 - 1 (strands run +V, root to tip)"},
                {"Smoothness", smoothness}}},
              {"URP",
               {{"shader", "Universal Render Pipeline/Lit"},
                {"Base Map", mapOr("BaseColor")},
                {"Alpha Clipping", true}, {"Threshold", 0.5}, {"Render Face", "Both"},
                {"Occlusion Map", mapOr("AO")},
                {"Normal Map", mapOr("Normal")},
                {"Smoothness", smoothness}}},
              {"scalpCap", "Transparent (alpha blend) with ScalpBaseColor, drawn under the cards"}}},
            {"uv", "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U"},
            {"vertexColor", kCardTintNote},
            {"normals", kCardNormalsNote},
            {"featherCards", kFeatherCardNote}};
    }
    const std::string manifestPath = outFile(directory, name + (unity ? "_unity.json" : "_cards.json"));
    writeJsonFile(manifestPath, manifest);
    std::vector<MeshOut> meshes{cardsMesh(*bake.cards, "cards")};
    meshes.front().info["fbx"] = fbx;
    if (unity) meshes.front().info["material"] = name + "_cards_mat";
    if (withScalp && scalp) {
        meshes.push_back(scalpCapMesh(*scalp, "scalpCap"));
        meshes.back().info["fbx"] = capFbx;
        if (unity) meshes.back().info["material"] = name + "_scalpCap_mat";
    }
    json r;
    r["maps"] = paths;
    r["manifest"] = manifestPath;
    r["fbx"] = fbx;
    r["scalpCap"] = withScalp ? json(capFbx) : json();
    r["name"] = name;
    r["warnings"] = warnings;
    // Unity's FBX: meters, Y up (Maya's FBXExportUpAxis y, FBXExportConvertUnitString m); Unreal's
    // as the scene is. The host writes the meshes (its own FBX exporter).
    r["fbxSettings"] = unity ? json{{"upAxis", "Y"}, {"units", "m"}} : json{{"upAxis", "scene"}, {"units", "scene"}};
    r["meshes"] = putMeshes(s, meshes, groomSpaceOf(args));
    return r;
}

// ---- The scales (Maya's scale_mesh, export_scales, bake_scale_maps) ------------------------------

std::shared_ptr<const ScaleMesh> scalesNow(Session& s, MeshPtr* scalp) {
    const ViewResult& view = evaluateNow(s, false);
    *scalp = scalpOf(s, view);
    ScaleMeshBuilder builder;
    const auto mesh = builder.build(*view.strands, *scalp, sessionPrototypes(s));
    if (!mesh || mesh->triangles.empty()) fail("state", "the groom has no scales (a Scales node)");
    return mesh;
}

json exportScalesFbx(Session& s, const json& args) {
    const std::string path = needString(args, "path");
    const std::string ext = fs::u8path(path).extension().u8string();
    if (ext != ".fbx") fail("bad_request", "the scales' mesh goes to .fbx (an Alembic or USD export writes them over a range)");
    MeshPtr scalp;
    const auto mesh = scalesNow(s, &scalp);
    std::vector<MeshOut> meshes{scalesMesh(*mesh, "scales")};
    meshes.front().info["fbx"] = path;
    const bool unity = optBool(args, "unity", false);
    return {{"fbx", path},
            {"fbxSettings", unity ? json{{"upAxis", "Y"}, {"units", "m"}} : json{{"upAxis", "scene"}, {"units", "scene"}}},
            {"meshes", putMeshes(s, meshes, groomSpaceOf(args))}};
}

json bakeScaleMapsNow(Session& s, const json& args) {
    const std::string directory = needString(args, "directory");
    const std::string name = optString(args, "name", "groom");
    ScaleMapOptions options;
    options.resolution = static_cast<int>(optNumber(args, "resolution", 2048));
    options.maxHeight = static_cast<float>(optNumber(args, "maxHeight", 0.0));
    options.padding = static_cast<int>(optNumber(args, "padding", 8));
    if (options.resolution < 16 || options.resolution > 8192) fail("bad_request", "resolution must be 16 to 8192");
    if (options.maxHeight < 0.0f) fail("bad_request", "maxHeight must be 0 (measured) or more");
    MeshPtr scalp;
    const auto mesh = scalesNow(s, &scalp);
    if (!scalp || !scalp->hasUVs()) fail("state", "the scalp has no uvs to bake the scales onto");
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    const std::string prefix = (fs::u8path(directory) / fs::u8path(name + "_")).generic_u8string();
    try {
        const ScaleMaps maps = bakeScaleMaps(*scalp, *mesh, options);
        const std::vector<std::string> files = writeScaleMaps(maps, prefix);
        json byMap = json::object(), pattern = json::object();
        for (const char* m : kScaleMapNames) {
            json list = json::array();
            const std::string stem = name + "_" + m + ".";
            for (const std::string& f : files) {
                if (baseName(f).compare(0, stem.size(), stem) == 0) list.push_back(f);
            }
            byMap[m] = list;
            pattern[m] = scaleMapPath(prefix, m, maps.udim);
        }
        return {{"files", files}, {"maps", byMap}, {"pattern", pattern}, {"maxHeight", maps.maxHeight}, {"udim", maps.udim},
                {"tiles", maps.tiles.size()}, {"covered", maps.texels ? double(maps.covered) / double(maps.texels) : 0.0}};
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
}

// ---- Unity feathers (Maya's export_unity_feathers) ----------------------------------------------

json exportUnityFeathers(Session& s, const json& args) {
#if GROOMLAB_HAS_ALEMBIC
    const std::string folder = needString(args, "directory");
    const std::string lod = optString(args, "lod", "");
    std::string name = optString(args, "name", "groom");
    if (!lod.empty() && !args.contains("name")) name += "_" + lod;
    EngineExportOptions e;
    e.engine = EngineExportOptions::Engine::Unity;
    e.groups = kGroupIdChannel;
    const double points = optNumber(args, "points", 16.0), barbPoints = optNumber(args, "barbPoints", 6.0);
    for (const double p : {points, barbPoints}) {
        if (p < 3.0 || p > 128.0 || p != std::floor(p)) fail("bad_request", "points and barbPoints must be 3 to 128 (Unity's strand particle counts)");
    }
    e.pointsPerStrand = static_cast<std::size_t>(points);
    e.barbPoints = static_cast<std::size_t>(barbPoints);
    const double most = optNumber(args, "maxStrandsPerGroup", static_cast<double>(kUnityMaxStrandsPerGroup));
    if (most < 0.0) fail("bad_request", "maxStrandsPerGroup must be 0 (no limit) or more");
    e.maxStrandsPerGroup = static_cast<std::size_t>(most);
    if (args.contains("shafts") && !args["shafts"].is_null()) e.shafts = optBool(args, "shafts", true);
    double barbDensity = 1.0;
    if (!lod.empty()) {
        static const std::map<std::string, double> kLods = {{"high", 1.0}, {"mid", 0.5}, {"low", 0.25}};
        const auto it = kLods.find(lod);
        if (it == kLods.end()) fail("bad_request", "lod is high, mid or low");
        barbDensity = it->second;
        e.barbDensity = static_cast<float>(barbDensity);
        e.barbWidthScale = static_cast<float>(1.0 / std::sqrt(barbDensity));
    }
    const std::string abc = outFile(folder, name + "_strands.abc");
    ViewRequest request;
    request.density = static_cast<float>(optNumber(args, "density", 1.0));
    request.time = s.seconds;
    request.wantBounds = false;
    feedSessionTime(s);
    const ViewResult& view = s.groom->evaluate(request);
    if (!view.errors.empty()) fail("evaluation", view.errors.front());
    if (!view.strands) fail("evaluation", "the groom gave no strands");
    const StrandSet& st = *view.strands;
    json objects = json::array();
    try {
        const double fps = s.groom->framesPerSecond();
        EngineGroomWriter writer(abc, name, s.seconds, frameToSeconds(1.0, fps), e);
        writer.writeFrame(st, nullptr, scalpOf(s, view).get());
        writer.close();
        for (const EngineGroomWriter::Object& o : writer.objects()) {
            objects.push_back({{"object", o.path}, {"group", o.group}, {"groupId", o.groupId}, {"feathers", o.feathers},
                               {"strands", o.strands}, {"pointsPerStrand", o.pointsPerStrand ? json(o.pointsPerStrand) : json()},
                               {"minPoints", o.minPoints}, {"maxPoints", o.maxPoints}});
        }
    } catch (const std::invalid_argument& ex) {
        fail("bad_request", ex.what());
    } catch (const std::exception& ex) {
        fail("io", ex.what());
    }
    json warnings = json::array();
    std::vector<MeshOut> meshes;
    std::string fbx;
    const bool rachis = optBool(args, "rachis", true) && hasRachis(st.featherLayers());
    if (rachis) {
        fbx = outFile(folder, name + "_rachis.fbx");
        meshes.push_back(rachisLayoutMesh(buildRachisMesh(st.featherLayers()), "rachis"));
        meshes.back().info["fbx"] = fbx;
    } else if (std::any_of(objects.begin(), objects.end(), [](const json& o) { return o.value("feathers", false); })) {
        warnings.push_back("the feathers have no rachis: their shafts go as strands, and there is no mesh for the barbs to ride");
    }
    bool uniform = true;
    for (const json& o : objects) uniform = uniform && o["minPoints"] == o["maxPoints"];
    json groups = json::array();
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const json& o = objects[index];
        json instance;
        if (o.value("feathers", false)) {
            instance = {{"Skinning", {{"Roots Attach", true},
                                      {"Roots Attach Target", "the rachis's SkinAttachmentTarget (" +
                                                                  (fbx.empty() ? std::string("no rachis written") : baseName(fbx)) + ")"}}},
                        {"Physics", {{"Global Position", true}, {"Global Position Influence", 1.0}, {"Global Position Interval", "Per 1ms"},
                                     {"Global Rotation", true}, {"Global Rotation Influence", 1.0}, {"Gravity", 0.0},
                                     {"why", "barbs ride the skinned rachis: held to their groomed shape, not simulated"}}},
                        {"Rendering", {{"Material", "a hair material of its own (the feather design's look)"}}}};
        } else {
            instance = {{"Skinning", {{"Roots Attach", true}, {"Roots Attach Target", "the body's SkinAttachmentTarget (the scalp's skinned mesh)"}}},
                        {"Physics", "the package's defaults (simulated)"},
                        {"Rendering", {{"Material", "the hair material"}}}};
        }
        groups.push_back({{"index", index}, {"object", o["object"]}, {"group", o["group"]}, {"groupId", o["groupId"]},
                          {"feathers", o["feathers"]}, {"strands", o["strands"]},
                          {"pointsPerStrand", o["pointsPerStrand"].is_null() ? o["maxPoints"] : o["pointsPerStrand"]},
                          {"hairInstance", instance}});
    }
    const json manifest = {
        {"engine", "Unity"},
        {"packages", {{"com.unity.demoteam.hair", "https://github.com/Unity-Technologies/com.unity.demoteam.hair.git"},
                      {"com.unity.formats.alembic", "2.2.2 or later (Unity's Package Manager)"},
                      {"com.unity.demoteam.digital-human", "https://github.com/Unity-Technologies/com.unity.demoteam.digital-human.git "
                                                           "(0.2.1-preview or later: roots attach with the mesh's twist)"}}},
        {"strands", baseName(abc)},
        {"rachis", fbx.empty() ? json() : json(baseName(fbx))},
        {"lod", lod.empty() ? json() : json(lod)},
        {"barbDensity", barbDensity},
        {"units", "the .abc in centimeters (the Alembic importer's Scale Factor 0.01 makes meters), widths in "
                  "centimeters; the rachis FBX in meters, Y up: both land in the same place"},
        {"import", {{"strands", {{"Scale Factor", 0.01}, {"Swap Handedness", true}, {"Import Curves", true}}},
                    {"rachis", {{"Convert Units", true}, {"then", "skin it to the character's skeleton (or combine it with the "
                                                                  "body before skinning) and give its SkinnedMeshRenderer a SkinAttachmentTarget"}}}}},
        {"hairAsset", {{"Type", "Alembic"}, {"Alembic Asset", baseName(abc)}, {"Alembic Asset Groups", "Preserve"},
                       {"Alembic Scale Positions", "Data In Meters"}, {"Alembic Scale Diameters", "Data In Centimeters"},
                       {"Resample Curves", !uniform}, {"Root UV", "Resolve From Curves"}, {"Strand Diameter", "Resolve From Curves"},
                       {"Tip Scale", "Resolve From Curves"},
                       {"why", "Preserve keeps every curves object its own strand group (Combine merges neighbours of the same "
                               "point count); Resample Curves on would give every group one Resample Resolution"}}},
        {"groups", groups},
        {"roots", "Unity attaches a strand group's roots (SkinAttachment, attachment type Mesh) to the nearest vertex "
                  "of the target mesh and the triangles round it: every barb root lies on its own rachis's surface "
                  "(rootOnShaft), so attach the feather groups to the rachis mesh, the hair to the body"}};
    const std::string manifestPath = outFile(folder, name + "_unity_feathers.json");
    writeJsonFile(manifestPath, manifest);
    return {{"abc", abc}, {"rachis", fbx.empty() ? json() : json(fbx)}, {"manifest", manifestPath}, {"groups", groups},
            {"warnings", warnings}, {"fbxSettings", {{"upAxis", "Y"}, {"units", "m"}}}, {"meshes", putMeshes(s, meshes, groomSpaceOf(args))}};
#else
    (void)s;
    (void)args;
    fail("unsupported", "this helper was built without Alembic");
#endif
}

// ---- Strands as a mesh (Maya's strands_to_mesh, StrandMesh nodes) --------------------------------

StrandMeshOptions meshOptions(const json& args) {
    StrandMeshOptions o;
    const std::string style = optString(args, "style", "tube");
    if (style != "tube" && style != "ribbon") fail("bad_request", "style is tube or ribbon");
    o.style = style == "tube" ? StrandMeshOptions::Style::Tube : StrandMeshOptions::Style::Ribbon;
    o.sides = static_cast<int>(optNumber(args, "sides", 4));
    if (o.sides < 3) fail("bad_request", "sides must be 3 or more");
    o.widthScale = static_cast<float>(optNumber(args, "widthScale", 1.0));
    o.minWidth = static_cast<float>(optNumber(args, "minWidth", 0.0));
    o.strandUVs = optBool(args, "strandUVs", false);
    return o;
}

MeshOut strandMeshOut(const StrandSet& strands, const ScalpMesh* scalp, const StrandMeshOptions& options, const std::string& key) {
    const StrandMesh sm = buildStrandMesh(strands, scalp, options);
    MeshOut m = rachisLayoutMesh(sm.mesh, key);
    if (!sm.u2.empty() && sm.mesh.faceUVs.size() == sm.mesh.faceVertices.size()) {
        std::vector<float>& set = m.uvSets["strandUVs"];
        for (const int k : sm.mesh.faceUVs) {
            const auto i = static_cast<std::size_t>(k);
            set.push_back(i < sm.u2.size() ? sm.u2[i] : 0.0f);
            set.push_back(i < sm.v2.size() ? sm.v2[i] : 0.0f);
        }
    }
    return m;
}

json strandMesh(Session& s, const json& args) {
    std::vector<MeshOut> meshes;
    json r;
    if (optBool(args, "nodes", false)) {
        // Every StrandMesh node making the output, each its own mesh by its node's options.
        feedSessionTime(s);
        std::vector<StrandMeshSource> sources;
        try {
            sources = s.groom->evaluateStrandMeshes(s.seconds);
        } catch (const std::exception& e) {
            fail("evaluation", e.what());
        }
        const MeshPtr scalp = s.groom->scalp();
        for (const StrandMeshSource& src : sources) {
            if (!src.strands) continue;
            meshes.push_back(strandMeshOut(*src.strands, scalp.get(), src.options, src.node));
            meshes.back().info["strands"] = src.strands->strandCount();
        }
        s.groom->releaseStrandMeshes();
        r["nodes"] = json::array();
        for (const MeshOut& m : meshes) r["nodes"].push_back(m.key);
    } else {
        // The groom's strands (Strands to Mesh): at `density`, at most `maxStrands` (thinned by ID).
        ViewRequest request;
        request.density = static_cast<float>(optNumber(args, "density", 1.0));
        if (!(request.density > 0.0f && request.density <= 1.0f)) fail("bad_request", "density must be above 0, at most 1");
        request.time = s.seconds;
        request.wantBounds = false;
        feedSessionTime(s);
        const ViewResult& view = s.groom->evaluate(request);
        if (!view.errors.empty()) fail("evaluation", view.errors.front());
        if (!view.strands) fail("evaluation", "the groom gave no strands");
        const StrandSet& all = *view.strands;
        const double most = optNumber(args, "maxStrands", 100000.0);
        if (most < 1.0) fail("bad_request", "maxStrands must be 1 or more");
        const std::vector<std::size_t> keep = thinStrandsById(all, static_cast<std::size_t>(most));
        StrandSet kept;
        const StrandSet* strands = &all;
        if (keep.size() < all.strandCount()) {
            for (const std::size_t i : keep) {
                const Vec3f* p = all.strandPoints(i);
                const std::size_t n = all.strandPointCount(i);
                std::vector<float> w;
                if (all.widths().size() == all.pointCount()) w.assign(all.widths().begin() + all.strandBegin(i), all.widths().begin() + all.strandBegin(i) + n);
                kept.addStrand(all.ids()[i], std::vector<Vec3f>(p, p + n), w, i < all.roots().size() ? all.roots()[i] : StrandRoot{});
            }
            strands = &kept;
        }
        meshes.push_back(strandMeshOut(*strands, scalpOf(s, view).get(), meshOptions(args), "strands"));
        r["strands"] = keep.size();
        r["total"] = all.strandCount();
        r["capped"] = keep.size() < all.strandCount();
    }
    r["meshes"] = putMeshes(s, meshes, groomSpaceOf(args));
    return r;
}

// ---- Simulation (Maya's groomlab.sim: Simulate Range, Bake, Clear) ------------------------------

json simStatusJson(Session& s) {
    json out = json::array();
    for (const SimStatus& st : s.groom->simulationStatus()) {
        json ranges = json::array();
        for (const auto& [a, b] : st.ranges) ranges.push_back({a, b});
        out.push_back({{"node", st.node}, {"startFrame", st.startFrame}, {"ranges", ranges}, {"frames", st.frames}, {"live", st.live},
                       {"baked", st.baked}, {"bakeStale", st.bakeStale}, {"bakedFirst", st.bakedFirst}, {"bakedLast", st.bakedLast},
                       {"bakedFile", st.bakedFile}, {"stepMs", st.stepMs}, {"bytes", st.bytes}, {"stamp", std::to_string(st.stamp)}});
    }
    return out;
}

// The frame each node has stepped to, the least of them (a node before its start frame: the frame
// before it); `to` the job's target.
int simulatedThrough(Session& s, int to) {
    int least = to;
    bool any = false;
    for (const SimStatus& st : s.groom->simulationStatus()) {
        int last = st.startFrame - 1;
        for (const auto& [a, b] : st.ranges) {
            if (a <= last + 1 && b > last) last = b;
        }
        least = any ? std::min(least, last) : last;
        any = true;
    }
    return any ? least : to;
}

json jobJson(Session& s, bool running) {
    json r;
    r["running"] = running;
    r["status"] = simStatusJson(s);
    if (s.simJob) {
        const SimJob& j = *s.simJob;
        const int through = simulatedThrough(s, j.target);
        r["target"] = j.target;
        r["frame"] = through;
        r["progress"] = j.target > j.from ? std::clamp(double(through - j.from) / double(j.target - j.from), 0.0, 1.0) : 1.0;
        r["frames"] = j.frames;
        r["milliseconds"] = j.milliseconds;
        r["errors"] = j.errors;
    }
    return r;
}

// One request's share of a Simulate Range: the nodes stepped on until the job's budget is spent
// (a frame at least), the frames on the way fed by the host's scalp frames.
json stepJob(Session& s) {
    SimJob& j = *s.simJob;
    const auto t0 = std::chrono::steady_clock::now();
    const double budget = j.budgetMs;
    const SimRun run = s.groom->simulateTo(j.target, [&](int, int) {
        return budget <= 0.0 || std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < budget;
    });
    j.frames += static_cast<std::size_t>(std::max(0, run.frames));
    j.milliseconds += run.milliseconds;
    j.errors.insert(j.errors.end(), run.errors.begin(), run.errors.end());
    const bool running = run.cancelled && run.errors.empty() && run.frames > 0;
    json r = jobJson(s, running);
    r["done"] = !running && run.errors.empty() && simulatedThrough(s, j.target) >= j.target;
    if (!running) s.simJob.reset();
    return r;
}

std::vector<std::string> simNodesOf(Session& s, const json& args) {
    const std::vector<std::string> all = s.groom->simulateNodes();
    const std::string node = optString(args, "node", "");
    if (node.empty()) return all;
    if (std::find(all.begin(), all.end(), node) == all.end()) fail("not_found", "no Simulate node '" + node + "' makes the output");
    return {node};
}

// The bakes the session's nodes read (the file's simCaches), as a step's undo puts them back:
// each read again from its file (a bake's reader holds its file open: a bake written again in its
// place lets the old one go first).
void setBakes(Session& s, const std::map<std::string, SimCacheRef>& bakes, const std::vector<std::string>& nodes) {
    for (const std::string& node : nodes) {
        const auto it = bakes.find(node);
        BakedSimulationPtr baked;
#if GROOMLAB_HAS_ALEMBIC
        if (it != bakes.end()) {
            try {
                baked = readSimulationCache(resolveMapFile(s, it->second.file), it->second.object, it->second.stamp);
            } catch (const std::exception&) {
            }
        }
#endif
        s.groom->setBakedSimulation(node, baked);
        if (it != bakes.end()) {
            s.file.simCaches[node] = it->second;
        } else {
            s.file.simCaches.erase(node);
        }
    }
}

std::string simCacheFolder(Session& s, const json& args) {
    std::string folder = optString(args, "directory", "");
    if (folder.empty()) {
        std::error_code ec;
        folder = s.file.directory.empty() ? (fs::temp_directory_path(ec) / blockName("s" + std::to_string(s.id) + "sim", 0)).generic_u8string()
                                          : (fs::u8path(s.file.directory) / "groomlab_sim").generic_u8string();
    }
    return folder;
}

json simulateBake(Session& s, const json& args) {
#if GROOMLAB_HAS_ALEMBIC
    const std::vector<std::string> nodes = simNodesOf(s, args);
    if (nodes.empty()) fail("state", "the graph has no Simulate node to bake");
    const std::string path = optString(args, "path", "");
    if (!path.empty() && nodes.size() > 1) fail("bad_request", "one path for " + std::to_string(nodes.size()) + " Simulate nodes: name the node");
    const double fps = s.groom->framesPerSecond();
    const int last = static_cast<int>(std::lround(optNumber(args, "last", secondsToFrame(s.seconds, fps))));
    const std::string name = optString(args, "name", "groom");
    const std::map<std::string, SimCacheRef> before = s.file.simCaches;
    // The nodes simulate rather than read their old bakes (their files are written again).
    for (const std::string& node : nodes) {
        s.groom->setBakedSimulation(node, nullptr);
        s.file.simCaches.erase(node);
    }
    s.simJob.reset();
    const SimRun run = s.groom->simulateTo(last);
    json written = json::object();
    try {
        if (!run.errors.empty()) throw std::runtime_error(run.errors.front());
        if (run.cancelled) throw std::runtime_error("the simulation stopped before frame " + std::to_string(last) + ": nothing baked");
        std::map<std::string, int> starts;
        for (const SimStatus& st : s.groom->simulationStatus()) starts[st.node] = st.startFrame;
        const std::string folder = simCacheFolder(s, args);
        for (const std::string& node : nodes) {
            const std::string out = path.empty() ? outFile(folder, name + "_" + node + "_sim.abc") : path;
            std::error_code ec;
            fs::create_directories(fs::u8path(out).parent_path(), ec);
            const int first = args.contains("first") && !args["first"].is_null() ? static_cast<int>(std::lround(optNumber(args, "first", 1)))
                                                                                     : starts[node];
            writeSimulationCache(*s.groom, node, out, first, last);
            const std::uint64_t stamp = s.groom->simulationStamp(node);
            s.groom->setBakedSimulation(node, readSimulationCache(out, {}, stamp));
            s.file.simCaches[node] = {fs::u8path(out).generic_u8string(), {}, stamp};
            written[node] = out;
        }
    } catch (const std::exception& e) {
        setBakes(s, before, nodes);
        fail("export", e.what());
    }
    const std::map<std::string, SimCacheRef> after = s.file.simCaches;
    Session* ps = &s;
    commitStep(s, "bake simulation", {}, [ps, before, after, nodes](bool redo) { setBakes(*ps, redo ? after : before, nodes); });
    return {{"written", written}, {"frames", run.frames}, {"status", simStatusJson(s)}, {"undo", undoStateJson(s)}};
#else
    (void)s;
    (void)args;
    fail("unsupported", "this helper was built without Alembic");
#endif
}

json simulateClear(Session& s, const json& args) {
    const std::vector<std::string> nodes = simNodesOf(s, args);
    s.simJob.reset();
    const std::string node = optString(args, "node", "");
    s.groom->invalidateSimulation(node);
    const std::map<std::string, SimCacheRef> before = s.file.simCaches;
    std::vector<std::string> cleared;
    for (const auto& [n, ref] : before) {
        if (node.empty() || n == node) cleared.push_back(n);
    }
    json r;
    if (!cleared.empty() && optBool(args, "bakes", true)) {
        for (const std::string& n : cleared) {
            s.groom->setBakedSimulation(n, nullptr);
            s.file.simCaches.erase(n);
        }
        const std::map<std::string, SimCacheRef> after = s.file.simCaches;
        Session* ps = &s;
        commitStep(s, "clear baked simulation", {}, [ps, before, after, cleared](bool redo) { setBakes(*ps, redo ? after : before, cleared); });
        r["clearedBakes"] = cleared;
    } else {
        r["clearedBakes"] = json::array();
    }
    r["nodes"] = nodes;
    r["status"] = simStatusJson(s);
    r["undo"] = undoStateJson(s);
    return r;
}

json setScalpFrames(Session& s, const json& args, BlockReader& blocks) {
    s.simJob.reset();
    if (!args.contains("points") || args["points"].is_null()) {
        s.scalpFrames.clear();
        s.scalpFramesRest.clear();
        applyScalpFrames(s);
        return {{"frames", 0}};
    }
    const MeshPtr& scalp = s.groom->rawScalp();
    if (!scalp) fail("state", "the session has no scalp: set_scalp first");
    const std::size_t n = scalp->pointCount();
    const std::vector<Vec3f> all = pointsToGroom(readArray<float>(args["points"], "points", 3, blocks), s.space);
    if (n == 0 || all.size() % n != 0) {
        fail("bad_request", "points must hold the scalp's " + std::to_string(n) + " points for each frame");
    }
    std::vector<Vec3f> rest;
    if (args.contains("restPoints") && !args["restPoints"].is_null()) {
        rest = pointsToGroom(readArray<float>(args["restPoints"], "restPoints", 3, blocks), s.space);
        if (rest.size() != all.size()) fail("bad_request", "restPoints must hold as many points as points");
    }
    const std::size_t frames = all.size() / n;
    s.scalpFramesFirst = static_cast<int>(std::lround(optNumber(args, "first", 1.0)));
    s.scalpFrames.assign(frames, {});
    s.scalpFramesRest.assign(rest.empty() ? 0 : frames, {});
    for (std::size_t f = 0; f < frames; ++f) {
        s.scalpFrames[f].assign(all.begin() + static_cast<std::ptrdiff_t>(f * n), all.begin() + static_cast<std::ptrdiff_t>((f + 1) * n));
        if (!rest.empty()) {
            s.scalpFramesRest[f].assign(rest.begin() + static_cast<std::ptrdiff_t>(f * n), rest.begin() + static_cast<std::ptrdiff_t>((f + 1) * n));
        }
    }
    applyScalpFrames(s);
    return {{"first", s.scalpFramesFirst}, {"last", s.scalpFramesFirst + static_cast<int>(frames) - 1}, {"frames", frames}};
}

// ---- Colliders, instance prototypes, other scalps (Maya's collision.py, extra_scalps.py) ---------

Vec3f directionToGroom(const HostSpace& space, float x, float y, float z) { return space.zUp() ? Vec3f{x, z, -y} : Vec3f{x, y, z}; }

// A host mesh as an Instance node's prototype: per face corner its point, normal and uv (hard
// edges stay hard, as Maya reads its scene meshes), fans of its polygons; in groom space (the
// host's up is the prototype's +Y, its long axis).
InstancePrototype prototypeFrom(const json& m, const HostSpace& space, BlockReader& blocks) {
    const std::vector<Vec3f> points = pointsToGroom(readArray<float>(need(m, "points"), "points", 3, blocks), space);
    const std::vector<std::int32_t> counts = readArray<std::int32_t>(need(m, "faceCounts"), "faceCounts", 1, blocks);
    const std::vector<std::int32_t> indices = readArray<std::int32_t>(need(m, "faceIndices"), "faceIndices", 1, blocks);
    std::size_t corners = 0;
    for (const std::int32_t c : counts) {
        if (c < 3) fail("bad_request", "a face has fewer than 3 corners");
        corners += static_cast<std::size_t>(c);
    }
    if (corners != indices.size()) fail("bad_request", "the face counts do not add up to the face indices");
    for (const std::int32_t i : indices) {
        if (i < 0 || static_cast<std::size_t>(i) >= points.size()) fail("bad_request", "a face index is not one of the points");
    }
    std::vector<float> normals, uvs;
    if (m.contains("normals") && !m["normals"].is_null()) {
        normals = readArray<float>(m["normals"], "normals", 3, blocks);
        if (normals.size() != 3 * corners) fail("bad_request", "normals must hold 3 floats per face corner");
    }
    if (m.contains("uvs") && !m["uvs"].is_null()) {
        uvs = readArray<float>(m["uvs"], "uvs", 2, blocks);
        if (uvs.size() != 2 * corners) fail("bad_request", "uvs must hold 2 floats per face corner");
    }
    InstancePrototype p;
    p.points.reserve(corners);
    std::size_t start = 0;
    for (const std::int32_t count : counts) {
        const auto n = static_cast<std::size_t>(count);
        const auto base = static_cast<std::uint32_t>(p.points.size());
        Vec3f face{0, 0, 0};
        for (std::size_t k = 1; k + 1 < n; ++k) {
            const Vec3f& a = points[static_cast<std::size_t>(indices[start])];
            face += (points[static_cast<std::size_t>(indices[start + k])] - a).cross(points[static_cast<std::size_t>(indices[start + k + 1])] - a);
        }
        face = face.length() > 1e-20f ? face.normalized() : Vec3f{0, 1, 0};
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = start + k;
            p.points.push_back(points[static_cast<std::size_t>(indices[c])]);
            p.normals.push_back(normals.empty() ? face : directionToGroom(space, normals[3 * c], normals[3 * c + 1], normals[3 * c + 2]));
            if (!uvs.empty()) p.uvs.push_back({uvs[2 * c], uvs[2 * c + 1]});
        }
        for (std::size_t k = 1; k + 1 < n; ++k) {
            p.triangles.insert(p.triangles.end(), {base, base + static_cast<std::uint32_t>(k), base + static_cast<std::uint32_t>(k + 1)});
        }
        start += n;
    }
    std::uint64_t signature = hash64(0x9607);
    for (const Vec3f& q : p.points) signature = hashFloat(hashFloat(hashFloat(signature, q.x), q.y), q.z);
    for (const Vec3f& q : p.normals) signature = hashFloat(hashFloat(hashFloat(signature, q.x), q.y), q.z);
    for (const std::uint32_t v : p.triangles) signature = hashCombine(signature, v);
    p.signature = signature ? signature : 1;
    p.found = !p.triangles.empty();
    return p;
}

// A mesh entry: a whole mesh (faceCounts given) or the points of the one there (a deforming mesh).
MeshPtr meshEntry(const json& m, const MeshPtr& existing, const HostSpace& space, BlockReader& blocks, const std::string& what) {
    if (!m.is_object()) fail("bad_request", what + " is a mesh {points, faceCounts, faceIndices, uvs, restPoints}, or null");
    if (m.contains("faceCounts")) return hostMesh(m, space, blocks);
    if (!existing) fail("bad_request", what + " is new: give its faceCounts and faceIndices");
    std::vector<Vec3f> points = pointsToGroom(readArray<float>(need(m, "points"), "points", 3, blocks), space);
    if (points.size() != existing->pointCount()) fail("bad_request", what + " has " + std::to_string(existing->pointCount()) + " points, not " + std::to_string(points.size()));
    auto mesh = std::make_shared<ScalpMesh>(existing->withPoints(std::move(points)));
    if (m.contains("restPoints") && !m["restPoints"].is_null()) {
        std::vector<Vec3f> rest = pointsToGroom(readArray<float>(m["restPoints"], "restPoints", 3, blocks), space);
        if (rest.size() != mesh->pointCount()) fail("bad_request", what + "'s restPoints must hold as many points as points");
        mesh->setRestPoints(std::move(rest));
    }
    return mesh;
}

json meshesJson(Session& s) {
    json colliders = json::array(), prototypes = json::array(), scalps = json::array();
    for (const auto& [name, mesh] : s.groom->data().meshes) {
        if (mesh) colliders.push_back({{"name", name}, {"points", mesh->pointCount()}, {"triangles", mesh->triangleCount()}, {"rest", mesh->hasRest()}});
    }
    for (const auto& [name, p] : s.file.prototypes) {
        prototypes.push_back({{"name", name}, {"points", p.points.size()}, {"triangles", p.triangles.size() / 3}, {"found", p.found}});
    }
    for (const auto& [name, mesh] : s.groom->data().scalps) {
        if (mesh) scalps.push_back({{"name", name}, {"points", mesh->pointCount()}, {"triangles", mesh->triangleCount()}, {"rest", mesh->hasRest()}});
    }
    return {{"colliders", colliders}, {"prototypes", prototypes}, {"scalps", scalps}};
}

json setMeshes(Session& s, const json& args, BlockReader& blocks) {
    const std::string kind = optString(args, "kind", "collider");
    if (kind != "collider" && kind != "prototype") fail("bad_request", "kind is collider or prototype");
    const json& meshes = need(args, "meshes");
    if (!meshes.is_object()) fail("bad_request", "meshes is {name: mesh or null}");
    const bool replace = optBool(args, "replace", false);
    for (const auto& [name, m] : meshes.items()) {
        if (name.empty() || name == "scalp" || name.find(':') != std::string::npos) fail("bad_request", "pick another mesh name than '" + name + "'");
    }
    if (kind == "collider") {
        std::map<std::string, MeshPtr> next;
        if (!replace) next = s.groom->data().meshes;
        for (const auto& [name, m] : meshes.items()) {
            if (m.is_null()) {
                next.erase(name);
                continue;
            }
            const auto it = s.groom->data().meshes.find(name);
            next[name] = meshEntry(m, it != s.groom->data().meshes.end() ? it->second : nullptr, s.space, blocks, "collider " + name);
        }
        s.groom->setMeshes(std::move(next));
    } else {
        std::map<std::string, InstancePrototype> next;
        if (!replace) next = s.file.prototypes;
        for (const auto& [name, m] : meshes.items()) {
            if (m.is_null()) {
                next.erase(name);
            } else {
                next[name] = prototypeFrom(m, s.space, blocks);
            }
        }
        s.file.prototypes = std::move(next);
    }
    return meshesJson(s);
}

json setScalps(Session& s, const json& args, BlockReader& blocks) {
    const json& list = need(args, "scalps");
    if (!list.is_array()) fail("bad_request", "scalps is [{name, points, faceCounts, faceIndices, uvs, restPoints}] in the order they join");
    NamedScalps scalps;
    std::set<std::string> names;
    const NamedScalps& now = s.groom->data().scalps;
    for (const json& m : list) {
        const std::string name = needString(m, "name");
        if (!validScalpName(name)) fail("bad_request", "pick another scalp name than '" + name + "' (letters, digits and _, not 'scalp')");
        if (!names.insert(name).second) fail("bad_request", "scalp '" + name + "' is given twice");
        MeshPtr existing;
        for (const auto& [n, mesh] : now) {
            if (n == name) existing = mesh;
        }
        scalps.emplace_back(name, meshEntry(m, existing, s.space, blocks, "scalp " + name));
    }
    s.groom->setScalps(std::move(scalps));
    return meshesJson(s);
}

// ---- Transfer Groom to Scalp (Maya's transfer.py, groomLabTransfer) ------------------------------

// Marker pairs given by the host: [{"old": side, "new": side}] or [[old, new]], a side a point
// [x, y, z] or {"point", "face", "u", "v"} in the host's space (face, u, v: the helper's own
// triangles, as an earlier answer gave them); or the stored JSON text.
std::vector<TransferMarker> markersFrom(const json& given, const HostSpace& space) {
    json list = given;
    if (given.is_string()) list = json::parse(given.get<std::string>().empty() ? std::string("[]") : given.get<std::string>());
    if (!list.is_array()) fail("bad_request", "markers is a list of pairs");
    auto side = [&](const json& v) -> json {
        json out = json::object();
        if (v.is_array()) {
            if (v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) fail("bad_request", "a marker's point is [x, y, z]");
            const Vec3f p = space.toGroom(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
            out["point"] = {p.x, p.y, p.z};
            return out;
        }
        if (!v.is_object()) fail("bad_request", "a marker side is a point or {point, face, u, v}");
        out = v;
        if (v.contains("point")) {
            const json& q = v["point"];
            if (!q.is_array() || q.size() != 3) fail("bad_request", "a marker's point is [x, y, z]");
            const Vec3f p = space.toGroom(q[0].get<float>(), q[1].get<float>(), q[2].get<float>());
            out["point"] = {p.x, p.y, p.z};
        }
        return out;
    };
    json pairs = json::array();
    for (const json& m : list) {
        if (m.is_array() && m.size() == 2) {
            pairs.push_back({{"old", side(m[0])}, {"new", side(m[1])}});
        } else if (m.is_object() && m.contains("old") && m.contains("new")) {
            pairs.push_back({{"old", side(m["old"])}, {"new", side(m["new"])}});
        } else {
            fail("bad_request", "a marker pair is {old, new} or [old, new]");
        }
    }
    try {
        return transferMarkersFromJson(pairs.dump());
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
}

json markersJson(const std::vector<TransferMarker>& markers, const HostSpace& space) {
    json out = json::parse(transferMarkersToJson(markers));
    for (json& pair : out) {
        for (const char* k : {"old", "new"}) {
            json& sideJson = pair[k];
            if (!sideJson.contains("point")) continue;
            const json& q = sideJson["point"];
            float h[3];
            space.toHost(Vec3f{q[0].get<float>(), q[1].get<float>(), q[2].get<float>()}, h);
            sideJson["point"] = {h[0], h[1], h[2]};
        }
    }
    return out;
}

// What a transfer changes beside the stored texts, as undo and redo put it: the scalp, its
// topology, the scale reference, the file's caches and bakes (made on the other scalp) and markers.
struct TransferState {
    MeshPtr scalp;
    std::uint64_t fingerprint = 0;
    double scaleReference = 0.0;
    GroomFile caches;  // scalpCache, guideCache, simCaches, transferMarkers
    GuidesPtr flatGuides;  // flat guides as the host gives them now (rooted on the scalp)
    GuidesPtr restGuides;
    MeshPtr restMesh;
};

TransferState transferStateOf(Session& s) {
    TransferState t;
    t.scalp = s.groom->rawScalp();
    t.fingerprint = s.groom->data().scalpFingerprint;
    t.scaleReference = s.groom->data().scaleReference;
    t.caches.scalpCache = s.file.scalpCache;
    t.caches.guideCache = s.file.guideCache;
    t.caches.simCaches = s.file.simCaches;
    t.caches.transferMarkers = s.file.transferMarkers;
    t.flatGuides = s.groom->data().guides;
    t.restGuides = s.restGuides;
    t.restMesh = s.restMesh;
    return t;
}

void putTransferState(Session& s, const TransferState& t) {
    s.groom->setScalp(t.scalp);
    s.groom->setScalpFingerprint(t.fingerprint);
    s.groom->setScaleReference(t.scaleReference);
    s.file.scalpCache = t.caches.scalpCache;
    s.file.guideCache = t.caches.guideCache;
    std::vector<std::string> nodes;
    for (const auto& [n, ref] : s.file.simCaches) nodes.push_back(n);
    for (const auto& [n, ref] : t.caches.simCaches) nodes.push_back(n);
    setBakes(s, t.caches.simCaches, nodes);
    s.file.transferMarkers = t.caches.transferMarkers;
    s.restGuides = t.restGuides;
    s.restMesh = t.restMesh;
    if (t.restGuides) {
        handSessionRestGuides(s);
    } else if (s.groom->data().guides != t.flatGuides) {
        s.groom->setGuides(t.flatGuides);
    }
}

json transfer(Session& s, const json& args, BlockReader& blocks) {
    const MeshPtr from = s.groom->rawScalp();
    if (!from || from->triangleCount() == 0) fail("state", "the groom has no scalp to move from");
    const MeshPtr to = hostMesh(args, s.space, blocks);
    if (to->triangleCount() == 0) fail("bad_request", "the new scalp has no faces");
    TransferOptions options;
    options.method = optBool(args, "fit", false) ? TransferMethod::Bounds : TransferMethod::Closest;
    try {
        if (args.contains("method") && !args["method"].is_null()) options.method = transferMethodFromName(needString(args, "method"));
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    }
    options.keepLengths = optBool(args, "keepLengths", false);
    const bool fitSize = optBool(args, "fitSize", false);
    const bool scaling = s.groom->scaleWithScalp();
    if (fitSize && scaling) options.keepLengths = true;  // the groom's scale grows the guides; the mapping only turns them
    bool markersGiven = false;
    try {
        if (args.contains("markers") && !args["markers"].is_null()) {
            options.markers = snapTransferMarkers(markersFrom(args["markers"], s.space), *from, *to);
            markersGiven = true;
        } else if (usesMarkers(options.method)) {
            options.markers = snapTransferMarkers(s.file.transferMarkers, *from, *to);
        }
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    if (optBool(args, "snapMarkers", false)) return {{"markers", markersJson(options.markers, s.space)}};  // nothing changed
    const std::string requirement = markerRequirement(options.method, options.markers.size());
    if (!requirement.empty()) fail("bad_request", requirement);
    std::vector<std::string> warnings;
    std::unique_ptr<ScalpMapping> mapping;
    try {
        mapping = std::make_unique<ScalpMapping>(*from, *to, options, &warnings);
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    const TransferState before = transferStateOf(s);
    const GroomData& d = s.groom->data();
    const double k = scaling ? s.groom->groomScale() : 1.0;  // the guides move as shown
    std::vector<std::pair<StrokeField, std::string>> texts;
    std::size_t moved = 0;
    json projected = json::array(), missing = json::array();
    try {
        const HierarchyPtr stored = s.groom->storedHierarchy();
        if (stored && !d.hierarchyJson.empty()) {
            GuideHierarchy h = GuideHierarchy::fromJson(d.hierarchyJson);
            h.setScale(static_cast<float>(k));  // moved as shown: the groom keeps its scale
            h.transfer(*mapping);
            moved = h.guides().size();
            texts.emplace_back(StrokeField::Guides, h.toJson());
        }
        if (!d.partsJson.empty()) {
            const PartSet parts = PartSet::fromJson(d.partsJson);
            const PartSet out = parts.transferred(*mapping);
            if (out.parts.size() < parts.parts.size()) warnings.push_back("some parts fell apart on the new scalp and were dropped");
            texts.emplace_back(StrokeField::Parts, out.empty() ? std::string() : out.toJson());
        }
        if (!d.flowJson.empty()) {
            const FlowSet flow = FlowSet::fromJson(d.flowJson);
            const FlowSet out = flow.transferred(*mapping);
            if (out.pins.size() < flow.pins.size() || out.sinks.size() < flow.sinks.size()) {
                warnings.push_back("some flow arrows did not fit the new scalp and were dropped");
            }
            texts.emplace_back(StrokeField::Flow, out.empty() ? std::string() : out.toJson());
        }
        // The painted maps through the same mapping: a vertex map gets values for the new vertices,
        // a texture map a new image at the new scalp's UVs (a free name in the maps folder; the
        // old file is left as it is). Not projected, vertex maps cannot fit another scalp: they go.
        ScalpMaps maps = s.groom->scalpMaps();
        if (!maps.maps.empty()) {
            if (optBool(args, "maps", true)) {
                MapProjectionFiles files;
                files.resolve = [&s](const std::string& stored) { return resolveMapFile(s, stored); };
                const std::string folder = mapsFolderOf(s);
                files.write = [&s, folder](const std::string& name, const TexturePaint& paint) {
                    std::error_code ec;
                    fs::create_directories(fs::u8path(folder), ec);
                    fs::path target = fs::u8path(folder) / fs::u8path(name + ".png");
                    for (int n = 2; fs::exists(target, ec); ++n) target = fs::u8path(folder) / fs::u8path(name + "_" + std::to_string(n) + ".png");
                    const std::string path = target.generic_u8string();
                    sideFileBefore(s, path);  // undo takes the new image away, redo writes it again
                    paint.save(path);
                    return path;
                };
                for (const std::string& name : projectScalpMaps(maps, {}, *mapping, files, &warnings)) projected.push_back(name);
            } else {
                for (auto it = maps.maps.begin(); it != maps.maps.end();) {
                    if (it->second.kind == "vertices") {
                        missing.push_back(it->first);
                        it = maps.maps.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            texts.emplace_back(StrokeField::ScalpMaps, maps.maps.empty() ? std::string() : maps.toJson());
        }
        if (mapping->uvMisses() > 0) {
            warnings.push_back(std::to_string(mapping->uvMisses()) + " roots found no matching UV on the new scalp and went to the closest point");
        }
    } catch (const RequestError&) {
        dropSideFiles(s);
        throw;
    } catch (const std::exception& e) {
        dropSideFiles(s);
        fail("bad_request", e.what());
    }
    // The groom on the new scalp: its topology the groom's now, the scale it had kept (the new
    // scalp is k times its 1x; fitting the size, k grows with the area), the caches and bakes
    // made on the old scalp dropped, flat guides kept at rest rooted on it again.
    TransferState after = before;
    after.scalp = to;
    after.fingerprint = to->topologyFingerprint();
    if (scaling && k > 0.0) {
        double target = k;
        const double oldArea = restSurfaceArea(*from), newArea = restSurfaceArea(*to);
        if (fitSize && oldArea > 0.0 && newArea > 0.0) target *= std::sqrt(newArea / oldArea);
        if (newArea > 0.0) after.scaleReference = newArea / (target * target);
    }
    after.caches.scalpCache = {};
    after.caches.guideCache = {};
    after.caches.simCaches.clear();
    if (markersGiven) after.caches.transferMarkers = options.markers;
    if (before.restGuides) {
        auto guides = std::make_shared<GuideSet>(*before.restGuides);
        guides->rootOnMesh(to->restPose());
        after.restGuides = guides;
        after.restMesh = nullptr;
    } else if (before.flatGuides && !before.flatGuides->empty()) {
        // Flat guides (Maya's guide curves) stay where they are, rooted on the new scalp by position.
        auto guides = std::make_shared<GuideSet>(*before.flatGuides);
        guides->rootOnMesh(*to);
        after.flatGuides = guides;
    }
    try {
        putTransferState(s, after);
        Session* ps = &s;
        commitStep(s, "transfer", texts, [ps, before, after](bool redo) { putTransferState(*ps, redo ? after : before); });
    } catch (const std::exception& e) {
        putTransferState(s, before);
        dropSideFiles(s);
        fail("bad_request", e.what());
    }
    return {{"method", transferMethodName(mapping->method())}, {"guides", moved}, {"projected", projected}, {"missing", missing},
            {"warnings", warnings}, {"markers", markersJson(s.file.transferMarkers, s.space)},
            {"scalp", {{"points", to->pointCount()}, {"triangles", to->triangleCount()}, {"topology", scalpFingerprintText(to->topologyFingerprint())}}},
            {"undo", undoStateJson(s)}};
}

json transferMarkers(Session& s, const json& args) {
    if (args.contains("markers") && !args["markers"].is_null()) {
        // The stored pairs replaced (Maya's set_transfer_markers): one undo step.
        const std::vector<TransferMarker> before = s.file.transferMarkers;
        std::vector<TransferMarker> after = markersFrom(args["markers"], s.space);
        if (const MeshPtr scalp = s.groom->rawScalp()) {
            for (TransferMarker& m : after) {
                if (m.oldRoot.face < 0) {
                    MarkerPoint p;
                    if (snapMarkerPoint(*scalp, m.oldPoint, &p)) m.oldRoot = p.root;
                }
            }
        }
        s.file.transferMarkers = after;
        Session* ps = &s;
        commitStep(s, "transfer markers", {}, [ps, before, after](bool redo) { ps->file.transferMarkers = redo ? after : before; });
        return {{"markers", markersJson(s.file.transferMarkers, s.space)}, {"undo", undoStateJson(s)}};
    }
    return {{"markers", markersJson(s.file.transferMarkers, s.space)}};
}

// ---- Sample meshes (Maya's groomlab.groom SAMPLES) ----------------------------------------------

struct Sample {
    const char* id;
    const char* label;
    const char* file;
    double extentCm;    // its largest extent: it comes in that size in any scene unit
    const char* name;   // the object it is called
    const char* part;   // the part the groom grows on ("" the only one)
    const char* kind;   // "mesh" (an OBJ) or "rig" (an FBX: an armature and the meshes skinned to it)
};
// Centimetres, Y up, facing +Z (CC BY 4.0, Nick Rutlinh). Buddy's OBJ (its body alone, buddy.obj) is the
// standalone app's; the hosts add the rig.
const Sample kSamples[] = {
    {"nika", "Nika (head)", "nika_head.obj", 34.61, "nika_head", "", "mesh"},
    {"manny", "Manny (head)", "manny_head.obj", 34.21, "manny_head", "", "mesh"},
    {"wolf", "Wolf", "wolf.obj", 109.7, "wolf", "wolf_body", "mesh"},
    {"buddy", "Buddy (rig)", "buddy_rig.fbx", 96.65, "buddy_rig", "buddy_body", "rig"},
};

json sampleMeshes() {
    const std::string dir = sampleMeshesDir();
    json samples = json::array();
    for (const Sample& m : kSamples) {
        if (dir.empty()) break;
        const fs::path file = fs::u8path(dir) / m.file;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        samples.push_back({{"id", m.id}, {"label", m.label}, {"file", file.generic_u8string()}, {"extentCm", m.extentCm}, {"name", m.name},
                           {"part", m.part}, {"kind", m.kind}, {"license", "CC BY 4.0"}, {"author", "Nick Rutlinh"}, {"units", "cm"}, {"upAxis", "Y"},
                           {"facing", "+Z"}});
    }
    return {{"dir", dir}, {"samples", samples}};
}

}  // namespace

// ---- Commands -----------------------------------------------------------------------------------------

void applyScalpFrames(Session& s) {
    if (s.scalpFrames.empty()) {
        s.groom->setSimulationFeed({});
        return;
    }
    // A frame Simulate steps through, given by the host's scalp at that frame (the frames a
    // Simulate Range or a jump forward passes over); the session puts its inputs back after.
    Session* ps = &s;
    s.groom->setSimulationFeed([ps](double seconds) {
        Session& s = *ps;
        const double frame = secondsToFrame(seconds, s.groom->framesPerSecond());
        const long whole = std::lround(frame);
        if (std::abs(frame - static_cast<double>(whole)) > 1e-4) return false;
        const long i = whole - s.scalpFramesFirst;
        if (i < 0 || static_cast<std::size_t>(i) >= s.scalpFrames.size()) return false;
        const MeshPtr& scalp = s.groom->rawScalp();
        const std::vector<Vec3f>& points = s.scalpFrames[static_cast<std::size_t>(i)];
        if (!scalp || points.size() != scalp->pointCount()) return false;
        if (static_cast<std::size_t>(i) < s.scalpFramesRest.size()) {
            s.groom->updateScalpPoints(points, &s.scalpFramesRest[static_cast<std::size_t>(i)]);
        } else {
            s.groom->updateScalpPoints(points);
        }
        handSessionRestGuides(s);
        return true;
    });
}

bool isPipelineExport(const std::string& format) {
    return format == "gltfCards" || format == "unrealCards" || format == "unityCards" || format == "unityFeathers" || format == "scalesFbx";
}

json runPipelineExport(Session& s, const json& args) {
    const std::string format = needString(args, "format");
    if (args.contains("frame") && !args["frame"].is_null()) {
        s.seconds = frameToSeconds(optNumber(args, "frame", 1.0), s.groom->framesPerSecond());
    }
    if (format == "gltfCards") return exportGltfCards(s, args);
    if (format == "unrealCards") return exportEngineCards(s, args, false);
    if (format == "unityCards") return exportEngineCards(s, args, true);
    if (format == "unityFeathers") return exportUnityFeathers(s, args);
    return exportScalesFbx(s, args);
}

namespace {

// ---- Bake Node to Guides, Alembic curves as guides ------------------------------------------------

// bake_to_guides (Maya's groomLabGuides -fromNode, bake.bake_guides): a graph node's strands or
// guides ("" the output's), evaluated at `density` with the inputs as they are now, added as primary
// guides of root group `group` ("baked": made when new, added to otherwise). One undo step.
json bakeToGuides(Session& s, const json& args) {
    const MeshPtr scalp = s.groom->scalp();
    if (!scalp || scalp->triangleCount() == 0) fail("state", "the session has no scalp: set_scalp first");
    std::string node = optString(args, "node", "");
    const std::string group = optString(args, "group", "baked");
    if (group.empty()) fail("bad_request", "group needs a name");
    const float density = static_cast<float>(optNumber(args, "density", 1.0));
    if (!(density > 0.0f && density <= 1.0f)) fail("bad_request", "density must be above 0, at most 1");
    const GraphEdit edit(graphTextOf(s));
    if (node.empty()) node = edit.outputNode();  // the groom's output, whatever its name
    const auto names = edit.nodeNames();
    if (std::find(names.begin(), names.end(), node) == names.end()) fail("not_found", "no node '" + node + "'");
    ViewRequest request;
    request.time = s.seconds;
    feedSessionTime(s);
    s.groom->evaluate(request);  // the inputs as they are now (evaluateNode takes the last evaluation's)
    Value value;
    try {
        value = s.groom->evaluateNode(node, density, s.seconds);
    } catch (const std::exception& e) {
        fail("graph", e.what());
    }
    GuideSet baked;
    if (const GuidesPtr* guides = std::get_if<GuidesPtr>(&value); guides && *guides) {
        baked = **guides;
    } else if (const StrandsPtr* strands = std::get_if<StrandsPtr>(&value); strands && *strands) {
        for (std::size_t i = 0; i < (*strands)->strandCount(); ++i) {  // one guide per strand
            const Vec3f* p = (*strands)->strandPoints(i);
            baked.add((*strands)->ids()[i], std::vector<Vec3f>(p, p + (*strands)->strandPointCount(i)));
        }
    } else {
        fail("state", "node '" + node + "' does not output guides or strands (nothing to bake)");
    }
    if (baked.empty()) fail("state", "node '" + node + "' made no guides");
    const HierarchyPtr authored = s.groom->authoredHierarchy();
    GuideHierarchy h = authored ? *authored : GuideHierarchy{};
    std::vector<GuideId> added;
    try {
        GroupId gid = h.findGroup(group);
        if (gid == kNoGroup) {
            gid = h.addGroup(group);
        } else if (h.groupDepth(gid) != 0) {
            fail("bad_request", "'" + group + "' is a child group: bake into a root group");
        }
        added = h.addGuides(gid, baked);
        h.evaluate(scalp.get());
    } catch (const RequestError&) {
        throw;
    } catch (const std::exception& e) {
        fail("bad_request", e.what());
    }
    commitTexts(s, "bake " + node, {{StrokeField::Guides, h.toJson()}});
    json ids = json::array();
    for (const GuideId id : added) ids.push_back(std::to_string(id));
    return {{"node", node}, {"group", group}, {"guides", added.size()}, {"ids", ids}, {"undo", undoStateJson(s)}};
}

// import_alembic_curves (Maya's guides.import_guides_from_alembic): the graph's "guides" node
// replaced by an ImportCurves node reading `path` (every curve, or those under `object`), rooted on
// the scalp. The curves are read as GroomLab's and Maya's Alembic files have them: Y up,
// centimeters. Not an undo step (the host keeps the graph, as an edit_graph op).
json importAlembicCurves(Session& s, const json& args) {
    const std::string path = needString(args, "path");
    const std::string object = optString(args, "object", "");
    const double samples = optNumber(args, "samples", 24.0);
    if (samples < 0.0 || samples > 1024.0) fail("bad_request", "samples is 0 (as read) to 1024");
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec)) fail("io", "no file at " + path);
    std::size_t curves = 0;
#if GROOMLAB_HAS_ALEMBIC
    try {
        curves = readAlembicCurvesAt(path, s.seconds, object).strandCount();
    } catch (const std::exception& e) {
        fail("io", std::string("cannot read the Alembic curves: ") + e.what());
    }
    if (curves == 0) fail("state", object.empty() ? "the file has no curves" : "the file has no curves under " + object);
#else
    fail("unsupported", "this helper was built without Alembic");
#endif
    json graph = json::parse(graphTextOf(s));
    json nodes = json::array();
    bool hasScalp = false;
    for (const json& n : graph.value("nodes", json::array())) {
        if (n.value("name", std::string()) == "guides") continue;
        hasScalp = hasScalp || n.value("name", std::string()) == "scalp";
        nodes.push_back(n);
    }
    json params = {{"path", path}, {"samples", static_cast<std::int64_t>(std::lround(samples))}};
    if (!object.empty()) params["object"] = object;
    json made = {{"name", "guides"}, {"type", "ImportCurves"}, {"params", params}};
    for (const json& n : graph.value("nodes", json::array()))  // where the Guides node was drawn
        if (n.value("name", std::string()) == "guides" && n.contains("ui")) made["ui"] = n["ui"];
    nodes.push_back(made);
    graph["nodes"] = nodes;
    json connections = json::array();
    for (const json& c : graph.value("connections", json::array()))
        if (c.value("to", std::string()) != "guides") connections.push_back(c);
    if (hasScalp) connections.push_back({{"from", "scalp"}, {"to", "guides"}, {"input", "scalp"}});
    graph["connections"] = connections;
    try {
        s.groom->setGraphJson(graph.dump());
    } catch (const std::exception& e) {
        fail("graph", e.what());
    }
    return {{"node", "guides"}, {"curves", curves}, {"path", path}, {"graphError", s.groom->graphError()}};
}

}  // namespace

bool isPipelineCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {
        "bake_scale_maps", "scale_mesh", "strand_mesh", "set_scalp_frames", "simulate_status", "simulate_range", "simulate_poll",
        "simulate_cancel", "simulate_bake", "simulate_clear", "set_meshes", "set_scalps", "meshes_info", "transfer",
        "transfer_markers", "sample_meshes", "cards_preset", "bake_to_guides", "import_alembic_curves"};
    return kCommands.count(cmd) > 0;
}

json runPipelineCommand(const std::string& cmd, const json& args, BlockReader& blocks, const std::function<Session&()>& session) {
    if (cmd == "sample_meshes") return sampleMeshes();
    Session& s = session();
    if (cmd == "bake_scale_maps") return bakeScaleMapsNow(s, args);
    if (cmd == "scale_mesh") {
        MeshPtr scalp;
        const auto mesh = scalesNow(s, &scalp);
        return {{"meshes", putMeshes(s, {scalesMesh(*mesh, "scales")}, groomSpaceOf(args))}};
    }
    if (cmd == "strand_mesh") return strandMesh(s, args);
    if (cmd == "set_scalp_frames") return setScalpFrames(s, args, blocks);
    if (cmd == "simulate_status") {
        json r = jobJson(s, static_cast<bool>(s.simJob));
        r["nodes"] = s.groom->simulateNodes();
        r["scalpFrames"] = s.scalpFrames.empty() ? json() : json{{"first", s.scalpFramesFirst},
                                                                  {"last", s.scalpFramesFirst + static_cast<int>(s.scalpFrames.size()) - 1}};
        r["bakes"] = json::object();
        for (const auto& [node, ref] : s.file.simCaches) r["bakes"][node] = {{"file", ref.file}, {"object", ref.object}, {"stamp", std::to_string(ref.stamp)}};
        return r;
    }
    if (cmd == "simulate_range") {
        if (s.groom->simulateNodes().empty()) fail("state", "the graph has no Simulate node making the output");
        auto job = std::make_unique<SimJob>();
        job->target = static_cast<int>(std::lround(optNumber(args, "last", secondsToFrame(s.seconds, s.groom->framesPerSecond()))));
        job->budgetMs = optNumber(args, "budgetMs", 200.0);
        if (job->budgetMs < 0.0) fail("bad_request", "budgetMs must be 0 (to the end) or more");
        s.simJob = std::move(job);
        s.simJob->from = std::min(simulatedThrough(s, s.simJob->target), s.simJob->target);
        return stepJob(s);
    }
    if (cmd == "simulate_poll") {
        if (!s.simJob) {
            json r = jobJson(s, false);
            r["done"] = true;
            return r;
        }
        if (args.contains("budgetMs") && !args["budgetMs"].is_null()) s.simJob->budgetMs = std::max(0.0, optNumber(args, "budgetMs", 200.0));
        return stepJob(s);
    }
    if (cmd == "simulate_cancel") {
        const bool was = static_cast<bool>(s.simJob);
        json r = jobJson(s, false);
        s.simJob.reset();
        r["cancelled"] = was;
        return r;
    }
    if (cmd == "simulate_bake") return simulateBake(s, args);
    if (cmd == "simulate_clear") return simulateClear(s, args);
    if (cmd == "set_meshes") return setMeshes(s, args, blocks);
    if (cmd == "set_scalps") return setScalps(s, args, blocks);
    if (cmd == "meshes_info") return meshesJson(s);
    if (cmd == "transfer") return transfer(s, args, blocks);
    if (cmd == "cards_preset") return cardsPreset(s, args);
    if (cmd == "bake_to_guides") return bakeToGuides(s, args);
    if (cmd == "import_alembic_curves") return importAlembicCurves(s, args);
    return transferMarkers(s, args);
}

}  // namespace groomlab::helper
