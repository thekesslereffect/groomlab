// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_edit.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/attributes.h"
#include "groomlab/graph_io.h"
#include "groomlab/node_library.h"
#include "groomlab/operators.h"
#include "groomlab/presets.h"
#include "operators/internal.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

constexpr double kLayoutColumn = 240.0;  // a new node's column right of the graph (the Graph Editor's LAYOUT_DX)

std::string quoted(const std::string& s) { return "'" + s + "'"; }

// Every registered operator's spec (operatorSpecsJson), by type: read once (again when an operator
// was registered since: a test's).
const std::map<std::string, json>& catalog() {
    static std::map<std::string, json> specs;
    static std::size_t registered = 0;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const std::size_t now = OperatorRegistry::instance().typeNames().size();
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
                    // A library node's header says how it shows (node_library.h).
                    if (const auto own = shown.find(exposed); own != shown.end()) {
                        const json& h = own->second;
                        for (const char* key : {"label", "description"}) {
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
        const std::string out = outputSpec(name, output).value("type", std::string());
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
    impl_->connect(src, dst, input, output);
    // A Clump picking its own centers takes the centers linked to it (its centersFrom: inputPoints).
    if (input != "centers") return false;
    for (json& n : impl_->nodes()) {
        if (!n.is_object() || n.value("name", std::string()) != dst) continue;
        if (n.value("type", std::string()) != op::kClump) return false;
        json& params = n["params"];
        if (!params.is_object()) params = json::object();
        const auto it = params.find("centersFrom");
        if (it != params.end() && !(it->is_string() && it->get<std::string>() == "pick")) return false;
        params["centersFrom"] = "inputPoints";
        return true;
    }
    return false;
}

bool GraphEdit::disconnect(const std::string& dst, const std::string& input) {
    bool found = false;
    impl_->disconnect(dst, input, &found);
    return found;
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

std::vector<std::string> GraphEdit::upstream(const std::string& name) const {
    impl_->node(name);
    const std::set<std::string> found = impl_->upstream(name);
    return {found.begin(), found.end()};
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
