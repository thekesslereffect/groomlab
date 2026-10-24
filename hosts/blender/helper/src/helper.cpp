// SPDX-License-Identifier: Apache-2.0
// groomlab-helper's commands over the core: a GroomSession per host groom, its file's extras
// (caches, bakes), the host's space at the boundary (host_space.h), arrays in shared memory
// (shared_memory.h).
// docs/BLENDER.md describes every command.

#include "helper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "groomlab/attributes.h"
#include "groomlab/card_files.h"
#include "groomlab/card_maps.h"
#include "groomlab/shortcuts.h"
#include "groomlab/color_patterns.h"
#include "groomlab/export_view.h"
#include "groomlab/expression.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/node_library.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/schema.h"
#include "groomlab/usd_writer.h"
#include "groomlab/version.h"

#if GROOMLAB_HAS_ALEMBIC
#include "groomlab/alembic_io.h"
#include "groomlab/sim_cache.h"
#endif

#include "authoring.h"
#include "host_space.h"
#include "pipeline.h"
#include "reference.h"
#include "request.h"
#include "session.h"
#include "shared_memory.h"
#include "tools.h"

namespace groomlab::helper {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- Graph text ---------------------------------------------------------------------------------

bool blankGraph(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    const auto last = text.find_last_not_of(" \t\r\n");
    return text[first] == '{' && text[last] == '}' && text.find_first_not_of(" \t\r\n", first + 1) == last;
}

std::string graphText(const GroomSession& session) {
    const std::string& text = session.data().graphJson;
    return blankGraph(text) ? defaultGraphJson() : text;
}

json parseGraph(const GroomSession& session) {
    json graph = json::parse(graphText(session), nullptr, false);
    if (graph.is_discarded() || !graph.is_object()) fail("graph", "the session's graph is not a JSON object");
    return graph;
}

json& nodeOf(json& graph, const std::string& name) {
    const auto nodes = graph.find("nodes");
    if (nodes != graph.end() && nodes->is_array()) {
        for (json& n : *nodes) {
            if (n.is_object() && n.value("name", std::string()) == name) return n;
        }
    }
    fail("not_found", "no node '" + name + "'");
}

json paramSpec(const json& node, const std::string& param) {
    json spec;
    try {
        spec = json::parse(nodeSpecJson(node.dump()));
    } catch (const std::exception&) {
        return nullptr;
    }
    for (const json& p : spec.value("params", json::array())) {
        if (p.is_object() && p.value("name", std::string()) == param) return p;
    }
    return nullptr;
}

// A map a param reads (map_ref.h), checked before the graph takes it: one source (texture,
// colorSet, attribute, expression, guide) naming something, a known component, numbers for min
// and max, nothing else; an expression that compiles (a vector one for a vec3 param, which
// takes an expression alone: ParamSpec::vectorExpression).
void checkMapValue(const std::string& where, const json& v, bool vector) {
    static const char* const kSources[] = {"texture", "colorSet", "attribute", "expression", "guide"};
    std::string source;
    int sources = 0;
    for (const char* k : kSources) {
        if (v.contains(k)) {
            source = k;
            ++sources;
        }
    }
    if (sources != 1) fail("type_mismatch", where + ": a map names exactly one of texture, colorSet, attribute, expression or guide");
    if (!v[source].is_string() || v[source].get<std::string>().empty()) fail("type_mismatch", where + ": the map's " + source + " is a name");
    for (const auto& [key, value] : v.items()) {
        if (key == source) continue;
        if (key == "component") {
            static const std::set<std::string> kComponents = {"r", "g", "b", "a", "luminance"};
            if (!value.is_string() || !kComponents.count(value.get<std::string>())) {
                fail("type_mismatch", where + ": component is r, g, b, a or luminance");
            }
        } else if (key == "min" || key == "max") {
            if (!value.is_number() || !std::isfinite(value.get<double>())) fail("type_mismatch", where + ": the map's " + key + " is a number");
        } else {
            fail("type_mismatch", where + ": a map has no '" + key + "' (source, component, min, max)");
        }
    }
    if (vector && source != "expression") fail("type_mismatch", where + " is a vector: it reads an expression, not a " + source);
    if (source == "expression") {
        try {
            Expression(v[source].get<std::string>(), {}, vector ? ExpressionType::Vector : ExpressionType::Number);
        } catch (const ExpressionError& e) {
            fail("expression", where + ": " + e.what());
        }
    }
}

const json& operatorSpecs() {
    static const json all = json::parse(operatorSpecsJson());
    return all;
}

// ---- Files --------------------------------------------------------------------------------------

std::string readText(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) fail("io", "cannot read '" + path + "'");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeAtomically(const std::string& path, const std::string& text) {
    const fs::path target = fs::u8path(path);
    const fs::path tmp = tempPathFor(target);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) fail("io", "cannot write '" + path + "'");
        out << text;
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            fail("io", "cannot write '" + path + "'");
        }
    }
    try {
        replaceFile(tmp, target);
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
}

// A path stored relative to a groom file read from `oldDir`, as a file saved in `newDir` stores it
// (paths.h: relative when it can be). Variables and empty paths are kept.
std::string rebase(const std::string& stored, const std::string& oldDir, const std::string& newDir) {
    if (stored.empty() || stored.find('$') != std::string::npos || stored.find('%') != std::string::npos) return stored;
    fs::path p = fs::u8path(stored);
    if (p.is_relative()) {
        if (oldDir.empty()) return stored;
        p = fs::u8path(oldDir) / p;
    }
    return pathFromGroomFolder(p.lexically_normal().generic_u8string(), newDir);
}

std::string platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// ---- Sessions -----------------------------------------------------------------------------------


void applySearchPaths(Session& s) {
    std::vector<std::string> dirs;
    if (!s.file.directory.empty()) dirs.push_back(s.file.directory);
    dirs.insert(dirs.end(), s.file.searchPaths.begin(), s.file.searchPaths.end());
    s.groom->setPathResolver(dirs.empty() ? PathResolver{} : searchPathResolver(std::move(dirs)));
}

// The groom of a file (in groom space) into the session: what GroomScene sets up, but the scalp
// stays the host's to feed.
void adopt(Session& s, GroomFile file) {
    auto session = std::make_unique<GroomSession>(file.data);  // throws for a bad graph or guides
    if (!(file.framesPerSecond > 0.0)) file.framesPerSecond = 24.0;
    session->setFramesPerSecond(file.framesPerSecond);
    std::vector<std::string> notes;
#if GROOMLAB_HAS_ALEMBIC
    for (const auto& [node, ref] : file.simCaches) {
        if (ref.file.empty()) continue;
        try {
            session->setBakedSimulation(node, readSimulationCache(ref.file, ref.object, ref.stamp));
        } catch (const std::exception& e) {
            notes.push_back("the simulation bake of " + node + " did not open: " + e.what());
        }
    }
    std::unique_ptr<AlembicCurvesReader> guideCache;
    if (!file.guideCache.empty()) {
        try {
            guideCache = std::make_unique<AlembicCurvesReader>(file.guideCache.file, file.guideCache.object);
        } catch (const std::exception& e) {
            notes.push_back("the guide cache did not open: " + std::string(e.what()));
        }
    }
    s.guideCache = std::move(guideCache);
#endif
    if (!file.scalpCache.empty()) notes.push_back("the scalp cache is the host's to play: the helper evaluates the scalp it is given");
    const GroomData data = std::move(file.data);
    file.data = GroomData{};
    s.groom = std::move(session);
    adoptMaps(s, data);  // painted maps outlive the host's scalp (no color sets)
    resetTools(s);       // the stroke host was the old groom's
    s.restGuides = nullptr;
    s.restMesh = nullptr;
    s.file = std::move(file);
    s.last = nullptr;
    s.evaluations = s.incrementals = 0;
    s.notes = std::move(notes);
    s.exports.clear();
    s.simJob.reset();
    applySearchPaths(s);
    applyScalpFrames(s);  // the host's scalp at other frames feeds the new groom's Simulate nodes
}

GroomFile currentFile(const Session& s) {
    GroomFile file = s.file;
    file.data = s.groom->data();
    file.data.scalpHistory = nullptr;
    file.data.scaleReference = s.groom->scaleReference();  // in use: the first area measured when none was set
    file.framesPerSecond = s.groom->framesPerSecond();
    writeMaps(s, file);
    return file;
}

// The time's inputs that come with the file (a guide cache's poses).
void feedTime(Session& s) {
#if GROOMLAB_HAS_ALEMBIC
    if (s.guideCache) {
        const StrandSet curves = s.guideCache->at(s.seconds);
        auto poses = std::make_shared<GuideSet>();
        for (std::size_t i = 0; i < curves.strandCount(); ++i) {
            const Vec3f* p = curves.strandPoints(i);
            poses->add(curves.ids()[i], std::vector<Vec3f>(p, p + curves.strandPointCount(i)));
        }
        s.groom->setGuidePoses(poses);
    }
#else
    (void)s;
#endif
}

std::optional<double> scalpSize(const Session& s) {
    // At the size the params are at: the groom's 1x when it is scaled with its scalp.
    const MeshPtr& scalp = s.groom->rawScalp();
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= s.groom->groomScale();
    return size;
}

// The scalp's rest pose as a mesh (the scalp itself without one), kept while it stays the same.
MeshPtr restMeshOf(Session& s) {
    const MeshPtr& scalp = s.groom->rawScalp();
    if (!scalp || !scalp->hasRest()) return scalp;
    const std::uint64_t key = scalp->restPoseHash();
    if (!s.restMesh || s.restMeshKey != key) {
        s.restMesh = std::make_shared<const ScalpMesh>(scalp->restPose());
        s.restMeshKey = key;
    }
    return s.restMesh;
}

// The rest guides handed to the session, carried from the rest pose to the scalp as it is (the
// same guides, bit for bit, while the scalp is at rest).
void handRestGuides(Session& s) {
    if (!s.restGuides) return;
    const MeshPtr& scalp = s.groom->rawScalp();
    const MeshPtr rest = restMeshOf(s);
    GuidesPtr now = s.restGuides;
    if (scalp && rest && rest != scalp) now = carryGuidesBack(s.restGuides, *scalp, *rest);
    if (now != s.groom->data().guides) s.groom->setGuides(now->empty() ? GuidesPtr{} : now);
}

json spaceJson(const HostSpace& space) {
    return {{"upAxis", space.upAxis}, {"metersPerUnit", space.metersPerUnit}, {"centimetersPerUnit", space.scale()}};
}

json groomInfo(Session& s) {
    const GroomData& d = s.groom->data();
    json info;
    info["units"] = "cm";
    info["upAxis"] = "Y";
    info["fps"] = s.groom->framesPerSecond();
    if (const MeshPtr& scalp = s.groom->rawScalp()) {
        info["scalp"] = {{"points", scalp->pointCount()},
                         {"triangles", scalp->triangleCount()},
                         {"polygons", scalp->polygons().size()},
                         {"uvs", scalp->hasUVs()},
                         {"rest", scalp->hasRest()},
                         {"colorSets", scalp->colorSetNames()},
                         {"topology", scalpFingerprintText(scalp->topologyFingerprint())}};
    } else {
        info["scalp"] = nullptr;
    }
    info["flatGuides"] = d.guides ? d.guides->size() : 0;
    const auto stored = s.groom->storedHierarchy();
    info["hierarchyGuides"] = stored ? stored->guides().size() : 0;
    try {
        const GraphEdit edit(graphText(*s.groom));
        info["nodes"] = edit.nodeNames().size();
        info["output"] = edit.outputNode();
    } catch (const std::exception&) {
        info["nodes"] = 0;
    }
    info["followScalp"] = d.followScalp;
    if (!s.file.scalpCache.empty()) info["scalpCache"] = {{"file", s.file.scalpCache.file}, {"object", s.file.scalpCache.object}};
    if (!s.file.guideCache.empty()) info["guideCache"] = {{"file", s.file.guideCache.file}, {"object", s.file.guideCache.object}};
    info["source"] = s.file.source;
    info["notes"] = s.notes;
    std::vector<std::string> errors = s.groom->inputErrors();
    info["inputErrors"] = errors;
    return info;
}

