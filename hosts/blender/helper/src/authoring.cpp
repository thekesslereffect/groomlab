// SPDX-License-Identifier: Apache-2.0
// The groom's authoring beyond the brushes (authoring.h, docs/BLENDER.md "Authoring"), as Maya
// makes it: the Graph Editor's edits (groomlab/graph.py's GraphModel, params.py's add_control and
// keyed params), the node library (nodes.py), Save as Preset (presets.py), the corrective stack
// (shots.py) and Sculpt nodes (sculpts.py). Each edit of the groom is one undo step on the
// session's stack (tools.h commitTexts): the graph, the correctives and the Sculpt nodes' edits
// are stored texts the stack puts back bit for bit.

#include "authoring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/correctives.h"
#include "groomlab/expression.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/node_library.h"
#include "groomlab/presets.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/sculpt.h"

#include "request.h"
#include "session.h"
#include "tools.h"

namespace groomlab::helper {

namespace fs = std::filesystem;

namespace {

// ---- The graph ----------------------------------------------------------------------------------

std::string graphTextOf(const GroomSession& g) {
    const std::string& text = g.data().graphJson;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return defaultGraphJson();
    const auto last = text.find_last_not_of(" \t\r\n");
    if (text[first] == '{' && text[last] == '}' && text.find_first_not_of(" \t\r\n", first + 1) == last) return defaultGraphJson();
    return text;
}

// The groom's scalp size at the size its params are at (helper.cpp's scalpSize).
std::optional<double> groomSizeOf(const Session& s) {
    const MeshPtr& scalp = s.groom->rawScalp();
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size) *size /= s.groom->groomScale();
    return size;
}

json graphJsonOf(const GraphEdit& edit) {
    json g = json::parse(edit.toJson());
    if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
    return g;
}

json& nodeIn(json& graph, const std::string& name) {
    for (json& n : graph["nodes"]) {
        if (n.is_object() && n.value("name", std::string()) == name) return n;
    }
    fail("not_found", "no node '" + name + "'");
}

json specOf(const GraphEdit& edit, const std::string& node) {
    if (!edit.hasNode(node)) fail("not_found", "no node '" + node + "'");
    return json::parse(edit.nodeSpecJson(node));
}

json paramSpecOf(const GraphEdit& edit, const std::string& node, const std::string& param) {
    for (const json& p : specOf(edit, node).value("params", json::array())) {
        if (p.is_object() && p.value("name", std::string()) == param) return p;
    }
    fail("not_found", "node '" + node + "' has no param '" + param + "'");
}

// One edit of the graph as one undo step: its text, with the Sculpt nodes' edits following the
// nodes it moved (renames, Group, Ungroup: sculpts.py's follow_graph_edit). A graph that does not
// load yet (an input left open) is kept as edit_graph keeps it, without an undo step; a graph that
// loads stays one that loads (GraphError, nothing changed).
json commitGraph(Session& s, const std::string& label, const std::string& before, const GraphEdit& edit, json r = json::object()) {
    const std::string text = edit.toJson();
    if (!r.is_object()) r = json::object();
    json moved = json::array();
    for (const auto& [from, to] : edit.movedNodes()) moved.push_back({from, to});
    r["moved"] = moved;
    if (text == before) {
        r["graphChanged"] = false;
        r["graphError"] = s.groom->graphError();
        r["undo"] = undoStateJson(s);
        return r;
    }
    const std::string& storedSculpts = s.groom->data().sculptsJson;
    std::string sculpts = storedSculpts;
    if (!edit.movedNodes().empty() && !storedSculpts.empty()) {
        try {
            sculpts = moveSculptNodes(storedSculpts, edit.movedNodes());
        } catch (const std::exception&) {
            sculpts = storedSculpts;  // unreadable edits stay as they are (the evaluation says why)
        }
    }
    if (!s.groom->graphError().empty()) {
        try {
            s.groom->setGraphJson(text);
            r["graphError"] = "";
        } catch (const GraphError& e) {
            s.groom->dropGraph(text, e.what());
            r["graphError"] = e.what();
        }
        if (sculpts != storedSculpts) s.groom->setSculptsJson(sculpts);
        r["undoable"] = false;
    } else {
        std::vector<std::pair<StrokeField, std::string>> texts = {{StrokeField::Graph, text}};
        if (sculpts != storedSculpts) texts.emplace_back(StrokeField::Sculpts, sculpts);
        commitTexts(s, label, texts);
        r["graphError"] = "";
        r["undoable"] = true;
    }
    r["graphChanged"] = true;
    r["undo"] = undoStateJson(s);
    return r;
}

std::vector<std::string> namesOf(const json& v, const char* what) {
    if (!v.is_array()) fail("bad_request", std::string(what) + " is a list of node names");
    std::vector<std::string> out;
    for (const json& n : v) {
        if (!n.is_string()) fail("bad_request", std::string(what) + " is a list of node names");
        out.push_back(n.get<std::string>());
    }
    return out;
}

// GraphModel.expose_param: the core's (graph_controls.h), its GraphError a bad request.
void exposeParamRequest(GraphEdit& edit, const std::string& node, const std::string& param) {
    specOf(edit, node);
    try {
        groomlab::exposeParam(edit, node, param);
    } catch (const GraphError& e) {
        fail("bad_request", e.what());
    }
}

// GraphModel.unexpose_param: the port goes with what fed it; the param keeps its own value.
void unexposeParam(GraphEdit& edit, const std::string& node, const std::string& param) {
    paramSpecOf(edit, node, param);
    {
        json g = graphJsonOf(edit);
        const json& n = nodeIn(g, node);
        if (!n.contains("paramInputs") || !n["paramInputs"].is_array()) return;
        if (std::find(n["paramInputs"].begin(), n["paramInputs"].end(), json(param)) == n["paramInputs"].end()) return;
    }
    edit.disconnect(node, param);
    json g = graphJsonOf(edit);
    json& n = nodeIn(g, node);
    json& exposed = n["paramInputs"];
    exposed.erase(std::find(exposed.begin(), exposed.end(), json(param)));
    if (exposed.empty()) n.erase("paramInputs");
    edit = GraphEdit(g.dump());
}

// GraphModel.expose (a Compound's panel): inner param `target` ("node.param") shown as `name`.
void compoundExpose(GraphEdit& edit, const std::string& compound, const std::string& name, const std::string& target) {
    if (edit.nodeType(compound) != "Compound") fail("bad_request", compound + " is not a Compound");
    if (name.empty()) fail("bad_request", "an exposed param needs a name");
    const auto dot = target.find('.');
    if (dot == std::string::npos) fail("bad_request", "target is \"node.param\" inside the compound");
    const std::string innerName = target.substr(0, dot), param = target.substr(dot + 1);
    json g = graphJsonOf(edit);
    json& n = nodeIn(g, compound);
    json& definition = n["params"]["definition"];
    if (!definition.is_object()) definition = json::object();
    const json* inner = nullptr;
    if (definition.contains("graph") && definition["graph"].is_object() && definition["graph"].contains("nodes") &&
        definition["graph"]["nodes"].is_array()) {
        for (const json& x : definition["graph"]["nodes"]) {
            if (x.is_object() && x.value("name", std::string()) == innerName) inner = &x;
        }
    }
    bool known = false;
    if (inner) {
        try {
            const json spec = json::parse(nodeSpecJson(inner->dump()));
            for (const json& p : spec.value("params", json::array())) known = known || p.value("name", std::string()) == param;
        } catch (const std::exception&) {
        }
    }
    if (!known) fail("not_found", "the compound has no param '" + target + "'");
    json& expose = definition["expose"];
    if (!expose.is_object()) expose = json::object();
    expose[name] = target;
    edit = GraphEdit(g.dump());
}

// GraphModel.unexpose: the compound stops showing `target`; its value inside stays what it set last.
void compoundUnexpose(GraphEdit& edit, const std::string& compound, const std::string& target) {
    if (edit.nodeType(compound) != "Compound") fail("bad_request", compound + " is not a Compound");
    json g = graphJsonOf(edit);
    json& n = nodeIn(g, compound);
    if (!n.contains("params") || !n["params"].is_object()) return;
    json& params = n["params"];
    if (!params.contains("definition") || !params["definition"].is_object()) return;
    json& definition = params["definition"];
    if (!definition.contains("expose") || !definition["expose"].is_object()) return;
    json& expose = definition["expose"];
    std::vector<std::string> names;
    for (const auto& [name, t] : expose.items()) {
        if (t.is_string() && t.get<std::string>() == target) names.push_back(name);
    }
    const auto dot = target.find('.');
    const std::string innerName = dot == std::string::npos ? target : target.substr(0, dot);
    const std::string param = dot == std::string::npos ? std::string() : target.substr(dot + 1);
    for (const std::string& name : names) {
        if (params.contains(name) && definition.contains("graph") && definition["graph"].is_object()) {
            for (json& inner : definition["graph"]["nodes"]) {
                if (inner.is_object() && inner.value("name", std::string()) == innerName) inner["params"][param] = params[name];
            }
            params.erase(name);
        }
        expose.erase(name);
    }
    edit = GraphEdit(g.dump());
}

// GraphModel.insert_on_link: the one link from (its output) -> to.input routed through `node`:
// node reads from on its first input that takes from's output, to reads node.
void insertOnLink(GraphEdit& edit, const std::string& from, const std::string& to, const std::string& input,
                  const std::string& node, const std::string& output) {
    if (!edit.hasNode(node)) fail("not_found", "no node '" + node + "'");
    const json g = graphJsonOf(edit);
    bool linked = false;
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("from", std::string()) == from && c.value("to", std::string()) == to && c.value("input", std::string()) == input &&
            c.value("output", std::string()) == output) {
            linked = true;
        }
    }
    if (!linked) fail("not_found", from + (output.empty() ? "" : "." + output) + " is not connected to " + to + "." + input);
    std::string why;
    for (const json& p : specOf(edit, node).value("inputs", json::array())) {
        GraphEdit trial = edit;
        try {
            trial.connect(from, node, p.value("name", std::string()), output);
            trial.connect(node, to, input);
        } catch (const GraphError& e) {
            if (why.empty()) why = e.what();
            continue;
        }
        edit = std::move(trial);
        return;
    }
    throw GraphError(node + " has no input that takes " + from + "'s " + edit.outputType(from, output) + " and feeds " + to + "." +
                     input + (why.empty() ? "" : " (" + why + ")"));
}

