// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_edit.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/attributes.h"
#include "groomlab/braid_tubes.h"
#include "groomlab/graph_io.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"
#include "groomlab/node_library.h"
#include "groomlab/operators.h"
#include "groomlab/presets.h"
#include "graph_internal.h"
#include "operators/internal.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

constexpr double kLayoutColumn = 240.0;  // a new node's column right of the graph (the Graph Editor's LAYOUT_DX)

std::string quoted(const std::string& s) { return "'" + s + "'"; }

// Every registered operator's spec (operatorSpecsJson), by type: read once (again when an operator
// was registered or dropped since: a test's; the registry's generation, not the count, which one
// dropped and another registered leaves as it was).
const std::map<std::string, json>& catalog() {
    static std::map<std::string, json> specs;
    static std::uint64_t registered = 0;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const std::uint64_t now = OperatorRegistry::instance().generation();
    if (specs.empty() || now != registered) {
        std::map<std::string, json> out;
        for (json& spec : json::parse(operatorSpecsJson())) {
            const std::string type = spec.at("type").get<std::string>();
            out[type] = std::move(spec);
        }
        specs = std::move(out);
        registered = now;
    }
    return specs;
}

const json& operatorSpec(const std::string& type) {
    const auto it = catalog().find(type);
    if (it == catalog().end()) throw GraphError("unknown operator type " + quoted(type));
    return it->second;
}

const json* paramSpecOf(const json& spec, const std::string& param) {
    const auto params = spec.find("params");
    if (params == spec.end() || !params->is_array()) return nullptr;
    for (const json& p : *params) {
        if (p.is_object() && p.value("name", std::string()) == param) return &p;
    }
    return nullptr;
}

std::string kindLabel(const std::string& type) {
    if (type == "Strands") return "strands";
    if (type == "Guides") return "guides";
    if (type == "Curves") return "strands or guides";
    return type;
}

bool typesMatch(const std::string& out, const std::string& in) {
    if (out == in) return true;
    try {
        return portTypesMatch(portTypeFromName(out), portTypeFromName(in));
    } catch (const GraphError&) {  // a name no port type has
        return false;
    }
}

// A port's type as a message names it: a Value's with its kind ("a number Value").
std::string portLabel(const json& port, const std::string& type) {
    if (type != "Value") return kindLabel(type);
    const std::string kind = port.value("valueKind", std::string("any"));
    return kind == "any" ? std::string("a Value") : "a " + kind + " Value";
}

json definitionOf(const json& node) {
    const json params = node.value("params", json::object());
    json d = params.value("definition", json::object());
    if (d.is_string()) d = json::parse(d.get<std::string>(), nullptr, false);
    return d.is_object() ? d : json::object();
}

// The type a compound's definition gives: its inner output node's.
std::string definitionOutputType(const json& definition, int depth = 0) {
    const json inner = definition.value("graph", json::object());
    const std::string outputName = inner.value("output", std::string());
    for (const json& n : inner.value("nodes", json::array())) {
        if (n.value("name", std::string()) != outputName) continue;
        const std::string type = n.value("type", std::string());
        if (type == "Input") return n.value("params", json::object()).value("type", std::string("Strands"));
        if (type == "Compound" && depth < 16) return definitionOutputType(definitionOf(n), depth + 1);
        const json& spec = operatorSpec(type);
        const json outputs = spec.value("outputs", json::array());
        return outputs.empty() ? spec.at("output").value("type", std::string("Strands")) : outputs[0].value("type", std::string("Strands"));
    }
    return "Strands";
}

// The behaviour a node's JSON was saved with ("version"; none: its operator's newest).
int storedVersion(const json& node, const json& spec) {
    const int newest = spec.value("version", 1);
    const auto it = node.find("version");
    return it != node.end() && it->is_number_integer() ? it->get<int>() : newest;
}

// Fills in every param the node's JSON leaves out with its spec default, and its version when it
// has none (the newest), inside compounds' definitions too: what a saved graph holds.
void completeNode(json& node, int depth = 0) {
    if (!node.is_object()) return;
    const std::string type = node.value("type", std::string());
    const auto it = catalog().find(type);
    if (it == catalog().end()) return;  // unknown: loading reports it
    const json& spec = it->second;
    if (!node.contains("version")) node["version"] = spec.value("version", 1);
    if (!node.contains("params") || !node["params"].is_object()) node["params"] = json::object();
    json& params = node["params"];
    const int version = storedVersion(node, spec);
    for (const json& p : spec.value("params", json::array())) {
        const std::string name = p.value("name", std::string());
        if (name.empty() || params.contains(name)) continue;
        // (A default changed since the node's version: the one it was saved with.)
        const json former = p.value("formerDefault", json());
        const bool older = former.is_object() && version < former.value("since", 0);
        params[name] = older ? former.at("value") : p.value("default", json());
    }
    if (type == "Script") {  // its header's params too (a definition that cannot be read: none)
        try {
            const std::unique_ptr<Operator> op = operatorFromJson(type, json{{"definition", definitionOf(node).dump()}}.dump());
            for (const ParamSpec& p : op->extraParamSpecs()) {
                if (!params.contains(p.name)) params[p.name] = json::parse(paramSpecJson(p))["default"];
            }
        } catch (const std::exception&) {
        }
    }
    if (type == "Compound" && depth < 16) {
        json& definition = params["definition"];
        const bool text = definition.is_string();
        json d = text ? json::parse(definition.get<std::string>(), nullptr, false) : definition;
        if (d.is_object() && d.contains("graph") && d["graph"].is_object()) {
            json& inner = d["graph"];
            if (inner.contains("nodes") && inner["nodes"].is_array()) {
                for (json& n : inner["nodes"]) completeNode(n, depth + 1);
            }
            definition = text ? json(d.dump()) : d;
        }
    }
}

// A library node's (a Compound's or a Script's copy of a .glnode file) label, description and
// "library" info: what it is, and whether the library has a newer copy (an Update:
// GraphEdit::updateLibraryNode).
void addLibraryInfo(json& spec, const json& definition) {
    const std::string id = definition["id"].get<std::string>();
    const int version = definition.value("version", json()).is_number_integer() ? definition["version"].get<int>() : 0;
    const std::optional<LibraryNode> latest = findLibraryNode(id);
    const std::string label = definition.value("label", json()).is_string() ? definition["label"].get<std::string>() : id;
    spec["label"] = label.empty() ? id : label;
    if (definition.value("description", json()).is_string()) spec["description"] = definition["description"];
    json info = {{"id", id}, {"version", version}, {"latest", latest ? latest->version : 0},
                 {"updateAvailable", latest && latest->version > version},
                 {"embedded", definition.contains("graph") || definition.contains("script")},
                 {"kind", definition.contains("script") ? "script" : "compound"}};
    for (const char* key : {"category", "author", "license", "icon"}) {
        info[key] = definition.value(key, json()).is_string() ? definition[key].get<std::string>() : std::string();
    }
    spec["library"] = info;
}

