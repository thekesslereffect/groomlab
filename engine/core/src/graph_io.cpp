// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_io.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "operators/internal.h"
#include "schema_internal.h"

#include "groomlab/node_catalog.h"
#include "groomlab/operators.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

ParamValue paramFromJson(const json& j, const ParamValue* declared, const std::string& name) {
    if (j.is_boolean()) return j.get<bool>();
    // Straight-segment ramp: {"keys": [[position, value], ...], "linear": true}.
    if (j.is_object() && j.contains("keys")) {
        ParamValue keys = paramFromJson(j.at("keys"), declared, name);
        Ramp* ramp = std::get_if<Ramp>(&keys);
        if (!ramp) throw GraphError("ramp for param '" + name + "' needs keys [[position, value], ...]");
        ramp->straight = j.value("linear", false);
        return *ramp;
    }
    if (j.is_object()) {
        MapRef map;
        int sources = 0;
        for (const auto source : {MapRef::Source::Texture, MapRef::Source::ColorSet, MapRef::Source::Attribute,
                                  MapRef::Source::Expression, MapRef::Source::Guide}) {
            if (j.contains(MapRef::sourceKey(source))) {
                map.source = source;
                map.name = j.at(MapRef::sourceKey(source)).get<std::string>();
                ++sources;
            }
        }
        if (sources != 1) {
            throw GraphError("map for param '" + name + "' needs exactly one of texture, colorSet, attribute, expression, guide");
        }
        const std::string component = j.value("component", std::string("r"));
        bool known = false;
        for (const auto c : {MapRef::Component::R, MapRef::Component::G, MapRef::Component::B, MapRef::Component::A,
                             MapRef::Component::Luminance}) {
            if (component == MapRef::componentName(c)) {
                map.component = c;
                known = true;
            }
        }
        if (!known) throw GraphError("map for param '" + name + "': unknown component '" + component + "'");
        map.min = j.value("min", 0.0f);
        map.max = j.value("max", 1.0f);
        return map;
    }
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number()) {
        const bool wantsInt = declared && std::holds_alternative<std::int64_t>(*declared);
        const bool wantsDouble = declared && std::holds_alternative<double>(*declared);
        if (wantsInt && j.is_number_integer()) return j.get<std::int64_t>();
        if (wantsInt) {
            // Truncated toward zero, as ints are typed; one past the int range is an error, not undefined behavior.
            const double d = std::trunc(j.get<double>());
            if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
                throw GraphError("param '" + name + "': " + j.dump() + " is out of range for an integer");
            }
            return static_cast<std::int64_t>(d);
        }
        if (wantsDouble || j.is_number_float()) return j.get<double>();
        return j.get<std::int64_t>();
    }
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        return Vec3f(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    }
    // Ramp: [[position, value], ...]
    if (j.is_array() && !j.empty() && std::all_of(j.begin(), j.end(), [](const json& k) {
            return k.is_array() && k.size() == 2 && k[0].is_number() && k[1].is_number();
        })) {
        Ramp ramp;
        for (const json& k : j) ramp.keys.emplace_back(k[0].get<float>(), k[1].get<float>());
        ramp.sort();
        return ramp;
    }
    throw GraphError("unsupported value for param '" + name + "'");
}

json paramToJson(const ParamValue& value) {
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Vec3f>) {
                return json::array({v.x, v.y, v.z});
            } else if constexpr (std::is_same_v<T, MapRef>) {
                return json{{MapRef::sourceKey(v.source), v.name},
                            {"component", MapRef::componentName(v.component)},
                            {"min", v.min},
                            {"max", v.max}};
            } else if constexpr (std::is_same_v<T, Ramp>) {
                json keys = json::array();
                for (const auto& [t, value] : v.keys) keys.push_back(json::array({t, value}));
                if (v.straight) return json{{"keys", keys}, {"linear", true}};
                return keys;
            } else {
                return json(v);
            }
        },
        value);
}

