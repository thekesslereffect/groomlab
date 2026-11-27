// SPDX-License-Identifier: Apache-2.0
// The agent tools (agent_tools.h): each tool's schema and description, written for a language model,
// and what it does. The arguments are checked against the schema first (unknown names, missing
// ones, wrong types), then by each tool (ranges, names, paths inside the root).

#include "groomlab/editor/agent_tools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "groomlab/card_files.h"
#include "groomlab/display.h"
#include "groomlab/editor/agent_align.h"
#include "groomlab/editor/agent_canvas.h"
#include "groomlab/png.h"
#include "groomlab/editor/agent_expressions.h"
#include "groomlab/editor/agent_face.h"
#include "groomlab/editor/agent_grooming.h"
#include "groomlab/editor/agent_shoot.h"
#include "groomlab/editor/agent_photos.h"
#include "groomlab/editor/game_exports.h"
#include "groomlab/root_subdivision.h"
#include "groomlab/root_tubes.h"
#include "groomlab/usd_writer.h"
#include "groomlab/editor/guide_edits.h"
#include "groomlab/editor/map_edits.h"
#include "groomlab/flow.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/node_library.h"
#include "groomlab/presets.h"
#include "groomlab/schema.h"
#include "groomlab/tube.h"

namespace groomlab::agent {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxMeshBytes = 512ull << 20;
constexpr std::uintmax_t kMaxTextBytes = 16ull << 20;
constexpr std::size_t kMaxGuidesPerCall = 5000;
constexpr std::size_t kMaxGuidePoints = 200;
constexpr std::size_t kMaxScriptBytes = 64u << 10;
constexpr std::size_t kMaxExportFrames = 1000;
constexpr std::size_t kMaxPreviewGuides = 20000;
constexpr std::size_t kMaxLabelledGuides = 400;  // render_preview label_guides
constexpr std::size_t kMaxCheckpoints = 32;      // a groom's, checkpoint
constexpr std::size_t kMaxBatchCalls = 64;       // batch
constexpr std::size_t kMaxPairCrops = 12;        // suggest_pairs' pictures
constexpr std::size_t kMaxStrokePoints = 4000;
constexpr int kPhotoShown = 768;  // a photo's picture at most this many pixels across (default)
constexpr int kMaxSimFrames = 1000;    // frames one simulate or sim_cache call steps
// A stroke's radius when none is given, and a placed guide's length (where no guides are near to
// copy): shares of the scalp's size.
constexpr double kDefaultRadius = 0.12;
constexpr double kDefaultGuideLength = 0.4;
const char* const kSiteDocs = "https://getgroomlab.com/docs/";

// ---- Small helpers --------------------------------------------------------------------------------

double r4(double v) { return std::round(v * 1e4) / 1e4; }
json vec(const Vec3f& v) { return json::array({r4(v.x), r4(v.y), r4(v.z)}); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string str(const json& a, const char* key, const std::string& fallback = {}) {
    return a.contains(key) ? a[key].get<std::string>() : fallback;
}
double num(const json& a, const char* key, double fallback) { return a.contains(key) ? a[key].get<double>() : fallback; }
bool flag(const json& a, const char* key, bool fallback) { return a.contains(key) ? a[key].get<bool>() : fallback; }

std::string readText(const std::string& path, std::uintmax_t limit = kMaxTextBytes) {
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) throw ToolError("cannot read '" + path + "': no such file");
    if (size > limit) throw ToolError("'" + path + "' is bigger than this tool reads (" + std::to_string(limit >> 20) + " MB)");
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw ToolError("cannot read '" + path + "'");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) throw ToolError("cannot write '" + path + "'");
    out << text;
    out.close();
    if (!out) throw ToolError("cannot write '" + path + "'");
}

bool hasExtension(const std::string& path, const std::string& ext) {
    return lower(fs::u8path(path).extension().u8string()) == ext;
}

ToolResult textResult(const json& value) {
    ToolResult r;
    r.content.push_back({{"type", "text"}, {"text", value.dump(-1, ' ', false, json::error_handler_t::replace)}});
    return r;
}

// "node" or "node.output".
std::pair<std::string, std::string> splitSource(const std::string& source) {
    const auto dot = source.find('.');
    if (dot == std::string::npos) return {source, {}};
    return {source.substr(0, dot), source.substr(dot + 1)};
}

// A groom or node name an agent picks: letters, digits, _ - and . (a file name too).
void checkName(const std::string& name, const char* what) {
    if (name.empty() || name.size() > 64) throw ToolError(std::string(what) + " must be 1 to 64 characters");
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
            throw ToolError(std::string(what) + " '" + name + "' may hold only letters, digits, _, - and .");
        }
    }
}

// ---- Schemas --------------------------------------------------------------------------------------

json prop(const char* type, const std::string& description) { return {{"type", type}, {"description", description}}; }

json enumProp(const std::vector<std::string>& values, const std::string& description) {
    return {{"type", "string"}, {"enum", values}, {"description", description}};
}

json objectSchema(json properties, std::vector<std::string> required = {}) {
    if (!properties.is_object()) properties = json::object();
    json o = {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) o["required"] = required;
    return o;
}

const char* kGroomDescription = "The groom's name (as given to new_groom or open_groom). May be left out while one groom is open.";

json point3() { return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}}; }

json numberOrList(const std::string& description) {
    return {{"type", json::array({"number", "array"})}, {"items", {{"type", "number"}}}, {"description", description}};
}

json tubeProperties() {
    return {{"shape", prop("string", "The tube's cross-section: round, ribbon, teardrop, blade, or a custom shape of the groom's library.")},
            {"width", numberOrList("Half-width of the section in scene units: one number for the whole tube, or a list spaced evenly root to tip (taper).")},
            {"aspect", numberOrList("Section height over width: 1 as drawn, toward 0 a flat ribbon. One number or a list root to tip.")},
            {"twist_degrees", numberOrList("Turns the section about the guide, in degrees, at each sample (absolute, not accumulated). One number or a list root to tip.")}};
}

// guide_op's edit_tubes: an edit of guides' tubes (tube.h editedTube's keys).
json tubeEditProperties() {
    return {{"shape", prop("string", "The cross-section: round, ribbon, teardrop, blade, or a library shape of the groom.")},
            {"width", numberOrList("Half-width in scene units: one number, or a list root to tip.")},
            {"aspect", numberOrList("Height over width: 1 as drawn, toward 0 flat. One number or a list root to tip.")},
            {"twist", numberOrList("Turn of the section about the guide in radians (absolute). One number or a list root to tip.")},
            {"scaleWidth", prop("number", "Multiplies every width (after width), the taper kept.")},
            {"smooth", enumProp({"width", "aspect", "twist", "all"}, "Smooths those lists along the guide.")},
            {"passes", prop("integer", "Smoothing passes (default 3).")},
            {"midShape", prop("string", "A second shape the section blends to along the tube (\"\" none).")},
            {"tipShape", prop("string", "The shape at the tip (\"\" none).")},
            {"rootHold", prop("number", "0-1: how far the root's shape holds before blending.")},
            {"midHold", prop("number", "0-1: how far the mid shape holds.")},
            {"tipHold", prop("number", "0-1: how far the tip's shape holds.")},
            {"midAt", prop("number", "0-1: where along the tube the mid shape sits (0.5).")}};
}

// render_preview's camera: a photo's, or one placed by eye and target.
json cameraProperties() {
    const json point = {{"type", json::array({"array", "string"})}, {"items", {{"type", "number"}}},
                        {"description", "[x, y, z] or a landmark's name."}};
    return {{"photo", prop("string", "Through a reference photo's lined-up camera (its shape), to compare with the photo.")},
            {"eye", point}, {"target", point},
            {"focal_mm", prop("number", "The focal length on a 36 mm film back (default 50).")},
            {"width", prop("integer", "The image's width in pixels (default 768).")},
            {"height", prop("integer", "The image's height in pixels (default 768).")}};
}

// A point or a landmark's name.
json pointOrName(const std::string& description) {
    return {{"type", json::array({"array", "string"})}, {"items", {{"type", "number"}}}, {"description", description}};
}

// The checks every call gets from its schema: no unknown argument, the required ones there, each
// of the type its schema says (nulls count as left out).
json checkArguments(const json& schema, const json& arguments) {
    json args = json::object();
    for (const auto& [key, value] : arguments.items()) {
        if (!value.is_null()) args[key] = value;
    }
    const json& props = schema["properties"];
    for (const auto& [key, value] : args.items()) {
        if (props.contains(key)) continue;
        std::string names;
        for (const auto& [k, v] : props.items()) names += (names.empty() ? "" : ", ") + k;
        throw ToolError("unknown argument '" + key + "' (this tool takes: " + (names.empty() ? "nothing" : names) + ")");
    }
    for (const json& r : schema.value("required", json::array())) {
        if (!args.contains(r.get<std::string>())) throw ToolError("missing argument '" + r.get<std::string>() + "'");
    }
    for (const auto& [key, value] : args.items()) {
        const json& p = props[key];
        if (!p.contains("type")) continue;
        const json types = p["type"].is_array() ? p["type"] : json::array({p["type"]});
        bool fits = false;
        for (const json& t : types) {
            const std::string type = t.get<std::string>();
            fits = fits || (type == "string" && value.is_string()) || (type == "number" && value.is_number()) ||
                   (type == "integer" && (value.is_number_integer() ||
                                          (value.is_number_float() && std::floor(value.get<double>()) == value.get<double>()))) ||
                   (type == "boolean" && value.is_boolean()) || (type == "array" && value.is_array()) ||
                   (type == "object" && value.is_object());
        }
        if (!fits) throw ToolError("'" + key + "' must be " + (types.size() == 1 ? "a " + types[0].get<std::string>() : "a " + types.dump()) + ", not " + value.dump());
        if (p.contains("enum") && std::find(p["enum"].begin(), p["enum"].end(), value) == p["enum"].end()) {
            throw ToolError("'" + key + "' must be one of " + p["enum"].dump() + ", not " + value.dump());
        }
    }
    return args;
}

std::vector<float> floatList(const json& value, const char* what, float lo, float hi, float scale = 1.0f) {
    std::vector<float> out;
    const json list = value.is_array() ? value : json::array({value});
    if (list.empty() || list.size() > 64) throw ToolError(std::string(what) + " takes 1 to 64 numbers");
    for (const json& v : list) {
        if (!v.is_number()) throw ToolError(std::string(what) + " takes numbers, not " + v.dump());
        const double d = v.get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) {
            throw ToolError(std::string(what) + " must be between " + json(lo).dump() + " and " + json(hi).dump() + ", not " + v.dump());
        }
        out.push_back(static_cast<float>(d) * scale);
    }
    return out;
}

Vec3f toPoint(const json& v, const char* what) {
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) {
        throw ToolError(std::string(what) + " must be [x, y, z], not " + v.dump());
    }
    const Vec3f p(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) throw ToolError(std::string(what) + " must be finite");
    return p;
}

// A tube from its arguments over `base` (the guide's own, or a new guide's).
TubeProfile tubeFrom(const json& t, TubeProfile base, const GuideHierarchy& h) {
    if (t.contains("shape")) {
        const std::string name = t["shape"].get<std::string>();
        TubeShape shape;
        if (parseTubeShape(name, &shape)) {
            base.shape = shape;
            base.custom = nullptr;
        } else if (const CustomTubeShapePtr custom = h.tubeShape(name)) {
            base.custom = custom;
        } else {
            std::string names = "round, ribbon, teardrop, blade";
            for (const auto& [n, s] : h.tubeShapes()) {
                if (!isOwnedTubeShape(n)) names += ", " + n;
            }
            throw ToolError("no tube shape '" + name + "' (shapes: " + names + ")");
        }
    }
    if (t.contains("width")) base.width = floatList(t["width"], "width", 1e-6f, 1e5f);
    if (t.contains("aspect")) base.aspect = floatList(t["aspect"], "aspect", kMinTubeAspect, kMaxTubeAspect);
    if (t.contains("twist_degrees")) base.twist = floatList(t["twist_degrees"], "twist_degrees", -36000.0f, 36000.0f, 3.14159265f / 180.0f);
    return base;
}

// String params that name files (a texture map, curves to import) must lie inside the root too.
void checkParamPaths(const AgentFolders& w, const json& value) {
    if (value.is_object() || value.is_array()) {
        for (const json& v : value) checkParamPaths(w, v);
        return;
    }
    if (!value.is_string()) return;
    const std::string s = value.get<std::string>();
    if (s.empty() || s.size() > 1024) return;
    static const std::set<std::string> fileTypes{".png", ".jpg", ".jpeg", ".tif", ".tiff", ".exr", ".tx", ".tga", ".hdr",
                                                 ".abc", ".usd", ".usda", ".usdc", ".vrscene", ".obj", ".json", ".groom", ".ptx"};
    const bool looksLikeFile = s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
                               fileTypes.count(lower(fs::u8path(s).extension().u8string())) != 0;
    if (looksLikeFile && !w.insideRoot(s)) {
        throw ToolError("the file '" + s + "' is outside the folder this server may use (" + w.root +
                        "): copy it into that folder, or start the server with a --root that holds it");
    }
}

json nodeJsonOf(const json& graph, const std::string& name) {
    for (const json& n : graph.value("nodes", json::array())) {
        if (n.value("name", std::string()) == name) return n;
    }
    throw ToolError("no node '" + name + "' in the graph (get_graph lists them)");
}

// The spec of a node's param by name (null when it has none).
json paramSpecOf(const json& spec, const std::string& param) {
    for (const json& p : spec.value("params", json::array())) {
        if (p.value("name", std::string()) == param) return p;
    }
    return nullptr;
}

// A param spec without what a model need not read (false flags, empty strings and lists).
json trimParam(const json& p) {
    static const std::set<std::string> keep{"name", "label", "kind", "default", "value", "unit", "min", "max", "softMin", "softMax",
                                            "choices", "mappable", "description", "group", "advanced", "showWhen"};
    json out = json::object();
    for (const auto& [k, v] : p.items()) {
        if (!keep.count(k)) continue;
        if ((v.is_boolean() && !v.get<bool>()) || (v.is_string() && v.get<std::string>().empty()) || (v.is_array() && v.empty())) continue;
        out[k] = v;
    }
    return out;
}

json trimSpec(const json& spec) {
    json out = json::object();
    for (const char* k : {"type", "label", "categoryTitle", "description", "role"}) {
        if (spec.contains(k)) out[std::string(k) == "categoryTitle" ? "category" : k] = spec[k];
    }
    json inputs = json::array();
    for (const json& i : spec.value("inputs", json::array())) {
        json in = {{"name", i.value("name", std::string())}, {"type", i.value("type", std::string())}};
        if (i.value("optional", false)) in["optional"] = true;
        if (i.value("param", false)) in["param"] = true;
        inputs.push_back(in);
    }
    out["inputs"] = inputs;
    json outputs = json::array();
    for (const json& o : spec.value("outputs", json::array())) outputs.push_back(o.value("name", std::string()) + ": " + o.value("type", std::string()));
    out["outputs"] = outputs;
    json params = json::array();
    for (const json& p : spec.value("params", json::array())) {
        if (p.value("name", std::string()) == "definition") continue;
        params.push_back(trimParam(p));
    }
    out["params"] = params;
    return out;
}

// What a Compound's or a Script's "definition" says, short.
json summarizeDefinition(const std::string& type, const json& d) {
    json out = json::object();
    if (!d.is_object()) return out;
    for (const char* k : {"id", "label", "version"}) {
        if (d.contains(k)) out[k] = d[k];
    }
    if (type == "Script") {
        json names = json::array();
        for (const json& p : d.value("params", json::array())) names.push_back(p.value("name", std::string()));
        out["params"] = names;
        out["writes"] = d.value("writes", json::array());
        out["script"] = d.value("script", std::string());
    } else if (d.contains("graph") && d["graph"].is_object()) {
        json inner = json::array();
        for (const json& n : d["graph"].value("nodes", json::array())) inner.push_back(n.value("name", std::string()) + " (" + n.value("type", std::string()) + ")");
        out["nodes"] = inner;
    }
    return out;
}

// The input of `node` the strands come in through: its first non-param input.
std::string firstInput(const GraphEdit& e, const std::string& node) {
    const json spec = json::parse(e.nodeSpecJson(node));
    for (const json& i : spec.value("inputs", json::array())) {
        if (!i.value("param", false)) return i.value("name", std::string());
    }
    return {};
}

std::vector<json> connectionsOf(const GraphEdit& e) {
    std::vector<json> out;
    for (const json& c : json::parse(e.toJson()).value("connections", json::array())) out.push_back(c);
    return out;
}

// Puts `made` between `after` and what `after`'s first output fed.
void insertAfter(GraphEdit& e, const std::string& made, const std::string& after) {
    if (!e.hasNode(after)) throw ToolError("no node '" + after + "' to insert after");
    std::vector<std::pair<std::string, std::string>> consumers;
    for (const json& c : connectionsOf(e)) {
        if (c.value("from", std::string()) == after && c.value("output", std::string()).empty() && c.value("to", std::string()) != made) {
            consumers.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
        }
    }
    const json spec = json::parse(e.nodeSpecJson(made));
    bool joined = false;
    for (const json& i : spec.value("inputs", json::array())) {
        if (i.value("param", false)) continue;
        try {
            e.connect(after, made, i.value("name", std::string()));
            joined = true;
            break;
        } catch (const GraphError&) {
        }
    }
    if (!joined) throw ToolError("'" + made + "' has no input that takes what '" + after + "' gives (" + e.outputType(after) + ")");
    for (const auto& [to, input] : consumers) e.connect(made, to, input);
}

// Puts `made` before `before`: between it and what fed its first input.
void insertBefore(GraphEdit& e, const std::string& made, const std::string& before) {
    if (!e.hasNode(before)) throw ToolError("no node '" + before + "' to insert before");
    const std::string input = firstInput(e, before);
    if (input.empty()) throw ToolError("'" + before + "' has no input to insert before");
    std::string from, output;
    for (const json& c : connectionsOf(e)) {
        if (c.value("to", std::string()) == before && c.value("input", std::string()) == input) {
            from = c.value("from", std::string());
            output = c.value("output", std::string());
        }
    }
    if (!from.empty()) {
        const std::string in = firstInput(e, made);
        if (in.empty()) throw ToolError("'" + made + "' has no input");
        e.connect(from, made, in, output);
    }
    e.connect(made, before, input);
}

// Where a new node goes: after, before, explicit inputs, or (none given) before the Output when
// it fits there. Says how it was connected.
std::string placeNode(GraphEdit& e, const std::string& made, const json& args) {
    if (args.contains("after")) {
        insertAfter(e, made, str(args, "after"));
        return "after " + str(args, "after");
    }
    if (args.contains("before")) {
        insertBefore(e, made, str(args, "before"));
        return "before " + str(args, "before");
    }
    if (args.contains("inputs")) {
        for (const auto& [input, source] : args["inputs"].items()) {
            if (!source.is_string()) throw ToolError("inputs map an input name to \"node\" or \"node.output\"");
            const auto [from, output] = splitSource(source.get<std::string>());
            e.connect(from, made, input, output);
        }
        return "inputs connected";
    }
    const std::string output = e.outputNode();
    if (output.empty()) return "not connected";
    GraphEdit attempt = e;
    try {
        insertBefore(attempt, made, output);
        e = attempt;
        return "before " + output + " (the end of the chain)";
    } catch (const std::exception&) {
        return "not connected (connect it with connect)";
    }
}

// ---- Docs -------------------------------------------------------------------------------------------

struct DocPage {
    std::string slug, title, description, body;
};

std::vector<DocPage> readDocs(const std::string& dir) {
    std::vector<DocPage> pages;
    if (dir.empty()) return pages;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(dir), ec)) {
        if (!entry.is_regular_file() || lower(entry.path().extension().u8string()) != ".mdx") continue;
        DocPage page;
        page.slug = entry.path().stem().u8string();
        std::string text = readText(entry.path().u8string());
        // The front matter: title and description.
        if (text.rfind("---", 0) == 0) {
            const auto end = text.find("\n---", 3);
            if (end != std::string::npos) {
                std::istringstream fm(text.substr(3, end - 3));
                std::string line;
                while (std::getline(fm, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string key = line.substr(0, colon), value = line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(' '));
                    if (key == "title") page.title = value;
                    if (key == "description") page.description = value;
                }
                const auto bodyStart = text.find('\n', end + 1);
                text = bodyStart == std::string::npos ? std::string() : text.substr(bodyStart + 1);
            }
        }
        page.body = std::move(text);
        pages.push_back(std::move(page));
    }
    std::sort(pages.begin(), pages.end(), [](const DocPage& a, const DocPage& b) { return a.slug < b.slug; });
    return pages;
}

std::size_t countOf(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++n;
    return n;
}

// A splitmix64 stream: the same numbers on every machine.
struct Random {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

// ---- The groom -------------------------------------------------------------------------------------

// The groom's graph as JSON text (the default graph for an empty one) and its guide hierarchy's.
std::string graphText(AgentGroom& g) { return graphOrDefault(g.session().data().graphJson); }
std::string hierarchyText(AgentGroom& g) { return g.session().data().hierarchyJson; }

// The scalp size presets and new nodes are fitted to (presetScalpSize, the groom's own scale
// divided out); nullopt without a scalp.
std::optional<double> fitSize(AgentGroom& g) {
    GroomSession& s = g.session();
    const MeshPtr& scalp = s.rawScalp();
    std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    if (size && s.groomScale() > 0.0) *size /= s.groomScale();
    return size;
}

// An edit: `change` gets copies of the graph and the hierarchy JSON and changes them; what changed
// goes to the groom as one undo step named `label` (ToolError, nothing changed, when the graph
// would not load).
void edit(AgentGroom& g, const std::string& label, const std::function<void(std::string& graph, std::string& hierarchy)>& change) {
    const std::string oldGraph = graphText(g), oldHierarchy = hierarchyText(g);
    std::string graph = oldGraph, hierarchy = oldHierarchy;
    change(graph, hierarchy);
    if (graph == oldGraph && hierarchy == oldHierarchy) return;
    auto stroke = std::make_shared<StrokeEdit>();
    stroke->mode = "agent";
    if (graph != oldGraph) {
        stroke->set(StrokeField::Graph, graph);
        stroke->text(StrokeField::Graph).before = oldGraph;
        stroke->text(StrokeField::Graph).hasBefore = true;
    }
    if (hierarchy != oldHierarchy) {
        stroke->set(StrokeField::Guides, hierarchy);
        stroke->text(StrokeField::Guides).before = oldHierarchy;
        stroke->text(StrokeField::Guides).hasBefore = true;
    }
    GroomEdit step;
    step.label = label;
    step.stroke = std::move(stroke);
    g.commit(std::move(step));
}

// Evaluates the groom (at its frame) and says how it went: strand and point counts, bounds,
// errors, the nodes' notes, the time.
json summary(AgentGroom& g, float density = 1.0f, const std::string& viewNode = {}, bool timings = false) {
    ViewRequest request;
    request.density = density;
    request.viewNode = viewNode;
    request.time = frameToSeconds(g.frame, g.framesPerSecond());
    request.wantBounds = true;
    const auto t0 = std::chrono::steady_clock::now();
    const ViewResult& view = g.view(request);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    json out;
    out["strands"] = view.strands ? view.strands->strandCount() : 0;
    out["points"] = view.strands ? view.strands->pointCount() : 0;
    out["guides"] = view.hierarchy ? view.hierarchy->guides().size() : 0;
    if (density < 1.0f) out["density"] = density;
    if (!view.viewedNode.empty()) out["node"] = view.viewedNode;
    if (view.bounds.valid) out["bounds"] = {{"min", vec(view.bounds.lo)}, {"max", vec(view.bounds.hi)}};
    out["errors"] = view.errors;
    json notes = json::object();
    for (const auto& [node, note] : g.session().nodeNotes()) notes[node] = note;
    if (!notes.empty()) out["notes"] = notes;
    out["ms"] = static_cast<long long>(ms + 0.5);
    if (timings) {
        auto times = g.session().nodeTimes();
        std::sort(times.begin(), times.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
        json slow = json::object();
        for (std::size_t i = 0; i < times.size() && i < 12; ++i) slow[times[i].first] = std::round(times[i].second * 10.0) / 10.0;
        out["nodeMs"] = slow;
    }
    return out;
}

}  // namespace

// ==== The tools ======================================================================================

struct AgentTools::Impl {
    AgentTarget& target;

    struct Tool {
        std::string name, title, description;
        json schema;
        std::function<ToolResult(Impl&, const json&)> run;
    };
    std::vector<Tool> tools;

    explicit Impl(AgentTarget& on) : target(on) { define(); }

    const AgentFolders& files() const { return target.folders(); }

    // Ends a changing tool's answer: what the groom is now.
    json withSummary(AgentGroom& g, json out) {
        out["groom"] = g.name();
        out["summary"] = summary(g);
        return out;
    }