json specOfNode(const json& node) {
    const std::string type = node.value("type", std::string());
    json spec = operatorSpec(type);
    // An Upgrade is available when the node keeps an older behaviour than its operator's newest.
    const int stored = storedVersion(node, spec);
    spec["nodeVersion"] = stored;
    spec["outdated"] = stored < spec.value("version", 1);
    if (!spec.contains("outputs")) spec["outputs"] = json::array({spec["output"]});
    const json params = node.value("params", json::object());
    // A Value output of a kind no spec can say (any): an Input's, a compound's.
    auto anyValue = [](json& port) {
        if (port.value("type", std::string()) == "Value") port["valueKind"] = "any";
        else port.erase("valueKind");
    };
    if (type == "Input") {
        spec["output"]["type"] = params.value("type", std::string("Strands"));
        anyValue(spec["output"]);
        spec["outputs"] = json::array({spec["output"]});
    } else if (type == "Value" || type == "Expression") {
        // Their output as their params set it (ValueOp, ExpressionOp): the node made as they are.
        const std::unique_ptr<Operator> op = operatorFromJson(type, params.dump());
        spec["output"] = {{"name", op->output().name}, {"type", portTypeName(op->output().type)}};
        if (op->output().type == PortType::Value) spec["output"]["valueKind"] = valueKindName(op->output().valueKind);
        spec["outputs"] = json::array({spec["output"]});
    } else if (type == "Compound") {
        const json definition = definitionOf(node);
        const bool library = definition.contains("glnode") && definition.value("id", json()).is_string();
        json inputs = json::array();
        for (const json& port : definition.value("inputs", json::array())) {
            if (!port.is_object()) continue;
            json in = {{"name", port.value("name", std::string())},
                       {"type", port.value("type", std::string("Strands"))},
                       {"optional", port.value("optional", false)}};
            if (in["type"] == "Value") in["valueKind"] = port.value("valueKind", std::string("any"));
            inputs.push_back(in);
        }
        if (library && inputs.empty() && !definition.contains("graph")) inputs.push_back({{"name", "strands"}, {"type", "Strands"}, {"optional", false}});
        spec["inputs"] = inputs;
        try {
            spec["output"]["type"] = definitionOutputType(definition);
            if (library && !definition.contains("graph")) throw GraphError("no copy");
        } catch (const GraphError&) {
            // A library node that cannot run hands its first input on (CompoundOp).
            if (!library) throw;
            spec["output"]["type"] = inputs.empty() ? std::string("Strands") : inputs[0].value("type", std::string("Strands"));
        }
        anyValue(spec["output"]);
        spec["outputs"] = json::array({spec["output"]});  // a compound gives one output
        // The exposed params, each with its inner param's spec (range, unit, group, advanced...),
        // its default the inner node's value. In the look's order: the inner nodes as the graph
        // lists them, each one's params as its operator does (two names for one param: by name).
        const json inner = definition.value("graph", json::object());
        const json expose = definition.value("expose", json::object());
        std::map<std::string, json> shown;  // a library node's params by name
        for (const json& p : library ? definition.value("params", json::array()) : json::array()) {
            if (p.is_object() && p.value("name", json()).is_string()) shown[p["name"].get<std::string>()] = p;
        }
        // The params shown by hand (Show Inner Param, Promote to Compound: compoundExpose), each
        // under a header named after its node; a preset's own keep their inner groups.
        std::set<std::string> byNode;
        for (const json& n : definition.value("exposeByNode", json::array())) {
            if (n.is_string()) byNode.insert(n.get<std::string>());
        }
        std::map<std::string, std::vector<std::string>> exposedAs;  // "node.param" -> the names showing it
        if (expose.is_object()) {
            for (const auto& [exposed, target] : expose.items()) {
                if (target.is_string()) exposedAs[target.get<std::string>()].push_back(exposed);
            }
        }
        for (const json& n : inner.value("nodes", json::array())) {
            if (exposedAs.empty() || !n.is_object()) break;
            const std::string nodeName = n.value("name", std::string());
            const auto it = catalog().find(n.value("type", std::string()));
            if (it == catalog().end()) continue;
            const json innerParams = n.value("params", json::object());
            for (const json& innerSpec : it->second.value("params", json::array())) {
                const std::string param = innerSpec.value("name", std::string());
                const auto names = exposedAs.find(nodeName + "." + param);
                if (names == exposedAs.end()) continue;
                for (const std::string& exposed : names->second) {
                    json exposedSpec = innerSpec;
                    exposedSpec["name"] = exposed;
                    exposedSpec["description"] = innerSpec.value("description", std::string()) + " (exposed " + names->first + ")";
                    if (innerParams.is_object() && innerParams.contains(param)) exposedSpec["default"] = innerParams[param];
                    // Shown by hand: under its node, a group per inner node (noise's params
                    // together, then length's), unless a library node's header gives it a group.
                    if (byNode.count(exposed)) {
                        exposedSpec["group"] = nodeName;
                        exposedSpec["groupShown"] = true;
                        exposedSpec.erase("advanced");  // (under its node, not in Advanced)
                    }
                    // A library node's header says how it shows (node_library.h).
                    if (const auto own = shown.find(exposed); own != shown.end()) {
                        const json& h = own->second;
                        for (const char* key : {"label", "description", "group"}) {
                            if (h.contains(key) && h[key].is_string() && !h[key].get<std::string>().empty()) exposedSpec[key] = h[key];
                        }
                        for (const char* key : {"min", "max", "softMin", "softMax"}) {
                            if (h.contains(key) && h[key].is_number()) exposedSpec[key] = h[key];
                        }
                    }
                    // As an input of the compound's node: one value (CompoundOp::extraParamSpecs).
                    if (exposedSpec.contains("port")) {
                        const std::string kind = exposedSpec.value("kind", std::string());
                        const json* choices = exposedSpec.contains("choices") ? &exposedSpec["choices"] : nullptr;
                        const bool choice = choices && choices->is_array() && !choices->empty();
                        const std::string valueKind = kind == "float" ? "number" : kind == "int" ? "integer"
                                                    : kind == "bool"  ? "bool"   : kind == "vec3" ? "vec3"
                                                    : kind == "string" ? (choice ? "choice" : "text") : "";
                        if (valueKind.empty()) exposedSpec.erase("port");
                        else exposedSpec["port"] = {{"type", "Value"}, {"valueKind", valueKind}};
                    }
                    // When it shows: by the compound's param for the inner param it names when that
                    // is exposed too; else always (the compound cannot change what it names).
                    if (exposedSpec.contains("showWhen")) {
                        json& when = exposedSpec["showWhen"];
                        const auto on = exposedAs.find(nodeName + "." + when.value("param", std::string()));
                        if (on != exposedAs.end()) when["param"] = on->second.front();
                        else exposedSpec.erase("showWhen");
                    }
                    spec["params"].push_back(exposedSpec);
                }
            }
        }
        if (library) addLibraryInfo(spec, definition);
    } else if (type == "Script") {
        // Its ports and params as its definition says (ScriptOp): the operator made as it is.
        const json definition = definitionOf(node);
        const std::unique_ptr<Operator> op = operatorFromJson(type, json{{"definition", definition.dump()}}.dump());
        json inputs = json::array();
        for (const PortSpec& p : op->inputs()) {
            inputs.push_back({{"name", p.name}, {"type", portTypeName(p.type)}, {"optional", p.optional}});
        }
        spec["inputs"] = inputs;
        std::map<std::string, std::string> labels;  // the header's labels of its params
        for (const json& p : definition.value("params", json::array())) {
            if (p.is_object() && p.value("name", json()).is_string() && p.value("label", json()).is_string()) {
                labels[p["name"].get<std::string>()] = p["label"].get<std::string>();
            }
        }
        for (const ParamSpec& p : op->extraParamSpecs()) {
            json param = json::parse(paramSpecJson(p));
            if (const auto label = labels.find(p.name); label != labels.end() && !label->second.empty()) param["label"] = label->second;
            spec["params"].push_back(std::move(param));
        }
        spec["scriptNode"] = true;
        if (definition.contains("glnode") && definition.value("id", json()).is_string()) addLibraryInfo(spec, definition);
    }
    const json exposedInputs = node.value("paramInputs", json::array());
    if (exposedInputs.is_array() && !exposedInputs.empty()) {
        // Each its param's port (operatorSpecsJson's "port": Field, Curve or a Value of its kind).
        std::map<std::string, json> ports;
        for (const json& p : spec.value("params", json::array())) {
            if (p.contains("port")) ports[p.value("name", std::string())] = p["port"];
        }
        for (const json& param : exposedInputs) {
            if (!param.is_string()) continue;
            const std::string name = param.get<std::string>();
            const auto port = ports.find(name);
            json in = {{"name", name}, {"type", port != ports.end() ? port->second.value("type", std::string("Field")) : "Field"},
                       {"optional", true}, {"param", true}};
            if (port != ports.end() && port->second.contains("valueKind")) in["valueKind"] = port->second["valueKind"];
            const auto paramSpec = std::find_if(spec["params"].begin(), spec["params"].end(), [&](const json& p) {
                return p.value("name", std::string()) == name;
            });
            if (paramSpec != spec["params"].end() && paramSpec->value("colorField", false)) in["valueKind"] = "vec3";
            spec["inputs"].push_back(in);
        }
    }
    // Each input it passes through: one more output handing it on (GraphEdit::passThrough).
    const json passes = node.value("passThrough", json::array());
    if (passes.is_array()) {
        for (const json& p : passes) {
            if (!p.is_string()) continue;
            for (const json& in : spec["inputs"]) {
                if (in.value("name", std::string()) != p.get<std::string>()) continue;
                json out = {{"name", GraphEdit::passThroughOutput(p.get<std::string>())},
                            {"type", in.value("type", std::string("Strands"))},
                            {"passThrough", p}};
                if (in.contains("valueKind")) out["valueKind"] = in["valueKind"];
                if (const json names = node.value("passThroughNames", json::object()); names.is_object() && names.contains(p.get<std::string>()) &&
                                                                                         names[p.get<std::string>()].is_string()) {
                    out["label"] = names[p.get<std::string>()];
                }
                spec["outputs"].push_back(std::move(out));
                break;
            }
        }
    }
    return spec;
}

}  // namespace

struct GraphEdit::Impl {
    json graph;
    std::vector<std::pair<std::string, std::string>> moved;  // movedNodes

    json& nodes() {
        if (!graph.contains("nodes") || !graph["nodes"].is_array()) graph["nodes"] = json::array();
        return graph["nodes"];
    }
    const json& nodesView() const {
        static const json none = json::array();
        const auto it = graph.find("nodes");
        return it != graph.end() && it->is_array() ? *it : none;
    }
    json& connections() {
        if (!graph.contains("connections") || !graph["connections"].is_array()) graph["connections"] = json::array();
        return graph["connections"];
    }
    const json& connectionsView() const {
        static const json none = json::array();
        const auto it = graph.find("connections");
        return it != graph.end() && it->is_array() ? *it : none;
    }
    const json* find(const std::string& name) const {
        for (const json& n : nodesView()) {
            if (n.is_object() && n.value("name", std::string()) == name) return &n;
        }
        return nullptr;
    }
    const json& node(const std::string& name) const {
        const json* n = find(name);
        if (!n) throw GraphError("no node named " + quoted(name));
        return *n;
    }

    json spec(const std::string& name) const { return specOfNode(node(name)); }

