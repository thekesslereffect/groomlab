// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_inserts.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/operators.h"
#include "groomlab/presets.h"

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

// A node's first input ("" when it has none).
std::string firstInput(const GraphEdit& edit, const std::string& node) {
    const json inputs = json::parse(edit.nodeSpecJson(node)).value("inputs", json::array());
    return inputs.empty() ? std::string() : inputs[0].value("name", std::string());
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

std::map<std::string, std::pair<double, double>> layoutPositions(const GraphEdit& edit, const GraphLayout& layout,
                                                                 const std::vector<std::string>* only) {
    const json g = graphOf(edit);
    std::vector<std::string> names;
    std::map<std::string, std::size_t> index;
    for (const json& n : g["nodes"]) {
        if (!n.is_object()) continue;
        const std::string name = n.value("name", std::string());
        if (index.emplace(name, names.size()).second) names.push_back(name);
    }
    // Each node's column: the longest path in from a source. A node on the trail (a cycle, in a
    // graph edited by hand) is not followed again.
    std::vector<std::vector<std::size_t>> feeds(names.size());
    for (const json& c : g["connections"]) {
        if (!c.is_object()) continue;
        const auto from = index.find(c.value("from", std::string())), to = index.find(c.value("to", std::string()));
        if (from != index.end() && to != index.end()) feeds[to->second].push_back(from->second);
    }
    std::vector<int> depth(names.size(), -1);
    std::vector<char> onTrail(names.size(), 0);
    std::function<int(std::size_t)> depthOf = [&](std::size_t i) {
        if (depth[i] >= 0) return depth[i];
        onTrail[i] = 1;
        int d = 0;
        for (const std::size_t s : feeds[i]) {
            if (!onTrail[s]) d = std::max(d, 1 + depthOf(s));
        }
        onTrail[i] = 0;
        return depth[i] = d;
    };
    const std::set<std::string> wanted = only ? std::set<std::string>(only->begin(), only->end()) : std::set<std::string>();
    auto placed = [&](std::size_t i) { return !only || wanted.count(names[i]) > 0; };
    const int perBlock = layout.columnsPerBlock > 0 ? layout.columnsPerBlock : 0;
    auto blockOf = [&](int d) { return perBlock ? d / perBlock : 0; };
    // Each block as tall as its tallest column.
    std::map<int, int> rows, blockRows;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!placed(i)) continue;
        const int d = depthOf(i);
        blockRows[blockOf(d)] = std::max(blockRows[blockOf(d)], ++rows[d]);
    }
    std::map<int, double> blockTop;
    double top = 0.0;
    for (const auto& [block, count] : blockRows) {
        blockTop[block] = top;
        top += count * layout.row + layout.blockGap;
    }
    rows.clear();
    std::map<std::string, std::pair<double, double>> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!placed(i)) continue;
        const int d = depthOf(i);
        const int row = rows[d]++;
        out[names[i]] = {(perBlock ? d % perBlock : d) * layout.column, blockTop[blockOf(d)] + row * layout.row};
    }
    return out;
}

// The Clumps just wired in (`nodes`) set to clump inside a Clump before them, and, one of them a
// Clump, the first Clump after them down the chain from `dst` (along each node's first input)
// inside theirs (GraphEdit::adoptUpstreamClump, issue #115).
static void adoptUpstreamClumps(GraphEdit& edit, const std::vector<std::string>& nodes, const std::string& dst) {
    bool clump = false;
    for (const std::string& n : nodes) {
        clump = clump || edit.nodeType(n) == op::kClump;
        edit.adoptUpstreamClump(n);
    }
    std::string at = clump ? dst : std::string();
    std::set<std::string> seen;
    while (!at.empty() && seen.insert(at).second) {
        if (edit.nodeType(at) == op::kClump) {
            edit.adoptUpstreamClump(at);
            return;
        }
        std::string next;
        for (const Link& l : linksFrom(graphOf(edit), at)) {
            if (l.input == firstInput(edit, l.to)) {
                next = l.to;
                break;
            }
        }
        at = next;
    }
}

void insertBetween(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& node,
                   const std::string& nodeInput) {
    const json g = graphOf(edit);
    std::vector<Link> links;
    for (const Link& l : linksFrom(g, src)) {
        if (l.to == dst) links.push_back(l);
    }
    if (links.empty()) throw GraphError(src + " does not feed " + dst);
    GraphEdit trial = edit;  // (the graph as it was when a link does not fit)
    trial.connect(src, node, nodeInput.empty() ? firstInput(trial, node) : nodeInput, links.front().output);
    for (const Link& l : links) trial.connect(node, dst, l.input);
    adoptUpstreamClumps(trial, {node}, dst);
    edit = std::move(trial);
}

void insertBefore(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& input,
                  const std::string& nodeInput) {
    const std::string upstream = sourceOf(graphOf(edit), anchor, input);
    if (upstream.empty()) throw GraphError(anchor + " has no " + input + " input to insert before");
    insertBetween(edit, upstream, anchor, node, nodeInput);
    if (const auto at = nodePosition(edit, anchor)) setNodePosition(edit, node, at->first, at->second + kInsertRow);
}

