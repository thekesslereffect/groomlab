// SPDX-License-Identifier: Apache-2.0
#include "groomlab/card_presets.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "operators/internal.h"

namespace groomlab {

const std::vector<CardLodPreset>& cardLodPresets() {
    // lod, triangles, cardsPerClump, layers, flyawayCards, ruffle, segments, widthSegments,
    // curvature, variations, atlasSize, widthScale.
    static const std::vector<CardLodPreset> presets = {
        {"hero", 50000, 6, 2, 200, 0.3, 10, 3, 0.35, 32, 4096, 1.0},
        {"mid", 15000, 2, 1, 0, 0.3, 8, 2, 0.3, 16, 2048, 1.0},
        {"low", 4000, 1, 1, 0, 0.0, 5, 1, 0.0, 8, 1024, 1.0},
    };
    return presets;
}

const std::vector<FeatherCardLodPreset>& featherCardLodPresets() {
    // lod, triangles, featherSegments, featherSpine, featherMinLength, variations, atlasSize.
    static const std::vector<FeatherCardLodPreset> presets = {
        {"hero", 30000, 6, true, 0.0, 24, 4096},
        {"mid", 8000, 3, true, 0.15, 12, 2048},
        {"low", 2000, 1, true, 0.35, 6, 1024},
    };
    return presets;
}

const CardLodPreset* findCardLodPreset(const std::string& lod) {
    for (const CardLodPreset& p : cardLodPresets()) {
        if (p.lod == lod) return &p;
    }
    return nullptr;
}

const FeatherCardLodPreset* findFeatherCardLodPreset(const std::string& lod) {
    for (const FeatherCardLodPreset& p : featherCardLodPresets()) {
        if (p.lod == lod) return &p;
    }
    return nullptr;
}

int featherCardTriangles(int segments, bool spine) { return 2 * segments * (spine ? 2 : 1); }

namespace {

using json = nlohmann::json;

// Whether a compound's definition holds a Propagate node (in nested compounds too).
bool definitionGrowsFeathers(const json& definition, int depth) {
    if (!definition.is_object() || !definition.contains("graph") || !definition["graph"].is_object()) return false;
    for (const json& n : definition["graph"].value("nodes", json::array())) {
        const std::string type = n.value("type", std::string());
        if (type == "Propagate") return true;
        if (type == "Compound" && depth < 16 && n.contains("params") && n["params"].is_object() &&
            definitionGrowsFeathers(n["params"].value("definition", json()), depth + 1)) {
            return true;
        }
    }
    return false;
}

const json* findNode(const json& graph, const std::string& name) {
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return nullptr;
    for (const json& n : *nodes) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

// The type of a node's input port (its operator's, a compound's from its definition); empty
// when it has none of that name (a param's port is a Field or a Curve: never strands).
std::string inputType(const json& node, const std::string& input) {
    const std::string type = node.value("type", std::string());
    try {
        if (type == "Compound") {
            const json params = node.value("params", json::object());
            const json definition = params.value("definition", json::object());
            for (const PortSpec& p : compoundInputs(definition.is_string() ? definition.get<std::string>() : definition.dump())) {
                if (p.name == input) return portTypeName(p.type);
            }
            return {};
        }
        if (!OperatorRegistry::instance().contains(type)) return {};
        for (const PortSpec& p : OperatorRegistry::instance().create(type)->inputs()) {
            if (p.name == input) return portTypeName(p.type);
        }
    } catch (const std::exception&) {
    }
    return {};
}

json parseGraph(const std::string& graphJson) {
    json graph = json::parse(graphJson, nullptr, false);
    if (graph.is_discarded() || !graph.is_object()) throw std::invalid_argument("the graph is not a JSON object");
    return graph;
}

StrandsReaching strandsReaching(const json& graph, const std::string& node) {
    // Feathers when a node on the way grows them, hair when a strand source is reached without
    // passing one.
    StrandsReaching out;
    std::set<std::string> seen;
    std::vector<std::string> stack{node};
    const json connections = graph.value("connections", json::array());
    while (!stack.empty()) {
        const std::string name = stack.back();
        stack.pop_back();
        if (!seen.insert(name).second) continue;
        const json* n = findNode(graph, name);
        if (!n) continue;
        const std::string type = n->value("type", std::string());
        if (type == "Propagate" ||
            (type == "Compound" && definitionGrowsFeathers(n->value("params", json::object()).value("definition", json()), 0))) {
            out.feathers = true;
            continue;
        }
        std::vector<std::string> inputs;
        for (const json& c : connections) {
            if (c.value("to", std::string()) != name) continue;
            const std::string port = inputType(*n, c.value("input", std::string()));
            if (port == "Strands" || port == "Curves") inputs.push_back(c.value("from", std::string()));
        }
        if (inputs.empty() && name != node) out.hair = true;
        stack.insert(stack.end(), inputs.begin(), inputs.end());
    }
    return out;
}

}  // namespace

StrandsReaching strandsReaching(const std::string& graphJson, const std::string& node) {
    const json graph = parseGraph(graphJson);
    if (!findNode(graph, node)) throw std::invalid_argument("the graph has no node '" + node + "'");
    return strandsReaching(graph, node);
}

std::string cardNodeKind(const std::string& graphJson, const std::string& node) {
    const json graph = parseGraph(graphJson);
    const json* cards = findNode(graph, node);
    if (!cards) throw std::invalid_argument("the graph has no node '" + node + "'");
    std::string mode;
    const json params = cards->value("params", json::object());
    if (params.contains("featherMode") && params["featherMode"].is_string()) {
        mode = params["featherMode"].get<std::string>();
    } else if (OperatorRegistry::instance().contains(cards->value("type", std::string()))) {
        for (const ParamSpec& s : OperatorRegistry::instance().create(cards->value("type", std::string()))->paramSpecs()) {
            if (s.name == "featherMode" && std::holds_alternative<std::string>(s.defaultValue)) mode = std::get<std::string>(s.defaultValue);
        }
    }
    if (mode == "off" || mode == "perVane") return "hair";
    if (mode == "perFeather") return "feathers";
    const StrandsReaching reaching = strandsReaching(graph, node);
    return reaching.feathers && reaching.hair ? "both" : reaching.feathers ? "feathers" : "hair";
}

std::vector<std::pair<std::string, ParamValue>> cardPresetParams(const std::string& lod, const std::string& kind,
                                                                 std::optional<int> triangles) {
    const CardLodPreset* hair = findCardLodPreset(lod);
    const FeatherCardLodPreset* feathers = findFeatherCardLodPreset(lod);
    if (!hair || !feathers) throw std::invalid_argument("lod must be one of hero, low, mid, not '" + lod + "'");
    if (kind != "hair" && kind != "feathers" && kind != "both") {
        throw std::invalid_argument("a Cards node cards hair, feathers or both, not '" + kind + "'");
    }
    const int perHair = 2 * hair->segments * hair->widthSegments;
    const int perFeather = featherCardTriangles(feathers->featherSegments, feathers->featherSpine);
    int featherCards = std::max(1, feathers->triangles / perFeather);
    using P = std::pair<std::string, ParamValue>;
    if (kind == "feathers") {
        if (triangles) featherCards = std::max(1, *triangles / perFeather);
        return {P{"featherSegments", std::int64_t{feathers->featherSegments}}, P{"featherSpine", feathers->featherSpine},
                P{"featherMinLength", feathers->featherMinLength}, P{"variations", std::int64_t{feathers->variations}},
                P{"atlasSize", std::int64_t{feathers->atlasSize}}, P{"maxCards", std::int64_t{featherCards}}};
    }
    int hairCards = std::max(1, (triangles ? *triangles : hair->triangles) / perHair);
    std::vector<P> params = {P{"widthScale", hair->widthScale},
                             P{"cardsPerClump", std::int64_t{hair->cardsPerClump}},
                             P{"layers", std::int64_t{hair->layers}},
                             P{"flyawayCards", std::int64_t{hair->flyawayCards}},
                             P{"ruffle", hair->ruffle},
                             P{"segments", std::int64_t{hair->segments}},
                             P{"widthSegments", std::int64_t{hair->widthSegments}},
                             P{"curvature", hair->curvature},
                             P{"variations", std::int64_t{hair->variations}},
                             P{"atlasSize", std::int64_t{hair->atlasSize}}};
    if (kind == "both") {  // the feathers take their cards from the budget first
        params.push_back(P{"featherSegments", std::int64_t{feathers->featherSegments}});
        params.push_back(P{"featherSpine", feathers->featherSpine});
        params.push_back(P{"featherMinLength", feathers->featherMinLength});
        hairCards += featherCards;
    }
    params.push_back(P{"maxCards", std::int64_t{hairCards}});
    return params;
}

std::vector<std::string> cardsNodeNames(const std::string& graphJson) {
    std::vector<std::string> names;
    const nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return names;
    for (const nlohmann::json& n : graph["nodes"]) {
        if (n.is_object() && n.value("type", std::string()) == "Cards") names.push_back(n.value("name", std::string()));
    }
    return names;
}

std::string withCardPreset(const std::string& graphJson, const std::string& lod, std::vector<std::string> nodes,
                           std::optional<int> triangles, std::vector<std::pair<std::string, ParamValue>>* set) {
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (graph.is_discarded() || !graph.is_object()) throw std::invalid_argument("not a graph");
    const std::vector<std::string> cards = cardsNodeNames(graphJson);
    if (nodes.empty()) nodes = cards;
    if (nodes.empty()) throw std::invalid_argument("the graph has no Cards node: add one first");
    for (const std::string& node : nodes) {
        if (std::find(cards.begin(), cards.end(), node) == cards.end()) throw std::invalid_argument("no Cards node '" + node + "'");
        const std::vector<std::pair<std::string, ParamValue>> params = cardPresetParams(lod, cardNodeKind(graphJson, node), triangles);
        for (nlohmann::json& n : graph["nodes"]) {
            if (n.value("name", std::string()) != node) continue;
            if (!n.contains("params") || !n["params"].is_object()) n["params"] = nlohmann::json::object();
            for (const auto& [name, value] : params) {
                std::visit(
                    [&](const auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                                      std::is_same_v<T, std::string>) {
                            n["params"][name] = v;
                        }
                    },
                    value);
            }
        }
        if (set) *set = params;
    }
    return graph.dump();
}

}  // namespace groomlab