// ---- Controls (params.py): Control nodes' sliders, Expression variables, keyed params ----------

double numberOr(const json& v, double fallback) { return v.is_number() ? v.get<double>() : fallback; }

// Every control the graph reads (params.py's _controls_in): {name: what it is}, the core's
// (graph_controls.h controlsJson: a keyed param it leaves out at its spec's default).
json controlsOf(const json& graph) { return json::parse(controlsJson(graph.dump())); }

// params.py's add_control: a Control node (its slider: name, default, range) driving node.param,
// the param exposed first: the core's (graph_controls.h addControl), as Maya and the app make it.
json addControl(Session& s, GraphEdit& edit, const json& args) {
    const std::string node = needString(args, "node"), param = needString(args, "param");
    paramSpecOf(edit, node, param);
    const std::optional<double> value =
        args.contains("default") && args["default"].is_number() ? std::optional<double>(args["default"].get<double>()) : std::nullopt;
    std::string control;
    try {
        control = groomlab::addControl(edit, node, param, optString(args, "name", ""), value, optNumber(args, "min", 0.0),
                                       optNumber(args, "max", 1.0), groomSizeOf(s));
    } catch (const GraphError& e) {
        fail("bad_request", e.what());
    }
    json g = graphJsonOf(edit);
    const json made = nodeIn(g, control).value("params", json::object());
    return {{"node", control}, {"control", made.value("name", param)}};
}

