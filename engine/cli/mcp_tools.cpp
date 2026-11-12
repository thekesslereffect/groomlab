// SPDX-License-Identifier: Apache-2.0
// The MCP tools (mcp_tools.h): each tool's schema and description, written for a language model,
// and what it does. The arguments are checked against the schema first (unknown names, missing
// ones, wrong types), then by each tool (ranges, names, paths inside the root).

#include "mcp_tools.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "cli_export.h"
#include "groomlab/alembic_io.h"
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
#include "mcp_groom.h"
#include "mesh_files.h"
#include "preview_render.h"

namespace groomlab::cli {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxMeshBytes = 512ull << 20;
constexpr std::uintmax_t kMaxTextBytes = 16ull << 20;
constexpr std::size_t kMaxGuidesPerCall = 5000;
constexpr std::size_t kMaxGuidePoints = 200;
constexpr std::size_t kMaxScriptBytes = 64u << 10;
constexpr std::size_t kMaxExportFrames = 1000;
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
            for (const auto& [n, s] : h.tubeShapes()) names += ", " + n;
            throw ToolError("no tube shape '" + name + "' (shapes: " + names + ")");
        }
    }
    if (t.contains("width")) base.width = floatList(t["width"], "width", 1e-6f, 1e5f);
    if (t.contains("aspect")) base.aspect = floatList(t["aspect"], "aspect", kMinTubeAspect, kMaxTubeAspect);
    if (t.contains("twist_degrees")) base.twist = floatList(t["twist_degrees"], "twist_degrees", -36000.0f, 36000.0f, 3.14159265f / 180.0f);
    return base;
}

