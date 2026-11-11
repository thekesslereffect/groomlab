// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_controls.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

// Where a new Control node goes: left of the node it drives and below it (every host's).
constexpr double kControlDx = -240.0;
constexpr double kControlDy = 160.0;

json graphOf(const GraphEdit& edit) {
    json g = json::parse(edit.toJson());
    if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
    return g;
}

json& nodeIn(json& graph, const std::string& name) {
    for (json& n : graph["nodes"]) {
        if (n.is_object() && n.value("name", std::string()) == name) return n;
    }
    throw GraphError("no node '" + name + "'");
}

json paramSpec(const GraphEdit& edit, const std::string& node, const std::string& param) {
    if (!edit.hasNode(node)) throw GraphError("no node '" + node + "'");
    for (const json& p : json::parse(edit.nodeSpecJson(node)).value("params", json::array())) {
        if (p.is_object() && p.value("name", std::string()) == param) return p;
    }
    throw GraphError("node '" + node + "' has no param '" + param + "'");
}

double numberOr(const json& v, double fallback) {
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    return v.is_number() ? v.get<double>() : fallback;
}

void eachNode(const json& graph, const std::function<void(const json&)>& fn, int depth = 0) {
    if (!graph.is_object() || depth > 16) return;
    for (const json& n : graph.value("nodes", json::array())) {
        if (!n.is_object()) continue;
        fn(n);
        const json params = n.value("params", json::object());
        if (n.value("type", std::string()) == "Compound" && params.is_object()) {
            json definition = params.value("definition", json::object());
            if (definition.is_string()) definition = json::parse(definition.get<std::string>(), nullptr, false);
            if (definition.is_object()) eachNode(definition.value("graph", json::object()), fn, depth + 1);
        }
    }
}

}  // namespace

std::string controlName(const std::string& wanted) {
    std::string out;
    for (const char c : wanted) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "p_" + out;
    return out;
}

std::string controlsJson(const std::string& graphJson) {
    json out = json::object();
    const json graph = json::parse(graphJson, nullptr, false);
    eachNode(graph, [&](const json& n) {
        const std::string type = n.value("type", std::string());
        const json params = n.contains("params") && n["params"].is_object() ? n["params"] : json::object();
        const std::string node = n.value("name", std::string());
        if (type == "Control") {
            out[params.value("name", std::string("control"))] = {{"kind", "control"},
                                                                {"node", node},
                                                                {"default", numberOr(params.value("default", json()), 0.0)},
                                                                {"min", numberOr(params.value("min", json()), 0.0)},
                                                                {"max", numberOr(params.value("max", json()), 1.0)}};
        } else if (type == "Expression" && params.contains("variables")) {
            json variables = params["variables"];
            if (variables.is_string()) variables = json::parse(variables.get<std::string>(), nullptr, false);
            if (variables.is_object()) {
                for (const auto& [var, value] : variables.items()) {
                    if (!value.is_object() || !value.contains("control") || !value["control"].is_string()) continue;
                    out[value["control"].get<std::string>()] = {{"kind", "variable"},
                                                                {"node", node},
                                                                {"variable", var},
                                                                {"default", numberOr(value.value("value", json()), 0.0)},
                                                                {"min", numberOr(value.value("min", json()), 0.0)},
                                                                {"max", numberOr(value.value("max", json()), 1.0)}};
                }
            }
        }
        if (n.contains("paramControls") && n["paramControls"].is_object()) {
            json spec;  // the node's spec, read once when a keyed param leaves its value out
            for (const auto& [param, control] : n["paramControls"].items()) {
                json own = params.contains(param) ? params[param] : json(nullptr);
                if (own.is_null()) {
                    if (spec.is_null()) spec = json::parse(nodeSpecJson(n.dump()), nullptr, false);
                    for (const json& p : spec.is_object() ? spec.value("params", json::array()) : json::array()) {
                        if (p.is_object() && p.value("name", std::string()) == param) own = p.value("default", json());
                    }
                }
                if (control.is_array()) {
                    for (std::size_t i = 0; i < control.size(); ++i) {
                        if (!control[i].is_string()) continue;
                        const double d = own.is_array() && own.size() == 3 ? numberOr(own[i], 0.0) : 0.0;
                        out[control[i].get<std::string>()] = {
                            {"kind", "keyed"}, {"node", node}, {"param", param}, {"component", i}, {"default", d}};
                    }
                } else if (control.is_string()) {
                    out[control.get<std::string>()] = {{"kind", "keyed"}, {"node", node}, {"param", param}, {"default", numberOr(own, 0.0)}};
                }
            }
        }
    });
    return out.dump();
}

std::string notKeyable(const GraphEdit& edit, const std::string& node, const std::string& param) {
    const json spec = paramSpec(edit, node, param);
    if (edit.nodeType(node) == "Control") return "a Control node's params make its slider: drive the slider's control itself";
    if (spec.contains("animatable") && spec["animatable"].is_boolean() && !spec["animatable"].get<bool>()) {
        return "it sets how many strands, points or cards there are, or where they go: that must stay the same on every frame "
               "(motion blur, Alembic and Unreal groom caches)";
    }
    const std::string kind = spec.value("kind", std::string());
    if (kind != "float" && kind != "int" && kind != "bool" && kind != "vec3") return "only numbers, switches and vectors can be keyed";
    json g = graphOf(edit);
    const json& n = nodeIn(g, node);
    const json params = n.value("params", json::object());
    if (params.is_object() && params.contains(param) && params[param].is_object()) return "it reads a map: set it back to a value to key it";
    return {};
}