// ---- The scalp ----------------------------------------------------------------------------------

// The mesh made of host polygons (fans from their first corner, as the C API makes them) in groom space.
std::shared_ptr<ScalpMesh> scalpFrom(const json& args, const HostSpace& space, BlockReader& blocks) {
    const std::vector<Vec3f> points = pointsToGroom(readArray<float>(need(args, "points"), "points", 3, blocks), space);
    const std::vector<std::int32_t> counts = readArray<std::int32_t>(need(args, "faceCounts"), "faceCounts", 1, blocks);
    const std::vector<std::int32_t> indices = readArray<std::int32_t>(need(args, "faceIndices"), "faceIndices", 1, blocks);
    std::size_t corners = 0, triangleCount = 0;
    for (std::size_t f = 0; f < counts.size(); ++f) {
        if (counts[f] < 3) fail("bad_request", "face " + std::to_string(f) + " has fewer than 3 corners");
        corners += static_cast<std::size_t>(counts[f]);
        triangleCount += static_cast<std::size_t>(counts[f]) - 2;
    }
    if (corners != indices.size()) {
        fail("bad_request", "the face counts add up to " + std::to_string(corners) + " corners, not the " +
                                std::to_string(indices.size()) + " face indices");
    }
    for (const std::int32_t i : indices) {
        if (i < 0 || static_cast<std::size_t>(i) >= points.size()) {
            fail("bad_request", "face index " + std::to_string(i) + " is not one of the " + std::to_string(points.size()) + " points");
        }
    }
    std::vector<float> uvs;
    if (args.contains("uvs") && !args["uvs"].is_null()) {
        uvs = readArray<float>(args["uvs"], "uvs", 2, blocks);
        if (uvs.size() != 2 * corners) fail("bad_request", "uvs must hold 2 floats per face corner (" + std::to_string(corners) + ")");
    }
    std::map<std::string, std::vector<float>> colorSets;
    if (args.contains("colorSets") && args["colorSets"].is_object()) {
        for (const auto& [name, value] : args["colorSets"].items()) {
            std::vector<float> c = readArray<float>(value, "colorSets", 4, blocks);
            if (c.size() != 4 * corners) fail("bad_request", "color set " + name + " must hold 4 floats per face corner");
            colorSets[name] = std::move(c);
        }
    }
    std::vector<ScalpMesh::Triangle> triangles;
    std::vector<ScalpMesh::UV> cornerUVs;
    std::map<std::string, std::vector<ScalpMesh::Color>> cornerColors;
    std::vector<std::int32_t> polygons;
    triangles.reserve(triangleCount);
    polygons.reserve(counts.size());
    std::size_t start = 0;
    auto corner = [&](std::size_t c) {
        if (!uvs.empty()) cornerUVs.push_back({uvs[2 * c], uvs[2 * c + 1]});
        for (const auto& [name, values] : colorSets) {
            cornerColors[name].push_back({values[4 * c], values[4 * c + 1], values[4 * c + 2], values[4 * c + 3]});
        }
    };
    for (const std::int32_t count : counts) {
        const auto n = static_cast<std::size_t>(count);
        for (std::size_t i = 1; i + 1 < n; ++i) {
            triangles.push_back({indices[start], indices[start + i], indices[start + i + 1]});
            corner(start);
            corner(start + i);
            corner(start + i + 1);
        }
        polygons.push_back(static_cast<std::int32_t>(n - 2));
        start += n;
    }
    auto mesh = std::make_shared<ScalpMesh>(points, std::move(triangles));
    if (!cornerUVs.empty()) mesh->setCornerUVs(std::move(cornerUVs));
    for (auto& [name, colors] : cornerColors) mesh->setCornerColors(name, std::move(colors));
    mesh->setPolygons(std::move(polygons));
    if (args.contains("restPoints") && !args["restPoints"].is_null()) {
        std::vector<Vec3f> rest = pointsToGroom(readArray<float>(args["restPoints"], "restPoints", 3, blocks), space);
        if (rest.size() != points.size()) fail("bad_request", "restPoints must hold as many points as points");
        mesh->setRestPoints(std::move(rest));
    }
    return mesh;
}

// The scalp's polygons again (a fan per polygon when its triangles make one), for a host building
// its mesh from a groom file. `exact`: every polygon came back as one face.
void scalpFaces(const ScalpMesh& m, std::vector<std::int32_t>& counts, std::vector<std::int32_t>& indices,
                std::vector<float>& uvs, bool& exact) {
    const auto& tris = m.triangles();
    const auto& polys = m.polygons();
    const auto& cornerUVs = m.cornerUVs();
    const bool hasUVs = m.hasUVs();
    exact = true;
    auto cornerOf = [&](std::size_t tri, int c) {
        indices.push_back(tris[tri][static_cast<std::size_t>(c)]);
        if (hasUVs) {
            uvs.push_back(cornerUVs[3 * tri + static_cast<std::size_t>(c)][0]);
            uvs.push_back(cornerUVs[3 * tri + static_cast<std::size_t>(c)][1]);
        }
    };
    auto triangle = [&](std::size_t tri) {
        counts.push_back(3);
        for (int c = 0; c < 3; ++c) cornerOf(tri, c);
    };
    std::size_t total = 0;
    for (const std::int32_t n : polys) total += static_cast<std::size_t>(std::max(0, n));
    if (polys.empty() || total != tris.size()) {
        exact = polys.empty() || total == tris.size();
        for (std::size_t t = 0; t < tris.size(); ++t) triangle(t);
        return;
    }
    std::size_t t = 0;
    for (const std::int32_t count : polys) {
        const auto n = static_cast<std::size_t>(count);
        bool fan = n >= 1;
        for (std::size_t i = 1; fan && i < n; ++i) {
            fan = tris[t + i][0] == tris[t][0] && tris[t + i][1] == tris[t + i - 1][2];
        }
        if (fan) {
            counts.push_back(static_cast<std::int32_t>(n + 2));
            cornerOf(t, 0);
            cornerOf(t, 1);
            cornerOf(t, 2);
            for (std::size_t i = 1; i < n; ++i) cornerOf(t + i, 2);
        } else {
            exact = false;
            for (std::size_t i = 0; i < n; ++i) triangle(t + i);
        }
        t += n;
    }
}

// ---- Guides -------------------------------------------------------------------------------------

// Points closer than this (centimeters) are the host's round trip of the same guide, not an edit:
// the guide keeps its stored points and root, bit for bit.
constexpr float kSameGuide = 1e-4f;

bool samePoints(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].x - b[i].x) > kSameGuide || std::abs(a[i].y - b[i].y) > kSameGuide ||
            std::abs(a[i].z - b[i].z) > kSameGuide) {
            return false;
        }
    }
    return true;
}

struct GuideCurves {
    std::vector<std::vector<Vec3f>> points;  // groom space
    std::vector<std::uint64_t> ids;          // 0: new
};

GuideCurves guideCurves(const json& args, const HostSpace& space, BlockReader& blocks) {
    const std::vector<float> xyz = readArray<float>(need(args, "points"), "points", 3, blocks);
    const std::vector<std::int32_t> counts = readArray<std::int32_t>(need(args, "counts"), "counts", 1, blocks);
    std::vector<std::uint64_t> ids;
    if (args.contains("ids") && !args["ids"].is_null()) {
        ids = readArray<std::uint64_t>(args["ids"], "ids", 1, blocks);
        if (ids.size() != counts.size()) fail("bad_request", "ids must hold one ID per curve");
    } else {
        ids.assign(counts.size(), 0);
    }
    std::size_t total = 0;
    for (const std::int32_t n : counts) {
        if (n < 2) fail("bad_request", "a guide needs 2 points or more");
        total += static_cast<std::size_t>(n);
    }
    if (total * 3 != xyz.size()) {
        fail("bad_request", "the counts add up to " + std::to_string(total) + " points, not the " + std::to_string(xyz.size() / 3) + " given");
    }
    GuideCurves c;
    c.ids = std::move(ids);
    std::size_t at = 0;
    for (const std::int32_t n : counts) {
        std::vector<Vec3f> pts(static_cast<std::size_t>(n));
        for (auto& p : pts) {
            p = space.toGroom(xyz[3 * at], xyz[3 * at + 1], xyz[3 * at + 2]);
            ++at;
        }
        c.points.push_back(std::move(pts));
    }
    std::set<std::uint64_t> seen;
    for (const std::uint64_t id : c.ids) {
        if (id != 0 && !seen.insert(id).second) fail("bad_request", "guide ID " + std::to_string(id) + " is given twice");
    }
    return c;
}

json setFlatGuides(Session& s, GuideCurves curves, bool atRest) {
    const GuidesPtr old = atRest && s.restGuides ? s.restGuides : s.groom->data().guides;
    std::map<std::uint64_t, const Guide*> before;
    std::uint64_t next = 1;
    if (old) {
        for (const Guide& g : old->guides()) {
            before[g.id] = &g;
            next = std::max(next, g.id + 1);
        }
    }
    for (const std::uint64_t id : curves.ids) next = std::max(next, id + 1);
    auto set = std::make_shared<GuideSet>();
    GuideSet fresh;  // the new and changed ones, rooted together
    std::vector<std::size_t> freshAt;
    json added = json::array();
    std::size_t changed = 0;
    for (std::size_t i = 0; i < curves.points.size(); ++i) {
        std::uint64_t id = curves.ids[i];
        if (id == 0) {
            id = next++;
            added.push_back({{"index", i}, {"id", id}});
        }
        const auto it = before.find(id);
        Guide& g = set->add(id, {});
        if (it != before.end() && samePoints(it->second->points, curves.points[i])) {
            g = *it->second;
            continue;
        }
        if (it != before.end()) {
            g.attributes = it->second->attributes;  // painted values stay with the guide
            for (const auto& [name, values] : it->second->pointAttributes) {
                if (values.size() == curves.points[i].size()) g.pointAttributes[name] = values;
            }
            ++changed;
        }
        g.points = curves.points[i];
        freshAt.push_back(i);
        fresh.add(id, g.points);
    }
    const MeshPtr scalp = atRest ? restMeshOf(s) : s.groom->rawScalp();
    if (scalp && !fresh.empty()) {
        fresh.orientAndRoot(*scalp);  // as Maya roots its guide curves (tip-first ones turned)
        for (std::size_t k = 0; k < freshAt.size(); ++k) {
            Guide& g = set->guides()[freshAt[k]];
            g.points = fresh.guides()[k].points;
            g.root = fresh.guides()[k].root;
        }
    }
    std::size_t removed = 0;
    for (const auto& [id, g] : before) {
        if (std::none_of(set->guides().begin(), set->guides().end(), [id = id](const Guide& n) { return n.id == id; })) ++removed;
    }
    bool same = old && changed == 0 && added.empty() && removed == 0 && old->size() == set->size();
    for (std::size_t i = 0; same && i < set->size(); ++i) same = old->guides()[i].id == set->guides()[i].id;
    if (atRest) {
        if (!same || !s.restGuides) {
            s.restGuides = set;
            handRestGuides(s);
        }
    } else {
        s.restGuides = nullptr;
        if (!same) s.groom->setGuides(set->empty() ? GuidesPtr{} : GuidesPtr(set));
    }
    return {{"pose", atRest ? "rest" : "now"}, {"target", "flat"}, {"guides", set->size()}, {"changed", changed}, {"added", added}, {"removed", removed}, {"updated", !same}};
}

