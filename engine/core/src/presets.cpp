// SPDX-License-Identifier: Apache-2.0
#include "groomlab/presets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/operators.h"
#include "groomlab/param_conditions.h"
#include "groomlab/paths.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/schema.h"

namespace groomlab {

namespace fs = std::filesystem;
// Presets are text the artist reads and shares: the fields stay in the order they were written.
using json = nlohmann::ordered_json;

const std::vector<std::string>& presetCategories() {
    static const std::vector<std::string> categories = {"Hair", "Braids", "Fur", "Feathers", "Scales", "Motion", "Techniques", "Games", "Add-ons"};
    return categories;
}

const std::vector<std::string>& presetFields() {
    static const std::vector<std::string> fields = {"size", "about", "requires", "backdrops", "controls", "exposed"};
    return fields;
}

// ---- Where presets are --------------------------------------------------------------------------

std::string presetUserDir() {
    if (const std::optional<std::string> own = environmentVariable("GROOMLAB_USER_PRESETS"); own && !own->empty()) return *own;
    // Maya's user application folder, where the Maya host saves (internalVar userAppDir).
    fs::path home;
#ifdef _WIN32
    if (const std::optional<std::string> profile = environmentVariable("USERPROFILE"); profile && !profile->empty()) {
        home = fs::path(*profile) / "Documents" / "maya";
    }
#elif defined(__APPLE__)
    if (const std::optional<std::string> h = environmentVariable("HOME"); h && !h->empty()) {
        home = fs::path(*h) / "Library" / "Preferences" / "Autodesk" / "maya";
    }
#else
    if (const std::optional<std::string> h = environmentVariable("HOME"); h && !h->empty()) home = fs::path(*h) / "maya";
#endif
    if (home.empty()) return {};
    return (home / "groomlab" / "presets").generic_string();
}

std::vector<std::string> presetSearchDirs(const std::string& userDir, const std::string& shippedDir) {
    std::vector<std::string> dirs;
    if (const std::optional<std::string> path = environmentVariable("GROOMLAB_PRESET_PATH")) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::stringstream in(*path);
        std::string dir;
        while (std::getline(in, dir, separator)) {
            if (!dir.empty()) dirs.push_back(dir);
        }
    }
    if (!userDir.empty()) dirs.push_back(userDir);
    if (!shippedDir.empty()) dirs.push_back(shippedDir);
    return dirs;
}

std::string presetPath(const std::string& name, const std::vector<std::string>& dirs) {
    for (const std::string& dir : dirs) {
        const fs::path path = fs::path(dir) / (name + ".json");
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) return path.generic_string();
    }
    return {};
}

std::string presetTitle(const std::string& name) {
    std::string words = name;
    std::replace(words.begin(), words.end(), '_', ' ');
    const auto first = words.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    words = words.substr(first, words.find_last_not_of(' ') - first + 1);
    words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
    return words;
}

namespace {

json parsePreset(const std::string& text) {
    json data = json::parse(text, nullptr, false);
    if (!data.is_object()) throw std::runtime_error("the preset is not a JSON object");
    return data;
}

std::string readFile(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

}  // namespace

PresetInfo presetInfo(const std::string& name, const std::string& path) {
    PresetInfo info;
    info.name = name;
    info.title = presetTitle(name);
    info.path = path;
    json data;
    try {
        data = parsePreset(readFile(path));
    } catch (const std::exception& e) {
        info.description = std::string("Cannot be read: ") + e.what();
        return info;
    }
    const auto about = data.find("about");
    if (about == data.end() || !about->is_object()) return info;
    auto field = [&](const char* key, std::string& out) {
        const auto v = about->find(key);
        if (v != about->end() && v->is_string() && !trimmed(v->get<std::string>()).empty()) out = trimmed(v->get<std::string>());
    };
    field("title", info.title);
    field("description", info.description);
    field("category", info.category);
    return info;
}

std::vector<PresetInfo> listPresets(const std::vector<std::string>& dirs) {
    // The first directory holding a name wins (the Maya host's rule: a user's preset hides a
    // shipped one of the same name).
    std::map<std::string, std::string> found;
    for (const std::string& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(fs::u8path(dir), ec)) continue;
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::u8path(dir), ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
            const std::string name = entry.path().stem().u8string();
            if (!found.count(name)) found[name] = entry.path().generic_u8string();
        }
    }
    std::vector<PresetInfo> out;
    for (const auto& [name, path] : found) out.push_back(presetInfo(name, path));
    return out;
}

std::string presetNameError(const std::string& name) {
    if (trimmed(name).empty()) return "a preset needs a name";
    for (const char c : name) {
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) return "a preset's name cannot hold " + std::string(1, c);
    }
    return {};
}

// ---- Fitting ------------------------------------------------------------------------------------

