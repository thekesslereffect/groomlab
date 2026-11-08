// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom_file.h"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef __APPLE__
#include <cstdlib>
#include <string>
#include <xlocale.h>  // strtod_l
#endif

#include <nlohmann/json.hpp>

#include "groomlab/export_view.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/paths.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/simulation.h"
#include "schema_internal.h"

#include "groomlab/guide_hierarchy.h"

namespace groomlab {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kFormat = "groomlab.groom";

// The double that text (std::to_chars's) reads as. Apple's libc++ has no floating-point
// std::from_chars: there strtod in the C locale, which reads it the same (to_chars text is plain).
double readDouble(const char* begin, const char* end) {
#ifdef __APPLE__
    static const locale_t c = newlocale(LC_ALL_MASK, "C", nullptr);
    return strtod_l(std::string(begin, end).c_str(), nullptr, c);
#else
    double read = 0.0;
    std::from_chars(begin, end, read);
    return read;
#endif
}

// Float arrays are written in the shortest form that reads back to the same
// float (std::to_chars), half the size of a double's 17 digits and still
// exact. nlohmann::json would print doubles, so the arrays go through
// FloatArrays: a placeholder string in the document, replaced by the array's
// text once the document is dumped.
class FloatArrays {
public:
    // A placeholder for `count` floats named `what`. Throws GroomFileError on
    // a NaN or infinity: JSON has no spelling for them, and a reader would
    // otherwise get null.
    json add(const float* values, std::size_t count, const char* what) {
        std::string text;
        text.reserve(count * 10 + 2);
        text += '[';
        char buf[32];
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(values[i])) {
                throw GroomFileError(std::string("groom file: '") + what + "' holds a non-finite value at index " +
                                     std::to_string(i));
            }
            if (i) text += ',';
            if (values[i] == 0.0f && std::signbit(values[i])) {
                text += "-0.0";  // "-0" would read back as the integer 0: the sign bit matters to a hash
                continue;
            }
            // The shortest text that reads back as this float. A reader parses it as a double
            // first (JSON), then rounds that to a float: for a very few values (7.038531e-26) the
            // two roundings land on the neighbour, and those are written as their double's text.
            auto r = std::to_chars(buf, buf + sizeof(buf), values[i]);
            const double read = readDouble(buf, r.ptr);
            if (static_cast<float>(read) != values[i]) r = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(values[i]));
            text.append(buf, r.ptr);
        }
        text += ']';
        arrays_.push_back(std::move(text));
        return kToken + std::to_string(arrays_.size() - 1) + "@";
    }

    // The dumped document with every placeholder replaced by its array.
    std::string resolve(const std::string& dumped) const {
        std::string out;
        out.reserve(dumped.size());
        std::size_t pos = 0;
        while (true) {
            const std::size_t at = dumped.find(kQuotedToken, pos);
            if (at == std::string::npos) break;
            const std::size_t digits = at + kQuotedToken.size();
            const std::size_t end = dumped.find("@\"", digits);
            if (end == std::string::npos) break;
            const std::size_t index = std::stoul(dumped.substr(digits, end - digits));
            out.append(dumped, pos, at - pos);
            out += arrays_.at(index);
            pos = end + 2;
        }
        out.append(dumped, pos, std::string::npos);
        return out;
    }

private:
    inline static const std::string kToken = "@groomlab.floats:";
    inline static const std::string kQuotedToken = "\"" + kToken;
    std::vector<std::string> arrays_;
};

json points(FloatArrays& arrays, const std::vector<Vec3f>& pts, const char* what) {
    static_assert(sizeof(Vec3f) == 3 * sizeof(float), "Vec3f must be three packed floats");
    return arrays.add(reinterpret_cast<const float*>(pts.data()), pts.size() * 3, what);
}

std::vector<float> readFloats(const json& a, std::size_t multiple, const char* what) {
    if (!a.is_array() || a.size() % multiple != 0) {
        throw GroomFileError(std::string("groom file: '") + what + "' must be an array of " +
                             std::to_string(multiple) + "-tuples");
    }
    std::vector<float> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const json& v = a[i];
        if (v.is_number()) {
            out[i] = v.get<float>();
        } else if (v.is_null()) {
            out[i] = 0.0f;  // a non-finite value another writer could not spell
        } else {
            throw GroomFileError(std::string("groom file: '") + what + "' must hold numbers");
        }
    }
    return out;
}

std::vector<Vec3f> readPoints(const json& a, const char* what) {
    const std::vector<float> f = readFloats(a, 3, what);
    std::vector<Vec3f> out(f.size() / 3);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = {f[3 * i], f[3 * i + 1], f[3 * i + 2]};
    return out;
}