    json inputSpec(const std::string& name, const std::string& input) const {
        const json s = spec(name);
        for (const json& port : s["inputs"]) {
            if (port.value("name", std::string()) == input) return port;
        }
        throw GraphError(name + " has no input " + quoted(input));
    }
    std::vector<std::string> outputNames(const std::string& name) const {
        std::vector<std::string> out;
        const json s = spec(name);
        for (const json& port : s["outputs"]) out.push_back(port.value("name", std::string()));
        return out;
    }
    json outputSpec(const std::string& name, const std::string& output) const {
        const json outputs = spec(name)["outputs"];
        if (output.empty()) return outputs.at(0);
        std::string listed;
        for (const json& port : outputs) {
            if (port.value("name", std::string()) == output) return port;
            listed += (listed.empty() ? "" : ", ") + port.value("name", std::string());
        }
        throw GraphError(name + " has no output " + quoted(output) + " (" + listed + ")");
    }
    std::string sourceOf(const std::string& dst, const std::string& input, std::string* output = nullptr) const {
        for (const json& c : connectionsView()) {
            if (c.value("to", std::string()) == dst && c.value("input", std::string()) == input) {
                if (output) *output = c.value("output", std::string());
                return c.value("from", std::string());
            }
        }
        return {};
    }
    std::string outputType(const std::string& name, const std::string& output, std::set<std::string>& seen) const {
        const json port = outputSpec(name, output);
        if (port.contains("passThrough") && port["passThrough"].is_string()) {
            // A pass-through gives what feeds its input (its input's type while nothing does).
            std::string sourceOutput;
            const std::string source = sourceOf(name, port["passThrough"].get<std::string>(), &sourceOutput);
            if (!source.empty() && seen.insert("\x1f" + name + "\x1f" + port["passThrough"].get<std::string>()).second) {
                return outputType(source, sourceOutput, seen);
            }
            return port.value("type", std::string());
        }
        const std::string out = port.value("type", std::string());
        if (out != "Curves" || !seen.insert(name).second) return out;
        const json s = spec(name);
        for (const json& port : s["inputs"]) {
            if (port.value("type", std::string()) != "Curves") continue;
            std::string sourceOutput;
            const std::string source = sourceOf(name, port.value("name", std::string()), &sourceOutput);
            return source.empty() ? out : outputType(source, sourceOutput, seen);
        }
        return out;
    }
    std::set<std::string> upstream(const std::string& name) const {
        std::set<std::string> seen;
        std::vector<std::string> stack{name};
        while (!stack.empty()) {
            const std::string n = stack.back();
            stack.pop_back();
            for (const json& c : connectionsView()) {
                if (c.value("to", std::string()) != n) continue;
                const std::string from = c.value("from", std::string());
                if (seen.insert(from).second) stack.push_back(from);
            }
        }
        return seen;
    }
    // {node: seed} of the nodes of `type`: their own seed, else the default (integers only).
    std::vector<std::pair<std::string, std::int64_t>> seedsOf(const std::string& type) const {
        std::vector<std::pair<std::string, std::int64_t>> out;
        const json* spec = paramSpecOf(operatorSpec(type), "seed");
        for (const json& n : nodesView()) {
            if (!n.is_object() || n.value("type", std::string()) != type) continue;
            const json params = n.value("params", json::object());
            const json value = params.contains("seed") ? params["seed"] : spec ? spec->value("default", json()) : json();
            if (value.is_number_integer()) out.emplace_back(n.value("name", std::string()), value.get<std::int64_t>());
        }
        return out;
    }
    void disconnect(const std::string& dst, const std::string& input, bool* found = nullptr) {
        json kept = json::array();
        bool any = false;
        for (const json& c : connectionsView()) {
            if (c.value("to", std::string()) == dst && c.value("input", std::string()) == input) {
                any = true;
            } else {
                kept.push_back(c);
            }
        }
        if (any) connections() = std::move(kept);
        if (found) *found = any;
    }
    void connect(const std::string& src, const std::string& dst, const std::string& input, const std::string& output) {
        const json port = inputSpec(dst, input);
        const json out = outputSpec(src, output);
        std::set<std::string> seen;
        const std::string outType = outputType(src, output, seen);
        const std::string inType = port.value("type", std::string());
        const std::string named = !output.empty() && outputNames(src).size() > 1 ? src + "." + output : src;
        if (outType == "Field" && inType == "Value") {
            throw GraphError("cannot connect " + named + " to " + dst + "." + input + ": it gives a value per strand (a " +
                             "Field), and " + input + " takes one value for the whole node (drive it with a Value node, " +
                             "or an Expression set to give a value)");
        }
        bool match = typesMatch(outType, inType);
        if (match && outType == "Value") {
            const ValueKind outKind = valueKindFromName(out.value("valueKind", std::string("any")));
            const ValueKind described = valueKindFromName(port.value("valueKind", std::string("any")));
            const ValueKind inKind = inType == "Field" && described == ValueKind::Any ? ValueKind::Number : described;
            match = valueKindsMatch(outKind, inKind);
        }
        if (!match) {
            throw GraphError("cannot connect " + named + " (it gives " + portLabel(out, outType) + ") to " + dst + "." + input +
                             " (it takes " + (inType == "Field" ? std::string("a Field or a number") : portLabel(port, inType)) + ")");
        }
        if (src == dst || upstream(src).count(dst)) throw GraphError("connecting " + src + " to " + dst + " would create a cycle");
        disconnect(dst, input);
        json link = {{"from", src}, {"to", dst}, {"input", input}};
        if (out.value("name", std::string()) != outputNames(src).at(0)) link["output"] = out.value("name", std::string());
        connections().push_back(std::move(link));
    }
};

GraphEdit::GraphEdit(const std::string& graphJson) : impl_(std::make_unique<Impl>()) {
    const bool blank = graphJson.find_first_not_of(" \t\r\n") == std::string::npos;
    impl_->graph = json::parse(blank ? std::string("{}") : graphJson, nullptr, false);
    if (impl_->graph.is_discarded() || !impl_->graph.is_object()) throw GraphError("the graph is not a JSON object");
    // A library node by its type ("node:<id>", a graph written by hand): the Compound it stands for.
    const auto nodes = impl_->graph.find("nodes");
    if (nodes == impl_->graph.end() || !nodes->is_array()) return;
    for (json& n : *nodes) {
        const std::string id = n.is_object() ? libraryNodeId(n.value("type", std::string())) : std::string();
        if (id.empty()) continue;
        const json params = n.value("params", json::object());
        std::string type;
        n["params"] = json::parse(libraryNodeStubParams(id, params.is_object() ? params.dump() : std::string(), &type));
        n["type"] = type;  // the Compound or Script it stands for
    }
}

GraphEdit::~GraphEdit() = default;
GraphEdit::GraphEdit(const GraphEdit& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
GraphEdit& GraphEdit::operator=(const GraphEdit& other) {
    *impl_ = *other.impl_;
    return *this;
}

std::string GraphEdit::toJson() const { return impl_->graph.dump(); }

bool GraphEdit::hasNode(const std::string& name) const { return impl_->find(name) != nullptr; }

std::string GraphEdit::nodeType(const std::string& name) const { return impl_->node(name).value("type", std::string()); }

std::vector<std::string> GraphEdit::nodeNames() const {
    std::vector<std::string> out;
    for (const nlohmann::json& n : impl_->nodesView()) {
        if (n.is_object()) out.push_back(n.value("name", std::string()));
    }
    return out;
}

std::string GraphEdit::uniqueName(const std::string& base) const {
    std::string b = base;
    if (!b.empty()) b[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[0])));
    if (!hasNode(b)) return b;
    int i = 1;
    while (hasNode(b + std::to_string(i))) ++i;
    return b + std::to_string(i);
}