json setHierarchyGuides(Session& s, GuideCurves curves, bool keepMissing, const std::string& groupName = "") {
    const auto stored = s.groom->storedHierarchy();
    GuideHierarchy h = stored ? *stored : GuideHierarchy();
    const HierarchyPtr rooted = s.groom->authoredHierarchy();  // the shapes get_guides gave
    const GuideHierarchy& shapes = rooted ? *rooted : h;
    std::set<GuideId> editable;
    for (const HierarchyGuide& g : shapes.guides()) {
        if (!g.followsParent()) editable.insert(g.id);
    }
    GroupId group = kNoGroup;
    if (!groupName.empty()) {
        // New guides join this group (a root group made when missing): curves drawn in the host as a
        // braid's paths or feathers' shafts (Maya's guides_from_curves group=).
        group = h.findGroup(groupName);
        if (group == kNoGroup) group = h.addGroup(groupName);
    }
    for (const GuideGroup& g : h.groups()) {
        if (group != kNoGroup) break;
        if (g.parent == kNoGroup) {
            group = g.id;
            break;
        }
    }
    json added = json::array();
    std::size_t changed = 0;
    std::set<GuideId> given;
    for (std::size_t i = 0; i < curves.points.size(); ++i) {
        const std::uint64_t id = curves.ids[i];
        if (id == 0 || !h.guide(id)) {
            if (group == kNoGroup) group = h.addGroup("guides");
            const GuideId made = h.addGuide(group, curves.points[i], TubeProfile{}, id);
            added.push_back({{"index", i}, {"id", made}});
            given.insert(made);
            continue;
        }
        given.insert(id);
        const HierarchyGuide* was = shapes.guide(id);
        if (was && samePoints(was->points, curves.points[i])) continue;
        h.setPoints(id, curves.points[i]);
        ++changed;
    }
    std::vector<GuideId> gone;
    if (!keepMissing) {
        for (const GuideId id : editable) {
            if (!given.count(id) && h.guide(id)) gone.push_back(id);
        }
        if (!gone.empty()) h.removeGuides(gone);
    }
    const bool update = changed || !added.empty() || !gone.empty();
    if (update) s.groom->setHierarchyJson(h.toJson());
    return {{"target", "hierarchy"}, {"guides", given.size()}, {"changed", changed}, {"added", added}, {"removed", gone.size()},
            {"updated", update}};
}

// ---- Strands out --------------------------------------------------------------------------------

struct AttributeOut {
    std::string name;
    int scope = 0;  // 0 per strand, 1 per point
    int components = 1;
    const float* data = nullptr;
    bool length = false;  // a length (width): scaled into the host's units
};

json writeStrands(Session& s, const ViewResult& view, const json& args, bool groomSpace, json& result) {
    static const StrandSet kEmpty;
    const StrandSet& st = view.strands ? *view.strands : kEmpty;
    const std::size_t S = st.strandCount(), P = st.pointCount();

    // Which attributes: none by default, a list of names, or "all".
    bool all = false;
    std::set<std::string> wanted;
    if (args.contains("attributes")) {
        const json& a = args["attributes"];
        if (a.is_string() && a.get<std::string>() == "all") {
            all = true;
        } else if (a.is_array()) {
            for (const json& n : a) {
                if (!n.is_string()) fail("bad_request", "attributes must name attributes");
                wanted.insert(n.get<std::string>());
            }
        } else if (!a.is_null()) {
            fail("bad_request", "attributes must be a list of names or \"all\"");
        }
    }
    const bool derived = all || wanted.count("rootUV") || wanted.count("color") || wanted.count("pointColor");
    ExportView ev;
    if (derived) ev = describeExport(st, view.scalp.get(), kExportPointColors);

    std::vector<AttributeOut> attrs;
    json available = json::array();
    std::set<std::pair<std::string, int>> taken;
    auto offer = [&](const std::string& name, int scope, int components, const float* data, bool length = false) {
        if (!taken.insert({name, scope}).second) return;
        available.push_back({{"name", name}, {"scope", scope ? "point" : "strand"}, {"components", components}});
        if (data && (all || wanted.count(name))) attrs.push_back({name, scope, components, data, length});
    };
    for (const std::string& name : st.strandChannelNames()) {
        const std::vector<float>* v = st.strandChannel(name);
        if (v && v->size() == S) offer(name, 0, 1, v->data());
    }
    for (const std::string& name : st.pointChannelNames()) {
        const std::vector<float>* v = st.pointChannel(name);
        if (v && v->size() == P) offer(name, 1, 1, v->data());
    }
    if (derived) {
        if (ev.rootUVs.size() == S && S) offer("rootUV", 0, 2, ev.rootUVs.front().data());
        if (ev.colors.size() == S && S) offer("color", 0, 3, ev.colors.front().data());
        if (ev.pointColors.size() == P && P) offer("pointColor", 1, 3, ev.pointColors.front().data());
    } else {
        if (view.scalp && view.scalp->hasUVs()) offer("rootUV", 0, 2, nullptr);
        offer("color", 0, 3, nullptr);
    }
    // With FeatherColor's iridescence: each strand's root normal (a feather's vane normal, else the
    // scalp's under the root, else its first segment's direction), the normal the iridescence's
    // angle is taken from (Arnold's root_normal user data, arnold_curves.h setStrandData).
    std::vector<float> rootNormals;
    const std::vector<float>* iridescence = st.pointChannel(kIridescenceChannel);
    if (iridescence && iridescence->size() == P && S) {
        if (all || wanted.count("rootNormal")) {
            rootNormals.resize(S * 3);
            const VaneNormals vanes(st);
            const ScalpMesh* scalp = view.scalp.get();
            for (std::size_t i = 0; i < S; ++i) {
                Vec3f up{0.0f, 1.0f, 0.0f};
                if (vanes.at(i, &up)) {
                } else if (scalp && i < st.roots().size() && scalp->contains(st.roots()[i])) {
                    up = scalp->normal(st.roots()[i]);
                } else if (st.strandPointCount(i) >= 2) {
                    up = (st.strandPoints(i)[1] - st.strandPoints(i)[0]).normalized();
                }
                if (groomSpace) {
                    rootNormals[i * 3] = up.x;
                    rootNormals[i * 3 + 1] = up.y;
                    rootNormals[i * 3 + 2] = up.z;
                } else {
                    s.space.directionToHost(up, &rootNormals[i * 3]);
                }
            }
        }
        offer("rootNormal", 0, 3, rootNormals.empty() ? nullptr : rootNormals.data());
    }
    for (const std::string& name : wanted) {
        if (!taken.count({name, 0}) && !taken.count({name, 1})) result["missingAttributes"].push_back(name);
    }

    const bool wide = P > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    std::size_t bytes = Arena::reserve(P * 12) + Arena::reserve(S * 4) + Arena::reserve((S + 1) * (wide ? 8 : 4)) +
                        Arena::reserve(S * 8) + Arena::reserve(P * 4);
    for (const AttributeOut& a : attrs) bytes += Arena::reserve((a.scope ? P : S) * a.components * 4);
    s.strands.begin(bytes);

    json arrays;
    arrays["positions"] = putPoints(s.strands, st.positions(), s.space, groomSpace);
    std::vector<std::int32_t> counts(S);
    for (std::size_t i = 0; i < S; ++i) counts[i] = static_cast<std::int32_t>(st.strandPointCount(i));
    arrays["pointCounts"] = put(s.strands, counts.data(), S, 1);
    if (wide) {
        std::vector<std::int64_t> offsets(S + 1, 0);
        for (std::size_t i = 0; i < S; ++i) offsets[i] = static_cast<std::int64_t>(st.strandBegin(i));
        offsets[S] = static_cast<std::int64_t>(P);
        arrays["offsets"] = put(s.strands, offsets.data(), S + 1, 1);
    } else {
        std::vector<std::int32_t> offsets(S + 1, 0);
        for (std::size_t i = 0; i < S; ++i) offsets[i] = static_cast<std::int32_t>(st.strandBegin(i));
        offsets[S] = static_cast<std::int32_t>(P);
        arrays["offsets"] = put(s.strands, offsets.data(), S + 1, 1);
    }
    static_assert(sizeof(StrandId) == 8, "strand IDs go out as <u8");
    arrays["ids"] = put(s.strands, reinterpret_cast<const std::uint64_t*>(st.ids().data()), S, 1);
    if (st.widths().size() == P) {
        if (groomSpace) {
            arrays["widths"] = put(s.strands, st.widths().data(), P, 1);
        } else {
            std::vector<float> w(P);
            for (std::size_t i = 0; i < P; ++i) w[i] = s.space.lengthToHost(st.widths()[i]);
            arrays["widths"] = put(s.strands, w.data(), P, 1);
        }
    }
    json attributes = json::object();
    for (const AttributeOut& a : attrs) {
        json ref = put(s.strands, a.data, a.scope ? P : S, static_cast<std::size_t>(a.components));
        ref["scope"] = a.scope ? "point" : "strand";
        attributes[a.name] = ref;
    }
    arrays["attributes"] = attributes;
    result["attributesAvailable"] = available;
    result["strandCount"] = S;
    result["pointCount"] = P;
    return arrays;
}

bool sameTopology(const StrandsPtr& a, const StrandsPtr& b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->strandCount() != b->strandCount() || a->pointCount() != b->pointCount()) return false;
    return (&a->ids() == &b->ids() || a->ids() == b->ids()) && (&a->offsets() == &b->offsets() || a->offsets() == b->offsets());
}

// ---- Exports ------------------------------------------------------------------------------------

PrototypeLookup storedPrototypes(const Session& s) {
    return [&s](const std::string& name) -> const InstancePrototype* {
        const auto it = s.file.prototypes.find(name);
        return it != s.file.prototypes.end() && it->second.found ? &it->second : nullptr;
    };
}

// Unity's hair package resamples strands to 3..128 particles (HairSim.MIN/MAX_STRAND_PARTICLE_COUNT),
// as Maya's groomlab.game checks a count it is given.
std::size_t unityPoints(const json& args, const char* key) {
    const double v = optNumber(args, key, 0.0);
    if (v != 0.0 && (v < 3.0 || v > 128.0 || v != std::floor(v))) {
        fail("bad_request", std::string(key) + " must be 3 to 128 (Unity's strand particle counts), or 0: as they are");
    }
    return static_cast<std::size_t>(v);
}

#if GROOMLAB_HAS_ALEMBIC
// The engine writer's options as Maya's export_unreal_groom and export_unity_strands take them
// (groomlab/game.py): groups ("auto", a channel, "" one object), shafts, a barb lod or density
// and width, Unreal's feather guides and iridescence as AO, Unity's scale and resampling.
EngineExportOptions engineOptions(const json& args, bool unreal) {
    EngineExportOptions e;
    e.engine = unreal ? EngineExportOptions::Engine::Unreal : EngineExportOptions::Engine::Unity;
    const std::string groups = optString(args, "groups", "auto");
    e.groups = groups == "auto" ? std::string(kGroupIdChannel) : groups;
    if (args.contains("shafts") && !args["shafts"].is_null()) e.shafts = optBool(args, "shafts", true);
    // A lighter groom for heavy shots: the barbs thinned to the lod's share (by ID), each kept one
    // wider by 1/sqrt(share) so the vanes keep their look (game.py UNREAL_LODS, _barb_lod).
    double density = 1.0;
    bool given = false;
    const std::string lod = optString(args, "lod", "");
    if (!lod.empty()) {
        static const std::map<std::string, double> kLods = {{"high", 1.0}, {"mid", 0.5}, {"low", 0.25}};
        const auto it = kLods.find(lod);
        if (it == kLods.end()) fail("bad_request", "lod is high, mid or low");
        density = it->second;
        given = true;
    }
    if (args.contains("barbDensity") && !args["barbDensity"].is_null()) {
        density = optNumber(args, "barbDensity", 1.0);
        given = true;
    }
    if (!(density > 0.0 && density <= 1.0)) fail("bad_request", "barbDensity must be above 0 and at most 1");
    e.barbDensity = static_cast<float>(density);
    e.barbWidthScale = static_cast<float>(optNumber(args, "barbWidthScale", given ? 1.0 / std::sqrt(density) : 1.0));
    if (!(e.barbWidthScale > 0.0f)) fail("bad_request", "barbWidthScale must be above 0");
    if (unreal) {
        e.featherGuides = optBool(args, "featherGuides", false);
        e.iridescenceAo = optBool(args, "iridescenceAo", false);
        // From groom space (Y up, centimeters: Blender's (x, y, z) meters is (x, z, -y) times 100
        // times the unit scale) into Unreal's, (x, z, y): Blender's (x, -y, z) times 100 times the
        // unit scale, as Unreal's FBX importer brings in the scene's meshes from Blender's FBX.
        e.upAxis = "Y";
        e.centimetersPerUnit = 1.0;
    } else {
        e.scale = static_cast<float>(optNumber(args, "scale", 1.0));
        if (!(e.scale > 0.0f)) fail("bad_request", "scale must be above 0");
        e.pointsPerStrand = unityPoints(args, "pointsPerStrand");
        if (!e.groups.empty()) {
            e.barbPoints = unityPoints(args, "barbPoints");
            const double most = optNumber(args, "maxStrandsPerGroup", static_cast<double>(kUnityMaxStrandsPerGroup));
            if (most < 0.0) fail("bad_request", "maxStrandsPerGroup must be 0 (no limit) or more");
            e.maxStrandsPerGroup = static_cast<std::size_t>(most);
        } else if (optNumber(args, "barbPoints", 0.0) != 0.0) {
            fail("bad_request", "barbPoints needs groups (the barbs' own curves objects)");
        }
    }
    const std::string problem = e.problem();
    if (!problem.empty()) fail("bad_request", problem);
    return e;
}
#endif