// A stored prototype mesh, its triangles checked, smooth normals made when it has none, and a
// signature of its content (what keeps a scale's bend from one build to the next).
InstancePrototype readPrototype(const json& m) {
    InstancePrototype p;
    p.points = readPoints(m.at("points"), "prototypes.points");
    const json& t = m.at("triangles");
    if (!t.is_array() || t.size() % 3 != 0) throw GroomFileError("groom file: bad prototypes.triangles");
    p.triangles = t.get<std::vector<std::uint32_t>>();
    for (const std::uint32_t v : p.triangles) {
        if (v >= p.points.size()) throw GroomFileError("groom file: a prototype's triangle names a point it does not have");
    }
    if (m.contains("normals")) p.normals = readPoints(m.at("normals"), "prototypes.normals");
    if (p.normals.size() != p.points.size()) {
        p.normals.assign(p.points.size(), Vec3f{});
        for (std::size_t k = 0; k + 2 < p.triangles.size(); k += 3) {
            const Vec3f& a = p.points[p.triangles[k]];
            const Vec3f face = (p.points[p.triangles[k + 1]] - a).cross(p.points[p.triangles[k + 2]] - a);
            for (int c = 0; c < 3; ++c) p.normals[p.triangles[k + static_cast<std::size_t>(c)]] += face;
        }
        for (Vec3f& n : p.normals) n = n.length() > 1e-20f ? n.normalized() : Vec3f{0, 0, 1};
    }
    if (m.contains("uvs")) {
        const std::vector<float> f = readFloats(m.at("uvs"), 2, "prototypes.uvs");
        if (f.size() == 2 * p.points.size()) {
            p.uvs.resize(p.points.size());
            for (std::size_t i = 0; i < p.uvs.size(); ++i) p.uvs[i] = {f[2 * i], f[2 * i + 1]};
        }
    }
    std::uint64_t signature = hash64(0x9607);
    for (const Vec3f& q : p.points) signature = hashFloat(hashFloat(hashFloat(signature, q.x), q.y), q.z);
    for (const std::uint32_t v : p.triangles) signature = hashCombine(signature, v);
    p.signature = signature ? signature : 1;
    p.found = !p.triangles.empty();
    return p;
}

json cacheJson(const CacheRef& c) { return {{"file", c.file}, {"object", c.object}}; }

// A stored path with its variables expanded, a relative one joined to the groom file's folder.
std::string fromBaseDir(const std::string& stored, const std::string& baseDir) {
    std::string path = expandEnvironment(stored);
    if (!path.empty() && !baseDir.empty() && fs::path(path).is_relative()) {
        path = (fs::path(baseDir) / path).lexically_normal().string();
    }
    return path;
}

CacheRef readCache(const json& root, const char* key, const std::string& baseDir) {
    CacheRef c;
    if (!root.contains(key) || root.at(key).is_null()) return c;
    const json& j = root.at(key);
    c.file = fromBaseDir(j.at("file").get<std::string>(), baseDir);
    c.object = j.value("object", std::string());
    return c;
}

json simCachesJson(const std::map<std::string, SimCacheRef>& caches) {
    json sims = json::object();
    for (const auto& [node, ref] : caches) {
        sims[node] = {{"file", ref.file}, {"object", ref.object}, {"stamp", formatSimulationStamp(ref.stamp)}};
    }
    return sims;
}

std::map<std::string, SimCacheRef> readSimCaches(const json& sims, const std::string& baseDir) {
    std::map<std::string, SimCacheRef> out;
    for (const auto& [node, j] : sims.items()) {
        SimCacheRef ref;
        ref.file = fromBaseDir(j.at("file").get<std::string>(), baseDir);
        ref.object = j.value("object", std::string());
        ref.stamp = parseSimulationStamp(j.value("stamp", std::string()));
        out[node] = std::move(ref);
    }
    return out;
}

// A mesh's shape as groom files keep it (the scalp's, a collider's): points, triangles, the host's
// polygons and render subdivision, the rest pose. UVs and color sets are the scalp's alone.
json meshJson(FloatArrays& arrays, const ScalpMesh& mesh, const std::string& what) {
    json m;
    m["points"] = points(arrays, mesh.points(), (what + ".points").c_str());
    json tris = json::array();
    for (const ScalpMesh::Triangle& t : mesh.triangles()) {
        tris.push_back(t[0]);
        tris.push_back(t[1]);
        tris.push_back(t[2]);
    }
    m["triangles"] = std::move(tris);
    // The host's polygons (triangles per polygon) and the render's subdivision, which a Scalp or
    // ColliderMesh node set to match render refines by (subdivision.h): written only when there are some.
    if (!mesh.polygons().empty()) m["polygons"] = mesh.polygons();
    if (mesh.renderSubdivision() > 0) {
        m["subdivision"] = mesh.renderSubdivision();
        if (!mesh.renderSubdivisionFrom().empty()) m["subdivisionFrom"] = mesh.renderSubdivisionFrom();
    }
    if (mesh.hasRest()) m["restPoints"] = points(arrays, mesh.restPoints(), (what + ".restPoints").c_str());
    return m;
}