// params.py's set_param_key, its first part: node.param linked to a control of its own
// (<node>_<param>, numbered when taken; three for a vector: X Y Z, or R G B for a color), at the
// param's value: the core's (graph_controls.h linkControl). The host keys the control
// (set_controls on each frame).
json linkControl(GraphEdit& edit, const json& args) {
    const std::string node = needString(args, "node"), param = needString(args, "param");
    paramSpecOf(edit, node, param);
    const std::string before = edit.toJson();
    std::string control;
    try {
        control = groomlab::linkControl(edit, node, param, optString(args, "control", ""));
    } catch (const GraphError& e) {
        fail("bad_request", e.what());
    }
    return {{"control", json::parse(control)}, {"changed", edit.toJson() != before}};
}

// params.py's remove_param_animation: the param back to its own value (the control's value now
// when the session has it).
json unlinkControl(Session& s, GraphEdit& edit, const json& args) {
    const std::string node = needString(args, "node"), param = needString(args, "param");
    const json spec = paramSpecOf(edit, node, param);
    json g = graphJsonOf(edit);
    json& n = nodeIn(g, node);
    if (!n.contains("paramControls") || !n["paramControls"].is_object() || !n["paramControls"].contains(param)) {
        return {{"changed", false}};
    }
    const json control = n["paramControls"][param];
    const std::map<std::string, float>& values = s.groom->data().controls;
    const std::string kind = spec.value("kind", std::string());
    auto valueOf = [&](const json& c, double fallback) {
        if (!c.is_string()) return fallback;
        const auto it = values.find(c.get<std::string>());
        return it == values.end() ? fallback : static_cast<double>(it->second);
    };
    json& params = n["params"];
    if (!params.is_object()) params = json::object();
    if (control.is_array() && control.size() == 3) {
        json own = params.contains(param) && params[param].is_array() ? params[param] : json::array({0.0, 0.0, 0.0});
        for (std::size_t i = 0; i < 3; ++i) own[i] = valueOf(control[i], numberOr(own[i], 0.0));
        params[param] = own;
    } else {
        const json own = params.contains(param) ? params[param] : spec.value("default", json());
        const double fallback = own.is_boolean() ? (own.get<bool>() ? 1.0 : 0.0) : numberOr(own, 0.0);
        const double v = valueOf(control, fallback);
        if (kind == "int") {
            params[param] = static_cast<std::int64_t>(std::llround(v));
        } else if (kind == "bool") {
            params[param] = v >= 0.5;
        } else {
            params[param] = v;
        }
    }
    n["paramControls"].erase(param);
    if (n["paramControls"].empty()) n.erase("paramControls");
    edit = GraphEdit(g.dump());
    return {{"changed", true}, {"value", params[param]}};
}

// ---- The node library (nodes.py) ----------------------------------------------------------------

// node_file's header from a request: "nodeId" its id (a request's "id" is the protocol's), the
// other fields by their names (nodes.py's node_file).
json headerOf(const json& args) {
    json header = json::object();
    if (args.contains("nodeId") && !args["nodeId"].is_null()) header["id"] = args["nodeId"];
    for (const char* key : {"version", "label", "category", "description", "author", "license", "icon", "params", "paramInfo"}) {
        if (args.contains(key) && !args[key].is_null()) header[key] = args[key];
    }
    return header;
}

std::set<std::string> libraryIds() {
    std::set<std::string> ids;
    for (const LibraryNode& n : libraryNodes()) {
        if (!n.id.empty()) ids.insert(n.id);
    }
    return ids;
}

int libraryVersion(const std::string& id) {
    for (const LibraryNode& n : libraryNodes()) {
        if (n.id == id && n.error.empty()) return n.version;
    }
    return 0;
}

json definitionOf(const json& v) {
    if (v.is_string()) {
        json d = json::parse(v.get<std::string>(), nullptr, false);
        if (d.is_discarded() || !d.is_object()) fail("bad_request", "definition is a node file: a JSON object (or its text)");
        return d;
    }
    if (!v.is_object()) fail("bad_request", "definition is a node file: a JSON object (or its text)");
    return v;
}

json scriptErrors(const json& definition) {
    const std::string text = definition.is_string() ? definition.get<std::string>() : definition.dump();
    return json::parse(scriptNodeErrorsJson(text)).value("errors", json::array());
}

std::string errorsText(const json& errors) {
    std::string out;
    for (const json& e : errors) {
        if (!out.empty()) out += "; ";
        const int line = e.value("line", 0);
        if (line > 0) out += "line " + std::to_string(line) + ", column " + std::to_string(e.value("column", 0)) + ": ";
        out += e.value("message", std::string());
    }
    return out;
}

// nodes.py's script_template: a new script node's file whose id the library does not have yet.
json scriptTemplate(const std::string& label, const std::string& author) {
    json definition = json::parse(scriptNodeTemplate(label, author));
    const std::set<std::string> taken = libraryIds();
    const std::string base = definition.value("id", std::string("my.script"));
    for (int i = 2; taken.count(definition.value("id", std::string())); ++i) definition["id"] = base + std::to_string(i);
    return definition;
}

// The node `node` of the graph fed by `after` and feeding what it fed (nodes.py's add_node).
void feedAfter(GraphEdit& edit, const std::string& node, const std::string& after) {
    if (after.empty()) return;
    if (!edit.hasNode(after)) fail("not_found", "no node '" + after + "'");
    const json g = graphJsonOf(edit);
    std::vector<std::pair<std::string, std::string>> fed;
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("from", std::string()) == after && c.value("output", std::string()).empty() && c.value("to", std::string()) != node) {
            fed.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
        }
    }
    edit.connect(after, node, "strands");
    for (const auto& [to, input] : fed) edit.connect(node, to, input);
}

// nodes.py's set_script: a script node's edited file (checked); params its header no longer has go.
void setScript(GraphEdit& edit, const std::string& node, const json& definition) {
    const json errors = scriptErrors(definition);
    if (!errors.empty()) fail("script", errorsText(errors));
    if (!edit.hasNode(node) || edit.nodeType(node) != "Script") fail("bad_request", node + " is not a script node");
    json g = graphJsonOf(edit);
    json& n = nodeIn(g, node);
    json& params = n["params"];
    if (!params.is_object()) params = json::object();
    std::set<std::string> kept;
    for (const json& p : definition.value("params", json::array())) kept.insert(p.value("name", std::string()));
    if (params.contains("definition") && params["definition"].is_object()) {
        for (const json& p : params["definition"].value("params", json::array())) {
            const std::string name = p.value("name", std::string());
            if (!kept.count(name)) params.erase(name);
        }
    }
    params["definition"] = definition;
    edit = GraphEdit(g.dump());
    edit.completeParams();  // the new params at their defaults
}