// A port as operatorSpecsJson lists it: name, type, optional (inputs), and a Value port's kind.
json portJson(const PortSpec& p, bool input) {
    json port = {{"name", p.name}, {"type", portTypeName(p.type)}};
    if (input) port["optional"] = p.optional;
    if (p.type == PortType::Value) port["valueKind"] = valueKindName(p.valueKind);
    return port;
}

std::size_t inputSlot(const Graph& graph, NodeId id, const std::string& inputName) {
    const auto specs = graph.inputSpecs(id);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].name == inputName) return i;
    }
    throw GraphError(graph.node(id).typeName() + " has no input '" + inputName + "'");
}

GraphDocument parse(json root) {
    if (!root.is_object()) throw GraphError("graph JSON must be an object");
    try {
        detail::checkVersion(DocumentKind::Graph, root);  // refuses graphs from a newer GroomLab
    } catch (const SchemaError& e) {
        throw GraphError(e.what());
    }

    GraphDocument doc;
    for (const json& n : root.value("nodes", json::array())) {
        const std::string name = n.at("name").get<std::string>();
        std::string type = n.at("type").get<std::string>();
        if (doc.nodesByName.count(name)) throw GraphError("duplicate node name '" + name + "'");
        json params = n.value("params", json::object());  // items() must not outlive this
        if (type.rfind("node:", 0) == 0) {  // a library node by its type: the Compound or Script it stands for
            params = json::parse(libraryNodeStubParams(type.substr(5), params.is_object() ? params.dump() : std::string(), &type));
        }

        const NodeId id = doc.graph.addNode(type);
        Operator& op = doc.graph.node(id);
        // A Script's params are its definition's: set it first.
        const bool script = type == op::kScript;
        if (script && params.is_object() && params.contains("definition")) {
            const json& d = params["definition"];
            op.params().set("definition", d.is_string() ? d.get<std::string>() : d.dump());
        }
        const auto specs = script ? nodeParamSpecs(op) : allParamSpecs(op);
        for (const auto& [key, value] : params.items()) {
            if ((type == op::kCompound || script) && key == "definition") {
                if (script) continue;
                if (value.is_object()) {
                    op.params().set(key, value.dump());  // stored as JSON text, written back as an object
                    continue;
                }
            }
            const ParamValue* declared = nullptr;
            for (const ParamSpec& s : specs) {
                if (s.name == key) declared = &s.defaultValue;
            }
            op.params().set(key, paramFromJson(value, declared, key));
        }
        if (n.contains("paramInputs")) doc.graph.setParamInputs(id, n.at("paramInputs").get<std::vector<std::string>>());
        if (n.contains("paramControls")) {
            // {"param": "control"} for a number, {"param": ["x", "y", "z"]} for a vec3.
            const json& links = n.at("paramControls");
            if (!links.is_object()) throw GraphError("node '" + name + "': paramControls must map params to controls");
            std::map<std::string, std::vector<std::string>> controls;
            for (const auto& [param, value] : links.items()) {
                if (value.is_string()) {
                    controls[param] = {value.get<std::string>()};
                } else if (value.is_array() && std::all_of(value.begin(), value.end(), [](const json& v) { return v.is_string(); })) {
                    controls[param] = value.get<std::vector<std::string>>();
                } else {
                    throw GraphError("node '" + name + "': param '" + param + "' must name its control (or three for a vector)");
                }
            }
            try {
                doc.graph.setParamControls(id, std::move(controls));
            } catch (const GraphError& e) {
                throw GraphError("node '" + name + "': " + e.what());
            }
        }
        if (n.value("bypass", false)) doc.graph.setBypass(id, true);
        if (n.contains("evaluation")) {
            // When it runs (EvaluationMode); none: always.
            const json& e = n.at("evaluation");
            if (!e.is_string()) throw GraphError("node '" + name + "': evaluation is always, viewportOnly or renderOnly");
            try {
                doc.graph.setEvaluationMode(id, evaluationModeFromName(e.get<std::string>()));
            } catch (const GraphError& error) {
                throw GraphError("node '" + name + "': " + error.what());
            }
        }
        if (n.contains("version")) {
            // The behaviour the node was saved with (Operator::nodeVersion); none: the newest.
            const json& v = n.at("version");
            if (!v.is_number_integer() || v.get<std::int64_t>() < 1) {
                throw GraphError("node '" + name + "': version " + v.dump() + " is not a whole number from 1");
            }
            if (v.get<std::int64_t>() > op.version()) {
                throw GraphError("node '" + name + "' is a " + type + " of version " + v.dump() +
                                 ", newer than this GroomLab's (" + std::to_string(op.version()) + "); update GroomLab to open it");
            }
            op.setNodeVersion(static_cast<int>(v.get<std::int64_t>()));
            // A param whose default changed since that version, left out: the former default, what
            // the node made then (ParamSpec::since).
            for (const ParamSpec& s : specs) {
                if (s.since > 0 && op.nodeVersion() < s.since && !(params.is_object() && params.contains(s.name))) {
                    op.params().set(s.name, s.before);
                }
            }
        }
        doc.graph.setLabel(id, name);
        doc.nodesByName[name] = id;
        // Where the editors draw it (carried, never evaluated).
        if (n.contains("ui") && n.at("ui").is_object()) {
            const json& ui = n.at("ui");
            doc.positions[name] = {ui.value("x", 0.0), ui.value("y", 0.0)};
        }
    }
    if (root.contains("backdrops") && root.at("backdrops").is_array() && !root.at("backdrops").empty()) {
        doc.backdropsJson = root.at("backdrops").dump();
    }
    // The editors' reroute points the links are drawn through (carried, never evaluated).
    if (root.contains("pins") && root.at("pins").is_object() && !root.at("pins").empty()) {
        doc.pinsJson = root.at("pins").dump();
    }

    for (const json& c : root.value("connections", json::array())) {
        const NodeId from = doc.node(c.at("from").get<std::string>());
        const NodeId to = doc.node(c.at("to").get<std::string>());
        // "output": which of from's outputs (absent: its first).
        const std::size_t output = c.contains("output") ? doc.graph.outputIndex(from, c.at("output").get<std::string>()) : 0;
        doc.graph.connect(from, to, inputSlot(doc.graph, to, c.at("input").get<std::string>()), output);
    }

    if (!root.contains("output")) throw GraphError("graph JSON has no 'output' node");
    doc.output = doc.node(root.at("output").get<std::string>());
    return doc;
}

}  // namespace