// The same back (no rest pose yet: set it after the UVs and color sets, as the hashes expect).
std::shared_ptr<ScalpMesh> readMesh(const json& m, const std::string& what) {
    std::vector<Vec3f> pts = readPoints(m.at("points"), (what + ".points").c_str());
    const json& t = m.at("triangles");
    if (!t.is_array() || t.size() % 3 != 0) throw GroomFileError("groom file: bad " + what + ".triangles");
    std::vector<ScalpMesh::Triangle> tris(t.size() / 3);
    for (std::size_t i = 0; i < tris.size(); ++i) {
        tris[i] = {t[3 * i].get<std::int32_t>(), t[3 * i + 1].get<std::int32_t>(), t[3 * i + 2].get<std::int32_t>()};
    }
    std::shared_ptr<ScalpMesh> mesh;
    try {
        mesh = std::make_shared<ScalpMesh>(std::move(pts), std::move(tris));
    } catch (const std::out_of_range& e) {
        throw GroomFileError("groom file: bad " + what + ".triangles: " + e.what());
    }
    if (m.contains("polygons")) {
        try {
            mesh->setPolygons(m.at("polygons").get<std::vector<std::int32_t>>());
        } catch (const std::invalid_argument& e) {
            throw GroomFileError("groom file: bad " + what + ".polygons: " + e.what());
        }
    }
    if (m.contains("subdivision")) {
        mesh->setRenderSubdivision(m.at("subdivision").get<int>(), m.value("subdivisionFrom", std::string("groom file")));
    }
    return mesh;
}

void readRest(const json& m, ScalpMesh& mesh, const std::string& what) {
    if (!m.contains("restPoints")) return;
    try {
        mesh.setRestPoints(readPoints(m.at("restPoints"), (what + ".restPoints").c_str()));
    } catch (const std::invalid_argument& e) {
        throw GroomFileError("groom file: bad " + what + ".restPoints: " + e.what());
    }
}

// A scalp as groom files keep it (the groom's own, each of its other scalps): its shape
// (meshJson), UVs and color sets.
json scalpJson(FloatArrays& arrays, const ScalpMesh& scalp, const std::string& what) {
    json s = meshJson(arrays, scalp, what);
    if (scalp.hasUVs()) s["uvs"] = arrays.add(scalp.cornerUVs().front().data(), scalp.cornerUVs().size() * 2, (what + ".uvs").c_str());
    json colorSets = json::object();
    for (const std::string& name : scalp.colorSetNames()) {
        const auto* colors = scalp.cornerColors(name);
        const float* values = colors->empty() ? nullptr : colors->front().data();  // a scalp without triangles has none
        colorSets[name] = arrays.add(values, colors->size() * 4, (what + ".colorSets").c_str());
    }
    if (!colorSets.empty()) s["colorSets"] = std::move(colorSets);
    return s;
}

// The same back, with its rest pose.
std::shared_ptr<ScalpMesh> readScalp(const json& s, const std::string& what) {
    const std::shared_ptr<ScalpMesh> scalp = readMesh(s, what);
    if (s.contains("uvs")) {
        const std::vector<float> f = readFloats(s.at("uvs"), 2, (what + ".uvs").c_str());
        std::vector<ScalpMesh::UV> uvs(f.size() / 2);
        for (std::size_t i = 0; i < uvs.size(); ++i) uvs[i] = {f[2 * i], f[2 * i + 1]};
        scalp->setCornerUVs(std::move(uvs));
    }
    if (s.contains("colorSets")) {
        for (const auto& [name, values] : s.at("colorSets").items()) {
            const std::vector<float> f = readFloats(values, 4, (what + ".colorSets").c_str());
            std::vector<ScalpMesh::Color> colors(f.size() / 4);
            for (std::size_t i = 0; i < colors.size(); ++i) {
                colors[i] = {f[4 * i], f[4 * i + 1], f[4 * i + 2], f[4 * i + 3]};
            }
            scalp->setCornerColors(name, std::move(colors));
        }
    }
    readRest(s, *scalp, what);
    return scalp;
}

