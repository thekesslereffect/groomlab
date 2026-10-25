// SPDX-License-Identifier: Apache-2.0
// Group and Ungroup (GraphEdit::collapse, GraphEdit::expand): the one implementation every host's
// node editor uses (Maya's Graph Editor through groomlab_core, the standalone app's node graph).
// Both work on a copy of the graph and take it only when every step fitted, so a refused edit
// leaves the graph as it was.

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

std::string str(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json& arrayOf(const json& object, const char* key) {
    static const json none = json::array();
    if (!object.is_object()) return none;
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? *it : none;
}

// A node's position ("ui"; none: the origin).
std::pair<double, double> positionOf(const json& node) {
    const auto it = node.find("ui");
    if (it == node.end() || !it->is_object()) return {0.0, 0.0};
    auto number = [&](const char* key) {
        const auto v = it->find(key);
        return v != it->end() && v->is_number() ? v->get<double>() : 0.0;
    };
    return {number("x"), number("y")};
}

json definitionOf(const json& node) {
    const json params = node.value("params", json::object());
    json d = params.is_object() ? params.value("definition", json::object()) : json::object();
    if (d.is_string()) d = json::parse(d.get<std::string>(), nullptr, false);
    return d.is_object() ? d : json::object();
}

std::string joined(const std::vector<std::string>& names, const char* separator) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : separator) + n;
    return out;
}

// GraphEdit::uniqueName over a set of names.
std::string uniqueAmong(const std::set<std::string>& names, const std::string& base) {
    std::string b = base;
    if (!b.empty()) b[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[0])));
    if (!names.count(b)) return b;
    int i = 1;
    while (names.count(b + std::to_string(i))) ++i;
    return b + std::to_string(i);
}

}  // namespace