// String params that name files (a texture map, curves to import) must lie inside the root too.
void checkParamPaths(const Workspace& w, const json& value) {
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
        throw ToolError("the file '" + s + "' is outside the folder this server may use (" + w.root() +
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

}  // namespace

// ==== The tools ======================================================================================

struct McpTools::Impl {
    Workspace w;

    struct Tool {
        std::string name, title, description;
        json schema;
        std::function<ToolResult(Impl&, const json&)> run;
    };
    std::vector<Tool> tools;

    explicit Impl(const McpSettings& s) : w(s.root, s.exeDir) { define(); }

    // Ends a changing tool's answer: what the groom is now.
    json withSummary(Groom& g, json out) {
        out["groom"] = g.name;
        out["summary"] = w.summary(g);
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
            Groom& g = w.groom(str(a, "groom"));
            const std::string name = str(a, "node");
            const std::string graphText = Workspace::graphText(g);
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
        for (const PresetInfo& p : groomlab::listPresets(w.presetDirs())) {
            json e = {{"name", p.name}, {"title", p.title}};
            if (!p.category.empty()) e["category"] = p.category;
            if (!p.description.empty()) e["description"] = p.description;
            presets.push_back(e);
        }
        return textResult({{"presets", presets}, {"hint", "new_groom or apply_preset take a preset by name."}});
    }

    std::vector<std::pair<std::string, std::string>> sampleMeshes() const {
        std::vector<std::pair<std::string, std::string>> out;  // (sample name, file)
        if (w.meshDir().empty()) return out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::u8path(w.meshDir()), ec)) {
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
        const std::vector<DocPage> pages = readDocs(w.docsDir());
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
        const std::string path = w.resolve(mesh);
        std::error_code ec;
        const auto size = fs::file_size(fs::u8path(path), ec);
        if (ec) throw ToolError("no mesh file '" + mesh + "'");
        if (size > kMaxMeshBytes) throw ToolError("'" + mesh + "' is bigger than a scalp this server reads (512 MB)");
        return path;
    }

    json applyPreset(Groom& g, const std::string& preset, const std::string& mode, const std::string& after) {
        std::string path;
        if (preset.find('/') != std::string::npos || preset.find('\\') != std::string::npos || hasExtension(preset, ".json")) {
            path = w.resolve(preset);
        } else {
            path = presetPath(preset, w.presetDirs());
            if (path.empty()) throw ToolError("no preset '" + preset + "' (list_presets lists them)");
        }
        const std::string text = readText(path);
        const json parsed = json::parse(text, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("nodes")) throw ToolError("'" + preset + "' is not a GroomLab preset (no \"nodes\")");
        const std::string stem = fs::u8path(path).stem().u8string();
        const std::optional<double> size = Workspace::fitSize(g);
        json out = {{"preset", stem}, {"title", parsed.value("about", json::object()).value("title", presetTitle(stem))}, {"mode", mode}};
        if (mode == "insert") {
            std::string made;
            w.edit(g, "insert preset " + stem, [&](std::string& graph, std::string&) {
                graph = insertPresetJson(graph, text, size, after, presetNodeName(stem), &made);
            });
            out["node"] = made;
        } else {
            const std::string graph = applyPresetControlsToGraph(presetGraphJson(text, size), presetControlsJson(text, size));
            // The guide groups it reads, made empty when missing (a hand-placed look: add_guides fills them).
            json groupsMade = json::array();
            w.edit(g, "preset " + stem, [&](std::string& current, std::string& hierarchy) {
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
        const MapCatalog& maps = g.scene->session().data().maps;
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
        const std::string name = str(a, "name");
        checkName(name, "a groom's name");
        if (w.grooms().count(name)) throw ToolError("a groom named '" + name + "' is open already: pick another name");
        const std::string path = meshPath(str(a, "mesh"));
        std::shared_ptr<const ScalpMesh> mesh;
        try {
            mesh = readMeshFile(path, str(a, "object"));
        } catch (const MeshFileError& e) {
            throw ToolError(e.what());
        }
        GroomFile file;
        file.data.scalp = mesh;
        file.data.graphJson = defaultGraphJsonFor(presetScalpSize(*mesh));
        file.source = fs::u8path(path).filename().u8string();
        Groom& g = w.add(name, std::make_unique<GroomScene>(std::move(file)));
        g.mesh = w.insideRoot(path) ? w.shown(path) : str(a, "mesh") + " (sample)";
        json out = {{"mesh", g.mesh}};
        Vec3f lo = mesh->points().empty() ? Vec3f() : mesh->points().front(), hi = lo;
        for (const Vec3f& p : mesh->points()) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        out["scalp"] = {{"points", mesh->pointCount()}, {"triangles", mesh->triangleCount()}, {"min", vec(lo)}, {"max", vec(hi)},
                        {"upAxis", "Y"}, {"units", "cm"}};
        try {
            if (a.contains("preset")) out["preset"] = applyPreset(g, str(a, "preset"), "replace", {});
        } catch (...) {
            w.grooms().erase(name);  // no half-made groom
            throw;
        }
        g.undo.clear();  // the groom starts here
        return textResult(withSummary(g, out));
    }

    ToolResult openGroom(const json& a) {
        const std::string path = w.resolve(str(a, "path"));
        if (!hasExtension(path, ".groom")) throw ToolError("open_groom reads .groom files");
        std::string name = str(a, "name", fs::u8path(path).stem().u8string());
        checkName(name, "a groom's name");
        if (w.grooms().count(name)) throw ToolError("a groom named '" + name + "' is open already: give another \"name\"");
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
            if (!ref.empty() && !w.insideRoot(ref)) throw ToolError("the groom reads '" + ref + "', outside the folder this server may use");
        }
        const json graph = json::parse(file.data.graphJson, nullptr, false);
        if (graph.is_object()) {
            for (const json& n : graph.value("nodes", json::array())) checkParamPaths(w, n.value("params", json::object()));
        }
        Groom& g = w.add(name, std::make_unique<GroomScene>(std::move(file)));
        g.path = path;
        const GroomFile& f = g.scene->file();
        return textResult(withSummary(g, {{"path", w.shown(path)}, {"upAxis", f.upAxis}, {"units", f.units},
                                          {"nodes", json::parse(Workspace::graphText(g)).value("nodes", json::array()).size()}}));
    }

    ToolResult listGrooms(const json&) {
        json list = json::array();
        for (auto& [name, g] : w.grooms()) {
            json e = {{"name", name}, {"undoSteps", g.undo.size()}, {"frame", g.frame}};
            if (!g.path.empty()) e["path"] = w.shown(g.path);
            if (!g.mesh.empty()) e["mesh"] = g.mesh;
            list.push_back(e);
        }
        return textResult({{"grooms", list}});
    }

    ToolResult applyPresetTool(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string mode = str(a, "mode", "replace");
        if (mode == "insert" && !a.contains("after")) throw ToolError("mode insert needs \"after\": the node the look goes after");
        return textResult(withSummary(g, applyPreset(g, str(a, "preset"), mode, str(a, "after"))));
    }

    ToolResult getGraph(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const json graph = json::parse(Workspace::graphText(g));
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
        return textResult({{"groom", g.name}, {"nodes", nodes}, {"output", graph.value("output", std::string())},
                           {"note", "params lists only values that differ from the node's defaults (describe_node shows them all)."}});
    }

    ToolResult addNode(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string type = str(a, "type");
        if (type == "Script") throw ToolError("add a script node with add_script_node");
        const json params = a.value("params", json::object());
        checkParamPaths(w, params);
        if (a.contains("name")) checkName(str(a, "name"), "a node's name");
        std::string made, placed;
        w.edit(g, "add " + type, [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            made = e.addNode(type, str(a, "name"), params.empty() ? std::string() : params.dump(), std::nullopt, std::nullopt, Workspace::fitSize(g));
            placed = placeNode(e, made, a);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"node", made}, {"placed", placed}}));
    }

    ToolResult removeNode(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const bool reconnect = flag(a, "reconnect", true);
        std::vector<std::string> rejoined;
        w.edit(g, "remove " + node, [&](std::string& graph, std::string&) {
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
        Groom& g = w.groom(str(a, "groom"));
        const auto [from, output] = splitSource(str(a, "from"));
        const std::string out = a.contains("output") ? str(a, "output") : output;
        w.edit(g, "connect " + from + " -> " + str(a, "to"), [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            e.connectByHand(from, str(a, "to"), str(a, "input"), out);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"connected", from + (out.empty() ? "" : "." + out) + " -> " + str(a, "to") + "." + str(a, "input")}}));
    }

    ToolResult disconnect(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        w.edit(g, "disconnect " + str(a, "to") + "." + str(a, "input"), [&](std::string& graph, std::string&) {
            GraphEdit e(graph);
            if (!e.disconnect(str(a, "to"), str(a, "input"))) throw ToolError("nothing feeds " + str(a, "to") + "." + str(a, "input"));
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"disconnected", str(a, "to") + "." + str(a, "input")}}));
    }

    ToolResult setParams(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const json& params = a["params"];
        if (params.empty()) throw ToolError("no params given");
        checkParamPaths(w, params);
        std::vector<std::string> set;
        w.edit(g, "set " + node, [&](std::string& graphText, std::string&) {
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
                values[param] = value;
                set.push_back(param);
            }
            graphText = graph.dump();
        });
        return textResult(withSummary(g, {{"node", node}, {"set", set}}));
    }

    // The hierarchy of a groom, its scale set, to edit.
    GuideHierarchy hierarchyOf(Groom& g) {
        GuideHierarchy h = GuideHierarchy::fromJson(Workspace::hierarchyText(g));
        const double k = g.scene->session().groomScale();
        if (k > 0.0) h.setScale(static_cast<float>(k));
        return h;
    }

    MeshPtr scalpOf(Groom& g) {
        const MeshPtr scalp = g.scene->session().scalp();
        if (!scalp || scalp->triangleCount() == 0) throw ToolError("the groom has no scalp");
        return scalp;
    }

    Vec3f upOf(Groom& g) const { return g.scene->file().upAxis == "Z" ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0); }

    ToolResult addGuides(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const json& list = a["guides"];
        if (list.empty() || list.size() > kMaxGuidesPerCall) throw ToolError("give 1 to " + std::to_string(kMaxGuidesPerCall) + " guides");
        const std::string groupName = str(a, "group", "guides");
        const bool snap = flag(a, "snap", true);
        const MeshPtr scalp = scalpOf(g);
        const Vec3f down = -upOf(g);
        const Vec3f back = g.scene->file().upAxis == "Z" ? Vec3f(0, 1, 0) : Vec3f(0, 0, -1);
        Vec3f lo = scalp->points().front(), hi = lo;
        for (const Vec3f& p : scalp->points()) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const Vec3f middle = (lo + hi) * 0.5f;
        std::vector<GuideId> ids;
        w.edit(g, "add guides", [&](std::string&, std::string& hierarchy) {
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
        Groom& g = w.groom(str(a, "groom"));
        const GuideHierarchy stored = GuideHierarchy::fromJson(Workspace::hierarchyText(g));
        const HierarchyPtr live = g.scene->session().hierarchy();  // rooted on the scalp
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
        json out = {{"groom", g.name}, {"groups", groups}, {"guides", guides}, {"count", total}};
        if (total > guides.size()) out["note"] = "the first " + std::to_string(guides.size()) + " guides are listed (raise limit)";
        return textResult(out);
    }

    ToolResult setTube(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        if (!a.contains("shape") && !a.contains("width") && !a.contains("aspect") && !a.contains("twist_degrees")) {
            throw ToolError("say what to set: shape, width, aspect or twist_degrees");
        }
        const MeshPtr scalp = scalpOf(g);
        std::size_t changed = 0;
        w.edit(g, "set tubes", [&](std::string&, std::string& hierarchy) {
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
        Groom& g = w.groom(str(a, "groom"));
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
        if (const std::optional<double> size = Workspace::fitSize(g)) def["size"] = *size;
        const json errors = json::parse(scriptNodeErrorsJson(def.dump())).value("errors", json::array());
        if (!errors.empty()) {
            ToolResult r = textResult({{"errors", errors}, {"message", "The script node has errors (line and column of the script); nothing changed. Fix them and call add_script_node again."}});
            r.isError = true;
            return r;
        }
        const std::string existing = str(a, "node");
        std::string made = existing, placed;
        w.edit(g, existing.empty() ? "add script node" : "edit " + existing, [&](std::string& graph, std::string&) {
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
            made = e.addNode("Script", name, json{{"definition", def}}.dump(), std::nullopt, std::nullopt, Workspace::fitSize(g));
            placed = placeNode(e, made, a);
            graph = e.toJson();
        });
        return textResult(withSummary(g, {{"node", made}, {"placed", placed}}));
    }

    ToolResult saveNode(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string node = str(a, "node");
        const std::string graphText = Workspace::graphText(g);
        const json n = nodeJsonOf(json::parse(graphText), node);
        std::string text;
        if (n.value("type", std::string()) == "Script") {
            text = n.value("params", json::object()).value("definition", json::object()).dump(2);
        } else if (n.value("type", std::string()) == "Compound") {
            text = nodeFileFromCompound(graphText, node, "{}", Workspace::fitSize(g));
        } else {
            throw ToolError("only script and compound nodes are saved as .glnode files");
        }
        checkNodeFile(text);
        const std::string id = json::parse(text).value("id", std::string("my.node"));
        const std::string path = w.resolveForWrite(str(a, "path", id + ".glnode"));
        if (!hasExtension(path, ".glnode")) throw ToolError("a node file ends in .glnode");
        writeText(path, text + "\n");
        return textResult({{"path", w.shown(path)}, {"id", id},
                           {"note", "Use it in GroomLab with Import Node... (Maya's Graph Editor, the app), or copy it into Documents/GroomLab/nodes."}});
    }

    ToolResult undoTool(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const int steps = static_cast<int>(num(a, "steps", 1));
        if (steps < 1 || steps > static_cast<int>(kMaxUndo)) throw ToolError("steps must be 1 to " + std::to_string(kMaxUndo));
        if (g.undo.empty()) throw ToolError("nothing to undo");
        const std::vector<std::string> labels = w.undo(g, steps);
        return textResult(withSummary(g, {{"undone", labels}, {"undoStepsLeft", g.undo.size()}}));
    }

    // ---- check ----
    ToolResult evaluate(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        if (a.contains("frame")) g.frame = num(a, "frame", g.frame);
        const double density = num(a, "density", 1.0);
        if (density < 0.0 || density > 1.0) throw ToolError("density must be 0 to 1");
        json out = w.summary(g, static_cast<float>(density), str(a, "node"), true);
        out["groom"] = g.name;
        out["frame"] = g.frame;
        return textResult(out);
    }

    // ---- see ----
    ToolResult renderPreviewTool(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        if (a.contains("frame")) g.frame = num(a, "frame", g.frame);
        PreviewOptions o;
        if (a.contains("views")) {
            o.views.clear();
            for (const json& v : a["views"]) {
                if (!v.is_string()) throw ToolError("views are names: front, side, back, three_quarter, top");
                o.views.push_back(v.get<std::string>());
            }
        }
        o.size = static_cast<int>(num(a, "size", 512));
        o.maxStrands = static_cast<std::size_t>(std::clamp(num(a, "max_strands", 50000.0), 100.0, 300000.0));
        o.zUp = g.scene->file().upAxis == "Z";
        const double density = num(a, "density", 1.0);
        if (density < 0.0 || density > 1.0) throw ToolError("density must be 0 to 1");
        ViewRequest request;
        request.density = static_cast<float>(density);
        request.viewNode = str(a, "node");
        request.time = frameToSeconds(g.frame, g.scene->framesPerSecond());
        const ViewResult& view = g.scene->view(request);
        const StrandsPtr strands = view.placed();
        const MeshPtr scalp = view.scalp ? view.scalp : g.scene->session().rawScalp();
        PreviewImage image;
        try {
            image = renderPreview(scalp.get(), strands.get(), o);
        } catch (const std::invalid_argument& e) {
            throw ToolError(e.what());
        }
        json info = {{"groom", g.name}, {"views", o.views}, {"width", image.width}, {"height", image.height},
                     {"columns", image.columns}, {"strands", image.strandsTotal}, {"strandsDrawn", image.strandsDrawn}};
        if (image.stride > 1) {
            info["note"] = "drew 1 in " + std::to_string(image.stride) + " strands (max_strands " + std::to_string(o.maxStrands) +
                           "): the preview looks thinner than the groom";
        }
        if (!view.errors.empty()) info["errors"] = view.errors;
        if (a.contains("save_to")) {
            const std::string path = w.resolveForWrite(str(a, "save_to"));
            if (!hasExtension(path, ".png")) throw ToolError("save_to must be a .png file");
            writeText(path, std::string(image.png.begin(), image.png.end()));
            info["saved"] = w.shown(path);
        }
        ToolResult r;
        r.content.push_back({{"type", "image"}, {"data", base64Encode(image.png)}, {"mimeType", "image/png"}});
        r.content.push_back({{"type", "text"}, {"text", info.dump()}});
        return r;
    }

    // ---- deliver ----
    ToolResult saveGroom(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        const std::string path = w.resolveForWrite(str(a, "path"));
        if (!hasExtension(path, ".groom")) throw ToolError("a groom file ends in .groom");
        const GroomSession& s = g.scene->session();
        GroomFile file = g.scene->file();
        file.data = s.data();
        file.data.scalpHistory = nullptr;
        file.framesPerSecond = s.framesPerSecond();
        try {
            writeGroomFile(path, file);
        } catch (const GroomFileError& e) {
            throw ToolError(e.what());
        }
        g.path = path;
        std::error_code ec;
        return textResult({{"groom", g.name}, {"path", w.shown(path)}, {"bytes", fs::file_size(fs::u8path(path), ec)}});
    }

    ToolResult exportTool(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
        ExportOptions o;
        o.out = w.resolveForWrite(str(a, "path"));
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
        ExportReport report;
        try {
            report = exportStrands(*g.scene, o, nullptr);
        } catch (const std::invalid_argument& e) {
            throw ToolError(e.what());
        }
        const char* format = isVrscenePath(o.out) ? "vrscene" : isUsdPath(o.out) ? "usd" : "alembic";
        return textResult({{"groom", g.name}, {"path", w.shown(o.out)}, {"format", format},
                           {"frames", report.frames}, {"strandsPerFrame", report.frames ? report.strandTotal / report.frames : 0},
                           {"ms", static_cast<long long>(report.milliseconds + 0.5)}});
    }

    ToolResult sampleScalp(const json& a) {
        Groom& g = w.groom(str(a, "groom"));
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
        return textResult({{"groom", g.name}, {"points", points}, {"upAxis", g.scene->file().upAxis},
                           {"hint", "add_guides takes these as roots: {\"root\": p, \"length\": ..., \"droop\": 0.5} grows a guide along n."}});
    }

    // ---- The table ---------------------------------------------------------------------------------
    void add(const char* name, const char* title, const std::string& description, json schema,
             ToolResult (Impl::*run)(const json&)) {
        tools.push_back({name, title, description, std::move(schema), [run](Impl& self, const json& a) { return (self.*run)(a); }});
    }

    void define() {
        const json groom = prop("string", kGroomDescription);
        const json views = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", previewViewNames()}}},
                            {"description", "Views to draw, laid out in a grid: front, side (from +X), back, three_quarter, top. Default [\"three_quarter\"]."}};
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
                          {"preset", prop("string", "A preset to start from (list_presets), instead of the default graph.")}},
                         {"name", "mesh"}),
            &Impl::newGroom);
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
            "such as {\"vertexMap\": \"density\"}. Answers with the groom's summary after the change.",
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
        add("undo", "Undo", "Undoes the last changes to a groom's graph or guides (64 steps are kept).",
            objectSchema({{"groom", groom}, {"steps", prop("integer", "How many changes to undo (default 1).")}}), &Impl::undoTool);

        add("evaluate", "Evaluate",
            "Evaluates the groom and reports strand and point counts, guides, bounds, errors, the nodes' notes and the slowest "
            "nodes' milliseconds. Every changing tool already answers with a summary; this one adds timings, a frame, a density "
            "and a node to look at instead of the output.",
            objectSchema({{"groom", groom}, {"frame", prop("number", "The frame (default the groom's, 1).")},
                          {"density", prop("number", "Share of strands to evaluate, 0-1 (default 1).")},
                          {"node", prop("string", "Evaluate up to this node instead of the output.")}}),
            &Impl::evaluate);
        add("render_preview", "Render a preview",
            "Draws the groom as a PNG image you can look at: the scalp shaded and the strands as lines in the hair's color (and "
            "instances with a built-in mesh: beads, cuffs, hair ties), from one or several views in a grid. A simple CPU preview (no lighting model or shadows), not a render: use it to check "
            "shape, length, direction and color. Big grooms are thinned to max_strands.",
            objectSchema({{"groom", groom}, {"views", views},
                          {"size", prop("integer", "The image's width in pixels (64-1024, default 512).")},
                          {"max_strands", prop("integer", "Draw at most about this many strands (default 50000).")},
                          {"frame", prop("number", "The frame.")}, {"density", prop("number", "Share of strands, 0-1 (default 1).")},
                          {"node", prop("string", "Draw this node's strands instead of the output's.")},
                          {"save_to", prop("string", "Also write the PNG to this file inside the server's folder.")}}),
            &Impl::renderPreviewTool);

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

    const Tool* find(const std::string& name) const {
        for (const Tool& t : tools) {
            if (t.name == name) return &t;
        }
        return nullptr;
    }
};