std::unique_ptr<ExportJob> openExport(Session& s, const json& args) {
    std::string format = optString(args, "format", "alembic");
    const bool engine = format == "unrealGroom" || format == "unityStrands";
    if (format != "alembic" && format != "usd" && format != "scalesUsd" && format != "vrscene" && !engine) {
        fail("bad_request", "format is alembic, usd, scalesUsd, vrscene, unrealGroom, unityStrands, gltfCards, unrealCards, "
                            "unityCards, unityFeathers or scalesFbx");
    }
    // The scales: Maya's export_alembic / export_usd's `scales` ("Scales Instancer (USD)" is a USD
    // export with the scales as a PointInstancer of their plates).
    const std::string scales = optString(args, "scales", format == "scalesUsd" ? "instancer" : "mesh");
    if (format == "scalesUsd") format = "usd";
    auto job = std::make_unique<ExportJob>();
    job->format = format;
    job->path = needString(args, "path");
    job->fps = s.groom->framesPerSecond();
    const std::string name = optString(args, "name", "groom");
    const double start = optNumber(args, "startFrame", 1.0);
    const double step = optNumber(args, "step", 1.0);
    if (!(step > 0.0)) fail("bad_request", "step must be above 0");
    try {
        if (format == "usd") {
            UsdCurvesWriter::Options o;
            o.framesPerSecond = job->fps;
            o.metersPerUnit = 0.01;  // groom space: centimeters, Y up
            o.upAxis = "Y";
            o.comment = s.file.source.empty() ? std::string("GroomLab for Blender") : s.file.source;
            o.prototypes = storedPrototypes(s);
            if (scales == "mesh") {
                o.scales = UsdCurvesWriter::Options::Scales::Mesh;
            } else if (scales == "instancer") {
                o.scales = UsdCurvesWriter::Options::Scales::Instancer;
            } else if (scales == "both") {
                o.scales = UsdCurvesWriter::Options::Scales::Both;
            } else if (scales == "none") {
                o.scales = UsdCurvesWriter::Options::Scales::None;
            } else {
                fail("bad_request", "scales is mesh, instancer, both or none");
            }
            job->usd = std::make_unique<UsdCurvesWriter>(job->path, name, o);
        } else if (format == "vrscene") {
            VrsceneWriter::Options o;
            o.framesPerSecond = job->fps;
            o.metersPerUnit = 0.01;  // groom space: centimeters, Y up (the Node's transform the identity)
            o.upAxis = "Y";
            // units "scene": the strands in the scene's unit, still Y up (the add-on's V-Ray Scene
            // object turns them Z up: vray.py; V-Ray does not scale hair widths with a transform),
            // times `scale` (the scalp object's own scale, which that object's transform leaves out).
            const std::string units = optString(args, "units", "groom");
            if (units == "scene") {
                o.scale = 1.0 / s.space.scale();
                o.metersPerUnit = s.space.metersPerUnit;
            } else if (units != "groom") {
                fail("bad_request", "units is groom or scene");
            }
            const double scale = optNumber(args, "scale", 1.0);
            if (!(scale > 0.0)) fail("bad_request", "scale must be above 0");
            o.scale *= scale;
            o.comment = s.file.source.empty() ? std::string("GroomLab for Blender") : s.file.source;
            job->vrscene = std::make_unique<VrsceneWriter>(job->path, name, o);
        } else if (engine) {
#if GROOMLAB_HAS_ALEMBIC
            job->unreal = format == "unrealGroom";
            job->engine = std::make_unique<EngineGroomWriter>(job->path, name, frameToSeconds(start, job->fps),
                                                              frameToSeconds(step, job->fps), engineOptions(args, job->unreal));
#else
            fail("unsupported", "this helper was built without Alembic");
#endif
        } else {
            if (scales != "mesh" && scales != "none") fail("bad_request", "scales is mesh or none (an instancer goes to USD)");
            job->withScales = scales == "mesh";
#if GROOMLAB_HAS_ALEMBIC
            job->abc = std::make_unique<AlembicCurvesWriter>(job->path, name, frameToSeconds(start, job->fps),
                                                             frameToSeconds(step, job->fps));
#else
            fail("unsupported", "this helper was built without Alembic");
#endif
        }
    } catch (const RequestError&) {
        throw;
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    return job;
}

// The primary guides as curves (what Unreal simulates): the hierarchy's level 0.
StrandSet primaryCurves(const HierarchyPtr& hierarchy) {
    StrandSet out;
    if (!hierarchy) return out;
    for (const Guide& g : hierarchy->flatten(0).guides()) {
        if (g.points.size() >= 2) out.addStrand(g.id, g.points, {}, g.root);
    }
    return out;
}

std::size_t exportFrame(Session& s, ExportJob& job, double frame, float density) {
    if (frame <= job.lastFrame) fail("bad_request", "export frames must increase");
    s.seconds = frameToSeconds(frame, s.groom->framesPerSecond());
    feedTime(s);
    ViewRequest request;
    request.density = density;
    request.time = s.seconds;
    request.wantBounds = false;
    const ViewResult& view = s.groom->evaluate(request);
    if (!view.strands) {
        fail("evaluation", view.errors.empty() ? std::string("the groom gave no strands") : view.errors.front());
    }
    const StrandSet& st = *view.strands;
    const MeshPtr scalp = view.scalp ? view.scalp : s.groom->rawScalp();
    bool scaled = job.withScales && st.instances() && st.instances()->hasMeshes();
#if GROOMLAB_HAS_ALEMBIC
    scaled = scaled && !job.engine;
#endif
    const auto scaleMesh = scaled ? job.scales.build(st, scalp, storedPrototypes(s)) : nullptr;
    try {
        if (job.usd) job.usd->writeFrame(frame, st, scalp.get(), scaleMesh.get());
        if (job.vrscene) {
            if (job.frames == 0) job.vrscene->setLobes(hairLobes(s.groom->hairLook()));  // HairColor's highlight
            job.vrscene->writeFrame(frame, st, scalp.get());
        }
#if GROOMLAB_HAS_ALEMBIC
        if (job.abc) job.abc->writeFrame(st, scalp.get(), scaleMesh.get());
        if (job.engine) {
            const StrandSet primaries = job.unreal ? primaryCurves(view.hierarchy) : StrandSet{};
            job.engine->writeFrame(st, job.unreal ? &primaries : nullptr, scalp.get());
        }
#endif
    } catch (const std::invalid_argument& e) {
        fail("bad_request", e.what());
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    job.lastFrame = frame;
    ++job.frames;
    return st.strandCount();
}

// What an export wrote, as its close answers: the path and frames, and for an engine's groom the
// objects its first frame decided (Unity's curves objects, Unreal's groups: Maya's -report).
json closeExport(ExportJob& job) {
    json r = {{"path", job.path}, {"frames", job.frames}};
    try {
        if (job.usd) job.usd->close();
        if (job.vrscene) job.vrscene->close();
#if GROOMLAB_HAS_ALEMBIC
        if (job.abc) job.abc->close();
        if (job.engine) {
            job.engine->close();
            json objects = json::array();
            for (const EngineGroomWriter::Object& o : job.engine->objects()) {
                objects.push_back({{"object", o.path}, {"group", o.group}, {"groupId", o.groupId}, {"feathers", o.feathers},
                                   {"strands", o.strands}, {"pointsPerStrand", o.pointsPerStrand ? json(o.pointsPerStrand) : json()},
                                   {"minPoints", o.minPoints}, {"maxPoints", o.maxPoints}});
            }
            r["objects"] = objects;
        }
#endif
    } catch (const std::exception& e) {
        fail("io", e.what());
    }
    return r;
}

// ---- Presets ------------------------------------------------------------------------------------

std::string g_executableDir;

std::string shippedPresetDir() {
    std::error_code ec;
    if (!g_executableDir.empty()) {
        for (const fs::path& p : {fs::u8path(g_executableDir) / "presets", fs::u8path(g_executableDir) / ".." / "presets"}) {
            if (fs::is_directory(p, ec)) return p.lexically_normal().generic_u8string();
        }
    }
#ifdef GROOMLAB_SHIPPED_PRESETS
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_PRESETS), ec)) return GROOMLAB_SHIPPED_PRESETS;
#endif
    return {};
}

std::vector<std::string> presetDirs() { return presetSearchDirs(presetUserDir(), shippedPresetDir()); }

// The compound nodes shipped with the helper (node_library.h): bin/nodes beside it (or one up),
// else the source tree's (a build tree's helper).
std::string shippedNodesDir() {
    std::error_code ec;
    if (!g_executableDir.empty()) {
        for (const fs::path& p : {fs::u8path(g_executableDir) / "nodes", fs::u8path(g_executableDir) / ".." / "nodes"}) {
            if (fs::is_directory(p, ec)) return p.lexically_normal().generic_u8string();
        }
    }
#ifdef GROOMLAB_SHIPPED_NODES
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_NODES), ec)) return GROOMLAB_SHIPPED_NODES;
#endif
    return {};
}

// A preset's file: by "path", else by "name" in the preset folders.
std::string presetFile(const json& args, std::string* stem) {
    std::string path = optString(args, "path", "");
    if (path.empty()) {
        const std::string name = needString(args, "name");
        path = presetPath(name, presetDirs());
        if (path.empty()) fail("not_found", "no preset named '" + name + "'");
    }
    if (stem) *stem = fs::u8path(path).stem().u8string();
    return path;
}

// A preset's graph in the session, fitted to its scalp, its control values set where the graph
// holds them (as every host applies one).
json applyPreset(Session& s, const json& args) {
    std::string stem;
    const std::string path = presetFile(args, &stem);
    const std::string text = readText(path);
    const std::optional<double> size = scalpSize(s);
    const std::string graph = completeGraphJson(applyPresetControlsToGraph(presetGraphJson(text, size), presetControlsJson(text, size)));
    s.groom->setGraphJson(graph);
    const json preset = json::parse(text, nullptr, false);
    double factor = 1.0;
    if (size && preset.is_object() && preset.contains("size") && preset["size"].is_number() && preset["size"].get<double>() > 0.0) {
        factor = *size / preset["size"].get<double>();
    }
    json missing = json::array();
    const MapCatalog& catalog = s.groom->data().maps;
    for (const PresetMap& m : presetRequiredMaps(text)) {
        if (!catalog.count(m.name) && !s.groom->scalpMaps().catalog().count(m.name)) {
            json entry = {{"name", m.name}, {"kind", m.kind}, {"value", m.value}};
            if (!m.color.empty()) entry["color"] = m.color;  // the add-on makes it a color map filled with this
            missing.push_back(std::move(entry));
        }
    }
    // The guide groups it reads (the add-on makes the missing ones, empty; "draw": its Draw tool
    // then draws into that group).
    json groups = json::array();
    for (const PresetGroup& g : presetRequiredGroups(text)) groups.push_back({{"name", g.name}, {"draw", g.draw}});
    return {{"preset", stem},
            {"fitFactor", factor},
            {"nodes", GraphEdit(graph).nodeNames().size()},
            {"missingMaps", missing},
            {"groups", groups}};
}

// ---- State --------------------------------------------------------------------------------------