std::string GraphEdit::addNode(const std::string& type, const std::string& name, const std::string& paramsJson,
                               std::optional<double> x, std::optional<double> y, std::optional<double> groomSize) {
    if (const std::string id = libraryNodeId(type); !id.empty()) {
        // A library node: a Compound holding the library's copy, fitted to the groom.
        const std::optional<LibraryNode> library = findLibraryNode(id);
        if (!library) throw GraphError("no library node " + quoted(id) + " in the node folders");
        nlohmann::json params = paramsJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(paramsJson, nullptr, false);
        if (params.is_discarded() || !params.is_object()) throw GraphError("a node's params are a JSON object");
        params["definition"] = nlohmann::json::parse(libraryNodeParamsJson(*library, groomSize))["definition"];
        const std::string made = library->kind == "script" ? op::kScript : op::kCompound;  // a script node: a Script
        return addNode(made, name.empty() ? uniqueName(libraryNodeName(library->label)) : name, params.dump(), x, y);
    }
    const nlohmann::json& spec = operatorSpec(type);
    const std::string chosen = name.empty() ? uniqueName(type) : name;
    if (hasNode(chosen)) throw GraphError("a node named " + quoted(chosen) + " exists");
    nlohmann::json params = paramsJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(paramsJson, nullptr, false);
    if (params.is_discarded() || !params.is_object()) throw GraphError("a node's params are a JSON object");
    if (groomSize) params = nlohmann::json::parse(fitNewNodeParamsJson(type, params.dump(), groomSize));
    // A seed no other node of its type has: two Scatters with one seed would make the same
    // strands, two Flyaways pick the same strays.
    if (const nlohmann::json* seed = paramSpecOf(spec, "seed"); seed && !params.contains("seed")) {
        const auto used = impl_->seedsOf(type);
        const nlohmann::json fallback = seed->value("default", nlohmann::json());
        const bool taken = fallback.is_number_integer() &&
                           std::any_of(used.begin(), used.end(), [&](const auto& s) { return s.second == fallback.get<std::int64_t>(); });
        if (taken) {
            std::int64_t most = used.front().second;
            for (const auto& s : used) most = std::max(most, s.second);
            params["seed"] = most + 1;
        }
    }
    if (!x) {  // a free column right of the graph
        double right = -kLayoutColumn;
        bool any = false;
        for (const nlohmann::json& n : impl_->nodesView()) {
            const nlohmann::json ui = n.value("ui", nlohmann::json::object());
            const double nx = ui.is_object() ? ui.value("x", 0.0) : 0.0;
            right = any ? std::max(right, nx) : nx;
            any = true;
        }
        x = right + kLayoutColumn;
        if (!y) y = 0.0;
    }
    nlohmann::json node = {{"name", chosen}, {"type", type}, {"version", spec.value("version", 1)}, {"params", params},
                           {"ui", {{"x", *x}, {"y", y.value_or(0.0)}}}};
    completeNode(node);  // every param written: a later default change never alters it
    // Its params shown as inputs from the start (ParamSpec::shownAsInput), in the spec's order.
    nlohmann::json shown = nlohmann::json::array();
    for (const nlohmann::json& p : spec.value("params", nlohmann::json::array())) {
        if (p.value("shownAsInput", false)) shown.push_back(p.value("name", std::string()));
    }
    if (!shown.empty()) node["paramInputs"] = std::move(shown);
    impl_->nodes().push_back(std::move(node));
    return chosen;
}

void GraphEdit::completeParams() {
    for (nlohmann::json& n : impl_->nodes()) completeNode(n);
}

int GraphEdit::nodeVersion(const std::string& name) const {
    const nlohmann::json& node = impl_->node(name);
    return storedVersion(node, operatorSpec(node.value("type", std::string())));
}

bool GraphEdit::upgradeNode(const std::string& name) {
    impl_->node(name);
    for (nlohmann::json& n : impl_->nodes()) {
        if (!n.is_object() || n.value("name", std::string()) != name) continue;
        const nlohmann::json& spec = operatorSpec(n.value("type", std::string()));
        const int newest = spec.value("version", 1);
        const int stored = storedVersion(n, spec);
        if (stored >= newest) return false;
        // A param whose default changed since, still at the former one, takes the new default (as a
        // new node has it); one set to anything else stays.
        if (n.contains("params") && n["params"].is_object()) {
            nlohmann::json& params = n["params"];
            for (const nlohmann::json& p : spec.value("params", nlohmann::json::array())) {
                const nlohmann::json former = p.value("formerDefault", nlohmann::json());
                const std::string name = p.value("name", std::string());
                if (!former.is_object() || stored >= former.value("since", 0) || !params.contains(name)) continue;
                if (params[name] == former.at("value")) params[name] = p.value("default", nlohmann::json());
            }
        }
        n["version"] = newest;
        completeNode(n);  // params the newer version added
        return true;
    }
    return false;
}

bool GraphEdit::updateLibraryNode(const std::string& name, std::optional<double> groomSize, const std::string& asId) {
    impl_->node(name);
    for (nlohmann::json& n : impl_->nodes()) {
        if (!n.is_object() || n.value("name", std::string()) != name) continue;
        const nlohmann::json old = definitionOf(n);
        const std::string type = n.value("type", std::string());
        const std::string own = (type == "Compound" || type == "Script") && old.contains("glnode") ? old.value("id", std::string()) : "";
        if (type != "Compound" && type != "Script") throw GraphError(name + " is not a Compound or a Script node");
        const std::string id = asId.empty() ? own : asId;
        if (id.empty()) throw GraphError(name + " is not a library node");
        const std::optional<LibraryNode> library = findLibraryNode(id);
        if (!library) throw GraphError("no library node " + quoted(id) + " in the node folders");
        // Its inner params all written, as addNode writes them (completeNode).
        const std::string made = library->kind == "script" ? "Script" : "Compound";
        nlohmann::json fresh = {{"type", made},
                                {"params", {{"definition", nlohmann::json::parse(libraryNodeParamsJson(*library, groomSize))["definition"]}}}};
        completeNode(fresh);
        const nlohmann::json definition = fresh["params"]["definition"];
        if (definition == old && made == type) return false;
        n["type"] = made;
        // The exposed params it still has keep their values; the others go (a script node's
        // params by name).
        auto paramNames = [](const nlohmann::json& d) {
            if (!d.contains("script")) return d.value("expose", nlohmann::json::object());
            nlohmann::json names = nlohmann::json::object();
            for (const nlohmann::json& p : d.value("params", nlohmann::json::array())) {
                if (p.is_object() && p.value("name", nlohmann::json()).is_string()) names[p["name"].get<std::string>()] = true;
            }
            return names;
        };
        const nlohmann::json oldExpose = paramNames(old);
        const nlohmann::json newExpose = paramNames(definition);
        nlohmann::json& params = n["params"];
        for (const auto& [exposed, target] : oldExpose.items()) {
            if (!newExpose.contains(exposed)) params.erase(exposed);
        }
        params["definition"] = definition;
        if (n.contains("paramInputs") && n["paramInputs"].is_array()) {
            nlohmann::json kept = nlohmann::json::array();
            for (const nlohmann::json& p : n["paramInputs"]) {
                if (p.is_string() && newExpose.contains(p.get<std::string>())) kept.push_back(p);
            }
            n["paramInputs"] = kept;
        }
        // Links into inputs it no longer has go.
        std::set<std::string> inputs;
        const nlohmann::json spec = specOfNode(n);
        for (const nlohmann::json& port : spec["inputs"]) inputs.insert(port.value("name", std::string()));
        nlohmann::json links = nlohmann::json::array();
        for (const nlohmann::json& c : impl_->connectionsView()) {
            if (c.value("to", std::string()) != name || inputs.count(c.value("input", std::string()))) links.push_back(c);
        }
        impl_->connections() = std::move(links);
        return true;
    }
    return false;
}

void GraphEdit::removeNode(const std::string& name) {
    impl_->node(name);
    nlohmann::json kept = nlohmann::json::array();
    for (const nlohmann::json& n : impl_->nodesView()) {
        if (!(n.is_object() && n.value("name", std::string()) == name)) kept.push_back(n);
    }
    impl_->nodes() = std::move(kept);
    nlohmann::json links = nlohmann::json::array();
    for (const nlohmann::json& c : impl_->connectionsView()) {
        if (c.value("from", std::string()) != name && c.value("to", std::string()) != name) links.push_back(c);
    }
    impl_->connections() = std::move(links);
    if (impl_->graph.value("output", std::string()) == name) impl_->graph.erase("output");
}

void GraphEdit::renameNode(const std::string& name, const std::string& newName) {
    impl_->node(name);
    if (newName.empty()) throw GraphError("a node needs a name");
    if (newName == name) return;
    if (hasNode(newName)) throw GraphError("a node named '" + newName + "' exists");
    for (nlohmann::json& n : impl_->nodes()) {
        if (n.is_object() && n.value("name", std::string()) == name) n["name"] = newName;
    }
    for (nlohmann::json& c : impl_->connections()) {
        if (c.value("from", std::string()) == name) c["from"] = newName;
        if (c.value("to", std::string()) == name) c["to"] = newName;
    }
    if (impl_->graph.value("output", std::string()) == name) impl_->graph["output"] = newName;
    // Inside a compound (compoundGraphJson): its exposed params naming the node follow it.
    if (impl_->graph.contains("exposed") && impl_->graph["exposed"].is_object()) {
        for (auto& item : impl_->graph["exposed"].items()) {
            nlohmann::json& target = item.value();
            if (target.is_string() && target.get<std::string>().rfind(name + ".", 0) == 0) target = newName + target.get<std::string>().substr(name.size());
        }
    }
    recordMove(name, newName);
}

const std::vector<std::pair<std::string, std::string>>& GraphEdit::movedNodes() const { return impl_->moved; }

void GraphEdit::recordMove(const std::string& from, const std::string& to) {
    if (from != to) impl_->moved.emplace_back(from, to);
}

std::string GraphEdit::outputNode() const { return impl_->graph.value("output", std::string()); }

void GraphEdit::setOutputNode(const std::string& name) {
    impl_->node(name);
    impl_->graph["output"] = name;
}

void GraphEdit::clearPositions() {
    for (nlohmann::json& n : impl_->nodes()) {
        if (n.is_object()) n.erase("ui");
    }
}

void GraphEdit::connect(const std::string& src, const std::string& dst, const std::string& input, const std::string& output) {
    impl_->connect(src, dst, input, output);
}