NodeId GraphDocument::node(const std::string& name) const {
    auto it = nodesByName.find(name);
    if (it == nodesByName.end()) throw GraphError("unknown node '" + name + "'");
    return it->second;
}

std::string GraphDocument::nameOf(NodeId id) const {
    for (const auto& [name, nodeId] : nodesByName) {
        if (nodeId == id) return name;
    }
    return {};
}

GraphDocument loadGraphJson(const std::string& text) {
    const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
    json root;
    try {
        root = json::parse(blank ? defaultGraphJson() : text);
    } catch (const json::exception& e) {
        throw GraphError(std::string("invalid graph JSON: ") + e.what());
    }
    if (root.is_object() && root.empty()) root = json::parse(defaultGraphJson());

    try {
        return parse(root);
    } catch (const json::exception& e) {
        throw GraphError(std::string("invalid graph JSON: ") + e.what());
    }
}

Ramp rampFromJson(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) throw GraphError("ramp: malformed JSON");
    const ParamValue value = paramFromJson(j, nullptr, "ramp");
    const Ramp* ramp = std::get_if<Ramp>(&value);
    if (!ramp) throw GraphError("ramp: needs keys [[position, value], ...]");
    return *ramp;
}

std::unique_ptr<Operator> operatorFromJson(const std::string& type, const std::string& params) {
    std::unique_ptr<Operator> op = OperatorRegistry::instance().create(type);
    const json values = json::parse(params.empty() ? std::string("{}") : params, nullptr, false);
    if (!values.is_object()) return op;
    // A Script's params are its definition's: set it first.
    if (type == op::kScript && values.contains("definition")) {
        const json& d = values["definition"];
        op->params().set("definition", d.is_string() ? d.get<std::string>() : d.dump());
    }
    const auto specs = type == op::kScript ? nodeParamSpecs(*op) : allParamSpecs(*op);
    for (const auto& [key, value] : values.items()) {
        if ((type == op::kCompound || type == op::kScript) && key == "definition") {
            op->params().set(key, value.is_string() ? value.get<std::string>() : value.dump());
            continue;
        }
        const ParamValue* declared = nullptr;
        for (const ParamSpec& s : specs) {
            if (s.name == key) declared = &s.defaultValue;
        }
        try {
            op->params().set(key, paramFromJson(value, declared, key));
        } catch (const std::exception&) {  // a value that does not parse: its default
        }
    }
    return op;
}