void insertAfter(GraphEdit& edit, const std::string& anchor, const std::string& node, const std::string& nodeInput) {
    const std::vector<Link> consumers = linksFrom(graphOf(edit), anchor);
    if (!consumers.empty()) {
        insertBetween(edit, anchor, consumers.front().to, node, nodeInput);
    } else {
        edit.connect(anchor, node, nodeInput.empty() ? firstInput(edit, node) : nodeInput);
        adoptUpstreamClumps(edit, {node}, {});
    }
    if (const auto at = placeBy(edit, anchor, kInsertColumn / 2.0, kInsertRow)) setNodePosition(edit, node, at->first, at->second);
}

void insertOnLink(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& input, const std::string& node,
                  const std::string& output) {
    for (const std::string& name : {src, dst, node}) {
        if (!edit.hasNode(name)) throw GraphError("no node '" + name + "'");
    }
    if (node == src || node == dst) throw GraphError(node + " is an end of that link");
    // The link as it is recorded: its output only when it is not src's first.
    const json outputs = json::parse(edit.nodeSpecJson(src)).value("outputs", json::array());
    const std::string first = outputs.empty() ? std::string() : outputs[0].value("name", std::string());
    auto named = [&](const std::string& o) { return o.empty() ? first : o; };
    bool linked = false;
    for (const Link& l : linksFrom(graphOf(edit), src)) {
        linked = linked || (l.to == dst && l.input == input && named(l.output) == named(output));
    }
    if (!linked) throw GraphError(src + (output.empty() ? "" : "." + output) + " is not connected to " + dst + "." + input);
    std::string why;
    for (const json& p : json::parse(edit.nodeSpecJson(node)).value("inputs", json::array())) {
        GraphEdit trial = edit;
        try {
            trial.connect(src, node, p.value("name", std::string()), output);
            trial.connect(node, dst, input);
        } catch (const GraphError& e) {
            if (why.empty()) why = e.what();
            continue;
        }
        adoptUpstreamClumps(trial, {node}, dst);
        edit = std::move(trial);
        return;
    }
    throw GraphError(node + " has no input that takes " + src + "'s " + edit.outputType(src, output) + " and feeds " + dst + "." + input +
                     (why.empty() ? "" : " (" + why + ")"));
}

std::string insertPart(GraphEdit& edit, const std::string& paramsJson, const std::string& after, std::optional<double> groomSize,
                       bool* made) {
    if (made) *made = false;
    const json g = graphOf(edit);
    const std::vector<std::string> parts = ofType(g, "Part");
    if (!parts.empty()) return parts.front();
    std::string anchor = !after.empty() && edit.hasNode(after) ? after : std::string();
    for (const char* kind : {"Interpolate", "Flow", "Bend", "Scatter"}) {
        if (!anchor.empty()) break;
        const std::vector<std::string> found = ofType(g, kind);
        if (!found.empty()) anchor = found.front();
    }
    if (anchor.empty()) throw GraphError("the graph has no strands to part (no Scatter)");
    const std::string node = edit.addNode("Part", {}, paramsJson, std::nullopt, std::nullopt, groomSize);
    insertAfter(edit, anchor, node);
    if (made) *made = true;
    return node;
}

std::string insertFlow(GraphEdit& edit, const std::string& paramsJson, const std::string& after, std::optional<double> groomSize,
                       bool* made) {
    if (made) *made = false;
    const json g = graphOf(edit);
    const std::vector<std::string> flows = ofType(g, "Flow");
    if (!flows.empty()) return flows.front();
    std::string anchor = !after.empty() && edit.hasNode(after) ? after : std::string();
    for (const char* kind : {"Interpolate", "Scatter"}) {
        if (!anchor.empty()) break;
        const std::vector<std::string> found = ofType(g, kind);
        if (!found.empty()) anchor = found.front();
    }
    if (anchor.empty()) throw GraphError("the graph has no strands to lay along the flow (no Scatter)");
    const std::string node = edit.addNode("Flow", {}, paramsJson, std::nullopt, std::nullopt, groomSize);
    insertAfter(edit, anchor, node);
    if (made) *made = true;
    return node;
}

std::string motionAnchor(const GraphEdit& edit) {
    std::string anchor = edit.outputNode();
    if (anchor.empty()) return anchor;
    const json g = graphOf(edit);
    std::set<std::string> seen;
    while (seen.insert(anchor).second) {
        const std::string from = sourceOf(g, anchor, "strands");
        const std::string type = from.empty() ? std::string() : typeOf(g, from);
        if (type != "Collide" && type != "Cards") break;
        anchor = from;
    }
    return anchor;
}

std::string insertMotion(GraphEdit& edit, const std::string& type, const std::string& paramsJson, std::optional<double> groomSize) {
    const std::string anchor = motionAnchor(edit);
    if (anchor.empty()) throw GraphError("the graph has no output");
    const std::string node = edit.addNode(type, {}, paramsJson, std::nullopt, std::nullopt, groomSize);
    insertBefore(edit, anchor, node);
    return node;
}