bool GraphEdit::connectByHand(const std::string& src, const std::string& dst, const std::string& input,
                              const std::string& output) {
    const std::string kind = valueKindForInput(src, dst, input);
    if (!kind.empty()) {
        json* found = nullptr;
        for (json& n : impl_->nodes()) {
            if (n.is_object() && n.value("name", std::string()) == src) found = &n;
        }
        json& value = *found;  // (valueKindForInput found it)
        const json before = value;
        json& params = value["params"];
        if (!params.is_object()) params = json::object();
        const bool changed = params.value("kind", std::string("number")) != kind;
        params["kind"] = kind;
        try {
            impl_->connect(src, dst, input, output);
        } catch (...) {
            value = before;  // (the graph as it was: the link refused)
            throw;
        }
        return changed;
    }
    impl_->connect(src, dst, input, output);
    if (input == "strands" && nodeType(dst) == op::kClump) return adoptUpstreamClump(dst);
    // A Clump picking its own centers takes the centers linked to it (its centersFrom: inputPoints;
    // guides, through any curve nodes after them, inputCurves: each clump along its guide); a Cards
    // node following the strands, the guides linked to it (its cardsFrom: guides). Guides are a
    // Guides node's, an ImportCurves', or as a GuideCache or Simulate node moves them (a simulated
    // guide is a clump's curve as much as a groomed one).
    auto fromGuidesNode = [&] {
        std::string at = src;
        std::set<std::string> seen;
        while (!at.empty() && seen.insert(at).second) {
            const std::string type = nodeType(at);
            if (type == op::kGuides || type == "GuideCache" || type == op::kSimulate || type == "ImportCurves") return true;
            std::string next;
            const json spec = impl_->spec(at);
            for (const json& port : spec["inputs"]) {
                const std::string portType = port.value("type", std::string());
                if (portType == "Curves" || portType == "Guides") {
                    next = impl_->sourceOf(at, port.value("name", std::string()));
                    break;
                }
            }
            at = next;
        }
        return false;
    };
    // A Clump linked to a Clump's centers (issue #115): the smaller clumps inside its clumps
    // (upstreamClump), reading the clump numbers it writes; this one's own written under another
    // name when it would take that one's (subClumpId).
    if (input == "centers" && nodeType(dst) == op::kClump && nodeType(src) == op::kClump) {
        std::string upChannel = "clumpId";
        for (const json& n : impl_->nodes()) {
            if (n.is_object() && n.value("name", std::string()) == src && n.contains("params") && n["params"].is_object()) {
                const auto it = n["params"].find("clumpChannel");
                if (it != n["params"].end() && it->is_string()) upChannel = it->get<std::string>();
            }
        }
        for (json& n : impl_->nodes()) {
            if (!n.is_object() || n.value("name", std::string()) != dst) continue;
            json& params = n["params"];
            if (!params.is_object()) params = json::object();
            const auto it = params.find("centersFrom");
            if (it != params.end() && !(it->is_string() && it->get<std::string>() == "pick")) return false;
            params["centersFrom"] = "upstreamClump";
            params["upstreamChannel"] = upChannel;
            const auto own = params.find("clumpChannel");
            const std::string ownChannel = own != params.end() && own->is_string() ? own->get<std::string>() : std::string("clumpId");
            if (!upChannel.empty() && ownChannel == upChannel) params["clumpChannel"] = "subClumpId";
            return true;
        }
        return false;
    }
    // Curves (not a Clump) linked to the centers of a Clump clumping inside upstream clumps: those
    // curves shape the clumps within each upstream clump (upstreamClumpCurves).
    if (input == "centers" && nodeType(dst) == op::kClump) {
        for (json& n : impl_->nodes()) {
            if (!n.is_object() || n.value("name", std::string()) != dst || !n.contains("params") || !n["params"].is_object()) continue;
            json& params = n["params"];
            const auto it = params.find("centersFrom");
            if (it != params.end() && it->is_string() && it->get<std::string>() == "upstreamClump") {
                params["centersFrom"] = "upstreamClumpCurves";
                return true;
            }
        }
    }
    struct Switch {
        const char *type, *input, *param, *from, *to, *toForGuides;
    };
    static const Switch kSwitches[] = {{op::kClump, "centers", "centersFrom", "pick", "inputPoints", "inputCurves"},
                                       {op::kCards, "guides", "cardsFrom", "strands", "guides", "guides"}};
    for (const Switch& sw : kSwitches) {
        if (input != sw.input) continue;
        for (json& n : impl_->nodes()) {
            if (!n.is_object() || n.value("name", std::string()) != dst) continue;
            if (n.value("type", std::string()) != sw.type) return false;
            json& params = n["params"];
            if (!params.is_object()) params = json::object();
            const auto it = params.find(sw.param);
            if (it != params.end() && !(it->is_string() && it->get<std::string>() == sw.from)) return false;
            params[sw.param] = fromGuidesNode() ? sw.toForGuides : sw.to;
            return true;
        }
    }
    return false;
}

bool GraphEdit::adoptUpstreamClump(const std::string& node) {
    if (nodeType(node) != op::kClump || !impl_->sourceOf(node, "centers").empty()) return false;
    json* dst = nullptr;
    for (json& n : impl_->nodes()) {
        if (n.is_object() && n.value("name", std::string()) == node) dst = &n;
    }
    if (!dst) return false;
    json& params = (*dst)["params"];
    if (!params.is_object()) params = json::object();
    const auto from = params.find("centersFrom");
    if (from != params.end() && !(from->is_string() && from->get<std::string>() == "pick")) return false;
    // Up the chain along each node's first input (the strands it passes on): the nearest Clump.
    std::string at = impl_->sourceOf(node, "strands");
    std::set<std::string> seen{node};
    while (!at.empty() && seen.insert(at).second) {
        if (nodeType(at) == op::kClump || nodeType(at) == op::kBraid) break;
        const json spec = impl_->spec(at);
        const json inputs = spec.is_object() ? spec.value("inputs", json::array()) : json::array();
        at = inputs.empty() ? std::string() : impl_->sourceOf(at, inputs[0].value("name", std::string()));
    }
    if (at.empty() || (nodeType(at) != op::kClump && nodeType(at) != op::kBraid)) return false;
    if (nodeType(at) == op::kBraid) {
        // After a Braid: clumps along each section of each braid (its braidSection, with braidId).
        params["centersFrom"] = "upstreamClump";
        params["upstreamChannel"] = "braidSection";
        return true;
    }
    std::string upChannel = "clumpId";
    for (const json& n : impl_->nodes()) {
        if (n.is_object() && n.value("name", std::string()) == at && n.contains("params") && n["params"].is_object()) {
            const auto it = n["params"].find("clumpChannel");
            if (it != n["params"].end() && it->is_string() && !it->get<std::string>().empty()) upChannel = it->get<std::string>();
        }
    }
    params["centersFrom"] = "upstreamClump";
    params["upstreamChannel"] = upChannel;
    const auto own = params.find("clumpChannel");
    const std::string ownChannel = own != params.end() && own->is_string() ? own->get<std::string>() : std::string("clumpId");
    if (ownChannel == upChannel) params["clumpChannel"] = "subClumpId";
    return true;
}

bool GraphEdit::disconnect(const std::string& dst, const std::string& input) {
    bool found = false;
    impl_->disconnect(dst, input, &found);
    return found;
}

GraphEdit::Detached GraphEdit::detach(const std::string& name, double drop) {
    impl_->node(name);
    Detached out;
    const json spec = impl_->spec(name);
    const json links = impl_->connectionsView();
    // The flow in: the first connected input, as the node lists them.
    std::string sourceOutput;
    for (const json& port : spec["inputs"]) {
        const std::string input = port.value("name", std::string());
        for (const json& c : links) {
            if (c.value("to", std::string()) == name && c.value("input", std::string()) == input) {
                out.source = c.value("from", std::string());
                sourceOutput = c.value("output", std::string());
                break;
            }
        }
        if (!out.source.empty()) break;
    }
    // Every link of the node goes; what it fed is fed by the source where that fits.
    std::vector<json> fed;
    json kept = json::array();
    for (const json& c : links) {
        const bool from = c.value("from", std::string()) == name, to = c.value("to", std::string()) == name;
        if (from && !to) fed.push_back(c);
        if (from || to) {
            out.changed = true;
        } else {
            kept.push_back(c);
        }
    }
    if (!out.changed) return out;
    // A link reading one of its pass-through outputs reads what fed that input (as it did).
    std::map<std::string, std::pair<std::string, std::string>> passed;  // passed input -> its source
    for (const json& c : links) {
        if (c.value("to", std::string()) == name) passed[c.value("input", std::string())] = {c.value("from", std::string()), c.value("output", std::string())};
    }
    impl_->connections() = std::move(kept);
    for (const json& c : fed) {
        const std::string dst = c.value("to", std::string()), input = c.value("input", std::string());
        const std::string thru = passThroughInput(c.value("output", std::string()));
        std::pair<std::string, std::string> from{out.source, sourceOutput};
        if (!thru.empty()) {
            const auto it = passed.find(thru);
            if (it == passed.end()) continue;
            from = it->second;
        }
        if (from.first.empty()) continue;
        try {
            impl_->connect(from.first, dst, input, from.second);
            out.relinked.emplace_back(dst, input);
        } catch (const GraphError&) {
            // (a source that does not fit that input: it is left unconnected)
        }
    }
    if (!out.source.empty() && impl_->graph.value("output", std::string()) == name) impl_->graph["output"] = out.source;
    // Below the new link: midway between the source and the first node it feeds now (else what
    // the node fed first, else just below the source).
    auto position = [&](const std::string& node, double* x, double* y) {
        const json* n = impl_->find(node);
        if (!n) return false;
        const auto ui = n->find("ui");
        if (ui == n->end() || !ui->is_object() || !ui->contains("x") || !ui->contains("y")) return false;
        *x = (*ui)["x"].get<double>();
        *y = (*ui)["y"].get<double>();
        return true;
    };
    const std::string after = !out.relinked.empty() ? out.relinked.front().first
                              : !fed.empty()        ? fed.front().value("to", std::string())
                                                    : std::string();
    double sx = 0.0, sy = 0.0, dx = 0.0, dy = 0.0;
    const bool hasSource = !out.source.empty() && position(out.source, &sx, &sy);
    const bool hasAfter = !after.empty() && position(after, &dx, &dy);
    if (hasSource || hasAfter) {
        const double x = hasSource && hasAfter ? 0.5 * (sx + dx) : hasSource ? sx : dx;
        const double y = (hasSource && hasAfter ? std::max(sy, dy) : hasSource ? sy : dy) + drop;
        for (json& n : impl_->nodes()) {
            if (n.is_object() && n.value("name", std::string()) == name) n["ui"] = {{"x", x}, {"y", y}};
        }
    }
    return out;
}