// A graph's world directions (ParamSpec::worldDirection) turned a quarter about X as the points
// are (toY: Z-up to Y-up), a param a node leaves out by its default: a vec3 as a vector, an axis
// name ("x", "y", "z") to the axis it becomes. An axis that comes out pointing the other way
// (Y-up's z is Z-up's -y) puts the node's plane on the other side: its `offset` negated, its
// `side` swapped (Mirror). Compounds' own graphs too. An unknown node type is left as it is.
void turnGraphDirections(json& graph, bool toY, int depth = 0) {
    if (!graph.is_object() || depth > 16) return;
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return;
    for (json& node : *nodes) {
        if (!node.is_object()) continue;
        json spec;
        try {
            spec = json::parse(nodeSpecJson(node.dump()));
        } catch (const std::exception&) {
            continue;
        }
        auto valueOf = [&](const std::string& name) -> json {
            const auto params = node.find("params");
            if (params != node.end() && params->is_object() && params->contains(name)) return params->at(name);
            for (const json& p : spec.value("params", json::array())) {
                if (p.value("name", std::string()) == name) return p.value("default", json());
            }
            return json();
        };
        for (const json& p : spec.value("params", json::array())) {
            if (!p.value("worldDirection", false)) continue;
            const std::string name = p.value("name", std::string());
            const json value = valueOf(name);
            if (value.is_array() && value.size() == 3 && value[0].is_number() && value[1].is_number() && value[2].is_number()) {
                const double x = value[0].get<double>(), y = value[1].get<double>(), z = value[2].get<double>();
                node["params"][name] = toY ? json::array({x, z, -y}) : json::array({x, -z, y});
            } else if (value.is_string() && (value == "y" || value == "z")) {
                // Z-up to Y-up: y becomes -z, z becomes y; Y-up to Z-up: y becomes z, z becomes -y.
                const bool wasY = value == "y";
                node["params"][name] = wasY ? "z" : "y";
                if (wasY != toY) continue;  // the same way along the new axis
                const json offset = valueOf("offset");
                if (offset.is_number() && offset.get<double>() != 0.0) node["params"]["offset"] = -offset.get<double>();
                const json side = valueOf("side");
                if (side == "positive" || side == "negative") node["params"]["side"] = side == "positive" ? "negative" : "positive";
            }
        }
        if (node.value("type", std::string()) == "Compound") {
            const auto params = node.find("params");
            if (params == node.end() || !params->is_object() || !params->contains("definition")) continue;
            json& definition = params->at("definition");
            if (definition.is_string()) {
                json d = json::parse(definition.get<std::string>(), nullptr, false);
                if (d.is_object() && d.contains("graph")) {
                    turnGraphDirections(d["graph"], toY, depth + 1);
                    definition = d.dump();
                }
            } else if (definition.is_object() && definition.contains("graph")) {
                turnGraphDirections(definition["graph"], toY, depth + 1);
            }
        }
    }
}


std::string checkedUpAxis(const std::string& axis) {
    if (axis != "Y" && axis != "Z") throw GroomFileError("groom file: upAxis is \"Y\" or \"Z\", not \"" + axis + "\"");
    return axis;
}

json parseEmbedded(const std::string& text, const char* what) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return nullptr;
    try {
        return json::parse(text);
    } catch (const json::exception& e) {
        throw GroomFileError(std::string("groom file: invalid ") + what + " JSON: " + e.what());
    }
}

}  // namespace

std::string resolveGroomPath(const GroomFile& file, const std::string& stored, const std::string& directory) {
    if (stored.empty()) return {};
    std::vector<std::string> folders;
    const std::string& folder = directory.empty() ? file.directory : directory;
    if (!folder.empty()) folders.push_back(folder);
    folders.insert(folders.end(), file.searchPaths.begin(), file.searchPaths.end());
    return resolvePathIn(stored, std::move(folders));
}