void exposeParam(GraphEdit& edit, const std::string& node, const std::string& param) {
    const json spec = paramSpec(edit, node, param);
    if (!spec.contains("port") || spec["port"].is_null()) throw GraphError(node + "." + param + " cannot be an input: it sets the node's own ports");
    const json nodeSpec = json::parse(edit.nodeSpecJson(node));
    for (const json& p : nodeSpec.value("inputs", json::array())) {
        if (p.value("param", false)) continue;
        if (p.value("name", std::string()) == param) throw GraphError(node + " already has an input named '" + param + "'");
    }
    json g = graphOf(edit);
    json& n = nodeIn(g, node);
    json& exposed = n["paramInputs"];
    if (!exposed.is_array()) exposed = json::array();
    if (std::find(exposed.begin(), exposed.end(), json(param)) != exposed.end()) return;
    exposed.push_back(param);
    const json params = nodeSpec.value("params", json::array());
    auto rank = [&](const json& name) {
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i].value("name", std::string()) == name.get<std::string>()) return i;
        }
        return params.size();
    };
    std::stable_sort(exposed.begin(), exposed.end(), [&](const json& a, const json& b) { return rank(a) < rank(b); });
    edit = GraphEdit(g.dump());
}

std::string addControl(GraphEdit& edit, const std::string& node, const std::string& param, const std::string& name,
                       std::optional<double> defaultValue, double min, double max, std::optional<double> groomSize) {
    const json spec = paramSpec(edit, node, param);
    const json controls = json::parse(controlsJson(edit.toJson()));
    std::string control = controlName(name.empty() ? param : name);
    if (!name.empty() && controls.contains(control)) throw GraphError("the groom has a control named '" + control + "' already");
    const std::string base = control;  // (the param's name: numbered until free)
    for (int i = 1; controls.contains(control); ++i) control = base + std::to_string(i);
    json g = graphOf(edit);
    const json& n = nodeIn(g, node);
    double start = min;
    if (defaultValue) {
        start = *defaultValue;
    } else {
        const json params = n.value("params", json::object());
        const json own = params.is_object() && params.contains(param) ? params[param] : spec.value("default", json());
        if (own.is_number()) start = own.get<double>();
    }
    const json ui = n.value("ui", json::object());
    const double x = ui.is_object() ? numberOr(ui.value("x", json()), 0.0) : 0.0;
    const double y = ui.is_object() ? numberOr(ui.value("y", json()), 0.0) : 0.0;
    const json params = {{"name", control}, {"default", start}, {"min", std::min(min, start)}, {"max", std::max(max, start)}};
    const std::string made = edit.addNode("Control", edit.uniqueName(control + "Control"), params.dump(), x + kControlDx, y + kControlDy, groomSize);
    exposeParam(edit, node, param);
    edit.connect(made, node, param);
    return made;
}

std::string linkControl(GraphEdit& edit, const std::string& node, const std::string& param, const std::string& wanted,
                        const std::vector<std::string>& taken) {
    const json spec = paramSpec(edit, node, param);
    json g = graphOf(edit);
    json& n = nodeIn(g, node);
    if (n.contains("paramControls") && n["paramControls"].is_object() && n["paramControls"].contains(param)) {
        return n["paramControls"][param].dump();
    }
    const std::string why = notKeyable(edit, node, param);
    if (!why.empty()) throw GraphError(node + "." + param + " cannot be keyed: " + why);
    json& params = n["params"];
    if (!params.is_object()) params = json::object();
    const json value = params.contains(param) ? params[param] : spec.value("default", json());
    const bool vector = spec.value("kind", std::string()) == "vec3";
    const std::string suffixes = spec.value("unit", std::string()) == "color" ? "RGB" : "XYZ";
    const json controls = json::parse(controlsJson(g.dump()));
    const std::set<std::string> held(taken.begin(), taken.end());
    auto isFree = [&](const std::string& c) {
        if (controls.contains(c) || held.count(c)) return false;
        for (const char s : std::string("XYZRGB")) {
            if (controls.contains(c + s) || held.count(c + s)) return false;
        }
        return true;
    };
    const std::string base = controlName(wanted.empty() ? node + "_" + param : wanted);
    std::string name = base;
    for (int i = 1; !isFree(name); ++i) name = base + std::to_string(i);
    json control = name;
    if (vector) control = json::array({name + suffixes[0], name + suffixes[1], name + suffixes[2]});
    if (!params.contains(param)) params[param] = value;  // stored, so its kind is known
    json& links = n["paramControls"];
    if (!links.is_object()) links = json::object();
    links[param] = control;
    edit = GraphEdit(g.dump());
    return control.dump();
}

}  // namespace groomlab