json scriptDefinition(const GraphEdit& edit, const std::string& node) {
    if (!edit.hasNode(node) || edit.nodeType(node) != "Script") fail("bad_request", node + " is not a script node");
    json g = graphJsonOf(edit);
    const json& n = nodeIn(g, node);
    if (n.contains("params") && n["params"].is_object() && n["params"].contains("definition") && n["params"]["definition"].is_object()) {
        return n["params"]["definition"];
    }
    return json::object();
}

// presets.py's preset_data: the graph (the default graph if none) with its scalp's size, "about",
// the layout and backdrops.
json presetData(const Session& s, const json& about) {
    json graph = json::parse(graphTextOf(*s.groom));
    json data = json::object();
    if (graph.contains("version")) {
        data["version"] = graph["version"];
        graph.erase("version");
    }
    if (const std::optional<double> size = groomSizeOf(s)) data["size"] = *size;
    if (about.is_object()) {
        json a = json::object();
        for (const char* key : {"title", "description", "category"}) {
            if (about.contains(key) && about[key].is_string() && !about[key].get<std::string>().empty()) a[key] = about[key];
        }
        if (!a.empty()) data["about"] = a;
    }
    json backdrops = graph.contains("backdrops") ? graph["backdrops"] : json::array();
    graph.erase("backdrops");
    for (auto& [key, value] : graph.items()) data[key] = value;
    if (backdrops.is_array() && !backdrops.empty()) data["backdrops"] = backdrops;
    return data;
}

// ---- Correctives (shots.py; the app's CorrectivesPanel) -----------------------------------------

Correctives correctivesOf(const Session& s) {
    try {
        return Correctives::fromJson(s.groom->data().correctivesJson);
    } catch (const std::exception& e) {
        fail("state", std::string("the groom's correctives do not read: ") + e.what());
    }
}

CorrectiveLayer& layerOf(Correctives& c, const std::string& name) {
    CorrectiveLayer* l = c.layer(name);
    if (!l) fail("not_found", "no corrective layer named '" + name + "'");
    return *l;
}

double fpsOf(const Session& s) { return s.groom->framesPerSecond(); }
double frameOf(const Session& s, double seconds) { return secondsToFrame(seconds, fpsOf(s)); }
double secondsOf(const Session& s, double frame) { return frameToSeconds(frame, fpsOf(s)); }

// shots.py's _clean_name: letters, digits and _ (a host may name a property after it), not starting
// with a digit, unique ("_2", "_3"...).
std::string cleanLayerName(const Correctives& c, const std::string& wanted, const std::string& current = {}) {
    std::string base;
    std::string trimmed = wanted;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    for (const char ch : trimmed) base += std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ? ch : '_';
    if (base.empty()) base = "corrective";
    if (std::isdigit(static_cast<unsigned char>(base[0]))) base = "c" + base;
    std::string name = base;
    for (int n = 2; name != current && c.layer(name); ++n) name = base + "_" + std::to_string(n);
    return name;
}

bool allFrames(const CorrectiveLayer& l) { return l.weightCurve.empty() && l.holdBefore && l.holdAfter; }