std::string insertCards(GraphEdit& edit, const std::string& paramsJson, std::optional<double> groomSize, bool* made) {
    if (made) *made = false;
    const json g = graphOf(edit);
    const std::vector<std::string> cards = ofType(g, "Cards");
    if (!cards.empty()) return cards.front();
    const std::string output = edit.outputNode();
    if (output.empty()) throw GraphError("the graph has no output");
    const std::string node = edit.addNode("Cards", edit.uniqueName("cards"), paramsJson, std::nullopt, std::nullopt, groomSize);
    insertBefore(edit, output, node);
    if (made) *made = true;
    return node;
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
    // The Guides node, or the DriveGuides or GuideCache nodes after it (posing the guides): the last
    // of them, feeding an Interpolate, so the simulation plays on top of the posed guides.
    std::string source;
    for (const std::string& n : ofType(g, "Guides")) {
        std::string at = n;
        for (int steps = 0; interpolates(at).empty() && steps < 8; ++steps) {
            const std::vector<Link> next = linksFrom(g, at);
            if (next.size() != 1 || (typeOf(g, next[0].to) != "DriveGuides" && typeOf(g, next[0].to) != "GuideCache")) break;
            at = next[0].to;
        }
        if (!interpolates(at).empty()) {
            source = at;
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

std::string insertDriveGuides(GraphEdit& edit, std::optional<double> groomSize) {
    const json g = graphOf(edit);
    const std::vector<std::string> existing = ofType(g, "DriveGuides");
    if (!existing.empty()) return existing.front();
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
    const std::string node = edit.addNode("DriveGuides", {}, "{}", at ? std::optional<double>(at->first) : std::nullopt,
                                          at ? std::optional<double>(at->second) : std::nullopt, groomSize);
    for (const Link& l : consumers) edit.connect(node, l.to, l.input, l.output);
    edit.connect(source, node, "strands");
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


namespace {

// The words of a list param ("a b", "a,b").
std::vector<std::string> wordsOf(const json& value) {
    std::vector<std::string> out;
    if (!value.is_string()) return out;
    std::string word;
    for (const char c : value.get<std::string>() + " ") {
        if (c == ' ' || c == ',') {
            if (!word.empty()) out.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    return out;
}

// The first Guides node reading guide group `group` ("" none).
std::string guidesOfGroup(const json& g, const std::string& group) {
    for (const std::string& n : ofType(g, "Guides")) {
        const json own = paramOf(g, n, "group");
        if (own.is_string() && own.get<std::string>() == group) return n;
    }
    return {};
}

std::string addNodeAt(GraphEdit& edit, const std::string& type, const std::string& base, const json& params,
                      std::optional<double> groomSize, const std::optional<std::pair<double, double>>& at = std::nullopt) {
    return edit.addNode(type, edit.uniqueName(base), params.is_null() ? std::string() : params.dump(),
                        at ? std::optional<double>(at->first) : std::nullopt, at ? std::optional<double>(at->second) : std::nullopt,
                        groomSize);
}

// The params of the Propagate nodes inside a Compound's params (and its compounds'), to edit in place.
void propagatesIn(json& params, std::vector<json*>* out) {
    if (!params.is_object() || !params.contains("definition") || !params["definition"].is_object()) return;
    json& definition = params["definition"];
    if (!definition.contains("graph") || !definition["graph"].is_object()) return;
    json& graph = definition["graph"];
    if (!graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (json& node : graph["nodes"]) {
        if (!node.is_object()) continue;
        const std::string type = node.value("type", std::string());
        if (type != "Propagate" && type != "Compound") continue;
        if (!node.contains("params") || !node["params"].is_object()) node["params"] = json::object();
        if (type == "Propagate") out->push_back(&node["params"]);
        else propagatesIn(node["params"], out);
    }
}

// Every Propagate's params in a node: its own (a Propagate), or those inside it (a Compound).
std::vector<json> propagatesOf(const json& node) {
    json params = node.value("params", json::object());
    if (!params.is_object()) params = json::object();
    const std::string type = node.value("type", std::string());
    if (type == "Propagate") return {params};
    std::vector<json> out;
    if (type == "Compound") {
        std::vector<json*> found;
        propagatesIn(params, &found);
        for (const json* p : found) out.push_back(*p);
    }
    return out;
}

std::map<std::string, int> featherGroupsOf(const json& g) {
    std::map<std::string, int> groups;
    for (const json& n : g["nodes"]) {
        if (!n.is_object()) continue;
        for (const json& p : propagatesOf(n)) {
            const int id = p.contains("groupId") && p["groupId"].is_number() ? p["groupId"].get<int>() : 0;
            if (id <= 0) continue;
            const std::string name = p.contains("groupName") && p["groupName"].is_string() && !p["groupName"].get<std::string>().empty()
                                         ? p["groupName"].get<std::string>()
                                         : "group" + std::to_string(id);
            groups.emplace(name, id);  // the first one named so
        }
    }
    return groups;
}

}  // namespace

ScalpNodes insertScalp(GraphEdit& edit, const std::string& meshName, std::optional<double> groomSize) {
    if (meshName.empty() || meshName == "scalp") throw GraphError("pick another scalp name than '" + meshName + "'");
    const json g = graphOf(edit);
    auto reads = [&](const std::string& scalp, const std::string& name) {
        const json input = paramOf(g, scalp, "input");
        return input.is_string() ? input.get<std::string>() == name : name == "scalp";  // (unset: the groom's own)
    };
    auto firstFed = [&](const std::string& source, const std::string& type) {
        for (const Link& l : linksFrom(g, source)) {
            if (typeOf(g, l.to) == type) return l.to;
        }
        return std::string();
    };
    std::string ownScalp;
    for (const std::string& n : ofType(g, "Scalp")) {
        if (reads(n, meshName)) return {n, {}, {}, {}};  // a Scalp node reads it already: the graph stays
        if (ownScalp.empty() && reads(n, "scalp")) ownScalp = n;
    }
    const std::string ownScatter = ownScalp.empty() ? std::string() : firstFed(ownScalp, "Scatter");
    const std::string ownInterp = ownScatter.empty() ? std::string() : firstFed(ownScatter, "Interpolate");
    auto paramsOf = [&](const std::string& node) {
        const json* n = nodeOf(g, node);
        const json params = n ? n->value("params", json::object()) : json::object();
        return params.is_object() ? params : json::object();
    };
    std::string base;
    for (const char c : meshName) base += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    GraphEdit made = edit;  // (the graph as it was when a link does not fit)
    ScalpNodes nodes;
    const json subdivision = ownScalp.empty() ? json() : paramOf(g, ownScalp, "subdivision");
    nodes.scalp = addNodeAt(made, "Scalp", "scalp_" + base,
                            {{"input", meshName}, {"subdivision", subdivision.is_string() ? subdivision : json("matchRender")}}, groomSize);
    json scatterParams = ownScatter.empty() ? json::object() : paramsOf(ownScatter);
    scatterParams.erase("seed");  // a seed of its own (addNode picks one)
    nodes.scatter = addNodeAt(made, "Scatter", "scatter_" + base, scatterParams, groomSize);
    made.connect(nodes.scalp, nodes.scatter, "scalp");
    std::string last = nodes.scatter;
    if (!ownInterp.empty()) {
        nodes.interpolate = addNodeAt(made, "Interpolate", "interpolate_" + base, paramsOf(ownInterp), groomSize);
        made.connect(nodes.scatter, nodes.interpolate, "strands");
        // The guides the hair reads (its Interpolate's): never the first Guides node there is (a braid's).
        for (const json& c : g["connections"]) {
            if (c.is_object() && c.value("to", std::string()) == ownInterp && c.value("input", std::string()) == "guides") {
                made.connect(c.value("from", std::string()), nodes.interpolate, "guides", c.value("output", std::string()));
                break;
            }
        }
        made.connect(nodes.scalp, nodes.interpolate, "scalp");
        last = nodes.interpolate;
    }
    nodes.merge = addNodeAt(made, "Merge", "merge_" + base, json(), groomSize);
    const std::string anchor = ownInterp.empty() ? ownScatter : ownInterp;
    const std::string output = made.outputNode();
    if (!anchor.empty() && !linksFrom(g, anchor).empty()) {
        insertAfter(made, anchor, nodes.merge, "a");  // anchor -> merge.a -> what anchor fed
    } else if (!output.empty() && !sourceOf(g, output, "strands").empty()) {
        insertBefore(made, output, nodes.merge, "strands", "a");
    }
    made.connect(last, nodes.merge, "b");
    if (const auto at = nodePosition(made, nodes.merge)) {
        int column = 0;
        for (const std::string& n : {nodes.scalp, nodes.scatter, nodes.interpolate}) {
            if (!n.empty()) setNodePosition(made, n, at->first - 660.0 + 220.0 * column, at->second + 160.0);
            ++column;
        }
    }
    edit = std::move(made);
    return nodes;
}

bool removeColliderNodes(GraphEdit& edit, const std::string& meshName) {
    const json g = graphOf(edit);
    std::vector<std::string> readers;
    for (const std::string& n : ofType(g, "ColliderMesh")) {
        const json name = paramOf(g, n, "name");
        if (name.is_string() && name.get<std::string>() == meshName) readers.push_back(n);
    }
    for (const std::string& reader : readers) {
        for (const Link& l : linksFrom(graphOf(edit), reader)) {
            if (!edit.hasNode(l.to)) continue;
            if (edit.nodeType(l.to) == "Collide") {
                edit.removeNodeKeepingFlow(l.to);  // the Collide made for it: strands pass through
            } else {
                edit.disconnect(l.to, l.input);  // another reader of the mesh: it reads nothing now
            }
        }
        edit.removeNode(reader);
    }
    return !readers.empty();
}

bool removeScalpNodes(GraphEdit& edit, const std::string& meshName) {
    if (meshName.empty() || meshName == "scalp") return false;
    const json g = graphOf(edit);
    std::set<std::string> doomed;
    for (const std::string& n : ofType(g, "Scalp")) {
        const json input = paramOf(g, n, "input");
        if (input.is_string() && input.get<std::string>() == meshName) doomed.insert(n);
    }
    if (doomed.empty()) return false;
    // What they alone feed: every input of a node from doomed nodes (Guides nodes aside), one at least.
    const std::set<std::string> guides = [&] {
        const std::vector<std::string> v = ofType(g, "Guides");
        return std::set<std::string>(v.begin(), v.end());
    }();
    for (bool grew = true; grew;) {
        grew = false;
        for (const json& n : g["nodes"]) {
            const std::string name = n.value("name", std::string()), type = n.value("type", std::string());
            if (doomed.count(name) || type == "Merge" || type == "Output" || type == "Guides") continue;
            bool any = false, all = true, fed = false;
            for (const json& c : g["connections"]) {
                if (c.value("to", std::string()) != name) continue;
                fed = true;
                const std::string from = c.value("from", std::string());
                if (doomed.count(from)) any = true;
                else if (!guides.count(from)) all = false;
            }
            if (fed && any && all) {
                doomed.insert(name);
                grew = true;
            }
        }
    }
    // The Merges a doomed node fed by `b`: passed through when left with `a` alone.
    std::set<std::string> merges;
    for (const std::string& n : doomed) {
        for (const Link& l : linksFrom(g, n)) {
            if (!doomed.count(l.to) && typeOf(g, l.to) == "Merge" && l.input == "b") merges.insert(l.to);
        }
    }
    for (const std::string& n : doomed) edit.removeNode(n);
    for (const std::string& merge : merges) {
        if (!edit.hasNode(merge) || !sourceOf(graphOf(edit), merge, "b").empty()) continue;
        edit.removeNodeKeepingFlow(merge);
    }
    return true;
}

bool excludeFromHairGuides(GraphEdit& edit, const std::string& group) {
    if (group.empty()) return false;
    json g = graphOf(edit);
    bool changed = false;
    for (json& n : g["nodes"]) {
        if (!n.is_object() || n.value("type", std::string()) != "Guides") continue;
        if (!n.contains("params") || !n["params"].is_object()) n["params"] = json::object();
        json& params = n["params"];
        if (!wordsOf(params.value("group", json())).empty()) continue;  // a group's own Guides node
        std::vector<std::string> excluded = wordsOf(params.value("exclude", json()));
        if (std::find(excluded.begin(), excluded.end(), group) != excluded.end()) continue;
        excluded.push_back(group);
        std::string joined;
        for (const std::string& w : excluded) joined += (joined.empty() ? "" : " ") + w;
        params["exclude"] = joined;
        changed = true;
    }
    if (changed) edit = GraphEdit(g.dump());
    return changed;
}

namespace {

const char* const kGroupParams[] = {"group", "exclude"};

// The Guides nodes of `graph` (one level) naming group `from` in their group or exclude lists named
// `to` instead, the compounds' definitions too.
bool renameGroupIn(json& graph, const std::string& from, const std::string& to, int depth) {
    bool changed = false;
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return false;
    for (json& n : graph["nodes"]) {
        if (!n.is_object() || !n.contains("params") || !n["params"].is_object()) continue;
        json& params = n["params"];
        const std::string type = n.value("type", std::string());
        if (type == "Compound" && depth < 16 && params.contains("definition") && params["definition"].is_object() &&
            params["definition"].contains("graph")) {
            changed = renameGroupIn(params["definition"]["graph"], from, to, depth + 1) || changed;
            continue;
        }
        if (type != "Guides") continue;
        for (const char* param : kGroupParams) {
            std::vector<std::string> words = wordsOf(params.value(param, json()));
            if (std::find(words.begin(), words.end(), from) == words.end()) continue;
            std::string joined;
            for (const std::string& w : words) joined += (joined.empty() ? "" : " ") + (w == from ? to : w);
            params[param] = joined;
            changed = true;
        }
    }
    return changed;
}

}  // namespace

std::string withImportedCurves(const std::string& graphJson, const std::string& path, const std::string& object, int samples) {
    if (samples < 0 || samples > 1024) throw std::invalid_argument("samples is 0 (as read) to 1024");
    json g = json::parse(graphJson);
    json nodes = json::array(), drawn;
    bool hasScalp = false;
    for (const json& n : g.value("nodes", json::array())) {
        const std::string name = n.value("name", std::string());
        if (name == "guides") {
            if (n.contains("ui")) drawn = n["ui"];  // where the Guides node was drawn
            continue;
        }
        hasScalp = hasScalp || name == "scalp";
        nodes.push_back(n);
    }
    json params = {{"path", path}, {"samples", samples}};
    if (!object.empty()) params["object"] = object;
    json made = {{"name", "guides"}, {"type", "ImportCurves"}, {"params", params}};
    if (!drawn.is_null()) made["ui"] = drawn;
    nodes.push_back(made);
    g["nodes"] = nodes;
    json connections = json::array();
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("to", std::string()) != "guides") connections.push_back(c);
    }
    if (hasScalp) connections.push_back({{"from", "scalp"}, {"to", "guides"}, {"input", "scalp"}});
    g["connections"] = connections;
    return g.dump();
}

std::vector<std::pair<std::string, std::string>> groupReaders(const std::string& graphJson, const std::string& group) {
    std::vector<std::pair<std::string, std::string>> out;
    const json g = json::parse(graphJson, nullptr, false);
    if (group.empty() || !g.is_object() || !g.contains("nodes") || !g["nodes"].is_array()) return out;
    for (const json& n : g["nodes"]) {
        if (!n.is_object() || n.value("type", std::string()) != "Guides" || !n.contains("params") || !n["params"].is_object()) continue;
        for (const char* param : kGroupParams) {
            const std::vector<std::string> words = wordsOf(n["params"].value(param, json()));
            if (std::find(words.begin(), words.end(), group) != words.end()) out.emplace_back(n.value("name", std::string()), param);
        }
    }
    return out;
}

std::string renameGroupReaders(const std::string& graphJson, const std::string& from, const std::string& to, bool* changed) {
    if (changed) *changed = false;
    json g = json::parse(graphJson, nullptr, false);
    if (from.empty() || to.empty() || from == to || g.is_discarded() || !renameGroupIn(g, from, to, 0)) return graphJson;
    if (changed) *changed = true;
    return g.dump();
}

std::string insertBraid(GraphEdit& edit, const std::string& group, const std::string& paramsJson, const std::string& after,
                        std::optional<double> groomSize) {
    if (group.empty()) throw GraphError("a braid's paths need a guide group");
    json params = paramsJson.empty() ? json::object() : json::parse(paramsJson, nullptr, false);
    if (!params.is_object()) throw GraphError("a braid's params are a JSON object");
    // The styles the Braid has (its spec's choices): every host offers the same.
    const json spec = json::parse(nodeSpecJson(R"({"name": "braid", "type": "Braid"})"));
    std::string style;
    for (const json& p : spec.value("params", json::array())) {
        if (p.value("name", std::string()) != "style") continue;
        const json choices = p.value("choices", json::array());
        if (params.contains("style")) {
            const json& given = params["style"];
            if (!given.is_string() || std::find(choices.begin(), choices.end(), given) == choices.end()) {
                std::string list;
                for (const json& c : choices) list += (list.empty() ? "" : ", ") + c.get<std::string>();
                throw GraphError("the braid's style is one of " + list + ", not " + given.dump());
            }
            style = given.get<std::string>();
        } else if (p.contains("default") && p["default"].is_string()) {
            style = p["default"].get<std::string>();
        }
    }
    if (!params.contains("feed")) params["feed"] = style == "cornrow";  // cornrows are fed along the scalp
    const json g = graphOf(edit);
    std::string anchor = after;
    if (anchor.empty()) {
        if (typeOf(g, "interpolate") == "Interpolate") anchor = "interpolate";
        for (const std::string& n : ofType(g, "Interpolate")) {
            if (anchor.empty() && !linksFrom(g, n).empty()) anchor = n;
        }
        if (anchor.empty()) throw GraphError("the groom's graph has no Interpolate node to braid after");
    } else if (!nodeOf(g, anchor)) {
        throw GraphError("no node '" + anchor + "' to braid after");
    }
    excludeFromHairGuides(edit, group);  // braid paths are not hair
    std::string paths = guidesOfGroup(graphOf(edit), group);
    if (paths.empty()) paths = addNodeAt(edit, "Guides", "braidPaths", json{{"group", group}}, groomSize);
    const std::string braid = addNodeAt(edit, "Braid", "braid", params, groomSize, placeBy(edit, anchor, kInsertColumn / 2.0, kInsertRow));
    const std::vector<Link> consumers = linksFrom(graphOf(edit), anchor);
    if (!consumers.empty()) insertBetween(edit, anchor, consumers.front().to, braid);
    else edit.connect(anchor, braid, "strands");
    edit.connect(paths, braid, "braids");
    if (const auto at = placeBy(edit, anchor, 0.0, 2.0 * kInsertRow)) setNodePosition(edit, paths, at->first, at->second);
    return braid;
}

BunNodes insertBun(GraphEdit& edit, const std::string& group, const std::string& style, const std::string& bunParamsJson,
                   const std::string& after, std::optional<double> groomSize) {
    json bunParams = bunParamsJson.empty() ? json::object() : json::parse(bunParamsJson, nullptr, false);
    if (!bunParams.is_object()) throw GraphError("a bun's params are a JSON object");
    // Sized for the Bun's defaults (radius 2, a turn 1 higher than the last, on a 20 unit head):
    // a braid about as wide as a turn rises, gathering the whole head.
    const double k = groomSize ? *groomSize / kDefaultsScalpSize : 1.0;
    const json braidParams{{"style", style}, {"width", 1.2 * k}, {"region", 30.0 * k}, {"end", "bound"},
                           {"widthProfile", json::array({json::array({0.0, 1.0}), json::array({1.0, 0.7})})}};
    BunNodes out;
    out.braid = insertBraid(edit, group, braidParams.dump(), after, groomSize);
    out.bun = addNodeAt(edit, "Bun", "bun", bunParams, groomSize, placeBy(edit, out.braid, 0.0, kInsertRow));
    edit.connect(guidesOfGroup(graphOf(edit), group), out.bun, "guides");
    edit.connect(out.bun, out.braid, "braids");
    return out;
}

BraidSculptNodes insertBraidSculpt(GraphEdit& edit, const std::string& braid, std::optional<double> groomSize) {
    const json g = graphOf(edit);
    if (typeOf(g, braid) != "Braid") throw GraphError("no Braid node '" + braid + "' to sculpt the sections of");
    BraidSculptNodes out;
    // Already there: a FollowGuides fed the Braid's hair, its guides from a Sculpt of its sections.
    for (const Link& l : linksFrom(g, braid)) {
        if (l.output != "sections" || typeOf(g, l.to) != "Sculpt") continue;
        for (const Link& f : linksFrom(g, l.to)) {
            if (typeOf(g, f.to) == "FollowGuides" && f.input == "guides") return {l.to, f.to, false};
        }
    }
    const std::vector<Link> fed = linksFrom(g, braid);  // (before the follow is made)
    out.made = true;
    out.sculpt = addNodeAt(edit, "Sculpt", "braidSculpt", json::object(), groomSize, placeBy(edit, braid, 0.0, kInsertRow));
    out.follow = addNodeAt(edit, "FollowGuides", "braidFollow", json::object(), groomSize,
                           placeBy(edit, braid, kInsertColumn / 2.0, kInsertRow / 2.0));
    edit.connect(braid, out.sculpt, "guides", "sections");
    // What the Braid's hair fed reads the follow (its other outputs' links stay).
    for (const Link& l : fed) {
        if (l.output.empty() || l.output == "out") edit.connect(out.follow, l.to, l.input);
    }
    edit.connect(braid, out.follow, "strands");
    edit.connect(braid, out.follow, "rest", "sections");
    edit.connect(out.sculpt, out.follow, "guides");
    return out;
}

FeatherNodes insertFeathers(GraphEdit& edit, const std::vector<FeatherDesign>& designs, const std::vector<double>& weights,
                            const std::string& source, const std::string& group, bool keepHair, std::optional<double> density,
                            std::optional<double> groomSize) {
    if (designs.empty() || designs.size() > 4) throw GraphError("one to four feather designs (a Split has four outputs)");
    const std::vector<double> shares = weights.empty() ? std::vector<double>(designs.size(), 1.0) : weights;
    if (shares.size() != designs.size()) throw GraphError("one weight per design");
    // Each design's feather Compound and its shafts' Scatter params, from its preset.
    struct Part {
        json compound;
        json scatter = json::object();
    };
    std::vector<Part> parts;
    for (const FeatherDesign& design : designs) {
        const json preset = json::parse(design.presetJson, nullptr, false);
        if (preset.is_discarded() || !preset.is_object()) throw GraphError("the feather design " + design.name + " is not a graph");
        Part part;
        for (const json& n : preset.value("nodes", json::array())) {
            if (!n.is_object()) continue;
            const std::string type = n.value("type", std::string());
            if (type == "Compound" && part.compound.is_null()) part.compound = n;
            if (type == "Scatter" && part.scatter.empty()) {
                const json p = n.value("params", json::object());
                for (const char* k : {"density", "length"}) {
                    if (p.is_object() && p.contains(k)) part.scatter[k] = p[k];
                }
            }
        }
        if (part.compound.is_null()) throw GraphError("the feather design " + design.name + " has no feather (a Compound node)");
        parts.push_back(std::move(part));
    }
    const std::string out = edit.outputNode();
    if (out.empty()) throw GraphError("the graph has no output to grow feathers into");
    FeatherNodes made;
    std::vector<std::string> row;  // the nodes made for the shafts, then the split
    std::string shaftOutput;
    if (source == "scatter") {
        const json g = graphOf(edit);
        const std::vector<std::string> scalps = ofType(g, "Scalp");
        std::string scalp = scalps.empty() ? std::string() : scalps.front();
        if (scalp.empty()) row.push_back(scalp = addNodeAt(edit, "Scalp", "scalp", nullptr, groomSize));
        std::string guides;
        for (const std::string& n : ofType(g, "Guides")) {
            if (guides.empty() && wordsOf(paramOf(g, n, "group")).empty()) guides = n;
        }
        if (guides.empty()) row.push_back(guides = addNodeAt(edit, "Guides", "guides", nullptr, groomSize));
        json scatterParams = parts.front().scatter;
        if (density) scatterParams["density"] = *density;
        const std::string scatter = addNodeAt(edit, "Scatter", "featherShafts", scatterParams, groomSize);
        const std::string shapes = addNodeAt(edit, "Interpolate", "featherShapes", nullptr, groomSize);
        edit.connect(scalp, scatter, "scalp");
        edit.connect(scatter, shapes, "strands");
        edit.connect(guides, shapes, "guides");
        edit.connect(scalp, shapes, "scalp");
        row.push_back(scatter);
        row.push_back(shapes);
        made.shafts = shapes;
    } else if (source == "guides") {
        if (group.empty()) throw GraphError("hand-placed feathers need a guide group");
        excludeFromHairGuides(edit, group);  // feather shafts are not hair
        made.shafts = guidesOfGroup(graphOf(edit), group);
        if (made.shafts.empty()) row.push_back(made.shafts = addNodeAt(edit, "Guides", "featherGuides", json{{"group", group}}, groomSize));
    } else {
        const std::size_t dot = source.find('.');
        made.shafts = source.substr(0, dot);
        if (dot != std::string::npos) shaftOutput = source.substr(dot + 1);
        if (made.shafts.empty() || !edit.hasNode(made.shafts)) {
            throw GraphError("the shafts come from scatter, guides or a node of the graph (node or node.output), not '" + source + "'");
        }
    }
    std::vector<std::pair<std::string, std::string>> feeds{{made.shafts, shaftOutput}};
    if (designs.size() > 1) {
        json params = json::object();
        for (std::size_t i = 0; i < 4; ++i) params[std::string("weight") + "ABCD"[i]] = i < shares.size() ? shares[i] : 0.0;
        made.split = addNodeAt(edit, "Split", "featherSplit", params, groomSize);
        edit.connect(made.shafts, made.split, "strands", shaftOutput);
        feeds.clear();
        for (std::size_t i = 0; i < designs.size(); ++i) feeds.emplace_back(made.split, std::string(1, "abcd"[i]));
        row.push_back(made.split);
    }
    // Each design a groom group of its own (groupId, groupName on its Propagates): Unreal gives it
    // its own material, LODs and physics. The same design again joins its group.
    std::map<std::string, int> groups = featherGroupsOf(graphOf(edit));
    for (std::size_t i = 0; i < designs.size(); ++i) {
        const std::string& design = designs[i].name;
        json params = parts[i].compound.value("params", json::object());
        if (!params.is_object()) params = json::object();
        if (!groups.count(design)) {
            int most = 0;
            for (const auto& [name, id] : groups) most = std::max(most, id);
            groups[design] = most + 1;
        }
        std::vector<json*> propagates;
        propagatesIn(params, &propagates);
        for (json* p : propagates) {
            (*p)["groupId"] = groups[design];
            (*p)["groupName"] = design;
        }
        const std::string name = addNodeAt(edit, "Compound", parts[i].compound.value("name", std::string("feathers")), params, groomSize);
        edit.connect(feeds[i].first, name, "shafts", feeds[i].second);
        made.designs.push_back(name);
    }
    std::string feathers = made.designs.front();
    for (std::size_t i = 1; i < made.designs.size(); ++i) {
        const std::string merge = addNodeAt(edit, "Merge", "featherMerge", nullptr, groomSize);
        edit.connect(feathers, merge, "a");
        edit.connect(made.designs[i], merge, "b");
        feathers = merge;
        made.merges.push_back(merge);
    }
    // A row under the output: the shafts, the split, the designs, then their merges.
    if (const auto at = nodePosition(edit, out)) {
        const double y = at->second + 2.0 * kInsertRow;
        const double x = at->first - static_cast<double>(row.size() + 2) * kInsertColumn;
        const double designsX = x + static_cast<double>(row.size()) * kInsertColumn;
        for (std::size_t i = 0; i < row.size(); ++i) setNodePosition(edit, row[i], x + static_cast<double>(i) * kInsertColumn, y);
        for (std::size_t i = 0; i < made.designs.size(); ++i) {
            setNodePosition(edit, made.designs[i], designsX, y + static_cast<double>(i) * kInsertRow);
        }
        for (std::size_t i = 0; i < made.merges.size(); ++i) {
            setNodePosition(edit, made.merges[i], designsX + kInsertColumn, y + static_cast<double>(i) * kInsertRow);
        }
    }
    if (keepHair && !sourceOf(graphOf(edit), out, "strands").empty()) {
        made.merge = addNodeAt(edit, "Merge", "feathersMerge", nullptr, groomSize);
        insertBefore(edit, out, made.merge, "strands", "a");
        edit.connect(feathers, made.merge, "b");
    } else {
        edit.connect(feathers, out, "strands");
    }
    return made;
}

std::map<std::string, int> featherGroups(const GraphEdit& edit) { return featherGroupsOf(graphOf(edit)); }

std::vector<std::string> featherGuideGroups(const GraphEdit& edit) {
    const json g = graphOf(edit);
    std::set<std::string> designs;
    for (const json& n : g["nodes"]) {
        if (n.is_object() && !propagatesOf(n).empty()) designs.insert(n.value("name", std::string()));
    }
    std::vector<std::string> found;
    for (const std::string& guides : ofType(g, "Guides")) {
        const std::vector<std::string> groups = wordsOf(paramOf(g, guides, "group"));
        if (groups.empty()) continue;
        std::set<std::string> seen;
        std::vector<std::string> todo{guides};
        bool feeds = false;
        while (!todo.empty() && !feeds) {
            const std::string n = todo.back();
            todo.pop_back();
            if (!seen.insert(n).second) continue;
            feeds = designs.count(n) > 0;
            for (const Link& l : linksFrom(g, n)) todo.push_back(l.to);
        }
        for (const std::string& name : groups) {
            if (feeds && std::find(found.begin(), found.end(), name) == found.end()) found.push_back(name);
        }
    }
    return found;
}

}  // namespace groomlab

namespace groomlab {

void insertNodesOnLink(GraphEdit& edit, const std::string& src, const std::string& dst, const std::string& input,
                       const std::vector<std::string>& nodes, const std::string& output) {
    std::vector<std::string> group;
    for (const std::string& n : edit.nodeNames()) {
        if (std::find(nodes.begin(), nodes.end(), n) != nodes.end()) group.push_back(n);
    }
    if (group.empty()) throw GraphError("no nodes to insert");
    if (group.size() == 1) {
        insertOnLink(edit, src, dst, input, group.front(), output);
        return;
    }
    const json g = graphOf(edit);
    auto feedsMember = [&](const std::string& from) {
        for (const json& c : g.value("connections", json::array())) {
            if (c.value("from", std::string()) == from && std::find(group.begin(), group.end(), c.value("to", std::string())) != group.end()) {
                return true;
            }
        }
        return false;
    };
    auto fedByMember = [&](const std::string& to) {
        for (const json& c : g.value("connections", json::array())) {
            if (c.value("to", std::string()) == to && std::find(group.begin(), group.end(), c.value("from", std::string())) != group.end()) {
                return true;
            }
        }
        return false;
    };
    std::string entry, exit;
    for (const std::string& n : group) {
        if (entry.empty() && !fedByMember(n)) entry = n;
        if (!feedsMember(n)) exit = n;
    }
    if (entry.empty() || exit.empty()) throw GraphError("the nodes have no one way in and out");
    if (std::find(group.begin(), group.end(), src) != group.end() || std::find(group.begin(), group.end(), dst) != group.end()) {
        throw GraphError("the link joins nodes of the group");
    }
    std::string why;
    for (const json& p : json::parse(edit.nodeSpecJson(entry)).value("inputs", json::array())) {
        GraphEdit trial = edit;
        try {
            trial.connect(src, entry, p.value("name", std::string()), output);
            trial.connect(exit, dst, input);
        } catch (const GraphError& e) {
            if (why.empty()) why = e.what();
            continue;
        }
        adoptUpstreamClumps(trial, group, dst);
        edit = std::move(trial);
        return;
    }
    throw GraphError(entry + " has no input that takes " + src + "'s output with " + exit + " feeding " + dst + "." + input +
                     (why.empty() ? "" : " (" + why + ")"));
}

}  // namespace groomlab
