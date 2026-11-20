// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_maps.h"

#include <map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/groom.h"
#include "groomlab/texture.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"

namespace groomlab {

ScalpMaps ScalpMaps::fromJson(const std::string& text) {
    ScalpMaps maps;
    if (text.empty()) return maps;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("scalpMaps: bad JSON: ") + e.what());
    }
    if (doc.is_null()) return maps;
    if (!doc.is_object()) throw std::runtime_error("scalpMaps: expected an object of maps");
    for (const auto& [name, entry] : doc.items()) {
        if (!entry.is_object()) throw std::runtime_error("scalpMaps: map '" + name + "' is not an object");
        ScalpMapEntry m;
        m.kind = entry.value("kind", "vertices");
        if (m.kind != "vertices" && m.kind != "texture") {
            throw std::runtime_error("scalpMaps: map '" + name + "' has unknown kind '" + m.kind + "'");
        }
        m.channels = entry.value("channels", 1);
        if (m.channels != 1 && m.channels != 4) {
            throw std::runtime_error("scalpMaps: map '" + name + "' has " + std::to_string(m.channels) + " channels (1 or 4)");
        }
        if (m.kind == "vertices") {
            const auto values = entry.find("values");
            if (values != entry.end() && values->is_array()) {
                m.values.reserve(values->size());
                for (const auto& v : *values) m.values.push_back(v.is_number() ? v.get<float>() : 0.0f);
            }
        } else {
            m.texture = entry.value("texture", "");
        }
        maps.maps[name] = std::move(m);
    }
    return maps;
}

std::string ScalpMaps::toJson() const {
    if (maps.empty()) return "";
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [name, m] : maps) {
        nlohmann::json entry;
        entry["kind"] = m.kind;
        if (m.channels != 1) entry["channels"] = m.channels;  // absent: gray
        if (m.kind == "vertices") {
            nlohmann::json values = nlohmann::json::array();
            for (const float v : m.values) values.push_back(std::round(static_cast<double>(v) * 1000.0) / 1000.0);
            entry["values"] = std::move(values);
        } else {
            entry["texture"] = m.texture;
        }
        doc[name] = std::move(entry);
    }
    return doc.dump();
}

std::uint64_t ScalpMaps::hash() const {
    std::uint64_t h = hash64(0x5CA1BAD5);
    for (const auto& [name, m] : maps) {
        h = hashText(h, name);
        h = hashText(h, m.kind);
        h = hashText(h, m.texture);
        if (m.channels != 1) h = hashCombine(h, static_cast<std::uint64_t>(m.channels));
        for (const float v : m.values) h = hashFloat(h, v);
    }
    return h;
}

MapCatalog ScalpMaps::catalog() const {
    MapCatalog out;
    for (const auto& [name, m] : maps) out[name] = {m.kind, m.texture, m.channels == 4 ? 4 : 1};
    return out;
}

MeshPtr withVertexMaps(const MeshPtr& scalp, const ScalpMaps& maps, const std::string& previewName,
                       const std::vector<float>* previewValues, std::vector<std::string>* errors, int previewChannels) {
    if (!scalp) return scalp;
    bool any = previewValues != nullptr;
    for (const auto& [name, m] : maps.maps) any = any || m.kind == "vertices";
    if (!any) return scalp;
    auto mapped = std::make_shared<ScalpMesh>(*scalp);
    auto put = [&](const std::string& name, const std::vector<float>& given, int channels) {
        const std::size_t per = channels == 4 ? 4 : 1;
        // Painted before other scalps were joined after the groom's own (scalp_pieces.h): the
        // vertices of the scalps it was painted on, the rest 0 (alpha 1).
        std::vector<float> padded;
        const std::size_t total = scalp->pointCount() * per;
        if (given.size() < total && !scalp->pieces().empty()) {
            std::size_t covered = 0;
            for (const ScalpPiece& p : scalp->pieces()) {
                covered += p.pointCount * per;
                if (covered != given.size()) continue;
                padded = given;
                padded.resize(total, 0.0f);
                if (per == 4) {
                    for (std::size_t i = given.size() + 3; i < total; i += 4) padded[i] = 1.0f;
                }
                break;
            }
        }
        const std::vector<float>& values = padded.empty() ? given : padded;
        if (values.size() != total) {
            if (errors) {
                errors->push_back("map '" + name + "' was painted on a scalp with " + std::to_string(values.size() / per) +
                                  " vertices; this one has " + std::to_string(scalp->pointCount()) +
                                  " (convert it to a texture or guide layer on the old scalp, or fill it anew)");
            }
            return;
        }
        mapped->setCornerColors(name, cornerColorsFromVertexMap(values, *scalp, static_cast<int>(per)));
    };
    for (const auto& [name, m] : maps.maps) {
        if (m.kind != "vertices" || (previewValues && name == previewName)) continue;
        put(name, m.values, m.channels);
    }
    if (previewValues) put(previewName, *previewValues, previewChannels);
    return mapped;
}

