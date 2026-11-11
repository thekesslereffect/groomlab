// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_inserts.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

json graphOf(const GraphEdit& edit) {
    json g = json::parse(edit.toJson());
    if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
    if (!g.contains("connections") || !g["connections"].is_array()) g["connections"] = json::array();
    return g;
}

const json* nodeOf(const json& g, const std::string& name) {
    for (const json& n : g["nodes"]) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

std::string typeOf(const json& g, const std::string& name) {
    const json* n = nodeOf(g, name);
    return n ? n->value("type", std::string()) : std::string();
}

json paramOf(const json& g, const std::string& name, const std::string& param) {
    const json* n = nodeOf(g, name);
    if (!n) return json();
    const json params = n->value("params", json::object());
    return params.is_object() && params.contains(param) ? params[param] : json();
}

std::vector<std::string> ofType(const json& g, const std::string& type) {
    std::vector<std::string> out;
    for (const json& n : g["nodes"]) {
        if (n.is_object() && n.value("type", std::string()) == type) out.push_back(n.value("name", std::string()));
    }
    return out;
}

// The links out of `src`, in the graph's order: (to, input, output).
struct Link {
    std::string to, input, output;
};
std::vector<Link> linksFrom(const json& g, const std::string& src) {
    std::vector<Link> out;
    for (const json& c : g["connections"]) {
        if (c.is_object() && c.value("from", std::string()) == src) {
            out.push_back({c.value("to", std::string()), c.value("input", std::string()), c.value("output", std::string())});
        }
    }
    return out;
}

std::string sourceOf(const json& g, const std::string& node, const std::string& input) {
    for (const json& c : g["connections"]) {
        if (c.is_object() && c.value("to", std::string()) == node && c.value("input", std::string()) == input) {
            return c.value("from", std::string());
        }
    }
    return {};
}

std::optional<std::pair<double, double>> placeBy(const GraphEdit& edit, const std::string& node, double dx, double dy) {
    const auto at = nodePosition(edit, node);
    if (!at) return std::nullopt;
    return std::make_pair(at->first + dx, at->second + dy);
}

std::string nodeBase(const std::string& name) {
    std::string out;
    for (const char c : name) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "collider" + out;
    return out;
}

}  // namespace

void setNodePosition(GraphEdit& edit, const std::string& node, double x, double y) {
    json g = graphOf(edit);
    for (json& n : g["nodes"]) {
        if (n.is_object() && n.value("name", std::string()) == node) {
            n["ui"] = {{"x", x}, {"y", y}};
            edit = GraphEdit(g.dump());
            return;
        }
    }
    throw GraphError("no node '" + node + "'");
}

std::optional<std::pair<double, double>> nodePosition(const GraphEdit& edit, const std::string& node) {
    const json g = graphOf(edit);
    const json* n = nodeOf(g, node);
    if (!n) throw GraphError("no node '" + node + "'");
    const json ui = n->value("ui", json());
    if (!ui.is_object() || !ui.contains("x") || !ui.contains("y") || !ui["x"].is_number() || !ui["y"].is_number()) return std::nullopt;
    return std::make_pair(ui["x"].get<double>(), ui["y"].get<double>());
}

void insertBetween(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& node,
                   const std::string& nodeInput) {
    const json g = graphOf(edit);
    std::vector<Link> links;
    for (const Link& l : linksFrom(g, src)) {
        if (l.to == dst) links.push_back(l);
    }
    if (links.empty()) throw GraphError(src + " does not feed " + dst);
    edit.connect(src, node, nodeInput, links.front().output);
    for (const Link& l : links) edit.connect(node, dst, l.input);
}

void insertBefore(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& input,
                  const std::string& nodeInput) {
    const std::string upstream = sourceOf(graphOf(edit), anchor, input);
    if (upstream.empty()) throw GraphError(anchor + " has no " + input + " input to insert before");
    insertBetween(edit, upstream, anchor, node, nodeInput);
    if (const auto at = nodePosition(edit, anchor)) setNodePosition(edit, node, at->first, at->second + kInsertRow);
}