GraphEdit::Detached GraphEdit::detachGroup(const std::vector<std::string>& names) {
    Detached out;
    std::set<std::string> group;
    for (const std::string& n : names) {
        if (hasNode(n)) group.insert(n);
    }
    if (group.empty()) return out;
    const json links = impl_->connectionsView();
    auto inside = [&](const std::string& n) { return group.count(n) > 0; };
    // What feeds a member's first connected input (its spec's order): (node, output).
    auto firstFeed = [&](const std::string& node, std::pair<std::string, std::string>* feed) {
        const json spec = impl_->spec(node);
        for (const json& port : spec["inputs"]) {
            for (const json& c : links) {
                if (c.value("to", std::string()) == node && c.value("input", std::string()) == port.value("name", std::string())) {
                    *feed = {c.value("from", std::string()), c.value("output", std::string())};
                    return true;
                }
            }
        }
        return false;
    };
    // The source outside the group a member's flow comes from (through members' first inputs).
    auto sourceOf = [&](std::string node) {
        std::pair<std::string, std::string> feed;
        for (std::size_t guard = 0; guard <= group.size(); ++guard) {
            if (!firstFeed(node, &feed)) return std::pair<std::string, std::string>();
            if (!inside(feed.first)) return feed;
            node = feed.first;
        }
        return std::pair<std::string, std::string>();
    };
    std::vector<json> leaving;
    json kept = json::array();
    for (const json& c : links) {
        const bool from = inside(c.value("from", std::string())), to = inside(c.value("to", std::string()));
        if (from != to) {
            out.changed = true;
            if (from) leaving.push_back(c);
        } else {
            kept.push_back(c);
        }
    }
    if (!out.changed) return out;
    std::map<std::string, std::pair<std::string, std::string>> sources;
    for (const json& c : leaving) {
        const std::string m = c.value("from", std::string());
        if (!sources.count(m)) sources[m] = sourceOf(m);
    }
    impl_->connections() = std::move(kept);
    for (const json& c : leaving) {
        const auto& [src, output] = sources[c.value("from", std::string())];
        if (src.empty()) continue;
        const std::string dst = c.value("to", std::string()), input = c.value("input", std::string());
        try {
            impl_->connect(src, dst, input, output);
            out.relinked.emplace_back(dst, input);
            if (out.source.empty()) out.source = src;
        } catch (const GraphError&) {
            // (a source that does not fit that input: it is left unconnected)
        }
    }
    const std::string graphOutput = impl_->graph.value("output", std::string());
    if (inside(graphOutput)) {
        const auto s = sourceOf(graphOutput);
        if (!s.first.empty()) impl_->graph["output"] = s.first;
    }
    return out;
}

GraphEdit::Detached GraphEdit::removeNodeKeepingFlow(const std::string& name) {
    Detached out = detach(name);
    removeNode(name);
    out.changed = true;
    return out;
}

namespace {

constexpr const char* kClipboardKey = "groomlabNodes";

}  // namespace

std::string GraphEdit::copyNodes(const std::vector<std::string>& names) const {
    nlohmann::json data = {{kClipboardKey, 1}, {"nodes", nlohmann::json::array()}, {"connections", nlohmann::json::array()}};
    std::set<std::string> copied;
    for (const nlohmann::json& n : impl_->nodesView()) {
        if (!n.is_object() || n.value("type", std::string()) == "Output") continue;  // the output node is one of a kind
        const std::string name = n.value("name", std::string());
        if (std::find(names.begin(), names.end(), name) == names.end()) continue;
        data["nodes"].push_back(n);
        copied.insert(name);
    }
    if (copied.empty()) return {};
    for (const nlohmann::json& c : impl_->connectionsView()) {
        if (c.is_object() && copied.count(c.value("from", std::string())) && copied.count(c.value("to", std::string()))) {
            data["connections"].push_back(c);
        }
    }
    return data.dump();
}

std::vector<std::pair<std::string, std::string>> GraphEdit::pasteNodes(const std::string& clipboard, double dx, double dy) {
    const nlohmann::json data = nlohmann::json::parse(clipboard, nullptr, false);
    const char* const none = "the clipboard holds no GroomLab nodes";
    if (!data.is_object() || !data.contains(kClipboardKey) || !data.contains("nodes") || !data["nodes"].is_array()) throw GraphError(none);
    std::vector<const nlohmann::json*> nodes;
    for (const nlohmann::json& n : data["nodes"]) {
        const std::string type = n.is_object() ? n.value("type", std::string()) : std::string();
        if (type == "Output") continue;  // (a clipboard from before copyNodes left it out)
        try {
            operatorSpec(type);
        } catch (const GraphError&) {
            throw GraphError("the clipboard names an unknown node type");
        }
        nodes.push_back(&n);
    }
    if (nodes.empty()) throw GraphError(none);
    GraphEdit pasted(*this);  // a link that does not fit leaves this graph as it was
    std::map<std::string, std::string> renamed;  // copied name -> pasted name
    std::vector<std::pair<std::string, std::string>> made;
    for (const nlohmann::json* n : nodes) {
        const std::string from = n->value("name", std::string());
        const std::string name = pasted.uniqueName(from.empty() ? n->value("type", std::string()) : from);
        nlohmann::json node = *n;
        node["name"] = name;
        const nlohmann::json ui = n->value("ui", nlohmann::json::object());
        auto at = [&](const char* axis) { return ui.is_object() && ui.contains(axis) && ui[axis].is_number() ? ui[axis].get<double>() : 0.0; };
        node["ui"] = {{"x", at("x") + dx}, {"y", at("y") + dy}};
        pasted.impl_->nodes().push_back(std::move(node));
        renamed[from] = name;
        made.emplace_back(name, from);
    }
    const nlohmann::json noLinks = nlohmann::json::array();
    const nlohmann::json& links = data.contains("connections") && data["connections"].is_array() ? data["connections"] : noLinks;
    for (const nlohmann::json& c : links) {
        if (!c.is_object()) continue;
        const auto a = renamed.find(c.value("from", std::string())), b = renamed.find(c.value("to", std::string()));
        if (a == renamed.end() || b == renamed.end()) continue;
        pasted.impl_->connect(a->second, b->second, c.value("input", std::string()), c.value("output", std::string()));
    }
    *impl_ = *pasted.impl_;
    return made;
}

std::vector<std::pair<std::string, std::string>> GraphEdit::duplicateNodes(const std::vector<std::string>& names, double dx, double dy) {
    const std::string clipboard = copyNodes(names);
    if (clipboard.empty()) throw GraphError("nothing to duplicate");
    GraphEdit work(*this);
    const std::vector<std::pair<std::string, std::string>> made = work.pasteNodes(clipboard, dx, dy);
    std::map<std::string, std::string> copyOf;  // node -> its copy
    for (const auto& [copy, node] : made) copyOf[node] = copy;
    // The links from outside the nodes into them, into the copies too.
    for (const nlohmann::json& c : impl_->connectionsView()) {
        const std::string from = c.value("from", std::string()), to = c.value("to", std::string());
        const auto copy = copyOf.find(to);
        if (copy == copyOf.end() || copyOf.count(from)) continue;
        const std::string input = c.value("input", std::string());
        work.impl_->connect(from, copy->second, input, c.value("output", std::string()));
        // Drawn as the node's link is: out of the same dot, through the same pins.
        if (const auto dot = portalOfLink(impl_->graph, to, input)) addPortalLink(work.impl_->graph, dot->portal, dot->exit, copy->second, input);
        const std::vector<std::string> via = pinRoute(impl_->graph, to, input);
        if (!via.empty()) setPinRoute(work.impl_->graph, copy->second, input, via);
    }
    *impl_ = *work.impl_;
    return made;
}