json layerJson(const Session& s, const CorrectiveLayer& l) {
    json keys = json::array();
    for (const auto& [seconds, w] : l.weightCurve) keys.push_back({frameOf(s, seconds), w});
    json frame = nullptr;
    if (!l.weightCurve.empty()) {
        const auto peak = std::max_element(l.weightCurve.begin(), l.weightCurve.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        frame = frameOf(s, peak->first);
    } else if (!allFrames(l)) {
        frame = frameOf(s, l.time);
    }
    json j = {{"name", l.name},
              {"enabled", l.enabled},
              {"guides", l.offsets.size()},
              {"weight", l.weight},
              {"weightNow", l.enabled ? l.weightAt(s.seconds) : 0.0f},
              {"keyed", !l.weightCurve.empty()},
              {"keys", keys},
              {"allFrames", allFrames(l)},
              {"frame", frame},
              {"fadeIn", l.fadeIn * fpsOf(s)},
              {"fadeOut", l.fadeOut * fpsOf(s)},
              {"holdBefore", l.holdBefore},
              {"holdAfter", l.holdAfter}};
    if (l.slot >= 0) j["slot"] = l.slot;
    return j;
}

json targetJson(Session& s) {
    const std::string node = s.groom->sculptTarget(), layer = shotLayer(s);
    if (!node.empty()) return {{"kind", "node"}, {"name", node}};
    if (!layer.empty()) return {{"kind", "corrective"}, {"name", layer}};
    return {{"kind", "base"}, {"name", nullptr}};
}

json layersJson(Session& s) {
    json layers = json::array();
    for (const CorrectiveLayer& l : correctivesOf(s).layers) layers.push_back(layerJson(s, l));
    return layers;
}

// One undo step of the corrective stack (the app's editLayers): no layers left is no text.
json commitLayers(Session& s, const std::string& label, const Correctives& c, json r = json::object()) {
    const std::string text = c.layers.empty() ? std::string() : c.toJson();
    const bool changed = text != s.groom->data().correctivesJson;
    if (changed) commitTexts(s, label, {{StrokeField::Correctives, text}});
    r["changed"] = changed;
    r["layers"] = layersJson(s);
    r["target"] = targetJson(s);
    r["undo"] = undoStateJson(s);
    return r;
}

// A fix's fades in frames (shots.py: fade, or fade_in / fade_out; 6 frames when none is given).
void fadesOf(const json& args, double& in, double& out, double fallbackIn, double fallbackOut) {
    const double fade = optNumber(args, "fade", -1.0);
    in = optNumber(args, "fadeIn", fade >= 0.0 ? fade : fallbackIn);
    out = optNumber(args, "fadeOut", fade >= 0.0 ? fade : fallbackOut);
    if (!(in >= 0.0) || !(out >= 0.0)) fail("bad_request", "fades are frames, 0 or more");
}

bool retimes(const json& args) {
    for (const char* key : {"frame", "fade", "fadeIn", "fadeOut"}) {
        if (args.contains(key) && !args[key].is_null()) return true;
    }
    return false;
}

float weightArg(const json& args) {
    const double w = optNumber(args, "weight", 1.0);
    if (!std::isfinite(w) || w < 0.0) fail("bad_request", "weight is 0 or more");
    return static_cast<float>(w);
}

// corrective_new (shots.py's add_layer, the app's Add / Fix Frame): a new empty layer on top of the
// stack, made what brushes sculpt into. Every frame (allFrames, the default without fades), or a
// fix at `frame` (default: the session's) fading in and out over the fades in frames.
json correctiveNew(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    const double frame = args.contains("frame") && args["frame"].is_number() ? args["frame"].get<double>() : frameOf(s, s.seconds);
    bool fades = optBool(args, "fix", false);  // the app's Fix Frame: fades of 6 frames
    for (const char* key : {"fade", "fadeIn", "fadeOut"}) fades = fades || (args.contains(key) && !args[key].is_null());
    const bool all = optBool(args, "allFrames", !fades);
    CorrectiveLayer layer;
    const std::string base = optString(args, "name", all ? "sculpt" + std::to_string(c.layers.size() + 1)
                                                         : "shot" + std::to_string(std::lround(frame)));
    layer.name = cleanLayerName(c, base);
    layer.time = secondsOf(s, frame);
    layer.holdBefore = layer.holdAfter = all;
    if (all) {
        layer.fadeIn = layer.fadeOut = 0.5;
    } else {
        double in = 6.0, out = 6.0;
        fadesOf(args, in, out, 6.0, 6.0);
        layer.fadeIn = in / fpsOf(s);
        layer.fadeOut = out / fpsOf(s);
    }
    layer.weight = 1.0f;
    const std::string name = layer.name;
    c.layers.push_back(std::move(layer));
    commitTexts(s, "add corrective", {{StrokeField::Correctives, c.toJson()}});
    // What the next strokes sculpt into (shots.py's set_target): no Sculpt node then.
    s.groom->setSculptTarget({});
    setShotLayer(s, name);
    return {{"name", name}, {"layers", layersJson(s)}, {"target", targetJson(s)}, {"undo", undoStateJson(s)}};
}

// The weight curve's value at a time put in (a key), the curve kept sorted.
void keyAt(CorrectiveLayer& l, double seconds, float w) {
    auto& curve = l.weightCurve;
    for (auto& [t, v] : curve) {
        if (std::abs(t - seconds) < 1e-6) {
            v = w;
            return;
        }
    }
    curve.emplace_back(seconds, w);
    std::sort(curve.begin(), curve.end());
}

// corrective_set (shots.py's set_layer, the app's weight and checkbox): on or off, its weight (a
// keyed layer keyed at the session's time, as auto key does), its keys (Blender's fcurve sampled:
// [[frame, weight], ...]; [] drops them), every frame, or re-timed as a fix.
json correctiveSet(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    CorrectiveLayer& l = layerOf(c, needString(args, "name"));
    if (args.contains("enabled") && !args["enabled"].is_null()) {
        if (!args["enabled"].is_boolean()) fail("bad_request", "enabled is true or false");
        l.enabled = args["enabled"].get<bool>();
    }
    const bool weighted = args.contains("weight") && !args["weight"].is_null();
    if (args.contains("keys") && !args["keys"].is_null()) {
        const json& keys = args["keys"];
        if (!keys.is_array()) fail("bad_request", "keys is [[frame, weight], ...]");
        std::vector<std::pair<double, float>> curve;
        for (const json& k : keys) {
            if (!k.is_array() || k.size() != 2 || !k[0].is_number() || !k[1].is_number()) fail("bad_request", "keys is [[frame, weight], ...]");
            const double w = k[1].get<double>();
            if (!std::isfinite(w) || !std::isfinite(k[0].get<double>())) fail("bad_request", "keys hold finite numbers");
            curve.emplace_back(secondsOf(s, k[0].get<double>()), static_cast<float>(w));
        }
        std::sort(curve.begin(), curve.end());
        const bool keyed = !curve.empty() || !l.weightCurve.empty();
        if (curve.empty() && !l.weightCurve.empty()) l.weight = l.weightAt(s.seconds);
        l.weightCurve = std::move(curve);
        if (keyed) l.holdBefore = l.holdAfter = true;  // the curve holds past its ends; dropped, every frame
    }
    if (optBool(args, "allFrames", false)) {
        l.weightCurve.clear();
        l.holdBefore = l.holdAfter = true;
        l.weight = weighted ? weightArg(args) : 1.0f;
    } else if (retimes(args)) {
        const double at = args.contains("frame") && args["frame"].is_number() ? args["frame"].get<double>()
                          : !allFrames(l)                                     ? frameOf(s, l.time)
                                                                              : frameOf(s, s.seconds);
        double in = 6.0, out = 6.0;
        const bool wasFix = !allFrames(l) && l.weightCurve.empty();
        fadesOf(args, in, out, wasFix ? l.fadeIn * fpsOf(s) : 6.0, wasFix ? l.fadeOut * fpsOf(s) : 6.0);
        float peak = l.weight;
        if (!l.weightCurve.empty()) {
            peak = 0.0f;
            for (const auto& k : l.weightCurve) peak = std::max(peak, k.second);
        }
        l.weightCurve.clear();
        l.time = secondsOf(s, at);
        l.fadeIn = in / fpsOf(s);
        l.fadeOut = out / fpsOf(s);
        l.holdBefore = l.holdAfter = false;
        l.weight = weighted ? weightArg(args) : peak;
    } else if (weighted) {
        const float w = weightArg(args);
        if (!l.weightCurve.empty()) {
            keyAt(l, s.seconds, w);
        } else {
            l.weight = w;
        }
    }
    const std::string name = l.name;
    return commitLayers(s, "edit corrective", c, {{"name", name}});
}

json correctiveRename(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    const std::string name = needString(args, "name"), to = needString(args, "to");
    CorrectiveLayer& l = layerOf(c, name);
    if (to == name) return commitLayers(s, "rename corrective", c, {{"name", name}});
    const std::string clean = cleanLayerName(c, to, name);
    if (clean != to) {
        fail("bad_request", "'" + to + "' cannot name a corrective (taken, or not letters, digits and _): try '" + clean + "'");
    }
    l.name = to;
    const bool target = shotLayer(s) == name;
    json r = commitLayers(s, "rename corrective", c, {{"name", to}, {"renamed", name}});
    if (target) {
        setShotLayer(s, to);  // the target follows (shots.py: shotLayer renamed with it)
        r["target"] = targetJson(s);
    }
    return r;
}

json correctiveMove(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    const std::string name = needString(args, "name");
    layerOf(c, name);
    const double stepArg = optNumber(args, "step", 1.0);
    if (!std::isfinite(stepArg)) fail("bad_request", "step is a whole number (+1 up the stack, -1 down)");
    const long step = std::lround(stepArg);
    auto it = std::find_if(c.layers.begin(), c.layers.end(), [&](const CorrectiveLayer& l) { return l.name == name; });
    const long i = static_cast<long>(it - c.layers.begin());
    const long j = std::clamp(i + step, 0L, static_cast<long>(c.layers.size()) - 1);
    CorrectiveLayer moved = std::move(*it);
    c.layers.erase(it);
    c.layers.insert(c.layers.begin() + j, std::move(moved));
    return commitLayers(s, "move corrective", c, {{"name", name}, {"index", j}});
}

// shots.py's solo_layer: only this one on; when it already is the only one on, every one comes back.
json correctiveSolo(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    const std::string name = needString(args, "name");
    layerOf(c, name);
    const bool alone = std::all_of(c.layers.begin(), c.layers.end(), [&](const CorrectiveLayer& l) { return l.enabled == (l.name == name); });
    for (CorrectiveLayer& l : c.layers) l.enabled = alone || l.name == name;
    return commitLayers(s, "solo corrective", c, {{"name", name}, {"soloed", !alone}});
}

json correctiveDelete(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    const std::string name = needString(args, "name");
    layerOf(c, name);
    c.layers.erase(std::remove_if(c.layers.begin(), c.layers.end(), [&](const CorrectiveLayer& l) { return l.name == name; }), c.layers.end());
    return commitLayers(s, "remove corrective", c, {{"removed", name}});  // the target goes with it (tools.cpp pruneTargets)
}

// shots.py's clear_keys: the keys (or a fix's fades) dropped, the layer on every frame at its weight now.
json correctiveClearKeys(Session& s, const json& args) {
    Correctives c = correctivesOf(s);
    CorrectiveLayer& l = layerOf(c, needString(args, "name"));
    if (!allFrames(l)) {
        l.weight = l.weightAt(s.seconds);
        l.weightCurve.clear();
        l.holdBefore = l.holdAfter = true;
    }
    const std::string name = l.name;
    return commitLayers(s, "clear corrective keys", c, {{"name", name}});
}

// ---- Sculpt nodes (sculpts.py) ------------------------------------------------------------------

json sculptNodesJson(Session& s) {
    json nodes = json::array();
    json edits = json::parse(s.groom->data().sculptsJson.empty() ? std::string("{}") : s.groom->data().sculptsJson, nullptr, false);
    try {
        const GraphEdit edit(graphTextOf(*s.groom));
        for (const std::string& n : edit.nodeNames()) {
            if (edit.nodeType(n) != "Sculpt") continue;
            std::size_t guides = 0;
            if (edits.is_object() && edits.contains(n) && edits[n].is_object() && edits[n].contains("guides") && edits[n]["guides"].is_object()) {
                guides = edits[n]["guides"].size();
            }
            nodes.push_back({{"name", n}, {"editedGuides", guides}});
        }
    } catch (const std::exception&) {
    }
    return nodes;
}

json sculptTarget(Session& s, const json& args) {
    const bool node = args.contains("node") && !args["node"].is_null();
    const bool corrective = args.contains("corrective") && !args["corrective"].is_null();
    const bool base = optBool(args, "base", false);
    if (static_cast<int>(node) + static_cast<int>(corrective) + static_cast<int>(base) > 1) {
        fail("bad_request", "give one of node, corrective or base");
    }
    if (node) {
        const std::string name = needString(args, "node");
        const GraphEdit edit(graphTextOf(*s.groom));
        if (!edit.hasNode(name) || edit.nodeType(name) != "Sculpt") fail("not_found", "the groom has no Sculpt node named '" + name + "'");
        setShotLayer(s, {});  // brushes shape the node's guides: no corrective (sculpts.py)
        s.groom->setSculptTarget(name);
    } else if (corrective) {
        const std::string name = needString(args, "corrective");
        Correctives c = correctivesOf(s);
        layerOf(c, name);
        s.groom->setSculptTarget({});  // shots.py's set_target: no Sculpt node
        setShotLayer(s, name);
    } else if (base) {
        setShotLayer(s, {});
        s.groom->setSculptTarget({});
    }
    return {{"target", targetJson(s)}, {"sculptNodes", sculptNodesJson(s)}};
}

json sculptClear(Session& s, const json& args) {
    const std::string node = needString(args, "node");
    const std::string& stored = s.groom->data().sculptsJson;
    std::string text;
    try {
        text = clearSculptNode(stored, node);
    } catch (const std::exception& e) {
        fail("state", std::string("the Sculpt nodes' edits do not read: ") + e.what());
    }
    const bool cleared = text != stored;
    if (cleared) commitTexts(s, "clear sculpt edits", {{StrokeField::Sculpts, text}});
    return {{"cleared", cleared}, {"sculptNodes", sculptNodesJson(s)}, {"undo", undoStateJson(s)}};
}

// ---- Controls' values -----------------------------------------------------------------------------

json controlsList(Session& s) {
    const json found = controlsOf(json::parse(graphTextOf(*s.groom)));
    const std::map<std::string, float>& values = s.groom->data().controls;
    json controls = json::array();
    for (const auto& [name, what] : found.items()) {
        json c = what;
        c["name"] = name;
        const auto it = values.find(name);
        c["value"] = it != values.end() ? json(it->second) : what.value("default", json(0.0));
        c["set"] = it != values.end();
        controls.push_back(std::move(c));
    }
    json extra = json::object();  // values for names the graph no longer reads
    for (const auto& [name, v] : values) {
        if (!found.contains(name)) extra[name] = v;
    }
    return {{"controls", controls}, {"unused", extra}};
}

// set_controls: the controls' values now (Maya's ctl_ attributes; the add-on's sliders and their
// fcurves, given on each frame as set_time is). Not an undo step: the host's animation holds them.
json setControls(Session& s, const json& args) {
    const json& given = need(args, "controls");
    if (!given.is_object()) fail("bad_request", "controls is {name: value}");
    std::map<std::string, float> values = optBool(args, "replace", false) ? std::map<std::string, float>{} : s.groom->data().controls;
    for (const auto& [name, v] : given.items()) {
        if (v.is_null()) {
            values.erase(name);
            continue;
        }
        double d = 0.0;
        if (v.is_boolean()) {
            d = v.get<bool>() ? 1.0 : 0.0;
        } else if (v.is_number()) {
            d = v.get<double>();
        } else {
            fail("bad_request", "the control '" + name + "' takes a number (null: unset)");
        }
        if (!std::isfinite(d)) fail("bad_request", "the control '" + name + "' must be finite");
        values[name] = static_cast<float>(d);
    }
    const bool changed = values != s.groom->data().controls;
    if (changed) s.groom->setControls(values);
    return {{"changed", changed}, {"controls", s.groom->data().controls}};
}

}  // namespace