std::string base64Encode(const std::string& bytes) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t(std::uint8_t(bytes[i])) << 16) | (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8) |
                                std::uint32_t(std::uint8_t(bytes[i + 2]));
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < bytes.size()) {
        std::uint32_t v = std::uint32_t(std::uint8_t(bytes[i])) << 16;
        if (i + 1 < bytes.size()) v |= std::uint32_t(std::uint8_t(bytes[i + 1])) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = value(c);
        if (v < 0) fail("bad_request", "a texture's png is not base64");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// The groom as a .groom text (groom space) with its texture maps' images: what the host keeps in
// its own file (a .blend) and set_state takes back.
json getState(Session& s, const json& args) {
    GroomFile file = currentFile(s);
    if (file.source.empty()) file.source = "Blender";
    // "scalp": true (the default), false, or "maps": only while maps painted on the scalp's
    // vertices live in its color sets (a host that feeds its own scalp keeps the state small:
    // the scalp as JSON is most of it on a dense mesh).
    bool keepScalp = true;
    if (args.contains("scalp") && args["scalp"].is_string() && args["scalp"].get<std::string>() == "maps") {
        keepScalp = false;
        for (const auto& [name, info] : file.data.maps) keepScalp = keepScalp || info.kind == "vertices";
    } else {
        keepScalp = optBool(args, "scalp", true);
    }
    if (!keepScalp) file.data.scalp = nullptr;
    json textures = json::array();
    if (optBool(args, "textures", true)) {
        for (const auto& [name, info] : file.data.maps) {
            if (info.kind != "texture" || info.texture.empty()) continue;
            const std::string full = resolveMapFile(s, info.texture);
            std::error_code ec;
            if (full.empty() || !fs::is_regular_file(fs::u8path(full), ec)) continue;
            textures.push_back({{"name", name}, {"stored", info.texture}, {"png", base64Encode(readText(full))}});
        }
    }
    json r;
    r["format"] = "groomlab.groom";
    r["groom"] = groomFileToJson(file);
    r["textures"] = textures;
    r["tools"] = toolStateJson(s);
    if (optBool(args, "texts", false)) {
        // The stored inputs the tools edit, as the session holds them (tests, debugging).
        const GroomData& d = s.groom->data();
        r["texts"] = {{"guideData", d.hierarchyJson},
                      {"partData", d.partsJson},
                      {"flowData", d.flowJson},
                      {"correctiveData", d.correctivesJson},
                      {"scalpMaps", s.groom->scalpMapsJson()},
                      {"graph", d.graphJson},
                      {"sculptData", d.sculptsJson}};
    }
    return r;
}

// A state from getState back into the session: the groom as load_groom adopts a file (the
// session's scalp kept with `keepScalp`, as the host feeds its own), its textures written into the
// maps folder.
json setState(Session& s, const json& args) {
    const json& g = need(args, "groom");
    const std::string text = g.is_string() ? g.get<std::string>() : g.dump();
    GroomFile file = groomFileFromJson(text);
    if (file.units != "cm") fail("unsupported", "the state's units are " + file.units + "; GroomLab keeps grooms in centimeters");
    convertUpAxis(file, "Y");
    if (args.contains("textures") && args["textures"].is_array()) {
        const fs::path folder = fs::u8path(mapsFolderOf(s));
        for (const json& t : args["textures"]) {
            const std::string name = needString(t, "name");
            const auto it = file.data.maps.find(name);
            if (it == file.data.maps.end()) continue;
            const std::string bytes = base64Decode(needString(t, "png"));
            std::error_code ec;
            fs::create_directories(folder, ec);
            fs::path target = folder / (name + ".png");
            for (int k = 2; fs::exists(target, ec); ++k) target = folder / (name + "_" + std::to_string(k) + ".png");
            writeAtomically(target.u8string(), bytes);
            it->second.texture = target.generic_u8string();
        }
    }
    const MeshPtr scalp = s.groom->rawScalp();
    const bool keepScalp = optBool(args, "keepScalp", true) && scalp;
    const double fps = s.groom->framesPerSecond();
    if (!(file.framesPerSecond > 0.0)) file.framesPerSecond = fps;
    adopt(s, std::move(file));
    if (keepScalp) s.groom->setScalp(scalp);
    if (args.contains("tools")) restoreToolState(s, args["tools"]);
    json info = groomInfo(s);
    info["space"] = spaceJson(s.space);
    return info;
}

// The sample meshes shipped with the helper (Maya's hosts/maya/meshes, CC BY 4.0): bin/meshes
// beside it (or one up), else the source tree's (a build tree's helper).
std::string shippedMeshesDir() {
    std::error_code ec;
    if (!g_executableDir.empty()) {
        for (const fs::path& p : {fs::u8path(g_executableDir) / "meshes", fs::u8path(g_executableDir) / ".." / "meshes"}) {
            if (fs::is_directory(p, ec)) return p.lexically_normal().generic_u8string();
        }
    }
#ifdef GROOMLAB_SHIPPED_MESHES
    if (fs::is_directory(fs::u8path(GROOMLAB_SHIPPED_MESHES), ec)) return GROOMLAB_SHIPPED_MESHES;
#endif
    return {};
}

}  // namespace

// ---- Shared with pipeline.cpp ---------------------------------------------------------------------

MeshPtr hostMesh(const json& args, const HostSpace& space, BlockReader& blocks) { return scalpFrom(args, space, blocks); }
void hostMeshFaces(const ScalpMesh& mesh, std::vector<std::int32_t>& counts, std::vector<std::int32_t>& indices,
                   std::vector<float>& uvs, bool& exact) {
    scalpFaces(mesh, counts, indices, uvs, exact);
}
void feedSessionTime(Session& s) { feedTime(s); }
PrototypeLookup sessionPrototypes(const Session& s) { return storedPrototypes(s); }
void handSessionRestGuides(Session& s) { handRestGuides(s); }
std::string sampleMeshesDir() { return shippedMeshesDir(); }

// ---- The helper ---------------------------------------------------------------------------------

void setExecutableDir(const std::string& dir) {
    g_executableDir = dir;
    setShippedNodeDir(shippedNodesDir());  // what "node:<id>" adds resolve through
}

Helper::Helper() = default;
Helper::~Helper() = default;

json Helper::malformed(const std::string& why) {
    return {{"id", nullptr}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", why}}}};
}

json Helper::handle(const json& request) {
    json id = nullptr;
    try {
        if (!request.is_object()) return malformed("a request is a JSON object");
        if (request.contains("id")) id = request["id"];
        const auto cmd = request.find("cmd");
        if (cmd == request.end() || !cmd->is_string()) fail("bad_request", "a request needs \"cmd\"");
        json result = run(cmd->get<std::string>(), request);
        return {{"id", id}, {"ok", true}, {"result", std::move(result)}};
    } catch (const RequestError& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", e.code()}, {"message", e.what()}}}};
    } catch (const GraphError& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "graph"}, {"message", e.what()}}}};
    } catch (const GroomFileError& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "parse"}, {"message", e.what()}}}};
    } catch (const SchemaError& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "parse"}, {"message", e.what()}}}};
    } catch (const json::exception& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", e.what()}}}};
    } catch (const std::invalid_argument& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "bad_request"}, {"message", e.what()}}}};
    } catch (const std::bad_alloc&) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "out_of_memory"}, {"message", "out of memory"}}}};
    } catch (const std::exception& e) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "internal"}, {"message", e.what()}}}};
    } catch (...) {
        return {{"id", id}, {"ok", false}, {"error", {{"code", "internal"}, {"message", "unknown exception"}}}};
    }
}

Session& Helper::session(const json& args) {
    const json& v = need(args, "session");
    if (!v.is_number_integer()) fail("bad_request", "\"session\" must be a session's number");
    const auto it = sessions_.find(v.get<int>());
    if (it == sessions_.end()) fail("no_session", "no session " + v.dump());
    return *it->second;
}