namespace {

// One groom's object: everything a groom file holds for it but the format and version.
json groomObject(FloatArrays& arrays, const GroomFile& file) {
    json root;
    if (!file.name.empty()) root["name"] = file.name;
    root["framesPerSecond"] = file.framesPerSecond;
    root["units"] = file.units.empty() ? std::string("cm") : file.units;
    root["upAxis"] = checkedUpAxis(file.upAxis.empty() ? std::string("Y") : file.upAxis);
    root["source"] = file.source;

    json graph = parseEmbedded(file.data.graphJson, "graph");
    // Every param and node version written (graph_io.h): a later default never changes the file.
    if (graph.is_object() && graph.contains("nodes")) graph = json::parse(completeGraphJson(graph.dump()));
    root["graph"] = graph.is_null() ? json::object() : graph;
    root["hierarchy"] = parseEmbedded(file.data.hierarchyJson, "hierarchy");
    const json parts = parseEmbedded(file.data.partsJson, "parts");
    if (!parts.is_null()) root["parts"] = parts;
    const json correctives = parseEmbedded(file.data.correctivesJson, "correctives");
    if (!correctives.is_null()) root["correctives"] = correctives;
    const json flow = parseEmbedded(file.data.flowJson, "flow");
    if (!flow.is_null()) root["flow"] = flow;
    const json sculpts = parseEmbedded(file.data.sculptsJson, "sculpts");
    if (sculpts.is_object() && !sculpts.empty()) root["sculpts"] = sculpts;
    if (!file.data.controls.empty()) root["controls"] = file.data.controls;  // values when written
    if (!file.controlKeys.empty()) {
        json keys = json::object();
        for (const auto& [name, k] : file.controlKeys) {
            json list = json::array();
            for (const auto& [frame, value] : k) list.push_back({frame, value});
            if (!list.empty()) keys[name] = list;
        }
        if (!keys.empty()) root["controlKeys"] = keys;
    }
    if (!file.data.maps.empty()) {  // the maps painted on the scalp: what each Map node reads
        json maps = json::object();
        for (const auto& [name, info] : file.data.maps) {
            maps[name] = {{"kind", info.kind}, {"texture", info.texture}};
            if (info.channels == 4) maps[name]["channels"] = 4;  // a color map (r g b a); gray says nothing
        }
        root["maps"] = std::move(maps);
    }

    if (const ScalpMesh* scalp = file.data.scalp.get()) {
        json s = scalpJson(arrays, *scalp, "scalp");
        // What every reader checks the scalp it is given against (ScalpMesh::topologyFingerprint).
        s["topology"] = scalpFingerprintText(scalp->topologyFingerprint());
        root["scalp"] = std::move(s);
    } else {
        root["scalp"] = nullptr;
    }
    // The groom's other scalps, in order (several scalps on one groom: scalp_pieces.h).
    json scalps = json::array();
    for (const auto& [name, mesh] : file.data.scalps) {
        if (!mesh) continue;
        json s = scalpJson(arrays, *mesh, "scalps." + name);
        s["name"] = name;
        scalps.push_back(std::move(s));
    }
    if (!scalps.empty()) root["scalps"] = std::move(scalps);

    json guides = json::array();
    if (file.data.guides) {
        for (const Guide& g : file.data.guides->guides()) {
            guides.push_back({{"id", g.id},
                              {"root", {g.root.face, g.root.u, g.root.v}},
                              {"points", points(arrays, g.points, "guides.points")}});
        }
    }
    root["guides"] = std::move(guides);
    root["scalpCache"] = file.scalpCache.empty() ? json(nullptr) : cacheJson(file.scalpCache);
    root["guideCache"] = file.guideCache.empty() ? json(nullptr) : cacheJson(file.guideCache);
    if (!file.searchPaths.empty()) root["searchPaths"] = file.searchPaths;
    if (file.data.followScalp) root["followScalp"] = true;  // written only when on (the default is off)
    if (file.data.scaleSource != ScaleSource::Area) root["scaleSource"] = scaleSourceName(file.data.scaleSource);  // (default "area")
    if (file.data.scaleReference > 0.0) root["scaleReference"] = file.data.scaleReference;  // the area the groom is 1x at
    if (file.data.scaleTransformReference > 0.0) root["scaleTransformReference"] = file.data.scaleTransformReference;
    if (!file.cardBake.empty()) root["cardBake"] = {{"prefix", file.cardBake.prefix}, {"strands", file.cardBake.strands}};
    if (!file.simCaches.empty()) root["simCaches"] = simCachesJson(file.simCaches);
    if (!file.prototypes.empty()) {
        json protos = json::object();
        for (const auto& [name, mesh] : file.prototypes) {
            json m;
            m["points"] = points(arrays, mesh.points, "prototypes.points");
            m["triangles"] = mesh.triangles;
            if (mesh.normals.size() == mesh.points.size()) m["normals"] = points(arrays, mesh.normals, "prototypes.normals");
            if (mesh.uvs.size() == mesh.points.size() && !mesh.uvs.empty()) {
                m["uvs"] = arrays.add(mesh.uvs.front().data(), mesh.uvs.size() * 2, "prototypes.uvs");
            }
            protos[name] = std::move(m);
        }
        root["prototypes"] = std::move(protos);
    }
    // The collider meshes the ColliderMesh nodes read, as they were when written (world space, the
    // rest pose with them): every host evaluates Collide as the one that wrote the file did.
    json colliders = json::object();
    for (const auto& [name, mesh] : file.data.meshes) {
        if (mesh) colliders[name] = meshJson(arrays, *mesh, "colliders." + name);
    }
    if (!colliders.empty()) root["colliders"] = std::move(colliders);
    if (!file.transferMarkers.empty()) root["transferMarkers"] = json::parse(transferMarkersToJson(file.transferMarkers));
    if (!file.referencePhotos.empty()) {
        json photos = json::parse(file.referencePhotos, nullptr, false);
        if (photos.is_array() && !photos.empty()) root["referencePhotos"] = std::move(photos);
    }
    if (!file.guideGroupColors.empty()) root["guideGroupColors"] = file.guideGroupColors;
    if (!file.hiddenGuideGroups.empty()) root["hiddenGuideGroups"] = file.hiddenGuideGroups;
    return root;
}

}  // namespace

std::string groomFileToJson(const GroomFile& file) { return groomFilesToJson({file}); }