// ---- edit_graph's undoable ops ----------------------------------------------------------------------

bool runGraphAuthoring(Session& s, const std::string& op, const json& args, json& result) {
    static const std::set<std::string> kOps = {"group",          "ungroup",           "expose_param", "unexpose_param", "compound_expose",
                                               "compound_unexpose", "insert_on_link", "add_control",  "link_control",   "unlink_control"};
    if (!kOps.count(op)) return false;
    GraphEdit edit(graphTextOf(*s.groom));
    const std::string before = edit.toJson();
    json r = json::object();
    std::string label = op;
    if (op == "group") {
        // Every host's Group (Ctrl+G): the nodes into one Compound (graph_edit.h collapse).
        r["name"] = edit.collapse(namesOf(need(args, "names"), "names"), optString(args, "name", ""));
        label = "group nodes";
    } else if (op == "ungroup") {
        const std::string name = needString(args, "name");
        if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
        if (edit.nodeType(name) != "Compound") fail("bad_request", name + " is not a Compound");
        r["names"] = edit.expand(name);
        label = "ungroup";
    } else if (op == "expose_param") {
        exposeParamRequest(edit, needString(args, "name"), needString(args, "param"));
        label = "expose param";
    } else if (op == "unexpose_param") {
        unexposeParam(edit, needString(args, "name"), needString(args, "param"));
        label = "unexpose param";
    } else if (op == "compound_expose") {
        const std::string name = needString(args, "name");
        if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
        compoundExpose(edit, name, needString(args, "param"), needString(args, "target"));
        label = "expose compound param";
    } else if (op == "compound_unexpose") {
        const std::string name = needString(args, "name");
        if (!edit.hasNode(name)) fail("not_found", "no node '" + name + "'");
        compoundUnexpose(edit, name, needString(args, "target"));
        label = "unexpose compound param";
    } else if (op == "insert_on_link") {
        const std::string from = needString(args, "from"), to = needString(args, "to");
        if (!edit.hasNode(from)) fail("not_found", "no node '" + from + "'");
        if (!edit.hasNode(to)) fail("not_found", "no node '" + to + "'");
        insertOnLink(edit, from, to, needString(args, "input"), needString(args, "node"), optString(args, "output", ""));
        label = "insert node";
    } else if (op == "add_control") {
        r.update(addControl(s, edit, args));
        label = "control";
    } else if (op == "link_control") {
        r.update(linkControl(edit, args));
        label = "key param";
    } else {
        r.update(unlinkControl(s, edit, args));
        label = "remove animation";
    }
    result = commitGraph(s, label, before, edit, r);
    return true;
}