std::string paramsJson(const ParamSet& params) {
    json out = json::object();
    for (const auto& [name, value] : params.values()) out[name] = paramToJson(value);
    return out.dump();
}

std::string saveGraphJson(const GraphDocument& doc) {
    json nodes = json::array();
    json connections = json::array();

    for (const auto& [name, id] : doc.nodesByName) {
        const Operator& op = doc.graph.node(id);
        json params = json::object();
        const ParamSet stored = doc.graph.storedParams(id);  // keyed params at their own values
        for (const auto& [key, value] : stored.values()) {
            const std::string* text = std::get_if<std::string>(&value);
            if ((op.typeName() == op::kCompound || op.typeName() == op::kScript) && key == "definition" && text) {
                params[key] = json::parse(*text, nullptr, false);
                if (params[key].is_discarded()) params[key] = *text;
            } else {
                params[key] = paramToJson(value);
            }
        }
        json node = {{"name", name}, {"type", op.typeName()}, {"version", op.nodeVersion()}, {"params", params}};
        if (!doc.graph.paramInputs(id).empty()) node["paramInputs"] = doc.graph.paramInputs(id);
        if (!doc.graph.paramControls(id).empty()) {
            json links = json::object();
            for (const auto& [param, controls] : doc.graph.paramControls(id)) {
                links[param] = controls.size() == 1 ? json(controls[0]) : json(controls);
            }
            node["paramControls"] = links;
        }
        if (doc.graph.bypassed(id)) node["bypass"] = true;
        if (doc.graph.evaluationMode(id) != EvaluationMode::Always) {
            node["evaluation"] = evaluationModeName(doc.graph.evaluationMode(id));
        }
        if (const auto at = doc.positions.find(name); at != doc.positions.end()) {
            node["ui"] = {{"x", at->second.x}, {"y", at->second.y}};
        }
        nodes.push_back(std::move(node));

        const auto specs = doc.graph.inputSpecs(id);
        for (std::size_t slot = 0; slot < specs.size(); ++slot) {
            NodeId src = 0;
            std::size_t output = 0;
            if (doc.graph.inputSource(id, slot, &src, &output)) {
                json link = {{"from", doc.nameOf(src)}, {"to", name}, {"input", specs[slot].name}};
                if (output != 0) link["output"] = doc.graph.outputSpecs(src).at(output).name;  // the first: left out
                connections.push_back(std::move(link));
            }
        }
    }

    json root = {
        {"version", currentSchemaVersion(DocumentKind::Graph)},
        {"nodes", nodes},
        {"connections", connections},
        {"output", doc.nameOf(doc.output)},
    };
    if (!doc.backdropsJson.empty()) {
        json backdrops = json::parse(doc.backdropsJson, nullptr, false);
        if (backdrops.is_array()) root["backdrops"] = std::move(backdrops);
    }
    if (!doc.pinsJson.empty()) {
        json pins = json::parse(doc.pinsJson, nullptr, false);
        if (pins.is_object()) root["pins"] = std::move(pins);
    }
    return root.dump(2);
}