std::string GraphEdit::collapse(const std::vector<std::string>& names, const std::string& compound) {
    const json graph = json::parse(toJson());
    const json& nodes = arrayOf(graph, "nodes");
    const json& links = arrayOf(graph, "connections");
    const std::set<std::string> wanted(names.begin(), names.end());
    std::vector<const json*> group;  // in the graph's order
    for (const json& n : nodes) {
        if (n.is_object() && wanted.count(str(n, "name"))) group.push_back(&n);
    }
    if (group.empty()) throw GraphError("nothing to collapse");
    std::set<std::string> inside;
    for (const json* n : group) inside.insert(str(*n, "name"));

    std::vector<json> incoming, outgoing, innerLinks;
    std::set<std::string> feeders;  // sorted, as the message lists them
    for (const json& c : links) {
        const bool from = inside.count(str(c, "from")) > 0, to = inside.count(str(c, "to")) > 0;
        if (to && !from) incoming.push_back(c);
        if (from && !to) {
            outgoing.push_back(c);
            feeders.insert(str(c, "from"));
        }
        if (from && to) innerLinks.push_back(c);
    }
    if (feeders.size() > 1) {
        throw GraphError("only one of the nodes may feed the rest of the graph (" +
                         joined({feeders.begin(), feeders.end()}, ", ") + " do)");
    }
    for (const json& c : outgoing) {
        if (!str(c, "output").empty()) {
            throw GraphError(str(c, "from") + " feeds the rest of the graph from its output '" + str(c, "output") +
                             "': a group gives its node's first output only");
        }
    }
    const std::string graphOutput = str(graph, "output");
    std::string innerOutput;
    if (!feeders.empty()) {
        innerOutput = *feeders.begin();
    } else {
        std::vector<std::string> sinks;
        for (const json* n : group) {
            const std::string name = str(*n, "name");
            const bool feedsInside = std::any_of(links.begin(), links.end(), [&](const json& c) {
                return str(c, "from") == name && inside.count(str(c, "to"));
            });
            if (!feedsInside) sinks.push_back(name);
        }
        if (inside.count(graphOutput)) sinks = {graphOutput};
        if (sinks.size() != 1) throw GraphError("the nodes need one output node");
        innerOutput = sinks.front();
    }

    // Inner nodes keep every field, so Ungroup gives them back as they were. One compound input
    // per outside source (node and output), an Input node inside giving it.
    json innerNodes = json::array();
    for (const json* n : group) innerNodes.push_back(*n);
    json inputs = json::array();
    std::vector<std::pair<std::pair<std::string, std::string>, std::string>> ports;  // (source, output) -> input, in order
    auto portOf = [&](const std::pair<std::string, std::string>& source) -> const std::string* {
        for (const auto& [s, port] : ports) {
            if (s == source) return &port;
        }
        return nullptr;
    };
    for (const json& c : incoming) {
        const std::pair<std::string, std::string> source{str(c, "from"), str(c, "output")};
        const std::string* port = portOf(source);
        if (!port) {
            const std::string input = str(c, "input");
            const bool taken = std::any_of(ports.begin(), ports.end(), [&](const auto& p) { return p.second == input; });
            const std::string name = !taken ? input : source.second.empty() ? source.first : source.first + "_" + source.second;
            const std::string type = outputType(source.first, source.second);
            inputs.push_back({{"name", name}, {"type", type}});
            innerNodes.push_back({{"name", "in_" + name}, {"type", "Input"}, {"params", {{"name", name}, {"type", type}}}});
            ports.emplace_back(source, name);
            port = &ports.back().second;
        }
        innerLinks.push_back({{"from", "in_" + *port}, {"to", str(c, "to")}, {"input", str(c, "input")}});
    }
    const json definition = {{"graph", {{"version", 1}, {"nodes", innerNodes}, {"connections", innerLinks}, {"output", innerOutput}}},
                             {"inputs", inputs},
                             {"expose", json::object()}};

    double sx = 0.0, sy = 0.0;
    for (const json* n : group) {
        const auto [x, y] = positionOf(*n);
        sx += x;
        sy += y;
    }
    GraphEdit work(*this);
    for (const std::string& n : inside) work.removeNode(n);
    const std::string name = work.addNode("Compound", compound.empty() ? work.uniqueName("compound") : compound,
                                          json{{"definition", definition}}.dump(), sx / group.size(), sy / group.size());
    for (const auto& [source, port] : ports) work.connect(source.first, name, port, source.second);
    for (const json& c : outgoing) work.connect(name, str(c, "to"), str(c, "input"));
    if (inside.count(graphOutput)) work.setOutputNode(name);
    *this = work;
    for (const json* n : group) recordMove(str(*n, "name"), name + "/" + str(*n, "name"));
    return name;
}