std::string groomFilesToJson(const std::vector<GroomFile>& grooms) {
    if (grooms.empty()) throw GroomFileError("groom file: no groom to write");
    FloatArrays arrays;
    // The first groom at the top, as a file of one groom is (a GroomLab before several grooms
    // reads it); the others in "grooms", each a groom's object of its own.
    json root = groomObject(arrays, grooms.front());
    root["format"] = kFormat;
    root["version"] = currentSchemaVersion(DocumentKind::GroomFile);
    if (grooms.size() > 1) {
        json others = json::array();
        for (std::size_t i = 1; i < grooms.size(); ++i) others.push_back(groomObject(arrays, grooms[i]));
        root["grooms"] = std::move(others);
    }
    return arrays.resolve(root.dump());
}

namespace {

json parsedGroomFile(const std::string& text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        throw GroomFileError(std::string("groom file: invalid JSON: ") + e.what());
    }
    if (!root.is_object() || root.value("format", std::string()) != kFormat) {
        throw GroomFileError("not a GroomLab groom file");
    }
    try {
        detail::checkVersion(DocumentKind::GroomFile, root);  // refuses groom files from a newer GroomLab
    } catch (const SchemaError& e) {
        throw GroomFileError(e.what());
    }
    return root;
}

// One groom's object (the file's top, or an entry of "grooms").
GroomFile groomFromObject(const json& root, const std::string& baseDir) {
    GroomFile file;
    try {
        file.name = root.value("name", std::string());
        file.framesPerSecond = root.value("framesPerSecond", 24.0);
        file.units = root.value("units", std::string("cm"));
        file.upAxis = checkedUpAxis(root.value("upAxis", std::string("Y")));
        file.source = root.value("source", std::string());
        file.data.graphJson = root.value("graph", json::object()).dump();
        if (root.contains("hierarchy") && !root.at("hierarchy").is_null()) {
            file.data.hierarchyJson = root.at("hierarchy").dump();
        }
        if (root.contains("parts") && !root.at("parts").is_null()) file.data.partsJson = root.at("parts").dump();
        if (root.contains("correctives") && !root.at("correctives").is_null()) {
            file.data.correctivesJson = root.at("correctives").dump();
        }
        if (root.contains("flow") && !root.at("flow").is_null()) file.data.flowJson = root.at("flow").dump();
        if (root.contains("sculpts") && root.at("sculpts").is_object()) file.data.sculptsJson = root.at("sculpts").dump();
        if (root.contains("controls") && root.at("controls").is_object()) {
            for (const auto& [name, value] : root.at("controls").items()) file.data.controls[name] = value.get<float>();
        }
        if (root.contains("controlKeys") && root.at("controlKeys").is_object()) {
            for (const auto& [name, list] : root.at("controlKeys").items()) {
                if (!list.is_array()) continue;
                ControlKeys& k = file.controlKeys[name];
                for (const json& key : list) {
                    if (key.is_array() && key.size() == 2 && key[0].is_number() && key[1].is_number()) {
                        setControlKey(k, key[0].get<double>(), key[1].get<double>());
                    }
                }
            }
        }
        if (root.contains("maps") && root.at("maps").is_object()) {
            for (const auto& [name, info] : root.at("maps").items()) {
                file.data.maps[name] = {info.value("kind", "vertices"), info.value("texture", ""), info.value("channels", 1) == 4 ? 4 : 1};
            }
        }

        if (root.contains("scalp") && !root.at("scalp").is_null()) {
            const json& s = root.at("scalp");
            const std::shared_ptr<ScalpMesh> scalp = readScalp(s, "scalp");
            // The topology the groom was made on: as stored, else the scalp's own.
            const std::uint64_t stored = parseScalpFingerprint(s.value("topology", std::string()));
            file.data.scalpFingerprint = stored ? stored : scalp->topologyFingerprint();
            file.data.scalp = scalp;
        }

        auto guides = std::make_shared<GuideSet>();
        for (const json& g : root.value("guides", json::array())) {
            Guide& guide = guides->add(g.at("id").get<std::uint64_t>(), readPoints(g.at("points"), "guides.points"));
            const json& r = g.at("root");
            guide.root = {r.at(0).get<std::int32_t>(), r.at(1).get<float>(), r.at(2).get<float>()};
        }
        if (!guides->empty()) file.data.guides = guides;

        file.scalpCache = readCache(root, "scalpCache", baseDir);
        file.guideCache = readCache(root, "guideCache", baseDir);
        file.searchPaths = root.value("searchPaths", std::vector<std::string>{});
        file.data.followScalp = root.value("followScalp", false);
        file.data.scaleSource = scaleSourceFromName(root.value("scaleSource", std::string("area")));
        file.data.scaleReference = std::max(0.0, root.value("scaleReference", 0.0));  // none: the first area measured
        file.data.scaleTransformReference = std::max(0.0, root.value("scaleTransformReference", 0.0));
        if (root.contains("simCaches") && root.at("simCaches").is_object()) file.simCaches = readSimCaches(root.at("simCaches"), baseDir);
        if (root.contains("cardBake") && root.at("cardBake").is_object()) {
            const json& bake = root.at("cardBake");
            file.cardBake.prefix = fromBaseDir(bake.value("prefix", std::string()), baseDir);
            file.cardBake.strands = bake.value("strands", std::string());
        }
        if (root.contains("prototypes") && root.at("prototypes").is_object()) {
            for (const auto& [name, m] : root.at("prototypes").items()) file.prototypes[name] = readPrototype(m);
        }
        if (root.contains("transferMarkers") && root.at("transferMarkers").is_array()) {
            file.transferMarkers = transferMarkersFromJson(root.at("transferMarkers").dump());
        }
        if (root.contains("referencePhotos") && root.at("referencePhotos").is_array()) {
            file.referencePhotos = root.at("referencePhotos").dump();
        }
        if (root.contains("guideGroupColors") && root.at("guideGroupColors").is_string()) {
            file.guideGroupColors = root.at("guideGroupColors").get<std::string>();
        }
        if (root.contains("hiddenGuideGroups") && root.at("hiddenGuideGroups").is_string()) {
            file.hiddenGuideGroups = root.at("hiddenGuideGroups").get<std::string>();
        }
        if (root.contains("scalps") && root.at("scalps").is_array()) {
            for (const json& s : root.at("scalps")) {
                const std::string name = s.value("name", std::string());
                if (!validScalpName(name)) throw GroomFileError("groom file: bad scalps name '" + name + "'");
                file.data.scalps.emplace_back(name, readScalp(s, "scalps." + name));
            }
        }
        if (root.contains("colliders") && root.at("colliders").is_object()) {
            for (const auto& [name, m] : root.at("colliders").items()) {
                const std::shared_ptr<ScalpMesh> mesh = readMesh(m, "colliders." + name);
                readRest(m, *mesh, "colliders." + name);
                file.data.meshes[name] = mesh;
            }
        }
        file.directory = baseDir;
        // Map names an older standalone app allowed (-, ., a leading digit) under the one rule
        // every host keeps (scalp_maps.h), with everything that reads them.
        sanitizeMapNames(file.data);
    } catch (const GroomFileError&) {
        throw;
    } catch (const std::exception& e) {
        throw GroomFileError(std::string("groom file: ") + e.what());
    }
    return file;
}

}  // namespace