std::string insertSimulate(GraphEdit& edit, const std::string& paramsJson, std::optional<double> groomSize) {
    const json g = graphOf(edit);
    auto interpolates = [&](const std::string& source) {
        std::vector<Link> out;
        for (const Link& l : linksFrom(g, source)) {
            if (typeOf(g, l.to) == "Interpolate") out.push_back(l);
        }
        return out;
    };
    std::string source;
    for (const std::string& n : ofType(g, "Guides")) {
        if (!interpolates(n).empty()) {
            source = n;
            break;
        }
    }
    if (source.empty()) throw GraphError("the groom's graph has no Guides node feeding an Interpolate");
    const std::vector<Link> consumers = interpolates(source);
    const auto at = placeBy(edit, source, kInsertColumn / 2.0, kInsertRow);
    const std::string node = edit.addNode("Simulate", {}, paramsJson, at ? std::optional<double>(at->first) : std::nullopt,
                                          at ? std::optional<double>(at->second) : std::nullopt, groomSize);
    for (const Link& l : consumers) edit.connect(node, l.to, l.input);
    edit.connect(source, node, "guides");
    return node;
}

std::string insertGuideCache(GraphEdit& edit, const std::string& path, const std::string& object, std::optional<double> groomSize) {
    const json g = graphOf(edit);
    const json params = {{"path", path}, {"object", object}};
    // A GuideCache on the Guides node already (or one fed by nothing): it reads the new file.
    for (const std::string& n : ofType(g, "GuideCache")) {
        const std::string source = sourceOf(g, n, "guides");
        if (source.empty() || typeOf(g, source) == "Guides") {
            json edited = graphOf(edit);
            for (json& m : edited["nodes"]) {
                if (m.is_object() && m.value("name", std::string()) == n) {
                    if (!m.contains("params") || !m["params"].is_object()) m["params"] = json::object();
                    m["params"]["path"] = path;
                    m["params"]["object"] = object;
                }
            }
            edit = GraphEdit(edited.dump());
            return n;
        }
    }
    std::string source;
    for (const std::string& n : ofType(g, "Guides")) {
        if (!linksFrom(g, n).empty()) {
            source = n;
            break;
        }
    }
    if (source.empty()) throw GraphError("the groom's graph has no Guides node feeding the groom");
    const std::vector<Link> consumers = linksFrom(g, source);
    const auto at = placeBy(edit, source, kInsertColumn / 2.0, kInsertRow);
    const std::string node = edit.addNode("GuideCache", {}, params.dump(), at ? std::optional<double>(at->first) : std::nullopt,
                                          at ? std::optional<double>(at->second) : std::nullopt, groomSize);
    for (const Link& l : consumers) edit.connect(node, l.to, l.input, l.output);
    edit.connect(source, node, "guides");
    return node;
}

std::string insertCollider(GraphEdit& edit, const std::string& meshName, bool wire, std::optional<double> offset,
                           std::optional<double> friction, std::optional<double> groomSize) {
    if (meshName.empty() || meshName == "scalp" || meshName.find(':') != std::string::npos) {
        throw GraphError("pick another collider name than '" + meshName + "'");
    }
    json g = graphOf(edit);
    std::string node;
    for (const std::string& n : ofType(g, "ColliderMesh")) {
        const json name = paramOf(g, n, "name");
        if (name.is_string() && name.get<std::string>() == meshName) node = n;
    }
    if (!node.empty() && !wire && !friction) return node;  // a ColliderMesh reads it already: the graph stays
    if (node.empty()) {
        node = edit.addNode("ColliderMesh", edit.uniqueName(nodeBase(meshName)), json{{"name", meshName}}.dump(), std::nullopt,
                            std::nullopt, groomSize);
    }
    if (friction) {
        json edited = graphOf(edit);
        for (json& m : edited["nodes"]) {
            if (m.is_object() && m.value("name", std::string()) == node) {
                if (!m.contains("params") || !m["params"].is_object()) m["params"] = json::object();
                m["params"]["friction"] = std::max(-1.0, std::min(1.0, *friction));
            }
        }
        edit = GraphEdit(edited.dump());
    }
    if (wire) {
        const std::string output = edit.outputNode();
        if (output.empty()) throw GraphError("the graph has no output to collide before");
        const json params = offset ? json{{"offset", *offset}} : json::object();
        const std::string collide = edit.addNode("Collide", edit.uniqueName("collide_" + nodeBase(meshName)), params.dump(),
                                                 std::nullopt, std::nullopt, groomSize);
        insertBefore(edit, output, collide);
        edit.connect(node, collide, "collider");
        g = graphOf(edit);
        if (sourceOf(g, collide, "scalp").empty()) {
            for (const std::string& s : ofType(g, "Scalp")) {
                const json input = paramOf(g, s, "input");
                if (input.is_null() || (input.is_string() && input.get<std::string>() == "scalp")) {
                    edit.connect(s, collide, "scalp");  // off the scalp too, as Collide is meant
                    break;
                }
            }
        }
        if (const auto at = nodePosition(edit, collide)) setNodePosition(edit, node, at->first - 220.0, at->second - 60.0);
    }
    return node;
}

}  // namespace groomlab