namespace {

// The operator specs by type (operatorSpecsJson), parsed once: a param's unit says how it fits.
const json& specs() {
    static const json parsed = [] {
        json byType = json::object();
        for (const json& op : json::parse(operatorSpecsJson())) byType[op.value("type", std::string())] = op;
        return byType;
    }();
    return parsed;
}

// The spec of an operator's param, or null for an unknown type or param.
const json* paramSpec(const std::string& type, const std::string& param) {
    const auto op = specs().find(type);
    if (op == specs().end()) return nullptr;
    const auto params = op->find("params");
    if (params == op->end() || !params->is_array()) return nullptr;
    for (const json& p : *params) {  // the spec lives in the cache: the pointer stays good
        if (p.value("name", std::string()) == param) return &p;
    }
    return nullptr;
}

// A node's param spec: its operator's, or for a compound's exposed param the inner node's (the
// inner node's operator's; an inner compound's exposed param stays as it is).
// A script node's param as its definition's header gives it (node_library.h), or null.
const json* scriptParamEntry(const json& node, const std::string& param) {
    if (node.value("type", std::string()) != "Script") return nullptr;
    const auto params = node.find("params");
    if (params == node.end() || !params->is_object()) return nullptr;
    const auto definition = params->find("definition");
    if (definition == params->end() || !definition->is_object() || !definition->contains("params")) return nullptr;
    for (const json& p : (*definition)["params"]) {
        if (p.is_object() && p.value("name", std::string()) == param) return &p;
    }
    return nullptr;
}

const json* nodeParamSpec(const json& node, const std::string& param, int depth = 0) {
    const std::string type = node.value("type", std::string());
    if (const json* spec = paramSpec(type, param)) return spec;
    if (const json* entry = scriptParamEntry(node, param)) return entry;
    if (type != "Compound" || depth > 16) return nullptr;
    const auto params = node.find("params");
    if (params == node.end() || !params->is_object()) return nullptr;
    const auto definition = params->find("definition");
    if (definition == params->end() || !definition->is_object()) return nullptr;
    const auto expose = definition->find("expose");
    if (expose == definition->end() || !expose->is_object()) return nullptr;
    const auto target = expose->find(param);
    if (target == expose->end() || !target->is_string()) return nullptr;
    const std::string spec = target->get<std::string>();
    const auto dot = spec.find('.');
    const std::string innerName = spec.substr(0, dot), innerParam = dot == std::string::npos ? std::string() : spec.substr(dot + 1);
    const auto graph = definition->find("graph");
    if (graph == definition->end() || !graph->is_object()) return nullptr;
    for (const json& inner : graph->value("nodes", json::array())) {
        if (inner.value("name", std::string()) == innerName) return nodeParamSpec(inner, innerParam, depth + 1);
    }
    return nullptr;
}

using Factors = std::map<std::string, double>;

bool isNumber(const json& v) { return v.is_number() && !v.is_boolean(); }

// The node of that name in a graph's JSON, or null.
const json* findNode(const json& graph, const std::string& name) {
    if (!graph.is_object()) return nullptr;
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return nullptr;
    for (const json& n : *nodes) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

// A node's param's kind (its spec's: float, vec3...), "" for an unknown one.
std::string kindOf(const json& node, const std::string& param) {
    const json* spec = nodeParamSpec(node, param);
    return spec ? spec->value("kind", std::string()) : std::string();
}

// A param's value on a node: as set, else its spec's default (null for neither).
json paramValueOf(const json& node, const std::string& param) {
    const auto params = node.find("params");
    if (params != node.end() && params->is_object() && params->contains(param)) return (*params)[param];
    const json* spec = nodeParamSpec(node, param);
    return spec ? spec->value("default", json()) : json();
}

// Whether a condition ({"param", "in"}: ParamSpec::unitWhen, ParamCondition) holds on `node`: its
// param's value (as set, else its default) by param_conditions.h's rule. No condition: it holds.
bool nodeConditionHolds(const json& node, const json& condition) {
    if (!condition.is_object() || condition.value("param", std::string()).empty()) return true;
    return groomlab::conditionHolds(condition, paramValueOf(node, condition["param"].get<std::string>()));
}

// What `param` of `node` measures: its spec's unit while its unitWhen holds (ParamSpec::unitWhen);
// a compound's exposed param the inner param's. "" for none.
std::string unitOf(const json& node, const std::string& param, int depth = 0) {
    if (depth > 16) return {};
    const std::string type = node.value("type", std::string());
    if (const json* entry = paramSpec(type, param) ? nullptr : scriptParamEntry(node, param)) {  // a script node's param
        const std::string kind = entry->value("type", std::string("number"));
        if (kind == "length") return "length";
        return kind == "number" ? entry->value("unit", std::string()) : std::string();
    }
    if (!paramSpec(type, param) && type == "Compound") {  // an exposed param: the inner node's
        const json params = node.value("params", json::object());
        const json definition = params.is_object() ? params.value("definition", json::object()) : json::object();
        const json expose = definition.is_object() ? definition.value("expose", json::object()) : json::object();
        const auto target = expose.find(param);
        if (target == expose.end() || !target->is_string()) return {};
        const std::string spec = target->get<std::string>();
        const auto dot = spec.find('.');
        if (dot == std::string::npos) return {};
        const json inner = definition.value("graph", json::object());
        const json* innerNode = findNode(inner, spec.substr(0, dot));
        return innerNode ? unitOf(*innerNode, spec.substr(dot + 1), depth + 1) : std::string();
    }
    const json* spec = paramSpec(type, param);
    if (!spec || !nodeConditionHolds(node, spec->value("unitWhen", json()))) return {};
    return spec->value("unit", std::string());
}

// The factor a unit takes, or nullopt when it does not scale.
std::optional<double> factorOf(const std::string& unit, const Factors& factors) {
    const auto it = factors.find(unit);
    return it == factors.end() ? std::nullopt : std::optional<double>(it->second);
}

// `value` (of a param of `kind`) scaled by its unit's factor: a number, a vector's every
// component (an expression in its place stays), or a map's output range (its default max of 1 too).
json fitValue(const json& value, const std::string& kind, const std::string& unit, const Factors& factors) {
    const std::optional<double> factor = factorOf(unit, factors);
    if (!factor || value.is_boolean()) return value;
    if (value.is_number()) return value.get<double>() * *factor;
    if (value.is_array()) {
        json out = value;
        for (json& v : out) {
            if (isNumber(v)) v = v.get<double>() * *factor;
        }
        return out;
    }
    if (value.is_object() && kind != "vec3") {
        json map = value;
        for (const char* key : {"min", "max"}) {
            if (map.contains(key) && isNumber(map[key])) map[key] = map[key].get<double>() * *factor;
        }
        if (!map.contains("max")) map["max"] = *factor;
        return map;
    }
    return value;
}

// An Expression's "variables" (JSON text) with each number that says its unit
// ({"value": 2, "unit": "length"}) fitted.
json fitVariables(const json& text, const Factors& factors) {
    if (!text.is_string()) return text;
    json variables = json::parse(text.get<std::string>(), nullptr, false);
    if (!variables.is_object()) return text;
    bool changed = false;
    for (auto& [name, v] : variables.items()) {
        if (!v.is_object() || !v.contains("unit") || !v["unit"].is_string() || !v.contains("value") || !isNumber(v["value"])) continue;
        if (const std::optional<double> f = factorOf(v["unit"].get<std::string>(), factors)) {
            v["value"] = v["value"].get<double>() * *f;
            changed = true;
        }
    }
    return changed ? json(variables.dump()) : text;
}

// The params of `node` (an object with a "type") its params leave at their spec defaults and whose
// unit scales, set to the defaults fitted by `factors`.
void fitDefaults(json& node, const Factors& factors) {
    const auto op = specs().find(node.value("type", std::string()));
    if (op == specs().end() || !op->contains("params") || !(*op)["params"].is_array()) return;
    const auto given = node.find("params");
    if (given != node.end() && !given->is_object()) return;
    for (const json& spec : (*op)["params"]) {
        const std::string name = spec.value("name", std::string());
        const json value = spec.value("default", json());
        const bool vector = value.is_array() && std::any_of(value.begin(), value.end(), [](const json& v) { return isNumber(v) && v.get<double>() != 0.0; });
        if (name.empty() || !(isNumber(value) || vector)) continue;
        if (node.contains("params") && node["params"].contains(name)) continue;
        const std::string unit = unitOf(node, name);
        if (!factorOf(unit, factors)) continue;
        node["params"][name] = fitValue(value, spec.value("kind", std::string()), unit, factors);
    }
}

// Fits the nodes of `graph` (an object with "nodes") in place; a Compound's inner nodes too, and
// its exposed params by the inner params they set (a feather design's barb length scales like any
// length). Each param's unit is its spec's as the node makes it (unitOf).
void fitNodes(json& graph, const Factors& factors, int depth = 0) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (json& node : graph["nodes"]) {
        if (!node.is_object()) continue;
        auto params = node.find("params");
        if (params == node.end()) {  // every param at its default
            fitDefaults(node, factors);
            continue;
        }
        if (!params->is_object()) continue;
        const std::string type = node.value("type", std::string());
        const bool compound = type == "Compound";
        const json given = node;
        for (auto& [name, value] : params->items()) {
            if (compound && name == "definition") continue;
            if (type == "Expression" && name == "variables") {
                value = fitVariables(value, factors);
                continue;
            }
            value = fitValue(value, kindOf(given, name), unitOf(given, name), factors);
        }
        if (compound && depth < 16) {
            auto definition = params->find("definition");
            if (definition != params->end() && definition->is_object()) {
                auto inner = definition->find("graph");
                if (inner != definition->end()) fitNodes(*inner, factors, depth + 1);
            }
        }
        fitDefaults(node, factors);
    }
}

// Every node of the graph, compounds' inner nodes too.
void eachNode(json& graph, const std::function<void(json&)>& visit, int depth = 0) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (json& node : graph["nodes"]) {
        if (!node.is_object()) continue;
        visit(node);
        if (node.value("type", std::string()) != "Compound" || depth > 16) continue;
        auto params = node.find("params");
        if (params == node.end() || !params->is_object()) continue;
        auto definition = params->find("definition");
        if (definition != params->end() && definition->is_object() && definition->contains("graph")) eachNode((*definition)["graph"], visit, depth + 1);
    }
}