// ---- The other commands -----------------------------------------------------------------------------

bool isAuthoringCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {
        "set_controls", "controls_list", "nodes_folder", "node_file", "save_node", "import_node", "script_template", "new_script_node",
        "set_script", "script_errors", "save_script_node", "save_preset", "correctives_list", "corrective_new", "corrective_set",
        "corrective_rename", "corrective_move", "corrective_solo", "corrective_delete", "corrective_clear_keys", "sculpt_target",
        "sculpt_clear"};
    return kCommands.count(cmd) > 0;
}

json runAuthoringCommand(const std::string& cmd, const json& args, const std::function<Session&()>& session) {
    // ---- Controls ----
    if (cmd == "set_controls") return setControls(session(), args);
    if (cmd == "controls_list") return controlsList(session());

    // ---- The node library (node_library.h, script.h) ----
    if (cmd == "nodes_folder") {
        return {{"userDir", nodeUserDir()}, {"dirs", nodeSearchDirs()}, {"shippedDir", shippedNodeDir()}};
    }
    if (cmd == "node_file" || cmd == "save_node") {
        // nodes.py's node_file / save_node: Compound `node` as a .glnode (the header's fields as given,
        // the rest from its library copy or itself). Saved, the compound holds the saved copy (one
        // undo step), so it shows no update. The file stays on undo, as Maya's does.
        Session& s = session();
        const std::string node = needString(args, "node");
        const std::string text = nodeFileFromCompound(graphTextOf(*s.groom), node, headerOf(args).dump(), groomSizeOf(s));
        json file = json::parse(text);
        if (cmd == "node_file") return {{"text", text}, {"file", file}};
        const std::string path = saveNodeFile(text, optString(args, "dir", ""));
        refreshNodeLibrary();
        GraphEdit edit(graphTextOf(*s.groom));
        const std::string before = edit.toJson();
        edit.updateLibraryNode(node, groomSizeOf(s), file.value("id", std::string()));
        json r = commitGraph(s, "save node", before, edit,
                             {{"path", path}, {"id", file.value("id", std::string())}, {"version", file.value("version", 1)}, {"node", node}});
        return r;
    }
    if (cmd == "import_node") {
        // nodes.py's import_node: a .glnode copied (checked) into the user's folder or `dir`.
        const std::string path = importNodeFile(needString(args, "path"), optString(args, "dir", ""));
        refreshNodeLibrary();
        const LibraryNode n = readNodeFile(path);
        return {{"path", path}, {"id", n.id}, {"version", n.version}, {"label", n.label}, {"kind", n.kind}};
    }
    if (cmd == "script_template") {
        return {{"definition", scriptTemplate(optString(args, "label", "My Script"), optString(args, "author", ""))}};
    }
    if (cmd == "script_errors") {
        const json& d = need(args, "definition");
        return {{"errors", scriptErrors(d.is_string() ? d : json(d.dump()))}};
    }
    if (cmd == "new_script_node") {
        // nodes.py's add_script_node: a Script node from `definition` (the template by default),
        // fed by `after` and feeding what it fed. One undo step.
        Session& s = session();
        const json definition = args.contains("definition") && !args["definition"].is_null()
                                    ? definitionOf(args["definition"])
                                    : scriptTemplate(optString(args, "label", "My Script"), optString(args, "author", ""));
        const json errors = scriptErrors(definition);
        if (!errors.empty()) fail("script", errorsText(errors));
        GraphEdit edit(graphTextOf(*s.groom));
        const std::string before = edit.toJson();
        std::string name = optString(args, "name", "");
        if (name.empty()) name = edit.uniqueName(libraryNodeName(definition.value("label", std::string("Script"))));
        std::optional<double> x, y;
        if (args.contains("x") && args["x"].is_number()) x = args["x"].get<double>();
        if (args.contains("y") && args["y"].is_number()) y = args["y"].get<double>();
        const std::string made = edit.addNode("Script", name, json{{"definition", definition}}.dump(), x, y, groomSizeOf(s));
        feedAfter(edit, made, optString(args, "after", ""));
        return commitGraph(s, "add script node", before, edit, {{"name", made}, {"definition", definition}});
    }
    if (cmd == "set_script") {
        Session& s = session();
        GraphEdit edit(graphTextOf(*s.groom));
        const std::string before = edit.toJson();
        const std::string node = needString(args, "node");
        setScript(edit, node, definitionOf(need(args, "definition")));
        return commitGraph(s, "edit script node", before, edit, {{"node", node}});
    }
    if (cmd == "save_script_node") {
        // nodes.py's save_script_node: to the library one version past its copy there (asNew: under
        // its id as a new node, version 1); the node then holds the saved copy (one undo step).
        Session& s = session();
        GraphEdit edit(graphTextOf(*s.groom));
        const std::string before = edit.toJson();
        const std::string node = needString(args, "node");
        json definition = scriptDefinition(edit, node);
        const std::string id = definition.value("id", std::string());
        if (optBool(args, "asNew", false)) {
            if (libraryIds().count(id)) fail("bad_request", "the library has a node " + id + " already: give the new node another id");
            definition["version"] = 1;
        } else if (const int listed = libraryVersion(id); listed > 0) {
            definition["version"] = std::max(definition.value("version", 1), listed + 1);
        }
        const std::string path = saveNodeFile(definition.dump(2), optString(args, "dir", ""));
        refreshNodeLibrary();
        setScript(edit, node, definition);
        return commitGraph(s, "save script node", before, edit, {{"path", path}, {"node", node}, {"version", definition.value("version", 1)}});
    }
    if (cmd == "save_preset") {
        // presets.py's save_preset: the graph (with its layout and backdrops) as a user preset.
        Session& s = session();
        const std::string name = needString(args, "name");
        const std::string why = presetNameError(name);
        if (!why.empty()) fail("bad_request", why);
        const std::string dir = presetUserDir();
        if (dir.empty()) fail("state", "no user preset folder (no home folder known)");
        const fs::path path = fs::u8path(dir) / fs::u8path(name + ".json");
        std::error_code ec;
        if (fs::exists(path, ec) && !optBool(args, "overwrite", false)) {
            fail("exists", "preset '" + name + "' exists (overwrite: true replaces it)");
        }
        fs::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) fail("io", "cannot write " + path.generic_u8string());
        f << presetData(s, args.contains("about") ? args["about"] : json()).dump(2);
        f.close();
        if (!f) fail("io", "cannot write " + path.generic_u8string());
        return {{"path", path.generic_u8string()}, {"name", name}};
    }

    // ---- Correctives and Sculpt nodes ----
    if (cmd == "correctives_list") {
        Session& s = session();
        return {{"layers", layersJson(s)}, {"target", targetJson(s)}, {"sculptNodes", sculptNodesJson(s)}};
    }
    if (cmd == "corrective_new") return correctiveNew(session(), args);
    if (cmd == "corrective_set") return correctiveSet(session(), args);
    if (cmd == "corrective_rename") return correctiveRename(session(), args);
    if (cmd == "corrective_move") return correctiveMove(session(), args);
    if (cmd == "corrective_solo") return correctiveSolo(session(), args);
    if (cmd == "corrective_delete") return correctiveDelete(session(), args);
    if (cmd == "corrective_clear_keys") return correctiveClearKeys(session(), args);
    if (cmd == "sculpt_target") return sculptTarget(session(), args);
    if (cmd == "sculpt_clear") return sculptClear(session(), args);
    fail("unknown_command", "no command '" + cmd + "'");
}

}  // namespace groomlab::helper