McpTools::McpTools(McpSettings settings) : impl_(std::make_unique<Impl>(settings)) {}
McpTools::~McpTools() = default;

const std::string& McpTools::root() const { return impl_->w.root(); }

json McpTools::list() const {
    json out = json::array();
    for (const Impl::Tool& t : impl_->tools) {
        out.push_back({{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.schema}});
    }
    return out;
}

bool McpTools::has(const std::string& name) const { return impl_->find(name) != nullptr; }

ToolResult McpTools::call(const std::string& name, const json& arguments) {
    const Impl::Tool* tool = impl_->find(name);
    auto failure = [](const std::string& message) {
        ToolResult r = textResult({{"error", message}});
        r.isError = true;
        return r;
    };
    if (!tool) return failure("unknown tool '" + name + "'");
    try {
        const json args = checkArguments(tool->schema, arguments.is_object() ? arguments : json::object());
        return tool->run(*impl_, args);
    } catch (const ToolError& e) {
        return failure(e.what());
    } catch (const GraphError& e) {
        return failure(std::string("graph: ") + e.what());
    } catch (const AlembicError& e) {
        return failure(std::string("alembic: ") + e.what());
    } catch (const json::exception& e) {
        return failure(std::string("bad JSON: ") + e.what());
    } catch (const std::bad_alloc&) {
        return failure("out of memory");
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}

}  // namespace groomlab::cli