// The attribute name (without ctl_) of a keyed param's control: a vector's parent attribute.
std::string controlBase(const json& control) {
    if (control.is_array() && !control.empty() && control[0].is_string()) {
        const std::string first = control[0].get<std::string>();
        return first.empty() ? first : first.substr(0, first.size() - 1);
    }
    return control.is_string() ? control.get<std::string>() : std::string();
}

// A preset's control values ("controls") fitted as what they set is: a keyed param's attribute
// as the param (a keyed length's sets the length, so it scales like one), a keyable Expression
// variable's by the unit it says. A Control node's value is a factor of the param it is plugged
// into (field.h), as every field's is: it stays.
void fitControls(json& graph, const Factors& factors) {
    auto values = graph.find("controls");
    if (values == graph.end() || !values->is_object()) return;
    std::set<std::string> done;
    auto fit = [&](const std::string& attr, const std::string& unit) {
        if (done.count(attr) || !values->contains(attr)) return;
        const std::optional<double> factor = factorOf(unit, factors);
        if (!factor) return;
        json& value = (*values)[attr];
        if (isNumber(value)) {
            value = value.get<double>() * *factor;
        } else if (value.is_array()) {
            for (json& v : value) {
                if (isNumber(v)) v = v.get<double>() * *factor;
            }
        }
        done.insert(attr);
    };
    eachNode(graph, [&](json& node) {
        const std::string type = node.value("type", std::string());
        const json params = node.value("params", json::object());
        if (type == "Expression" && params.is_object() && params.contains("variables") && params["variables"].is_string()) {
            const json variables = json::parse(params["variables"].get<std::string>(), nullptr, false);
            if (variables.is_object()) {
                for (const auto& [name, v] : variables.items()) {
                    if (v.is_object() && v.contains("control") && v["control"].is_string() && v.contains("unit") && v["unit"].is_string()) {
                        fit("ctl_" + v["control"].get<std::string>(), v["unit"].get<std::string>());
                    }
                }
            }
        }
        const auto links = node.find("paramControls");
        if (links == node.end() || !links->is_object()) return;
        for (const auto& [param, control] : links->items()) fit("ctl_" + controlBase(control), unitOf(node, param));
    });
}

// What each unit's params are multiplied by on a scalp `k` times as big (unitScaleFactor's table).
Factors factorsFor(double k) {
    Factors factors;
    for (const char* unit : {"length", "width", "density", "perLength"}) factors[unit] = *unitScaleFactor(unit, k);
    return factors;
}

json fitted(json graph, std::optional<double> size) {
    const auto recorded = graph.find("size");
    double from = 0.0;
    if (recorded != graph.end()) {
        if (isNumber(*recorded)) from = recorded->get<double>();
        graph.erase("size");
    }
    if (from <= 0.0 || !size || *size <= 0.0 || std::abs(*size / from - 1.0) < 1e-6) return graph;
    const Factors factors = factorsFor(*size / from);
    fitControls(graph, factors);  // by the units the graph's params have before they are fitted
    fitNodes(graph, factors);
    return graph;
}

}  // namespace

std::string fitPresetJson(const std::string& presetJson, std::optional<double> size) {
    return fitted(parsePreset(presetJson), size).dump();
}

std::string fitNewNodeParamsJson(const std::string& type, const std::string& paramsJson, std::optional<double> size) {
    json params = paramsJson.empty() ? json::object() : json::parse(paramsJson, nullptr, false);
    if (params.is_discarded() || !params.is_object()) throw std::runtime_error("a node's params are a JSON object");
    if (!size || !(*size > 0.0) || !std::isfinite(*size) || std::abs(*size / kDefaultsScalpSize - 1.0) < 1e-6) return params.dump();
    json node = {{"type", type}, {"params", params}};
    fitDefaults(node, factorsFor(*size / kDefaultsScalpSize));
    return node["params"].dump();
}

std::string defaultGraphJsonFor(std::optional<double> size) {
    json graph = json::parse(defaultGraphJson());
    graph["size"] = kDefaultsScalpSize;
    return fitted(std::move(graph), size).dump();
}