GroomFile groomFileFromJson(const std::string& text, const std::string& baseDir) {
    return groomFromObject(parsedGroomFile(text), baseDir);
}

std::vector<GroomFile> groomFilesFromJson(const std::string& text, const std::string& baseDir) {
    const json root = parsedGroomFile(text);
    std::vector<GroomFile> out;
    out.push_back(groomFromObject(root, baseDir));
    if (root.contains("grooms") && root.at("grooms").is_array()) {
        for (const json& g : root.at("grooms")) {
            const std::string which = "groom " + std::to_string(out.size() + 1);
            if (!g.is_object()) throw GroomFileError("groom file: " + which + " is not a groom");
            try {
                out.push_back(groomFromObject(g, baseDir));
            } catch (const GroomFileError& e) {
                throw GroomFileError(which + ": " + e.what());
            }
        }
    }
    return out;
}

std::string simCachesToJson(const std::map<std::string, SimCacheRef>& caches) { return simCachesJson(caches).dump(); }

std::string scalpFingerprintText(std::uint64_t fingerprint) { return formatSimulationStamp(fingerprint); }  // 16 hex digits

std::uint64_t parseScalpFingerprint(const std::string& text) { return parseSimulationStamp(text); }

std::map<std::string, SimCacheRef> simCachesFromJson(const std::string& text, const std::string& baseDir) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return {};
    try {
        const json sims = json::parse(text);
        if (!sims.is_object()) throw GroomFileError("simCaches: not an object of Simulate nodes' bakes");
        return readSimCaches(sims, baseDir);
    } catch (const json::exception& e) {
        throw GroomFileError(std::string("simCaches: ") + e.what());
    }
}

void writeGroomFile(const std::string& path, const GroomFile& file) { writeGroomFiles(path, {file}); }

void writeGroomFiles(const std::string& path, const std::vector<GroomFile>& grooms) { writeGroomFileText(path, groomFilesToJson(grooms)); }

void writeGroomFileText(const std::string& path, const std::string& text) {
    // Written beside the target and moved over it, so a failure (or a reader
    // opening it meanwhile) never sees a half-written file.
    const fs::path target = fs::u8path(path);
    const fs::path tmp = tempPathFor(target);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw GroomFileError("cannot write '" + path + "'");
        out << text;
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw GroomFileError("cannot write '" + path + "'");
        }
    }
    try {
        replaceFile(tmp, target);
    } catch (const std::runtime_error& e) {
        throw GroomFileError(e.what());
    }
}