namespace {

// How many decimals a host shows a number param with (the spec's "decimals"): none for an int or
// pixels, one for degrees, else by its slider's span (a fraction as 0 to 1: one decimal as a
// percentage). Every host formats the same value the same way.
int paramDecimals(const ParamSpec& p, const char* kind) {
    if (std::string(kind) == "int" || p.unit == "pixels") return 0;
    if (p.unit == "angle") return 1;
    if (p.unit == "fraction" || p.unit == "color") return 3;
    if (p.unit == "turns" || p.unit == "seconds") return 2;
    double span = 0.0;
    const auto lo = p.softMin ? p.softMin : p.min;
    const auto hi = p.softMax ? p.softMax : p.max;
    if (lo && hi) {
        span = *hi - *lo;
    } else if (const double* d = std::get_if<double>(&p.defaultValue)) {
        span = std::abs(*d) * 2.0;
    }
    if (span >= 100.0) return 1;
    if (span >= 10.0) return 2;
    if (span >= 0.1 || span <= 0.0) return 3;
    return 4;
}

}  // namespace

std::string paramSpecJson(const ParamSpec& p) {
    const char* kinds[] = {"bool", "int", "float", "string", "vec3", "ramp", "map"};  // ParamValue order
    json param = {{"name", p.name},
                  {"kind", kinds[p.defaultValue.index()]},
                  {"default", paramToJson(p.defaultValue)},
                  {"description", p.description},
                  {"mappable", p.description.find("(mappable") != std::string::npos},
                  {"unit", p.unit},
                  {"choices", p.choices},
                  {"animatable", p.animatable},
                  {"group", p.group},
                  {"advanced", p.advanced},
                  {"worldDirection", p.worldDirection},
                  {"vectorExpression", p.vectorExpression},
                  {"colorField", p.colorField},
                  {"channel", channelUseName(p.channel)}};
    if (p.defaultValue.index() == 1 || p.defaultValue.index() == 2 || p.defaultValue.index() == 4) {
        param["decimals"] = paramDecimals(p, kinds[p.defaultValue.index()]);
    }
    // Limits and slider ranges only where set; the slider's falls back to the limits.
    if (p.min) param["min"] = *p.min;
    if (p.max) param["max"] = *p.max;
    if (p.softMin || p.min) param["softMin"] = p.softMin ? *p.softMin : *p.min;
    if (p.softMax || p.max) param["softMax"] = p.softMax ? *p.softMax : *p.max;
    if (!p.showWhen.empty()) param["showWhen"] = {{"param", p.showWhen.param}, {"in", p.showWhen.in}};
    if (p.channelOptional) param["channelOptional"] = true;
    if (!p.unitWhen.param.empty()) param["unitWhen"] = {{"param", p.unitWhen.param}, {"in", p.unitWhen.in}};
    // A changed default: nodes saved before version `since` take `before` (ParamSpec::since).
    if (p.since > 0) param["formerDefault"] = {{"since", p.since}, {"value", paramToJson(p.before)}};
    // The input port it takes when exposed (none: it cannot be an input).
    PortSpec port;
    if (paramPortSpec(p, &port)) {
        param["port"] = {{"type", portTypeName(port.type)}};
        if (port.type == PortType::Value) param["port"]["valueKind"] = valueKindName(port.valueKind);
    }
    return param.dump();
}