    // ---- discover ----
    ToolResult listNodes(const json& a) {
        const std::string category = lower(str(a, "category")), search = lower(str(a, "search"));
        struct Entry {
            int categoryOrder, order;
            std::string category;
            json node;
        };
        std::vector<Entry> entries;
        auto take = [&](const json& spec, bool library) {
            if (spec.value("hidden", false)) return;
            const std::string cat = spec.value("categoryTitle", spec.value("category", std::string()));
            if (!category.empty() && lower(cat) != category && lower(spec.value("category", std::string())) != category) return;
            const std::string text = lower(spec.value("type", std::string()) + " " + spec.value("label", std::string()) + " " +
                                           spec.value("description", std::string()) + " " + spec.value("keywords", std::string()));
            if (!search.empty() && text.find(search) == std::string::npos) return;
            json node = {{"type", spec.value("type", std::string())}, {"description", spec.value("description", std::string())}};
            if (spec.value("label", std::string()) != spec.value("type", std::string())) node["label"] = spec.value("label", std::string());
            if (library) node["library"] = true;
            entries.push_back({spec.value("categoryOrder", 999), spec.value("order", 9999), cat, node});
        };
        for (const json& spec : json::parse(operatorSpecsJson())) take(spec, false);
        for (const json& spec : json::parse(libraryNodesJson()).value("nodes", json::array())) take(spec, true);
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
            return x.categoryOrder != y.categoryOrder ? x.categoryOrder < y.categoryOrder : x.order < y.order;
        });
        json groups = json::array();
        for (const Entry& e : entries) {
            if (groups.empty() || groups.back()["category"] != e.category) groups.push_back({{"category", e.category}, {"nodes", json::array()}});
            groups.back()["nodes"].push_back(e.node);
        }
        return textResult({{"count", entries.size()}, {"categories", groups},
                           {"hint", "describe_node gives a type's params; library nodes (type node:<id>) add like any other."}});
    }

    ToolResult describeNode(const json& a) {
        if (a.contains("node")) {
            AgentGroom& g = target.groom(str(a, "groom"));
            const std::string name = str(a, "node");
            const std::string graphText = agent::graphText(g);
            const json node = nodeJsonOf(json::parse(graphText), name);
            json spec = trimSpec(json::parse(GraphEdit(graphText).nodeSpecJson(name)));
            const json values = node.value("params", json::object());
            for (json& p : spec["params"]) {
                const std::string pn = p.value("name", std::string());
                if (values.contains(pn)) p["value"] = values[pn];
            }
            spec["name"] = name;
            if (node.contains("params") && node["params"].contains("definition")) {
                spec["definition"] = summarizeDefinition(node.value("type", std::string()), node["params"]["definition"]);
            }
            return textResult(spec);
        }
        if (!a.contains("type")) throw ToolError("give a node type (list_nodes), or a groom's node by \"node\"");
        const std::string type = str(a, "type");
        if (!libraryNodeId(type).empty()) {
            GraphEdit e(R"({"version": 1, "nodes": [], "connections": []})");
            const std::string made = e.addNode(type);
            json spec = trimSpec(json::parse(e.nodeSpecJson(made)));
            spec["type"] = type;
            return textResult(spec);
        }
        std::vector<std::string> near;
        for (const json& spec : json::parse(operatorSpecsJson())) {
            const std::string t = spec.value("type", std::string());
            if (t == type) return textResult(trimSpec(spec));
            if (lower(t).find(lower(type)) != std::string::npos || lower(type).find(lower(t)) != std::string::npos) near.push_back(t);
        }
        std::string hint;
        for (const std::string& n : near) hint += (hint.empty() ? "" : ", ") + n;
        throw ToolError("no node type '" + type + "'" + (hint.empty() ? std::string(" (list_nodes lists them)") : " (did you mean " + hint + "?)"));
    }

    ToolResult listPresets(const json&) {
        json presets = json::array();
        for (const PresetInfo& p : groomlab::listPresets(files().presetDirs())) {
            json e = {{"name", p.name}, {"title", p.title}};
            if (!p.category.empty()) e["category"] = p.category;
            if (!p.description.empty()) e["description"] = p.description;
            presets.push_back(e);
        }
        return textResult({{"presets", presets}, {"hint", "new_groom or apply_preset take a preset by name."}});
    }

    std::vector<std::pair<std::string, std::string>> sampleMeshes() const {
        std::vector<std::pair<std::string, std::string>> out;  // (sample name, file)
        if (files().meshDir.empty()) return out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::u8path(files().meshDir), ec)) {
            if (!entry.is_regular_file() || lower(entry.path().extension().u8string()) != ".obj") continue;
            std::string stem = entry.path().stem().u8string();
            const auto underscore = stem.find('_');
            out.emplace_back(underscore == std::string::npos ? stem : stem.substr(0, underscore), entry.path().u8string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    ToolResult listSampleMeshes(const json&) {
        static const std::map<std::string, std::string> about{
            {"nika", "Nika, a human head (CC BY 4.0), about 25 units tall, Y-up, facing +Z: for hair."},
            {"manny", "Manny, a human head (CC BY 4.0), about 34 units tall, Y-up, facing +Z: for hair."},
            {"wolf", "A wolf's body, Y-up: for fur."},
            {"buddy", "Buddy, a cartoon character's body (CC BY 4.0), about 57 units tall, Y-up, facing +Z; the Maya/Blender "
                      "samples also ship it rigged."}};
        json meshes = json::array();
        for (const auto& [name, file] : sampleMeshes()) {
            json m = {{"name", name}, {"file", fs::u8path(file).filename().u8string()}};
            const auto it = about.find(name);
            if (it != about.end()) m["description"] = it->second;
            meshes.push_back(m);
        }
        return textResult({{"meshes", meshes}, {"hint", "new_groom takes a sample by name as its mesh."}});
    }

    ToolResult getDocs(const json& a) {
        const std::vector<DocPage> pages = readDocs(files().docsDir);
        if (pages.empty()) throw ToolError("the docs are not installed beside groomlab-cli; read them at " + std::string(kSiteDocs));
        const std::string topic = lower(str(a, "topic"));
        if (topic.empty()) {
            json list = json::array();
            for (const DocPage& p : pages) list.push_back({{"topic", p.slug}, {"title", p.title}, {"description", p.description}});
            return textResult({{"pages", list}, {"hint", "get_docs with a topic (a page's name, or words) gives its text."}});
        }
        auto normal = [](std::string s) {
            std::replace(s.begin(), s.end(), '_', '-');
            std::replace(s.begin(), s.end(), ' ', '-');
            return s;
        };
        const DocPage* best = nullptr;
        std::vector<std::pair<double, const DocPage*>> scored;
        for (const DocPage& p : pages) {
            if (normal(topic) == p.slug) best = &p;
        }
        if (!best) {
            std::vector<std::string> words;
            std::istringstream in(topic);
            for (std::string word; in >> word;) {
                if (word.size() >= 3) words.push_back(word);
            }
            for (const DocPage& p : pages) {
                const std::string title = lower(p.title), description = lower(p.description), body = lower(p.body);
                double score = 0.0;
                for (const std::string& word : words) {
                    if (title.find(word) != std::string::npos) score += 5.0;
                    if (description.find(word) != std::string::npos) score += 3.0;
                    score += 0.6 * static_cast<double>(std::min<std::size_t>(countOf(body, "# " + word), 5));
                    score += 0.1 * static_cast<double>(std::min<std::size_t>(countOf(body, word), 20));
                }
                if (score > 0.0) scored.emplace_back(score, &p);
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            if (scored.empty()) throw ToolError("no docs page matches '" + topic + "' (get_docs without a topic lists them)");
            best = scored.front().second;
        }
        const std::size_t limit = static_cast<std::size_t>(std::clamp(num(a, "max_chars", 40000.0), 1000.0, 200000.0));
        json out = {{"topic", best->slug}, {"title", best->title}, {"url", std::string(kSiteDocs) + best->slug},
                    {"text", best->body.substr(0, limit)}};
        if (best->body.size() > limit) out["truncated"] = true;
        json others = json::array();
        for (std::size_t i = 1; i < scored.size() && i < 5; ++i) others.push_back(scored[i].second->slug);
        if (!others.empty()) out["see_also"] = others;
        return textResult(out);
    }

    // ---- build ----
    std::string meshPath(const std::string& mesh) {
        const bool plainName = mesh.find('/') == std::string::npos && mesh.find('\\') == std::string::npos && fs::u8path(mesh).extension().empty();
        if (plainName) {
            for (const auto& [name, file] : sampleMeshes()) {
                if (lower(mesh) == name || lower(mesh) == lower(fs::u8path(file).stem().u8string())) return file;
            }
            std::string names;
            for (const auto& [name, file] : sampleMeshes()) names += (names.empty() ? "" : ", ") + name;
            throw ToolError("no sample mesh '" + mesh + "' (samples: " + (names.empty() ? std::string("none installed") : names) +
                            "; or give a .obj or .abc file's path)");
        }
        const std::string path = files().resolve(mesh);
        std::error_code ec;
        const auto size = fs::file_size(fs::u8path(path), ec);
        if (ec) throw ToolError("no mesh file '" + mesh + "'");
        if (size > kMaxMeshBytes) throw ToolError("'" + mesh + "' is bigger than a scalp this server reads (512 MB)");
        return path;
    }

    json applyPreset(AgentGroom& g, const std::string& preset, const std::string& mode, const std::string& after) {
        std::string path;
        if (preset.find('/') != std::string::npos || preset.find('\\') != std::string::npos || hasExtension(preset, ".json")) {
            path = files().resolve(preset);
        } else {
            path = presetPath(preset, files().presetDirs());
            if (path.empty()) throw ToolError("no preset '" + preset + "' (list_presets lists them)");
        }
        const std::string text = readText(path);
        const json parsed = json::parse(text, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("nodes")) throw ToolError("'" + preset + "' is not a GroomLab preset (no \"nodes\")");
        const std::string stem = fs::u8path(path).stem().u8string();
        const std::optional<double> size = fitSize(g);
        json out = {{"preset", stem}, {"title", parsed.value("about", json::object()).value("title", presetTitle(stem))}, {"mode", mode}};
        if (mode == "insert") {
            std::string made;
            edit(g, "insert preset " + stem, [&](std::string& graph, std::string&) {
                graph = insertPresetJson(graph, text, size, after, presetNodeName(stem), &made);
            });
            out["node"] = made;
        } else {
            const std::string graph = applyPresetControlsToGraph(presetGraphJson(text, size), presetControlsJson(text, size));
            // The guide groups it reads, made empty when missing (a hand-placed look: add_guides fills them).
            json groupsMade = json::array();
            edit(g, "preset " + stem, [&](std::string& current, std::string& hierarchy) {
                current = graph;
                const std::vector<PresetGroup> groups = presetRequiredGroups(text);
                if (groups.empty()) return;
                GuideHierarchy h = hierarchyOf(g);
                for (const PresetGroup& group : groups) {
                    if (h.findGroup(group.name) != kNoGroup) continue;
                    h.addGroup(group.name);
                    groupsMade.push_back(group.name);
                }
                if (!groupsMade.empty()) hierarchy = h.toJson();
            });
            if (!groupsMade.empty()) {
                out["groupsMade"] = groupsMade;
                out["groupsNote"] = "Each guide added to these groups (add_guides with group) grows as the preset says: one feather per guide.";
            }
        }
        json missing = json::array();
        const MapCatalog& maps = g.session().data().maps;
        for (const PresetMap& m : presetRequiredMaps(text)) {
            if (!maps.count(m.name)) missing.push_back(m.name);
        }
        if (!missing.empty()) {
            out["missingMaps"] = missing;
            out["note"] = "The preset reads painted maps this groom lacks; they read as empty until painted in GroomLab (Maya or the app).";
        }
        return out;
    }

    ToolResult newGroom(const json& a) {
        if (const std::string why = target.opensNoGrooms(); !why.empty()) throw ToolError(why);
        const std::string name = str(a, "name");
        checkName(name, "a groom's name");
        if (target.find(name)) throw ToolError("a groom named '" + name + "' is open already: pick another name");
        const std::string path = meshPath(str(a, "mesh"));
        const std::shared_ptr<const ScalpMesh> mesh = target.readMesh(path, str(a, "object"));
        GroomFile file;
        file.data.scalp = mesh;
        file.data.graphJson = defaultGraphJsonFor(presetScalpSize(*mesh));
        file.source = fs::u8path(path).filename().u8string();
        AgentGroom& g = target.open(name, std::move(file));
        g.mesh = files().insideRoot(path) ? files().shown(path) : str(a, "mesh") + " (sample)";
        json out = {{"mesh", g.mesh}};
        Vec3f lo = mesh->points().empty() ? Vec3f() : mesh->points().front(), hi = lo;
        for (const Vec3f& p : mesh->points()) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        out["scalp"] = {{"points", mesh->pointCount()}, {"triangles", mesh->triangleCount()}, {"min", vec(lo)}, {"max", vec(hi)},
                        {"upAxis", "Y"}, {"units", "cm"}};
        const std::string scalpRegion = str(a, "scalp", "whole");
        if (scalpRegion != "whole" && scalpRegion != "from_landmarks") throw ToolError("scalp is \"whole\" or \"from_landmarks\"");
        try {
            if (a.contains("preset")) out["preset"] = applyPreset(g, str(a, "preset"), "replace", {});
            // A whole head (its face found): hair everywhere until density is painted; or above the
            // hairline the face's landmarks give.
            const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalpOf(g));
            if (face->head) {
                out["head"] = {{"face", true},
                               {"note", scalpRegion == "from_landmarks"
                                            ? "A whole head (face and neck): Scatter's density reads scalpRegion, 1 above the hairline the "
                                              "face's landmarks give, 0 on the face, ears and neck. Check it with render_preview; repaint "
                                              "it with paint_map."
                                            : "This mesh is a whole head (face and neck): hair grows over all of it until Scatter's density "
                                              "is painted. Run scalp_from_landmarks (or new_groom with scalp \"from_landmarks\") to keep "
                                              "the hair above the hairline the face's landmarks give."}};
                if (scalpRegion == "from_landmarks") out["scalpRegion"] = applyScalpRegion(g);
            } else if (scalpRegion == "from_landmarks") {
                throw ToolError("scalp from_landmarks needs a whole head with its face (" + face->why + ")");
            }
        } catch (...) {
            target.close(name);  // no half-made groom
            throw;
        }
        g.history().clear();  // the groom starts here
        return textResult(withSummary(g, out));
    }

    // The part of `scalp` whose triangles weigh 0.5 or more on average (its own points), null when
    // that is too little of it (under 20 triangles) or all of it.
    static MeshPtr capOf(const ScalpMesh& scalp, const std::vector<float>& weight) {
        std::vector<std::int32_t> index(scalp.pointCount(), -1);
        std::vector<Vec3f> points;
        std::vector<ScalpMesh::Triangle> triangles;
        for (const ScalpMesh::Triangle& t : scalp.triangles()) {
            const float w = (weight[static_cast<std::size_t>(t[0])] + weight[static_cast<std::size_t>(t[1])] + weight[static_cast<std::size_t>(t[2])]) / 3.0f;
            if (w < 0.5f) continue;
            ScalpMesh::Triangle n{};
            for (std::size_t k = 0; k < 3; ++k) {
                std::int32_t& at = index[static_cast<std::size_t>(t[k])];
                if (at < 0) {
                    at = static_cast<std::int32_t>(points.size());
                    points.push_back(scalp.points()[static_cast<std::size_t>(t[k])]);
                }
                n[k] = at;
            }
            triangles.push_back(n);
        }
        if (triangles.size() < 20 || triangles.size() + 20 >= scalp.triangleCount()) return nullptr;
        return std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
    }

    // The groom's lined-up photos as reference views (image, mask, mask strokes, camera).
    std::vector<ReferenceView> linedUpViews(const AgentGroom& g) {
        std::vector<ReferenceView> views;
        for (const AgentPhoto& p : photosFromJson(g.referencePhotos)) {
            if (!p.camera) continue;
            ReferenceView view;
            view.camera = *p.camera;
            view.image = loadImage(p.image, 3);
            if (!p.mask.empty()) view.mask = loadImage(p.mask, 1);
            view.maskStrokes = p.state.maskStrokes;
            views.push_back(std::move(view));
        }
        return views;
    }

    // The hair region's weight per point of a whole head (`scalp`): the landmarks' hairline, moved
    // where lined-up photos see the hair meet the face (within a band round the landmarks' guess,
    // the photos' hair masks on the head's points that face them). `photoPoints`: how many points
    // the photos set.
    std::vector<float> hairRegionOf(AgentGroom& g, const MeshPtr& scalp, const FaceLandmarks& face, float softness, std::size_t* photoPoints = nullptr) {
        const ScalpLandmarks marks = scalpLandmarks(*scalp, g.upAxis());
        std::vector<float> weight(scalp->pointCount());
        for (std::size_t i = 0; i < weight.size(); ++i) weight[i] = hairRegionWeight(face, marks, scalp->points()[i], softness);
        if (photoPoints) *photoPoints = 0;
        const std::vector<ReferenceView> views = linedUpViews(g);
        const MeshPtr cap = views.empty() ? nullptr : capOf(*scalp, weight);
        if (!cap) return weight;
        ReferenceGuideOptions options;
        options.up = upOf(g);
        std::vector<float> cover;
        try {
            cover = photoHairCover(*cap, *scalp, views, options);
        } catch (const std::exception&) {
            return weight;
        }
        // A pass of the neighbours' mean over the points the photos see (a mask's ragged edge).
        std::vector<std::vector<std::uint32_t>> next(scalp->pointCount());
        for (const ScalpMesh::Triangle& t : scalp->triangles())
            for (std::size_t k = 0; k < 3; ++k) {
                next[static_cast<std::size_t>(t[k])].push_back(static_cast<std::uint32_t>(t[(k + 1) % 3]));
                next[static_cast<std::size_t>(t[k])].push_back(static_cast<std::uint32_t>(t[(k + 2) % 3]));
            }
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> smooth = cover;
            for (std::size_t i = 0; i < cover.size(); ++i) {
                if (cover[i] < 0.0f) continue;
                float sum = cover[i];
                int n = 1;
                for (std::uint32_t j : next[i])
                    if (cover[j] >= 0.0f) sum += cover[j], ++n;
                smooth[i] = sum / static_cast<float>(n);
            }
            cover = std::move(smooth);
        }
        for (std::size_t i = 0; i < weight.size(); ++i) {
            if (cover[i] < 0.0f) continue;
            // (the band: half a face's third either side of the landmarks' hairline)
            const float wide = hairRegionWeight(face, marks, scalp->points()[i], 1.0f);
            if (wide <= 0.02f || wide >= 0.98f) continue;
            const float t = std::clamp((cover[i] - 0.5f) / 0.5f + 0.5f, 0.0f, 1.0f);
            weight[i] = t * t * (3.0f - 2.0f * t);
            if (photoPoints) ++*photoPoints;
        }
        return weight;
    }

    // A whole head's scalp from its face's landmarks: a vertex map (scalpRegion, or the one
    // Scatter's density reads already) 1 above the hairline, 0 on the face, ears and neck, read by
    // Scatter's density; with lined-up photos the hairline where they show it. One undo step.
    json applyScalpRegion(AgentGroom& g, float softness = 0.15f) {
        const MeshPtr scalp = scalpOf(g);
        const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
        if (!face->head) throw ToolError("no face found on the scalp (" + face->why + "): paint scatter.density with paint_map instead");
        GroomSession& session = g.session();
        const MeshPtr cage = session.cageScalp() ? session.cageScalp() : scalp;
        ScalpMaps maps = session.scalpMaps();
        const std::string oldMaps = maps.toJson();
        std::string graph = graphText(g);
        const std::string oldGraph = graph;
        GuideHierarchy h = hierarchyOf(g);
        h.evaluate(scalp.get());
        std::string scatter;
        for (const json& n : json::parse(graph).value("nodes", json::array())) {
            if (n.value("type", std::string()) == "Scatter") {
                scatter = n.value("name", std::string());
                break;
            }
        }
        if (scatter.empty()) throw ToolError("the graph has no Scatter node to read the scalp's region");
        ParamPaintRequest request;
        request.kind = "vertices";
        // (the map it reads already, else a new one: its value as it is, the map white over it)
        bool reads = false;
        try {
            reads = !paramMapOf(graph, maps, &h, scatter, "density").map.empty();
        } catch (const std::exception& e) {
            throw ToolError(std::string("Scatter's density: ") + e.what());
        }
        request.name = reads ? std::string() : std::string("scalpRegion");
        const MapFiles noFiles;
        const ParamPaint pp = paintParam(graph, maps, &h, cage.get(), scatter, "density", request, noFiles);
        if (pp.made && !pp.created.guideLayer) maps = pp.created.maps;
        if (!pp.graph.empty()) graph = pp.graph;
        auto entry = maps.maps.find(pp.map);
        if (entry == maps.maps.end() || entry->second.kind != "vertices") {
            throw ToolError("Scatter's density reads '" + pp.map + "', not a vertex map: paint it with paint_map instead");
        }
        const ScalpLandmarks marks = scalpLandmarks(*scalp, g.upAxis());
        const int channels = std::max(1, entry->second.channels);
        std::vector<float> values(cage->pointCount() * static_cast<std::size_t>(channels), 1.0f);
        std::size_t hair = 0, fromPhotos = 0;
        // (the refined scalp's points start with the cage's)
        const std::vector<float> region = hairRegionOf(g, scalp, *face, softness, &fromPhotos);
        for (std::size_t i = 0; i < cage->pointCount(); ++i) {
            const float w = i < region.size() && scalp->pointCount() >= cage->pointCount() ? region[i] : hairRegionWeight(*face, marks, cage->points()[i], softness);
            for (int c = 0; c < channels; ++c) values[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = c == 3 ? 1.0f : w;
            if (w > 0.5f) ++hair;
        }
        entry->second.values = std::move(values);
        AgentStrokeHost& host = g.strokeHost();
        host.clearStage();
        host.resetCommitted();
        host.label = "scalp from landmarks";
        if (maps.toJson() != oldMaps) host.stage(StrokeField::ScalpMaps, maps.toJson());
        if (graph != oldGraph) host.stage(StrokeField::Graph, graph);
        try {
            host.commit(StrokeEdit{});
        } catch (...) {
            host.clearStage();
            throw;
        }
        host.clearStage();
        json out = {{"map", pp.map}, {"linked", scatter + ".density"}, {"scalpShare", r4(static_cast<double>(hair) / std::max<std::size_t>(1, cage->pointCount()))},
                    {"hairline", "through the forehead's hairline_middle, the temples, over the ears and down to the nape"}};
        if (fromPhotos) {
            out["hairline"] = "the landmarks' (the forehead's hairline_middle, the temples, over the ears, the nape), moved where the "
                              "lined-up photos show the hair meet the face";
            out["photoPoints"] = fromPhotos;
        }
        return out;
    }

    ToolResult scalpFromLandmarks(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const double softness = num(a, "softness", 0.15);
        if (softness < 0.02 || softness > 1.0) throw ToolError("softness is 0.02 to 1 (a share of the face's lower third)");
        json out = applyScalpRegion(g, static_cast<float>(softness));
        out["hint"] = "Look with render_preview (front and side): hair should start at the forehead and stop above the ears; "
                      "paint_map on the same map fixes what the landmarks got wrong.";
        return textResult(withSummary(g, out));
    }

    // The look the photos suggest set on the graph: HairColor's melanin and redness, Clump's size and
    // strength, Curl's radius and frequency, Flyaway's fraction (each node the graph's first of its
    // type, added before the chain's color and width end when it has none). One undo step.
    // Locks: the guides carry each lock's curl (guides_from_photos on curly hair), so the strands clump
    // onto their own guide's curve (Clump centersFrom inputCurves, the guides its centers; strength per
    // strand from the guides' lockStrength) and keep the guides' curls where they blend (Interpolate's
    // keepCurls); no Curl node over them all, Noise low.
    struct LockLook {
        bool on = false;
        float radius = 0.0f, wavelength = 0.0f;
    };
    json applyPhotoLook(AgentGroom& g, const ReferenceLookFit& look, const LockLook& locks = {}) {
        std::vector<std::pair<std::string, json>> wants;
        wants.push_back({"HairColor", {{"mode", "melanin"}, {"melanin", r4(look.melanin)}, {"redness", r4(look.redness)}}});
        if (locks.on) {
            // (each lock a round lock about as thick as the photos' clumps are apart show it)
            json clump = {{"centersFrom", "inputCurves"}, {"strength", {{"attribute", "lockStrength"}}}, {"pastTip", "fit"}};
            if (look.clumped) clump["volumize"] = r4(0.35f * look.clumpSize);
            wants.push_back({"Clump", clump});
            wants.push_back({"Interpolate", {{"keepCurls", 1.0}, {"curlLength", r4(1.5f * locks.wavelength)}}});
            wants.push_back({"Noise", {{"amplitude", r4(0.3f * locks.radius)}}});
        } else {
            // (Clump's strength at most 0.5: the measure saturates at 1 on any clumped photo, and a whole
            // groom gathered that hard reads as spikes, not locks)
            if (look.clumped) {
                wants.push_back({"Clump", {{"clumpBy", "size"}, {"size", r4(look.clumpSize)}, {"strength", r4(std::min(0.5f, look.clumpStrength))}}});
            }
            if (look.curly) wants.push_back({"Curl", {{"radius", r4(look.curlRadius)}, {"frequency", r4(look.curlFrequency)}}});
        }
        if (look.flyawayFraction > 0.0f) wants.push_back({"Flyaway", {{"fraction", r4(look.flyawayFraction)}}});
        json done = json::object();
        edit(g, "apply photo look", [&](std::string& graph, std::string&) {
            auto firstOfType = [&](const std::string& type) {
                for (const json& n : json::parse(graph).value("nodes", json::array())) {
                    if (n.value("type", std::string()) == type) return n.value("name", std::string());
                }
                return std::string();
            };
            if (locks.on) {
                // (the curl comes from the guides: a Curl over every strand would frizz the locks)
                const std::string curl = firstOfType("Curl");
                if (!curl.empty()) {
                    GraphEdit e(graph);
                    e.removeNode(curl);
                    graph = e.toJson();
                    done["Curl"] = {{"node", curl}, {"removed", true}};
                }
            }
            for (const auto& [type, values] : wants) {
                auto firstOf = [&](const std::vector<std::string>& types) {
                    for (const json& n : json::parse(graph).value("nodes", json::array())) {
                        if (std::find(types.begin(), types.end(), n.value("type", std::string())) != types.end()) return n.value("name", std::string());
                    }
                    return std::string();
                };
                std::string node = firstOf({type});
                bool added = false;
                if (node.empty()) {
                    GraphEdit e(graph);
                    node = e.addNode(type, "", std::string(), std::nullopt, std::nullopt, fitSize(g));
                    const std::string anchor = type == "HairColor" ? firstOf({"Width", "Resample", "Output"}) : firstOf({"HairColor", "Width", "Resample", "Output"});
                    if (!anchor.empty()) {
                        GraphEdit attempt = e;
                        try {
                            insertBefore(attempt, node, anchor);
                            e = attempt;
                        } catch (const std::exception&) {
                        }
                    }
                    graph = e.toJson();
                    added = true;
                }
                const json spec = json::parse(GraphEdit(graph).nodeSpecJson(node));
                json gj = json::parse(graph);
                json set = json::object();
                for (json& n : gj["nodes"]) {
                    if (n.value("name", std::string()) != node) continue;
                    if (!n.contains("params") || !n["params"].is_object()) n["params"] = json::object();
                    for (const auto& [param, value] : values.items()) {
                        if (paramSpecOf(spec, param).is_null()) continue;
                        n["params"][param] = value;
                        set[param] = value;
                    }
                }
                graph = gj.dump();
                done[type] = {{"node", node}, {"set", set}, {"added", added}};
                if (locks.on && type == "Clump") {
                    // The guides as its centers' curves.
                    const std::string guides = firstOfType("Guides");
                    if (!guides.empty()) {
                        GraphEdit e(graph);
                        try {
                            e.connect(guides, node, "centers");
                            graph = e.toJson();
                            done[type]["centers"] = guides;
                        } catch (const std::exception& error) {
                            done[type]["centersError"] = error.what();
                        }
                    }
                }
            }
        });
        return done;
    }

    ToolResult morphToPhotosTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr before = scalpOf(g);
        AgentGroom::MorphRequest request;
        request.stiffness = num(a, "stiffness", 0.0);
        if (request.stiffness < 0.0 || request.stiffness > 0.95) throw ToolError("stiffness is 0 to 0.95");
        request.symmetric = flag(a, "symmetric", true);
        request.reset = flag(a, "reset", false);
        const AgentGroom::MorphOutcome outcome = g.morphToPhotos(request);
        const MeshPtr after = scalpOf(g);
        const std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        auto rmsOf = [&before](const AgentPhoto& p, const ScalpMesh& scalp) {
            const MarkerLists& marks = p.state.landmarks;
            double sum = 0.0;
            for (std::size_t k = 0; k < marks.pairCount(); ++k) {
                const Vec3f at = markerOn(*before, scalp, marks.point(kLandmarkMesh, k));
                const Vec3f& want = marks.point(kLandmarkPhoto, k).point;
                float u = 0.0f, v = 0.0f;
                if (!p.camera->project(at, u, v)) u = v = -1e4f;
                sum += (u - want.x) * (u - want.x) + (v - want.y) * (v - want.y);
            }
            return marks.pairCount() ? std::sqrt(sum / static_cast<double>(marks.pairCount())) : 0.0;
        };
        ToolResult r;
        json per = json::array();
        int shown = 0;
        for (const AgentPhoto& p : photos) {
            if (!p.camera || p.state.landmarks.pairCount() == 0) continue;
            per.push_back({{"photo", p.name}, {"pairs", p.state.landmarks.pairCount()}, {"rmsBefore", r4(rmsOf(p, *before))},
                           {"rmsAfter", r4(rmsOf(p, *after))}});
            if (shown++ >= 3) continue;
            AgentPhoto moved = p;
            for (std::size_t k = 0; k < moved.state.landmarks.pairCount(); ++k) {
                MarkerPoint m = moved.state.landmarks.point(kLandmarkMesh, k);
                m.point = markerOn(*before, *after, m);
                moved.state.landmarks.set(kLandmarkMesh, k, m);
            }
            const RefImage image = loadImage(p.image, 3);
            r.content.push_back({{"type", "image"}, {"data", base64Encode(photoOverlayPng(image, *after, moved, 512))}, {"mimeType", "image/png"}});
        }
        json info = {{"groom", g.name()}, {"reset", request.reset}, {"photos", per}};
        if (!request.reset) {
            if (outcome.pins > 0) info["pins"] = outcome.pins;
            if (outcome.pinsRmsBefore >= 0.0) info["pinsRms"] = {{"before", r4(outcome.pinsRmsBefore)}, {"after", r4(outcome.pinsRmsAfter)}};
            info["stiffness"] = request.stiffness;
            info["symmetric"] = request.symmetric;
        }
        if (!outcome.note.empty()) info["note"] = outcome.note;
        info["undoable"] = outcome.undoable;
        info["hint"] = request.reset ? std::string("the scalp as it was before the first morph")
                                     : std::string("the images: each lined-up photo with the morphed scalp's outline over it and its pairs "
                                                   "(green: the photo's, red: the mesh's). A morph always starts from the scalp before the "
                                                   "first one; reset puts that back") +
                                           (outcome.undoable ? "; undo takes this one back." : ".");
        r.content.push_back({{"type", "text"}, {"text", withSummary(g, info).dump(-1, ' ', false, json::error_handler_t::replace)}});
        return r;
    }

    ToolResult openGroom(const json& a) {
        if (const std::string why = target.opensNoGrooms(); !why.empty()) throw ToolError(why);
        const std::string path = files().resolve(str(a, "path"));
        if (!hasExtension(path, ".groom")) throw ToolError("open_groom reads .groom files");
        std::string name = str(a, "name", fs::u8path(path).stem().u8string());
        checkName(name, "a groom's name");
        if (target.find(name)) throw ToolError("a groom named '" + name + "' is open already: give another \"name\"");
        GroomFile file;
        try {
            file = groomFileFromJson(readText(path, 1ull << 30), fs::u8path(path).parent_path().u8string());
        } catch (const GroomFileError& e) {
            throw ToolError(std::string("cannot read the groom: ") + e.what());
        }
        if (!file.data.scalp) throw ToolError("the groom file has no scalp");
        // The caches it names are read too: they must lie inside the root as well.
        std::vector<std::string> refs{file.scalpCache.file, file.guideCache.file, file.driverCache.file};
        for (const auto& [node, ref] : file.simCaches) refs.push_back(ref.file);
        for (const std::string& ref : refs) {
            if (!ref.empty() && !files().insideRoot(ref)) throw ToolError("the groom reads '" + ref + "', outside the folder this server may use");
        }
        const json graph = json::parse(file.data.graphJson, nullptr, false);
        if (graph.is_object()) {
            for (const json& n : graph.value("nodes", json::array())) checkParamPaths(files(), n.value("params", json::object()));
        }
        // The vertex maps painted on it (the scalp's color sets), for paint_map to go on painting.
        ScalpMaps vertexMaps = scalpMapsFromGroomData(file.data);
        for (auto it = vertexMaps.maps.begin(); it != vertexMaps.maps.end();) {
            it = it->second.kind == "vertices" ? std::next(it) : vertexMaps.maps.erase(it);
        }
        AgentGroom& g = target.open(name, std::move(file));
        if (!vertexMaps.maps.empty()) g.session().setScalpMaps(std::move(vertexMaps));
        g.path = path;
        return textResult(withSummary(g, {{"path", files().shown(path)}, {"upAxis", g.upAxis()}, {"units", g.units()},
                                          {"nodes", json::parse(graphText(g)).value("nodes", json::array()).size()}}));
    }

    ToolResult listGrooms(const json&) {
        json list = json::array();
        for (AgentGroom* g : target.grooms()) {
            json e = {{"name", g->name()}, {"undoSteps", g->history().undoIndex()}, {"frame", g->frame}};
            if (!g->path.empty()) e["path"] = files().shown(g->path);
            if (!g->mesh.empty()) e["mesh"] = g->mesh;
            list.push_back(e);
        }
        return textResult({{"grooms", list}});
    }

    ToolResult applyPresetTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string mode = str(a, "mode", "replace");
        if (mode == "insert" && !a.contains("after")) throw ToolError("mode insert needs \"after\": the node the look goes after");
        return textResult(withSummary(g, applyPreset(g, str(a, "preset"), mode, str(a, "after"))));
    }

    ToolResult getGraph(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const json graph = json::parse(graphText(g));
        if (flag(a, "full", false)) return textResult(graph);
        std::map<std::string, json> inputs;
        for (const json& c : graph.value("connections", json::array())) {
            const std::string output = c.value("output", std::string());
            inputs[c.value("to", std::string())][c.value("input", std::string())] = c.value("from", std::string()) + (output.empty() ? "" : "." + output);
        }
        json nodes = json::array();
        for (const json& n : graph.value("nodes", json::array())) {
            const std::string name = n.value("name", std::string()), type = n.value("type", std::string());
            json entry = {{"name", name}, {"type", type}};
            json spec;
            try {
                spec = json::parse(nodeSpecJson(n.dump()));
            } catch (const std::exception&) {
            }
            json changed = json::object();
            const json params = n.value("params", json::object());
            for (const auto& [k, v] : params.items()) {
                if (k == "definition") {
                    entry["definition"] = summarizeDefinition(type, v);
                    continue;
                }
                const json ps = paramSpecOf(spec, k);
                if (ps.is_null() || !ps.contains("default") || ps["default"] != v) changed[k] = v;
            }
            if (!changed.empty()) entry["params"] = changed;
            if (n.value("bypass", false)) entry["bypass"] = true;
            if (n.contains("paramInputs")) entry["paramInputs"] = n["paramInputs"];
            if (n.contains("paramControls")) entry["paramControls"] = n["paramControls"];
            if (inputs.count(name)) entry["inputs"] = inputs[name];
            nodes.push_back(entry);
        }
        return textResult({{"groom", g.name()}, {"nodes", nodes}, {"output", graph.value("output", std::string())},
                           {"note", "params lists only values that differ from the node's defaults (describe_node shows them all)."}});
    }

    ToolResult addNode(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string type = str(a, "type");
        if (type == "Script") throw ToolError("add a script node with add_script_node");
        const json params = a.value("params", json::object());
        checkParamPaths(files(), params);
        if (a.contains("name")) checkName(str(a, "name"), "a node's name");
        std::string made, placed;
        edit(g, "add " + type, [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            made = e.addNode(type, str(a, "name"), params.empty() ? std::string() : params.dump(), std::nullopt, std::nullopt, fitSize(g));
            placed = placeNode(e, made, a);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"node", made}, {"placed", placed}}));
    }

    ToolResult removeNode(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const bool reconnect = flag(a, "reconnect", true);
        std::vector<std::string> rejoined;
        edit(g, "remove " + node, [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            if (!e.hasNode(node)) throw ToolError("no node '" + node + "' in the graph");
            if (node == e.outputNode()) throw ToolError("'" + node + "' is the graph's output: it cannot be removed");
            std::string from, output;
            std::vector<std::pair<std::string, std::string>> consumers;
            if (reconnect) {
                const std::string input = firstInput(e, node);
                for (const json& c : connectionsOf(e)) {
                    if (c.value("to", std::string()) == node && c.value("input", std::string()) == input) {
                        from = c.value("from", std::string());
                        output = c.value("output", std::string());
                    }
                    if (c.value("from", std::string()) == node) consumers.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
                }
            }
            e.removeNode(node);
            if (!from.empty()) {
                for (const auto& [to, input] : consumers) {
                    e.connect(from, to, input, output);
                    rejoined.push_back(to + "." + input);
                }
            }
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"removed", node}, {"reconnected", rejoined}}));
    }

    ToolResult connect(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const auto [from, output] = splitSource(str(a, "from"));
        const std::string out = a.contains("output") ? str(a, "output") : output;
        edit(g, "connect " + from + " -> " + str(a, "to"), [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            e.connectByHand(from, str(a, "to"), str(a, "input"), out);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"connected", from + (out.empty() ? "" : "." + out) + " -> " + str(a, "to") + "." + str(a, "input")}}));
    }

    ToolResult disconnect(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        edit(g, "disconnect " + str(a, "to") + "." + str(a, "input"), [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            if (!e.disconnect(str(a, "to"), str(a, "input"))) throw ToolError("nothing feeds " + str(a, "to") + "." + str(a, "input"));
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"disconnected", str(a, "to") + "." + str(a, "input")}}));
    }

    ToolResult setParams(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const json& params = a["params"];
        if (params.empty()) throw ToolError("no params given");
        checkParamPaths(files(), params);
        std::vector<std::string> set;
        edit(g, "set " + node, [&](std::string& graphText, std::string&) {
            json graph = json::parse(graphText);
            json* target = nullptr;
            for (json& n : graph["nodes"]) {
                if (n.value("name", std::string()) == node) target = &n;
            }
            if (!target) throw ToolError("no node '" + node + "' in the graph (get_graph lists them)");
            const json spec = json::parse(GraphEdit(graphText).nodeSpecJson(node));
            json& values = (*target)["params"];
            if (!values.is_object()) values = json::object();
            for (const auto& [param, value] : params.items()) {
                if (param == "definition") throw ToolError("change a script node's script with add_script_node and \"node\"");
                const json ps = paramSpecOf(spec, param);
                if (ps.is_null()) {
                    std::string names;
                    for (const json& p : spec.value("params", json::array())) {
                        if (p.value("name", std::string()) != "definition") names += (names.empty() ? "" : ", ") + p.value("name", std::string());
                    }
                    throw ToolError("'" + node + "' has no param '" + param + "' (its params: " + names + ")");
                }
                const std::string kind = ps.value("kind", std::string());
                const bool mapped = ps.value("mappable", false) && value.is_object();
                const bool fits = (kind == "bool" && value.is_boolean()) ||
                                  (kind == "int" && (value.is_number_integer() || mapped)) ||
                                  (kind == "float" && (value.is_number() || mapped)) || (kind == "string" && value.is_string()) ||
                                  (kind == "vec3" && ((value.is_array() && value.size() == 3) || value.is_object())) || kind == "ramp" ||
                                  kind == "map";
                if (!fits) {
                    throw ToolError(node + "." + param + " takes a " + kind + (ps.value("mappable", false) ? " (or a map object)" : "") +
                                    ", not " + value.dump());
                }
                if (kind == "string" && ps.contains("choices") && !ps["choices"].empty() &&
                    std::find(ps["choices"].begin(), ps["choices"].end(), value) == ps["choices"].end()) {
                    throw ToolError(node + "." + param + " is one of " + ps["choices"].dump() + ", not " + value.dump());
                }
                if (value.is_number()) {
                    const double v = value.get<double>();
                    if ((ps.contains("min") && v < ps["min"].get<double>()) || (ps.contains("max") && v > ps["max"].get<double>())) {
                        throw ToolError(node + "." + param + " must be within [" + (ps.contains("min") ? ps["min"].dump() : "-inf") + ", " +
                                        (ps.contains("max") ? ps["max"].dump() : "inf") + "], not " + value.dump());
                    }
                }
                // An expression is compiled first: its errors (with their place) come back now.
                const bool exprParam = (value.is_object() && value.contains("expression") && value["expression"].is_string()) ||
                                       (kind == "string" && param == "expression" && value.is_string());
                if (exprParam) {
                    const std::string text = value.is_object() ? value["expression"].get<std::string>() : value.get<std::string>();
                    const ExpressionCheck check = checkExpression(text, {}, nullptr, nullptr, 0);
                    if (!check.ok) throw ToolError(node + "." + param + ": the expression does not compile: " + check.error);
                }
                values[param] = value;
                set.push_back(param);
            }
            graphText = graph.dump();
        });
        return textResult(withSummary(g, {{"node", node}, {"set", set}}));
    }

    // The hierarchy of a groom, its scale set, to edit.
    GuideHierarchy hierarchyOf(AgentGroom& g) {
        GuideHierarchy h = GuideHierarchy::fromJson(hierarchyText(g));
        const double k = g.session().groomScale();
        if (k > 0.0) h.setScale(static_cast<float>(k));
        return h;
    }

    MeshPtr scalpOf(AgentGroom& g) {
        const MeshPtr scalp = g.session().scalp();
        if (!scalp || scalp->triangleCount() == 0) throw ToolError("the groom has no scalp");
        return scalp;
    }

    Vec3f upOf(AgentGroom& g) const { return g.upAxis() == "Z" ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0); }

    ToolResult addGuides(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const json& list = a["guides"];
        if (list.empty() || list.size() > kMaxGuidesPerCall) throw ToolError("give 1 to " + std::to_string(kMaxGuidesPerCall) + " guides");
        const std::string groupName = str(a, "group", "guides");
        const bool snap = flag(a, "snap", true);
        const MeshPtr scalp = scalpOf(g);
        const Vec3f down = -upOf(g);
        const Vec3f back = g.upAxis() == "Z" ? Vec3f(0, 1, 0) : Vec3f(0, 0, -1);
        Vec3f lo = scalp->points().front(), hi = lo;
        for (const Vec3f& p : scalp->points()) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const Vec3f middle = (lo + hi) * 0.5f;
        std::vector<GuideId> ids;
        edit(g, "add guides", [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            GroupId group = h.findGroup(groupName);
            if (group == kNoGroup) group = h.addGroup(groupName);
            if (h.groupDepth(group) != 0) throw ToolError("'" + groupName + "' is a child group: guides go into a root group");
            if (a.contains("tube")) checkArguments(objectSchema(tubeProperties()), a["tube"]);  // no unknown keys
            const TubeProfile tube = a.contains("tube") ? tubeFrom(a["tube"], TubeProfile{}, h) : TubeProfile{};
            for (std::size_t i = 0; i < list.size(); ++i) {
                const json item = list[i].is_array() ? json{{"points", list[i]}} : list[i];
                const std::string where = "guides[" + std::to_string(i) + "]";
                if (!item.is_object()) throw ToolError(where + " must be an object ({\"points\": ...} or {\"root\": ..., \"length\": ...}) or a list of points");
                std::vector<Vec3f> points;
                if (item.contains("points")) {
                    const json& pts = item["points"];
                    if (!pts.is_array() || pts.size() < 2 || pts.size() > kMaxGuidePoints) {
                        throw ToolError(where + ".points needs 2 to " + std::to_string(kMaxGuidePoints) + " points");
                    }
                    for (const json& p : pts) points.push_back(toPoint(p, (where + ".points").c_str()));
                    if (snap) {
                        const Vec3f shift = scalp->closestPoint(points.front()).position - points.front();
                        for (Vec3f& p : points) p += shift;
                    }
                } else if (item.contains("root")) {
                    const SurfacePoint at = scalp->closestPoint(toPoint(item["root"], (where + ".root").c_str()));
                    Vec3f dir = item.contains("direction") ? toPoint(item["direction"], (where + ".direction").c_str()) : at.normal;
                    if (dir.length() < 1e-9f) throw ToolError(where + ".direction must not be zero");
                    dir = dir.normalized();
                    if (!item.contains("length") || !item["length"].is_number() || !(item["length"].get<double>() > 0.0)) {
                        throw ToolError(where + " needs a positive \"length\" (scene units)");
                    }
                    const float length = item["length"].get<float>();
                    const int segments = item.value("segments", 8);
                    if (segments < 1 || segments > 64) throw ToolError(where + ".segments must be 1 to 64");
                    const float droop = item.value("droop", 0.0f);
                    if (droop < 0.0f || droop > 1.0f) throw ToolError(where + ".droop must be 0 to 1");
                    // Droop turns the direction toward `down` along the guide (most of the turn early, as
                    // hair bends near the root), in the plane of down and the guide's sideways part;
                    // a guide standing straight up falls away from the scalp's middle (else backward).
                    const float cosine = std::clamp(dir.dot(down), -1.0f, 1.0f);
                    Vec3f side = dir - down * cosine;
                    if (side.length() < 0.3f) {
                        Vec3f away = at.position - middle;
                        away = away - down * away.dot(down);
                        side = away.length() > 1e-6f ? away : back;
                    }
                    side = side.normalized();
                    const float angle = std::acos(cosine);  // from down
                    Vec3f p = at.position;
                    points.push_back(p);
                    for (int s = 0; s < segments; ++s) {
                        const float t = (static_cast<float>(s) + 0.5f) / static_cast<float>(segments);
                        const float turn = droop * (1.0f - (1.0f - t) * (1.0f - t));
                        const float bend = angle * (1.0f - turn);
                        const Vec3f d = down * std::cos(bend) + side * std::sin(bend);
                        p += d * (length / static_cast<float>(segments));
                        points.push_back(p);
                    }
                } else {
                    throw ToolError(where + " needs \"points\" or a \"root\" and \"length\"");
                }
                ids.push_back(h.addGuide(group, std::move(points), tube));
            }
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        return textResult(withSummary(g, {{"group", groupName}, {"ids", ids}}));
    }

    ToolResult getGuides(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const GuideHierarchy stored = GuideHierarchy::fromJson(hierarchyText(g));
        const HierarchyPtr live = g.session().hierarchy();  // rooted on the scalp
        const GuideHierarchy& h = live ? *live : stored;
        const std::string only = str(a, "group");
        const std::size_t limit = static_cast<std::size_t>(std::clamp(num(a, "limit", 100.0), 1.0, 2000.0));
        json groups = json::array();
        for (const GuideGroup& gr : h.groups()) {
            json e = {{"name", gr.name}, {"depth", h.groupDepth(gr.id)}, {"guides", h.guidesInGroup(gr.id).size()}};
            if (gr.parent != kNoGroup && h.group(gr.parent)) e["parent"] = h.group(gr.parent)->name;
            groups.push_back(e);
        }
        json guides = json::array();
        std::size_t total = 0;
        for (const HierarchyGuide& guide : h.guides()) {
            const GuideGroup* gr = h.group(guide.group);
            if (!only.empty() && (!gr || gr->name != only)) continue;
            ++total;
            if (guides.size() >= limit || guide.points.empty()) continue;
            float length = 0.0f;
            for (std::size_t i = 1; i < guide.points.size(); ++i) length += (guide.points[i] - guide.points[i - 1]).length();
            auto rounded = [](const std::vector<float>& values, double scale) {
                json list = json::array();
                for (const float v : values) list.push_back(r4(v * scale));
                return list;
            };
            const json t = {{"shape", guide.tube.shapeName()}, {"width", rounded(guide.tube.width, 1.0)},
                            {"aspect", rounded(guide.tube.aspect, 1.0)}, {"twist_degrees", rounded(guide.tube.twist, 180.0 / 3.14159265358979)}};
            json e = {{"id", guide.id}, {"group", gr ? gr->name : std::string()}, {"root", vec(guide.points.front())},
                      {"tip", vec(guide.points.back())}, {"length", r4(length)}, {"points", guide.points.size()}, {"tube", t}};
            if (guide.isChild()) e["parent"] = guide.parent;
            guides.push_back(e);
        }
        json out = {{"groom", g.name()}, {"groups", groups}, {"guides", guides}, {"count", total}};
        if (total > guides.size()) out["note"] = "the first " + std::to_string(guides.size()) + " guides are listed (raise limit)";
        return textResult(out);
    }

    ToolResult setTube(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        if (!a.contains("shape") && !a.contains("width") && !a.contains("aspect") && !a.contains("twist_degrees")) {
            throw ToolError("say what to set: shape, width, aspect or twist_degrees");
        }
        const MeshPtr scalp = scalpOf(g);
        std::size_t changed = 0;
        edit(g, "set tubes", [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            std::vector<GuideId> targets;
            if (a.contains("guides")) {
                for (const json& id : a["guides"]) {
                    if (!id.is_number_integer() || !h.guide(id.get<GuideId>())) throw ToolError("no guide " + id.dump() + " (get_guides lists them)");
                    targets.push_back(id.get<GuideId>());
                }
            } else if (a.contains("group")) {
                const GroupId group = h.findGroup(str(a, "group"));
                if (group == kNoGroup) throw ToolError("no guide group '" + str(a, "group") + "'");
                targets = h.guidesInGroup(group);
            } else {
                for (const HierarchyGuide& guide : h.guides()) targets.push_back(guide.id);
            }
            if (targets.empty()) throw ToolError("no guides to set (add_guides makes some)");
            for (const GuideId id : targets) h.setTube(id, tubeFrom(a, h.guide(id)->tube, h));
            changed = targets.size();
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        return textResult(withSummary(g, {{"guides", changed},
                                          {"note", "Tubes shape the hair where Interpolate's fillTubes is on (the tube_locks preset) and where children are made in them."}}));
    }

    ToolResult addScriptNode(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        json def;
        if (a.contains("definition")) {
            def = a["definition"];
        } else {
            if (!a.contains("script")) throw ToolError("give the node's \"script\" (or a whole .glnode \"definition\")");
            const std::string label = str(a, "label", "My Script");
            def = json::parse(scriptNodeTemplate(label));
            def["id"] = suggestNodeId(label, {});
            def["script"] = str(a, "script");
            def["params"] = a.value("params", json::array());
            if (a.contains("writes")) def["writes"] = a["writes"];
            if (a.contains("inputs")) def["inputs"] = a["inputs"];
            if (a.contains("description")) def["description"] = str(a, "description");
            if (a.contains("category")) def["category"] = str(a, "category");
        }
        if (def.value("script", std::string()).size() > kMaxScriptBytes) throw ToolError("the script is longer than 64 KB");
        // Authored on this groom: its numbers are this scalp's, nothing to fit.
        if (const std::optional<double> size = fitSize(g)) def["size"] = *size;
        const json errors = json::parse(scriptNodeErrorsJson(def.dump())).value("errors", json::array());
        if (!errors.empty()) {
            ToolResult r = textResult({{"errors", errors}, {"message", "The script node has errors (line and column of the script); nothing changed. Fix them and call add_script_node again."}});
            r.isError = true;
            return r;
        }
        const std::string existing = str(a, "node");
        std::string made = existing, placed;
        edit(g, existing.empty() ? "add script node" : "edit " + existing, [&](std::string& graph, std::string&) {
            if (!existing.empty()) {
                json doc = json::parse(graph);
                bool found = false;
                std::set<std::string> names;
                for (const json& p : def.value("params", json::array())) names.insert(p.value("name", std::string()));
                for (json& n : doc["nodes"]) {
                    if (n.value("name", std::string()) != existing) continue;
                    if (n.value("type", std::string()) != "Script") throw ToolError("'" + existing + "' is not a script node");
                    found = true;
                    json& params = n["params"];
                    const json old = params.value("definition", json::object());
                    for (const json& p : old.value("params", json::array())) {
                        if (!names.count(p.value("name", std::string()))) params.erase(p.value("name", std::string()));
                    }
                    params["definition"] = def;
                }
                if (!found) throw ToolError("no node '" + existing + "' in the graph");
                graph = doc.dump();
                placed = "updated in place";
                return;
            }
            GraphEdit e(graph);
            const std::string name = a.contains("name") ? str(a, "name") : e.uniqueName(libraryNodeName(def.value("label", std::string("My Script"))));
            checkName(name, "a node's name");
            made = e.addNode("Script", name, json{{"definition", def}}.dump(), std::nullopt, std::nullopt, fitSize(g));
            placed = placeNode(e, made, a);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"node", made}, {"placed", placed}}));
    }

    ToolResult saveNode(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const std::string graphText = agent::graphText(g);
        const json n = nodeJsonOf(json::parse(graphText), node);
        std::string text;
        if (n.value("type", std::string()) == "Script") {
            text = n.value("params", json::object()).value("definition", json::object()).dump(2);
        } else if (n.value("type", std::string()) == "Compound") {
            text = nodeFileFromCompound(graphText, node, "{}", fitSize(g));
        } else {
            throw ToolError("only script and compound nodes are saved as .glnode files");
        }
        checkNodeFile(text);
        const std::string id = json::parse(text).value("id", std::string("my.node"));
        const std::string path = files().resolveForWrite(str(a, "path", id + ".glnode"));
        if (!hasExtension(path, ".glnode")) throw ToolError("a node file ends in .glnode");
        writeText(path, text + "\n");
        return textResult({{"path", files().shown(path)}, {"id", id},
                           {"note", "Use it in GroomLab with Import Node... (Maya's Graph Editor, the app), or copy it into Documents/GroomLab/nodes."}});
    }

    ToolResult undoTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const int steps = static_cast<int>(num(a, "steps", 1));
        if (steps < 1 || steps > static_cast<int>(kMaxUndo)) throw ToolError("steps must be 1 to " + std::to_string(kMaxUndo));
        if (!g.history().canUndo()) throw ToolError("nothing to undo");
        std::vector<std::string> labels;
        for (int i = 0; i < steps && g.history().canUndo(); ++i) {
            labels.push_back(g.history().toUndo().title());
            const std::string error = g.undo();
            if (!error.empty()) throw ToolError("undo: " + error);
        }
        keepSelected(g);
        return textResult(withSummary(g, {{"undone", labels}, {"undoStepsLeft", g.history().undoIndex()}}));
    }

    // ---- check ----
    ToolResult evaluate(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        if (a.contains("frame")) g.frame = num(a, "frame", g.frame);
        const double density = num(a, "density", 1.0);
        if (density < 0.0 || density > 1.0) throw ToolError("density must be 0 to 1");
        json out = summary(g, static_cast<float>(density), str(a, "node"), true);
        out["groom"] = g.name();
        out["frame"] = g.frame;
        return textResult(out);
    }

    // ---- see ----
    // The groom drawn as it is now: its strands (and guides) at its frame.
    std::size_t lastLabelled = 0;  // the guides drawGroom labelled last
    PreviewImage drawGroom(AgentGroom& g, const json& a, PreviewOptions o, json& errors) {
        const double density = num(a, "density", 1.0);
        if (density < 0.0 || density > 1.0) throw ToolError("density must be 0 to 1");
        ViewRequest request;
        request.density = static_cast<float>(density);
        request.viewNode = str(a, "node");
        request.time = frameToSeconds(g.frame, g.framesPerSecond());
        const ViewResult& view = g.view(request);
        const StrandsPtr strands = view.placed();
        const MeshPtr scalp = view.scalp ? view.scalp : g.session().rawScalp();
        const HierarchyPtr guides = view.hierarchy;
        if (flag(a, "guides", false) && guides && !a.contains("label_guides")) {
            for (const HierarchyGuide& guide : guides->guides()) {
                if (guide.points.size() < 2) continue;
                if (o.guides.size() >= kMaxPreviewGuides) break;
                o.guides.push_back(guide.points);
            }
        }
        // Guides labelled with their IDs at their tips: all, a group's, or the selection's.
        if (a.contains("label_guides") && guides) {
            const json& which = a["label_guides"];
            GroupId group = kNoGroup;
            bool selected = false;
            if (which.is_string()) {
                if (which.get<std::string>() == "selection") {
                    selected = true;
                } else {
                    group = guides->findGroup(which.get<std::string>());
                    if (group == kNoGroup) throw ToolError("label_guides: no guide group '" + which.get<std::string>() + "'");
                }
            } else if (!which.is_boolean()) {
                throw ToolError("label_guides is true, a group's name or \"selection\"");
            }
            if (!which.is_boolean() || which.get<bool>()) {
                for (const HierarchyGuide& guide : guides->guides()) {
                    if (guide.points.size() < 2) continue;
                    if (group != kNoGroup && guide.group != group) continue;
                    if (selected && std::find(g.selection.begin(), g.selection.end(), guide.id) == g.selection.end()) continue;
                    if (o.guides.size() >= kMaxLabelledGuides) break;
                    o.guides.push_back(guide.points);
                    o.guideLabels.push_back(std::to_string(guide.id));
                }
            }
        }
        if (flag(a, "tubes", false) && guides) {
            const double k = g.session().groomScale();
            std::size_t drawn = 0;
            for (const HierarchyGuide* guide : activeTubeGuides(*guides, "", "freeze", g.tubeViewGroup, g.mixedView)) {
                if (++drawn > kMaxPreviewGuides / 4) break;
                appendTubeWire(o.tubeLines, *guide, k > 0.0 ? static_cast<float>(k) : 1.0f, 6, 2);
            }
        }
        errors = view.errors;
        lastLabelled = o.guideLabels.size();
        try {
            return target.preview(g, scalp.get(), strands.get(), o);
        } catch (const std::invalid_argument& e) {
            throw ToolError(e.what());
        }
    }

    ToolResult renderPreviewTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        std::string sizeNote;
        if (a.contains("frame")) g.frame = num(a, "frame", g.frame);
        PreviewOptions o;
        if (a.contains("views")) {
            o.views.clear();
            for (const json& v : a["views"]) {
                if (!v.is_string()) throw ToolError("views are names: front, back, left, right, side, three_quarter, top");
                o.views.push_back(v.get<std::string>());
            }
        }
        if (a.contains("turntable")) {
            if (a.contains("views")) throw ToolError("give views or turntable, not both");
            const int n = static_cast<int>(num(a, "turntable", 8));
            if (n < 2 || n > 12) throw ToolError("turntable takes 2 to 12 frames");
            o.views.clear();
            for (int i = 0; i < n; ++i) o.views.push_back("turn:" + json(360.0 * i / n).dump());
        }
        o.look = str(a, "look", "studio");
        // Beside a photo: through its camera, at its size.
        std::optional<AgentPhoto> besidePhoto;
        if (a.contains("beside")) {
            if (a.contains("camera") || a.contains("views") || a.contains("turntable")) throw ToolError("beside draws through the photo's own camera: give no camera, views or turntable");
            std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
            const AgentPhoto& p = photoNamed(photos, str(a, "beside"));
            if (!p.camera) throw ToolError("the photo '" + p.name + "' is not lined up yet (align_photo)");
            besidePhoto = p;
            o.camera = *p.camera;
        }
        if (a.contains("camera")) {
            const json& c = a["camera"];
            checkArguments(objectSchema(cameraProperties()), c);
            if (c.contains("photo")) {
                std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
                const AgentPhoto& p = photoNamed(photos, c["photo"].get<std::string>());
                if (!p.camera) throw ToolError("the photo '" + p.name + "' is not lined up yet (align_photo)");
                o.camera = *p.camera;
            } else {
                if (!c.contains("eye") || !c.contains("target")) throw ToolError("camera takes a photo, or eye and target");
                const ScalpLandmarks marks = landmarksOf(g);
                const Vec3f eye = pointOrLandmark(c["eye"], marks, "camera.eye"), at = pointOrLandmark(c["target"], marks, "camera.target");
                const int w = static_cast<int>(c.value("width", 768.0)), h = static_cast<int>(c.value("height", 768.0));
                if (w < 16 || h < 16 || w > 8192 || h > 8192) throw ToolError("camera width and height are 16 to 8192 pixels");
                const Vec3f z = (eye - at);
                if (z.length() < 1e-6f) throw ToolError("the camera's eye and target are the same point");
                const Vec3f back = z.normalized();
                Vec3f up = marks.frame.up;
                if (std::abs(up.dot(back)) > 0.99f) up = marks.frame.front;
                const Vec3f x = up.cross(back).normalized(), y = back.cross(x);
                RefCamera cam;
                cam.width = w;
                cam.height = h;
                cam.focalPx = focalMmToPixels(c.value("focal_mm", 50.0), w);
                auto part = [](const Vec3f& v, int r) { return static_cast<double>(r == 0 ? v.x : r == 1 ? v.y : v.z); };
                for (int r = 0; r < 3; ++r) cam.pose.rotation[static_cast<std::size_t>(r)] = {part(x, r), part(y, r), part(back, r)};
                cam.pose.position = {eye.x, eye.y, eye.z};
                o.camera = cam;
            }
        }
        // A camera's own size when no size is asked for (up to 1024 across; the shape kept).
        o.size = static_cast<int>(num(a, "size", o.camera ? std::min(1024.0, o.camera->width) : 512.0));
        if (o.camera && !a.contains("size") && o.camera->width > 1024.0) sizeNote = "the camera's image is drawn 1024 pixels across";
        o.maxStrands = static_cast<std::size_t>(std::clamp(num(a, "max_strands", 50000.0), 100.0, 300000.0));
        o.zUp = g.upAxis() == "Z";
        if (flag(a, "viewport", false)) {
            const std::optional<PreviewImage> shot = target.viewportImage(g, o.size);
            if (!shot) throw ToolError("viewport: this server has no viewport (it works headless); live in a host, its view as the artist sees it");
            ToolResult r;
            r.content.push_back({{"type", "image"}, {"data", base64Encode(shot->png)}, {"mimeType", "image/png"}});
            r.content.push_back({{"type", "text"}, {"text", json{{"groom", g.name()}, {"viewport", true}, {"width", shot->width},
                                                                 {"height", shot->height}}.dump()}});
            return r;
        }
        const bool compare = flag(a, "compare", false);
        if (compare && !g.history().canUndo()) throw ToolError("compare needs an edit to compare with: this groom has none to undo");
        json info = {{"groom", g.name()}, {"views", o.camera ? json::array({"camera"}) : json(o.views)}, {"look", o.look}};
        if (!sizeNote.empty()) info["note"] = sizeNote;
        if (o.camera) info["camera"] = cameraInfo(*o.camera);
        ToolResult r;
        // Before the last edit: the step taken back for the picture, then done again.
        if (compare) {
            const GroomEdit& last = g.history().toUndo();
            std::string error = g.apply(last, false);
            if (!error.empty()) throw ToolError("compare: " + error);
            json errors;
            PreviewImage before;
            try {
                before = drawGroom(g, a, o, errors);
            } catch (...) {
                g.apply(last, true);
                throw;
            }
            error = g.apply(last, true);
            if (!error.empty()) throw ToolError("compare: " + error);
            r.content.push_back({{"type", "image"}, {"data", base64Encode(before.png)}, {"mimeType", "image/png"}});
            info["images"] = {"before: the groom before '" + last.title() + "'", "after: the groom now"};
        }
        json errors;
        const PreviewImage image = drawGroom(g, a, o, errors);
        info["width"] = image.width;
        info["height"] = image.height;
        info["columns"] = image.columns;
        info["strands"] = image.strandsTotal;
        info["strandsDrawn"] = image.strandsDrawn;
        if (flag(a, "guides", false)) info["guidesDrawn"] = "orange lines, yellow roots";
        if (flag(a, "tubes", false)) info["tubesDrawn"] = "cyan wires" + std::string(g.mixedView.empty() ? "" : " (tube_levels view " + g.mixedView + ")");
        if (image.stride > 1) {
            info["note"] = "drew 1 in " + std::to_string(image.stride) + " strands (max_strands " + std::to_string(o.maxStrands) +
                           "): the preview looks thinner than the groom";
        }
        if (errors.is_array() && !errors.empty()) info["errors"] = errors;
        if (a.contains("save_to")) {
            const std::string path = files().resolveForWrite(str(a, "save_to"));
            if (!hasExtension(path, ".png")) throw ToolError("save_to must be a .png file");
            writeText(path, std::string(image.png.begin(), image.png.end()));
            info["saved"] = files().shown(path);
        }
        if (lastLabelled) info["guidesLabelled"] = {{"count", lastLabelled}, {"about", "each guide yellow-orange, its ID beside its tip (where the scalp does not hide it)"}};
        if (besidePhoto) {
            if (image.rgb.empty()) throw ToolError("beside: this host's preview gives no pixels to set beside the photo");
            const RefImage photo = loadImage(besidePhoto->image, 3).resized(image.width, image.height);
            const bool blended = a.contains("blend");
            const double blend = std::clamp(num(a, "blend", 0.5), 0.0, 1.0);
            const int w = image.width, h = image.height, outW = blended ? w : 2 * w;
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(outW) * static_cast<std::size_t>(h) * 3);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const float pv = photo.at(x, y, c) * 255.0f;
                        const float gv = static_cast<float>(image.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]);
                        if (blended) {
                            rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] =
                                static_cast<std::uint8_t>(std::lround(std::clamp((1.0 - blend) * pv + blend * gv, 0.0, 255.0)));
                        } else {
                            rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] =
                                static_cast<std::uint8_t>(std::lround(std::clamp(pv, 0.0f, 255.0f)));
                            rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) + static_cast<std::size_t>(x + w)) * 3 + static_cast<std::size_t>(c)] =
                                static_cast<std::uint8_t>(gv);
                        }
                    }
            const std::vector<std::uint8_t> png = encodePng(outW, h, 3, rgb);
            info["beside"] = besidePhoto->name;
            info["width"] = outW;
            info["images"] = blended ? json("the groom through the photo's camera blended over the photo (blend " + json(r4(blend)).dump() + ": 0 the photo, 1 the groom)")
                                     : json("left: the photo; right: the groom through its camera, the same size");
            if (a.contains("save_to")) {
                const std::string path = files().resolveForWrite(str(a, "save_to"));
                writeText(path, std::string(png.begin(), png.end()));
            }
            r.content.push_back({{"type", "image"}, {"data", base64Encode(png)}, {"mimeType", "image/png"}});
        } else {
            r.content.push_back({{"type", "image"}, {"data", base64Encode(image.png)}, {"mimeType", "image/png"}});
        }
        r.content.push_back({{"type", "text"}, {"text", info.dump()}});
        return r;
    }

    // ---- checkpoints ----
    struct Snapshot {
        std::array<std::string, kStrokeFields> texts;
        std::string photos;
        std::shared_ptr<const ScalpMesh> scalp;
        int order = 0;
    };
    std::map<std::string, std::map<std::string, Snapshot>> checkpoints;  // groom -> name -> its state
    int checkpointOrder = 0;

    json checkpointNames(const std::string& groom) const {
        json names = json::array();
        const auto it = checkpoints.find(groom);
        if (it == checkpoints.end()) return names;
        std::vector<std::pair<int, std::string>> ordered;
        for (const auto& [name, s] : it->second) ordered.push_back({s.order, name});
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [order, name] : ordered) names.push_back(name);
        return names;
    }

    ToolResult checkpointTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string name = str(a, "name");
        if (name.empty()) return textResult({{"groom", g.name()}, {"checkpoints", checkpointNames(g.name())}});
        if (name.size() > 64) throw ToolError("a checkpoint's name is at most 64 characters");
        Snapshot s;
        AgentStrokeHost& host = g.strokeHost();
        for (std::size_t i = 0; i < kStrokeFields; ++i) {
            const StrokeField field = static_cast<StrokeField>(i);
            if (field == StrokeField::ShotLayer) continue;
            s.texts[i] = host.storedText(field);
        }
        s.photos = g.referencePhotos;
        s.scalp = g.session().rawScalp();
        s.order = ++checkpointOrder;
        auto& mine = checkpoints[g.name()];
        if (mine.size() >= kMaxCheckpoints && !mine.count(name)) throw ToolError("at most " + std::to_string(kMaxCheckpoints) + " checkpoints a groom: restore or reuse a name");
        const bool replaced = mine.count(name) != 0;
        mine[name] = std::move(s);
        return textResult({{"groom", g.name()}, {"saved", name}, {"replaced", replaced}, {"checkpoints", checkpointNames(g.name())},
                           {"about", "the groom's graph, guides, parts, flow, correctives, maps, sculpts, reference photos and scalp, "
                                     "kept in this session (not saved with the groom): restore puts them back as one undo step"}});
    }

    ToolResult restoreTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string name = str(a, "name");
        const auto mine = checkpoints.find(g.name());
        if (mine == checkpoints.end() || !mine->second.count(name)) {
            throw ToolError("no checkpoint '" + name + "' for this groom (checkpoints: " + checkpointNames(g.name()).dump() + ")");
        }
        const Snapshot& s = mine->second.at(name);
        AgentStrokeHost& host = g.strokeHost();
        auto stroke = std::make_shared<StrokeEdit>();
        stroke->mode = "agent";
        json changed = json::array();
        for (std::size_t i = 0; i < kStrokeFields; ++i) {
            const StrokeField field = static_cast<StrokeField>(i);
            if (field == StrokeField::ShotLayer) continue;
            const std::string now = host.storedText(field);
            if (now == s.texts[i]) continue;
            stroke->set(field, s.texts[i]);
            stroke->text(field).before = now;
            stroke->text(field).hasBefore = true;
            changed.push_back(strokeFieldName(field));
        }
        GroomEdit step;
        step.label = "restore " + name;
        if (!changed.empty()) step.stroke = std::move(stroke);
        if (g.referencePhotos != s.photos) {
            step.side = true;
            step.referencePhotos = std::make_pair(g.referencePhotos, s.photos);
            changed.push_back("referencePhotos");
        }
        const std::shared_ptr<const ScalpMesh> scalpNow = g.session().rawScalp();
        if (s.scalp && scalpNow != s.scalp) {
            step.side = true;
            step.scalp = std::make_pair(scalpNow, s.scalp);
            changed.push_back("scalp");
        }
        if (changed.empty()) return textResult(withSummary(g, {{"restored", name}, {"changed", changed}, {"note", "the groom is as the checkpoint already"}}));
        g.commit(std::move(step));
        return textResult(withSummary(g, {{"restored", name}, {"changed", changed}, {"undo", "one step: undo takes the restore back"}}));
    }

    // ---- batch ----
    ToolResult batchTool(const json& a) {
        if (!a.contains("calls") || !a["calls"].is_array()) throw ToolError("calls is a list of {\"tool\": name, \"arguments\": {...}}");
        const json& calls = a["calls"];
        if (calls.empty() || calls.size() > kMaxBatchCalls) throw ToolError("calls holds 1 to " + std::to_string(kMaxBatchCalls) + " tool calls");
        const bool images = flag(a, "images", false);
        ToolResult r;
        json answers = json::array();
        int stopped = -1;
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const json& c = calls[i];
            if (!c.is_object() || !c.contains("tool") || !c["tool"].is_string()) throw ToolError("calls[" + std::to_string(i) + "] needs a \"tool\" name");
            const std::string name = c["tool"].get<std::string>();
            if (name == "batch") throw ToolError("a batch cannot hold a batch");
            const ToolResult one = runTool(name, c.value("arguments", json::object()));
            json texts = json::array();
            int pictures = 0;
            for (const json& item : one.content) {
                if (item.value("type", std::string()) == "text") {
                    const std::string text = item.value("text", std::string());
                    const json parsed = json::parse(text, nullptr, false);
                    texts.push_back(parsed.is_discarded() ? json(text) : parsed);
                } else if (item.value("type", std::string()) == "image") {
                    ++pictures;
                    if (images) r.content.push_back(item);
                }
            }
            json entry = {{"call", i}, {"tool", name}, {"ok", !one.isError}, {"answer", texts.size() == 1 ? texts[0] : texts}};
            if (pictures) entry["images"] = images ? json(pictures) : json(std::to_string(pictures) + " left out (images: true sends them)");
            answers.push_back(entry);
            if (one.isError) {
                stopped = static_cast<int>(i);
                break;
            }
        }
        json info = {{"calls", answers}, {"ran", answers.size()}, {"of", calls.size()}};
        if (stopped >= 0) {
            info["stoppedAt"] = stopped;
            info["note"] = "stopped at the first error: the calls after it did not run (the ones before it did)";
        }
        r.isError = stopped >= 0;
        r.content.push_back({{"type", "text"}, {"text", info.dump(-1, ' ', false, json::error_handler_t::replace)}});
        return r;
    }

    // ---- maps over the scalp ----
    ToolResult showMapTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        std::string text;
        if (a.contains("map")) {
            if (a.contains("expression")) throw ToolError("give map or expression, not both");
            const std::string component = str(a, "component", "luminance");
            text = "map(\"" + str(a, "map") + "\", \"" + component + "\")";
        } else if (a.contains("expression")) {
            text = str(a, "expression");
        } else {
            throw ToolError("give a map (its name) or an expression");
        }
        std::vector<std::pair<std::string, float>> constants;
        if (a.contains("variables")) {
            try {
                constants = parseExpressionVariables(a["variables"].dump());
            } catch (const std::exception& e) {
                throw ToolError(std::string("variables: ") + e.what());
            }
        }
        return scalpHeatmap(g, a, text, constants);
    }

    ToolResult scalpHeatmap(AgentGroom& g, const json& a, const std::string& text, const std::vector<std::pair<std::string, float>>& constants) {
        ViewRequest request;
        request.viewNode = str(a, "node");
        request.time = frameToSeconds(g.frame, g.framesPerSecond());
        const ViewResult& view = g.view(request);
        const MeshPtr scalp = view.scalp ? view.scalp : g.session().scalp();
        if (!scalp || scalp->triangleCount() == 0) throw ToolError("the groom has no scalp");
        std::vector<float> values;
        try {
            values = expressionFaceValues(text, constants, view.strands.get(), *scalp);
        } catch (const ExpressionError& e) {
            throw ToolError(std::string("the expression: ") + e.what());
        }
        float lo = values.empty() ? 0.0f : *std::min_element(values.begin(), values.end());
        float hi = values.empty() ? 1.0f : *std::max_element(values.begin(), values.end());
        const float shownLo = a.contains("min") ? static_cast<float>(num(a, "min", 0.0)) : lo;
        const float shownHi = a.contains("max") ? static_cast<float>(num(a, "max", 1.0)) : (hi > lo ? hi : lo + 1.0f);
        // A perceptual ramp (dark blue, teal, yellow), linear for the preview's shading.
        auto ramp = [](float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            const float stops[5][3] = {{0.05f, 0.03f, 0.30f}, {0.10f, 0.25f, 0.55f}, {0.05f, 0.55f, 0.50f}, {0.50f, 0.80f, 0.20f}, {1.0f, 0.95f, 0.20f}};
            const float f = t * 4.0f;
            const int i = std::min(3, static_cast<int>(f));
            const float u = f - static_cast<float>(i);
            std::array<float, 3> c{};
            for (int k = 0; k < 3; ++k) c[static_cast<std::size_t>(k)] = std::pow(stops[i][k] + (stops[i + 1][k] - stops[i][k]) * u, 2.2f);
            return c;
        };
        PreviewOptions o;
        o.look = "flat";
        o.size = static_cast<int>(num(a, "size", 512.0));
        o.zUp = g.upAxis() == "Z";
        o.views = {"front", "left", "back", "top"};
        if (a.contains("views")) {
            o.views.clear();
            for (const json& v : a["views"]) o.views.push_back(v.get<std::string>());
        }
        o.faceColors.resize(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) o.faceColors[i] = ramp((values[i] - shownLo) / std::max(1e-9f, shownHi - shownLo));
        PreviewImage image;
        try {
            image = renderPreview(scalp.get(), nullptr, o);
        } catch (const std::invalid_argument& e) {
            throw ToolError(e.what());
        }
        // The legend: the ramp along the bottom, its ends' values.
        RefImage shown;
        shown.width = image.width;
        shown.height = image.height;
        shown.channels = 3;
        shown.pixels.resize(image.rgb.size());
        for (std::size_t i = 0; i < image.rgb.size(); ++i) shown.pixels[i] = static_cast<float>(image.rgb[i]) / 255.0f;
        Canvas canvas = Canvas::ofImage(shown);
        const int barY = canvas.height() - 22, barX0 = 60, barX1 = canvas.width() - 60;
        for (int x = barX0; x < barX1; ++x) {
            const std::array<float, 3> c = ramp(static_cast<float>(x - barX0) / static_cast<float>(std::max(1, barX1 - barX0 - 1)));
            const Rgb shownColor{std::pow(c[0], 1.0f / 2.2f), std::pow(c[1], 1.0f / 2.2f), std::pow(c[2], 1.0f / 2.2f)};
            for (int y = barY; y < barY + 10; ++y) canvas.set(x, y, shownColor);
        }
        auto label = [](float v) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.3g", static_cast<double>(v));
            return std::string(buf);
        };
        canvas.text(4, barY, label(shownLo), {1.0f, 1.0f, 1.0f});
        canvas.text(barX1 + 4, barY, label(shownHi), {1.0f, 1.0f, 1.0f});
        ToolResult r;
        r.content.push_back({{"type", "image"}, {"data", base64Encode(canvas.png())}, {"mimeType", "image/png"}});
        double mean = 0.0;
        for (float v : values) mean += v;
        if (!values.empty()) mean /= static_cast<double>(values.size());
        json info = {{"groom", g.name()}, {"shown", text}, {"views", o.views}, {"values", {{"min", r4(lo)}, {"mean", r4(mean)}, {"max", r4(hi)}}},
                     {"legend", {{"min", r4(shownLo)}, {"max", r4(shownHi)}, {"colors", "dark blue (min), teal, green, yellow (max)"}}},
                     {"about", "the scalp from each view, each triangle colored by the value over it (the mean over the strands "
                               "rooted there, else its value at the triangle's middle)"}};
        r.content.push_back({{"type", "text"}, {"text", info.dump()}});
        return r;
    }

    // ---- photo pairs ----
    ToolResult suggestPairsTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        const AgentPhoto& p = photoNamed(photos, str(a, "photo"));
        const RefImage image = loadImage(p.image, 3);
        RefCamera camera;
        std::string from;
        if (a.contains("view")) {
            try {
                camera = viewCamera(*scalp, g.upAxis(), str(a, "view"), image.width, image.height, num(a, "focal_mm", 50.0));
            } catch (const std::invalid_argument& e) {
                throw ToolError(e.what());
            }
            from = "the view hint '" + str(a, "view") + "' (a guess: confirm each pick)";
        } else if (p.camera) {
            camera = *p.camera;
            if (camera.width > 0.0 && std::abs(camera.width - image.width) > 0.5) camera = camera.scaled(image.width / camera.width);
            from = "the photo's camera as lined up";
        } else {
            throw ToolError("the photo '" + p.name + "' is not lined up: give view (front, right, three_quarter_front_left...) to predict from");
        }
        const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
        if (!face->head) throw ToolError("no face found on the mesh (" + face->why + "): suggest_pairs names the face's landmarks");
        std::vector<std::string> names;
        if (a.contains("landmarks")) {
            for (const json& n : a["landmarks"]) names.push_back(n.get<std::string>());
        } else {
            for (const FaceLandmark& l : face->points) names.push_back(l.name);
        }
        const float diagonal = std::hypot(static_cast<float>(image.width), static_cast<float>(image.height));
        const float window = static_cast<float>(num(a, "window", a.contains("view") ? 0.06 * diagonal : 0.025 * diagonal));
        const int zoom = static_cast<int>(std::clamp(num(a, "zoom", 4.0), 1.0, 8.0));
        const Vec3f eye = camera.eye();
        ToolResult r;
        json pairs = json::array();
        for (const std::string& name : names) {
            const FaceLandmark* l = face->find(name);
            if (!l) throw ToolError("no landmark '" + name + "' (mesh_landmarks names them)");
            float u = 0, v = 0;
            json e = {{"landmark", name}, {"point", vec(l->point)}};
            // (hidden: the mesh in front of it from the camera)
            bool facing = true;
            {
                const Vec3f toPoint = l->point - eye;
                const float distance = toPoint.length();
                SurfacePoint hit;
                float t = 0.0f;
                if (distance > 1e-6f && scalp->intersectRay(eye, toPoint / distance, &hit, &t) && t < distance - 0.02f * distance) facing = false;
            }
            if (!camera.project(l->point, u, v) || u < 0.0f || v < 0.0f || u >= static_cast<float>(image.width) || v >= static_cast<float>(image.height)) {
                e["seen"] = false;
                e["why"] = "outside the photo from this camera";
                pairs.push_back(e);
                continue;
            }
            e["predicted"] = {r4(u), r4(v)};
            if (!facing) {
                e["seen"] = false;
                e["why"] = "the mesh hides it from this camera";
                pairs.push_back(e);
                continue;
            }
            const PairSnap snap = snapToFeature(image, u, v, window);
            e["seen"] = true;
            e["pixel"] = {r4(snap.u), r4(snap.v)};
            e["confidence"] = r4(snap.confidence);
            e["moved"] = r4(snap.moved);
            e["landmarkConfidence"] = r4(l->confidence);
            if (r.content.size() < kMaxPairCrops) {
                e["image"] = static_cast<int>(r.content.size());
                r.content.push_back({{"type", "image"}, {"data", base64Encode(pairCropPng(image, u, v, snap, window, zoom))}, {"mimeType", "image/png"}});
            }
            pairs.push_back(e);
        }
        json info = {{"groom", g.name()}, {"photo", p.name}, {"from", from}, {"window", r4(window)}, {"pairs", pairs},
                     {"about", "predicted: where the landmark lands through the camera; pixel: the strongest corner within the "
                               "window (nearer preferred), confidence how much it stands out (0: none, the prediction kept). Each "
                               "image: the photo round it zoomed, the prediction a yellow ring, the pick a green cross, a 10-pixel "
                               "grid labelled from the corner's pixel. Confirm or correct each, then align_photo with "
                               "{\"pixel\": pixel, \"point\": landmark}."}};
        r.content.push_back({{"type", "text"}, {"text", info.dump()}});
        return r;
    }

    // ---- deliver ----
    ToolResult saveGroom(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string path = files().resolveForWrite(str(a, "path"));
        if (!hasExtension(path, ".groom")) throw ToolError("a groom file ends in .groom");
        GroomFile file = g.toFile();
        // The maps painted as the file keeps them (every host's save).
        const MapCatalog catalog = file.data.maps;
        storeScalpMaps(file.data, g.session().scalpMaps(), catalog);
        try {
            writeGroomFile(path, file);
        } catch (const GroomFileError& e) {
            throw ToolError(e.what());
        }
        g.path = path;
        std::error_code ec;
        return textResult({{"groom", g.name()}, {"path", files().shown(path)}, {"bytes", fs::file_size(fs::u8path(path), ec)}});
    }

    ToolResult exportTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        ExportRequest o;
        o.path = files().resolveForWrite(str(a, "path"));
        if (a.contains("frames")) {
            const json& f = a["frames"];
            if (f.is_number()) {
                o.start = o.end = f.get<double>();
            } else {
                const std::string text = f.get<std::string>();
                char* rest = nullptr;
                o.start = std::strtod(text.c_str(), &rest);
                if (rest == text.c_str()) throw ToolError("frames is a frame (\"12\") or a range (\"1-24\")");
                o.end = o.start;
                if (*rest == '-') {
                    const char* second = rest + 1;
                    o.end = std::strtod(second, &rest);
                    if (rest == second) throw ToolError("frames is a frame (\"12\") or a range (\"1-24\")");
                }
                if (*rest != '\0') throw ToolError("frames is a frame (\"12\") or a range (\"1-24\")");
            }
        } else {
            o.start = o.end = g.frame;
        }
        o.step = num(a, "step", 1.0);
        o.density = static_cast<float>(num(a, "density", 1.0));
        o.scales = str(a, "scales", "mesh");
        if (o.step > 0.0 && (o.end - o.start) / o.step + 1.0 > static_cast<double>(kMaxExportFrames)) {
            throw ToolError("at most " + std::to_string(kMaxExportFrames) + " frames in one export");
        }
        const ExportReport report = target.exportCurves(g, o);
        return textResult({{"groom", g.name()}, {"path", files().shown(o.path)}, {"format", report.format},
                           {"frames", report.frames}, {"strandsPerFrame", report.frames ? report.strandTotal / report.frames : 0},
                           {"ms", static_cast<long long>(report.milliseconds + 0.5)}});
    }

    ToolResult sampleScalp(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const std::size_t count = static_cast<std::size_t>(std::clamp(num(a, "count", 40.0), 1.0, 500.0));
        const bool facing = a.contains("min_up");
        const float minUp = static_cast<float>(num(a, "min_up", -1.0));
        const Vec3f up = upOf(g);
        Vec3f boxLo(-1e30f, -1e30f, -1e30f), boxHi(1e30f, 1e30f, 1e30f);
        if (a.contains("box_min")) boxLo = toPoint(a["box_min"], "box_min");
        if (a.contains("box_max")) boxHi = toPoint(a["box_max"], "box_max");
        // Candidates spread over the area, then the farthest apart of them: an even spread.
        std::vector<double> cumulative;
        double total = 0.0;
        const auto& pts = scalp->points();
        for (const auto& t : scalp->triangles()) {
            const Vec3f& p0 = pts[static_cast<std::size_t>(t[0])];
            total += 0.5 * static_cast<double>((pts[static_cast<std::size_t>(t[1])] - p0).cross(pts[static_cast<std::size_t>(t[2])] - p0).length());
            cumulative.push_back(total);
        }
        Random rng{0x67726F6F6D6C6162ull};
        std::vector<std::pair<Vec3f, Vec3f>> candidates;
        for (std::size_t i = 0; i < count * 12 && total > 0.0; ++i) {
            const double pick = rng.unit() * total;
            const std::size_t face = static_cast<std::size_t>(std::lower_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
            float su = static_cast<float>(std::sqrt(rng.unit())), v = static_cast<float>(rng.unit());
            StrandRoot root;
            root.face = static_cast<std::int32_t>(std::min(face, cumulative.size() - 1));
            root.u = su * (1.0f - v);
            root.v = su * v;
            const Vec3f p = scalp->position(root), n = scalp->normal(root);
            if (facing && n.dot(up) < minUp) continue;
            if (p.x < boxLo.x || p.y < boxLo.y || p.z < boxLo.z || p.x > boxHi.x || p.y > boxHi.y || p.z > boxHi.z) continue;
            candidates.emplace_back(p, n);
        }
        if (candidates.empty()) throw ToolError("no scalp area matches (loosen min_up or the box)");
        std::vector<std::size_t> chosen;
        std::vector<float> nearest(candidates.size(), 1e30f);
        std::size_t next = 0;  // the highest candidate first
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            if (candidates[i].first.dot(up) > candidates[next].first.dot(up)) next = i;
        }
        while (chosen.size() < std::min(count, candidates.size())) {
            chosen.push_back(next);
            const Vec3f last = candidates[next].first;
            float farthest = -1.0f;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                nearest[i] = std::min(nearest[i], (candidates[i].first - last).length());
                if (nearest[i] > farthest) {
                    farthest = nearest[i];
                    next = i;
                }
            }
            if (farthest <= 0.0f) break;
        }
        json points = json::array();
        for (const std::size_t i : chosen) points.push_back({{"p", vec(candidates[i].first)}, {"n", vec(candidates[i].second)}});
        return textResult({{"groom", g.name()}, {"points", points}, {"upAxis", g.upAxis()},
                           {"hint", "add_guides takes these as roots: {\"root\": p, \"length\": ..., \"droop\": 0.5} grows a guide along n."}});
    }

    // ---- groom by hand ----
    // The groom's landmarks (agent_grooming.h) on the scalp as it is now.
    // The face's landmarks of a groom's scalp (computed once a scalp: a few thousand rays).
    std::map<std::string, std::shared_ptr<const FaceLandmarks>> faces;
    std::shared_ptr<const FaceLandmarks> faceOf(AgentGroom& g, const MeshPtr& scalp) {
        const std::string key = std::to_string(scalp->contentHash()) + g.upAxis();
        auto it = faces.find(key);
        if (it != faces.end()) return it->second;
        if (faces.size() > 16) faces.clear();
        return faces[key] = std::make_shared<const FaceLandmarks>(faceLandmarks(*scalp, g.upAxis()));
    }

    // The scalp's landmarks; on a whole head (a face found) its front_hairline and temples on the
    // face's hairline, and the face's own landmarks by name besides.
    ScalpLandmarks landmarksOf(AgentGroom& g) {
        const MeshPtr scalp = scalpOf(g);
        ScalpLandmarks marks = scalpLandmarks(*scalp, g.upAxis());
        const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
        if (!face->head) return marks;
        for (Landmark& l : marks.points) {
            const float angle = l.name == "front_hairline" ? 0.0f : l.name == "left_temple" ? 45.0f : l.name == "right_temple" ? -45.0f : 999.0f;
            if (angle > 360.0f) continue;
            l.point = hairlinePoint(*scalp, *face, marks, angle, &l.normal);
        }
        for (const FaceLandmark& f : face->points) {
            if (!marks.find(f.name)) marks.points.push_back({f.name, f.point, f.normal});
        }
        return marks;
    }

    // A point an agent gave: [x, y, z], or a landmark's name.
    Vec3f pointOrLandmark(const json& v, const ScalpLandmarks& marks, const char* what) {
        if (v.is_string()) {
            const std::string name = v.get<std::string>();
            if (const Landmark* l = marks.find(name)) return l->point;
            std::string names;
            for (const Landmark& n : marks.points) names += (names.empty() ? "" : ", ") + n.name;
            throw ToolError(std::string(what) + ": no landmark '" + name + "' (landmarks: " + names + "; or give [x, y, z])");
        }
        return toPoint(v, what);
    }

    // A stroke's path: its world points as given, or from / via / to along the scalp.
    std::vector<Vec3f> strokePath(const json& a, const ScalpMesh& scalp, const ScalpLandmarks& marks, float spacing) {
        if (a.contains("points")) {
            if (a.contains("from") || a.contains("to") || a.contains("via")) throw ToolError("give points, or from and to: not both");
            const json& list = a["points"];
            if (list.empty() || list.size() > kMaxStrokePoints) {
                throw ToolError("points takes 1 to " + std::to_string(kMaxStrokePoints) + " points");
            }
            std::vector<Vec3f> path;
            for (const json& p : list) path.push_back(pointOrLandmark(p, marks, "points"));
            return path;
        }
        if (!a.contains("from") || !a.contains("to")) throw ToolError("give the path: points, or from and to (landmarks or [x, y, z])");
        std::vector<Vec3f> stops{pointOrLandmark(a["from"], marks, "from")};
        if (a.contains("via")) {
            if (a["via"].size() > 16) throw ToolError("via takes at most 16 points");
            for (const json& p : a["via"]) stops.push_back(pointOrLandmark(p, marks, "via"));
        }
        stops.push_back(pointOrLandmark(a["to"], marks, "to"));
        const float offset = static_cast<float>(num(a, "offset", 0.0));
        if (!std::isfinite(offset) || std::abs(offset) > marks.size) throw ToolError("offset must be within the scalp's size");
        std::vector<Vec3f> path = scalpPath(scalp, marks.middle, stops, spacing, offset);
        if (path.size() > kMaxStrokePoints) throw ToolError("the path would take too many points: raise spacing");
        return path;
    }

    // The settings every stroke takes from its arguments (radius, strength, symmetry...).
    void strokeSettings(BrushToolSettings& s, const json& a, const ScalpLandmarks& marks) {
        s.brush.radius = static_cast<float>(num(a, "radius", kDefaultRadius * marks.size));
        if (!(s.brush.radius > 0.0f) || s.brush.radius > 4.0f * marks.size) throw ToolError("radius must be positive and within the scalp's size");
        s.brush.strength = static_cast<float>(num(a, "strength", 1.0));
        if (!std::isfinite(s.brush.strength) || std::abs(s.brush.strength) > 100.0f) throw ToolError("strength must be within -100 and 100");
        if (a.contains("hardness")) s.setHardness(static_cast<float>(num(a, "hardness", 0.0)));
        s.setSymmetry(str(a, "symmetry", "none"));
        s.brush.mirrorCenter = static_cast<float>(num(a, "symmetry_center", 0.0));
    }

    json changeJson(const GuideChange& c) {
        json out = {{"guidesTouched", c.touched}, {"guidesAdded", c.added}, {"guidesRemoved", c.removed}};
        if (c.touched > 0) {
            out["meanMove"] = r4(c.meanMove);
            out["maxMove"] = r4(c.maxMove);
            out["meanLengthBefore"] = r4(c.lengthBefore);
            out["meanLengthAfter"] = r4(c.lengthAfter);
        }
        return out;
    }

    // The selection less the guides gone (deleted, undone).
    void keepSelected(AgentGroom& g) {
        if (g.selection.empty()) return;
        const std::shared_ptr<const GuideHierarchy> h = g.session().authoredHierarchy();
        std::vector<GuideId> kept;
        for (const GuideId id : g.selection) {
            if (h && h->guide(id)) kept.push_back(id);
        }
        g.selection = std::move(kept);
    }

    // The guides as authored now (a copy: the next edit replaces them).
    GuideHierarchy authored(AgentGroom& g) {
        const std::shared_ptr<const GuideHierarchy> h = g.session().authoredHierarchy();
        return h ? *h : GuideHierarchy{};
    }

    ToolResult scalpLandmarksTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const ScalpLandmarks m = landmarksOf(g);
        json points = json::object();
        for (const Landmark& l : m.points) points[l.name] = {{"p", vec(l.point)}, {"n", vec(l.normal)}};
        const ScalpFrame& f = m.frame;
        json at = json::array();
        if (a.contains("at")) {
            if (a["at"].size() > 200) throw ToolError("at takes at most 200 points");
            const MeshPtr scalp = scalpOf(g);
            for (const json& p : a["at"]) {
                const SurfacePoint sp = scalp->closestPoint(pointOrLandmark(p, m, "at"));
                at.push_back({{"p", vec(sp.position)}, {"n", vec(sp.normal)}});
            }
        }
        json out = {{"groom", g.name()}, {"upAxis", g.upAxis()}, {"units", g.units()},
                    {"bounds", {{"min", vec(m.lo)}, {"max", vec(m.hi)}}}, {"middle", vec(m.middle)}, {"size", r4(m.size)},
                    {"directions", {{"up", vec(f.up)}, {"down", vec(-f.up)}, {"front", vec(f.front)}, {"back", vec(-f.front)},
                                    {"left", vec(f.left)}, {"right", vec(-f.left)}}},
                    {"landmarks", points},
                    {"note", "Approximate: the head is taken as a ball under the scalp's top as wide as the scalp (middle), "
                             "each landmark the scalp point nearest that ball in its direction (left and right are the "
                             "character's, facing front). n is the surface's outward normal there. "
                             "brush_stroke, paint_map and select_guides take these names wherever they take a point."}};
        if (!at.empty()) out["at"] = at;
        return textResult(out);
    }

    ToolResult brushStroke(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const ScalpLandmarks marks = landmarksOf(g);
        const std::string mode = str(a, "mode");
        BrushToolSettings s;
        if (mode == "freeze" || mode == "unfreeze") {
            s.setMode("paint");
            s.brush.attribute = kFreezeLayer;
            s.brush.value = mode == "freeze" ? 1.0f : 0.0f;
        } else {
            try {
                s.setMode(mode);
            } catch (const std::invalid_argument& e) {
                throw ToolError(e.what());
            }
        }
        strokeSettings(s, a, marks);
        s.brush.level = static_cast<int>(num(a, "level", 0));
        if (s.brush.level < -1) throw ToolError("level is -1 (every level) or 0 and up");
        s.brush.group = str(a, "group");
        s.brush.propagate = flag(a, "propagate", false);
        s.brush.collide = flag(a, "collide", true);
        s.brush.stretch = flag(a, "stretch", false);
        if (a.contains("tube_channel")) s.brush.tubeChannel = tubeChannelFromString(str(a, "tube_channel"));
        if (mode == "paint") {
            if (!a.contains("attribute")) throw ToolError("paint needs \"attribute\": the guide layer it paints (paint_map paints scalp maps)");
            s.brush.attribute = str(a, "attribute");
        }
        if (a.contains("value")) s.brush.value = static_cast<float>(num(a, "value", 1.0));
        if (a.contains("paint_mode")) s.brush.paintMode = paintModeFromString(str(a, "paint_mode"));
        if (a.contains("targets")) s.setTargets(str(a, "targets"));
        s.place.length = static_cast<float>(num(a, "length", kDefaultGuideLength * marks.size));
        if (!(s.place.length > 0.0f)) throw ToolError("length must be positive");
        s.setSegments(static_cast<int>(num(a, "segments", 12)));
        s.place.matchNeighbors = flag(a, "match_neighbors", true);
        s.partName = str(a, "part_name", "part");
        if (a.contains("flow_sink")) {
            s.flowSink = str(a, "flow_sink");
            if (!s.flowSink.empty()) flowSinkAngle(s.flowSink);  // (throws for an unknown kind)
        }
        s.selectedOnly = flag(a, "selected_only", false);
        if (s.selectedOnly && g.selection.empty()) throw ToolError("selected_only: no guides are selected (select_guides picks them)");
        const bool places = s.tool == BrushToolSettings::Tool::Place;
        const float spacing = static_cast<float>(num(a, "spacing", places ? s.brush.radius : 0.25 * s.brush.radius));
        if (!(spacing > 0.0f)) throw ToolError("spacing must be positive");
        const std::vector<Vec3f> path = strokePath(a, *scalp, marks, spacing);

        AgentStrokeHost& host = g.strokeHost();
        host.clearStage();
        host.resetCommitted();
        host.label = mode + " stroke";
        host.setSelection(g.selection);
        const GuideHierarchy before = authored(g);
        const std::string partsBefore = g.session().data().partsJson, flowBefore = g.session().data().flowJson;
        scriptedStroke(host, s, path);
        keepSelected(g);
        json out = {{"mode", mode}, {"pathPoints", path.size()}, {"radius", r4(s.brush.radius)}, {"strength", r4(s.brush.strength)},
                    {"from", vec(path.front())}, {"to", vec(path.back())}};
        out["changed"] = changeJson(guideChange(before, authored(g)));
        if (g.session().data().partsJson != partsBefore) out["changed"]["parts"] = true;
        if (g.session().data().flowJson != flowBefore) out["changed"]["flow"] = true;
        if (!host.committed()) {
            out["note"] = "nothing changed: no guides within the radius of the path (raise radius or offset, check level and group, "
                          "or see where the guides are with get_guides)";
        }
        return textResult(withSummary(g, out));
    }

    ToolResult selectGuides(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string mode = str(a, "mode", "replace");
        std::vector<GuideId> picked;
        const GuideHierarchy h = authored(g);
        const int level = static_cast<int>(num(a, "level", -1));
        const std::string group = str(a, "group");
        if (!group.empty() && h.findGroup(group) == kNoGroup) throw ToolError("no guide group '" + group + "'");
        if (mode != "clear") {
            if (a.contains("ids")) {
                for (const json& id : a["ids"]) {
                    if (!id.is_number_integer() || !h.guide(id.get<GuideId>())) throw ToolError("no guide " + id.dump() + " (get_guides lists them)");
                    picked.push_back(id.get<GuideId>());
                }
            } else if (a.contains("center")) {
                const ScalpLandmarks marks = landmarksOf(g);
                const Vec3f center = pointOrLandmark(a["center"], marks, "center");
                const float radius = static_cast<float>(num(a, "radius", kDefaultRadius * marks.size));
                if (!(radius > 0.0f)) throw ToolError("radius must be positive");
                picked = guidesInSphere(h, center, radius, flag(a, "roots_only", true), level, group);
            } else if (!group.empty() || level >= 0) {
                for (const HierarchyGuide& guide : h.guides()) {
                    if (level >= 0 && h.groupDepth(guide.group) != level) continue;
                    const GuideGroup* gr = h.group(guide.group);
                    if (!group.empty() && (!gr || gr->name != group)) continue;
                    picked.push_back(guide.id);
                }
            } else {
                throw ToolError("say which guides: ids, a center (and radius), a group or a level; or mode clear");
            }
        }
        std::set<GuideId> now(g.selection.begin(), g.selection.end());
        if (mode == "replace" || mode == "clear") now.clear();
        for (const GuideId id : picked) {
            if (mode == "remove") {
                now.erase(id);
            } else {
                now.insert(id);
            }
        }
        g.selection.clear();
        for (const HierarchyGuide& guide : h.guides()) {  // (in the hierarchy's order)
            if (now.count(guide.id)) g.selection.push_back(guide.id);
        }
        json byGroup = json::object();
        for (const GuideId id : g.selection) {
            const GuideGroup* gr = h.group(h.guide(id)->group);
            const std::string name = gr ? gr->name : std::string();
            byGroup[name] = byGroup.value(name, 0) + 1;
        }
        json ids = json::array();
        for (std::size_t i = 0; i < g.selection.size() && i < 200; ++i) ids.push_back(g.selection[i]);
        return textResult({{"groom", g.name()}, {"picked", picked.size()}, {"selected", g.selection.size()}, {"ids", ids},
                           {"groups", byGroup},
                           {"hint", "brush_stroke with selected_only, guide_op and measure_guides with selection work on these."}});
    }

    ToolResult guideOpTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const std::string opName = str(a, "op");
        std::vector<GuideId> given;
        if (a.contains("guides")) {
            for (const json& id : a["guides"]) {
                if (!id.is_number_integer()) throw ToolError("guides are guide ids (get_guides, select_guides)");
                given.push_back(id.get<GuideId>());
            }
        } else {
            given = g.selection;
        }
        json out = {{"op", opName}};
        GuideOp op;
        op.op = opName;
        op.guides = given;
        op.group = str(a, "group");
        op.parent = str(a, "parent");
        op.name = str(a, "name");
        op.value = static_cast<float>(num(a, "value", 1.0));
        const std::string axis = str(a, "axis", "x");
        op.axis = axis == "y" ? 1 : axis == "z" ? 2 : 0;
        op.center = static_cast<float>(num(a, "center", 0.0));
        op.fromPositive = flag(a, "from_positive", true);
        op.side = static_cast<int>(num(a, "side", 0));
        op.segments = static_cast<int>(num(a, "segments", 0));
        op.groupWide = flag(a, "group_wide", false);
        op.level = static_cast<int>(num(a, "level", -1));
        op.byLength = flag(a, "by_length", false);
        if (a.contains("parent_guide")) op.parentGuide = static_cast<GuideId>(num(a, "parent_guide", 0.0));
        if (opName == "mirror_shapes") {
            BrushToolSettings sym;
            sym.setSymmetry(str(a, "symmetry", "x"));
            sym.brush.mirrorCenter = op.center;
            op.symmetry = StrokeSymmetry::of(sym.brush, scalp.get());
        }
        const std::string label = opName == "generate_children" ? std::string("generate children")
                                  : opName == "edit_tubes"      ? std::string("edit tubes")
                                                                : guideOpLabel(op);
        edit(g, label, [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            h.evaluate(scalp.get());
            if (opName == "generate_children") {
                ChildrenRequest r;
                r.parents = given;
                r.group = str(a, "group");
                r.childGroup = str(a, "child_group");
                r.params.count = static_cast<std::size_t>(std::clamp(num(a, "count", 5.0), 0.0, 1e6));
                r.params.rootSpread = static_cast<float>(num(a, "root_spread", r.params.rootSpread));
                r.params.tipSpread = static_cast<float>(num(a, "tip_spread", r.params.tipSpread));
                r.params.jitter = static_cast<float>(num(a, "jitter", r.params.jitter));
                r.params.twist = static_cast<float>(num(a, "twist_degrees", 0.0) * 3.14159265358979 / 180.0);
                r.params.childTubeScale = static_cast<float>(num(a, "child_tube_scale", r.params.childTubeScale));
                r.params.seed = static_cast<std::uint64_t>(std::max(0.0, num(a, "seed", 0.0)));
                r.params.split = flag(a, "split", false);
                const ChildrenMade made = generateChildren(h, r);
                out["parentGroup"] = made.parentGroup;
                out["childGroup"] = made.childGroup;
                out["added"] = made.added;
                out["childCount"] = made.childCount;
            } else if (opName == "edit_tubes") {
                if (!a.contains("tube")) throw ToolError("edit_tubes needs \"tube\": {shape, width, aspect, twist}");
                checkArguments(objectSchema(tubeEditProperties()), a["tube"]);
                std::vector<GuideId> guides = given;
                if (!a.contains("guides") && a.contains("group")) {
                    const GroupId group = h.findGroup(str(a, "group"));
                    if (group == kNoGroup) throw ToolError("no guide group '" + str(a, "group") + "'");
                    guides = h.guidesInGroup(group);
                }
                std::string note;
                out["count"] = editTubes(h, guides, a["tube"].dump(), &note);
                if (!note.empty()) out["note"] = note;
            } else {
                const GuideOpResult res = applyGuideOp(h, op);
                if (!res.group.empty()) out["group"] = res.group;
                if (!res.made.empty()) out["made"] = res.made;
                out["count"] = res.count;
                if (res.segments) out["segments"] = res.segments;
            }
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        out["step"] = label;
        keepSelected(g);
        return textResult(withSummary(g, out));
    }

    // Map values' range, for the answer.
    json mapStats(AgentGroom& g, const std::string& name) {
        const ScalpMaps& maps = g.session().scalpMaps();
        const auto it = maps.maps.find(name);
        if (it == maps.maps.end() || it->second.kind != "vertices" || it->second.values.empty()) return nullptr;
        const ScalpMapEntry& e = it->second;
        const std::size_t step = static_cast<std::size_t>(std::max(1, e.channels));
        float lo = 1e30f, hi = -1e30f;
        double sum = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i < e.values.size(); i += step) {
            lo = std::min(lo, e.values[i]);
            hi = std::max(hi, e.values[i]);
            sum += e.values[i];
            ++n;
        }
        return {{"vertices", n}, {"min", r4(lo)}, {"mean", r4(sum / static_cast<double>(n))}, {"max", r4(hi)}};
    }

    ToolResult paintMap(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        GroomSession& session = g.session();
        const std::string kind = str(a, "kind", "vertices");
        std::string name = str(a, "map");
        const std::string forParam = str(a, "for");
        if (name.empty() && forParam.empty()) throw ToolError("say which map: \"map\" (its name) or \"for\" (\"node.param\" it drives)");
        const MeshPtr cage = session.cageScalp() ? session.cageScalp() : scalp;
        ScalpMaps maps = session.scalpMaps();
        const std::string oldMaps = maps.toJson();
        std::string graph = graphText(g);
        const std::string oldGraph = graph;
        GuideHierarchy h = hierarchyOf(g);
        h.evaluate(scalp.get());
        const std::string oldHierarchy = h.toJson();
        const MapFiles noFiles;  // (vertex maps and guide layers: no files)
        json out = {{"kind", kind}};
        bool made = false;
        if (!forParam.empty()) {
            const auto dot = forParam.find('.');
            if (dot == std::string::npos) throw ToolError("for is \"node.param\" (e.g. \"scatter.density\")");
            ParamPaintRequest request;
            request.kind = kind;
            request.name = name;
            const ParamPaint p = paintParam(graph, maps, &h, cage.get(), forParam.substr(0, dot), forParam.substr(dot + 1), request, noFiles);
            name = p.map;
            made = p.made;
            if (p.made) {
                if (p.created.guideLayer) {
                    applyCreatedLayer(h, name, p.created);
                } else {
                    maps = p.created.maps;
                }
            }
            if (!p.graph.empty()) graph = p.graph;
            if (p.kind != kind) out["kind"] = p.kind;
            out["linked"] = forParam;
        } else {
            checkName(name, "a map's name");
            const std::vector<std::string> layers = h.attributeNames();
            const bool exists = maps.maps.count(name) || std::find(layers.begin(), layers.end(), name) != layers.end();
            if (!exists) {
                MapCreateRequest request;
                request.kind = kind;
                request.value = static_cast<float>(num(a, "initial", 0.0));
                const MapCreate c = createMap(maps, &h, cage.get(), name, request, noFiles);
                if (c.guideLayer) {
                    applyCreatedLayer(h, name, c);
                } else {
                    maps = c.maps;
                }
                made = true;
            }
        }
        if (a.contains("fill")) {
            MapFillRequest fill;
            fill.value = static_cast<float>(num(a, "fill", 0.0));
            const MapFill f = fillMap(maps, &h, cage.get(), name, fill, noFiles);
            if (f.kind == "guides") {
                applyFilledLayer(h, name, f);
            } else if (f.mapsChanged) {
                maps = f.maps;
            }
            out["filled"] = fill.value;
        }
        out["map"] = name;
        out["made"] = made;

        AgentStrokeHost& host = g.strokeHost();
        host.clearStage();
        host.resetCommitted();
        host.label = "paint map " + name;
        if (maps.toJson() != oldMaps) host.stage(StrokeField::ScalpMaps, maps.toJson());
        if (graph != oldGraph) host.stage(StrokeField::Graph, graph);
        if (h.toJson() != oldHierarchy) host.stage(StrokeField::Guides, h.toJson());
        const bool stroke = a.contains("points") || a.contains("from");
        try {
            if (stroke) {
                const std::string mapKind = maps.maps.count(name) ? maps.maps.at(name).kind : std::string("guides");
                if (mapKind == "texture") throw ToolError("'" + name + "' is a texture map: paint it in GroomLab (vertex maps and guide layers paint here)");
                const ScalpLandmarks marks = landmarksOf(g);
                BrushToolSettings s;
                s.setMode("paint");
                s.brush.attribute = name;
                s.brush.value = static_cast<float>(num(a, "value", 1.0));
                s.brush.level = -1;
                s.paintKind = mapKind;
                strokeSettings(s, a, marks);
                if (a.contains("paint_mode")) s.brush.paintMode = paintModeFromString(str(a, "paint_mode"));
                const std::vector<Vec3f> path = strokePath(a, *scalp, marks, static_cast<float>(num(a, "spacing", 0.25 * s.brush.radius)));
                const std::size_t changed = scriptedStroke(host, s, path);
                out["pathPoints"] = path.size();
                out["radius"] = r4(s.brush.radius);
                out["value"] = s.brush.value;
                if (mapKind == "vertices") out["verticesPainted"] = changed;
            } else {
                host.commit(StrokeEdit{});  // (what was made, linked or filled: one step)
            }
        } catch (...) {
            host.clearStage();
            throw;
        }
        host.clearStage();
        if (!host.committed()) out["note"] = "nothing changed";
        const json stats = mapStats(g, name);
        if (!stats.is_null()) out["values"] = stats;
        if (!forParam.empty()) {
            out["hint"] = "the map multiplies " + forParam + ": white (1) keeps its value, 0 takes it away (density 0: no hair there)";
        }
        return textResult(withSummary(g, out));
    }

    ToolResult redoTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const int steps = static_cast<int>(num(a, "steps", 1));
        if (steps < 1 || steps > static_cast<int>(kMaxUndo)) throw ToolError("steps must be 1 to " + std::to_string(kMaxUndo));
        if (!g.history().canRedo()) throw ToolError("nothing to redo");
        std::vector<std::string> labels;
        for (int i = 0; i < steps && g.history().canRedo(); ++i) {
            labels.push_back(g.history().toRedo().title());
            const std::string error = g.redo();
            if (!error.empty()) throw ToolError("redo: " + error);
        }
        keepSelected(g);
        return textResult(withSummary(g, {{"redone", labels}, {"undoStepsLeft", g.history().undoIndex()},
                                          {"redoStepsLeft", g.history().size() - g.history().undoIndex()}}));
    }

    ToolResult historyTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const EditHistory& h = g.history();
        json steps = json::array();
        for (std::size_t i = 0; i < h.size(); ++i) {
            steps.push_back({{"step", i + 1}, {"title", h.step(i).title()}, {"state", i < h.undoIndex() ? "done" : "undone"}});
        }
        json out = {{"groom", g.name()}, {"steps", steps}, {"undoSteps", h.undoIndex()}, {"redoSteps", h.size() - h.undoIndex()}};
        if (h.canUndo()) out["undo"] = h.toUndo().title();
        if (h.canRedo()) out["redo"] = h.toRedo().title();
        return textResult(out);
    }

    ToolResult measureGuidesTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const HierarchyPtr live = g.session().hierarchy();
        const GuideHierarchy stored = authored(g);
        const GuideHierarchy& h = live ? *live : stored;
        const MeshPtr scalp = g.session().scalp();
        const ScalpFrame frame = ScalpFrame::of(g.upAxis());
        const int level = static_cast<int>(num(a, "level", -1));
        auto entry = [&](const std::string& label, const std::vector<GuideId>& ids) {
            const GuideStats st = measureGuides(h, ids, scalp.get());
            json e = {{"name", label}, {"count", st.count}};
            if (st.count == 0) return e;
            e["length"] = {{"min", r4(st.lengthMin)}, {"mean", r4(st.lengthMean)}, {"max", r4(st.lengthMax)}};
            e["direction"] = vec(st.direction);
            e["directionWords"] = directionWords(st.direction, frame);
            e["straightness"] = r4(st.straightness);
            e["curlDegrees"] = std::round(st.curlDegrees * 10.0f) / 10.0f;
            e["rootAngleDegrees"] = std::round(st.rootAngleDegrees * 10.0f) / 10.0f;
            e["rootMiddle"] = vec(st.rootMiddle);
            e["tipMiddle"] = vec(st.tipMiddle);
            return e;
        };
        json groups = json::array();
        if (flag(a, "selection", false)) {
            if (g.selection.empty()) throw ToolError("no guides are selected (select_guides picks them)");
            groups.push_back(entry("selection", g.selection));
        } else {
            const std::string only = str(a, "group");
            if (!only.empty() && h.findGroup(only) == kNoGroup) throw ToolError("no guide group '" + only + "'");
            for (const GuideGroup& gr : h.groups()) {
                if (!only.empty() && gr.name != only) continue;
                if (level >= 0 && h.groupDepth(gr.id) != level) continue;
                json e = entry(gr.name, h.guidesInGroup(gr.id));
                e["level"] = h.groupDepth(gr.id);
                groups.push_back(e);
            }
        }
        return textResult({{"groom", g.name()}, {"groups", groups},
                           {"about", "length in scene units; direction is the mean root-to-tip direction; straightness is the "
                                     "root-to-tip distance over the length (1 straight); curlDegrees the angle turned root to tip "
                                     "(360 a full turn); rootAngleDegrees how far the first segment leans from the scalp's normal "
                                     "(0 standing out, 90 lying flat)."}});
    }

    // ---- reference photos ----
    static ToolResult imageResult(const std::vector<std::uint8_t>& png, const json& info) {
        ToolResult r;
        r.content.push_back({{"type", "image"}, {"data", base64Encode(png)}, {"mimeType", "image/png"}});
        r.content.push_back({{"type", "text"}, {"text", info.dump(-1, ' ', false, json::error_handler_t::replace)}});
        return r;
    }

    static RefImage loadImage(const std::string& path, int channels) {
        try {
            return loadRefImage(path, channels);
        } catch (const std::exception& e) {
            throw ToolError("cannot read the image '" + path + "': " + e.what());
        }
    }

    // A grid step that gives about ten lines across the photo: 10, 20, 25, 50, 100, 200, 250, 500...
    static int gridStepFor(int width, int height) {
        const double raw = std::max(width, height) / 10.0;
        double base = 1.0;
        while (base * 10.0 <= raw) base *= 10.0;
        for (const double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
            if (base * m >= raw) return std::max(10, static_cast<int>(base * m));
        }
        return static_cast<int>(base * 10.0);
    }

    // The photo named (or the only one; or by its number, 1 on).
    static AgentPhoto& photoNamed(std::vector<AgentPhoto>& photos, const std::string& name) {
        if (photos.empty()) throw ToolError("the groom has no photos: add one with add_photo");
        if (name.empty()) {
            if (photos.size() == 1) return photos.front();
            throw ToolError("the groom has several photos: say which with \"photo\"");
        }
        for (AgentPhoto& p : photos) {
            if (p.name == name) return p;
        }
        std::string names;
        for (const AgentPhoto& p : photos) names += (names.empty() ? "" : ", ") + p.name;
        throw ToolError("no photo '" + name + "' (photos: " + names + ")");
    }

    static json cameraInfo(const RefCamera& c) {
        const Vec3f look = -c.axis(2);
        return {{"eye", vec(c.eye())}, {"looking", vec(look)}, {"focal_mm", r4(focalPixelsToMm(c.focalPx, c.width))},
                {"width", c.width}, {"height", c.height}};
    }

    ToolResult addPhoto(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string path = files().resolve(str(a, "path"));
        const RefImage image = loadImage(path, 3);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        std::string name = str(a, "name", fs::u8path(path).stem().u8string());
        checkName(name, "a photo's name");
        auto taken = [&](const std::string& n) {
            return std::any_of(photos.begin(), photos.end(), [&](const AgentPhoto& p) { return p.name == n; });
        };
        if (taken(name)) {
            if (a.contains("name")) throw ToolError("a photo named '" + name + "' is there already");
            const std::string base = name;
            for (int i = 2; taken(name); ++i) name = base + "_" + std::to_string(i);
        }
        AgentPhoto p;
        p.name = name;
        p.image = path;
        p.width = image.width;
        p.height = image.height;
        photos.push_back(p);
        g.commitPhotos("add photo " + name, photosToJson(photos));
        const int step = gridStepFor(p.width, p.height);
        return imageResult(photoGridPng(image, p, step, kPhotoShown),
                           {{"groom", g.name()}, {"photo", name}, {"width", p.width}, {"height", p.height}, {"grid", step},
                            {"hint", "Line it up: mesh_landmarks shows numbered points of the mesh (from a side like the photo's), "
                                     "then align_photo pairs photo pixels (read off the grid) with those points: 6 or more pairs "
                                     "solve the focal length too, 4 keep it."}});
    }

    ToolResult photoView(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        const AgentPhoto& p = photoNamed(photos, str(a, "photo"));
        const RefImage image = loadImage(p.image, 3);
        const int size = static_cast<int>(std::clamp(num(a, "size", kPhotoShown), 128.0, 1536.0));
        if (a.contains("region")) {
            const json& r = a["region"];
            if (!r.is_array() || r.size() != 4) throw ToolError("region is [x0, y0, x1, y1] in the photo's pixels");
            std::array<int, 4> box{};
            for (int k = 0; k < 4; ++k) {
                if (!r[static_cast<std::size_t>(k)].is_number()) throw ToolError("region is [x0, y0, x1, y1] in the photo's pixels");
                box[static_cast<std::size_t>(k)] = static_cast<int>(std::lround(r[static_cast<std::size_t>(k)].get<double>()));
            }
            box = {std::clamp(std::min(box[0], box[2]), 0, p.width), std::clamp(std::min(box[1], box[3]), 0, p.height),
                   std::clamp(std::max(box[0], box[2]), 0, p.width), std::clamp(std::max(box[1], box[3]), 0, p.height)};
            if (box[2] - box[0] < 8 || box[3] - box[1] < 8) throw ToolError("the region must be 8 pixels across or more, inside the photo");
            const int across = std::max(box[2] - box[0], box[3] - box[1]);
            const int fine = a.contains("grid") ? static_cast<int>(num(a, "grid", 10)) : across <= 120 ? 5 : across <= 300 ? 10 : 25;
            if (fine < 1) throw ToolError("grid must be 1 pixel or more");
            int shownW = 0, shownH = 0;
            const auto png = photoRegionPng(image, p, box, fine, size, &shownW, &shownH);
            return imageResult(png, {{"groom", g.name()}, {"photo", p.name}, {"region", box}, {"grid", fine}, {"shown", {shownW, shownH}},
                                     {"scale", r4(static_cast<double>(shownW) / static_cast<double>(box[2] - box[0]))},
                                     {"note", "the labels are the photo's own pixels (x right, y down from its top-left): read a "
                                              "feature's pixel here, then pair it in align_photo"}});
        }
        const int step = a.contains("grid") ? static_cast<int>(num(a, "grid", 50)) : gridStepFor(p.width, p.height);
        if (step < 8) throw ToolError("grid must be 8 pixels or more (finer: give a region)");
        std::optional<ReferenceViewPreview> preview;
        json info = {{"groom", g.name()}, {"photo", p.name}, {"width", p.width}, {"height", p.height}, {"grid", step},
                     {"linedUp", p.camera.has_value()}, {"landmarks", p.state.landmarks.summary("none")},
                     {"flowArrows", p.state.strokes.size()}, {"maskTouchUps", p.state.maskStrokes.size()}};
        if (flag(a, "analysis", false)) {
            if (!p.camera) throw ToolError("analysis needs the photo lined up first (align_photo)");
            ReferenceView view;
            view.camera = *p.camera;
            view.image = image;
            if (!p.mask.empty()) view.mask = loadImage(p.mask, 1);
            view.hints = p.state.strokes;
            view.maskStrokes = p.state.maskStrokes;
            const HairScalp hair = hairScalpOf(g);
            ReferenceGuideOptions analysisOptions;
            analysisOptions.up = upOf(g);
            if (hair.head) analysisOptions.occluders.push_back(hair.head);
            preview = previewReferenceView(*hair.scalp, view, analysisOptions);
            info["analysis"] = "pink: the hair mask guides_from_photos reads; cyan: the strands' flow it reads";
        }
        int shownW = 0, shownH = 0;
        const auto png = photoGridPng(image, p, step, size, preview ? &*preview : nullptr, &shownW, &shownH);
        info["shown"] = {shownW, shownH};
        info["note"] = "the grid's labels are the photo's own pixels (x right, y down from the top-left), whatever the size shown";
        return imageResult(png, info);
    }

    ToolResult meshLandmarks(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        const std::string view = str(a, "view", "front");
        int width = 768, height = 768;
        const AgentPhoto* photo = nullptr;
        if (a.contains("photo") || view == "photo") photo = &photoNamed(photos, str(a, "photo"));
        if (photo) {
            width = photo->width;
            height = photo->height;
        }
        RefCamera camera;
        if (view == "photo") {
            if (!photo->camera) throw ToolError("view photo needs the photo lined up (align_photo); pick a side instead");
            camera = *photo->camera;
        } else {
            camera = viewCamera(*scalp, g.upAxis(), view, width, height, num(a, "focal_mm", 50.0));
        }
        const int count = static_cast<int>(std::clamp(num(a, "count", 24.0), 4.0, 80.0));
        const std::vector<MeshCandidate> found = meshCandidates(*scalp, camera, count);
        json points = json::array();
        for (std::size_t i = 0; i < found.size(); ++i) {
            points.push_back({{"n", i + 1}, {"p", vec(found[i].point)}, {"normal", vec(found[i].normal)},
                              {"pixel", {std::round(found[i].u), std::round(found[i].v)}}});
        }
        // A whole head's facial landmarks this camera sees, by name (heuristic: their confidence).
        const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
        json named = json::array();
        std::vector<PointLabel> labels;
        if (face->head) {
            const Vec3f eye = camera.eye();
            for (const FaceLandmark& f : face->points) {
                float u = 0.0f, v = 0.0f;
                if (!camera.project(f.point, u, v) || u < 0.0f || v < 0.0f || u > camera.width || v > camera.height) continue;
                if (f.normal.dot((eye - f.point).normalized()) < 0.05f) continue;  // (facing away: hidden)
                static const std::map<std::string, std::string> codes = {
                    {"nose_tip", "NT"},         {"chin", "CH"},           {"left_eye", "LE"},          {"right_eye", "RE"},
                    {"left_eye_inner", "LEI"},  {"left_eye_outer", "LEO"}, {"right_eye_inner", "REI"}, {"right_eye_outer", "REO"},
                    {"left_mouth_corner", "LM"}, {"right_mouth_corner", "RM"}, {"left_ear_top", "LET"}, {"left_earlobe", "LEB"},
                    {"right_ear_top", "RET"},   {"right_earlobe", "REB"},  {"hairline_middle", "HL"},  {"crown", "CR"},
                    {"nape", "NP"}};
                const auto c = codes.find(f.name);
                const std::string code = c != codes.end() ? c->second : f.name;
                labels.push_back({code, u, v});
                named.push_back({{"name", f.name}, {"code", code}, {"p", vec(f.point)}, {"pixel", {std::round(u), std::round(v)}},
                                 {"confidence", r4(f.confidence)}});
            }
        }
        const int size = static_cast<int>(std::clamp(num(a, "size", kPhotoShown), 128.0, 1536.0));
        json info = {{"groom", g.name()}, {"view", view}, {"camera", cameraInfo(camera)}, {"points", points},
                     {"hint", "Find the same features on the photo (photo_view's grid, or a zoomed region, gives their pixels) and pair "
                              "them in align_photo: {\"pixel\": [u, v], \"point\": p or a name}. Pick points spread over the photo, "
                              "not on a line; pixel is where this camera sees each point (not the photo)."}};
        if (face->head) {
            info["named"] = named;
            info["namedNote"] = "Facial landmarks found from the mesh's shape (heuristic: trust those with confidence 0.6 or more; "
                                "check the rest on the picture, cyan codes). Use their names as align_photo points, so an earlobe "
                                "is paired with the photo's earlobe, not the middle of the ear.";
        } else {
            info["namedNote"] = face->why;
        }
        return imageResult(meshCandidatesPng(*scalp, camera, found, size, labels), info);
    }

    // A photo's pairs as an agent gave them ({"pixel", "point": [x, y, z] or a landmark's name}),
    // after `lists`' own.
    MarkerLists pairsOf(const json& pairs, const AgentPhoto& p, const ScalpMesh& scalp, const ScalpLandmarks& marks, MarkerLists lists,
                        const std::string& prefix = "pairs") {
        if (lists.pendingCount() > 0) throw ToolError("the photo has a landmark without its partner: clear_landmarks with photo_edit first");
        if (!pairs.is_array() || pairs.empty() || pairs.size() > 200) throw ToolError(prefix + ": give 1 to 200 pairs");
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            const json& pair = pairs[i];
            const std::string where = prefix + "[" + std::to_string(i) + "]";
            if (!pair.is_object() || !pair.contains("pixel") || !pair.contains("point")) {
                throw ToolError(where + " must be {\"pixel\": [u, v], \"point\": [x, y, z] or a landmark}");
            }
            const json& px = pair["pixel"];
            if (!px.is_array() || px.size() != 2 || !px[0].is_number() || !px[1].is_number()) throw ToolError(where + ".pixel must be [u, v]");
            const float u = px[0].get<float>(), v = px[1].get<float>();
            if (u < 0.0f || v < 0.0f || u > static_cast<float>(p.width) || v > static_cast<float>(p.height)) {
                throw ToolError(where + ".pixel lies outside the photo (" + std::to_string(p.width) + " x " + std::to_string(p.height) + ")");
            }
            MarkerPoint mesh;
            const SurfacePoint at = scalp.closestPoint(pointOrLandmark(pair["point"], marks, (where + ".point").c_str()));
            mesh.root = at.root;
            mesh.point = at.position;
            lists.add(kLandmarkPhoto, ReferencePhotoEditor::photoPoint(u, v));
            lists.add(kLandmarkMesh, mesh);
        }
        return lists;
    }

    static std::vector<CameraCorrespondence> correspondencesOf(const MarkerLists& lists) {
        std::vector<CameraCorrespondence> out;
        for (std::size_t i = 0; i < lists.pairCount(); ++i) {
            const Vec3f& w = lists.point(kLandmarkMesh, i).point;
            const Vec3f& px = lists.point(kLandmarkPhoto, i).point;
            out.push_back({px.x, px.y, {w.x, w.y, w.z}});
        }
        return out;
    }

    // The shoot's frame: the head's middle and directions.
    ShootOptions shootFrame(AgentGroom& g) {
        const ScalpLandmarks m = scalpLandmarks(*scalpOf(g), g.upAxis());
        ShootOptions o;
        o.center = m.middle;
        o.up = m.frame.up;
        o.front = m.frame.front;
        return o;
    }

    static double focalMmOf(const RefCamera& c) { return focalPixelsToMm(c.focalPx, c.width); }

    ToolResult alignPhoto(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const ScalpLandmarks marks = landmarksOf(g);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        AgentPhoto& p = photoNamed(photos, str(a, "photo"));
        const json& pairs = a["pairs"];
        const MarkerLists lists = pairsOf(pairs, p, *scalp, marks, flag(a, "append", false) ? p.state.landmarks : MarkerLists{});
        double focal = p.camera && p.camera->focalPx > 0.0 ? p.camera->focalPx : focalMmToPixels(50.0, p.width);
        if (a.contains("focal_mm")) focal = focalMmToPixels(num(a, "focal_mm", 50.0), p.width);
        const bool keep = flag(a, "keep_focal", false);
        // A view hint: the solve starts from a camera on that side (the photo's own when none).
        std::optional<RefCamera> hint;
        const std::string viewHint = str(a, "view");
        if (!viewHint.empty()) {
            try {
                hint = viewCamera(*scalp, g.upAxis(), viewHint, p.width, p.height, focalPixelsToMm(focal, p.width));
            } catch (const std::invalid_argument& e) {
                throw ToolError(e.what());
            }
        }
        const CameraPose* start = hint ? &hint->pose : p.camera ? &p.camera->pose : nullptr;
        ReferenceLandmarkSolve solve = solveReferenceLandmarks(lists, p.width, p.height, focal, keep, start, true);
        json info = {{"groom", g.name()}, {"photo", p.name}, {"pairs", solve.pairs}, {"linedUp", solve.ok}};
        // The same solve with what a photo shoot is like (agent_shoot.h): the view's angle round the
        // head and level, the focal and distance of the shoot's photos lined up already (like: "auto"
        // the others, a photo's name, "none"), a free focal leaning to a portrait lens. Kept unless
        // its pairs fit clearly worse.
        const std::string like = str(a, "like", "auto");
        std::vector<ShootView> shoot;
        if (like != "none") {
            for (const AgentPhoto& o : photos) {
                if (o.name == p.name || !o.camera || (like != "auto" && o.name != like)) continue;
                ShootView v;
                v.width = o.width;
                v.height = o.height;
                v.fixed = *o.camera;
                shoot.push_back(v);
            }
            if (like != "auto" && shoot.empty()) throw ToolError("like: no lined-up photo '" + like + "'");
        }
        double yaw = 0.0;
        bool level = true;
        const bool hasYaw = !viewHint.empty() && viewYaw(viewHint, yaw, &level);
        if (lists.pairCount() >= 3 && (hasYaw || !shoot.empty() || !keep)) {
            ShootOptions o = shootFrame(g);
            if (keep || a.contains("focal_mm")) {
                if (keep || !shoot.empty()) o.focalMm = focalPixelsToMm(focal, p.width);
            }
            if (!o.focalMm && !shoot.empty()) {
                std::vector<double> fs;
                for (const ShootView& v : shoot) fs.push_back(focalMmOf(*v.fixed));
                std::sort(fs.begin(), fs.end());
                o.focalMm = fs[fs.size() / 2];
            }
            o.priorWeight = num(a, "prior_weight", 1.0);
            ShootView view;
            view.pairs = correspondencesOf(lists);
            view.width = p.width;
            view.height = p.height;
            if (hasYaw) view.yawDegrees = yaw;
            view.level = hasYaw && level;
            std::vector<ShootView> views = shoot;
            views.push_back(view);
            const ShootSolve s = solveShoot(views, o);
            json priors = {{"done", s.ok}};
            if (s.ok) {
                const ShootCamera& c = s.cameras.back();
                const double plain = solve.ok ? solve.rms : std::numeric_limits<double>::infinity();
                const bool plainFar = solve.ok && hint &&
                                      std::acos(std::clamp(static_cast<double>((-hint->axis(2)).dot(-solve.camera.axis(2))), -1.0, 1.0)) * 57.29578 > 25.0;
                // Kept: when its pairs fit nearly as well, or the pairs alone left the camera far from the view.
                const bool take = !solve.ok || c.rms <= std::max(1.15 * plain, plain + 0.3) || (plainFar && c.rms <= std::max(2.0 * plain, plain + 4.0));
                priors["rms"] = r4(c.rms);
                priors["focal_mm"] = r4(s.focalMm);
                if (!s.focalNote.empty()) priors["focalNote"] = s.focalNote;
                priors["pairsAloneRms"] = solve.ok ? json(r4(solve.rms)) : json(nullptr);
                priors["pairsAloneFocalMm"] = solve.ok ? json(r4(focalMmOf(solve.camera))) : json(nullptr);
                priors["shootPhotos"] = shoot.size();
                priors["kept"] = take;
                if (hasYaw) priors["fromView"] = r4(c.fromPrior);
                if (take) {
                    solve.ok = true;
                    solve.camera = c.camera;
                    solve.rms = c.rms;
                    solve.errors = c.errors;
                    solve.focalSolved = !o.focalMm;
                    solve.bad.assign(solve.errors.size(), false);
                    solve.why.clear();
                }
                priors["note"] = take ? "kept: the camera with the shoot's priors (its view's angle, level, the shoot's focal and "
                                        "distance, a portrait lens when the pairs cannot tell the focal)"
                                      : "the pairs alone fit clearly better: kept their camera";
            } else {
                priors["why"] = s.why;
            }
            info["priors"] = priors;
            info["linedUp"] = solve.ok;
        }
        // The pairs' names a landmark gave, and those the mesh's shape found only roughly.
        json unsure = json::array();
        for (const json& pair : pairs) {
            if (!pair["point"].is_string()) continue;
            const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
            if (const FaceLandmark* f = face->find(pair["point"].get<std::string>()); f && f->confidence < 0.5f) unsure.push_back(f->name);
        }
        if (!unsure.empty()) info["unsureLandmarks"] = unsure;
        // Tightened by the outline where it is not hair (refine).
        if (solve.ok && flag(a, "refine", false)) {
            const RefImage image = loadImage(p.image, 3);
            std::vector<CameraCorrespondence> correspondences;
            for (std::size_t i = 0; i < lists.pairCount(); ++i) {
                const Vec3f& w = lists.point(kLandmarkMesh, i).point;
                const Vec3f& px = lists.point(kLandmarkPhoto, i).point;
                correspondences.push_back({px.x, px.y, {w.x, w.y, w.z}});
            }
            // The photo's hair (what guides_from_photos would read), when it tells the hair from the rest.
            RefImage hair;
            {
                ReferenceView view;
                view.camera = solve.camera;
                view.image = image;
                if (!p.mask.empty()) view.mask = loadImage(p.mask, 1);
                view.maskStrokes = p.state.maskStrokes;
                const ReferenceViewPreview preview = previewReferenceView(*scalp, view, ReferenceGuideOptions{});
                if (!preview.mask.empty()) {
                    hair.width = p.width;
                    hair.height = p.height;
                    hair.channels = 1;
                    hair.pixels.assign(static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height), 0.0f);
                    const RefImage subject = subjectMask(image);
                    double inHair = 0.0, inSubject = 0.0;
                    const auto& c = preview.crop;
                    for (int y = 0; y < p.height; ++y) {
                        for (int x = 0; x < p.width; ++x) {
                            const float mu = (static_cast<float>(x) + 0.5f - static_cast<float>(c[0])) / static_cast<float>(std::max(1, c[2])) *
                                             static_cast<float>(preview.mask.width);
                            const float mv = (static_cast<float>(y) + 0.5f - static_cast<float>(c[1])) / static_cast<float>(std::max(1, c[3])) *
                                             static_cast<float>(preview.mask.height);
                            float m = 0.0f;
                            if (mu >= 0.0f && mv >= 0.0f && mu < static_cast<float>(preview.mask.width) && mv < static_cast<float>(preview.mask.height)) {
                                m = preview.mask.sample(mu, mv);
                            }
                            hair.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width) + static_cast<std::size_t>(x)] = m;
                            if (subject.at(x, y) > 0.5f) inSubject += 1.0, inHair += m > 0.5f ? 1.0 : 0.0;
                        }
                    }
                    if (inSubject > 0.0 && inHair / inSubject > 0.85) hair = RefImage{};  // (it saw the whole subject as hair: no help)
                }
            }
            const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
            const ScalpLandmarks marks2 = scalpLandmarks(*scalp, g.upAxis());
            std::function<bool(const Vec3f&)> usable;
            if (face->head) usable = [face, marks2](const Vec3f& q) { return hairRegionWeight(*face, marks2, q) < 0.5f; };
            const OutlineFit fit = refineWithOutline(*scalp, image, hair, solve.camera, correspondences, usable);
            json r = {{"done", fit.ok}};
            if (!fit.ok) {
                r["why"] = fit.why;
            } else {
                r["pairsRmsBefore"] = r4(fit.pairsBefore);
                r["pairsRmsAfter"] = r4(fit.pairsAfter);
                r["outlineBefore"] = r4(fit.outlineBefore);
                r["outlineAfter"] = r4(fit.outlineAfter);
                r["outlineSamples"] = fit.samples;
                r["photoOutlinePixels"] = fit.edgePixels;
                r["hairMaskUsed"] = !hair.empty();
                // Kept when the outline fits better and the pairs stay about where they were.
                const bool better = fit.outlineAfter < 0.9 * fit.outlineBefore && fit.pairsAfter <= std::max(1.25 * fit.pairsBefore, fit.pairsBefore + 1.0);
                r["kept"] = better;
                if (better) {
                    solve.camera = fit.camera;
                    solve.rms = fit.pairsAfter;
                    solve.errors.clear();
                    for (const CameraCorrespondence& q : correspondences) {
                        float u = 0, v = 0;
                        fit.camera.project(Vec3f(static_cast<float>(q.world[0]), static_cast<float>(q.world[1]), static_cast<float>(q.world[2])), u, v);
                        solve.errors.push_back(std::sqrt((u - q.u) * (u - q.u) + (v - q.v) * (v - q.v)));
                    }
                }
                r["note"] = "the outline counted: the mesh's face, ears and neck (below its hairline) against the photo's subject's "
                            "edge where the photo shows no hair; outline numbers are mean pixels to the photo's edge (capped)";
            }
            info["refine"] = r;
        }
        p.state.landmarks = lists;
        if (solve.ok) p.camera = solve.camera;
        g.commitPhotos("align photo " + p.name, photosToJson(photos));
        if (!solve.ok) {
            info["why"] = solve.why;
        } else {
            info["rmsPixels"] = r4(solve.rms);
            info["errorsPixels"] = json::array();
            for (const double e : solve.errors) info["errorsPixels"].push_back(r4(e));
            json bad = json::array();
            for (std::size_t i = 0; i < solve.bad.size(); ++i) {
                if (solve.bad[i]) bad.push_back(i + 1);
            }
            if (!bad.empty()) info["badPairs"] = bad;
            info["focalSolved"] = solve.focalSolved;
            if (!solve.focalSolved) {
                info["focalNote"] = keep ? "the focal length was kept, as asked"
                                         : solve.pairs < kReferenceMinPairsFocal
                                               ? "the focal length was kept: 6 pairs or more can solve it"
                                               : "the focal length was kept: these pairs cannot tell it (they lie too near a plane, or "
                                                 "too close together in depth); give focal_mm if you know the lens";
            }
            info["camera"] = cameraInfo(solve.camera);
            if (hint) {
                const Vec3f want = -hint->axis(2), got = -solve.camera.axis(2);
                const double angle = std::acos(std::clamp(static_cast<double>(want.dot(got)), -1.0, 1.0)) * 57.29578;
                info["viewAngleFromHint"] = std::round(angle * 10.0) / 10.0;
                if (angle > 25.0) {
                    info["viewWarning"] = "the solved camera looks " + std::to_string(static_cast<int>(std::lround(angle))) +
                                          " degrees away from the view hint: a pair may be wrong, or too few pairs hold the camera "
                                          "(add pairs spread in height and depth, e.g. the chin and the neck)";
                }
            }
            info["hint"] = std::string("The image: the mesh's outline (cyan) over the photo, each pair's photo point (green ring) "
                                       "joined to its mesh point through the camera (red). ") +
                           (bad.empty() ? "Under about 1% of the photo's width is lined up." : "Fix or drop the pairs listed in badPairs first.");
        }
        const RefImage image = loadImage(p.image, 3);
        return imageResult(photoOverlayPng(image, *scalp, p, kPhotoShown), info);
    }

    ToolResult alignPhotos(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const ScalpLandmarks marks = landmarksOf(g);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        const json& list = a["photos"];
        if (!list.is_array() || list.empty() || list.size() > 16) throw ToolError("photos: 1 to 16 entries {photo, view, pairs}");
        std::vector<std::size_t> which;
        std::vector<ShootView> views;
        std::vector<MarkerLists> lists;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const json& e = list[i];
            if (!e.is_object()) throw ToolError("photos[" + std::to_string(i) + "] is {\"photo\", \"view\", \"pairs\"}");
            AgentPhoto& p = photoNamed(photos, e.value("photo", std::string()));
            const std::size_t index = static_cast<std::size_t>(&p - photos.data());
            if (std::find(which.begin(), which.end(), index) != which.end()) throw ToolError("photo '" + p.name + "' is listed twice");
            MarkerLists l = e.contains("pairs") ? pairsOf(e["pairs"], p, *scalp, marks, MarkerLists{}, "photos[" + std::to_string(i) + "].pairs")
                                                : p.state.landmarks;
            if (l.pairCount() < 3) throw ToolError("photo '" + p.name + "' needs 3 pairs or more (give its pairs)");
            ShootView v;
            v.pairs = correspondencesOf(l);
            v.width = p.width;
            v.height = p.height;
            const std::string view = e.value("view", std::string());
            double yaw = 0.0;
            bool level = true;
            if (!view.empty()) {
                if (!viewYaw(view, yaw, &level)) throw ToolError("photos[" + std::to_string(i) + "].view: unknown view '" + view + "'");
                v.yawDegrees = yaw;
                v.level = level;
            } else {
                v.level = false;
            }
            which.push_back(index);
            views.push_back(v);
            lists.push_back(l);
        }
        ShootOptions o = shootFrame(g);
        if (a.contains("focal_mm")) o.focalMm = num(a, "focal_mm", 100.0);
        o.priorWeight = num(a, "prior_weight", 1.0);
        if (o.priorWeight < 0.0 || o.priorWeight > 20.0) throw ToolError("prior_weight is 0 (pairs alone) to 20");
        const ShootSolve s = solveShoot(views, o);
        if (!s.ok) throw ToolError("the photos could not be lined up together: " + s.why);
        json per = json::array();
        ToolResult r;
        for (std::size_t k = 0; k < which.size(); ++k) {
            AgentPhoto& p = photos[which[k]];
            const ShootCamera& c = s.cameras[k];
            p.state.landmarks = lists[k];
            p.camera = c.camera;
            json errors = json::array();
            for (const double e : c.errors) errors.push_back(r4(e));
            json entry = {{"photo", p.name}, {"pairs", lists[k].pairCount()}, {"rmsPixels", r4(c.rms)}, {"errorsPixels", errors},
                          {"yaw", r4(c.yaw)}, {"pitch", r4(c.pitch)}, {"roll", r4(c.roll)}, {"distance", r4(c.distance)},
                          {"camera", cameraInfo(c.camera)}};
            if (views[k].yawDegrees) entry["fromView"] = r4(c.fromPrior);
            if (c.fights) entry["fights"] = "its pairs disagree with the rest of the shoot: check them (or another person's head)";
            per.push_back(entry);
        }
        g.commitPhotos("align photos", photosToJson(photos));
        for (std::size_t k = 0; k < which.size() && k < 6; ++k) {
            const AgentPhoto& p = photos[which[k]];
            r.content.push_back({{"type", "image"}, {"data", base64Encode(photoOverlayPng(loadImage(p.image, 3), *scalp, p, 384))},
                                 {"mimeType", "image/png"}});
        }
        json info = {{"groom", g.name()}, {"focal_mm", r4(s.focalMm)}, {"focalSolved", !a.contains("focal_mm")}, {"rmsPixels", r4(s.rms)},
                     {"focalNote", s.focalNote},
                     {"photos", per}, {"priorWeight", o.priorWeight},
                     {"hint", "The images: each photo with the mesh's outline through its camera and its pairs (green: the photo's, red: "
                              "the mesh's), in the order given. One focal for the shoot; each camera near its view's angle, level, at "
                              "about the shoot's distance (soft: prior_weight 0 lets the pairs alone decide)."}};
        r.content.push_back({{"type", "text"}, {"text", info.dump(-1, ' ', false, json::error_handler_t::replace)}});
        return r;
    }

    // What the photo tools grow hair on: on a whole head (its face found), the scalp's hair region
    // alone (the map Scatter's density reads, else the landmarks' hairline) with the whole head an
    // occluder, so the face, ears and neck are neither roots nor hair in the photos' masks.
    struct HairScalp {
        MeshPtr scalp;    // where the hair grows
        MeshPtr head;     // the whole mesh (an occluder), null when it is the scalp itself
    };
    HairScalp hairScalpOf(AgentGroom& g) {
        const MeshPtr scalp = scalpOf(g);
        HairScalp out{scalp, nullptr};
        const std::shared_ptr<const FaceLandmarks> face = faceOf(g, scalp);
        if (!face->head) return out;
        // The region per vertex: the density map's (it is on the raw scalp, the refined one's points
        // first) when its count fits, else the landmarks' hairline.
        std::vector<float> weight(scalp->pointCount(), -1.0f);
        try {
            const std::string graph = graphText(g);
            std::string scatter;
            for (const json& n : json::parse(graph).value("nodes", json::array()))
                if (n.value("type", std::string()) == "Scatter") scatter = n.value("name", std::string());
            if (!scatter.empty()) {
                const GuideHierarchy h = hierarchyOf(g);
                const ParamMapRef read = paramMapOf(graph, g.session().scalpMaps(), &h, scatter, "density");
                const auto it = g.session().scalpMaps().maps.find(read.map);
                if (!read.map.empty() && it != g.session().scalpMaps().maps.end() && it->second.kind == "vertices") {
                    const int ch = std::max(1, it->second.channels);
                    for (std::size_t i = 0; i < weight.size() && (i + 1) * static_cast<std::size_t>(ch) <= it->second.values.size(); ++i)
                        weight[i] = it->second.values[i * static_cast<std::size_t>(ch)];
                }
            }
        } catch (const std::exception&) {
        }
        const ScalpLandmarks marks = scalpLandmarks(*scalp, g.upAxis());
        for (std::size_t i = 0; i < weight.size(); ++i) {
            if (weight[i] < 0.0f) weight[i] = hairRegionWeight(*face, marks, scalp->points()[i]);
        }
        const MeshPtr cap = capOf(*scalp, weight);
        if (!cap) return out;
        out.scalp = cap;
        out.head = scalp;
        return out;
    }

    ToolResult compareToPhotoTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        std::vector<const AgentPhoto*> which;
        if (a.contains("photo")) {
            which.push_back(&photoNamed(photos, str(a, "photo")));
            if (!which.front()->camera) throw ToolError("the photo '" + which.front()->name + "' is not lined up yet (align_photo)");
        } else {
            for (const AgentPhoto& p : photos) {
                if (p.camera) which.push_back(&p);
            }
        }
        if (which.empty()) throw ToolError(photos.empty() ? "the groom has no photos: add_photo, then align_photo" : "no photo is lined up yet");
        ViewRequest request;
        request.time = g.frame / std::max(g.framesPerSecond(), 1e-9);
        request.density = 1.0f;
        const ViewResult& seen = g.view(request);
        if (!seen.strands || seen.strands->strandCount() == 0) throw ToolError("the groom has no strands to compare (evaluate it)");
        const StrandSet& strands = *seen.strands;
        const HairScalp hair = hairScalpOf(g);
        ToolResult r;
        json per = json::array();
        auto c3 = [](const std::array<float, 3>& c) { return json::array({r4(c[0]), r4(c[1]), r4(c[2])}); };
        for (const AgentPhoto* p : which) {
            ReferenceView view;
            view.camera = *p->camera;
            view.image = loadImage(p->image, 3);
            if (!p->mask.empty()) view.mask = loadImage(p->mask, 1);
            view.maskStrokes = p->state.maskStrokes;
            ReferenceGuideOptions compareOptions;
            compareOptions.up = upOf(g);
            if (hair.head) compareOptions.occluders.push_back(hair.head);
            const PhotoComparison c = compareToPhoto(*hair.scalp, strands, view, compareOptions);
            json regions = json::object();
            for (const PhotoComparison::Region& rg : c.regions) regions[rg.name] = {{"missing", r4(rg.missing)}, {"extra", r4(rg.extra)}};
            json e = {{"photo", p->name}, {"iou", r4(c.iou)}, {"missedShare", r4(c.missedShare)}, {"extraShare", r4(c.extraShare)},
                      {"regions", regions},
                      {"color", {{"photo", {{"linear", c3(c.photoColor)}, {"melanin", r4(c.photoMelanin)}, {"redness", r4(c.photoRedness)}}},
                                 {"groom", {{"linear", c3(c.groomColor)}, {"melanin", r4(c.groomMelanin)}, {"redness", r4(c.groomRedness)}}}}}};
            if (c.hairlineFound) e["hairlineOffset"] = r4(c.hairlineOffset);
            if (c.flowPixels > 0) e["flow"] = {{"agreement", r4(c.flowAgreement)}, {"aligned", r4(c.flowAligned)}};
            per.push_back(e);
            if (r.content.size() < 6) {
                r.content.push_back({{"type", "image"}, {"data", base64Encode(comparisonPng(view.image, c, 512))}, {"mimeType", "image/png"}});
            }
        }
        json info = {{"groom", g.name()}, {"units", g.units()}, {"photos", per},
                     {"about", "iou: the groom's hair silhouette through each photo's camera against the photo's hair mask (1 the same); "
                               "missedShare: the photo's hair the groom does not cover; extraShare: the groom's hair outside the "
                               "photo's; regions: how far the photo's hair reaches past the groom's (missing) and the groom's past the "
                               "photo's (extra), round the head in the picture, scene units; hairlineOffset: where the hair from the "
                               "top ends on the face (the head's middle third), the groom's below the photo's when positive; color: the photo's hair (white "
                               "balanced) and the strands' appearance, each's nearest melanin; flow: the strands' directions against "
                               "the photo's where both show hair, lock by lock (agreement: the mean of cos 2x their angle, 1 alike, "
                               "0 unrelated; aligned: the share within 20 degrees): curls in step with the photo's raise it, an even "
                               "frizz or straight strands over curls lower it. The images: green the hair the groom "
                               "misses, magenta the groom's outside the photo's, each outline."}};
        r.content.push_back({{"type", "text"}, {"text", withSummary(g, info).dump(-1, ' ', false, json::error_handler_t::replace)}});
        return r;
    }

    ToolResult photoEdit(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        AgentPhoto& p = photoNamed(photos, str(a, "photo"));
        const std::string op = str(a, "op");
        auto pixels = [&]() {
            if (!a.contains("points")) throw ToolError(op + " needs \"points\": [[u, v], ...] in the photo's pixels");
            std::vector<std::array<float, 2>> out;
            for (const json& q : a["points"]) {
                if (!q.is_array() || q.size() != 2 || !q[0].is_number() || !q[1].is_number()) throw ToolError("points are [u, v] pixels");
                out.push_back({q[0].get<float>(), q[1].get<float>()});
            }
            return out;
        };
        const std::string name = p.name;
        ReferencePhotoEditor editor(p.state);
        json info = {{"groom", g.name()}, {"photo", name}, {"op", op}};
        if (op == "add_flow") {
            editor.addStroke(pixels(), flag(a, "directed", true));
        } else if (op == "erase_flow") {
            info["erased"] = editor.eraseStrokes(pixels(), static_cast<float>(num(a, "radius", 10.0)));
        } else if (op == "clear_flow") {
            editor.clearStrokes();
        } else if (op == "add_mask") {
            editor.addMaskStroke(pixels(), static_cast<float>(num(a, "radius", 8.0)), flag(a, "include", true));
        } else if (op == "clear_mask") {
            editor.clearMaskStrokes();
        } else if (op == "remove_landmark") {
            const int index = static_cast<int>(num(a, "index", 0));
            if (index < 1 || static_cast<std::size_t>(index) > editor.landmarks().count(kLandmarkPhoto)) {
                throw ToolError("index is a landmark pair's number, 1 to " + std::to_string(editor.landmarks().count(kLandmarkPhoto)));
            }
            editor.removeLandmark(kLandmarkPhoto, static_cast<std::size_t>(index - 1));
        } else if (op == "clear_landmarks") {
            editor.clearLandmarks();
        } else if (op == "set_mask") {
            if (!a.contains("path")) throw ToolError("set_mask needs \"path\": a mask image (white hair, black backdrop), or \"\" for none");
            p.mask = str(a, "path").empty() ? std::string() : files().resolve(str(a, "path"));
            if (!p.mask.empty()) loadImage(p.mask, 1);
        } else if (op == "remove_photo") {
            photos.erase(std::find_if(photos.begin(), photos.end(), [&](const AgentPhoto& x) { return x.name == name; }));
            g.commitPhotos("remove photo " + name, photosToJson(photos));
            info["photos"] = photos.size();
            return textResult(info);
        }
        if (op != "set_mask") p.state = editor.state();
        g.commitPhotos(op + " on " + name, photosToJson(photos));
        const RefImage image = loadImage(p.image, 3);
        info["flowArrows"] = p.state.strokes.size();
        info["maskTouchUps"] = p.state.maskStrokes.size();
        info["landmarks"] = p.state.landmarks.summary("none");
        return imageResult(photoGridPng(image, p, gridStepFor(p.width, p.height), kPhotoShown), info);
    }

    ToolResult guidesFromPhotos(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const std::vector<AgentPhoto> photos = photosFromJson(g.referencePhotos);
        std::vector<ReferenceView> views;
        json used = json::array();
        for (const AgentPhoto& p : photos) {
            if (!p.camera) continue;
            ReferenceView view;
            view.camera = *p.camera;
            view.image = loadImage(p.image, 3);
            if (!p.mask.empty()) view.mask = loadImage(p.mask, 1);
            view.hints = p.state.strokes;
            view.maskStrokes = p.state.maskStrokes;
            views.push_back(std::move(view));
            used.push_back(p.name);
        }
        if (views.empty()) throw ToolError(photos.empty() ? "the groom has no photos: add_photo, then align_photo"
                                                           : "no photo is lined up yet: align_photo first");
        ReferenceGuideOptions options;
        options.guideCount = static_cast<int>(std::clamp(num(a, "guide_count", 300.0), 10.0, 5000.0));
        options.maxLength = static_cast<float>(num(a, "max_length", 0.0));
        options.symmetry = referenceSymmetryFromName(str(a, "symmetry", "auto"));
        options.lockCurls = flag(a, "locks", true);
        if (a.contains("volume")) {
            const double volume = num(a, "volume", 0.75);
            if (volume < 0.0 || volume > 1.5) throw ToolError("volume is 0 to 1.5 (a share of the hair's thickness the photos show)");
            options.volume = static_cast<float>(volume);
        }
        options.up = upOf(g);
        const HairScalp hair = hairScalpOf(g);
        if (hair.head) options.occluders.push_back(hair.head);
        const ReferenceGuidesResult result = guidesFromReference(*hair.scalp, views, options);
        const std::string groupName = str(a, "group", "fromReference");
        checkName(groupName, "a group's name");
        std::size_t added = 0;
        edit(g, "guides from photos", [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            const GroupId old = h.findGroup(groupName);
            if (old != kNoGroup) h.removeGroup(old);
            const GroupId group = h.addGroup(groupName);
            for (const ReferenceGuide& rg : result.guides) {
                if (rg.points.size() < 2) continue;
                const GuideId id = h.addGuide(group, rg.points);
                // (each lock's tightness, for its strands' clump: Interpolate carries it onto them)
                if (result.locksTraced > 0) h.setAttribute(id, "lockStrength", rg.lock ? rg.lockStrength : 0.5f);
                ++added;
            }
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        // The scalp's regions no lined-up photo sees (a share of each; the face, ears and neck left out on a whole head).
        json unseen = json::object();
        json alignMore = json::array();
        if (result.scalpCoverage.size() == hair.scalp->pointCount()) {
            const ScalpLandmarks marks = scalpLandmarks(*scalp, g.upAxis());
            std::map<std::string, std::array<double, 2>> counts;  // region -> {unseen, all}
            for (std::size_t i = 0; i < hair.scalp->pointCount(); ++i) {
                const Vec3f& q = hair.scalp->points()[i];
                const Vec3f d = (q - marks.middle).normalized();
                const float a2 = d.dot(marks.frame.left), b2 = d.dot(marks.frame.up), c2 = d.dot(marks.frame.front);
                std::string region = "top";
                if (b2 < 0.75f) {
                    const float theta = std::atan2(a2, c2) * 57.29578f;
                    region = std::abs(theta) < 45.0f ? "front" : std::abs(theta) > 135.0f ? "back" : theta > 0.0f ? "left" : "right";
                }
                counts[region][1] += 1.0;
                if (result.scalpCoverage[i] < 0.0f) counts[region][0] += 1.0;
            }
            for (const auto& [region, c] : counts) {
                if (c[1] <= 0.0) continue;
                const double share = c[0] / c[1];
                unseen[region] = r4(share);
                if (share > 0.4) alignMore.push_back(region);
            }
        }
        json out = {{"group", groupName}, {"guides", added}, {"photos", used}, {"symmetry", referenceSymmetryName(result.symmetry)},
                    {"rootsTried", result.rootsTried}, {"rootsBald", result.rootsBald}, {"rootsUnseen", result.rootsUnseen},
                    {"rootsShort", result.rootsShort}, {"maxLength", r4(result.maxLength)}, {"seconds", r4(result.seconds)}};
        const ReferenceLookFit& look = result.look;
        LockLook locks;
        if (result.locksTraced > 0) {
            locks = {true, result.lockRadius, result.lockWavelength};
            out["locks"] = {{"detected", result.locksDetected}, {"rootsAtLocks", result.lockRoots},
                            {"traced", result.locksTraced}, {"filled", result.locksFilled}, {"radius", r4(result.lockRadius)},
                            {"turnLength", r4(result.lockWavelength)},
                            {"about", "detected: the locks the photos show (streamlines curling clearly); rootsAtLocks: the roots "
                                      "taken where they grow from (the rest spread evenly); each guide follows a lock: traced, the "
                                      "curl a photo shows along it (moved onto the lock, "
                                      "its turn, size, phase and handedness); filled, its traced neighbours' curl where no "
                                      "photo shows one. Scene units. apply_look clumps each guide's strands onto it "
                                      "(Clump centersFrom inputCurves, strength per lock from the guides' lockStrength) "
                                      "instead of a Curl node over every strand."}};
        }
        if (look.ok) {
            out["look"] = {{"melanin", r4(look.melanin)}, {"redness", r4(look.redness)}, {"baseColor", {r4(look.baseColor[0]), r4(look.baseColor[1]), r4(look.baseColor[2])}},
                           {"curly", look.curly}, {"curlFrequency", r4(look.curlFrequency)}, {"curlRadius", r4(look.curlRadius)},
                           {"curlShare", r4(look.curlShare)}, {"curlSwingDegrees", r4(look.curlAmplitudeDeg)},
                           {"clumped", look.clumped}, {"clumpSize", r4(look.clumpSize)}, {"clumpStrength", r4(look.clumpStrength)},
                           {"flyawayFraction", r4(look.flyawayFraction)}, {"frizzFraction", r4(look.frizzFraction)},
                           {"coverage", r4(look.coverage)}};
            if (!look.curly && look.curlFrequency > 0.0f) {
                out["curlNote"] = "a curl was measured but under the share that counts as curly hair (curly false): if the photos "
                                  "show curls, add a Curl node (add_node) with the measured radius and frequency";
            }
            if (flag(a, "apply_look", false)) {
                out["lookApplied"] = applyPhotoLook(g, look, locks);
            } else {
                out["lookHint"] = "What the photos suggest for HairColor (melanin, redness), Curl (frequency, radius), Clump (size, "
                                  "strength) and Flyaway: apply_look: true sets them, or set_params one by one.";
            }
        }
        if (!unseen.empty()) out["unseenShare"] = unseen;
        if (!alignMore.empty()) {
            out["alignMore"] = alignMore;
            out["alignHint"] = "No lined-up photo sees much of these sides of the scalp: their guides are guessed (mirrored or "
                               "from the priors). Line up a photo from each (align_photo with view), then grow again.";
        }
        return textResult(withSummary(g, out));
    }

    // ---- roots and tubes ----
    // A closed outline on the scalp: the points given, or a circle of `radius` round `center`.
    std::vector<Vec3f> rootOutline(const json& a, const ScalpMesh& scalp, const ScalpLandmarks& marks) {
        std::vector<Vec3f> outline;
        if (a.contains("outline")) {
            for (const json& p : a["outline"]) outline.push_back(scalp.closestPoint(pointOrLandmark(p, marks, "outline")).position);
            return outline;
        }
        if (!a.contains("center")) throw ToolError("give the root: \"outline\" (points round it on the scalp) or \"center\" and \"radius\"");
        const SurfacePoint c = scalp.closestPoint(pointOrLandmark(a["center"], marks, "center"));
        const float radius = static_cast<float>(num(a, "radius", 0.04 * marks.size));
        if (!(radius > 0.0f)) throw ToolError("radius must be positive");
        const int sides = static_cast<int>(std::clamp(num(a, "sides", 8.0), 3.0, 64.0));
        const Vec3f n = c.normal.normalized();
        Vec3f t = std::abs(n.dot(marks.frame.up)) < 0.9f ? marks.frame.up : marks.frame.front;
        t = (t - n * t.dot(n)).normalized();
        const Vec3f b = n.cross(t);
        for (int i = 0; i < sides; ++i) {
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(sides);
            outline.push_back(scalp.closestPoint(c.position + (t * std::cos(angle) + b * std::sin(angle)) * radius).position);
        }
        return outline;
    }

    std::vector<GuideId> givenGuides(const json& a, AgentGroom& g) {
        if (!a.contains("guides")) return g.selection;
        std::vector<GuideId> out;
        for (const json& id : a["guides"]) {
            if (!id.is_number_integer()) throw ToolError("guides are guide ids");
            out.push_back(id.get<GuideId>());
        }
        return out;
    }

    // The scalp with the vertex maps on it (the subdivision density reads its color set).
    MeshPtr mappedScalp(AgentGroom& g, const MeshPtr& scalp) {
        std::vector<std::string> errors;
        const MeshPtr mapped = withVertexMaps(scalp, g.session().scalpMaps(), {}, nullptr, &errors);
        return mapped ? mapped : scalp;
    }

    ToolResult rootTube(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const ScalpLandmarks marks = landmarksOf(g);
        const std::string op = str(a, "op");
        json out = {{"op", op}};
        if (op == "list") {
            const GuideHierarchy h = authored(g);
            json roots = json::array();
            for (const GuideId id : rootTubesOf(h)) {
                const HierarchyGuide* guide = h.guide(id);
                const ScalpPointNormal c = rootTubeCenter(h, *scalp, id);
                const GuideGroup* gr = guide ? h.group(guide->group) : nullptr;
                roots.push_back({{"id", id}, {"group", gr ? gr->name : std::string()}, {"center", vec(c.point)},
                                 {"cvs", rootTubeOutline(h, *scalp, id).size()}, {"points", guide ? guide->points.size() : 0}});
            }
            out["roots"] = roots;
            out["subdivisionMap"] = kSubdivisionDensityMap;
            return textResult(out);
        }
        RootTubeOptions options;
        options.length = static_cast<float>(num(a, "length", 0.3 * marks.size));
        options.segments = static_cast<int>(std::clamp(num(a, "segments", 12.0), 1.0, 200.0));
        options.taper = static_cast<float>(num(a, "taper", 0.5));
        options.density = static_cast<float>(num(a, "density", 1.0));
        options.group = str(a, "group");
        RootSubdivisionOptions cut;
        cut.sizeAtZero = static_cast<float>(num(a, "size_at_zero", cut.sizeAtZero));
        cut.sizeAtOne = static_cast<float>(num(a, "size_at_one", cut.sizeAtOne));
        cut.multiplier = static_cast<float>(num(a, "multiplier", cut.multiplier));
        if (a.contains("seed")) cut.seed = static_cast<std::uint64_t>(std::max(0.0, num(a, "seed", 0.0)));
        cut.force = flag(a, "force", false);
        const std::vector<GuideId> guides = givenGuides(a, g);
        std::string label = "root tube " + op;
        std::vector<GuideId> made;
        edit(g, label, [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            h.evaluate(scalp.get());
            if (op == "draw") {
                std::vector<Vec3f> path;
                if (a.contains("path")) {
                    for (const json& p : a["path"]) path.push_back(pointOrLandmark(p, marks, "path"));
                }
                made.push_back(makeRootTube(h, *scalp, rootOutline(a, *scalp, marks), path, options));
            } else if (op == "edit_outline") {
                if (guides.size() != 1) throw ToolError("edit_outline takes one root tube (guides: [id])");
                editRootTube(h, *scalp, guides.front(), rootOutline(a, *scalp, marks));
                made.push_back(guides.front());
            } else if (op == "duplicate") {
                if (!a.contains("to")) throw ToolError("duplicate needs \"to\": where the copy goes (a point or landmark)");
                const std::vector<GuideId> roots = rootTubesOf(h, guides);
                if (roots.empty()) throw ToolError("no root tube given or selected (root_tube list lists them)");
                const Vec3f to = scalp->closestPoint(pointOrLandmark(a["to"], marks, "to")).position;
                const Vec3f start = rootTubeCenter(h, *scalp, roots.front()).point;
                for (const GuideId id : roots) made.push_back(duplicateRootTube(h, *scalp, id, to, roots.size() > 1 ? &start : nullptr));
            } else if (op == "delete") {
                const std::vector<GuideId> roots = rootTubesOf(h, guides);
                if (roots.empty()) throw ToolError("no root tube given or selected");
                out["removed"] = deleteRootTubes(h, *scalp, roots);
            } else if (op == "subdivide" || op == "apply_density") {
                const MeshPtr mapped = mappedScalp(g, scalp);
                const SubdivisionResult r = op == "subdivide" ? subdivideRootTubes(h, *mapped, guides, cut)
                                                              : applySubdivisionDensity(h, *mapped, guides, cut);
                made = r.cells;
                out["removed"] = r.removed;
                out["kept"] = r.kept;
                out["cellLevel"] = r.cellLevel;
            } else {
                throw ToolError("op is list, draw, edit_outline, duplicate, delete, subdivide or apply_density");
            }
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        out["guides"] = made;
        if (op == "subdivide") {
            out["densityHint"] = std::string("Paint where the cells are smaller with paint_map {\"map\": \"") + kSubdivisionDensityMap +
                                 "\", \"initial\": 0.5} (1 smallest, 0 largest), then root_tube apply_density.";
        }
        keepSelected(g);
        return textResult(withSummary(g, out));
    }

    ToolResult tubeLevelsTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const std::string op = str(a, "op", "list");
        const std::vector<GuideId> guides = givenGuides(a, g);
        const std::string picked = str(a, "group");
        json out = {{"op", op}};
        auto describe = [&](const GuideHierarchy& h) {
            const TubeLevels levels = tubeLevels(h, guides, picked);
            json list = json::array();
            for (std::size_t i = 0; i < levels.levels.size(); ++i) {
                const GroupId gr = h.findGroup(levels.levels[i]);
                list.push_back({{"level", i + 1}, {"group", levels.levels[i]}, {"tubes", gr == kNoGroup ? 0 : h.guidesInGroup(gr).size()}});
            }
            out["base"] = levels.base;
            out["levels"] = list;
            return levels;
        };
        if (op == "list") {
            describe(authored(g));
        } else if (op == "add" || op == "remove") {
            std::size_t count = 0;
            edit(g, op == "add" ? "add tube level" : "remove tube level", [&](std::string&, std::string& hierarchy) {
                GuideHierarchy h = hierarchyOf(g);
                h.evaluate(scalp.get());
                if (op == "add") {
                    count = addTubeLevel(h, scalp.get(), guides, picked);
                } else {
                    const TubeLevels levels = tubeLevels(h, guides, picked);
                    count = removeTubeLevel(h, scalp.get(), levels.base, a.contains("guides") ? guides : std::vector<GuideId>{});
                }
                h.evaluate(scalp.get());
                hierarchy = h.toJson();
            });
            out[op == "add" ? "split" : "removed"] = count;
            describe(authored(g));
        } else if (op == "view") {
            const TubeLevels levels = describe(authored(g));
            const int level = static_cast<int>(num(a, "level", 0));
            if (level < 0 || level > static_cast<int>(levels.levels.size())) {
                throw ToolError("level is 1 to " + std::to_string(levels.levels.size()) + " (0: every level)");
            }
            g.mixedView = level > 0 ? levels.base + ":" + std::to_string(level) + (flag(a, "mixed", true) ? "" : ":solo") : std::string();
            g.tubeViewGroup = str(a, "pin");
            out["mixedView"] = g.mixedView;
            out["pinned"] = g.tubeViewGroup;
            out["note"] = "render_preview with tubes: true draws the tubes as this view shows them (not saved with the groom).";
            return textResult(out);
        } else {
            throw ToolError("op is list, add, remove or view");
        }
        return textResult(op == "list" ? out : withSummary(g, out));
    }

    ToolResult tubeShapeTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const MeshPtr scalp = scalpOf(g);
        const std::string op = str(a, "op", "list");
        if (op == "list") {
            const GuideHierarchy h = authored(g);
            json library = json::array();
            for (const auto& [name, shape] : h.tubeShapes()) {
                if (!isOwnedTubeShape(name)) library.push_back(name);
            }
            return textResult({{"builtin", tubeShapeNames()}, {"library", library},
                               {"hint", "set_tube or guide_op edit_tubes give guides a shape by name (shape, midShape, tipShape)."}});
        }
        TubeShapeOp shape;
        shape.op = op;
        shape.name = str(a, "name");
        shape.to = str(a, "to");
        if (a.contains("points")) {
            std::vector<SectionPoint> pts;
            for (const json& q : a["points"]) {
                if (!q.is_array() || q.size() != 2 || !q[0].is_number() || !q[1].is_number()) throw ToolError("points are [u, v] pairs");
                pts.push_back({q[0].get<float>(), q[1].get<float>()});
            }
            shape.points = pts;
        }
        if (a.contains("smooth")) shape.smooth = flag(a, "smooth", true);
        if (flag(a, "give", false)) shape.give = givenGuides(a, g);
        shape.profile = str(a, "profile", "shape");
        std::size_t count = 0;
        edit(g, tubeShapeLabel(shape), [&](std::string&, std::string& hierarchy) {
            GuideHierarchy h = hierarchyOf(g);
            h.evaluate(scalp.get());
            count = editTubeShape(h, shape);
            h.evaluate(scalp.get());
            hierarchy = h.toJson();
        });
        return textResult(withSummary(g, {{"op", op}, {"shape", op == "rename" ? shape.to : shape.name}, {"guides", count}}));
    }

    // ---- expressions ----
    ToolResult checkExpressionTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        std::vector<std::pair<std::string, float>> constants;
        if (a.contains("variables")) {
            try {
                constants = parseExpressionVariables(a["variables"].dump());
            } catch (const std::exception& e) {
                throw ToolError(std::string("variables: ") + e.what());
            }
        }
        const std::string type = str(a, "type", "any");
        const ExpressionType want = type == "number" ? ExpressionType::Number : type == "vector" ? ExpressionType::Vector : ExpressionType::Any;
        ViewRequest request;
        request.viewNode = str(a, "node");
        request.time = frameToSeconds(g.frame, g.framesPerSecond());
        const ViewResult& view = g.view(request);
        const StrandsPtr strands = view.strands;
        const MeshPtr scalp = view.scalp ? view.scalp : g.session().scalp();
        const std::size_t samples = static_cast<std::size_t>(std::clamp(num(a, "samples", 2000.0), 10.0, 100000.0));
        const ExpressionCheck c = checkExpression(str(a, "expression"), constants, strands.get(), scalp.get(), samples, want);
        json out = {{"ok", c.ok}};
        if (!c.ok) {
            out["error"] = c.error;
            out["hint"] = "Fix the expression where the error says (positions count from 0, lines and columns from 1) and check again.";
        } else {
            out["type"] = c.vector ? "vector" : "number";
            out["perPoint"] = c.perPoint;
            out["readsTime"] = c.readsTime;
            out["variables"] = c.variables;
            out["channels"] = c.channels;
            if (!c.missingChannels.empty()) out["missingChannels"] = c.missingChannels;
            out["maps"] = c.maps;
            if (!c.error.empty()) out["warning"] = c.error;
            out["sampledOver"] = c.sampledOver;
            out["samples"] = c.samples;
            if (c.samples > 0) {
                out["values"] = {{"min", r4(c.min)}, {"mean", r4(c.mean)}, {"max", r4(c.max)}, {"histogram", c.histogram}};
                if (c.vector) out["valuesAre"] = "the vectors' lengths";
                json ex = json::array();
                for (const auto& [where, value] : c.examples) ex.push_back({where, r4(value)});
                out["examples"] = ex;
            }
            out["hint"] = "Set it on a mappable param with set_params: {\"param\": {\"expression\": \"<text>\", \"min\": lo, "
                          "\"max\": hi}} (the value 0-1 maps onto min..max), or on an Expression node's expression param.";
        }
        json available = json::object();
        for (const auto& [name, about] : expressionVariables()) available[name] = about;
        if (strands) {
            json channels = json::array();
            for (const std::string& n : strands->strandChannelNames()) channels.push_back(n);
            for (const std::string& n : strands->pointChannelNames()) channels.push_back(n + " (per point)");
            out["strandChannels"] = channels;
        }
        out["available"] = available;
        ToolResult r = textResult(out);
        if (c.ok && flag(a, "image", false)) {
            const ToolResult map = scalpHeatmap(g, a, str(a, "expression"), constants);
            for (const json& item : map.content)
                if (item.value("type", std::string()) == "image") r.content.insert(r.content.begin(), item);
        }
        return r;
    }

    // ---- simulation ----
    ToolResult importAnimation(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        AnimationInputs inputs;
        auto cache = [&](const char* fileKey, const char* objectKey) -> std::optional<CacheRef> {
            if (!a.contains(fileKey)) return std::nullopt;
            CacheRef ref;
            const std::string file = str(a, fileKey);
            if (!file.empty()) {
                ref.file = files().resolve(file);
                if (!hasExtension(ref.file, ".abc")) throw ToolError(std::string(fileKey) + " is an Alembic (.abc) file");
            }
            ref.object = str(a, objectKey);
            return ref;
        };
        inputs.scalpCache = cache("scalp_cache", "scalp_object");
        inputs.guideCache = cache("guide_cache", "guide_object");
        json out = json::object();
        if (inputs.scalpCache || inputs.guideCache) {
            target.setAnimation(g, inputs);
            if (inputs.scalpCache) out["scalpCache"] = inputs.scalpCache->file.empty() ? "none" : files().shown(inputs.scalpCache->file);
            if (inputs.guideCache) out["guideCache"] = inputs.guideCache->file.empty() ? "none" : files().shown(inputs.guideCache->file);
        }
        if (a.contains("colliders")) {
            json made = json::array();
            for (const json& c : a["colliders"]) {
                if (!c.is_object() || !c.contains("name") || !c.contains("path")) throw ToolError("colliders are {\"name\", \"path\", \"object\"}");
                const std::string name = c["name"].get<std::string>();
                checkName(name, "a collider's name");
                const std::string path = files().resolve(c["path"].get<std::string>());
                g.setCollider(name, target.readMesh(path, c.value("object", std::string())));
                made.push_back(name);
            }
            out["colliders"] = made;
            out["colliderHint"] = "A Collide (or Simulate) node reads a collider through a ColliderMesh node naming it.";
        }
        if (out.empty()) throw ToolError("give scalp_cache, guide_cache or colliders");
        out["note"] = "Animation inputs are the groom's scene, not an edit: undo does not take them back.";
        return textResult(withSummary(g, out));
    }

    ToolResult animateScalp(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const ScalpLandmarks marks = landmarksOf(g);
        const GroomFile file = g.toFile();
        const MeshPtr base = file.data.scalp;
        if (!base) throw ToolError("the groom has no scalp");
        struct Key {
            double frame;
            Vec3f translate, rotate;
        };
        std::vector<Key> keys;
        for (const json& k : a["keys"]) {
            if (!k.is_object() || !k.contains("frame") || !k["frame"].is_number()) throw ToolError("keys are {\"frame\", \"translate\", \"rotate_degrees\"}");
            Key key{k["frame"].get<double>(), Vec3f(0, 0, 0), Vec3f(0, 0, 0)};
            if (k.contains("translate")) key.translate = toPoint(k["translate"], "translate");
            if (k.contains("rotate_degrees")) key.rotate = toPoint(k["rotate_degrees"], "rotate_degrees");
            keys.push_back(key);
        }
        if (keys.size() < 2) throw ToolError("give 2 keys or more");
        std::sort(keys.begin(), keys.end(), [](const Key& x, const Key& y) { return x.frame < y.frame; });
        const int first = static_cast<int>(std::floor(num(a, "first", keys.front().frame)));
        const int last = static_cast<int>(std::ceil(num(a, "last", keys.back().frame)));
        if (last <= first || last - first > 1000) throw ToolError("the frames run first to last, 2 to 1001 of them");
        const Vec3f pivot = a.contains("pivot") ? pointOrLandmark(a["pivot"], marks, "pivot") : marks.middle;
        auto valueAt = [&](double f) {
            if (f <= keys.front().frame) return std::make_pair(keys.front().translate, keys.front().rotate);
            if (f >= keys.back().frame) return std::make_pair(keys.back().translate, keys.back().rotate);
            std::size_t i = 1;
            while (keys[i].frame < f) ++i;
            const Key& k0 = keys[i - 1];
            const Key& k1 = keys[i];
            double t = (f - k0.frame) / std::max(1e-9, k1.frame - k0.frame);
            t = t * t * (3.0 - 2.0 * t);  // (eased: no jolt at the keys)
            const float s = static_cast<float>(t);
            return std::make_pair(k0.translate + (k1.translate - k0.translate) * s, k0.rotate + (k1.rotate - k0.rotate) * s);
        };
        std::vector<std::shared_ptr<const ScalpMesh>> frames;
        for (int f = first; f <= last; ++f) {
            const auto [move, turn] = valueAt(f);
            const CameraMat3 r = rotationFromEulerXYZDegrees({turn.x, turn.y, turn.z});
            std::vector<Vec3f> points = base->points();
            for (Vec3f& p : points) {
                const Vec3f d = p - pivot;
                const Vec3f q(static_cast<float>(r[0][0] * d.x + r[0][1] * d.y + r[0][2] * d.z),
                              static_cast<float>(r[1][0] * d.x + r[1][1] * d.y + r[1][2] * d.z),
                              static_cast<float>(r[2][0] * d.x + r[2][1] * d.y + r[2][2] * d.z));
                p = pivot + q + move;
            }
            frames.push_back(std::make_shared<ScalpMesh>(base->withPoints(std::move(points))));
        }
        const std::string path = files().resolveForWrite(str(a, "path", "anim/" + g.name() + "_scalp.abc"));
        if (!hasExtension(path, ".abc")) throw ToolError("path is an Alembic (.abc) file");
        const double fps = g.framesPerSecond();
        target.writeMeshCache(path, "scalp", frames, frameToSeconds(first, fps), 1.0 / fps);
        AnimationInputs inputs;
        inputs.scalpCache = CacheRef{path, {}};
        target.setAnimation(g, inputs);
        return textResult(withSummary(g, {{"path", files().shown(path)}, {"frames", frames.size()}, {"first", first}, {"last", last},
                                          {"pivot", vec(pivot)},
                                          {"note", "the scalp cache is the groom's animated scalp now (saved with it); simulate steps "
                                                   "through its frames. Not an undo step."}}));
    }

    json simulationStats(AgentGroom& g, const std::string& node, int frame) {
        json s = {{"node", node}};
        for (const SimStatus& st : g.session().simulationStatus()) {
            if (st.node != node) continue;
            s["startFrame"] = st.startFrame;
            s["framesCached"] = st.frames;
            s["stepMs"] = r4(st.stepMs);
            s["baked"] = st.baked;
            if (st.baked) s["bakedFrames"] = {st.bakedFirst, st.bakedLast};
            if (st.bakeStale) s["bakeStale"] = true;
        }
        const GuidesPtr sim = g.session().simulatedFrame(node, frame);
        const std::shared_ptr<const GuideHierarchy> groomed = g.session().authoredHierarchy();
        if (!sim || !groomed) return s;
        const MeshPtr scalp = g.session().scalp();
        Vec3f lo, hi;
        lo = hi = scalp && !scalp->points().empty() ? scalp->points().front() : Vec3f(0, 0, 0);
        if (scalp) {
            for (const Vec3f& p : scalp->points()) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        }
        const float tolerance = 0.002f * (hi - lo).length();
        double stretchSum = 0.0, tipSum = 0.0;
        float stretchMax = 0.0f, tipMax = 0.0f;
        std::size_t guides = 0, points = 0, under = 0;
        for (const Guide& guide : sim->guides()) {
            const HierarchyGuide* rest = groomed->guide(guide.id);
            if (!rest || guide.points.size() < 2 || rest->points.size() < 2) continue;
            float lengthSim = 0.0f, lengthRest = 0.0f;
            for (std::size_t i = 1; i < guide.points.size(); ++i) lengthSim += (guide.points[i] - guide.points[i - 1]).length();
            for (std::size_t i = 1; i < rest->points.size(); ++i) lengthRest += (rest->points[i] - rest->points[i - 1]).length();
            if (lengthRest > 0.0f) {
                const float stretch = std::abs(lengthSim / lengthRest - 1.0f);
                stretchSum += stretch;
                stretchMax = std::max(stretchMax, stretch);
            }
            const float tip = (guide.points.back() - rest->points.back()).length();
            tipSum += tip;
            tipMax = std::max(tipMax, tip);
            if (scalp) {
                for (std::size_t i = 1; i < guide.points.size(); ++i) {
                    const SurfacePoint sp = scalp->closestPoint(guide.points[i]);
                    ++points;
                    if ((guide.points[i] - sp.position).dot(sp.normal) < -tolerance) ++under;
                }
            }
            ++guides;
        }
        if (guides == 0) return s;
        s["guides"] = guides;
        s["stretch"] = {{"mean", r4(stretchSum / static_cast<double>(guides))}, {"max", r4(stretchMax)}};
        s["tipMoveFromGroom"] = {{"mean", r4(tipSum / static_cast<double>(guides))}, {"max", r4(tipMax)}};
        s["pointsUnderScalp"] = under;
        s["pointsChecked"] = points;
        return s;
    }

    ToolResult simulateTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        GroomSession& s = g.session();
        const std::string preset = str(a, "preset");
        if (flag(a, "add", false)) {
            if (!s.simulateNodes().empty()) throw ToolError("the graph has a Simulate node already (" + s.simulateNodes().front() + ")");
            edit(g, "add Simulate", [&](std::string& graph, std::string&) {
                GraphEdit e(graph);
                const std::string before = firstNodeOfType(graph, "Interpolate");
                if (before.empty()) throw ToolError("the graph has no Interpolate node to simulate the guides before");
                const std::string made = e.addNode("Simulate", "simulate", preset.empty() ? std::string() : simulatePresetParamsJson(preset),
                                                   std::nullopt, std::nullopt, fitSize(g));
                // Into Interpolate's guides input: between it and what feeds it guides.
                std::string input;
                for (const json& i : json::parse(e.nodeSpecJson(before)).value("inputs", json::array())) {
                    if (input.empty() && !i.value("param", false) && i.value("type", std::string()) == "Guides") input = i.value("name", std::string());
                }
                if (input.empty()) throw ToolError("Interpolate has no guides input to simulate");
                std::string from, output;
                for (const json& c : connectionsOf(e)) {
                    if (c.value("to", std::string()) == before && c.value("input", std::string()) == input) {
                        from = c.value("from", std::string());
                        output = c.value("output", std::string());
                    }
                }
                if (from.empty()) throw ToolError("nothing feeds Interpolate's guides: add guides first");
                e.connect(from, made, firstInput(e, made), output);
                e.connect(made, before, input);
                graph = e.toJson();
            });
        } else if (!preset.empty()) {
            const json params = json::parse(simulatePresetParamsJson(preset));
            const std::vector<std::string> nodes = s.simulateNodes();
            if (nodes.empty()) throw ToolError("no Simulate node to set the preset on (simulate with add: true makes one)");
            edit(g, "simulate preset " + preset, [&](std::string& graph, std::string&) {
                json doc = json::parse(graph);
                for (json& n : doc["nodes"]) {
                    if (std::find(nodes.begin(), nodes.end(), n.value("name", std::string())) == nodes.end()) continue;
                    for (const auto& [k, v] : params.items()) n["params"][k] = v;
                }
                graph = doc.dump();
            });
        }
        const std::vector<std::string> nodes = s.simulateNodes();
        if (nodes.empty()) {
            throw ToolError("the graph has no Simulate node: call simulate with add: true (it goes before Interpolate), or add_node "
                            "Simulate before interpolate");
        }
        json out = {{"nodes", nodes}};
        if (a.contains("to_frame")) {
            const int to = static_cast<int>(num(a, "to_frame", 1));
            int start = to;
            for (const SimStatus& st : s.simulationStatus()) start = std::min(start, st.startFrame);
            if (to - start > kMaxSimFrames) throw ToolError("at most " + std::to_string(kMaxSimFrames) + " frames in one call");
            const SimRun run = g.simulateTo(to);
            out["framesStepped"] = run.frames;
            out["ms"] = static_cast<long long>(run.milliseconds + 0.5);
            if (!run.errors.empty()) out["errors"] = run.errors;
            g.frame = to;
            out["frame"] = to;
            out = withSummary(g, out);
            json stats = json::array();
            for (const std::string& node : nodes) stats.push_back(simulationStats(g, node, to));
            out["simulation"] = stats;
            out["hint"] = "render_preview at this frame shows it (frame); sim_cache write bakes the frames to a file.";
            return textResult(out);
        }
        return textResult(withSummary(g, out));
    }

    ToolResult simCache(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::vector<std::string> nodes = g.session().simulateNodes();
        std::string node = str(a, "node");
        if (node.empty()) {
            if (nodes.empty()) throw ToolError("the graph has no Simulate node");
            node = nodes.front();
        }
        const std::string op = str(a, "op");
        const std::string path = op == "write" ? files().resolveForWrite(str(a, "path", "caches/" + g.name() + "_" + node + "_sim.abc"))
                                               : files().resolve(str(a, "path"));
        if (!hasExtension(path, ".abc")) throw ToolError("a simulation cache is an Alembic (.abc) file");
        json out = {{"node", node}, {"path", files().shown(path)}};
        if (op == "write") {
            int first = 1;
            for (const SimStatus& st : g.session().simulationStatus()) {
                if (st.node == node) first = st.startFrame;
            }
            first = static_cast<int>(num(a, "first", first));
            const int last = static_cast<int>(num(a, "last", g.frame));
            if (last < first || last - first > kMaxSimFrames) throw ToolError("frames first to last, at most " + std::to_string(kMaxSimFrames));
            out["frames"] = target.bakeSimulation(g, node, path, first, last);
            out["note"] = "baked: the groom reads these frames from the file (saved with it) until the node's settings change";
        } else if (op == "read") {
            target.loadSimulationCache(g, node, path, str(a, "object"));
        } else {
            throw ToolError("op is write or read");
        }
        return textResult(withSummary(g, out));
    }

    // ---- games and stats ----
    // The groom's cards as a game export evaluates them, baked.
    CardBake bakeCardsOf(AgentGroom& g, int resolution, float occlusion) { return g.bakeCards(resolution, occlusion); }

    ToolResult exportCards(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const std::string path = files().resolveForWrite(str(a, "path"));
        const bool binary = hasExtension(path, ".glb");
        if (!binary && !hasExtension(path, ".gltf")) throw ToolError("cards export to .glb or .gltf");
        const int resolution = gameExportResolution(static_cast<int>(num(a, "resolution", 0)));
        const float occlusion = static_cast<float>(num(a, "occlusion", 1.0));
        const CardBake bake = bakeCardsOf(g, resolution, occlusion);
        GltfCardsOptions o;
        o.path = path;
        o.name = fs::u8path(path).stem().u8string();
        o.binary = binary;
        o.resolution = resolution;
        o.occlusion = occlusion;
        o.cap = flag(a, "cap", true);
        o.scale = static_cast<float>(usdMetersPerUnit(g.units()));
        o.zUp = g.upAxis() == "Z";
        GltfCardsSource source;
        source.bake = [&](const std::string&, int, float) { return bake; };
        source.capGroom = [&](MeshPtr* scalp, std::string* whyNot) -> StrandsPtr {
            if (!bake.scalp || !bake.scalp->hasUVs()) {
                *whyNot = "the scalp has no uvs";
                return nullptr;
            }
            ViewRequest request;
            request.density = 1.0f;
            request.time = frameToSeconds(g.frame, g.framesPerSecond());
            request.gameExport = true;
            const ViewResult& view = g.view(request);
            *scalp = view.scalp ? view.scalp : bake.scalp;
            return view.placed();
        };
        const GltfCardsExport done = writeGltfCards(o, g.session().hairLook(), source);
        json written = json::array();
        for (const std::string& f : done.files) written.push_back(files().shown(f));
        return textResult({{"groom", g.name()}, {"files", written}, {"cards", done.cards}, {"meshes", done.meshes},
                           {"warnings", done.warnings}});
    }

    ToolResult exportGame(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        const bool unity = str(a, "engine", "unreal") == "unity";
        GameExportOptions o;
        o.directory = files().resolve(str(a, "folder", "game/" + g.name()));
        std::error_code ec;
        fs::create_directories(fs::u8path(o.directory), ec);
        o.name = str(a, "name", g.name());
        checkName(o.name, "the export's name");
        o.nameGiven = true;
        o.resolution = gameExportResolution(static_cast<int>(num(a, "resolution", 0)));
        o.occlusion = static_cast<float>(num(a, "occlusion", 1.0));
        const CardBake bake = bakeCardsOf(g, o.resolution, o.occlusion);
        ScalpTextures cap;
        bool withCap = bake.scalp && bake.scalp->hasUVs() && bake.strands;
        std::string whyNot = withCap ? std::string() : std::string("no hair cap: the scalp has no uvs");
        if (withCap) {
            try {
                ScalpBakeSettings settings;
                settings.size = o.resolution > 0 ? o.resolution : 2048;
                cap = bakeScalpTextures(*bake.strands, *bake.scalp, settings);
            } catch (const std::exception& e) {
                whyNot = std::string("no scalp maps (") + e.what() + ")";
                withCap = false;
            }
        }
        const EngineCardsExport done = writeEngineCards(bake, g.session().stats(), g.session().hairLook(), withCap ? &cap : nullptr,
                                                        withCap ? bake.scalp.get() : nullptr, whyNot, unity, o);
        json written = json::array();
        for (const std::string& f : done.files()) written.push_back(files().shown(f));
        return textResult({{"groom", g.name()}, {"engine", unity ? "unity" : "unreal"}, {"files", written}, {"manifest", files().shown(done.manifest)},
                           {"warnings", done.warnings}});
    }

    ToolResult statsTool(const json& a) {
        AgentGroom& g = target.groom(str(a, "groom"));
        json out = summary(g, 1.0f, {}, true);
        out["groom"] = g.name();
        out["stats"] = json::parse(g.session().stats().toJson(), nullptr, false);
        return textResult(out);
    }

    // ---- The table ---------------------------------------------------------------------------------
    // ---- render_frame ----
    RenderRequest renderRequestOf(const json& a) {
        const std::vector<RendererInfo> offered = target.renderers();
        if (offered.empty()) throw ToolError("this server renders only previews (render_preview): render_frame runs live in a host");
        RenderRequest r;
        r.groom = target.groom(str(a, "groom")).name();
        r.renderer = str(a, "renderer", offered.front().name);
        const bool known = std::any_of(offered.begin(), offered.end(), [&](const RendererInfo& i) { return i.name == r.renderer; });
        if (!known) {
            std::string names;
            for (const RendererInfo& i : offered) names += (names.empty() ? "" : ", ") + i.name;
            throw ToolError("renderer '" + r.renderer + "' is not one of this host's: " + names);
        }
        const double w = num(a, "width", 640.0), h = num(a, "height", 480.0);
        if (w < 16 || w > 2048 || h < 16 || h > 2048) throw ToolError("width and height are 16 to 2048 pixels");
        r.width = static_cast<int>(w);
        r.height = static_cast<int>(h);
        r.quality = str(a, "quality", "draft");
        if (r.quality != "draft" && r.quality != "preview" && r.quality != "final") throw ToolError("quality is draft, preview or final");
        const double samples = num(a, "samples", 0.0);
        if (samples < 0 || samples > 1024) throw ToolError("samples is 1 to 1024 (0: the quality's)");
        r.samples = static_cast<int>(samples);
        if (a.contains("frame") && a["frame"].is_number()) r.frame = a["frame"].get<double>();
        r.camera = str(a, "camera");
        return r;
    }

    ToolResult renderResultOf(const RenderRequest& r, const RenderedFrame& f) {
        if (f.image.png.empty()) throw ToolError("the " + r.renderer + " render gave no image");
        json info = {{"groom", r.groom}, {"renderer", r.renderer}, {"width", f.image.width}, {"height", f.image.height},
                     {"quality", r.quality}, {"seconds", std::round(f.seconds * 100.0) / 100.0}};
        if (r.samples > 0) info["samples"] = r.samples;
        if (r.frame) info["frame"] = *r.frame;
        if (!f.file.empty()) info["file"] = files().insideRoot(f.file) ? files().shown(f.file) : f.file;
        for (const auto& [k, v] : f.info.items()) info[k] = v;
        return imageResult(f.image.png, info);
    }

    ToolResult renderFrameTool(const json& a) {
        const RenderRequest r = renderRequestOf(a);
        AgentGroom& g = target.groom(r.groom);
        return renderResultOf(r, target.render(g, r));
    }

    void add(const char* name, const char* title, const std::string& description, json schema,
             ToolResult (Impl::*run)(const json&)) {
        tools.push_back({name, title, description, std::move(schema), [run](Impl& self, const json& a) { return (self.*run)(a); }});
    }

    void define() {
        const json groom = prop("string", kGroomDescription);
        const json views = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", previewViewNames()}}},
                            {"description", "Views to draw, laid out in a grid: front, back, left, right (the character's sides; side is "
                                            "left), three_quarter, top. Default [\"three_quarter\"]."}};
        const json placing = {{"after", prop("string", "Insert the new node after this node: it takes that node's output and feeds what it fed.")},
                              {"before", prop("string", "Insert the new node before this node, into its first input.")},
                              {"inputs", {{"type", "object"}, {"additionalProperties", {{"type", "string"}}},
                                          {"description", "Connect inputs explicitly instead: {\"input\": \"node\" or \"node.output\"}."}}}};

        add("list_nodes", "List node types",
            "Lists every node type you can add to a groom's graph, by category in the order a groom is built (sources, guides, "
            "shape, look, color, ...), with a line on what each does. Includes library nodes (compound and script nodes, type "
            "\"node:<id>\"). Filter by category or a search word.",
            objectSchema({{"category", prop("string", "Only this category (its id or title, e.g. \"look\").")},
                          {"search", prop("string", "Only types whose name or description holds this word (e.g. \"curl\").")}}),
            &Impl::listNodes);
        add("describe_node", "Describe a node",
            "Gives a node type's inputs, outputs and params: each param's kind, default, range, unit, choices, whether a map can "
            "drive it, and what it does. With groom and node, describes that node in the groom with its current values.",
            objectSchema({{"type", prop("string", "A node type (from list_nodes), e.g. \"Clump\" or \"node:groomlab.wave\".")},
                          {"groom", groom},
                          {"node", prop("string", "A node of the groom's graph (by name) instead of a type.")}}),
            &Impl::describeNode);
        add("list_presets", "List presets",
            "Lists the graph presets (complete looks: hair basics, curly, braids, tube locks, fur, feathers, cards...) with what "
            "each makes. new_groom and apply_preset take them by name.",
            objectSchema({}), &Impl::listPresets);
        add("list_sample_meshes", "List sample meshes",
            "Lists the meshes that ship with GroomLab to groom on (Nika and Manny, human heads; a wolf for fur; Buddy, a cartoon "
            "character's body). new_groom takes them by name.",
            objectSchema({}), &Impl::listSampleMeshes);
        add("get_docs", "Read the docs",
            "Reads a page of GroomLab's documentation as text (Markdown): how features work, what nodes and params do, recipes. "
            "Without a topic, lists the pages. A topic is a page's name (\"guides-and-brushes\", \"node-graph\", \"expressions\", "
            "\"custom-nodes\", \"ai-agents\") or a few words (\"tube guides\").",
            objectSchema({{"topic", prop("string", "A page's name or words to look for.")},
                          {"max_chars", prop("integer", "Longest text to return (default 40000).")}}),
            &Impl::getDocs);

        add("new_groom", "New groom",
            "Makes a new groom on a mesh and opens it by name: the default hair graph (Scalp, Scatter, Interpolate, Length, Noise, "
            "HairColor, Width, Resample, Output), or a preset's. The mesh is a sample's name (list_sample_meshes) or an .obj or "
            ".abc file inside the server's folder. Answers with the scalp's bounds and a summary of the groom.",
            objectSchema({{"name", prop("string", "A name for the groom (letters, digits, _ - .), used by every other tool.")},
                          {"mesh", prop("string", "A sample mesh's name (\"nika\", \"manny\", \"wolf\", \"buddy\") or a mesh file's path (.obj, .abc).")},
                          {"object", prop("string", "The mesh's name inside an Alembic file (default: its first mesh).")},
                          {"preset", prop("string", "A preset to start from (list_presets), instead of the default graph.")},
                          {"scalp", enumProp({"whole", "from_landmarks"},
                                             "whole (default): hair over the whole mesh until density is painted; from_landmarks: on a "
                                             "whole head, Scatter's density 1 above the hairline the face's landmarks give, 0 on the face, "
                                             "ears and neck (scalp_from_landmarks).")}},
                         {"name", "mesh"}),
            &Impl::newGroom);
        add("scalp_from_landmarks", "Scalp from the face's landmarks",
            "On a whole head (face and neck in the mesh): paints the vertex map Scatter's density reads (scalpRegion, made and "
            "linked when it reads none) 1 above a hairline through the forehead's middle, the temples, over the ears and down "
            "to the nape (from the facial landmarks mesh_landmarks names), 0 on the face, ears and neck. One undo step. "
            "Heuristic: check with render_preview and fix with paint_map.",
            objectSchema({{"groom", groom}, {"softness", prop("number", "The hairline's blend, a share of the face's lower third (default 0.15).")}}),
            &Impl::scalpFromLandmarks);
        add("open_groom", "Open a groom file",
            "Opens a .groom file (from Maya, the app, Blender or save_groom) by name, to change, preview or export it.",
            objectSchema({{"path", prop("string", "The .groom file, relative to the server's folder.")},
                          {"name", prop("string", "The name to open it under (default: the file's name).")}},
                         {"path"}),
            &Impl::openGroom);
        add("list_grooms", "List open grooms", "Lists the grooms open in this session.", objectSchema({}), &Impl::listGrooms);
        add("apply_preset", "Apply a preset",
            "Replaces the groom's graph with a preset's (mode replace, the default), its sizes fitted to this scalp; or inserts "
            "the preset's look as one compound node after a node of the graph (mode insert), keeping the rest.",
            objectSchema({{"groom", groom},
                          {"preset", prop("string", "A preset's name (list_presets) or a preset .json file inside the server's folder.")},
                          {"mode", enumProp({"replace", "insert"}, "replace (default) or insert.")},
                          {"after", prop("string", "insert: the node the look goes after (e.g. \"interpolate\").")}},
                         {"preset"}),
            &Impl::applyPresetTool);
        add("get_graph", "Get the graph",
            "Gives the groom's node graph: each node's name, type, the params it sets away from their defaults and what feeds its "
            "inputs (\"input\": \"node\" or \"node.output\"), and the output node. full: the whole graph JSON as stored.",
            objectSchema({{"groom", groom}, {"full", prop("boolean", "The whole graph JSON, every param written.")}}),
            &Impl::getGraph);
        {
            json p = {{"groom", groom},
                      {"type", prop("string", "The node type (list_nodes), e.g. \"Curl\", or a library node \"node:<id>\".")},
                      {"name", prop("string", "The new node's name (default: from its type, made unique).")},
                      {"params", prop("object", "Params to set on it: {\"param\": value}. Lengths are scene units of this scalp.")}};
            p.update(placing);
            add("add_node", "Add a node",
                "Adds a node to the groom's graph. Place it with after (the usual way: \"after\": \"noise\" puts it in the chain "
                "after noise), before, or explicit inputs; with none of them it goes just before the output when it fits there. "
                "Defaults that are lengths are fitted to the scalp's size.",
                objectSchema(p, {"type"}), &Impl::addNode);
        }
        add("remove_node", "Remove a node",
            "Removes a node from the graph. By default what fed it is connected to what it fed, so the chain stays whole.",
            objectSchema({{"groom", groom}, {"node", prop("string", "The node's name.")},
                          {"reconnect", prop("boolean", "Join what fed the node to what it fed (default true).")}},
                         {"node"}),
            &Impl::removeNode);
        add("connect", "Connect nodes",
            "Connects a node's output to another node's input, replacing what fed that input. Types must fit (Strands, Guides, "
            "Mesh, Field...): describe_node lists a node's inputs.",
            objectSchema({{"groom", groom}, {"from", prop("string", "The source node (\"node\" or \"node.output\").")},
                          {"to", prop("string", "The node to feed.")}, {"input", prop("string", "Its input's name.")},
                          {"output", prop("string", "The source's output by name (default: its first).")}},
                         {"from", "to", "input"}),
            &Impl::connect);
        add("disconnect", "Disconnect an input", "Removes what feeds a node's input.",
            objectSchema({{"groom", groom}, {"to", prop("string", "The node.")}, {"input", prop("string", "Its input's name.")}},
                         {"to", "input"}),
            &Impl::disconnect);
        add("set_params", "Set params",
            "Sets several params of one node at once, checked against its spec (kind, range, choices). Numbers, booleans, "
            "strings, [x, y, z] vectors and colors, ramps as [[position, value], ...]; a mappable param also takes a map object "
            "such as {\"vertexMap\": \"density\"}, or an expression (SeExpr, as XGen's; check it first with check_expression): "
            "{\"expression\": \"0.5 + 0.5 * noise($P * 4)\", \"min\": 0, \"max\": 2} (its 0-1 value mapped onto min..max). "
            "Examples to adapt: \"smoothstep($y, 5, 12)\" more toward the top; \"1 - $t\" fading root to tip (per point); "
            "\"$rand > 0.9 ? 1 : 0.4\" a few strands apart; \"map('density')\" a painted map. Answers with the groom's "
            "summary after the change.",
            objectSchema({{"groom", groom}, {"node", prop("string", "The node's name (get_graph).")},
                          {"params", prop("object", "{\"param\": value, ...}")}},
                         {"node", "params"}),
            &Impl::setParams);
        add("sample_scalp", "Sample scalp points",
            "Gives points spread evenly over the scalp with their normals, to place guide roots on. Filter by how much the "
            "surface faces up (min_up: 0.3 keeps the top of a head) or a box.",
            objectSchema({{"groom", groom}, {"count", prop("integer", "How many points (1-500, default 40).")},
                          {"min_up", prop("number", "Keep points whose normal points up at least this much (-1 to 1).")},
                          {"box_min", point3()}, {"box_max", point3()}}),
            &Impl::sampleScalp);
        add("add_guides", "Add guides",
            "Adds guide curves that shape the hair (Interpolate follows them). Each guide is either {\"points\": [[x,y,z], ...]} "
            "in world space, root first (its root is moved onto the scalp unless snap is false), or {\"root\": [x,y,z], "
            "\"length\": L} grown from the scalp point nearest root along its normal (or \"direction\"), with \"segments\" "
            "(default 8) and \"droop\" (0 straight .. 1 bent down by gravity). Optionally a tube for all of them.",
            objectSchema({{"groom", groom},
                          {"guides", {{"type", "array"}, {"description", "The guides: objects as described, or lists of points."},
                                      {"items", {{"type", json::array({"object", "array"})}}}}},
                          {"group", prop("string", "The guide group to add to (default \"guides\"; made when missing).")},
                          {"snap", prop("boolean", "Move each points guide so its root lies on the scalp (default true).")},
                          {"tube", objectSchema(tubeProperties())}},
                         {"guides"}),
            &Impl::addGuides);
        add("get_guides", "List guides",
            "Lists the groom's guide groups and guides: each guide's id, root and tip, length, points and tube.",
            objectSchema({{"groom", groom}, {"group", prop("string", "Only this group's guides.")},
                          {"limit", prop("integer", "List at most this many guides (default 100).")}}),
            &Impl::getGuides);
        {
            json p = {{"groom", groom},
                      {"guides", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Guide ids (get_guides)."}}},
                      {"group", prop("string", "Every guide of this group instead.")}};
            p.update(tubeProperties());
            add("set_tube", "Shape guide tubes",
                "Sets the tube of guides (by ids, a group, or all): the cross-section shape, its width, aspect (flatness) and "
                "twist, each one number or a list root to tip. Tubes make anime and cartoon locks: with Interpolate's fillTubes "
                "on (the tube_locks preset) strands fill each tube to the tip. Only what is given changes.",
                objectSchema(p), &Impl::setTube);
        }
        {
            json p = {{"groom", groom},
                      {"label", prop("string", "The node's label (\"Wave\"); its id and name come from it.")},
                      {"script", prop("string", "The script: per strand { ... } and/or per point { ... } blocks in GroomLab's expression "
                                                "language. Assign P (point), width, color or channel.<name> (each listed in writes); read "
                                                "params as $name; t is 0 at the root, 1 at the tip; rand is per strand. get_docs "
                                                "\"custom-nodes\" has the language.")},
                      {"params", {{"type", "array"}, {"items", {{"type", "object"}}},
                                  {"description", "Header params: {\"name\", \"type\": number|length|int|bool|enum|color|ramp, "
                                                  "\"default\", \"min\", \"max\", \"label\", \"description\", \"mappable\"}."}}},
                      {"writes", {{"type", "array"}, {"items", {{"type", "string"}}},
                                  {"description", "What the script writes: \"P\", \"width\", \"color\", \"channel.<name>\" (default [\"P\"])."}}},
                      {"inputs", {{"type", "array"}, {"items", {{"type", "object"}}},
                                  {"description", "Inputs (default strands only); add {\"name\": \"guides\", \"type\": \"Guides\", \"optional\": true} for guidePoint()."}}},
                      {"description", prop("string", "A line on what it does.")},
                      {"category", prop("string", "Its Add Node menu category.")},
                      {"definition", prop("object", "A whole .glnode script node file instead of the fields above.")},
                      {"name", prop("string", "The node's name in the graph.")},
                      {"node", prop("string", "An existing script node to change in place instead of adding one.")}};
            p.update(placing);
            add("add_script_node", "Add a script node",
                "Writes a node of your own as a short script and adds it to the graph (placed like add_node), or changes a script "
                "node in place (node). The script is checked first: errors come back with their line and column and nothing "
                "changes, so fix and call again.",
                objectSchema(p), &Impl::addScriptNode);
        }
        add("save_node", "Save a node file",
            "Saves a script or compound node of the groom as a .glnode file inside the server's folder, to share or import into "
            "GroomLab's node library.",
            objectSchema({{"groom", groom}, {"node", prop("string", "The node's name.")},
                          {"path", prop("string", "The file (default <id>.glnode).")}},
                         {"node"}),
            &Impl::saveNode);
        add("undo", "Undo", "Undoes the last changes to a groom: its graph, guides or maps (64 steps are kept; redo does them again).",
            objectSchema({{"groom", groom}, {"steps", prop("integer", "How many changes to undo (default 1).")}}), &Impl::undoTool);
        add("redo", "Redo", "Redoes changes undo took back (until a new change is made).",
            objectSchema({{"groom", groom}, {"steps", prop("integer", "How many changes to redo (default 1).")}}), &Impl::redoTool);
        add("history", "Undo history",
            "Lists the groom's changes in order, each done or undone (undo takes back the last done, redo does the first undone "
            "again), and what undo and redo would take.",
            objectSchema({{"groom", groom}}), &Impl::historyTool);

        // ---- groom by hand ----
        const json point = pointOrName("[x, y, z] in world space, or a landmark's name (scalp_landmarks): \"crown\", \"nape\"...");
        const json pathProps = {
            {"points", {{"type", "array"}, {"items", {{"type", json::array({"array", "string"})}}},
                        {"description", "The path as world points [[x, y, z], ...] (or landmark names), used as given."}}},
            {"from", point},
            {"to", point},
            {"via", {{"type", "array"}, {"items", {{"type", json::array({"array", "string"})}}},
                     {"description", "Points or landmarks the path passes through between from and to, in order."}}},
            {"offset", prop("number", "from/to paths: lifted this far off the scalp along its normal (scene units, default 0: on "
                                      "the scalp, where the guides' roots are; lift it to reach further along them).")},
            {"spacing", prop("number", "from/to paths: the samples' spacing along the surface (default a quarter of the radius; "
                                       "place: the radius, one guide per sample).")},
            {"radius", prop("number", "The brush sphere's radius in scene units (default 0.12 x the scalp's size).")},
            {"strength", prop("number", "Effect per stamp, signed (default 1). Comb: the share of the drag guides follow; negative "
                                        "length or lift reverses them; paint: how far toward value.")},
            {"hardness", prop("number", "0 (default) falls off smoothly to the edge, 1 full strength to the edge.")},
            {"symmetry", enumProp({"none", "x", "y", "z", "topology"},
                                  "Repeat the stroke mirrored: across the world plane x/y/z = symmetry_center, or across the "
                                  "scalp's own left and right (topology). Default none.")},
            {"symmetry_center", prop("number", "Where the mirror plane is (default 0).")}};
        add("scalp_landmarks", "Scalp landmarks",
            "Gives named points on the scalp (top, crown, front_hairline, nape, back, left/right_temple, left/right_ear), each "
            "with the surface's outward normal, and the groom's directions (up, down, front, back, left, right: the character's, "
            "facing front) and bounds. Approximate (from the scalp's bounds and shape), so check with render_preview. Strokes, "
            "map painting and selection take the names as points: \"comb from crown to nape\". With at: the scalp points "
            "nearest the points given and the outward normal there.",
            objectSchema({{"groom", groom},
                          {"at", {{"type", "array"}, {"items", {{"type", json::array({"array", "string"})}}},
                                  {"description", "Points ([x, y, z] or landmarks): each one's nearest scalp point and the outward "
                                                  "normal there."}}}}),
            &Impl::scalpLandmarksTool);
        {
            json p = {{"groom", groom},
                      {"mode", enumProp({"comb", "smooth", "straighten", "lift", "length", "cut", "twist", "attract", "slide", "erase",
                                         "shape", "clump", "paint", "freeze", "unfreeze", "place", "draw", "part", "flow"},
                                        "comb: drag guides along the path (roots stay, lengths kept). smooth: take kinks out. "
                                        "straighten: toward straight. lift: stand up from the scalp (negative: lay down). length: "
                                        "grow (negative: shorten). cut: trim where the path crosses. twist: swing about the root "
                                        "normal (signed). attract: pull tips together (negative: apart). slide: move roots along the "
                                        "scalp. erase: remove guides. shape: paint tubes (tube_channel). clump / paint: paint a guide "
                                        "layer (clump, or attribute) toward value. freeze / unfreeze: hold guides so no brush moves "
                                        "them, or let go. place: a guide at each path point (copying nearby guides' shape, or "
                                        "length tall). draw: one guide along the path. part: a part through the path. flow: flow "
                                        "arrows along the path.")},
                      {"level", prop("integer", "The hierarchy level brushed (0 primary guides, default; -1 every level).")},
                      {"group", prop("string", "Only this guide group (place and draw: the group new guides go in).")},
                      {"selected_only", prop("boolean", "Only the guides select_guides picked.")},
                      {"propagate", prop("boolean", "Also brush locked children of the guides touched.")},
                      {"collide", prop("boolean", "Keep guides out of the scalp (default true).")},
                      {"stretch", prop("boolean", "comb: guides stretch with the drag instead of keeping their length.")},
                      {"length", prop("number", "place and draw: a new guide's length where no guides are near to copy (default 0.4 x "
                                                "the scalp's size).")},
                      {"segments", prop("integer", "place and draw: a new guide's segments (default 12).")},
                      {"match_neighbors", prop("boolean", "place: copy the nearest guides' shape and length (default true).")},
                      {"tube_channel", enumProp({"width", "flatness", "twist"}, "shape: what it paints on the tubes (default width).")},
                      {"attribute", prop("string", "paint: the guide layer painted (clump paints \"clump\").")},
                      {"value", prop("number", "paint, clump: the value painted toward (default 1).")},
                      {"paint_mode", enumProp({"replace", "add", "subtract", "min", "max"}, "paint: how values combine (default replace).")},
                      {"targets", prop("string", "What shaping brushes act on: \"guides\" (default), \"parts\", \"flow\" or several "
                                                 "joined by commas.")},
                      {"part_name", prop("string", "part: its name (made unique; default part).")},
                      {"flow_sink", enumProp({"", "whorl", "whorlcw", "converge", "part"}, "flow: a sink of this kind at each path point "
                                                                                       "instead of arrows along it.")}};
            p.update(pathProps);
            add("brush_stroke", "Brush stroke",
                "Runs one of GroomLab's brushes along a path, as an artist's stroke: one undo step. Give the path as world "
                "points, or from/to (and via) as landmarks or points: it then runs along the scalp's surface. The brush is a sphere "
                "of radius moved along the path; it acts on guide points inside it (on the scalp: near the roots; raise offset to "
                "reach further out). Answers with what changed (guides touched, added, removed, how far points moved, lengths) "
                "and the groom's summary; check the look with render_preview and measure_guides.",
                objectSchema(p, {"mode"}), &Impl::brushStroke);
        }
        add("select_guides", "Select guides",
            "Picks guides for brush_stroke (selected_only), guide_op and measure_guides: by ids, by a sphere (center: a point "
            "or landmark, radius) around their roots, or a whole group or level. mode replace (default), add, remove or clear.",
            objectSchema({{"groom", groom},
                          {"mode", enumProp({"replace", "add", "remove", "clear"}, "How the pick joins the selection (default replace).")},
                          {"ids", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Guide ids (get_guides)."}}},
                          {"center", point},
                          {"radius", prop("number", "The sphere's radius (default 0.12 x the scalp's size).")},
                          {"roots_only", prop("boolean", "A sphere picks guides by their root (default true), else by any point.")},
                          {"group", prop("string", "Only (or all of) this group's guides.")},
                          {"level", prop("integer", "Only (or all of) this level's guides (0 primary).")}}),
            &Impl::selectGuides);
        add("guide_op", "Edit guides",
            "Edits guides as Groom Tools' Edit Guides menu does, one undo step, on guides (ids) or the selection "
            "(select_guides). Ops: delete_guides, lock / unlock (children follow their parents), freeze / unfreeze, unfreeze_all, "
            "set_attribute (name, value), mirror_all / mirror_group (axis, center, from_positive), mirror_shapes (symmetry; side "
            "0 the guides given, 1 / -1 a whole side over the other, with group), move_to_group / duplicate_to_group (group), "
            "new_group_from (name), add_group (name, parent), remove_group, promote_group, demote_group (group under parent), "
            "reparent (parent_guide), resample (segments; group_wide, level, by_length), set_group_segments / "
            "reset_group_segments (group, segments), invert_layer / remove_layer (name); generate_children (count children in "
            "each parent's tube: the guides given, else every guide of group); edit_tubes (tube: the guides given, else group's).",
            objectSchema({{"groom", groom},
                          {"op", enumProp({"delete_guides", "lock", "unlock", "freeze", "unfreeze", "unfreeze_all", "set_attribute",
                                           "mirror_all", "mirror_group", "mirror_shapes", "move_to_group", "duplicate_to_group",
                                           "new_group_from", "add_group", "remove_group", "promote_group", "demote_group", "reparent",
                                           "resample", "set_group_segments", "reset_group_segments", "invert_layer", "remove_layer",
                                           "generate_children", "edit_tubes"},
                                          "The edit.")},
                          {"guides", {{"type", "array"}, {"items", {{"type", "integer"}}},
                                      {"description", "Guide ids (default: the selection)."}}},
                          {"group", prop("string", "The group the op works on or moves guides to.")},
                          {"name", prop("string", "A new group's name, a layer's name.")},
                          {"parent", prop("string", "add_group, demote_group: the parent group.")},
                          {"value", prop("number", "set_attribute: the value (default 1).")},
                          {"axis", enumProp({"x", "y", "z"}, "mirror_all / mirror_group: the mirror plane's axis (default x).")},
                          {"center", prop("number", "The mirror plane's place on its axis (default 0).")},
                          {"from_positive", prop("boolean", "Mirror the positive side over the negative (default true).")},
                          {"symmetry", enumProp({"x", "y", "z", "topology"}, "mirror_shapes: the symmetry (default x).")},
                          {"side", prop("integer", "mirror_shapes: 0 the guides given take their mirror's shape; 1 or -1 that side over the other.")},
                          {"segments", prop("integer", "resample, set_group_segments: segments per guide.")},
                          {"group_wide", prop("boolean", "resample: whole groups.")},
                          {"level", prop("integer", "resample: the level.")},
                          {"by_length", prop("boolean", "resample: segments by length.")},
                          {"parent_guide", prop("integer", "reparent: the new parent guide's id.")},
                          {"count", prop("integer", "generate_children: children per parent (default 5).")},
                          {"child_group", prop("string", "generate_children: the child group (default the group's own).")},
                          {"root_spread", prop("number", "generate_children: share of the parent's tube the roots spread over (0.8).")},
                          {"tip_spread", prop("number", "generate_children: share the tips spread over (1).")},
                          {"jitter", prop("number", "generate_children: random tip offset (0.1).")},
                          {"twist_degrees", prop("number", "generate_children: turn about the parent, root to tip.")},
                          {"child_tube_scale", prop("number", "generate_children: a child's tube width over its parent's (0.35).")},
                          {"seed", prop("integer", "generate_children: the random seed.")},
                          {"split", prop("boolean", "generate_children: cut the parent's tube into count slices instead.")},
                          {"tube", objectSchema(tubeEditProperties())}},
                         {"op"}),
            &Impl::guideOpTool);
        {
            json p = {{"groom", groom},
                      {"map", prop("string", "The map's name (made when missing, at initial).")},
                      {"for", prop("string", "\"node.param\" the map drives (e.g. \"scatter.density\", \"clump.amount\"): the map it "
                                             "reads, or one made white and linked so it multiplies the param.")},
                      {"kind", enumProp({"vertices", "guides"}, "A new map's kind: vertices (a value per scalp vertex, default) or "
                                                                 "guides (a value per guide).")},
                      {"initial", prop("number", "A new map's value everywhere (default 0; one made for a param starts at 1).")},
                      {"fill", prop("number", "Fill the whole map with this value (0-1) before painting.")},
                      {"value", prop("number", "What the stroke paints toward, 0-1 (default 1).")},
                      {"paint_mode", enumProp({"replace", "add", "subtract", "min", "max"}, "How values combine (default replace).")}};
            p.update(pathProps);
            p["spacing"] = prop("number", "from/to paths: the samples' spacing (default a quarter of the radius).");
            add("paint_map", "Paint a map",
                "Makes, fills and paints a scalp map, one undo step: density (\"for\": \"scatter.density\": 0 where no hair grows), "
                "masks for clumps, lengths, colors... Name the map (map) or the param it drives (for: the map linked to it, made "
                "white when missing); fill sets it everywhere; a path (points, or from/to along the scalp, as brush_stroke) paints "
                "toward value with the radius' falloff. Answers with the map's values (min, mean, max) and the groom's summary.",
                objectSchema(p), &Impl::paintMap);
        }


        add("evaluate", "Evaluate",
            "Evaluates the groom and reports strand and point counts, guides, bounds, errors, the nodes' notes and the slowest "
            "nodes' milliseconds. Every changing tool already answers with a summary; this one adds timings, a frame, a density "
            "and a node to look at instead of the output.",
            objectSchema({{"groom", groom}, {"frame", prop("number", "The frame (default the groom's, 1).")},
                          {"density", prop("number", "Share of strands to evaluate, 0-1 (default 1).")},
                          {"node", prop("string", "Evaluate up to this node instead of the output.")}}),
            &Impl::evaluate);
        add("render_preview", "Render a preview",
            "Draws the groom as a PNG image you can look at (a turntable, a look, a photo's camera to compare with it): the scalp "
            "shaded and the strands as lines in the hair's color (and "
            "instances with a built-in mesh: beads, cuffs, hair ties), from one or several views in a grid. A simple CPU preview (no lighting model or shadows), not a render: use it to check "
            "shape, length, direction and color. Big grooms are thinned to max_strands.",
            objectSchema({{"groom", groom}, {"views", views},
                          {"size", prop("integer", "The image's width in pixels (64-1024, default 512).")},
                          {"max_strands", prop("integer", "Draw at most about this many strands (default 50000).")},
                          {"frame", prop("number", "The frame.")}, {"density", prop("number", "Share of strands, 0-1 (default 1).")},
                          {"node", prop("string", "Draw this node's strands instead of the output's.")},
                          {"guides", prop("boolean", "Draw the guides over the hair (orange, roots yellow; hidden only behind the scalp).")},
                          {"compare", prop("boolean", "Two images: the groom before the last change (what undo would take back), "
                                                      "then now.")},
                          {"look", enumProp(previewLookNames(), "studio (default: a key light), flat (colors unshaded: color and "
                                                                "coverage), rim (lit from behind: outline, flyaways), clay (one gray: "
                                                                "the shape alone).")},
                          {"turntable", prop("integer", "Instead of views: 2-12 frames round the groom (a contact sheet).")},
                          {"camera", objectSchema(cameraProperties())},
                          {"tubes", prop("boolean", "Draw the guides' tubes as cyan wires (as tube_levels view shows them).")},
                          {"viewport", prop("boolean", "Live in a host (Maya...): its own viewport as the artist sees it now, "
                                                       "instead of the CPU preview (the other options aside from size are not read).")},
                          {"save_to", prop("string", "Also write the PNG to this file inside the server's folder.")},
                          {"beside", prop("string", "A lined-up photo's name: the groom through that photo's camera next to the "
                                                    "photo itself, the same size (left the photo, right the groom), to judge the look.")},
                          {"blend", prop("number", "With beside: one image, the groom blended over the photo instead (0 the photo, "
                                                   "1 the groom; 0.5 halfway).")},
                          {"label_guides", {{"description", "Draw guides labelled with their IDs at their tips, to pick them by "
                                                            "number (guide_op, brush_stroke): true (all, at most 400), a group's "
                                                            "name, or \"selection\" (select_guides')."},
                                            {"type", json::array({"boolean", "string"})}}}}),
            &Impl::renderPreviewTool);
        add("checkpoint", "Checkpoint a groom",
            "Keeps the groom's whole state under a name (graph, guides, parts, flow, maps, sculpts, reference photos, scalp) for "
            "this session, to try a variant and come back with restore instead of undoing step by step. No name: lists them.",
            objectSchema({{"groom", groom}, {"name", prop("string", "The checkpoint's name (a name used already is replaced).")}}),
            &Impl::checkpointTool);
        add("restore", "Restore a checkpoint",
            "Puts the groom back as a checkpoint kept it: one undo step (undo takes the restore back). Says what changed.",
            objectSchema({{"groom", groom}, {"name", prop("string", "The checkpoint's name.")}}, {"name"}),
            &Impl::restoreTool);
        add("batch", "Run tool calls in a row",
            "Runs a list of tool calls in order in one request, stopping at the first error, and answers with each call's answer "
            "(their images too with images: true). Each call is its own undo step as alone. Use it to iterate in fewer round "
            "trips: set_params, then evaluate, then compare_to_photo.",
            objectSchema({{"calls", {{"type", "array"}, {"description", "[{\"tool\": \"set_params\", \"arguments\": {...}}, ...] (1-64)."},
                                     {"items", {{"type", "object"}}}}},
                          {"images", prop("boolean", "Send the calls' images too (default false: their count only).")}},
                         {"calls"}),
            &Impl::batchTool);
        add("show_map", "Show a map over the scalp",
            "Draws the scalp from a few views colored by a map's values or an expression's (a heatmap with a legend), to see "
            "where a density, length or clump map is high or low before using it.",
            objectSchema({{"groom", groom}, {"map", prop("string", "A map's name (as map(\"name\") reads it: a vertex map, a guide layer...).")},
                          {"component", enumProp({"r", "g", "b", "a", "luminance"}, "Which of the map's channels (default luminance).")},
                          {"expression", prop("string", "An expression instead (as check_expression takes it).")},
                          {"variables", prop("object", "The expression's own constants.")},
                          {"node", prop("string", "The strands this node gives (default the output) for strand variables.")},
                          {"views", views}, {"size", prop("integer", "The image's width (64-1024, default 512).")},
                          {"min", prop("number", "The legend's low end (default the values' least).")},
                          {"max", prop("number", "The legend's high end (default the values' most).")}}),
            &Impl::showMapTool);
        add("suggest_pairs", "Suggest landmark pairs on a photo",
            "For the face's named landmarks (all, or the ones listed), where each lands in a photo through its camera (or a view "
            "hint's guess when it is not lined up yet), snapped to the strongest image feature nearby, with a confidence and a "
            "zoomed picture of each: confirm or correct the picks, then align_photo with them.",
            objectSchema({{"groom", groom}, {"photo", prop("string", "The photo's name.")},
                          {"landmarks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Landmark names (default all the face's)."}}},
                          {"view", prop("string", "Predict from this side's camera instead of the photo's (front, right, "
                                                  "three_quarter_front_left...): for a photo not lined up yet.")},
                          {"focal_mm", prop("number", "With view: the lens (default 50).")},
                          {"window", prop("number", "How far round the prediction to look, pixels (default 2.5% of the photo's "
                                                    "diagonal lined up, 6% from a view hint).")},
                          {"zoom", prop("integer", "The pictures' zoom, 1-8 (default 4).")}},
                         {"photo"}),
            &Impl::suggestPairsTool);
        // The host's own renderers, where it has any (live in Maya, the app, Blender).
        if (const std::vector<RendererInfo> offered = target.renderers(); !offered.empty()) {
            std::vector<std::string> names;
            std::string which;
            for (const RendererInfo& i : offered) {
                names.push_back(i.name);
                which += (which.empty() ? "" : "; ") + i.name + ": " + i.label;
            }
            add("render_frame", "Render a frame",
                "Renders the groom with one of this host's own renderers and gives back the image: the look the artist renders "
                "(its shading, lights and shadows), unlike render_preview's quick CPU drawing. Renderers here: " + which +
                    ". It takes seconds to minutes: iterate at the defaults (640x480, draft) and render final only to check. A "
                    "renderer that is not installed says so.",
                objectSchema({{"groom", groom}, {"renderer", enumProp(names, "The renderer (default the first listed).")},
                              {"width", prop("integer", "Pixels across, 16-2048 (default 640).")},
                              {"height", prop("integer", "Pixels down, 16-2048 (default 480).")},
                              {"quality", enumProp({"draft", "preview", "final"},
                                                   "How many samples the renderer takes (default draft: fast and noisy).")},
                              {"samples", prop("integer", "The renderer's samples instead of the quality's (1-1024).")},
                              {"frame", prop("number", "Render this frame (default the frame shown).")},
                              {"camera", prop("string", "The host's camera by name (default its render or studio camera, else the "
                                                        "view's).")}}),
                &Impl::renderFrameTool);
        }
        add("measure_guides", "Measure guides",
            "Numbers that describe the guides, per group (or the selection): count, length min/mean/max, the mean root-to-tip "
            "direction (and in words: \"down, back\"), straightness (1 straight), curl (degrees turned root to tip), how far "
            "they lean from the scalp's normal at the root, and the roots' and tips' middle. Check a stroke's effect with it.",
            objectSchema({{"groom", groom}, {"group", prop("string", "Only this group.")},
                          {"level", prop("integer", "Only this level's groups (0 primary).")},
                          {"selection", prop("boolean", "The selection (select_guides) as one set instead.")}}),
            &Impl::measureGuidesTool);

        // ---- reference photos ----
        const json photoName = prop("string", "The photo's name (default: the only one).");
        const json pixelPoints = {{"type", "array"}, {"items", {{"type", "array"}}},
                                  {"description", "Points in the photo's pixels, [[u, v], ...] (x right, y down from the top-left)."}};
        add("add_photo", "Add a reference photo",
            "Adds a photo (PNG, JPEG... inside the server's folder) to the groom as a reference, one undo step, and shows it under a "
            "labelled pixel grid. Then line it up with the mesh (mesh_landmarks, align_photo) and grow guides from it "
            "(guides_from_photos). Kept in the .groom as the app's Reference window keeps photos.",
            objectSchema({{"groom", groom}, {"path", prop("string", "The image file.")},
                          {"name", prop("string", "Its name (default: the file's).")}},
                         {"path"}),
            &Impl::addPhoto);
        add("photo_view", "Look at a photo",
            "Shows a reference photo under a pixel grid labelled in its own pixels (read features' coordinates off it), with its "
            "landmarks numbered, flow arrows and mask touch-ups. region: a part of it enlarged under a fine grid (10 px), to read "
            "pixels exactly. analysis (a lined-up photo): the hair mask and strand flow guides_from_photos would read, tinted over it.",
            objectSchema({{"groom", groom}, {"photo", photoName},
                          {"grid", prop("integer", "Grid spacing in photo pixels (default: about 10 lines across; with region 5-25).")},
                          {"size", prop("integer", "The picture's larger side in pixels (default 768).")},
                          {"region", {{"type", "array"}, {"items", {{"type", "number"}}},
                                      {"description", "Zoom: [x0, y0, x1, y1] of the photo's pixels, enlarged under a fine grid labelled "
                                                      "in the photo's pixels, to read a feature's pixel exactly."}}},
                          {"analysis", prop("boolean", "Show the hair mask and flow it reads.")}}),
            &Impl::photoView);
        add("mesh_landmarks", "Numbered mesh points",
            "Renders the scalp mesh from a side (front, back, left, right, top, three_quarter; or photo: through the photo's "
            "camera) with numbered points you can find on a photo too (its outline's extremes, the nearest point, then spread "
            "out), and gives each point's 3D position. On a whole head (face included) also the facial landmarks found from its "
            "shape, by name (named: nose_tip, chin, left/right_eye and their inner/outer corners, left/right_mouth_corner, "
            "left/right_ear_top, left/right_earlobe, hairline_middle, crown, nape; heuristic, each with a confidence): align_photo "
            "takes the names as points. Pair them with the photo's pixels in align_photo. Give the photo to render at its size.",
            objectSchema({{"groom", groom}, {"photo", photoName},
                          {"view", enumProp([] {
                              std::vector<std::string> v = viewCameraNames();
                              v.push_back("photo");
                              return v;
                          }(), "The side (default front; the character's left and right).")},
                          {"count", prop("integer", "How many points (4-80, default 24).")},
                          {"focal_mm", prop("number", "The view's focal length on a 36 mm film back (default 50).")},
                          {"size", prop("integer", "The picture's larger side (default 768).")}}),
            &Impl::meshLandmarks);
        add("align_photo", "Line a photo up",
            "Solves the photo's camera from pairs of a photo pixel and the same feature's point on the mesh (camera_solve: 4 pairs "
            "with the focal length kept; 6 or more spread in depth can solve it too, focalSolved says whether they did). One undo "
            "step. A second solve with what a photo shoot is like (the view's angle, level, the shoot's focal and distance from the "
            "photos lined up already, a portrait lens when the pairs cannot tell the focal) is kept unless its pairs fit clearly "
            "worse: a lone profile cannot tilt 30 degrees (priors in the answer). Several photos: align_photos solves them "
            "together. Points: [x, y, z], or a landmark's name (mesh_landmarks' named facial landmarks: nose_tip, left_earlobe...). "
            "view: the side the photo was taken from (a start for the solve: profiles and backs line up far better with it). "
            "refine: then tighten the camera so the mesh's outline lies on the photo's where neither is hair (jaw, chin, neck, "
            "ears; the pairs kept in the fit), reported before and after. Answers with the reprojection error (pixels; each "
            "pair's, and pairs that disagree) and a picture of the mesh's outline over the photo through the solved camera.",
            objectSchema({{"groom", groom}, {"photo", photoName},
                          {"pairs", {{"type", "array"}, {"items", {{"type", "object"}}},
                                     {"description", "[{\"pixel\": [u, v], \"point\": [x, y, z] or a landmark's name}, ...]."}}},
                          {"append", prop("boolean", "Add to the photo's pairs (default: replace them).")},
                          {"keep_focal", prop("boolean", "Keep the focal length (default: solved with 6 pairs or more when they can tell it).")},
                          {"focal_mm", prop("number", "The focal length to start from or keep (36 mm film back; default 50 or the camera's).")},
                          {"view", enumProp(viewCameraNames(), "The side the photo shows (the character's left and right): the solve's start.")},
                          {"refine", prop("boolean", "Tighten the camera with the outline where it is not hair (default false).")},
                          {"like", prop("string", "The shoot: \"auto\" (default: the photos lined up already lend their focal and "
                                                  "distance), a lined-up photo's name, or \"none\".")},
                          {"prior_weight", prop("number", "How much the shoot's priors count against the pairs (default 1; 0: pairs alone).")}},
                         {"pairs"}),
            &Impl::alignPhoto);
        add("align_photos", "Line photos up together",
            "Solves several photos of one shoot together: one focal length for all (solved from every photo's pairs, leaning to a "
            "portrait lens when they cannot tell, or focal_mm), each camera near its view's angle round the head (front 0, "
            "three-quarters 45, sides 90, three-quarter backs 135, back 180), level, all at about the same distance: soft priors "
            "the pairs can overrule (prior_weight). Each photo's pairs as align_photo takes them (names of mesh_landmarks' facial "
            "landmarks too), or its stored ones. One undo step. Answers each photo's error, its angles (yaw, pitch, roll in "
            "degrees), how far it is from its view, the shared focal, the photos whose pairs fight the rest, and each overlay.",
            objectSchema({{"groom", groom},
                          {"photos", {{"type", "array"}, {"items", {{"type", "object"}}},
                                      {"description", "[{\"photo\": name, \"view\": front | back | left | right | top | "
                                                      "three_quarter_front_left | ..., \"pairs\": [{\"pixel\": [u, v], \"point\": "
                                                      "[x, y, z] or a landmark}]}, ...]"}}},
                          {"focal_mm", prop("number", "The shoot's focal length on a 36 mm film back, when known (kept).")},
                          {"prior_weight", prop("number", "How much the priors count against the pairs (default 1; 0: pairs alone).")}},
                         {"photos"}),
            &Impl::alignPhotos);
        add("photo_edit", "Edit a photo's marks",
            "Edits a reference photo, one undo step: add_flow (an arrow along the hair, root end first: points, directed), "
            "erase_flow (points, radius), clear_flow, add_mask (a touch-up of the hair mask: points, radius, include true for "
            "hair, false for backdrop), clear_mask, set_mask (path: a mask image, white hair), remove_landmark (index), "
            "clear_landmarks, remove_photo. Shows the photo after.",
            objectSchema({{"groom", groom}, {"photo", photoName},
                          {"op", enumProp({"add_flow", "erase_flow", "clear_flow", "add_mask", "clear_mask", "set_mask", "remove_landmark",
                                           "clear_landmarks", "remove_photo"},
                                          "The edit.")},
                          {"points", pixelPoints}, {"directed", prop("boolean", "add_flow: its first point is the root end (default true).")},
                          {"radius", prop("number", "add_mask: half its width; erase_flow: how near (pixels).")},
                          {"include", prop("boolean", "add_mask: hair (default) or backdrop.")},
                          {"index", prop("integer", "remove_landmark: the pair's number (1 on).")},
                          {"path", prop("string", "set_mask: the mask image (\"\" none).")}},
                         {"op"}),
            &Impl::photoEdit);
        add("guides_from_photos", "Guides from photos",
            "Grows guides from the lined-up photos (reference_guides: the hair's silhouette and strand flow in each photo, their "
            "flow arrows and mask touch-ups), into one group (default fromReference, replaced each time). One undo step. Answers "
            "with the guides made, the roots that were bald or unseen, the scalp's sides no photo sees (unseenShare, alignMore: "
            "line up a photo from there), and the look the photos suggest (melanin, curls, clumps; apply_look sets it). On curly "
            "hair each guide follows a lock (locks: traced in a photo, or filled from its neighbours) with its curl in its points; "
            "apply_look then clumps each guide's strands onto it instead of a Curl node over every strand.",
            objectSchema({{"groom", groom}, {"guide_count", prop("integer", "Roots tried, spread over the scalp (default 300).")},
                          {"max_length", prop("number", "The longest guide (scene units; default 0: what the photos show).")},
                          {"symmetry", enumProp({"auto", "none", "x", "topology"}, "Mirror what one side shows (default auto).")},
                          {"group", prop("string", "The guide group (default fromReference).")},
                          {"volume", prop("number", "How far the guides lift off the scalp: a share of the hair's thickness the "
                                                    "photos' outlines show over each spot (default 0.75; 0 as grown, along the scalp).")},
                          {"locks", prop("boolean", "On curly hair, each guide follows its lock's curl (default true; false: smooth "
                                                    "guides and a Curl node over every strand).")},
                          {"apply_look", prop("boolean", "Also set the look the photos suggest (HairColor melanin and redness, Clump, "
                                                         "Curl, Flyaway; nodes added where missing): a second undo step.")}}),
            &Impl::guidesFromPhotos);
        add("compare_to_photo", "Compare the groom with its photos",
            "Measures how the groom matches each lined-up photo (or the one named): its hair's silhouette through the photo's "
            "camera against the photo's hair mask (IoU, the photo's hair it misses, its hair outside the photo's), how far the "
            "photo's hair reaches past the groom's round the head (top, left, right, lower; scene units), where the hair from "
            "the top ends on the face (the hairline), and the hair's color (the photo's and the groom's, each's nearest melanin). "
            "With a picture of both outlines a photo. Use it after guides_from_photos and each change to judge by numbers.",
            objectSchema({{"groom", groom}, {"photo", photoName}}), &Impl::compareToPhotoTool);
        add("morph_to_photos", "Morph the scalp to the photos",
            "Shapes the scalp so every lined-up photo's landmark pairs land on their photo points (KeenTools-style pins: one photo "
            "slides a pair across its view, two or more set its depth too), the rest of the mesh following smoothly; always from "
            "the scalp as it was before the first morph, so morphs never stack. Use it when the mesh is another person's head "
            "than the photos'. One undo step (reset: the scalp before the first morph again). Answers each photo's pairs' error "
            "before and after (pixels) and the morphed outline over the photos.",
            objectSchema({{"groom", groom},
                          {"stiffness", prop("number", "0 (default: every pair lands) to 0.95 (the scalp keeps its shape, pairs fall short).")},
                          {"symmetric", prop("boolean", "Both sides alike: each pair mirrored across the head's middle (default true).")},
                          {"reset", prop("boolean", "Put the scalp back as it was before the first morph.")}}),
            &Impl::morphToPhotosTool);

        // ---- roots and tubes ----
        const json point3OrName = pointOrName("[x, y, z] or a landmark's name.");
        const json pointList = {{"type", "array"}, {"items", {{"type", json::array({"array", "string"})}}},
                                {"description", "World points (or landmarks), in order."}};
        const json guideIds = {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Guide ids (default: the selection)."}};
        add("root_tube", "Root tubes",
            "The Tube tool's root-first tubes, one undo step each: draw (a root's outline on the scalp, or a center, radius and "
            "sides; the tube grows along path, or straight off the scalp for length, its tip taper times the root's width), "
            "edit_outline (a root tube's new outline), duplicate (the guides' root tubes copied to \"to\"), delete, subdivide (cut "
            "each root into Voronoi cells, smaller where the tubeSubdivisionDensity map is high: paint it with paint_map), "
            "apply_density (cut again where the density map changed), list. Interpolate's fillTubes fills each tube with the "
            "strands rooted in it (the tube_locks preset).",
            objectSchema({{"groom", groom},
                          {"op", enumProp({"list", "draw", "edit_outline", "duplicate", "delete", "subdivide", "apply_density"}, "What to do.")},
                          {"outline", pointList}, {"center", point3OrName},
                          {"radius", prop("number", "draw with center: the root's radius (default 0.04 x the scalp's size).")},
                          {"sides", prop("integer", "draw with center: the outline's points (3-64, default 8).")},
                          {"path", pointList},
                          {"length", prop("number", "draw without a path: the tube's length (default 0.3 x the scalp's size).")},
                          {"segments", prop("integer", "draw without a path: its spans (default 12).")},
                          {"taper", prop("number", "The tip's width, a share of the root's (default 0.5).")},
                          {"density", prop("number", "A drawn path's points, times this (default 1).")},
                          {"group", prop("string", "The group new roots go into (default rootTubes).")},
                          {"guides", guideIds}, {"to", point3OrName},
                          {"size_at_zero", prop("number", "subdivide: a cell's size where the density is 0, a share of the root's (0.5).")},
                          {"size_at_one", prop("number", "subdivide: ... where it is 1 (0.12).")},
                          {"multiplier", prop("number", "subdivide: more cells overall (2: about twice as many).")},
                          {"seed", prop("integer", "subdivide: where the cells fall.")},
                          {"force", prop("boolean", "apply_density: cut again all over.")}},
                         {"op"}),
            &Impl::rootTube);
        add("tube_levels", "Tube levels",
            "The tube hierarchy: each level splits the tubes above in two (add), or takes the deepest split back (remove), one undo "
            "step; list shows the levels; view sets how render_preview's tubes show them (level: that level, mixed: with the "
            "unsplit tubes above; pin: a group whose tubes alone draw).",
            objectSchema({{"groom", groom}, {"op", enumProp({"list", "add", "remove", "view"}, "What to do (default list).")},
                          {"group", prop("string", "The base group (default: the guides' or the first top group).")},
                          {"guides", guideIds}, {"level", prop("integer", "view: the level shown (0: all).")},
                          {"mixed", prop("boolean", "view: with the tubes above it (default true).")},
                          {"pin", prop("string", "view: a group whose tubes alone draw (\"\" none).")}}),
            &Impl::tubeLevelsTool);
        add("tube_shape", "Tube shapes",
            "The groom's tube shape library, one undo step: add (a cross-section from points [[u, v], ...] round it, -1..1; "
            "give: the guides take it at once as their shape, midShape or tipShape), set (new points or smooth), rename (to), "
            "remove; list shows the built-in and library shapes. set_tube and guide_op edit_tubes give guides a shape by name, "
            "and hold or blend shapes along a tube (rootHold, midShape, midHold, tipShape, tipHold).",
            objectSchema({{"groom", groom}, {"op", enumProp({"list", "add", "set", "rename", "remove"}, "What to do (default list).")},
                          {"name", prop("string", "The shape.")}, {"to", prop("string", "rename: its new name.")},
                          {"points", {{"type", "array"}, {"items", {{"type", "array"}}}, {"description", "The outline, [[u, v], ...]."}}},
                          {"smooth", prop("boolean", "A smooth curve through the points (default) or straight edges.")},
                          {"give", prop("boolean", "add: the guides (ids or the selection) take it.")},
                          {"guides", guideIds},
                          {"profile", enumProp({"shape", "midShape", "tipShape"}, "give: as which of their shapes (default shape).")}}),
            &Impl::tubeShapeTool);

        // ---- expressions ----
        add("check_expression", "Check an expression",
            "Compiles an expression (GroomLab's SeExpr, as XGen's) as a param would, says what it reads (variables, strand "
            "channels, maps) and samples its values over the groom's strands at a node (or the scalp): min, mean, max, a "
            "10-bin histogram and a few examples; errors come back with their place. Then set it with set_params "
            "({\"expression\": ..., \"min\", \"max\"}). Examples to adapt: \"0.5 + 0.5 * sin($u * 6.2832)\" stripes round the scalp; "
            "\"smoothstep($y, 5, 12)\" more toward the top; \"$t ^ 2\" growing toward the tips (per point); \"noise($P * 0.3)\" "
            "patches; \"$clumpId % 2\" every other clump; \"$length > 10 ? 1 : 0.2\".",
            objectSchema({{"groom", groom}, {"expression", prop("string", "The expression.")},
                          {"node", prop("string", "Sample the strands this node gives (default the output).")},
                          {"variables", prop("object", "Its own constants, as an Expression node's: {\"amount\": 0.5, \"center\": [0, 1, 0]}.")},
                          {"type", enumProp({"any", "number", "vector"}, "What it must give (default any).")},
                          {"samples", prop("integer", "Strands sampled (default 2000).")},
                          {"image", prop("boolean", "Also a heatmap of it over the scalp (as show_map draws it).")}},
                         {"expression"}),
            &Impl::checkExpressionTool);

        // ---- simulation ----
        add("import_animation", "Import animation",
            "Gives the groom animated inputs from Alembic files inside the server's folder: scalp_cache (the scalp over time: the "
            "same vertices as the groom's), guide_cache (guide poses by guide ID), and static colliders (meshes: .obj or .abc, "
            "read by name through ColliderMesh nodes). The scene's inputs, not edits: undo does not take them back. An empty "
            "scalp_cache or guide_cache removes it.",
            objectSchema({{"groom", groom}, {"scalp_cache", prop("string", "The scalp cache (.abc).")},
                          {"scalp_object", prop("string", "Its mesh's name in the file (default the first).")},
                          {"guide_cache", prop("string", "A guide cache (.abc curves with a strandId per guide).")},
                          {"guide_object", prop("string", "Its curves' name in the file.")},
                          {"colliders", {{"type", "array"}, {"items", {{"type", "object"}}},
                                         {"description", "[{\"name\", \"path\", \"object\"}, ...]."}}}}),
            &Impl::importAnimation);
        add("animate_scalp", "Animate the scalp",
            "Moves the scalp by keyframes (translate, rotate_degrees XYZ about a pivot; eased between keys), writes it as an "
            "Alembic scalp cache and makes it the groom's animated scalp: a head turn or shake to try a simulation on. Not an "
            "undo step.",
            objectSchema({{"groom", groom},
                          {"keys", {{"type", "array"}, {"items", {{"type", "object"}}},
                                    {"description", "[{\"frame\": 1, \"translate\": [x, y, z], \"rotate_degrees\": [x, y, z]}, ...] (2 or more)."}}},
                          {"pivot", point3OrName}, {"first", prop("integer", "The first frame (default the first key's).")},
                          {"last", prop("integer", "The last frame (default the last key's).")},
                          {"path", prop("string", "The cache file (default anim/<groom>_scalp.abc).")}},
                         {"keys"}),
            &Impl::animateScalp);
        add("simulate", "Simulate",
            "Hair dynamics on the guides (a Simulate node before Interpolate): add: true inserts one (one undo step), preset "
            "(default, fur) sets its motion params; to_frame steps every Simulate node from its start frame to that frame through "
            "the scalp's animation (at most 1000 frames a call). Answers with the time taken and, per node, the guides' stretch "
            "against their groomed length, how far the tips moved from the groom, and points under the scalp. Configure the node "
            "with describe_node / set_params (gravity, stiffness, damping, collisions).",
            objectSchema({{"groom", groom}, {"add", prop("boolean", "Insert a Simulate node before Interpolate.")},
                          {"preset", enumProp({"default", "fur"}, "Set the motion params to a preset.")},
                          {"to_frame", prop("integer", "Simulate up to this frame.")}}),
            &Impl::simulateTool);
        add("sim_cache", "Simulation cache",
            "write: simulates a Simulate node's frames first..last and bakes them to an Alembic guide cache the groom then reads "
            "(saved with it: renders and the farm read the bake); read: takes a cache file as the node's bake.",
            objectSchema({{"groom", groom}, {"op", enumProp({"write", "read"}, "What to do.")},
                          {"node", prop("string", "The Simulate node (default the first).")},
                          {"path", prop("string", "The cache file (.abc; write's default caches/<groom>_<node>_sim.abc).")},
                          {"first", prop("integer", "write: the first frame (default the node's start).")},
                          {"last", prop("integer", "write: the last frame (default the groom's frame).")},
                          {"object", prop("string", "read: the curves' name in the file.")}},
                         {"op"}),
            &Impl::simCache);

        // ---- games and stats ----
        add("export_cards", "Export hair cards (glTF)",
            "Bakes the groom's hair cards (a Cards node in the graph) and writes them as glTF 2.0 (.glb, or .gltf with its .bin and "
            "PNG maps), with the hair cap (the groom baked onto its scalp) and their materials.",
            objectSchema({{"groom", groom}, {"path", prop("string", "The .glb or .gltf file.")},
                          {"resolution", prop("integer", "The maps' size (default the Cards node's).")},
                          {"occlusion", prop("number", "The cards' ambient occlusion (default 1).")},
                          {"cap", prop("boolean", "Also the hair cap (default true).")}},
                         {"path"}),
            &Impl::exportCards);
        add("export_game", "Export for a game engine",
            "Bakes the groom's hair cards and writes them for Unreal (TGA maps in Unreal's layout, centimeters) or Unity (PNG maps, "
            "meters): the maps, the hair cap's, a manifest and the meshes as FBX, into a folder.",
            objectSchema({{"groom", groom}, {"engine", enumProp({"unreal", "unity"}, "The engine (default unreal).")},
                          {"folder", prop("string", "The folder (default game/<groom>).")}, {"name", prop("string", "The files' base name.")},
                          {"resolution", prop("integer", "The maps' size.")}, {"occlusion", prop("number", "Ambient occlusion (1).")}}),
            &Impl::exportGame);
        add("stats", "Groom statistics",
            "Everything the last evaluation knows: strands, points, guides, cards, instances, memory, each node's time and "
            "notes, simulations: GroomLab's stats as JSON.",
            objectSchema({{"groom", groom}}), &Impl::statsTool);

        add("save_groom", "Save a groom file",
            "Saves the groom as a .groom file (graph, guides, scalp) that Maya, the app, Blender, groomlab-cli and the render "
            "procedurals open.",
            objectSchema({{"groom", groom}, {"path", prop("string", "The .groom file, relative to the server's folder.")}}, {"path"}),
            &Impl::saveGroom);
        add("export", "Export curves",
            "Writes the groom's strands as curves to Alembic (.abc) or USD (.usda, .usd) over a frame range, as groomlab-cli "
            "eval does: widths, root UVs, colors and channels; or as a V-Ray scene (.vrscene: the hair and its V-Ray hair "
            "shader).",
            objectSchema({{"groom", groom}, {"path", prop("string", "The output file: .abc, .usda, .usd or .vrscene.")},
                          {"frames", {{"type", json::array({"string", "number"})}, {"description", "A frame (12) or a range (\"1-24\"); default the groom's frame."}}},
                          {"step", prop("number", "Frames between samples (default 1).")},
                          {"density", prop("number", "Share of strands, 0-1 (default 1).")},
                          {"scales", enumProp({"mesh", "instancer", "both", "none"}, "How scales are written (USD instancer); default mesh.")}},
                         {"path"}),
            &Impl::exportTool);
    }

    // One tool run (AgentTools::call, batch): never throws, an error result instead.
    ToolResult runTool(const std::string& name, const json& arguments);

    const Tool* find(const std::string& name) const {
        for (const Tool& t : tools) {
            if (t.name == name) return &t;
        }
        return nullptr;
    }
};