std::vector<float> vertexMapFromColorSet(const ScalpMesh& scalp, const std::vector<ScalpMesh::Color>& corners, int* channels) {
    bool color = false;
    for (const ScalpMesh::Color& c : corners) {
        if (c[0] != c[1] || c[1] != c[2] || c[3] != 1.0f) {
            color = true;
            break;
        }
    }
    const std::size_t per = color ? 4 : 1;
    if (channels) *channels = static_cast<int>(per);
    std::vector<float> values(scalp.pointCount() * per, 0.0f);
    const auto& triangles = scalp.triangles();
    for (std::size_t t = 0; t < triangles.size() && t * 3 + 2 < corners.size(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            const auto v = static_cast<std::size_t>(triangles[t][k]);
            const ScalpMesh::Color& c = corners[t * 3 + k];
            if (per == 4) {
                for (std::size_t i = 0; i < 4; ++i) values[v * 4 + i] = c[i];
            } else {
                values[v] = c[0];
            }
        }
    }
    return values;
}

ScalpMaps scalpMapsFromGroomData(const GroomData& data) {
    ScalpMaps maps;
    const ScalpMesh* scalp = data.scalp.get();
    for (const auto& [name, info] : data.maps) {
        if (info.kind == "vertices") {
            const std::vector<ScalpMesh::Color>* corners = scalp ? scalp->cornerColors(name) : nullptr;
            if (!corners || corners->size() != scalp->triangleCount() * 3) continue;
            ScalpMapEntry entry;
            entry.kind = "vertices";
            entry.values = vertexMapFromColorSet(*scalp, *corners, &entry.channels);
            if (info.channels == kColorMap && entry.channels == kGrayMap) {  // a color map painted gray so far
                entry.values = convertMapChannels(entry.values, kGrayMap, kColorMap);
                entry.channels = kColorMap;
            }
            maps.maps[name] = std::move(entry);
        } else if (info.kind == "texture") {
            ScalpMapEntry entry;
            entry.kind = "texture";
            entry.texture = info.texture;
            entry.channels = info.channels == kColorMap ? kColorMap : kGrayMap;
            maps.maps[name] = std::move(entry);
        }
    }
    return maps;
}

// ---- Map names ---------------------------------------------------------------------------------