std::vector<std::string> GraphEdit::expand(const std::string& compound) {
    json graph = json::parse(toJson());
    const json* found = nullptr;
    for (const json& n : arrayOf(graph, "nodes")) {
        if (n.is_object() && str(n, "name") == compound) found = &n;
    }
    if (!found) throw GraphError("no node named '" + compound + "'");
    const json node = *found;
    if (str(node, "type") != "Compound") throw GraphError(compound + " is not a Compound");
    const json definition = definitionOf(node);
    const json inner = definition.value("graph", json::object());
    json innerNodes = arrayOf(inner, "nodes");
    for (const json& n : innerNodes) {  // an unknown type: refused before anything changes
        if (!OperatorRegistry::instance().contains(str(n, "type"))) throw GraphError("unknown operator type '" + str(n, "type") + "'");
    }
    // The compound's values of its exposed params go to the inner params they show.
    const json params = node.value("params", json::object());
    const json expose = definition.value("expose", json::object());
    if (expose.is_object() && params.is_object()) {
        for (const auto& [exposed, target] : expose.items()) {
            if (!target.is_string() || !params.contains(exposed)) continue;
            const std::string t = target.get<std::string>();
            const std::size_t dot = t.find('.');
            const std::string innerName = t.substr(0, dot), param = dot == std::string::npos ? std::string() : t.substr(dot + 1);
            for (json& n : innerNodes) {
                if (str(n, "name") != innerName) continue;
                if (!n.contains("params") || !n["params"].is_object()) n["params"] = json::object();
                n["params"][param] = params[exposed];
            }
        }
    }
    std::vector<const json*> real;
    for (const json& n : innerNodes) {
        if (str(n, "type") != "Input") real.push_back(&n);
    }
    const std::string innerOutput = str(inner, "output");
    if (std::none_of(real.begin(), real.end(), [&](const json* n) { return str(*n, "name") == innerOutput; })) {
        throw GraphError(compound + " has no output node inside");
    }
    std::map<std::string, std::pair<std::string, std::string>> sources;  // compound input -> (node, output)
    std::vector<json> consumers;
    for (const json& c : arrayOf(graph, "connections")) {
        if (str(c, "to") == compound) sources[str(c, "input")] = {str(c, "from"), str(c, "output")};
        if (str(c, "from") == compound) consumers.push_back(c);
    }
    const auto [x, y] = positionOf(node);
    const bool wasOutput = str(graph, "output") == compound;
    // Where they were when grouped, moved with the compound; nodes without a position fan out.
    double cx = x, cy = y;
    std::size_t placed = 0;
    double sx = 0.0, sy = 0.0;
    for (const json* n : real) {
        if (!n->contains("ui")) continue;
        const auto [nx, ny] = positionOf(*n);
        sx += nx;
        sy += ny;
        ++placed;
    }
    if (placed) {
        cx = sx / placed;
        cy = sy / placed;
    }

    GraphEdit work(*this);
    work.removeNode(compound);
    json g = json::parse(work.toJson());
    if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
    std::set<std::string> taken;
    for (const json& n : g["nodes"]) taken.insert(str(n, "name"));
    std::vector<std::pair<std::string, std::string>> renamed;  // inner name -> new name, in order
    std::map<std::string, std::pair<std::string, std::string>> fromInputs;  // Input node -> what feeds it
    for (std::size_t i = 0; i < innerNodes.size(); ++i) {
        const json& n = innerNodes[i];
        if (str(n, "type") == "Input") {
            const json p = n.value("params", json::object());
            const std::string port = p.is_object() && p.contains("name") && p["name"].is_string() ? p["name"].get<std::string>() : "strands";
            const auto s = sources.find(port);
            fromInputs[str(n, "name")] = s != sources.end() ? s->second : std::pair<std::string, std::string>();
            continue;
        }
        const std::string name = uniqueAmong(taken, str(n, "name"));
        taken.insert(name);
        json fields = n;
        fields["name"] = name;
        double px = x + 40.0 * static_cast<double>(i), py = y + 40.0 * static_cast<double>(i);
        if (n.contains("ui")) {
            const auto [nx, ny] = positionOf(n);
            px = nx - cx + x;
            py = ny - cy + y;
        }
        fields["ui"] = {{"x", px}, {"y", py}};
        if (!fields.contains("params") || !fields["params"].is_object()) fields["params"] = json::object();
        g["nodes"].push_back(std::move(fields));
        renamed.emplace_back(str(n, "name"), name);
    }
    auto newName = [&](const std::string& old) {
        for (const auto& [from, to] : renamed) {
            if (from == old) return to;
        }
        return std::string();
    };
    work = GraphEdit(g.dump());
    for (const json& c : arrayOf(inner, "connections")) {
        const std::string from = str(c, "from"), to = str(c, "to");
        std::string src, output;
        if (const auto in = fromInputs.find(from); in != fromInputs.end()) {
            src = in->second.first;
            output = in->second.second;
        } else {
            src = newName(from);
            output = str(c, "output");
        }
        const std::string dst = newName(to);
        if (dst.empty()) throw GraphError(compound + " links " + from + " to a node it does not hold (" + to + ")");
        if (!src.empty()) work.connect(src, dst, str(c, "input"), output);
    }
    const std::string out = newName(innerOutput);
    for (const json& c : consumers) work.connect(out, str(c, "to"), str(c, "input"));
    if (wasOutput) work.setOutputNode(out);
    const auto moved = movedNodes();  // work was made from the text alone: this edit's moves so far
    *this = work;
    for (const auto& [from, to] : moved) recordMove(from, to);
    for (const auto& [from, to] : renamed) recordMove(compound + "/" + from, to);
    std::vector<std::string> made;
    for (const auto& [from, to] : renamed) made.push_back(to);
    return made;
}