std::vector<std::pair<std::string, std::string>> GraphEdit::outputLinks(const std::string& src, const std::string& output) const {
    const std::vector<std::string> outputs = impl_->outputNames(src);
    const std::string first = outputs.empty() ? std::string() : outputs.front();
    const std::string wanted = output.empty() ? first : output;
    std::vector<std::pair<std::string, std::string>> out;
    for (const nlohmann::json& c : impl_->connectionsView()) {
        if (c.value("from", std::string()) != src) continue;
        const std::string reads = c.value("output", std::string());
        if ((reads.empty() ? first : reads) == wanted) out.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> GraphEdit::moveOutputLinks(const std::string& src, const std::string& output,
                                                                             const std::string& newSrc, const std::string& newOutput) {
    impl_->outputSpec(src, output);
    impl_->outputSpec(newSrc, newOutput);
    std::vector<std::pair<std::string, std::string>> moved;
    for (const auto& [dst, input] : outputLinks(src, output)) {
        GraphEdit attempt(*this);
        try {
            attempt.impl_->connect(newSrc, dst, input, newOutput);
        } catch (const GraphError&) {
            continue;  // (it does not fit there: the link stays as it was)
        }
        *impl_ = *attempt.impl_;
        moved.emplace_back(dst, input);
    }
    return moved;
}

std::string GraphEdit::passThroughInput(const std::string& output) {
    const std::string prefix = passThroughOutput({});
    return output.size() > prefix.size() && output.compare(0, prefix.size(), prefix) == 0 ? output.substr(prefix.size()) : std::string();
}

std::vector<std::string> GraphEdit::passThrough(const std::string& name) const {
    std::vector<std::string> out;
    const nlohmann::json& node = impl_->node(name);
    const auto it = node.find("passThrough");
    if (it == node.end() || !it->is_array()) return out;
    for (const nlohmann::json& p : *it) {
        if (p.is_string()) out.push_back(p.get<std::string>());
    }
    return out;
}

bool GraphEdit::setPassThrough(const std::string& name, const std::string& input, bool on) {
    impl_->inputSpec(name, input);  // (GraphError for no such input)
    std::vector<std::string> inputs = passThrough(name);
    const auto at = std::find(inputs.begin(), inputs.end(), input);
    if ((at != inputs.end()) == on) return false;
    if (on) {
        inputs.push_back(input);
    } else {
        inputs.erase(at);
        // What read it reads nothing now.
        const std::string output = passThroughOutput(input);
        nlohmann::json kept = nlohmann::json::array();
        for (const nlohmann::json& c : impl_->connectionsView()) {
            if (!(c.value("from", std::string()) == name && c.value("output", std::string()) == output)) kept.push_back(c);
        }
        impl_->connections() = std::move(kept);
    }
    for (nlohmann::json& n : impl_->nodes()) {
        if (!n.is_object() || n.value("name", std::string()) != name) continue;
        if (inputs.empty()) n.erase("passThrough");
        else n["passThrough"] = inputs;
        if (!on && n.contains("passThroughNames") && n["passThroughNames"].is_object()) {
            n["passThroughNames"].erase(input);
            if (n["passThroughNames"].empty()) n.erase("passThroughNames");
        }
    }
    return true;
}

bool GraphEdit::movePassThrough(const std::string& name, const std::string& input, int by) {
    std::vector<std::string> inputs = passThrough(name);
    const auto at = std::find(inputs.begin(), inputs.end(), input);
    if (at == inputs.end()) throw GraphError("node '" + name + "' does not pass '" + input + "' through");
    const int from = static_cast<int>(at - inputs.begin());
    const int to = std::clamp(from + by, 0, static_cast<int>(inputs.size()) - 1);
    if (to == from) return false;
    inputs.erase(inputs.begin() + from);
    inputs.insert(inputs.begin() + to, input);
    for (nlohmann::json& n : impl_->nodes()) {
        if (n.is_object() && n.value("name", std::string()) == name) n["passThrough"] = inputs;
    }
    return true;
}

std::string GraphEdit::passThroughName(const std::string& name, const std::string& input) const {
    const nlohmann::json& node = impl_->node(name);
    const auto it = node.find("passThroughNames");
    if (it == node.end() || !it->is_object() || !it->contains(input) || !(*it)[input].is_string()) return {};
    return (*it)[input].get<std::string>();
}

bool GraphEdit::setPassThroughName(const std::string& name, const std::string& input, const std::string& label) {
    const std::vector<std::string> inputs = passThrough(name);
    if (std::find(inputs.begin(), inputs.end(), input) == inputs.end()) {
        throw GraphError("node '" + name + "' does not pass '" + input + "' through");
    }
    if (passThroughName(name, input) == label) return false;
    for (nlohmann::json& n : impl_->nodes()) {
        if (!n.is_object() || n.value("name", std::string()) != name) continue;
        nlohmann::json names = n.contains("passThroughNames") && n["passThroughNames"].is_object() ? n["passThroughNames"] : nlohmann::json::object();
        if (label.empty()) names.erase(input);
        else names[input] = label;
        if (names.empty()) n.erase("passThroughNames");
        else n["passThroughNames"] = std::move(names);
    }
    return true;
}

void resolvePassThrough(nlohmann::json& graph) {
    if (!graph.is_object() || !graph.contains("connections") || !graph["connections"].is_array()) return;
    std::map<std::string, std::set<std::string>> passes;  // node -> the inputs it passes
    for (const nlohmann::json& n : graph.value("nodes", nlohmann::json::array())) {
        if (!n.is_object() || !n.contains("passThrough") || !n["passThrough"].is_array()) continue;
        for (const nlohmann::json& p : n["passThrough"]) {
            if (p.is_string()) passes[n.value("name", std::string())].insert(p.get<std::string>());
        }
    }
    if (passes.empty()) return;
    std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> feeds;  // (to, input) -> (from, output)
    for (const nlohmann::json& c : graph["connections"]) {
        if (c.is_object()) feeds[{c.value("to", std::string()), c.value("input", std::string())}] = {c.value("from", std::string()), c.value("output", std::string())};
    }
    nlohmann::json resolved = nlohmann::json::array();
    for (nlohmann::json c : graph["connections"]) {
        if (!c.is_object()) continue;
        std::string from = c.value("from", std::string()), output = c.value("output", std::string());
        bool lost = false;
        for (int guard = 0; guard < 256; ++guard) {
            const std::string input = GraphEdit::passThroughInput(output);
            const auto node = passes.find(from);
            if (input.empty() || node == passes.end() || !node->second.count(input)) break;
            const auto source = feeds.find({from, input});
            if (source == feeds.end()) {
                lost = true;  // nothing comes in: nothing goes out
                break;
            }
            from = source->second.first;
            output = source->second.second;
        }
        if (lost) continue;
        c["from"] = from;
        if (output.empty()) c.erase("output");
        else c["output"] = output;
        resolved.push_back(std::move(c));
    }
    graph["connections"] = std::move(resolved);
}

std::string resolvePassThroughJson(const std::string& graphJson) {
    nlohmann::json graph = nlohmann::json::parse(graphJson, nullptr, false);
    if (!graph.is_object()) throw GraphError("the graph is not a JSON object");
    resolvePassThrough(graph);
    return graph.dump();
}

void GraphEdit::checkConnections() {
    const nlohmann::json links = impl_->connectionsView();
    impl_->connections() = nlohmann::json::array();
    for (const nlohmann::json& c : links) {
        impl_->connect(c.value("from", std::string()), c.value("to", std::string()), c.value("input", std::string()),
                       c.value("output", std::string()));
    }
}

std::string GraphEdit::nodeSpecJson(const std::string& name) const { return impl_->spec(name).dump(); }

std::string GraphEdit::outputType(const std::string& name, const std::string& output) const {
    std::set<std::string> seen;
    return impl_->outputType(name, output, seen);
}

std::vector<std::string> GraphEdit::outputTypes(const std::string& name) const {
    std::vector<std::string> out;
    for (const std::string& output : impl_->outputNames(name)) out.push_back(outputType(name, output));
    return out;
}

bool GraphEdit::viewable(const std::string& name) const {
    if (!impl_->find(name)) return false;
    try {
        for (const std::string& type : outputTypes(name)) {
            if (type == "Strands" || type == "Curves") return true;
        }
    } catch (const std::exception&) {
        // (a node no operator makes: nothing to view)
    }
    return false;
}

bool GraphEdit::bakeable(const std::string& name) const {
    const json* n = impl_->find(name);
    if (!n || n->value("type", std::string()) == op::kOutput) return false;
    try {
        const std::string type = outputType(name, "");
        return type == "Strands" || type == "Guides";
    } catch (const std::exception&) {
        return false;
    }
}

std::string GraphEdit::valueKindForInput(const std::string& src, const std::string& dst, const std::string& input) const {
    const json* value = impl_->find(src);
    if (!value || value->value("type", std::string()) != op::kValue || !impl_->find(dst)) return {};
    for (const json& c : impl_->connectionsView()) {
        if (c.value("from", std::string()) == src) return {};  // (it feeds a param already: its kind kept)
    }
    json spec;
    try {
        spec = impl_->spec(dst);
    } catch (const std::exception&) {
        return {};
    }
    for (const json& p : spec.value("params", json::array())) {
        if (!p.is_object() || p.value("name", std::string()) != input) continue;
        const std::string kind = p.value("kind", std::string());
        if (kind == "float") return "number";
        if (kind == "int") return "integer";
        if (kind == "bool") return "bool";
        if (kind == "vec3") return p.value("unit", std::string()) == "color" ? "color" : "vector";
        if (kind == "string") return "text";
        return {};
    }
    return {};
}

std::vector<std::string> GraphEdit::upstream(const std::string& name) const {
    impl_->node(name);
    const std::set<std::string> found = impl_->upstream(name);
    return {found.begin(), found.end()};
}

std::string GraphEdit::clumpChannel(const std::string& node) const {
    const nlohmann::json* n = impl_->find(node);
    if (!n || n->value("type", std::string()) != "Clump") return {};
    const auto params = n->find("params");
    if (params == n->end() || !params->is_object()) return "clumpId";
    const auto it = params->find("clumpChannel");
    if (it == params->end()) return "clumpId";
    return it->is_string() ? it->get<std::string>() : std::string();
}

std::string GraphEdit::clumpColorSource(const std::string& node) const {
    const std::string channel = clumpChannel(node);
    return channel.empty() ? std::string() : channel + "@" + node;
}

bool GraphEdit::clumpColored(const std::string& node, const std::string& source) const {
    const std::string channel = clumpChannel(node);
    if (channel.empty() || source.empty()) return false;
    if (source == channel + "@" + node) return true;
    if (source != channel) return false;
    // A bare channel: the last Clump writing it is the one shown.
    std::vector<std::string> writers;
    for (const nlohmann::json& n : impl_->nodesView()) {
        const std::string name = n.is_object() ? n.value("name", std::string()) : std::string();
        if (!name.empty() && clumpChannel(name) == channel) writers.push_back(name);
    }
    const std::set<std::string> feeding = impl_->upstream(node);
    for (const std::string& other : writers) {
        if (other != node && !feeding.count(other)) return false;
    }
    return true;
}

bool GraphEdit::braidTubesOffered(const std::string& node) const {
    const nlohmann::json* n = impl_->find(node);
    if (!n) return false;
    const std::string type = n->value("type", std::string());
    if (type == "Braid") return true;
    // A node giving guides (a Guides node, a Curl on them, a Sculpt): the tubes they carry. Not a
    // Clump (fed guides): its header's own button is Clump Colors.
    if (type == "Clump") return false;
    try {
        return outputType(node, "") == "Guides";
    } catch (const std::exception&) {
        return false;
    }
}

bool GraphEdit::braidTubesShown(const std::string& node, const std::string& shown) const {
    if (!braidTubesOffered(node)) return false;
    const std::vector<std::string> names = braidTubeNames(shown);
    return std::find(names.begin(), names.end(), node) != names.end();
}

std::string GraphEdit::braidTubesToggled(const std::string& node, const std::string& shown) const {
    if (!braidTubesOffered(node)) return shown;
    std::vector<std::string> names = braidTubeNames(shown);
    const auto at = std::find(names.begin(), names.end(), node);
    if (at != names.end()) names.erase(at);
    else names.push_back(node);
    return braidTubesText(names);
}

std::string GraphEdit::braidTubesKept(const std::string& shown) const {
    std::vector<std::string> names = braidTubeNames(shown);
    names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& name) { return !braidTubesOffered(name); }),
                names.end());
    return braidTubesText(names);
}