std::string operatorSpecsJson() {
    const OperatorRegistry& registry = OperatorRegistry::instance();
    json ops = json::array();
    for (const std::string& type : registry.typeNames()) {
        const std::unique_ptr<Operator> op = registry.create(type);
        json inputs = json::array();
        for (const PortSpec& p : op->inputs()) inputs.push_back(portJson(p, true));
        const PortSpec out = op->output();
        json outputs = json::array();
        for (const PortSpec& p : op->outputs()) outputs.push_back(portJson(p, false));
        json params = json::array();
        for (const ParamSpec& p : allParamSpecs(*op)) params.push_back(json::parse(paramSpecJson(p)));
        // The channels it writes under fixed names, as its defaults set it (not a param's).
        json fixedChannels = json::array();
        for (const ChannelWrite& w : op->channelWrites()) {
            if (w.param.empty()) fixedChannels.push_back(w.name);
        }
        // The add-node menu's category and label (node_catalog.h: the info the type registered
        // with); the operator's own category is its role in a chain (presets: where a look starts).
        const NodeInfo* info = nodeInfo(type);
        const std::string category = info && !info->category.empty() ? info->category : op->category();
        const int categoryOrder = nodeCategoryOrder(category);
        const std::string categoryTitle = categoryOrder < static_cast<int>(nodeCategories().size())
                                              ? nodeCategories()[static_cast<std::size_t>(categoryOrder)].title
                                              : category;
        ops.push_back({{"type", type},
                       {"fixedChannels", fixedChannels},
                       {"version", op->version()},
                       {"category", category},
                       {"categoryTitle", categoryTitle},
                       {"categoryOrder", categoryOrder},
                       {"order", nodeMenuOrder(type)},
                       {"role", op->category()},
                       {"label", nodeLabel(type)},
                       {"keywords", info ? info->keywords : std::string()},
                       {"hidden", info ? info->hidden : false},
                       {"description", info ? info->description : std::string()},
                       {"inputs", inputs},
                       {"output", portJson(out, false)},
                       {"outputs", outputs},
                       {"params", params}});
    }
    return ops.dump();
}

namespace {

json compoundDefinition(const std::string& definition) {
    json d = json::parse(definition, nullptr, false);
    if (d.is_discarded() || !d.is_object()) throw GraphError("the definition is not a JSON object");
    return d;
}

}  // namespace

std::string compoundGraphJson(const std::string& definition) {
    const json d = compoundDefinition(definition);
    if (!d.contains("graph") || !d.at("graph").is_object()) throw GraphError("the definition has no graph");
    return d.at("graph").dump();
}

std::vector<PortSpec> compoundInputs(const std::string& definition) {
    const json d = compoundDefinition(definition);
    const json inputs = d.value("inputs", json::array());
    if (!inputs.is_array()) throw GraphError("the definition's inputs are not a list");
    std::vector<PortSpec> ports;
    for (const json& p : inputs) {
        PortSpec port;
        try {
            port.name = p.at("name").get<std::string>();
            const std::string type = p.value("type", std::string("Strands"));
            try {
                port.type = portTypeFromName(type);
            } catch (const GraphError&) {
                throw GraphError("compound input '" + port.name + "' has unknown type '" + type + "'");
            }
            port.optional = p.value("optional", false);
            if (p.contains("valueKind")) port.valueKind = valueKindFromName(p.at("valueKind").get<std::string>());
        } catch (const json::exception&) {
            throw GraphError("a compound input needs a \"name\" (text), and may give a \"type\" (text) and \"optional\" (true or false)");
        }
        ports.push_back(port);
    }
    return ports;
}

std::map<std::string, std::string> compoundExposed(const std::string& definition) {
    const json d = compoundDefinition(definition);
    const json expose = d.value("expose", json::object());  // items() must not outlive it
    if (!expose.is_object()) throw GraphError("the definition's expose is not an object of name: \"node.param\"");
    std::map<std::string, std::string> out;
    for (const auto& [name, target] : expose.items()) {
        if (!target.is_string()) throw GraphError("exposed param '" + name + "' must name node.param");
        out[name] = target.get<std::string>();
    }
    return out;
}

const std::string& defaultGraphJson() {
    static const std::string text = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter"},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "length", "type": "Length", "params": {"fraction": 1.0, "trimMin": 0.0, "trimMax": 0.2}},
    {"name": "noise", "type": "Noise"},
    {"name": "color", "type": "HairColor"},
    {"name": "width", "type": "Width"},
    {"name": "resample", "type": "Resample"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "length", "input": "strands"},
    {"from": "length", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "color", "input": "strands"},
    {"from": "color", "to": "width", "input": "strands"},
    {"from": "width", "to": "resample", "input": "strands"},
    {"from": "resample", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";
    return text;
}

}  // namespace groomlab
