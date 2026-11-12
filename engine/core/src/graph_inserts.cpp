// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_inserts.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"

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