std::string presetGraphJson(const std::string& presetJson, std::optional<double> size) {
    json graph = fitted(parsePreset(presetJson), size);
    // The backdrops go in the graph, as every host keeps the layout (the other fields are the preset's own).
    json backdrops = json::array();
    if (graph.contains("backdrops") && graph["backdrops"].is_array()) {
        for (const json& b : graph["backdrops"]) {
            if (b.is_object()) backdrops.push_back(b);
        }
    }
    for (const std::string& field : presetFields()) graph.erase(field);
    if (!backdrops.empty()) graph["backdrops"] = backdrops;
    return graph.dump();
}

std::string presetControlsJson(const std::string& presetJson, std::optional<double> size) {
    const json graph = fitted(parsePreset(presetJson), size);
    const auto values = graph.find("controls");
    return values != graph.end() && values->is_object() ? values->dump() : "{}";
}

std::string applyPresetControlsToGraph(const std::string& graphJson, const std::string& controlsJson) {
    json graph = parsePreset(graphJson);
    const json controls = json::parse(controlsJson.empty() ? "{}" : controlsJson, nullptr, false);
    if (!controls.is_object() || controls.empty()) return graph.dump();
    auto valueOf = [&](const std::string& name) -> const json* {
        const auto it = controls.find("ctl_" + name);
        return it == controls.end() ? nullptr : &*it;
    };
    eachNode(graph, [&](json& node) {
        const std::string type = node.value("type", std::string());
        auto params = node.find("params");
        if (params == node.end() || !params->is_object()) {
            if (type != "Control") return;
            node["params"] = json::object();
            params = node.find("params");
        }
        if (type == "Control") {
            if (const json* v = valueOf(params->value("name", std::string("control")))) {
                if (isNumber(*v) || v->is_boolean()) (*params)["value"] = *v;
            }
        } else if (type == "Expression" && params->contains("variables") && (*params)["variables"].is_string()) {
            json variables = json::parse((*params)["variables"].get<std::string>(), nullptr, false);
            if (!variables.is_object()) return;
            bool changed = false;
            for (auto& [name, var] : variables.items()) {
                if (!var.is_object() || !var.contains("control") || !var["control"].is_string()) continue;
                if (const json* v = valueOf(var["control"].get<std::string>()); v && isNumber(*v)) {
                    var["value"] = *v;
                    changed = true;
                }
            }
            if (changed) (*params)["variables"] = variables.dump();
        }
        const auto links = node.find("paramControls");
        if (links == node.end() || !links->is_object()) return;
        for (const auto& [param, control] : links->items()) {
            const json* v = valueOf(controlBase(control));
            if (!v) continue;
            const bool vector = control.is_array();
            if (vector ? (v->is_array() && v->size() == 3) : (isNumber(*v) || v->is_boolean())) (*params)[param] = *v;
        }
    });
    return graph.dump();
}