AgentTools::AgentTools(AgentTarget& target) : impl_(std::make_unique<Impl>(target)) {}
AgentTools::~AgentTools() = default;

AgentTarget& AgentTools::target() { return impl_->target; }

RenderRequest AgentTools::renderRequest(const json& arguments) {
    const Impl::Tool* tool = impl_->find("render_frame");
    if (!tool) throw ToolError("this server renders only previews (render_preview)");
    return impl_->renderRequestOf(checkArguments(tool->schema, arguments.is_object() ? arguments : json::object()));
}

ToolResult AgentTools::renderResult(const RenderRequest& request, const RenderedFrame& frame) {
    return impl_->renderResultOf(request, frame);
}

json AgentTools::list() const {
    json out = json::array();
    for (const Impl::Tool& t : impl_->tools) {
        out.push_back({{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.schema}});
    }
    return out;
}

bool AgentTools::has(const std::string& name) const { return impl_->find(name) != nullptr; }

ToolResult AgentTools::call(const std::string& name, const json& arguments) { return impl_->runTool(name, arguments); }

ToolResult AgentTools::Impl::runTool(const std::string& name, const json& arguments) {
    const Impl::Tool* tool = find(name);
    auto failure = [](const std::string& message) {
        ToolResult r = textResult({{"error", message}});
        r.isError = true;
        return r;
    };
    if (!tool) return failure("unknown tool '" + name + "'");
    try {
        const json args = checkArguments(tool->schema, arguments.is_object() ? arguments : json::object());
        return tool->run(*this, args);
    } catch (const ToolError& e) {
        return failure(e.what());
    } catch (const GraphError& e) {
        return failure(std::string("graph: ") + e.what());
    } catch (const json::exception& e) {
        return failure(std::string("bad JSON: ") + e.what());
    } catch (const std::bad_alloc&) {
        return failure("out of memory");
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

}  // namespace groomlab::agent