GroomFile readGroomFile(const std::string& path) { return readGroomFiles(path).front(); }

std::vector<GroomFile> readGroomFiles(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw GroomFileError("cannot read '" + path + "'");
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        return groomFilesFromJson(buffer.str(), fs::u8path(path).parent_path().u8string());
    } catch (const GroomFileError& e) {
        throw GroomFileError(path + ": " + e.what());
    }
}

void convertUpAxis(GroomFile& file, const std::string& upAxis) {
    const std::string from = checkedUpAxis(file.upAxis.empty() ? std::string("Y") : file.upAxis);
    const std::string to = checkedUpAxis(upAxis);
    if (from == to) return;
    // A quarter turn about X: Z-up to Y-up takes (x, y, z) to (x, z, -y), Y-up to Z-up back.
    const bool toY = to == "Y";
    auto turn = [toY](const Vec3f& p) { return toY ? Vec3f(p.x, p.z, -p.y) : Vec3f(p.x, -p.z, p.y); };
    auto turnAll = [&](std::vector<Vec3f> points) {
        for (Vec3f& p : points) p = turn(p);
        return points;
    };

    auto turnScalp = [&](const ScalpMesh& scalp) {
        auto turned = std::make_shared<ScalpMesh>(turnAll(scalp.points()), scalp.triangles());
        if (scalp.hasUVs()) turned->setCornerUVs(scalp.cornerUVs());
        for (const std::string& name : scalp.colorSetNames()) turned->setCornerColors(name, *scalp.cornerColors(name));
        if (scalp.hasRest()) turned->setRestPoints(turnAll(scalp.restPoints()));
        turned->setPolygons(scalp.polygons());
        turned->setRenderSubdivision(scalp.renderSubdivision(), scalp.renderSubdivisionFrom());
        return turned;
    };
    if (const ScalpMesh* scalp = file.data.scalp.get()) file.data.scalp = turnScalp(*scalp);
    for (auto& [name, mesh] : file.data.scalps) {
        if (mesh) mesh = turnScalp(*mesh);
    }
    for (TransferMarker& m : file.transferMarkers) {  // their roots turn with the meshes
        m.oldPoint = turn(m.oldPoint);
        m.newPoint = turn(m.newPoint);
    }
    for (auto& [name, mesh] : file.data.meshes) {  // the colliders: shapes alone
        if (!mesh) continue;
        auto turned = std::make_shared<ScalpMesh>(turnAll(mesh->points()), mesh->triangles());
        if (mesh->hasRest()) turned->setRestPoints(turnAll(mesh->restPoints()));
        turned->setPolygons(mesh->polygons());
        turned->setRenderSubdivision(mesh->renderSubdivision(), mesh->renderSubdivisionFrom());
        mesh = turned;
    }
    if (file.data.guides) {
        auto guides = std::make_shared<GuideSet>(*file.data.guides);
        for (Guide& g : guides->guides()) g.points = turnAll(std::move(g.points));
        file.data.guides = guides;
    }
    if (!file.data.hierarchyJson.empty()) {
        // The world shapes (points) and frame references (frameUp) of the guides that have them;
        // rooted shapes (rootLocal) and children's offsets are in the guides' own frames.
        json h;
        try {
            h = json::parse(file.data.hierarchyJson);
        } catch (const json::exception& e) {
            throw GroomFileError(std::string("groom file: invalid hierarchy JSON: ") + e.what());
        }
        json none = json::array();
        for (json& g : h.is_object() && h.contains("guides") && h.at("guides").is_array() ? h.at("guides") : none) {
            if (g.contains("points") && g.at("points").is_array()) {
                json& p = g.at("points");
                for (std::size_t i = 0; i + 2 < p.size(); i += 3) {
                    const Vec3f v = turn({p[i].get<float>(), p[i + 1].get<float>(), p[i + 2].get<float>()});
                    p[i] = v.x;
                    p[i + 1] = v.y;
                    p[i + 2] = v.z;
                }
            }
            if (g.contains("frameUp") && g.at("frameUp").is_array() && g.at("frameUp").size() == 3) {
                json& u = g.at("frameUp");
                const Vec3f v = turn({u[0].get<float>(), u[1].get<float>(), u[2].get<float>()});
                u = {v.x, v.y, v.z};
            }
        }
        file.data.hierarchyJson = h.dump();
    }
    // The params that are world directions (Gravity's, Wind's, Bend's, Mirror's axis...): the hair
    // falls, blows and mirrors the same way in the turned world.
    if (file.data.graphJson.find_first_not_of(" \t\r\n{}") != std::string::npos) {
        json graph = json::parse(file.data.graphJson, nullptr, false);
        if (graph.is_discarded()) throw GroomFileError("groom file: invalid graph JSON");
        turnGraphDirections(graph, toY);
        file.data.graphJson = graph.dump();
    }
    file.upAxis = to;
}

}  // namespace groomlab