namespace {

// What feeds `node`'s `input` in a graph's JSON ("" nothing).
std::string feederOf(const json& graph, const std::string& node, const std::string& input) {
    const auto connections = graph.find("connections");
    if (connections == graph.end() || !connections->is_array()) return {};
    for (const json& c : *connections) {
        if (c.is_object() && c.value("to", std::string()) == node && c.value("input", std::string()) == input) {
            return c.value("from", std::string());
        }
    }
    return {};
}

const json* nodeIn(const json& graph, const std::string& name) {
    const auto nodes = graph.find("nodes");
    if (nodes == graph.end() || !nodes->is_array()) return nullptr;
    for (const json& n : *nodes) {  // a pointer into the graph (value() would copy the array)
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

std::string categoryOf(const std::string& type) {
    const auto op = specs().find(type);
    // The operator's role in a chain (a source or grow node starts a look), not the menu's category.
    return op == specs().end() ? std::string() : op->value("role", std::string());
}

// The node a preset's look starts from: the first grow or source node up the Output's strands
// chain (or the node with nothing feeding its strands).
std::string lookBase(const json& graph, const std::string& first) {
    std::string node = first;
    for (int guard = 0; guard < 4096; ++guard) {
        const json* n = nodeIn(graph, node);
        if (!n) break;
        const std::string category = categoryOf(n->value("type", std::string()));
        if (category == "source" || category == "grow") return node;
        const std::string up = feederOf(graph, node, "strands");
        if (up.empty()) return node;
        node = up;
    }
    return node;
}

// The types whose params the default exposure (presets.h) picks first, in this order; the other
// look nodes follow in the graph's order.
const std::vector<std::string>& exposePriority() {
    static const std::vector<std::string> types = {"Clump", "Wet", "Curl", "Noise", "Braid", "Tangle", "Flyaway",
                                                   "Gravity", "Bend", "Length", "HairColor", "Width"};
    return types;
}
constexpr std::size_t kDefaultExposed = 6;

// The Compound param name for `param` of look node `node`: `wanted` when free, else node + Param,
// else that with 2, 3... `taken` holds the names in use (the Compound's own params, its inputs).
std::string exposedName(const std::string& wanted, const std::string& node, const std::string& param, std::set<std::string>& taken) {
    std::string name = wanted;
    if (name.empty() || taken.count(name)) {
        std::string capital = param;
        if (!capital.empty()) capital[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(capital[0])));
        name = node + capital;
        const std::string base = name;
        for (int i = 2; taken.count(name); ++i) name = base + std::to_string(i);
    }
    taken.insert(name);
    return name;
}

// The look's params the Compound shows (insertPresetJson): {name: {node, param}}, in the order
// picked. `lookNodes`: the definition's nodes (the look's, in the preset's order).
std::vector<std::pair<std::string, std::pair<std::string, std::string>>> exposedParams(const json& presetData, const json& lookNodes,
                                                                                       std::set<std::string> taken) {
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> out;
    auto lookNode = [&](const std::string& name) -> const json* {
        for (const json& n : lookNodes) {
            if (n.value("name", std::string()) == name && n.value("type", std::string()) != "Input") return &n;
        }
        return nullptr;
    };
    std::set<std::string> done;  // "node.param"
    const auto listed = presetData.find("exposed");
    if (listed != presetData.end() && listed->is_array()) {
        // The preset's own list: each entry naming a look node's param (others skipped: a node
        // the look leaves outside, such as the Scatter, or a param its operator has not).
        for (const json& e : *listed) {
            if (!e.is_object()) continue;
            const std::string node = e.value("node", std::string()), param = e.value("param", std::string());
            const json* n = lookNode(node);
            if (!n || !paramSpec(n->value("type", std::string()), param) || !done.insert(node + "." + param).second) continue;
            const json wanted = e.value("name", json());
            out.push_back({exposedName(wanted.is_string() ? wanted.get<std::string>() : param, node, param, taken), {node, param}});
        }
        return out;
    }
    // None listed: each look node's first plain number param (a float or int, not advanced, holding
    // a number: no map, no field port, not keyed), the nodes of exposePriority's types first.
    std::vector<const json*> order;
    for (const std::string& type : exposePriority()) {
        for (const json& n : lookNodes) {
            if (n.value("type", std::string()) == type) order.push_back(&n);
        }
    }
    for (const json& n : lookNodes) {
        const std::string type = n.value("type", std::string());
        const std::string category = categoryOf(type);
        if (type == "Input" || type == "Compound" || category == "field" || category == "utility") continue;
        if (std::find(exposePriority().begin(), exposePriority().end(), type) == exposePriority().end()) order.push_back(&n);
    }
    for (const json* n : order) {
        if (out.size() >= kDefaultExposed) break;
        const auto op = specs().find(n->value("type", std::string()));
        if (op == specs().end()) continue;
        const json params = n->value("params", json::object());
        const json inputs = n->value("paramInputs", json::array());
        const json controls = n->value("paramControls", json::object());
        for (const json& p : op->value("params", json::array())) {
            const std::string param = p.value("name", std::string()), kind = p.value("kind", std::string());
            if ((kind != "float" && kind != "int") || p.value("advanced", false) || p.value("group", std::string()) == "Advanced") continue;
            if (params.is_object() && params.contains(param) && !isNumber(params[param])) continue;
            if (std::find(inputs.begin(), inputs.end(), json(param)) != inputs.end()) continue;
            if (controls.is_object() && controls.contains(param)) continue;
            const std::string node = n->value("name", std::string());
            out.push_back({exposedName(param, node, param, taken), {node, param}});
            break;
        }
    }
    return out;
}

}  // namespace

std::string presetNodeName(const std::string& presetName) {
    std::string out;
    bool upper = false;
    for (char ch : presetName) {
        if (ch == '_' || ch == '-' || ch == ' ') {
            upper = !out.empty();
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
        upper = false;
    }
    return out.empty() ? "look" : out;
}

std::string insertPresetJson(const std::string& graphJson, const std::string& presetJson, std::optional<double> size,
                             const std::string& after, const std::string& name, std::string* nameMade) {
    // The preset's look: fitted, its own fields gone.
    const GraphEdit source(presetGraphJson(presetJson, size));
    const json preset = json::parse(source.toJson());
    std::string output = preset.value("output", std::string());
    if (output.empty()) {
        for (const json& n : preset.value("nodes", json::array())) {
            if (n.value("type", std::string()) == "Output") output = n.value("name", std::string());
        }
    }
    if (output.empty() || !nodeIn(preset, output)) throw std::runtime_error("the preset has no Output node");
    const std::string first = feederOf(preset, output, "strands");
    if (first.empty()) throw std::runtime_error("nothing feeds the preset's Output");
    const std::string base = lookBase(preset, first);
    if (base == first) throw std::runtime_error("the preset has no look nodes between its sources and its Output to insert");
    std::set<std::string> look;
    for (const std::string& n : source.upstream(output)) look.insert(n);
    for (const std::string& n : source.upstream(base)) look.erase(n);
    look.erase(base);
    look.erase(output);

    // The Compound's definition: an Input for the strands, one per other outside node the look
    // reads (named after it), the look's nodes and the connections among them.
    json definition = {{"graph", {{"version", 1}, {"nodes", json::array()}, {"connections", json::array()}, {"output", first}}},
                       {"inputs", json::array({{{"name", "strands"}, {"type", "Strands"}}})},
                       {"expose", json::object()}};
    if (look.count("strands")) throw std::runtime_error("the preset's look has a node named strands, which its input needs");
    definition["graph"]["nodes"].push_back({{"name", "strands"}, {"type", "Input"}, {"params", {{"name", "strands"}, {"type", "Strands"}}}});
    std::vector<std::pair<std::string, std::string>> outside;  // (input name, the preset node's type)
    for (const json& n : preset["nodes"]) {
        if (look.count(n.value("name", std::string()))) definition["graph"]["nodes"].push_back(n);
    }
    for (const json& c : preset.value("connections", json::array())) {
        const std::string from = c.value("from", std::string()), to = c.value("to", std::string());
        if (!look.count(to)) continue;
        if (look.count(from)) {
            definition["graph"]["connections"].push_back(c);
        } else if (from == base) {
            definition["graph"]["connections"].push_back({{"from", "strands"}, {"to", to}, {"input", c.value("input", std::string())}});
        } else {
            const json* n = nodeIn(preset, from);
            const std::string type = n ? n->value("type", std::string()) : std::string();
            bool known = false;
            for (const auto& [inputName, inputType] : outside) known = known || inputName == from;
            if (!known) {
                const std::string portType = source.outputType(from, c.value("output", std::string()));
                definition["inputs"].push_back({{"name", from}, {"type", portType}});
                definition["graph"]["nodes"].push_back({{"name", from}, {"type", "Input"}, {"params", {{"name", from}, {"type", portType}}}});
                outside.emplace_back(from, type);
            }
            definition["graph"]["connections"].push_back({{"from", from}, {"to", to}, {"input", c.value("input", std::string())}});
        }
    }
    // The look's key params on the Compound itself (the preset's "exposed", else the default
    // pick), each holding the inner node's value (fitted): the Compound reads as one node.
    json compoundParams = {{"definition", definition}};
    std::set<std::string> taken = {"definition", "repeat"};
    for (const json& input : definition["inputs"]) taken.insert(input.value("name", std::string()));
    for (const auto& [exposed, target] : exposedParams(parsePreset(presetJson), definition["graph"]["nodes"], taken)) {
        compoundParams["definition"]["expose"][exposed] = target.first + "." + target.second;
        for (const json& n : definition["graph"]["nodes"]) {
            if (n.value("name", std::string()) != target.first) continue;
            const json params = n.value("params", json::object());
            const json* spec = paramSpec(n.value("type", std::string()), target.second);
            compoundParams[exposed] = params.is_object() && params.contains(target.second) ? params[target.second]
                                                                                            : spec->value("default", json());
        }
    }

    // Where it goes: after `after`, or before the Output.
    json graph = parsePreset(graphJson);
    const std::string graphOutput = graph.value("output", std::string());
    std::string at = after;
    if (at.empty() || at == graphOutput) {
        if (graphOutput.empty()) throw std::runtime_error("the graph has no Output to insert before: pick a node to insert after");
        at = feederOf(graph, graphOutput, "strands");
        if (at.empty()) throw std::runtime_error("nothing feeds the graph's Output: pick a node to insert after");
    }
    const json* atNode = nodeIn(graph, at);
    if (!atNode) throw std::runtime_error("no node named " + at + " to insert after");
    // The nodes to its right move over a column.
    double x = 0.0, y = 0.0;
    if (atNode->contains("ui") && (*atNode)["ui"].is_object()) {
        x = (*atNode)["ui"].value("x", 0.0);
        y = (*atNode)["ui"].value("y", 0.0);
        for (json& n : graph["nodes"]) {
            if (n.contains("ui") && n["ui"].is_object() && n["ui"].value("x", 0.0) > x) n["ui"]["x"] = n["ui"].value("x", 0.0) + 240.0;
        }
    }
    std::vector<std::pair<std::string, std::string>> fed;  // (node, input) `at`'s strands fed
    for (const json& c : graph.value("connections", json::array())) {
        if (c.value("from", std::string()) != at) continue;
        const std::string out = c.value("output", std::string());
        if (!out.empty() && out != "strands") continue;
        fed.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
    }
    GraphEdit edit(graph.dump());
    if (edit.outputType(at) != "Strands") throw std::runtime_error(at + " gives no strands to insert the look after");
    for (const auto& [inputName, type] : outside) {
        bool found = false;
        for (const std::string& n : edit.nodeNames()) found = found || edit.nodeType(n) == type;
        if (!found) throw std::runtime_error("the preset's look reads a " + type + " node (" + inputName + ") this graph has none of");
    }
    const std::string made = edit.uniqueName(name.empty() ? "look" : name);
    edit.addNode("Compound", made, compoundParams.dump(), x + 240.0, y);
    for (const auto& [to, input] : fed) edit.connect(made, to, input);
    edit.connect(at, made, "strands");
    for (const auto& [inputName, type] : outside) {
        for (const std::string& n : edit.nodeNames()) {
            if (edit.nodeType(n) == type) {
                edit.connect(n, made, inputName);
                break;
            }
        }
    }
    if (nameMade) *nameMade = made;
    return edit.toJson();
}

std::vector<PresetMap> presetRequiredMaps(const std::string& presetJson) {
    std::vector<PresetMap> out;
    const json data = parsePreset(presetJson);
    const auto requires = data.find("requires");
    if (requires == data.end() || !requires->is_object()) return out;
    const auto maps = requires->find("maps");
    if (maps == requires->end() || !maps->is_array()) return out;
    for (const json& m : *maps) {
        if (!m.is_object() || !m.contains("name") || !m["name"].is_string() || m["name"].get<std::string>().empty()) continue;
        PresetMap map;
        map.name = m["name"].get<std::string>();
        if (m.contains("kind") && m["kind"].is_string()) map.kind = m["kind"].get<std::string>();
        if (m.contains("value") && isNumber(m["value"])) map.value = m["value"].get<double>();
        if (m.contains("color") && m["color"].is_array()) {
            for (const json& c : m["color"]) {
                if (isNumber(c)) map.color.push_back(c.get<double>());
            }
        }
        out.push_back(std::move(map));
    }
    return out;
}

std::vector<PresetGroup> presetRequiredGroups(const std::string& presetJson) {
    std::vector<PresetGroup> out;
    const json data = parsePreset(presetJson);
    const auto requires = data.find("requires");
    if (requires == data.end() || !requires->is_object()) return out;
    const auto groups = requires->find("groups");
    if (groups == requires->end() || !groups->is_array()) return out;
    for (const json& g : *groups) {
        if (!g.is_object() || !g.contains("name") || !g["name"].is_string() || g["name"].get<std::string>().empty()) continue;
        PresetGroup group;
        group.name = g["name"].get<std::string>();
        group.draw = g.contains("draw") && g["draw"].is_boolean() && g["draw"].get<bool>();
        const bool seen = std::any_of(out.begin(), out.end(), [&](const PresetGroup& o) { return o.name == group.name; });
        if (!seen) out.push_back(std::move(group));
    }
    return out;
}

// ---- Making one ---------------------------------------------------------------------------------

std::string makePresetJson(const std::string& graphJson, std::optional<double> size, const std::string& title,
                           const std::string& description, const std::string& category, const std::string& controlsJson) {
    json graph = parsePreset(graphJson);
    json backdrops;
    if (graph.contains("backdrops")) {
        backdrops = graph["backdrops"];
        graph.erase("backdrops");
    }
    json out = json::object();
    if (graph.contains("version")) {
        out["version"] = graph["version"];
        graph.erase("version");
    }
    if (size && *size > 0.0) out["size"] = *size;  // applied elsewhere, it is fitted from this
    json about = json::object();
    if (!trimmed(title).empty()) about["title"] = trimmed(title);
    if (!trimmed(description).empty()) about["description"] = trimmed(description);
    if (!trimmed(category).empty()) about["category"] = trimmed(category);
    if (!about.empty()) out["about"] = about;
    for (const std::string& field : presetFields()) graph.erase(field);
    for (auto& [key, value] : graph.items()) out[key] = value;
    if (backdrops.is_array() && !backdrops.empty()) out["backdrops"] = backdrops;
    if (!controlsJson.empty()) {
        const json controls = json::parse(controlsJson, nullptr, false);
        if (controls.is_object() && !controls.empty()) out["controls"] = controls;
    }
    return out.dump(2);
}

std::optional<double> unitScaleFactor(const std::string& unit, double k) {
    if (unit == "length" || unit == "width") return k;
    if (unit == "density") return 1.0 / (k * k);
    if (unit == "perLength") return 1.0 / k;
    return std::nullopt;
}

namespace {

bool scales(double k) { return std::isfinite(k) && k > 0.0 && std::abs(k - 1.0) > 1e-9; }

// The graph's JSON as an object (blank or "{}": the default graph); null for text that is not one.
json graphObject(const std::string& graphJson) {
    const bool blank = graphJson.find_first_not_of(" \t\r\n") == std::string::npos;
    json graph = json::parse(blank ? defaultGraphJson() : graphJson, nullptr, false);
    if (graph.is_object() && graph.empty()) graph = json::parse(defaultGraphJson());
    return graph.is_object() ? graph : json();
}

}  // namespace

std::string scaleGraphJson(const std::string& graphJson, double k) {
    if (!scales(k)) return graphJson;
    json graph = graphObject(graphJson);
    if (graph.is_null()) return graphJson;  // loadGraphJson says what is wrong with it
    fitNodes(graph, factorsFor(k));
    return graph.dump();
}

std::map<std::string, double> controlScaleFactors(const std::string& graphJson, double k) {
    std::map<std::string, double> out;
    if (!scales(k)) return out;
    json graph = graphObject(graphJson);
    if (graph.is_null()) return out;
    const Factors factors = factorsFor(k);
    auto add = [&](const std::string& control, const std::string& unit) {
        if (const std::optional<double> f = factorOf(unit, factors); f && !control.empty()) out.emplace(control, *f);
    };
    eachNode(graph, [&](json& node) {
        const json params = node.value("params", json::object());
        if (node.value("type", std::string()) == "Expression" && params.is_object() && params.contains("variables") &&
            params["variables"].is_string()) {
            const json variables = json::parse(params["variables"].get<std::string>(), nullptr, false);
            if (variables.is_object()) {
                for (const auto& [name, v] : variables.items()) {
                    if (v.is_object() && v.contains("control") && v["control"].is_string() && v.contains("unit") && v["unit"].is_string()) {
                        add(v["control"].get<std::string>(), v["unit"].get<std::string>());
                    }
                }
            }
        }
        const auto links = node.find("paramControls");
        if (links == node.end() || !links->is_object()) return;
        for (const auto& [param, controls] : links->items()) {
            const std::string unit = unitOf(node, param);
            if (controls.is_string()) add(controls.get<std::string>(), unit);
            if (!controls.is_array()) continue;
            for (const json& c : controls) {
                if (c.is_string()) add(c.get<std::string>(), unit);
            }
        }
    });
    return out;
}

std::optional<double> presetScalpSize(const ScalpMesh& scalp) {
    const std::vector<Vec3f>& points = scalp.points();
    if (points.empty()) return std::nullopt;
    Vec3f lo = points[0], hi = points[0];
    for (const Vec3f& p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const double size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    return size > 0.0 ? std::optional<double>(size) : std::nullopt;
}

// ---- A look as text ---------------------------------------------------------------------------

namespace {

std::string unfencedText(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    std::string out = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    if (out.rfind("```", 0) == 0) {
        const auto newline = out.find('\n');
        if (newline != std::string::npos && newline <= 13) {
            std::string rest = out.substr(newline + 1);
            const auto last = rest.find_last_not_of(" \t\r\n");
            rest = last == std::string::npos ? std::string() : rest.substr(0, last + 1);
            if (rest.size() >= 3 && rest.compare(rest.size() - 3, 3, "```") == 0) {
                rest = rest.substr(0, rest.size() - 3);
                const auto a = rest.find_first_not_of(" \t\r\n");
                const auto b = rest.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : rest.substr(a, b - a + 1);
            }
        }
    }
    return out;
}

bool isName(const json& v) { return v.is_string() && !v.get<std::string>().empty() && v.get<std::string>().size() <= 256; }

bool finiteAll(const json& v, int depth = 0) {
    if (depth > 64) return true;
    if (v.is_number_float()) return std::isfinite(v.get<double>());
    if (v.is_object() || v.is_array()) {
        for (const json& x : v) {
            if (!finiteAll(x, depth + 1)) return false;
        }
    }
    return true;
}

void graphShape(const json& graph, const std::string& where, int depth, std::size_t& count, std::vector<std::string>& problems) {
    if (depth > kMaxPresetDepth) {
        problems.push_back(where + "compounds nest more than " + std::to_string(kMaxPresetDepth) + " deep");
        return;
    }
    const json nodes = graph.value("nodes", json::array());
    if (!nodes.is_array()) {
        problems.push_back(where + "\"nodes\" is not a list");
        return;
    }
    for (const json& node : nodes) {
        if (++count > kMaxPresetNodes) return;
        if (!node.is_object() || !isName(node.value("name", json())) || !isName(node.value("type", json()))) {
            problems.push_back(where + "a node without a name and a type");
            continue;
        }
        const std::string label = where + node["name"].get<std::string>();
        const std::string type = node["type"].get<std::string>();
        const json params = node.value("params", json::object());
        if (!params.is_object()) {
            problems.push_back(label + ": \"params\" is not an object");
            continue;
        }
        const json inputs = node.value("paramInputs", json::array());
        if (!inputs.is_array() || !std::all_of(inputs.begin(), inputs.end(), [](const json& p) { return p.is_string(); })) {
            problems.push_back(label + ": \"paramInputs\" is not a list of names");
        }
        const json links = node.value("paramControls", json::object());
        bool linksOk = links.is_object();
        if (linksOk) {
            for (const json& c : links) {
                const bool three = c.is_array() && c.size() == 3 && std::all_of(c.begin(), c.end(), isName);
                linksOk = linksOk && (isName(c) || three);
            }
        }
        if (!linksOk) problems.push_back(label + ": \"paramControls\" does not name controls");
        if (node.contains("ui") && !node["ui"].is_null()) {
            const json& ui = node["ui"];
            const bool ok = ui.is_object() && (!ui.contains("x") || ui["x"].is_number()) && (!ui.contains("y") || ui["y"].is_number());
            if (!ok) problems.push_back(label + ": \"ui\" is not a position");
        }
        const bool library = type.rfind("node:", 0) == 0;
        if (!library && !OperatorRegistry::instance().contains(type)) {
            problems.push_back(label + ": unknown node type '" + type + "'");
        }
        if (type == "Compound" && params.contains("definition") && params["definition"].is_object()) {
            const json& definition = params["definition"];
            const json inner = definition.value("graph", json::object());
            if (!inner.is_object() || !definition.value("expose", json::object()).is_object()) {
                problems.push_back(label + ": the Compound's definition is not a graph");
            } else {
                graphShape(inner, label + "/", depth + 1, count, problems);
            }
        }
    }
    const json connections = graph.value("connections", json::array());
    bool linksOk = connections.is_array();
    if (linksOk) {
        for (const json& c : connections) {
            linksOk = linksOk && c.is_object() && isName(c.value("from", json())) && isName(c.value("to", json())) &&
                      isName(c.value("input", json())) && (!c.contains("output") || c["output"].is_null() || isName(c["output"]));
        }
    }
    if (!linksOk) problems.push_back(where + "\"connections\" are not links between nodes");
    if (graph.contains("output") && !graph["output"].is_null() && !isName(graph["output"])) {
        problems.push_back(where + "\"output\" is not a node's name");
    }
}

// A pasted graph's params its nodes' kinds no longer have, left out: an older GroomLab's (a groom
// keeps them, unread, and Copy Graph copies them), in compounds' inner graphs too, with the param
// inputs and keys naming them. A Compound's own params are its exposed ones (kept); a library
// node's are its definition's (kept); a Script's are its definition's header's.
void dropUnknownParams(json& graph, int depth) {
    if (depth > kMaxPresetDepth || !graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) return;
    const OperatorRegistry& registry = OperatorRegistry::instance();
    for (json& node : graph["nodes"]) {
        if (!node.is_object() || !node.contains("type") || !node["type"].is_string()) continue;
        if (!node.contains("params") || !node["params"].is_object()) continue;
        const std::string type = node["type"].get<std::string>();
        json& params = node["params"];
        if (type == op::kCompound) {
            if (params.contains("definition") && params["definition"].is_object() && params["definition"].contains("graph")) {
                dropUnknownParams(params["definition"]["graph"], depth + 1);
            }
            continue;
        }
        if (type.rfind("node:", 0) == 0 || !registry.contains(type)) continue;
        std::set<std::string> known;
        try {
            const std::unique_ptr<Operator> op = registry.create(type);
            const bool script = type == op::kScript;
            if (script && params.contains("definition")) {
                const json& d = params["definition"];
                op->params().set("definition", d.is_string() ? d.get<std::string>() : d.dump());
            }
            for (const ParamSpec& spec : script ? nodeParamSpecs(*op) : allParamSpecs(*op)) known.insert(spec.name);
            if (script) known.insert("definition");
        } catch (const std::exception&) {
            continue;  // (a Script whose definition does not read: its params as they are)
        }
        for (auto it = params.begin(); it != params.end();) {
            it = known.count(it.key()) ? std::next(it) : params.erase(it);
        }
        if (node.contains("paramInputs") && node["paramInputs"].is_array()) {
            json kept = json::array();
            for (const json& p : node["paramInputs"]) {
                if (!p.is_string() || known.count(p.get<std::string>())) kept.push_back(p);
            }
            node["paramInputs"] = kept;
        }
        if (node.contains("paramControls") && node["paramControls"].is_object()) {
            json& links = node["paramControls"];
            for (auto it = links.begin(); it != links.end();) {
                it = known.count(it.key()) ? std::next(it) : links.erase(it);
            }
        }
    }
}

}  // namespace

std::string presetFromText(const std::string& text) {
    const std::string none = "the clipboard holds no GroomLab graph";
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) throw std::invalid_argument("the clipboard is empty");
    if (text.size() > kMaxPresetText) {
        std::ostringstream s;
        s.precision(1);
        s << std::fixed << "the text is " << static_cast<double>(text.size()) / 1048576.0 << " MB, more than any groom's graph (at most "
          << kMaxPresetText / 1048576 << " MB)";
        throw std::invalid_argument(s.str());
    }
    const std::string body = unfencedText(text);
    {
        // Nested past any graph's depth: refused before parsing (a parse, and the value's
        // destruction, would go as deep).
        int depth = 0, deepest = 0;
        bool quoted = false, escaped = false;
        for (const char c : body) {
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
                continue;
            }
            if (c == '"') quoted = true;
            else if (c == '[' || c == '{') deepest = std::max(deepest, ++depth);
            else if (c == ']' || c == '}') --depth;
        }
        if (deepest > 256) throw std::invalid_argument("the text is nested too deeply");
    }
    json data;
    try {
        data = json::parse(body);
    } catch (const json::exception&) {
        throw std::invalid_argument(none + " (it is not JSON text; copy one with Copy Graph)");
    }
    if (data.is_object() && data.contains("groomlabNodes")) {
        throw std::invalid_argument("the clipboard holds copied nodes; paste them into a graph with Ctrl+V");
    }
    if (data.is_object() && !data.contains("nodes") && data.contains("graph") && data["graph"].is_object()) {
        data = data["graph"];  // a .groom file's graph
    }
    if (!data.is_object() || !data.contains("nodes") || !data["nodes"].is_array()) {
        throw std::invalid_argument(none + " (no \"nodes\"; copy one with Copy Graph)");
    }
    std::vector<std::string> problems;
    const json version = data.contains("version") ? data["version"] : json(1);
    const int current = currentSchemaVersion(DocumentKind::Graph);
    if (!version.is_number() || version.get<double>() < 1) {
        problems.push_back("\"version\" is not a version number");
    } else if (version.get<double>() > current) {
        problems.push_back("it was made by a newer GroomLab (graph version " + version.dump() + "; this one reads " +
                           std::to_string(current) + ")");
    }
    if (data.contains("size") && !data["size"].is_null() &&
        !(data["size"].is_number() && data["size"].get<double>() > 0.0 && data["size"].get<double>() < 1e12)) {
        problems.push_back("\"size\" is not a scalp size");
    }
    const std::vector<std::pair<std::string, bool>> kinds = {
        {"about", true}, {"requires", true}, {"controls", true}, {"backdrops", false}, {"exposed", false}};
    for (const auto& [key, object] : kinds) {
        if (data.contains(key) && !(object ? data[key].is_object() : data[key].is_array())) {
            problems.push_back("\"" + key + "\" is not " + (object ? "an object" : "a list"));
        }
    }
    if (!finiteAll(data)) problems.push_back("it holds a number too big to use");
    std::size_t count = 0;
    graphShape(data, "", 0, count, problems);
    if (count > kMaxPresetNodes) {
        problems.insert(problems.begin(), "it has more than " + std::to_string(kMaxPresetNodes) + " nodes, more than a groom can use");
    }
    if (!problems.empty()) {
        std::string message;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, problems.size()); ++i) {
            std::string p = problems[i];
            if (p.size() > 200) p = p.substr(0, 200) + "...";
            message += (i ? "; " : "") + p;
        }
        if (problems.size() > 3) message += " (and " + std::to_string(problems.size() - 3) + " more)";
        throw std::invalid_argument(message);
    }
    dropUnknownParams(data, 0);
    return data.dump();
}

}  // namespace groomlab