namespace {

bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Every reader of `from` in a graph's nodes (and its compounds' definitions) reads `to`.
bool retargetNodes(nlohmann::json& graph, const std::string& from, const std::string& to, int depth) {
    bool changed = false;
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return false;
    for (nlohmann::json& node : *nodes) {
        if (!node.is_object()) continue;
        if (!node.contains("params") || !node["params"].is_object()) {
            if (node.value("type", std::string()) != "Map" || from != "mask") continue;
            node["params"] = nlohmann::json::object();  // a Map node reading the default "mask"
        }
        nlohmann::json& params = node["params"];
        if (node.value("type", std::string()) == "Map" && params.value("name", std::string("mask")) == from) {
            params["name"] = to;
            changed = true;
        }
        // HairColor's map and Interpolate's regionMap name a map.
        for (const auto& [type, param] : {std::pair<const char*, const char*>{"HairColor", "map"}, {"Interpolate", "regionMap"}}) {
            if (node.value("type", std::string()) == type && params.contains(param) && params[param].is_string() &&
                params[param].get<std::string>() == from) {
                params[param] = to;
                changed = true;
            }
        }
        for (auto& [key, value] : params.items()) {
            if (!value.is_object()) continue;
            if (key == "definition" && node.value("type", std::string()) == "Compound" && depth < 16 && value.contains("graph") &&
                value["graph"].is_object()) {
                changed = retargetNodes(value["graph"], from, to, depth + 1) || changed;
                continue;
            }
            for (const char* source : {"guide", "colorSet"}) {
                if (value.contains(source) && value[source].is_string() && value[source].get<std::string>() == from) {
                    value[source] = to;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

}  // namespace

bool validMapName(const std::string& name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), nameChar);
}

std::string mapNameError(const std::string& name) {
    return validMapName(name) ? std::string() : std::string("map names are letters, digits and _ (not starting with a digit)");
}

std::string sanitizeMapName(const std::string& name) {
    std::string out;
    for (const char c : name) out += nameChar(c) ? c : '_';
    if (out.empty()) return "map";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

namespace {

void collectGraphMapNames(const nlohmann::json& graph, std::vector<std::pair<std::string, std::string>>& found) {
    auto note = [&](const std::string& name, const char* how) {
        if (name.empty()) return;
        for (const auto& [n, h] : found) {
            if (n == name) return;
        }
        found.emplace_back(name, how);
    };
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (const nlohmann::json& node : graph["nodes"]) {
        if (!node.is_object()) continue;
        const std::string type = node.value("type", std::string());
        const nlohmann::json params = node.contains("params") && node["params"].is_object() ? node["params"] : nlohmann::json::object();
        if (type == "Map") note(params.value("name", std::string("mask")), "map");
        for (const auto& [param, kind] : {std::pair<const char*, const char*>{"map", "HairColor"}, {"regionMap", "Interpolate"}}) {
            if (type != kind || !params.contains(param) || !params[param].is_string()) continue;
            const std::string value = params[param].get<std::string>();
            if (!value.empty() && !isImagePath(value)) note(value, "map");
        }
        for (const auto& [key, value] : params.items()) {
            if (!value.is_object()) continue;
            if (value.contains("guide") && value["guide"].is_string()) note(value["guide"].get<std::string>(), "guides");
            if (value.contains("colorSet") && value["colorSet"].is_string()) note(value["colorSet"].get<std::string>(), "colorSet");
        }
        if (type == "Compound" && params.contains("definition") && params["definition"].is_object() && params["definition"].contains("graph")) {
            collectGraphMapNames(params["definition"]["graph"], found);
        }
    }
}

}  // namespace

std::vector<std::pair<std::string, std::string>> graphMapNames(const std::string& graphJson) {
    std::vector<std::pair<std::string, std::string>> found;
    collectGraphMapNames(nlohmann::json::parse(graphJson, nullptr, false), found);
    return found;
}

std::string mapNameFromFile(const std::string& path, const std::function<bool(const std::string&)>& taken) {
    // The stem: after the last folder separator, before the last dot.
    const std::size_t slash = path.find_last_of("/\\");
    std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    std::string base;
    for (const char c : stem) base += nameChar(c) ? c : '_';
    const std::size_t first = base.find_first_not_of('_'), last = base.find_last_not_of('_');
    base = first == std::string::npos ? std::string("map") : base.substr(first, last - first + 1);
    if (std::isdigit(static_cast<unsigned char>(base[0]))) base = "map_" + base;
    std::string name = base;
    for (int n = 2; taken && taken(name); ++n) name = base + "_" + std::to_string(n);
    return name;
}

std::string retargetMapReaders(const std::string& graphJson, const std::string& from, const std::string& to, bool* changed) {
    if (changed) *changed = false;
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded() || !graph.is_object() || from == to) return graphJson;
    if (!retargetNodes(graph, from, to, 0)) return graphJson;
    if (changed) *changed = true;
    return graph.dump();
}

namespace {

// The params naming a map as text: HairColor's map and Interpolate's regionMap.
constexpr std::pair<const char*, const char*> kNamedMapParams[] = {{"HairColor", "map"}, {"Interpolate", "regionMap"}};

// The params of `node` reading map `name`: a guide or colorSet source naming it, then the text params.
std::vector<std::string> mapParamsOf(const nlohmann::json& node, const std::string& name) {
    std::vector<std::string> out;
    if (!node.is_object() || !node.contains("params") || !node["params"].is_object()) return out;
    const nlohmann::json& params = node["params"];
    for (const auto& [param, v] : params.items()) {
        if (v.is_object() && (v.value("guide", nlohmann::json()) == name || v.value("colorSet", nlohmann::json()) == name)) out.push_back(param);
    }
    for (const auto& [type, param] : kNamedMapParams) {
        if (node.value("type", std::string()) == type && params.contains(param) && params[param].is_string() &&
            params[param].get<std::string>() == name) {
            out.push_back(param);
        }
    }
    return out;
}

MapReaders readersIn(const nlohmann::json& graph, const std::string& name) {
    MapReaders out;
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return out;
    std::set<std::string> readers;
    for (const nlohmann::json& n : graph["nodes"]) {
        if (!n.is_object() || n.value("type", std::string()) != "Map") continue;
        const nlohmann::json params = n.contains("params") && n["params"].is_object() ? n["params"] : nlohmann::json::object();
        const nlohmann::json read = params.contains("name") ? params["name"] : nlohmann::json("mask");
        if (read == name && readers.insert(n.value("name", std::string())).second) out.nodes.push_back(n.value("name", std::string()));
    }
    if (graph.contains("connections") && graph["connections"].is_array()) {
        for (const nlohmann::json& c : graph["connections"]) {
            if (c.is_object() && readers.count(c.value("from", std::string()))) {
                out.users.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
            }
        }
    }
    for (const nlohmann::json& n : graph["nodes"]) {
        for (const std::string& param : mapParamsOf(n, name)) out.users.emplace_back(n.value("name", std::string()), param);
    }
    return out;
}

// The params of `n` naming map `name` (mapParamsOf) back to their spec's default (none: gone).
// Whether any did.
bool letGoOfMap(nlohmann::json& n, const std::string& name) {
    const std::vector<std::string> reset = mapParamsOf(n, name);
    if (reset.empty()) return false;
    nlohmann::json spec;
    try {
        spec = nlohmann::json::parse(nodeSpecJson(n.dump()));
    } catch (const std::exception&) {  // a node this build cannot read: the params just go
    }
    const nlohmann::json specs = spec.is_object() ? spec.value("params", nlohmann::json::array()) : nlohmann::json::array();
    for (const std::string& param : reset) {
        nlohmann::json value;
        for (const nlohmann::json& p : specs) {
            if (p.is_object() && p.value("name", std::string()) == param && p.contains("default")) value = p["default"];
        }
        if (value.is_null()) {
            n["params"].erase(param);
        } else {
            n["params"][param] = std::move(value);
        }
    }
    return true;
}

// Inside the compounds of `graph` (their definitions, and theirs): every param naming map `name`
// let go of (letGoOfMap), as retargetNodes follows a rename there. Whether any did.
bool letGoInCompounds(nlohmann::json& graph, const std::string& name, int depth) {
    bool changed = false;
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return false;
    for (nlohmann::json& node : *nodes) {
        if (!node.is_object() || node.value("type", std::string()) != "Compound" || depth >= 16) continue;
        if (!node.contains("params") || !node["params"].is_object() || !node["params"].contains("definition")) continue;
        nlohmann::json& definition = node["params"]["definition"];
        if (!definition.is_object() || !definition.contains("graph") || !definition["graph"].is_object()) continue;
        nlohmann::json& inner = definition["graph"];
        if (inner.contains("nodes") && inner["nodes"].is_array()) {
            for (nlohmann::json& n : inner["nodes"]) changed = (n.is_object() && letGoOfMap(n, name)) || changed;
        }
        changed = letGoInCompounds(inner, name, depth + 1) || changed;
    }
    return changed;
}

}  // namespace

MapReaders mapReaders(const std::string& graphJson, const std::string& name) {
    return readersIn(nlohmann::json::parse(graphJson, nullptr, false), name);
}

std::string unlinkMapReaders(const std::string& graphJson, const std::string& name, bool* changed,
                             std::vector<std::pair<std::string, std::string>>* unlinked) {
    if (changed) *changed = false;
    if (unlinked) unlinked->clear();
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    const MapReaders readers = readersIn(graph, name);
    const bool inCompounds = graph.is_object() && letGoInCompounds(graph, name, 0);
    if (readers.nodes.empty() && readers.users.empty()) {
        if (!inCompounds) return graphJson;
        if (changed) *changed = true;
        return graph.dump();
    }
    const std::set<std::string> gone(readers.nodes.begin(), readers.nodes.end());
    const nlohmann::json links = graph.contains("connections") && graph["connections"].is_array() ? graph["connections"] : nlohmann::json::array();
    for (nlohmann::json& n : graph["nodes"]) {
        if (!n.is_object() || gone.count(n.value("name", std::string()))) continue;
        const std::string node = n.value("name", std::string());
        // The ports of exposed params a Map node of it fed go (the param reads its own value again).
        if (n.contains("paramInputs") && n["paramInputs"].is_array()) {
            nlohmann::json ports = nlohmann::json::array();
            for (const nlohmann::json& port : n["paramInputs"]) {
                const bool fed = std::any_of(links.begin(), links.end(), [&](const nlohmann::json& c) {
                    return c.is_object() && gone.count(c.value("from", std::string())) && c.value("to", std::string()) == node &&
                           c.value("input", nlohmann::json()) == port;
                });
                if (!fed) ports.push_back(port);
            }
            if (ports.empty()) {
                n.erase("paramInputs");
            } else {
                n["paramInputs"] = std::move(ports);
            }
        }
        letGoOfMap(n, name);  // the params naming it: back to the spec's default
    }
    GraphEdit edit(graph.dump());
    for (const std::string& node : readers.nodes) edit.removeNode(node);  // with every link to or from it
    if (changed) *changed = true;
    if (unlinked) *unlinked = readers.users;
    return edit.toJson();
}

std::string renamedMapTexture(const std::string& stored, const std::string& from, const std::string& to) {
    if (stored.empty() || from.empty()) return {};
    std::string path = stored;
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::size_t slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.rfind('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : base.substr(dot);
    // <map>_2: a numbered name taken when <map> was.
    std::string numbered = stem;
    const std::size_t underscore = stem.rfind('_');
    if (underscore != std::string::npos && underscore + 1 < stem.size() &&
        std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(underscore) + 1, stem.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        numbered = stem.substr(0, underscore);
    }
    for (const std::string& root : {stem, numbered}) {
        const bool own = root == from;
        const bool prefixed = root.size() > from.size() + 1 && root.compare(root.size() - from.size(), from.size(), from) == 0 &&
                              root[root.size() - from.size() - 1] == '_';  // an older <groom>_<map>
        if (own || prefixed) return folder + root.substr(0, root.size() - from.size()) + to + ext;
    }
    return {};
}

std::map<std::string, std::string> sanitizeMapNames(GroomData& data) {
    // Every map name the groom has: the catalog's and the guide layers'.
    std::set<std::string> names;
    for (const auto& [name, info] : data.maps) names.insert(name);
    std::unique_ptr<GuideHierarchy> hierarchy;
    if (!data.hierarchyJson.empty()) {
        try {
            hierarchy = std::make_unique<GuideHierarchy>(GuideHierarchy::fromJson(data.hierarchyJson));
            for (const std::string& layer : hierarchy->attributeNames()) names.insert(layer);
        } catch (const std::exception&) {
            hierarchy.reset();  // the session reports a bad hierarchy; its layers are left alone
        }
    }
    std::map<std::string, std::string> renames;
    std::set<std::string> taken(names.begin(), names.end());
    for (const std::string& name : names) {
        if (validMapName(name)) continue;
        const std::string base = sanitizeMapName(name);
        std::string fresh = base;
        for (int k = 2; taken.count(fresh); ++k) fresh = base + "_" + std::to_string(k);
        taken.insert(fresh);
        renames[name] = fresh;
    }
    if (renames.empty()) return renames;
    std::shared_ptr<ScalpMesh> scalp;
    bool layers = false;
    for (const auto& [from, to] : renames) {
        const auto entry = data.maps.find(from);
        if (entry != data.maps.end()) {
            MapInfo info = entry->second;
            data.maps.erase(entry);
            data.maps[to] = info;
        }
        if (data.scalp && data.scalp->hasColorSet(from)) {  // a vertex map's color set
            if (!scalp) scalp = std::make_shared<ScalpMesh>(*data.scalp);
            scalp->setCornerColors(to, *scalp->cornerColors(from));
            scalp->removeColorSet(from);
        }
        if (hierarchy) {
            const std::vector<std::string> own = hierarchy->attributeNames();
            if (std::find(own.begin(), own.end(), from) != own.end()) {
                hierarchy->renameAttribute(from, to);
                layers = true;
            }
        }
        data.graphJson = retargetMapReaders(data.graphJson, from, to);
    }
    if (scalp) data.scalp = scalp;
    if (layers) data.hierarchyJson = hierarchy->toJson();
    return renames;
}

bool refitVertexMaps(ScalpMaps& maps, const ScalpCounts& before, const ScalpCounts& after) {
    if (before == after) return false;
    std::size_t total = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> was;  // name -> (first vertex, count)
    for (const auto& [name, count] : before) {
        was[name] = {total, count};
        total += count;
    }
    bool changed = false;
    for (auto& [name, entry] : maps.maps) {
        if (entry.kind != "vertices") continue;
        const std::size_t per = entry.channels == kColorMap ? 4 : 1;
        if (entry.values.size() != total * per) continue;  // fits no layout it had: left alone
        std::vector<float> values;
        for (const auto& [scalp, count] : after) {
            const auto it = was.find(scalp);
            if (it != was.end() && it->second.second == count) {
                const auto from = entry.values.begin() + static_cast<std::ptrdiff_t>(it->second.first * per);
                values.insert(values.end(), from, from + static_cast<std::ptrdiff_t>(count * per));
            } else {
                for (std::size_t v = 0; v < count; ++v) {  // a new scalp (or one remeshed): unpainted (a color: black, opaque)
                    for (std::size_t c = 0; c < per; ++c) values.push_back(per == 4 && c == 3 ? 1.0f : 0.0f);
                }
            }
        }
        if (values != entry.values) {
            entry.values = std::move(values);
            changed = true;
        }
    }
    return changed;
}

namespace {
bool hasGuideLayer(const GuideHierarchy* guides, const std::string& name) {
    if (!guides) return false;
    const std::vector<std::string> names = guides->attributeNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}
}  // namespace

std::string mapKindOf(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name) {
    const auto it = maps.maps.find(name);
    if (it != maps.maps.end()) return it->second.kind;
    return hasGuideLayer(guides, name) ? "guides" : "";
}

int mapChannelsOf(const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& name) {
    const auto it = maps.maps.find(name);
    if (it != maps.maps.end()) return it->second.channels == kColorMap ? kColorMap : kGrayMap;
    return hasGuideLayer(guides, name) ? kGrayMap : 0;
}

}  // namespace groomlab