std::pair<std::string, std::string> GraphEdit::FlatCompounds::param(const std::string& node, const std::string& name) const {
    std::pair<std::string, std::string> at{node, name};
    for (int guard = 0; guard < 16; ++guard) {  // (a compound inside a compound: its exposed param in turn)
        const auto c = exposed.find(at.first);
        if (c == exposed.end()) break;
        const auto p = c->second.find(at.second);
        if (p == c->second.end()) break;
        at = p->second;
    }
    return at;
}

std::string GraphEdit::FlatCompounds::result(const std::string& node) const {
    std::string at = node;
    for (int guard = 0; guard < 16; ++guard) {
        const auto o = output.find(at);
        if (o == output.end()) break;
        at = o->second;
    }
    return at;
}

GraphEdit::FlatCompounds GraphEdit::flattenCompounds() {
    FlatCompounds flat;
    std::set<std::string> kept;  // compounds left as they are
    for (int guard = 0; guard < 4096; ++guard) {
        // The next compound to flatten (the outer ones first: those inside come out with them).
        const json graph = json::parse(toJson());
        const json* next = nullptr;
        for (const json& n : arrayOf(graph, "nodes")) {
            if (!n.is_object() || str(n, "type") != "Compound" || kept.count(str(n, "name"))) continue;
            const json params = n.value("params", json::object());
            const bool repeats = params.is_object() && params.contains("repeat") && params["repeat"].is_number() &&
                                 params["repeat"].get<double>() != 1.0;
            const bool sometimes = n.contains("evaluation") && n["evaluation"] != "always";
            // (Its pass, read through an Input named "iteration": the compound's to give.)
            bool counts = false;
            const json innerGraph = definitionOf(n).value("graph", json::object());
            for (const json& i : arrayOf(innerGraph, "nodes")) {
                const json ip = i.value("params", json::object());
                counts = counts || (str(i, "type") == "Input" && ip.is_object() && ip.value("name", std::string()) == "iteration");
            }
            if (repeats || sometimes || counts || n.value("bypass", false) || n.contains("paramControls")) {
                kept.insert(str(n, "name"));
                continue;
            }
            next = &n;
            break;
        }
        if (!next) break;
        const std::string compound = str(*next, "name");
        const json definition = definitionOf(*next);
        const json inner = definition.value("graph", json::object());
        std::vector<std::string> names;  // the inner nodes Ungroup makes, in its order
        for (const json& n : arrayOf(inner, "nodes")) {
            if (str(n, "type") != "Input") names.push_back(str(n, "name"));
        }
        std::vector<std::string> made;
        try {
            made = expand(compound);
        } catch (const GraphError&) {
            kept.insert(compound);  // (a library node's copy that cannot run: it hands its input on, as itself)
            continue;
        }
        if (made.size() != names.size()) throw GraphError("flattening " + compound + ": its nodes came out " + std::to_string(made.size()));
        // Each by its path, as the compound named it.
        for (std::size_t i = 0; i < made.size(); ++i) {
            const std::string path = compound + "/" + names[i];
            if (made[i] != path) renameNode(made[i], path);
        }
        flat.output[compound] = compound + "/" + str(inner, "output");
        const json expose = definition.value("expose", json::object());
        if (expose.is_object()) {
            for (const auto& [name, target] : expose.items()) {
                if (!target.is_string()) continue;
                const std::string t = target.get<std::string>();
                const std::size_t dot = t.find('.');
                if (dot == std::string::npos) continue;
                flat.exposed[compound][name] = {compound + "/" + t.substr(0, dot), t.substr(dot + 1)};
            }
        }
    }
    return flat;
}

}  // namespace groomlab