json Helper::run(const std::string& cmd, const json& args) {
    BlockReader blocks;

    // ---- The process ----
    if (cmd == "hello") {
        if (args.contains("protocol") && args["protocol"].is_number_integer() && args["protocol"].get<int>() != kProtocolVersion) {
            fail("protocol", "the helper speaks protocol " + std::to_string(kProtocolVersion) + ", not " + args["protocol"].dump() +
                                 ": install the GroomLab helper that comes with this add-on");
        }
        json r;
        r["protocol"] = kProtocolVersion;
        r["helper"] = "groomlab-helper";
        r["version"] = kVersionString;
        r["versionNumbers"] = {kVersionMajor, kVersionMinor, kVersionPatch};
        r["platform"] = platformName();
#ifdef _WIN32
        r["sharedMemory"] = "windows";
#else
        r["sharedMemory"] = "posix";
#endif
        r["alembic"] = static_cast<bool>(GROOMLAB_HAS_ALEMBIC);
        r["groomSpace"] = {{"upAxis", "Y"}, {"units", "cm"}};
        // What this helper offers beyond protocol 1's first commands, each with its own version
        // (raised when its commands change in a way an add-on must know): GroomLab's own tools
        // (new_groom, tool_*, stroke_*, select, guides_edit, maps, undo, display), the state, card
        // maps, authoring and the pipeline (game exports, simulation, meshes, transfer: pipeline.cpp).
        r["features"] = {{"tools", kToolsVersion}, {"state", 1}, {"cardMaps", 1}, {"authoring", 1}, {"pipeline", 1}, {"photos", 1}};
        return r;
    }
    if (cmd == "ping") return {{"pong", true}};
    if (cmd == "shutdown") {
        done_ = true;
        sessions_.clear();
        return {{"bye", true}};
    }

    // ---- Sessions ----
    if (cmd == "new_session") {
        auto s = std::make_unique<Session>(nextSession_);
        if (args.contains("space") && args["space"].is_object()) {
            const json& sp = args["space"];
            s->space = HostSpace::make(optString(sp, "upAxis", "Z"), optNumber(sp, "metersPerUnit", 1.0));
        }
        const double fps = optNumber(args, "fps", 24.0);
        if (!(fps > 0.0)) fail("bad_request", "fps must be above 0");
        s->groom->setFramesPerSecond(fps);
        s->file.framesPerSecond = fps;
        const int id = nextSession_++;
        json r = {{"session", id}, {"space", spaceJson(s->space)}};
        sessions_[id] = std::move(s);
        return r;
    }
    if (cmd == "close_session") {
        Session& s = session(args);
        sessions_.erase(s.id);
        return {{"closed", true}};
    }
    if (cmd == "set_options") {
        Session& s = session(args);
        if (args.contains("followScalp") && !args["followScalp"].is_null()) {
            if (!args["followScalp"].is_boolean()) fail("bad_request", "followScalp is true or false");
            s.groom->setFollowScalp(args["followScalp"].get<bool>());
        }
        if (args.contains("mapsDir") && !args["mapsDir"].is_null()) s.mapsDir = optString(args, "mapsDir", "");
        return {{"followScalp", s.groom->followScalp()}, {"mapsDir", mapsFolderOf(s)}};
    }
    if (cmd == "set_space") {
        Session& s = session(args);
        s.space = HostSpace::make(optString(args, "upAxis", "Z"), optNumber(args, "metersPerUnit", 1.0));
        return {{"space", spaceJson(s.space)}};
    }

    // ---- Groom files ----
    if (cmd == "load_groom") {
        Session& s = session(args);
        std::error_code ec;
        const std::string path = fs::absolute(fs::u8path(needString(args, "path")), ec).u8string();
        if (!fs::is_regular_file(fs::u8path(path), ec)) fail("io", "no groom file '" + path + "'");
        GroomFile file = readGroomFile(path);
        if (file.units != "cm") {
            fail("unsupported", path + ": its units are " + file.units + "; GroomLab reads grooms in centimeters (Maya's own unit)");
        }
        const std::string fileAxis = file.upAxis.empty() ? "Y" : file.upAxis;
        convertUpAxis(file, "Y");  // groom space; the host's axis is turned at the boundary
        adopt(s, std::move(file));
        s.seconds = 0.0;
        json info = groomInfo(s);
        info["fileUpAxis"] = fileAxis;
        info["space"] = spaceJson(s.space);
        return info;
    }
    if (cmd == "save_groom") {
        Session& s = session(args);
        const std::string path = needString(args, "path");
        const std::string axis = optString(args, "upAxis", "Y");
        if (axis != "Y" && axis != "Z") fail("bad_request", "upAxis is \"Y\" or \"Z\"");
        std::error_code ec;
        const fs::path target = fs::absolute(fs::u8path(path), ec);
        const std::string newDir = target.parent_path().generic_u8string();
        const std::string oldDir = s.file.directory;
        // The caches, bakes and their paths as a file in the new folder stores them.
        GroomFile& f = s.file;
        f.scalpCache.file = rebase(f.scalpCache.file, oldDir, newDir);
        f.guideCache.file = rebase(f.guideCache.file, oldDir, newDir);
        f.cardBake.prefix = rebase(f.cardBake.prefix, oldDir, newDir);
        for (auto& [node, ref] : f.simCaches) ref.file = rebase(ref.file, oldDir, newDir);
        // The graph's relative paths (texture maps, ImportCurves) still resolve where they did: the
        // folder it was read from becomes a search path of the file (after its own folder).
        if (!oldDir.empty() && fs::path(fs::u8path(oldDir)).lexically_normal() != fs::path(fs::u8path(newDir)).lexically_normal() &&
            std::find(f.searchPaths.begin(), f.searchPaths.end(), oldDir) == f.searchPaths.end()) {
            f.searchPaths.push_back(oldDir);
        }
        GroomFile file = currentFile(s);
        if (file.source.empty()) file.source = "Blender";
        if (axis == "Z") convertUpAxis(file, "Z");
        writeAtomically(target.u8string(), groomFileToJson(file));
        if (f.source.empty()) f.source = file.source;
        f.directory = newDir;
        applySearchPaths(s);
        return {{"path", target.generic_u8string()}, {"upAxis", axis}, {"units", "cm"}};
    }
    if (cmd == "groom_info") {
        Session& s = session(args);
        json info = groomInfo(s);
        info["space"] = spaceJson(s.space);
        return info;
    }

    // ---- The scalp ----
    if (cmd == "set_scalp") {
        Session& s = session(args);
        auto mesh = scalpFrom(args, s.space, blocks);
        const std::uint64_t topology = mesh->topologyFingerprint();
        const std::size_t points = mesh->pointCount(), triangles = mesh->triangleCount();
        const MeshPtr before = s.groom->rawScalp();
        s.groom->setScalp(std::move(mesh));
        // Flat guides keep their roots (a face and its barycentrics) while the scalp keeps its
        // triangles; on other triangles they are rooted again, as Maya roots its guide curves.
        const MeshPtr& now = s.groom->rawScalp();
        std::size_t rerooted = 0;
        const bool otherTriangles = now && (!before || before->topologyHash() != now->topologyHash());
        if (s.restGuides) {
            if (otherTriangles && !s.restGuides->empty()) {
                auto guides = std::make_shared<GuideSet>(*s.restGuides);
                guides->rootOnMesh(*restMeshOf(s));
                rerooted = guides->size();
                s.restGuides = guides;
            }
            handRestGuides(s);
        } else if (const GuidesPtr flat = s.groom->data().guides; flat && !flat->empty() && otherTriangles) {
            auto guides = std::make_shared<GuideSet>(*flat);
            guides->rootOnMesh(*now);
            rerooted = guides->size();
            s.groom->setGuides(guides);
        }
        const std::uint64_t made = s.groom->data().scalpFingerprint;
        return {{"points", points}, {"triangles", triangles}, {"topology", scalpFingerprintText(topology)},
                {"matchesGroom", made == 0 || made == topology}, {"guidesRerooted", rerooted}};
    }
    if (cmd == "update_scalp") {
        Session& s = session(args);
        const MeshPtr& scalp = s.groom->rawScalp();
        if (!scalp) fail("state", "the session has no scalp: set_scalp first");
        std::vector<Vec3f> points = pointsToGroom(readArray<float>(need(args, "points"), "points", 3, blocks), s.space);
        if (points.size() != scalp->pointCount()) {
            fail("bad_request", "the scalp has " + std::to_string(scalp->pointCount()) + " points, not " + std::to_string(points.size()));
        }
        if (args.contains("restPoints") && !args["restPoints"].is_null()) {
            const std::vector<Vec3f> rest = pointsToGroom(readArray<float>(args["restPoints"], "restPoints", 3, blocks), s.space);
            if (rest.size() != points.size()) fail("bad_request", "restPoints must hold as many points as points");
            s.groom->updateScalpPoints(std::move(points), &rest);
        } else {
            s.groom->updateScalpPoints(std::move(points));
        }
        handRestGuides(s);  // guides kept at rest follow the scalp
        return {{"points", s.groom->rawScalp()->pointCount()}};
    }
    if (cmd == "get_scalp") {
        Session& s = session(args);
        const MeshPtr& scalp = s.groom->rawScalp();
        if (!scalp) return {{"scalp", nullptr}};
        const bool groom = optString(args, "space", "host") == "groom";
        std::vector<std::int32_t> counts, indices;
        std::vector<float> uvs;
        bool exact = true;
        scalpFaces(*scalp, counts, indices, uvs, exact);
        const std::size_t n = scalp->pointCount();
        s.scalp.begin(Arena::reserve(n * 12) * 2 + Arena::reserve(counts.size() * 4) + Arena::reserve(indices.size() * 4) +
                      Arena::reserve(uvs.size() * 4));
        json arrays;
        arrays["points"] = putPoints(s.scalp, scalp->points(), s.space, groom);
        arrays["faceCounts"] = put(s.scalp, counts.data(), counts.size(), 1);
        arrays["faceIndices"] = put(s.scalp, indices.data(), indices.size(), 1);
        if (!uvs.empty()) arrays["uvs"] = put(s.scalp, uvs.data(), uvs.size() / 2, 2);
        if (scalp->hasRest()) arrays["restPoints"] = putPoints(s.scalp, scalp->restPoints(), s.space, groom);
        return {{"scalp", {{"points", n}, {"faces", counts.size()}, {"triangles", scalp->triangleCount()}, {"facesExact", exact},
                           {"colorSets", scalp->colorSetNames()}, {"topology", scalpFingerprintText(scalp->topologyFingerprint())}}},
                {"arrays", arrays}};
    }

    // ---- Guides ----
    if (cmd == "get_guides") {
        Session& s = session(args);
        std::string source = optString(args, "source", "auto");
        const GuidesPtr flat = s.groom->data().guides;
        const auto stored = s.groom->storedHierarchy();
        if (source == "auto") source = flat && !flat->empty() ? "flat" : stored && !stored->guides().empty() ? "hierarchy" : "flat";
        if (source != "flat" && source != "hierarchy") fail("bad_request", "source is \"auto\", \"flat\" or \"hierarchy\"");
        const bool groom = optString(args, "space", "host") == "groom";
        std::vector<std::uint64_t> ids;
        std::vector<std::int32_t> counts;
        std::vector<Vec3f> points;
        auto take = [&](std::uint64_t id, const std::vector<Vec3f>& p) {
            ids.push_back(id);
            counts.push_back(static_cast<std::int32_t>(p.size()));
            points.insert(points.end(), p.begin(), p.end());
        };
        const std::string pose = optString(args, "pose", "now");
        if (pose != "now" && pose != "rest") fail("bad_request", "pose is \"now\" or \"rest\"");
        if (source == "flat") {
            const GuidesPtr given = pose == "rest" && s.restGuides ? s.restGuides : flat;
            if (given) {
                for (const Guide& g : given->guides()) take(g.id, g.points);
            }
        } else if (stored) {
            const HierarchyPtr rooted = s.groom->authoredHierarchy();
            const GuideHierarchy& h = rooted ? *rooted : *stored;
            for (const HierarchyGuide& g : h.guides()) {
                if (!g.followsParent()) take(g.id, g.points);
            }
        }
        s.guides.begin(Arena::reserve(points.size() * 12) + Arena::reserve(counts.size() * 4) + Arena::reserve(ids.size() * 8));
        json arrays;
        arrays["points"] = putPoints(s.guides, points, s.space, groom);
        arrays["counts"] = put(s.guides, counts.data(), counts.size(), 1);
        arrays["ids"] = put(s.guides, ids.data(), ids.size(), 1);
        return {{"source", source}, {"pose", source == "flat" && pose == "rest" && s.restGuides ? "rest" : "now"},
                {"guides", ids.size()}, {"points", points.size()}, {"arrays", arrays}};
    }
    if (cmd == "set_guides") {
        Session& s = session(args);
        GuideCurves curves = guideCurves(args, s.space, blocks);
        std::string target = optString(args, "target", "auto");
        if (target == "auto") {
            const GuidesPtr flat = s.groom->data().guides;
            const auto stored = s.groom->storedHierarchy();
            target = stored && !stored->guides().empty() && !(flat && !flat->empty()) ? "hierarchy" : "flat";
        }
        const std::string pose = optString(args, "pose", "now");
        if (pose != "now" && pose != "rest") fail("bad_request", "pose is \"now\" or \"rest\"");
        if (target == "flat") return setFlatGuides(s, std::move(curves), pose == "rest");
        if (target == "hierarchy") {
            return setHierarchyGuides(s, std::move(curves), optBool(args, "keepMissing", false), optString(args, "group", ""));
        }
        fail("bad_request", "target is \"auto\", \"flat\" or \"hierarchy\"");
    }

    // ---- Time and evaluation ----
    if (cmd == "set_time") {
        Session& s = session(args);
        if (args.contains("fps") && !args["fps"].is_null()) {
            const double fps = optNumber(args, "fps", 24.0);
            if (!(fps > 0.0)) fail("bad_request", "fps must be above 0");
            if (fps != s.groom->framesPerSecond()) {
                s.groom->setFramesPerSecond(fps);
                s.file.framesPerSecond = fps;
            }
        }
        const double fps = s.groom->framesPerSecond();
        if (args.contains("frame") && !args["frame"].is_null()) {
            s.seconds = frameToSeconds(optNumber(args, "frame", 1.0), fps);  // every host's rule (frame_time.h)
        } else {
            s.seconds = optNumber(args, "seconds", s.seconds);
        }
        return {{"seconds", s.seconds}, {"frame", secondsToFrame(s.seconds, fps)}, {"fps", fps}};
    }
    if (cmd == "evaluate") {
        Session& s = session(args);
        const std::string mode = optString(args, "mode", "preview");
        if (mode != "preview" && mode != "render") fail("bad_request", "mode is \"preview\" or \"render\"");
        const std::string space = optString(args, "space", "host");
        if (space != "host" && space != "groom") fail("bad_request", "space is \"host\" or \"groom\"");
        ViewRequest request;
        request.viewport = mode == "preview";  // renderOnly nodes off in the preview, viewportOnly ones in renders
        if (mode == "preview") {
            request.density = static_cast<float>(optNumber(args, "density", 1.0));
            request.pointBudget = optNumber(args, "pointBudget", 0.0);
            request.viewNode = optString(args, "viewNode", "");
            // While a stroke is live the strands step back to this fraction (the app's 0.35), so
            // each dab evaluates fast; the evaluation after the stroke has every strand again.
            request.sculptDensity = static_cast<float>(optNumber(args, "sculptDensity", 1.0));
            if (!(request.sculptDensity > 0.0f && request.sculptDensity <= 1.0f)) fail("bad_request", "sculptDensity must be above 0, at most 1");
            if (!(request.density >= 0.0f && request.density <= 1.0f)) fail("bad_request", "density must be between 0 and 1");
            if (request.pointBudget < 0.0) fail("bad_request", "pointBudget must be 0 or more");
        }
        request.time = s.seconds;
        request.wantBounds = optBool(args, "bounds", false);
        const auto t0 = std::chrono::steady_clock::now();
        feedTime(s);
        const ViewResult& view = s.groom->evaluate(request);
        const double evalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        json r;
        json arrays = writeStrands(s, view, args, space == "groom", r);
        r["arrays"] = std::move(arrays);
        r["space"] = space;
        r["seconds"] = s.seconds;
        r["errors"] = view.errors;
        json messages = json::array();
        for (const SessionMessage& m : view.messages) messages.push_back({{"text", m.text}, {"error", m.error}});
        r["messages"] = messages;
        json notes = json::object();
        for (const auto& [node, note] : s.groom->nodeNotes()) notes[node] = note;
        r["notes"] = notes;
        // What changed since the last answer: the add-on keeps its curves when the topology is the same.
        r["serial"] = view.strandsSerial;
        r["changesBase"] = view.changesBase;
        r["changedStrands"] = view.strandsChanges && !view.strandsChanges->all ? json(view.strandsChanges->strands.size()) : json(nullptr);
        r["sameTopology"] = sameTopology(s.last, view.strands);
        const std::size_t evaluations = s.groom->evaluationCount(), incrementals = s.groom->incrementalCount();
        r["nodesEvaluated"] = evaluations >= s.evaluations ? evaluations - s.evaluations : evaluations;
        r["nodesIncremental"] = incrementals >= s.incrementals ? incrementals - s.incrementals : incrementals;
        s.evaluations = evaluations;
        s.incrementals = incrementals;
        s.last = view.strands;
        if (request.wantBounds && view.bounds.valid) {
            float lo[3], hi[3];
            s.space.toHost(view.bounds.lo, lo);
            s.space.toHost(view.bounds.hi, hi);
            r["bounds"] = {{"lo", {std::min(lo[0], hi[0]), std::min(lo[1], hi[1]), std::min(lo[2], hi[2])}},
                           {"hi", {std::max(lo[0], hi[0]), std::max(lo[1], hi[1]), std::max(lo[2], hi[2])}}};
        }
        r["milliseconds"] = evalMs;
        r["following"] = s.groom->following();
        r["look"] = json::parse(s.groom->hairLookJson());
        return r;
    }
    if (cmd == "stats") {
        Session& s = session(args);
        return json::parse(s.groom->stats().toJson());
    }
    if (cmd == "node_status") {
        Session& s = session(args);
        json notes = json::object();
        for (const auto& [node, note] : s.groom->nodeNotes()) notes[node] = note;
        json times = json::object();
        for (const auto& [node, ms] : s.groom->nodeTimes()) times[node] = ms;
        return {{"notes", notes}, {"times", times}, {"errors", s.groom->last().errors}, {"inputErrors", s.groom->inputErrors()},
                {"graphError", s.groom->graphError()}};
    }

    // ---- The graph ----
    if (cmd == "operator_specs") return {{"operators", operatorSpecs()}};
    if (cmd == "shortcuts") {
        // Every host's keys in one table (shortcuts.h): the rows Blender's tool has.
        json rows = json::array();
        for (const Shortcut& r : shortcutsFor(kShortcutBlender)) {
            rows.push_back({{"id", r.id}, {"context", r.context}, {"keys", r.keys}, {"action", r.action}});
        }
        return {{"shortcuts", rows}};
    }
    if (cmd == "default_graph") return {{"graph", json::parse(defaultGraphJson())}};
    if (cmd == "get_graph") {
        Session& s = session(args);
        return {{"graph", parseGraph(*s.groom)}};
    }
    if (cmd == "graph_channels") {
        // The attributes per node as the graph is set (attributes.h), the guide layers as the
        // guides' painted maps.
        Session& s = session(args);
        std::vector<std::string> guideMaps;
        if (const HierarchyPtr h = s.groom->authoredHierarchy()) guideMaps = h->attributeNames();
        return {{"channels", json::parse(graphChannelsJson(parseGraph(*s.groom).dump(), guideMaps))}};
    }
    if (cmd == "set_graph") {
        Session& s = session(args);
        const json& g = need(args, "graph");
        const std::string text = g.is_string() ? g.get<std::string>() : g.dump();
        s.groom->setGraphJson(text);  // GraphError for a bad graph: the session keeps its own
        return {{"nodes", GraphEdit(graphText(*s.groom)).nodeNames().size()}};
    }
    if (cmd == "list_nodes") {
        Session& s = session(args);
        const GraphEdit edit(graphText(*s.groom));
        json nodes = json::array();
        const std::string output = edit.outputNode();
        for (const std::string& name : edit.nodeNames()) {
            nodes.push_back({{"name", name}, {"type", edit.nodeType(name)}, {"version", edit.nodeVersion(name)}, {"output", name == output}});
        }
        return {{"nodes", nodes}, {"output", output}};
    }
    if (cmd == "node_spec") {
        Session& s = session(args);
        const GraphEdit edit(graphText(*s.groom));
        const std::string node = needString(args, "node");
        if (!edit.hasNode(node)) fail("not_found", "no node '" + node + "'");
        return {{"spec", json::parse(edit.nodeSpecJson(node))}};
    }
    if (cmd == "get_param") {
        Session& s = session(args);
        const std::string node = needString(args, "node"), param = needString(args, "param");
        json graph = parseGraph(*s.groom);
        const json& n = nodeOf(graph, node);
        const auto params = n.find("params");
        if (params != n.end() && params->is_object() && params->contains(param)) return {{"value", (*params)[param]}, {"set", true}};
        const json spec = paramSpec(n, param);
        if (spec.is_null()) fail("not_found", "node '" + node + "' has no param '" + param + "'");
        return {{"value", spec.value("default", json(nullptr))}, {"set", false}};
    }
    if (cmd == "set_param") {
        Session& s = session(args);
        const std::string node = needString(args, "node"), param = needString(args, "param");
        const json& value = need(args, "value");
        json graph = parseGraph(*s.groom);
        json& n = nodeOf(graph, node);
        const json spec = paramSpec(n, param);
        if (spec.is_null()) fail("not_found", "node '" + node + "' has no param '" + param + "'");
        const std::string kind = spec.value("kind", std::string());
        const bool vectorExpression = kind == "vec3" && spec.value("vectorExpression", false);
        const bool mapped = (spec.value("mappable", false) || vectorExpression) && value.is_object();
        const bool fits = (kind == "bool" && value.is_boolean()) || (kind == "int" && (value.is_number_integer() || mapped)) ||
                          (kind == "float" && (value.is_number() || mapped)) || (kind == "string" && value.is_string()) ||
                          (kind == "vec3" && ((value.is_array() && value.size() == 3) || mapped)) || kind == "ramp" || kind == "map";
        if (!fits) {
            fail("type_mismatch", node + "." + param + " takes a " + kind +
                                      (spec.value("mappable", false) ? " or a map" : vectorExpression ? " or an expression" : "") + ", not " +
                                      value.dump());
        }
        if (mapped) checkMapValue(node + "." + param, value, vectorExpression);
        json& params = n["params"];
        if (!params.is_object()) params = json::object();
        params[param] = value;
        s.groom->setGraphJson(graph.dump());
        return {{"value", value}};
    }
    if (cmd == "edit_graph") {
        Session& s = session(args);
        const std::string op = needString(args, "op");
        json r;
        // The Graph Editor's authoring (Group, exposed params, a node on a link, sliders, keyed
        // params): one undo step each on the session's stack (authoring.cpp).
        if (runGraphAuthoring(s, op, args, r)) return r;
        GraphEdit edit(graphText(*s.groom));
        if (op == "add_node") {
            const json& params = args.contains("params") && args["params"].is_object() ? args["params"] : json::object();
            std::optional<double> x, y;
            if (args.contains("x") && args["x"].is_number()) x = args["x"].get<double>();
            if (args.contains("y") && args["y"].is_number()) y = args["y"].get<double>();
            r["name"] = edit.addNode(needString(args, "type"), optString(args, "name", ""), params.dump(), x, y, scalpSize(s));
        } else if (op == "remove_node") {
            edit.removeNode(needString(args, "name"));
        } else if (op == "rename_node") {
            edit.renameNode(needString(args, "name"), needString(args, "newName"));
        } else if (op == "connect") {
            // The add-on's links are the user's (its node tree, its chain edits): a Clump fed centers
            // takes them (GraphEdit::connectByHand).
            edit.connectByHand(needString(args, "from"), needString(args, "to"), needString(args, "input"), optString(args, "output", ""));
        } else if (op == "disconnect") {
            r["disconnected"] = edit.disconnect(needString(args, "to"), needString(args, "input"));
        } else if (op == "set_output") {
            edit.setOutputNode(needString(args, "name"));
        } else if (op == "upgrade_node") {
            r["upgraded"] = edit.upgradeNode(needString(args, "name"));
        } else if (op == "update_library_node") {  // a node from the library takes its newer copy
            r["updated"] = edit.updateLibraryNode(needString(args, "name"), scalpSize(s));
        } else if (op == "set_positions" || op == "set_bypass" || op == "set_evaluation") {
            // The editors' fields of a node (graph_io.h): its "ui" position, as Maya's Graph Editor and
            // the app keep it, "bypass" (Disable) and "evaluation" (when it runs). Set here rather
            // than by set_graph so a graph that does not load yet (an input left open) takes them too.
            json g = json::parse(edit.toJson());
            if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
            if (op == "set_positions") {
                // The backdrops framing parts of the graph go with the positions (Maya's
                // GraphModel.save_layout: the graph's "backdrops", layout only): [{"title", "x",
                // "y", "w", "h", "color": [r, g, b] 0-255}], all of them each time ([] none).
                if (args.contains("backdrops") && !args["backdrops"].is_null()) {
                    const json& given = args["backdrops"];
                    if (!given.is_array()) fail("bad_request", "backdrops is [{title, x, y, w, h, color}]");
                    json backdrops = json::array();
                    for (const json& b : given) {
                        if (!b.is_object()) fail("bad_request", "a backdrop is {title, x, y, w, h, color}");
                        if (b.contains("title") && !b["title"].is_string()) fail("bad_request", "a backdrop's title is text");
                        json kept = {{"title", b.value("title", std::string())}};
                        for (const char* k : {"x", "y", "w", "h"}) {
                            if (!b.contains(k) || !b[k].is_number() || !std::isfinite(b[k].get<double>())) {
                                fail("bad_request", std::string("a backdrop's ") + k + " is a number");
                            }
                            kept[k] = b[k].get<double>();
                        }
                        if (!(kept["w"].get<double>() > 0.0) || !(kept["h"].get<double>() > 0.0)) fail("bad_request", "a backdrop's w and h are above 0");
                        if (b.contains("color") && !b["color"].is_null()) {
                            const json& c = b["color"];
                            if (!c.is_array() || c.size() != 3 || !c[0].is_number() || !c[1].is_number() || !c[2].is_number()) {
                                fail("bad_request", "a backdrop's color is [r, g, b] 0-255");
                            }
                            json rgb = json::array();
                            for (const json& x : c) rgb.push_back(static_cast<int>(std::clamp(std::lround(x.get<double>()), 0L, 255L)));
                            kept["color"] = rgb;
                        }
                        backdrops.push_back(std::move(kept));
                    }
                    if (backdrops.empty()) {
                        g.erase("backdrops");
                    } else {
                        g["backdrops"] = std::move(backdrops);
                    }
                }
                // And the pins the links are drawn through (Maya's GraphModel._pins_dict, Blender's
                // reroutes, layout only): {"points": [{"id", "x", "y"}], "routes": [{"from", "output"?,
                // "to", "input", "via": [pin ids]}]}, all of them each time (no routes: none).
                if (args.contains("pins") && !args["pins"].is_null()) {
                    const json& given = args["pins"];
                    if (!given.is_object()) fail("bad_request", "pins is {points, routes}");
                    const json noPoints = json::array(), noRoutes = json::array();
                    const json& givenPoints = given.contains("points") ? given["points"] : noPoints;
                    const json& givenRoutes = given.contains("routes") ? given["routes"] : noRoutes;
                    if (!givenPoints.is_array() || !givenRoutes.is_array()) fail("bad_request", "pins is {points: [...], routes: [...]}");
                    json points = json::array(), routes = json::array();
                    for (const json& p : givenPoints) {
                        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) fail("bad_request", "a pin is {id, x, y}");
                        json kept = {{"id", p["id"]}};
                        for (const char* k : {"x", "y"}) {
                            if (!p.contains(k) || !p[k].is_number() || !std::isfinite(p[k].get<double>())) {
                                fail("bad_request", std::string("a pin's ") + k + " is a number");
                            }
                            kept[k] = p[k].get<double>();
                        }
                        points.push_back(std::move(kept));
                    }
                    for (const json& r : givenRoutes) {
                        if (!r.is_object()) fail("bad_request", "a route is {from, output?, to, input, via}");
                        json kept = json::object();
                        for (const char* k : {"from", "to", "input"}) {
                            if (!r.contains(k) || !r[k].is_string()) fail("bad_request", std::string("a route's ") + k + " is text");
                            kept[k] = r[k];
                        }
                        if (r.contains("output") && !r["output"].is_null()) {
                            if (!r["output"].is_string()) fail("bad_request", "a route's output is text");
                            if (!r["output"].get<std::string>().empty()) kept["output"] = r["output"];
                        }
                        if (!r.contains("via") || !r["via"].is_array() || r["via"].empty()) fail("bad_request", "a route's via is [pin ids]");
                        for (const json& v : r["via"]) {
                            if (!v.is_string()) fail("bad_request", "a route's via is [pin ids]");
                        }
                        kept["via"] = r["via"];
                        routes.push_back(std::move(kept));
                    }
                    if (routes.empty()) {
                        g.erase("pins");
                    } else {
                        g["pins"] = {{"points", std::move(points)}, {"routes", std::move(routes)}};
                    }
                }
                const json positions = args.contains("positions") && !args["positions"].is_null() ? args["positions"] : json::object();
                if (!args.contains("positions") && !args.contains("backdrops") && !args.contains("pins")) need(args, "positions");
                if (!positions.is_object()) fail("bad_request", "positions is {node: [x, y]}");
                for (const auto& [name, at] : positions.items()) {
                    if (!at.is_array() || at.size() != 2 || !at[0].is_number() || !at[1].is_number()) {
                        fail("bad_request", "the position of '" + name + "' is [x, y]");
                    }
                    if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
                }
                for (json& n : g["nodes"]) {
                    const std::string name = n.is_object() ? n.value("name", std::string()) : std::string();
                    if (!positions.contains(name)) continue;
                    n["ui"] = {{"x", positions[name][0].get<double>()}, {"y", positions[name][1].get<double>()}};
                }
            } else if (op == "set_evaluation") {
                // "always", "viewportOnly" (bypassed in renders and exports) or "renderOnly" (in the preview).
                const std::string name = needString(args, "name");
                if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
                const std::string mode = needString(args, "mode");
                try {
                    evaluationModeFromName(mode);
                } catch (const GraphError& e) {
                    fail("bad_request", e.what());
                }
                for (json& n : g["nodes"]) {
                    if (!n.is_object() || n.value("name", std::string()) != name) continue;
                    if (mode == "always") {
                        n.erase("evaluation");
                    } else {
                        n["evaluation"] = mode;
                    }
                }
            } else {
                const std::string name = needString(args, "name");
                if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
                const bool on = optBool(args, "bypass", true);
                for (json& n : g["nodes"]) {
                    if (!n.is_object() || n.value("name", std::string()) != name) continue;
                    if (on) {
                        n["bypass"] = true;
                    } else {
                        n.erase("bypass");
                    }
                }
            }
            edit = GraphEdit(g.dump());
        } else {
            fail("bad_request",
                 "op is add_node, remove_node, rename_node, connect, disconnect, set_output, upgrade_node, set_positions, "
                 "set_bypass or set_evaluation");
        }
        // A graph that does not load yet (an input left open) is kept: the session says why.
        const std::string text = edit.toJson();
        try {
            s.groom->setGraphJson(text);
            r["graphError"] = "";
        } catch (const GraphError& e) {
            s.groom->dropGraph(text, e.what());
            r["graphError"] = e.what();
        }
        return r;
    }

    // ---- Presets ----
    if (cmd == "list_presets") {
        json presets = json::array();
        for (const PresetInfo& p : listPresets(presetDirs())) {
            presets.push_back({{"name", p.name}, {"title", p.title}, {"description", p.description}, {"category", p.category}, {"path", p.path}});
        }
        return {{"presets", presets}, {"categories", presetCategories()}, {"dirs", presetDirs()}};
    }
    if (cmd == "apply_preset") return applyPreset(session(args), args);
    // ---- Compound nodes (node_library.h): the library Maya and the app share; edit_graph's
    // add_node takes a "node:<id>" type ----
    if (cmd == "library_nodes") {
        if (shippedNodeDir().empty()) setShippedNodeDir(shippedNodesDir());
        return json::parse(libraryNodesJson());
    }
    if (cmd == "preset_graph") {
        // A preset's graph as it would apply to this scalp (fitted), nothing changed: the add-on's
        // graph inserts take nodes from it (Maya's presets.preset_for: a feather design).
        Session& s = session(args);
        std::string stem;
        const std::string text = readText(presetFile(args, &stem));
        return {{"preset", stem}, {"graph", json::parse(presetGraphJson(text, scalpSize(s)))}};
    }
    if (cmd == "insert_preset") {
        Session& s = session(args);
        std::string stem;
        const std::string path = presetFile(args, &stem);
        const std::string text = readText(path);
        std::string made;
        const std::string graph = insertPresetJson(graphText(*s.groom), text, scalpSize(s), optString(args, "after", ""),
                                                   optString(args, "nodeName", presetNodeName(stem)), &made);
        s.groom->setGraphJson(graph);
        return {{"preset", stem}, {"node", made}};
    }

    // ---- Exports ----
    if (cmd == "export_open") {
        Session& s = session(args);
        auto job = openExport(s, args);
        const int id = s.nextExport++;
        s.exports[id] = std::move(job);
        return {{"export", id}};
    }
    if (cmd == "export_frame" || cmd == "export_close") {
        Session& s = session(args);
        const json& e = need(args, "export");
        if (!e.is_number_integer() || !s.exports.count(e.get<int>())) fail("not_found", "no export " + e.dump());
        ExportJob& job = *s.exports[e.get<int>()];
        if (cmd == "export_frame") {
            const std::size_t strands =
                exportFrame(s, job, optNumber(args, "frame", secondsToFrame(s.seconds, s.groom->framesPerSecond())),
                            static_cast<float>(optNumber(args, "density", 1.0)));
            return {{"strands", strands}, {"frames", job.frames}};
        }
        const json r = closeExport(job);
        s.exports.erase(e.get<int>());
        return r;
    }
    // ---- Card maps (card_maps.h): the presets every host lists, and an export of the groom's
    // cards' maps as one packs them ----
    if (cmd == "card_map_presets") {
        json r;
        r["presets"] = json::parse(cardMapPresetsJson());
        r["sources"] = json::parse(cardMapSourcesJson());
        r["userDir"] = cardMapPresetUserDir();
        return r;
    }
    if (cmd == "save_card_map_preset") {
        const json& preset = need(args, "preset");
        try {
            return {{"path", saveCardMapPreset(cardMapPresetFromJson(preset.dump()))}};
        } catch (const std::runtime_error& e) {
            fail("bad_request", e.what());
        }
    }
    if (cmd == "delete_card_map_preset") return {{"deleted", deleteCardMapPreset(needString(args, "name"))}};
    if (cmd == "bake_card_maps") {
        Session& s = session(args);
        CardMapPreset preset;
        const json& given = args.contains("preset") ? args["preset"] : json(builtinCardMapPresets().front().name);
        if (given.is_object()) {
            try {
                preset = cardMapPresetFromJson(given.dump());
            } catch (const std::runtime_error& e) {
                fail("bad_request", e.what());
            }
        } else if (const auto found = findCardMapPreset(given.is_string() ? given.get<std::string>() : std::string())) {
            preset = *found;
        } else {
            fail("not_found", "no card map preset named " + given.dump());
        }
        CardMapExport settings;
        settings.resolution = static_cast<int>(optNumber(args, "resolution", 0.0));
        settings.format = optString(args, "format", "");
        settings.bitDepth = static_cast<int>(optNumber(args, "bitDepth", 0.0));
        settings.padding = static_cast<int>(optNumber(args, "padding", -1.0));
        settings.supersample = static_cast<int>(optNumber(args, "supersample", 1.0));
        settings.nameTemplate = optString(args, "nameTemplate", "{name}_{map}");
        const std::string directory = needString(args, "directory");
        const std::string name = optString(args, "name", "groom");
        // The Cards nodes building their cards at full density, at the session's time.
        ViewRequest request;
        request.density = 1.0f;
        request.time = s.seconds;
        request.wantCards = true;
        request.cardDensity = 1.0f;
        request.wantBounds = false;
        feedTime(s);
        const ViewResult& view = s.groom->evaluate(request);
        if (!view.errors.empty()) fail("evaluate", view.errors.front());
        try {
            const CardBake bake = bakeCardMapsInMemory(view.cardStrands, view.scalp ? view.scalp : s.groom->scalp(), settings,
                                                       static_cast<float>(optNumber(args, "occlusion", 1.0)));
            const std::vector<std::string> paths = writeCardMaps(*bake.maps, preset, settings, directory, name);
            return {{"paths", paths}, {"cards", bake.cards->cards.size()}, {"size", bake.maps->size}};
        } catch (const std::runtime_error& e) {
            fail("export", e.what());
        }
    }
    if (cmd == "export") {
        Session& s = session(args);
        // A game export of one frame (cards, the scales' mesh, Unity's feathers): pipeline.cpp.
        if (isPipelineExport(optString(args, "format", "alembic"))) return runPipelineExport(s, args);
        const json frames = args.contains("frames") && args["frames"].is_object() ? args["frames"] : json::object();
        const double fps = s.groom->framesPerSecond();
        const double first = optNumber(frames, "first", secondsToFrame(s.seconds, fps));
        const double last = optNumber(frames, "last", first);
        const double step = optNumber(frames, "step", 1.0);
        if (!(step > 0.0) || last < first) fail("bad_request", "frames: first <= last, step above 0");
        json open = args;
        open["startFrame"] = first;
        open["step"] = step;
        auto job = openExport(s, open);
        const auto count = static_cast<std::size_t>(std::floor((last - first) / step + 1e-6)) + 1;
        std::size_t strands = 0;
        for (std::size_t i = 0; i < count; ++i) {
            strands = exportFrame(s, *job, first + static_cast<double>(i) * step, static_cast<float>(optNumber(args, "density", 1.0)));
        }
        json r = closeExport(*job);
        r["strands"] = strands;
        return r;
    }

    // ---- Grooming in the host: a new groom, its state, GroomLab's own tools (tools.cpp) ----
    if (cmd == "new_groom") {
        // Maya's create_groom: the default graph and no guides (they live in the guide hierarchy,
        // which Place and Draw fill), on the scalp the session has; a preset's graph when named.
        Session& s = session(args);
        const MeshPtr scalp = s.groom->rawScalp();
        const bool follow = s.groom->followScalp();
        GroomFile file;
        file.framesPerSecond = s.groom->framesPerSecond();
        adopt(s, std::move(file));
        if (scalp) s.groom->setScalp(scalp);
        s.groom->setFollowScalp(optBool(args, "followScalp", follow));
        json r;
        if (args.contains("preset") && args["preset"].is_string()) {
            json preset = {{"name", args["preset"]}};
            r["preset"] = applyPreset(s, preset);
        } else if (const std::optional<double> size = scalpSize(s);
                   size && std::abs(*size / kDefaultsScalpSize - 1.0) > 1e-6) {
            // Maya's create_groom(fit=True): the default graph's lengths, widths and densities in
            // proportion to this scalp (they are authored on a 20 cm head).
            s.groom->setGraphJson(defaultGraphJsonFor(size));
        }
        r["groom"] = groomInfo(s);
        r["guides"] = "hierarchy";
        return r;
    }
    if (cmd == "get_state") return getState(session(args), args);
    if (cmd == "set_state") return setState(session(args), args);
    if (cmd == "tool_catalog") return toolCatalogJson();
    if (isPipelineCommand(cmd)) return runPipelineCommand(cmd, args, blocks, [&]() -> Session& { return session(args); });
    if (isReferenceCommand(cmd)) return runReferenceCommand(cmd, args, [&]() -> Session& { return session(args); });
    if (isAuthoringCommand(cmd)) {
        if (shippedNodeDir().empty()) setShippedNodeDir(shippedNodesDir());
        return runAuthoringCommand(cmd, args, [&]() -> Session& { return session(args); });
    }
    if (isToolCommand(cmd)) {
        json r;
        runToolCommand(session(args), cmd, args, blocks, r);
        return r;
    }

    fail("unknown_command", "no command '" + cmd + "'");
}

}  // namespace groomlab::helper