std::string GraphEdit::channelsJson(const std::vector<std::string>& guideMaps) const {
    return graphChannelsJson(impl_->graph.dump(), guideMaps);
}

std::vector<std::string> GraphEdit::seedWarnings() const {
    // Operators whose nodes of one type should not share a seed, and why (sorted by type).
    static const std::map<std::string, std::string> sharing = {
        {"Flyaway", "they pick the same strays"},
        {"Instance", "they pick the same strands"},
        {"Scales", "their scales vary alike"},
        {"Scatter", "their strands coincide (same roots and IDs)"},
    };
    std::vector<std::string> found;
    for (const auto& [type, why] : sharing) {
        std::map<std::int64_t, std::vector<std::string>> bySeed;
        for (const auto& [name, seed] : impl_->seedsOf(type)) bySeed[seed].push_back(name);
        for (const auto& [seed, names] : bySeed) {
            if (names.size() < 2) continue;
            std::string joined;
            for (const std::string& n : names) joined += (joined.empty() ? "" : " and ") + n;
            found.push_back(joined + " share seed " + std::to_string(seed) + ": " + why + "; give each its own seed");
        }
    }
    return found;
}

std::vector<std::string> GraphEdit::feedingOutput() const {
    const std::string output = outputNode();
    if (output.empty() || !impl_->find(output)) return {};
    std::set<std::string> feeding = impl_->upstream(output);
    feeding.insert(output);
    std::vector<std::string> out;
    for (const std::string& name : nodeNames()) {
        if (feeding.count(name)) out.push_back(name);
    }
    return out;
}

namespace {

// A port's type as a problem names it: a Value's with its kind ("number Value").
std::string problemLabel(const std::string& type, const json& port) {
    const std::string kind = port.value("valueKind", std::string());
    return type == "Value" && !kind.empty() && kind != "any" ? kind + " Value" : kindLabel(type);
}

// Whether an output (its type as connected, its port) can feed an input: the types (typesMatch),
// and for a Value its kind (a Field takes a number unless its port says otherwise).
bool portsFit(const std::string& outType, const json& out, const json& in) {
    const std::string inType = in.value("type", std::string());
    if (!typesMatch(outType, inType)) return false;
    if (outType != "Value") return true;
    try {
        return valueKindsMatch(valueKindFromName(out.value("valueKind", std::string("any"))),
                               valueKindFromName(in.value("valueKind", std::string(inType == "Field" ? "number" : "any"))));
    } catch (const GraphError&) {  // a name no kind has
        return outType == inType;
    }
}

// Whether a param's value is unset as Python reads one (None, "", 0, False, nothing in it).
bool unset(const json& value) {
    if (value.is_null()) return true;
    if (value.is_string()) return value.get_ref<const std::string&>().empty();
    if (value.is_boolean()) return !value.get<bool>();
    if (value.is_number()) return value.get<double>() == 0.0;
    return value.empty();
}

}  // namespace

std::vector<std::string> GraphEdit::problems(bool feedingOutput) const {
    // A map param's sources and what a problem calls them (the editors' map kinds).
    static const std::pair<const char*, const char*> mapSources[] = {
        {"texture", "texture"}, {"colorSet", "color set"}, {"attribute", "attribute"}, {"expression", "expression"}, {"guide", "painted map"}};
    std::vector<std::string> found;
    const std::string output = outputNode();
    if (feedingOutput && output.empty()) found.push_back("no output node");
    else if (feedingOutput && !impl_->find(output)) found.push_back("output node '" + output + "' does not exist");
    const std::vector<std::string> made = this->feedingOutput();
    const std::set<std::string> feeding(made.begin(), made.end());
    for (const json& n : impl_->nodesView()) {
        const std::string name = n.is_object() ? n.value("name", std::string()) : std::string();
        if (name.empty() || (feeding.count(name) > 0) != feedingOutput) continue;
        try {
            const json spec = specOfNode(n);
            for (const json& port : spec["inputs"]) {
                const std::string input = port.value("name", std::string());
                std::string sourceOutput;
                const std::string source = impl_->sourceOf(name, input, &sourceOutput);
                if (source.empty()) {
                    if (!port.value("optional", false)) found.push_back(name + "." + input + " is not connected");
                    continue;
                }
                const std::vector<std::string> outputs = impl_->outputNames(source);
                if (sourceOutput.empty() && !outputs.empty()) sourceOutput = outputs.front();
                if (std::find(outputs.begin(), outputs.end(), sourceOutput) == outputs.end()) {  // e.g. a Compound's definition changed
                    found.push_back(source + " has no output '" + sourceOutput + "' (" + name + "." + input + " reads it)");
                    continue;
                }
                // e.g. a Noise now fed guides, still linked to a strands-only input; a Value node set
                // to give a vector, still linked to a count
                const json out = impl_->outputSpec(source, sourceOutput);
                std::string outType = out.value("type", std::string());
                try {
                    std::set<std::string> seen;
                    outType = impl_->outputType(source, sourceOutput, seen);
                } catch (const GraphError&) {  // a link further up reads an output its source lacks: said there, once
                }
                if (!portsFit(outType, out, port)) {
                    found.push_back(source + " gives " + problemLabel(outType, out) + " but " + name + "." + input + " takes " +
                                    problemLabel(port.value("type", std::string()), port));
                }
            }
        } catch (const GraphError& e) {  // a node or a source the graph cannot read (an unknown type, a link from no node)
            found.push_back(name + ": " + e.what());
        }
        const auto params = n.find("params");
        if (params == n.end() || !params->is_object()) continue;
        for (const auto& [param, value] : params->items()) {
            if (!value.is_object()) continue;
            for (const auto& [key, label] : mapSources) {
                if (!value.contains(key)) continue;
                if (unset(value[key])) found.push_back(name + "." + param + ": the " + label + " has no name");
                break;
            }
        }
    }
    return found;
}

std::string completeGraphJson(const std::string& graphJson) {
    GraphEdit edit(graphJson);
    edit.completeParams();
    return edit.toJson();
}

std::string nodeSpecJson(const std::string& nodeJson) {
    const nlohmann::json node = nlohmann::json::parse(nodeJson, nullptr, false);
    if (node.is_discarded() || !node.is_object()) throw GraphError("a node is a JSON object");
    return specOfNode(node).dump();
}

}  // namespace groomlab
