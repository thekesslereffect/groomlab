// SPDX-License-Identifier: Apache-2.0
// Compound nodes: the .glnode files, the library folders, and a node file made from a compound
// (node_library.h). The files are text people read and share: fields keep their written order.

#include "groomlab/node_library.h"

#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/node_catalog.h"
#include "groomlab/operators.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "operators/internal.h"

namespace groomlab {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

const std::string kTypePrefix = "node:";

std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string readText(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw GraphError("cannot read " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A text field: absent or a string ("" for absent); GraphError naming it otherwise.
std::string textField(const json& object, const char* key, const std::string& where = {}) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return {};
    if (!it->is_string()) throw GraphError(where + "\"" + key + "\" must be text");
    return it->get<std::string>();
}

std::optional<double> numberField(const json& object, const char* key, const std::string& where) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::nullopt;
    if (!it->is_number() || it->is_boolean()) throw GraphError(where + "\"" + key + "\" must be a number");
    return it->get<double>();
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// "guideRadius" -> "Guide radius" (as the hosts' param labels read).
std::string paramWords(const std::string& name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0) {
            out += ' ';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c == '_' ? ' ' : c;
        }
    }
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// "softFrizz" -> "Soft Frizz" (a compound's name as a node's label).
std::string nameWords(const std::string& name) {
    std::string out;
    bool start = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_' || c == ' ') {
            out += ' ';
            start = true;
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0 && !start) out += ' ';
        out += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        start = false;
    }
    return trimmed(out);
}

// "Soft Frizz" -> "soft_frizz".
std::string slug(const std::string& text) {
    std::string out;
    for (const char c : lower(text)) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// "Soft Frizz" -> "softFrizz": a node name from a label.
std::string camelName(const std::string& label) {
    std::string out;
    bool up = false;
    for (const char c : label) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            up = !out.empty();
            continue;
        }
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "node" + out;
    out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    return out;
}

std::vector<std::string> splitPathList(const std::string& list) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> out;
    std::stringstream in(list);
    std::string dir;
    while (std::getline(in, dir, separator)) {
        if (!trimmed(dir).empty()) out.push_back(trimmed(dir));
    }
    return out;
}

// The library cache: the last listing and what the folders held when it was made.
struct Cache {
    std::mutex mutex;
    std::string shipped;
    std::string signature;
    std::vector<std::string> dirs;  // the folders the listing is of
    std::vector<LibraryNode> nodes;
    std::chrono::steady_clock::time_point checked;
    bool fresh = false;
};

Cache& cache() {
    static Cache c;
    return c;
}

// What the folders hold, cheaply: each node file's path, size and time.
std::string folderSignature(const std::vector<std::string>& dirs) {
    std::string out;
    for (const std::string& dir : dirs) {
        out += dir + "|";
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".glnode") continue;
            std::error_code e2;
            out += it->path().generic_u8string() + ":" + std::to_string(it->file_size(e2)) + ":" +
                   // (A long long: Emscripten's libc++ counts file times in __int128.)
                   std::to_string(static_cast<long long>(it->last_write_time(e2).time_since_epoch().count())) + ";";
        }
    }
    return out;
}

void markChanged() {
    std::lock_guard<std::mutex> lock(cache().mutex);
    cache().fresh = false;
}

// A built-in category for a file's category (its id or title, any case), else nullopt.
std::optional<std::size_t> builtinCategory(const std::string& category) {
    const std::string wanted = lower(trimmed(category));
    const auto& categories = nodeCategories();
    for (std::size_t i = 0; i < categories.size(); ++i) {
        if (lower(categories[i].id) == wanted || lower(categories[i].title) == wanted) return i;
    }
    return std::nullopt;
}

}  // namespace

std::string libraryNodeType(const std::string& id) { return kTypePrefix + id; }

std::string libraryNodeName(const std::string& label) { return camelName(label); }

std::string libraryNodeId(const std::string& type) {
    return type.rfind(kTypePrefix, 0) == 0 ? type.substr(kTypePrefix.size()) : std::string();
}

std::string nodeIdError(const std::string& id) {
    if (id.empty()) return "a node needs an id (like \"yourname.soft_frizz\")";
    std::size_t parts = 1;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (c == '.') {
            if (i == 0 || i + 1 == id.size() || id[i - 1] == '.') return "the id '" + id + "' has an empty part between dots";
            ++parts;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return "the id '" + id + "' may hold only a-z, 0-9, _ and - between dots";
        }
    }
    if (parts < 2) return "the id '" + id + "' needs two parts or more joined by dots (like \"yourname.soft_frizz\")";
    return {};
}

std::string suggestNodeId(const std::string& label, const std::string& author) {
    const std::string who = slug(author), what = slug(label);
    return (who.empty() ? std::string("my") : who) + "." + (what.empty() ? std::string("node") : what);
}

std::string checkNodeFile(const std::string& text) {
    json file = json::parse(text, nullptr, false);
    if (file.is_discarded()) throw GraphError("the node file is not valid JSON");
    if (!file.is_object()) throw GraphError("the node file is not a JSON object");
    const auto format = file.find("glnode");
    if (format == file.end()) throw GraphError("not a node file: it has no \"glnode\" format number");
    if (!format->is_number_integer() || format->get<std::int64_t>() < 1) throw GraphError("\"glnode\" must be a whole number from 1");
    if (format->get<std::int64_t>() > kNodeFileFormat) {
        throw GraphError("the node file is of format " + format->dump() + ", newer than this GroomLab reads (" +
                         std::to_string(kNodeFileFormat) + "): update GroomLab to use it");
    }
    if (!file.contains("id") || !file["id"].is_string()) throw GraphError("the node file needs an \"id\" (text)");
    const std::string id = file["id"].get<std::string>();
    if (const std::string error = nodeIdError(id); !error.empty()) throw GraphError(error);
    const auto version = file.find("version");
    if (version == file.end() || !version->is_number_integer() || version->get<std::int64_t>() < 1) {
        throw GraphError("\"version\" must be a whole number from 1");
    }
    if (trimmed(textField(file, "label")).empty()) throw GraphError("the node file needs a \"label\" (the name the menus show)");
    for (const char* key : {"category", "description", "author", "license", "icon"}) textField(file, key);
    if (const auto size = numberField(file, "size", ""); size && !(*size > 0.0)) throw GraphError("\"size\" must be above 0");

    // A script node: its header and its script (compiled), the file as it is.
    if (file.contains("script")) {
        if (file.contains("graph")) {
            throw GraphError("a node file holds a \"graph\" (a compound node) or a \"script\" (a script node), not both");
        }
        const ScriptNodeHeader header = scriptNodeHeader(file.dump());
        try {
            Script(header.script, header.scriptParams, header.writes);
        } catch (const ScriptError& e) {
            throw GraphError(std::string("script: ") + e.what());
        }
        if (!file.contains("inputs")) file["inputs"] = json::array({{{"name", "strands"}, {"type", "Strands"}}});
        if (!file.contains("output")) file["output"] = {{"name", "out"}, {"type", "Strands"}};
        if (!file.contains("params")) file["params"] = json::array();
        if (!file.contains("writes")) file["writes"] = json::array();
        return file.dump();
    }

    // Inputs: as a compound's (graph_io.cpp checks them).
    if (!file.contains("inputs")) file["inputs"] = json::array();
    std::vector<PortSpec> inputs;
    try {
        inputs = compoundInputs(nlohmann::json{{"inputs", nlohmann::json::parse(file["inputs"].dump())}}.dump());
    } catch (const GraphError& e) {
        throw GraphError(std::string("inputs: ") + e.what());
    }
    std::set<std::string> inputNames;
    for (const PortSpec& p : inputs) {
        if (!inputNames.insert(p.name).second) throw GraphError("inputs: two are named '" + p.name + "'");
    }

    if (!file.contains("graph") || !file["graph"].is_object()) throw GraphError("the node file needs a \"graph\" (an object)");
    json& graph = file["graph"];
    if (!graph.contains("nodes") || !graph["nodes"].is_array()) throw GraphError("graph: needs \"nodes\" (a list)");
    auto innerNode = [&](const std::string& name) -> json* {
        for (json& n : graph["nodes"]) {
            if (n.is_object() && n.value("name", std::string()) == name) return &n;
        }
        return nullptr;
    };

    // Params: each names the inner param it shows; its default is set there.
    if (!file.contains("params")) file["params"] = json::array();
    if (!file["params"].is_array()) throw GraphError("\"params\" must be a list");
    json expose = json::object();
    std::set<std::string> paramNames;
    for (std::size_t i = 0; i < file["params"].size(); ++i) {
        const json& p = file["params"][i];
        const std::string where = "params[" + std::to_string(i) + "]: ";
        if (!p.is_object()) throw GraphError(where + "must be an object");
        const std::string name = textField(p, "name", where);
        if (!isIdentifier(name)) throw GraphError(where + "\"name\" must be a word of letters, digits and _ ('" + name + "')");
        if (name == "definition" || name == "repeat" || inputNames.count(name)) {
            throw GraphError(where + "the name '" + name + "' is taken (by the node itself or an input)");
        }
        if (!paramNames.insert(name).second) throw GraphError(where + "two params are named '" + name + "'");
        const std::string target = textField(p, "target", where);
        const std::size_t dot = target.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == target.size()) {
            throw GraphError(where + "\"target\" must name an inner node's param as node.param ('" + target + "')");
        }
        json* node = innerNode(target.substr(0, dot));
        if (!node) throw GraphError(where + "the graph has no node '" + target.substr(0, dot) + "'");
        for (const char* key : {"label", "description", "unit"}) textField(p, key, where);
        for (const char* key : {"min", "max", "softMin", "softMax"}) numberField(p, key, where);
        if (p.contains("mappable") && !p["mappable"].is_boolean()) throw GraphError(where + "\"mappable\" must be true or false");
        if (p.contains("default")) {
            if (!node->contains("params") || !(*node)["params"].is_object()) (*node)["params"] = json::object();
            (*node)["params"][target.substr(dot + 1)] = p["default"];
        }
        expose[name] = target;
    }

    // The graph loads (operator types known, links fit, values right) and its Input nodes stand
    // for listed inputs; every target is a param its node has.
    GraphDocument doc;
    try {
        doc = loadGraphJson(graph.dump());
    } catch (const GraphError& e) {
        throw GraphError(std::string("graph: ") + e.what());
    }
    for (const auto& [name, nodeId] : doc.nodesByName) {
        const Operator& op = doc.graph.node(nodeId);
        if (op.typeName() == op::kInput && !inputNames.count(op.params().getString("name"))) {
            throw GraphError("graph: Input node '" + name + "' stands for input '" + op.params().getString("name") +
                             "', which \"inputs\" does not list");
        }
    }
    for (std::size_t i = 0; i < file["params"].size(); ++i) {
        const std::string target = file["params"][i]["target"].get<std::string>();
        const std::size_t dot = target.find('.');
        const Operator& op = doc.graph.node(doc.node(target.substr(0, dot)));
        const auto specs = nodeParamSpecs(op);
        const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == target.substr(dot + 1); });
        if (spec == specs.end()) {
            throw GraphError("params[" + std::to_string(i) + "]: " + op.typeName() + " node '" + target.substr(0, dot) +
                             "' has no param '" + target.substr(dot + 1) + "'");
        }
        // Its default a value of the param's kind (or a map).
        if (file["params"][i].contains("default") && op.params().has(spec->name)) {
            const ParamValue& value = op.params().get(spec->name);
            if (value.index() != spec->defaultValue.index() && !std::holds_alternative<MapRef>(value)) {
                throw GraphError("params[" + std::to_string(i) + "]: \"default\" " + file["params"][i]["default"].dump() +
                                 " does not fit " + target);
            }
        }
    }
    const PortSpec output = doc.graph.node(doc.output).output();
    if (file.contains("output")) {
        const json& out = file["output"];
        if (!out.is_object()) throw GraphError("\"output\" must be an object ({\"name\", \"type\"})");
        const std::string type = textField(out, "type", "output: ");
        // (A node giving strands or guides as fed, Curves, may say which it is given.)
        const bool fits = type == portTypeName(output.type) ||
                          (output.type == PortType::Curves && (type == "Strands" || type == "Guides"));
        if (!type.empty() && !fits) {
            throw GraphError("\"output\" says " + type + " but the graph's output node gives " + portTypeName(output.type));
        }
    } else {
        file["output"] = {{"name", "out"}, {"type", portTypeName(output.type)}};
    }
    file["expose"] = expose;
    return file.dump();
}

LibraryNode readNodeFile(const std::string& path) {
    LibraryNode node;
    node.path = fs::u8path(path).generic_u8string();
    std::string text;
    try {
        text = readText(path);
        node.definition = checkNodeFile(text);
    } catch (const std::exception& e) {
        node.error = e.what();
    }
    const json file = json::parse(node.definition.empty() ? text : node.definition, nullptr, false);
    if (file.is_object()) node.kind = file.contains("script") ? "script" : "compound";
    if (file.is_object()) {
        auto field = [&](const char* key) {
            const auto it = file.find(key);
            return it != file.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        node.id = field("id");
        node.label = trimmed(field("label"));
        node.category = trimmed(field("category"));
        node.description = field("description");
        node.author = field("author");
        node.license = field("license");
        node.icon = field("icon");
        const auto v = file.find("version");
        if (v != file.end() && v->is_number_integer()) node.version = v->get<int>();
    }
    if (node.label.empty()) node.label = node.id.empty() ? fs::u8path(path).stem().u8string() : node.id;
    return node;
}

std::string nodeUserDir() {
    if (const std::optional<std::string> own = environmentVariable("GROOMLAB_USER_NODES"); own && !own->empty()) return *own;
#ifdef _WIN32
    const std::optional<std::string> home = environmentVariable("USERPROFILE");
#else
    const std::optional<std::string> home = environmentVariable("HOME");
#endif
    if (!home || home->empty()) return {};
    return (fs::u8path(*home) / "Documents" / "GroomLab" / "nodes").generic_u8string();
}

void setShippedNodeDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(cache().mutex);
    cache().shipped = dir;
    cache().fresh = false;
}

std::string shippedNodeDir() {
    std::lock_guard<std::mutex> lock(cache().mutex);
    return cache().shipped;
}

std::vector<std::string> nodeSearchDirs() {
    std::vector<std::string> dirs;
    if (const std::optional<std::string> path = environmentVariable("GROOMLAB_NODE_PATH")) dirs = splitPathList(*path);
    if (const std::string user = nodeUserDir(); !user.empty()) dirs.push_back(user);
    if (const std::string shipped = shippedNodeDir(); !shipped.empty()) dirs.push_back(shipped);
    return dirs;
}

std::vector<LibraryNode> listLibraryNodes(const std::vector<std::string>& dirs) {
    std::vector<LibraryNode> out;
    std::set<std::string> seen;
    for (const std::string& dir : dirs) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".glnode") files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            LibraryNode node = readNodeFile(file.u8string());
            if (!node.id.empty() && !seen.insert(node.id).second) continue;  // an earlier folder's hides it
            out.push_back(std::move(node));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LibraryNode& a, const LibraryNode& b) {
        const std::string la = lower(a.label), lb = lower(b.label);
        return la != lb ? la < lb : a.id < b.id;
    });
    return out;
}

std::vector<LibraryNode> libraryNodes() {
    Cache& c = cache();
    const std::vector<std::string> dirs = nodeSearchDirs();
    std::lock_guard<std::mutex> lock(c.mutex);
    // The folders looked at again at most once a second (every node spec asks), at once after a save.
    const auto now = std::chrono::steady_clock::now();
    if (c.fresh && dirs == c.dirs && now - c.checked < std::chrono::seconds(1)) return c.nodes;
    c.checked = now;
    const std::string signature = folderSignature(dirs);
    if (!c.fresh || dirs != c.dirs || signature != c.signature) {
        c.nodes = listLibraryNodes(dirs);
        c.signature = signature;
        c.dirs = dirs;
        c.fresh = true;
    }
    return c.nodes;
}

void refreshNodeLibrary() { markChanged(); }

std::optional<LibraryNode> findLibraryNode(const std::string& id) {
    for (LibraryNode& node : libraryNodes()) {
        if (node.id == id && node.error.empty()) return std::move(node);
    }
    return std::nullopt;
}

std::string libraryNodesJson() {
    markChanged();  // a menu opening: the folders as they are now
    json nodes = json::array(), errors = json::array();
    const auto& categories = nodeCategories();
    int order = static_cast<int>(builtinNodeTypes().size()) + 1000;  // after the built-in nodes of a category
    const std::vector<LibraryNode> library = libraryNodes();
    // Categories of the files' own, by name after the built-in ones; My Nodes last.
    std::set<std::string> own;
    for (const LibraryNode& node : library) {
        if (node.error.empty() && !node.category.empty() && !builtinCategory(node.category)) own.insert(lower(node.category));
    }
    for (const LibraryNode& node : library) {
        if (!node.error.empty()) {
            errors.push_back({{"path", node.path}, {"id", node.id}, {"error", node.error}});
            continue;
        }
        const json definition = json::parse(node.definition);
        std::string category = "mynodes", title = "My Nodes";
        int categoryOrder = static_cast<int>(categories.size() + own.size());
        if (const auto builtin = builtinCategory(node.category)) {
            category = categories[*builtin].id;
            title = categories[*builtin].title;
            categoryOrder = static_cast<int>(*builtin);
        } else if (!node.category.empty()) {
            category = lower(node.category);
            title = node.category;
            categoryOrder = static_cast<int>(categories.size() + std::distance(own.begin(), own.find(category)));
        }
        json inputs = json::array();
        for (const json& in : definition.value("inputs", json::array())) {
            inputs.push_back({{"name", in.value("name", std::string())},
                              {"type", in.value("type", std::string("Strands"))},
                              {"optional", in.value("optional", false)}});
        }
        nodes.push_back({{"type", libraryNodeType(node.id)},
                         {"id", node.id},
                         {"version", node.version},
                         {"label", node.label},
                         {"category", category},
                         {"categoryTitle", title},
                         {"categoryOrder", categoryOrder},
                         {"order", order++},
                         {"description", node.description},
                         {"keywords", trimmed("library " + node.kind + " " + node.author + " " + node.id)},
                         {"kind", node.kind},
                         {"author", node.author},
                         {"license", node.license},
                         {"icon", node.icon},
                         {"path", node.path},
                         {"inputs", inputs},
                         {"output", definition.value("output", json::object())}});
    }
    return json{{"nodes", nodes}, {"errors", errors}, {"dirs", nodeSearchDirs()}, {"userDir", nodeUserDir()}}.dump();
}

std::string libraryNodeParamsJson(const LibraryNode& node, std::optional<double> size) {
    if (!node.error.empty() || node.definition.empty()) throw GraphError("library node '" + node.id + "': " + node.error);
    json definition = json::parse(node.definition);
    const auto recorded = definition.find("size");
    if (definition.contains("script")) {
        // A script node's params fitted by their units, defaults and ranges alike.
        if (size && recorded != definition.end() && recorded->is_number() && recorded->get<double>() > 0.0) {
            const double ratio = *size / recorded->get<double>();
            for (json& p : definition["params"]) {
                const std::string type = p.value("type", std::string("number"));
                const std::string unit = type == "length" ? std::string("length") : p.value("unit", std::string());
                double factor = 1.0;
                if (unit == "length" || unit == "width") factor = ratio;
                else if (unit == "perLength") factor = 1.0 / ratio;
                else if (unit == "density") factor = 1.0 / (ratio * ratio);
                if (factor == 1.0 || (type != "number" && type != "length")) continue;
                for (const char* key : {"default", "min", "max", "softMin", "softMax"}) {
                    if (p.contains(key) && p[key].is_number()) p[key] = p[key].get<double>() * factor;
                }
            }
            definition["size"] = *size;
        }
        return json{{"definition", definition}}.dump();
    }
    if (size && recorded != definition.end() && recorded->is_number() && recorded->get<double>() > 0.0) {
        // Fitted as a preset is (presets.h): the inner nodes' lengths, widths, densities...
        const json graph = {{"version", 1}, {"size", recorded->get<double>()},
                            {"nodes", json::array({{{"name", "n"}, {"type", "Compound"}, {"params", {{"definition", definition}}}}})},
                            {"connections", json::array()}, {"output", "n"}};
        json fitted = json::parse(fitPresetJson(graph.dump(), size))["nodes"][0]["params"]["definition"];
        // Each param's default is its inner value as fitted, its range scaled with it.
        for (json& p : fitted["params"]) {
            const std::string target = p.value("target", std::string());
            const std::size_t dot = target.find('.');
            for (const json& n : fitted["graph"]["nodes"]) {
                if (n.value("name", std::string()) != target.substr(0, dot)) continue;
                const json params = n.value("params", json::object());
                const auto value = params.find(target.substr(dot + 1));
                if (value == params.end()) continue;
                const json before = p.value("default", json());
                if (before.is_number() && value->is_number() && before.get<double>() != 0.0) {
                    const double ratio = value->get<double>() / before.get<double>();
                    for (const char* key : {"min", "max", "softMin", "softMax"}) {
                        if (p.contains(key) && p[key].is_number() && ratio > 0.0) p[key] = p[key].get<double>() * ratio;
                    }
                }
                if (p.contains("default")) p["default"] = *value;
            }
        }
        fitted["size"] = *size;
        definition = std::move(fitted);
    }
    return json{{"definition", definition}}.dump();
}

std::string libraryNodeStubParams(const std::string& id, const std::string& paramsJson, std::string* type) {
    json params = json::parse(paramsJson.empty() ? std::string("{}") : paramsJson, nullptr, false);
    if (!params.is_object()) params = json::object();
    if (!params.contains("definition")) {
        if (const auto node = findLibraryNode(id)) {
            params["definition"] = json::parse(node->definition);
        } else {
            // Nothing to run: it hands its strands on, saying why (CompoundOp).
            params["definition"] = {{"glnode", kNodeFileFormat}, {"id", id}, {"inputs", json::array({{{"name", "strands"}, {"type", "Strands"}}})}};
        }
    }
    if (type) {
        const json& d = params["definition"];
        const bool script = (d.is_object() && d.contains("script")) || (d.is_string() && isScriptNodeFile(d.get<std::string>()));
        *type = script ? op::kScript : op::kCompound;
    }
    return params.dump();
}

bool isScriptNodeFile(const std::string& text) {
    const json file = json::parse(text, nullptr, false);
    return file.is_object() && file.contains("script");
}

ScriptNodeHeader scriptNodeHeader(const std::string& definitionJson) {
    const json file = json::parse(definitionJson, nullptr, false);
    if (file.is_discarded() || !file.is_object()) throw GraphError("the node file is not a JSON object");
    ScriptNodeHeader header;

    // Inputs: strands first, then guides (for guidePoint) at most.
    const json inputs = file.contains("inputs") ? file["inputs"] : json::array({{{"name", "strands"}, {"type", "Strands"}}});
    try {
        header.inputs = compoundInputs(nlohmann::json{{"inputs", nlohmann::json::parse(inputs.dump())}}.dump());
    } catch (const GraphError& e) {
        throw GraphError(std::string("inputs: ") + e.what());
    }
    if (header.inputs.empty() || header.inputs[0].type != PortType::Strands) {
        throw GraphError("inputs: a script node takes strands first ({\"name\": \"strands\", \"type\": \"Strands\"})");
    }
    for (std::size_t i = 1; i < header.inputs.size(); ++i) {
        if (header.inputs[i].type != PortType::Guides || i > 1) {
            throw GraphError("inputs: '" + header.inputs[i].name + "': a script node takes strands, then guides (for guidePoint) at most");
        }
        if (header.inputs[i].name == header.inputs[0].name) throw GraphError("inputs: two are named '" + header.inputs[i].name + "'");
    }
    if (file.contains("output")) {
        const json& out = file["output"];
        if (!out.is_object()) throw GraphError("\"output\" must be an object ({\"name\", \"type\"})");
        const std::string type = textField(out, "type", "output: ");
        if (!type.empty() && type != "Strands") throw GraphError("\"output\": a script node gives Strands, not " + type);
    }

    // What it writes.
    if (file.contains("writes")) {
        if (!file["writes"].is_array()) throw GraphError("\"writes\" must be a list: \"P\", \"width\", \"color\", \"channel.name\"");
        for (const json& w : file["writes"]) {
            const std::string what = w.is_string() ? w.get<std::string>() : w.dump();
            const bool channel = what.rfind("channel.", 0) == 0 && isIdentifier(what.substr(8));
            if (!w.is_string() || (what != "P" && what != "width" && what != "color" && !channel)) {
                throw GraphError("writes: '" + what + "' is none of \"P\", \"width\", \"color\", \"channel.name\" (a channel's name: letters, digits and _)");
            }
            if (std::find(header.writes.begin(), header.writes.end(), what) != header.writes.end()) {
                throw GraphError("writes: '" + what + "' is listed twice");
            }
            header.writes.push_back(what);
        }
    }
    if (!file.contains("script") || !file["script"].is_string() || trimmed(file["script"].get<std::string>()).empty()) {
        throw GraphError("the node file needs a \"script\" (text): per point { ... }");
    }
    header.script = file["script"].get<std::string>();

    // Params: each a spec of its own.
    const json params = file.contains("params") ? file["params"] : json::array();
    if (!params.is_array()) throw GraphError("\"params\" must be a list");
    const std::vector<std::string>& builtins = scriptBuiltinNames();
    std::set<std::string> names;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const json& p = params[i];
        const std::string where = "params[" + std::to_string(i) + "]: ";
        if (!p.is_object()) throw GraphError(where + "must be an object");
        const std::string name = textField(p, "name", where);
        if (!isIdentifier(name)) throw GraphError(where + "\"name\" must be a word of letters, digits and _ ('" + name + "')");
        if (std::find(builtins.begin(), builtins.end(), name) != builtins.end()) {
            throw GraphError(where + "'" + name + "' is a name scripts read already (a built-in input): name the param otherwise");
        }
        for (const ExpressionFunction& f : scriptFunctions()) {
            if (f.name == name) throw GraphError(where + "'" + name + "' is a script function's name: name the param otherwise");
        }
        bool input = false;
        for (const PortSpec& port : header.inputs) input = input || port.name == name;
        if (name == "definition" || name == kAffectParam || input) {
            throw GraphError(where + "the name '" + name + "' is taken (by the node itself or an input)");
        }
        if (!names.insert(name).second) throw GraphError(where + "two params are named '" + name + "'");
        const std::string type = p.contains("type") ? textField(p, "type", where) : std::string("number");
        static const char* const kTypes[] = {"number", "length", "int", "bool", "enum", "color", "ramp"};
        if (std::find(std::begin(kTypes), std::end(kTypes), type) == std::end(kTypes)) {
            throw GraphError(where + "\"type\" '" + type + "' is none of number, length, int, bool, enum, color, ramp");
        }
        for (const char* key : {"label", "description", "unit"}) textField(p, key, where);
        const auto min = numberField(p, "min", where), max = numberField(p, "max", where);
        const auto softMin = numberField(p, "softMin", where), softMax = numberField(p, "softMax", where);
        if (min && max && *min > *max) throw GraphError(where + "\"min\" is above \"max\"");
        if (p.contains("mappable") && !p["mappable"].is_boolean()) throw GraphError(where + "\"mappable\" must be true or false");
        const bool mappable = p.value("mappable", false);
        const bool number = type == "number" || type == "length";
        if (mappable && !number) throw GraphError(where + "only number and length params are mappable");
        if ((min || max || softMin || softMax) && !number && type != "int") throw GraphError(where + "only number, length and int params have ranges");
        if (p.contains("unit") && type != "number") throw GraphError(where + "only a number param takes a \"unit\" (a length's is length)");
        const json value = p.contains("default") ? p["default"] : json();
        auto wrong = [&](const std::string& want) { return GraphError(where + "\"default\" " + value.dump() + " is not " + want); };

        ParamSpec spec;
        spec.name = name;
        ScriptParam script;
        script.name = name;
        if (number) {
            if (!value.is_null() && (!value.is_number() || value.is_boolean())) throw wrong("a number");
            spec.defaultValue = value.is_null() ? 0.0 : value.get<double>();
            spec.unit = type == "length" ? std::string("length") : textField(p, "unit", where);
            script.kind = ScriptParam::Kind::Number;
        } else if (type == "int") {
            if (!value.is_null() && !value.is_number_integer() &&
                !(value.is_number_float() && value.get<double>() == std::floor(value.get<double>()))) {
                throw wrong("a whole number");
            }
            spec.defaultValue = value.is_null() ? std::int64_t{0} : static_cast<std::int64_t>(std::llround(value.get<double>()));
            script.kind = ScriptParam::Kind::Int;
            script.max = max;
        } else if (type == "bool") {
            if (!value.is_null() && !value.is_boolean()) throw wrong("true or false");
            spec.defaultValue = value.is_null() ? false : value.get<bool>();
            script.kind = ScriptParam::Kind::Bool;
        } else if (type == "enum") {
            if (!p.contains("choices") || !p["choices"].is_array() || p["choices"].empty()) {
                throw GraphError(where + "an enum param needs \"choices\" (a list of text)");
            }
            for (const json& c : p["choices"]) {
                if (!c.is_string() || c.get<std::string>().empty()) throw GraphError(where + "\"choices\" are text");
                if (std::find(spec.choices.begin(), spec.choices.end(), c.get<std::string>()) != spec.choices.end()) {
                    throw GraphError(where + "\"choices\" lists '" + c.get<std::string>() + "' twice");
                }
                spec.choices.push_back(c.get<std::string>());
            }
            if (!value.is_null() && (!value.is_string() || std::find(spec.choices.begin(), spec.choices.end(), value.get<std::string>()) == spec.choices.end())) {
                throw wrong("one of the choices");
            }
            spec.defaultValue = value.is_null() ? spec.choices.front() : value.get<std::string>();
            script.kind = ScriptParam::Kind::Choice;
        } else if (type == "color") {
            const bool color = value.is_array() && value.size() == 3 && value[0].is_number() && value[1].is_number() && value[2].is_number();
            if (!value.is_null() && !color) throw wrong("a color [r, g, b]");
            spec.defaultValue = value.is_null() ? Vec3f{1.0f, 1.0f, 1.0f} : Vec3f{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
            spec.unit = "color";
            script.kind = ScriptParam::Kind::Color;
        } else if (type == "ramp") {
            try {
                spec.defaultValue = value.is_null() ? Ramp::linear(0.0f, 1.0f) : rampFromJson(value.dump());
            } catch (const GraphError&) {
                throw wrong("a ramp [[position, value], ...]");
            }
            script.kind = ScriptParam::Kind::Ramp;
        } else {
            throw GraphError(where + "\"type\" '" + type + "' is none of number, length, int, bool, enum, color, ramp");
        }
        spec.description = textField(p, "description", where);
        if (mappable) spec.description += spec.description.empty() ? "(mappable)" : " (mappable)";
        spec.min = min;
        spec.max = max;
        spec.softMin = softMin;
        spec.softMax = softMax;
        header.params.push_back(std::move(spec));
        header.labels.push_back(trimmed(textField(p, "label", where)));
        header.scriptParams.push_back(script);
    }
    return header;
}

std::string scriptNodeErrorsJson(const std::string& text) {
    json errors = json::array();
    auto add = [&](const std::string& part, const std::string& message, int line, int column) {
        errors.push_back({{"part", part}, {"message", message}, {"line", line}, {"column", column}});
    };
    const json file = json::parse(text, nullptr, false);
    if (file.is_discarded() || !file.is_object()) {
        add("json", "the node file is not valid JSON", 0, 0);
        return json{{"errors", errors}}.dump();
    }
    try {
        const ScriptNodeHeader header = scriptNodeHeader(text);
        try {
            Script(header.script, header.scriptParams, header.writes);
        } catch (const ScriptError& e) {
            add("script", e.message(), e.line(), e.column());
            return json{{"errors", errors}}.dump();
        }
        // The rest of the file (id, version, label...) as checkNodeFile checks it.
        checkNodeFile(text);
    } catch (const GraphError& e) {
        add("header", e.what(), 0, 0);
    }
    return json{{"errors", errors}}.dump();
}

std::string scriptNodeTemplate(const std::string& label, const std::string& author) {
    const std::string name = trimmed(label).empty() ? std::string("My Script") : trimmed(label);
    json file = {{"glnode", kNodeFileFormat},
                 {"id", suggestNodeId(name, author)},
                 {"version", 1},
                 {"label", name},
                 {"category", "shape"},
                 {"description", "Pushes each strand out from the scalp, more toward the tip."}};
    if (!trimmed(author).empty()) file["author"] = trimmed(author);
    file["size"] = kDefaultsScalpSize;
    file["inputs"] = json::array({{{"name", "strands"}, {"type", "Strands"}}});
    file["output"] = {{"name", "out"}, {"type", "Strands"}};
    file["params"] = json::array({{{"name", "amount"},
                                   {"type", "length"},
                                   {"label", "Amount"},
                                   {"default", 0.5},
                                   {"min", 0},
                                   {"softMax", 3},
                                   {"mappable", true},
                                   {"description", "How far the tips move out"}}});
    file["writes"] = json::array({"P"});
    file["script"] =
        "# Runs for every point of every strand, root to tip.\n"
        "# P is the point, t goes 0 (root) to 1 (tip), N is the scalp's normal at the root,\n"
        "# $amount is the node's param. Only what \"writes\" lists may be assigned.\n"
        "per point {\n"
        "    P += N * $amount * t * t;\n"
        "}\n";
    return file.dump(2);
}

std::string nodeFileFromCompound(const std::string& graphJson, const std::string& compound, const std::string& headerJson,
                                 std::optional<double> size) {
    const json graph = json::parse(graphJson, nullptr, false);
    if (!graph.is_object()) throw GraphError("the graph is not a JSON object");
    const json* found = nullptr;
    if (graph.contains("nodes") && graph["nodes"].is_array()) {
        for (const json& n : graph["nodes"]) {
            if (n.is_object() && n.value("name", std::string()) == compound) found = &n;
        }
    }
    if (!found) throw GraphError("no node named '" + compound + "'");
    const json& node = *found;
    if (node.value("type", std::string()) != "Compound") throw GraphError(compound + " is not a Compound");
    const json header = headerJson.empty() ? json::object() : json::parse(headerJson, nullptr, false);
    if (!header.is_object()) throw GraphError("the node's header is not a JSON object");
    const json nodeParams = node.value("params", json::object());
    json definition = nodeParams.value("definition", json::object());
    if (definition.is_string()) definition = json::parse(definition.get<std::string>(), nullptr, false);
    if (!definition.is_object() || !definition.contains("graph")) throw GraphError(compound + " has no subgraph to save");
    const bool library = definition.contains("glnode");

    // The header: given, else the library copy's, else from the compound.
    auto pick = [&](const char* key, const std::string& fallback) {
        const std::string given = trimmed(textField(header, key));
        if (!given.empty()) return given;
        const std::string own = library ? trimmed(textField(definition, key)) : std::string();
        return own.empty() ? fallback : own;
    };
    const std::string label = pick("label", nameWords(compound));
    const std::string author = pick("author", "");
    const std::string id = pick("id", suggestNodeId(label, author));
    if (const std::string error = nodeIdError(id); !error.empty()) throw GraphError(error);
    int version = 0;
    if (header.contains("version") && header["version"].is_number_integer()) version = header["version"].get<int>();
    if (version < 1) {  // the next after the library's copy (a new id: 1)
        const auto existing = findLibraryNode(id);
        version = existing ? existing->version + 1 : 1;
    }

    // The inner graph with the compound's values of every exposed param in it.
    json inner = definition["graph"];
    const json expose = definition.value("expose", json::object());
    for (const auto& [name, target] : expose.items()) {
        if (!target.is_string() || !nodeParams.contains(name)) continue;
        const std::string t = target.get<std::string>();
        const std::size_t dot = t.find('.');
        for (json& n : inner["nodes"]) {
            if (n.value("name", std::string()) != t.substr(0, dot)) continue;
            if (!n.contains("params") || !n["params"].is_object()) n["params"] = json::object();
            n["params"][t.substr(dot + 1)] = nodeParams[name];
        }
    }

    // The params: the exposed ones as the node's spec shows them (range, unit, description).
    const json spec = json::parse(nodeSpecJson(nlohmann::json::parse(node.dump()).dump()));
    std::vector<std::string> chosen;
    if (header.contains("params") && header["params"].is_array()) {
        for (const json& p : header["params"]) {
            if (!p.is_string() || !expose.contains(p.get<std::string>())) {
                throw GraphError("'" + (p.is_string() ? p.get<std::string>() : p.dump()) + "' is not a param " + compound + " exposes");
            }
            chosen.push_back(p.get<std::string>());
        }
    } else {
        for (const json& p : spec.value("params", json::array())) {
            const std::string name = p.value("name", std::string());
            if (expose.contains(name)) chosen.push_back(name);
        }
    }
    const json info = header.value("paramInfo", json::object());
    json params = json::array();
    for (const std::string& name : chosen) {
        json shown = json::object();
        for (const json& p : spec.value("params", json::array())) {
            if (p.value("name", std::string()) == name) shown = p;
        }
        const json own = info.is_object() ? info.value(name, json::object()) : json::object();
        auto text = [&](const char* key, const std::string& fallback) {
            const std::string given = own.is_object() ? trimmed(textField(own, key)) : std::string();
            return given.empty() ? fallback : given;
        };
        std::string description = shown.value("description", std::string());
        if (const std::size_t at = description.rfind(" (exposed "); at != std::string::npos) description.erase(at);
        json p = {{"name", name}, {"target", expose[name]}, {"label", text("label", shown.value("label", paramWords(name)))}};
        const std::string t = expose[name].get<std::string>();
        const std::size_t dot = t.find('.');
        for (const json& n : inner["nodes"]) {
            if (n.value("name", std::string()) == t.substr(0, dot) && n.contains("params") && n["params"].contains(t.substr(dot + 1))) {
                p["default"] = n["params"][t.substr(dot + 1)];
            }
        }
        if (!p.contains("default") && shown.contains("default")) p["default"] = shown["default"];
        for (const char* key : {"min", "max", "softMin", "softMax"}) {
            if (own.is_object() && own.contains(key) && own[key].is_number()) p[key] = own[key];
            else if (shown.contains(key) && shown[key].is_number()) p[key] = shown[key];
        }
        if (!shown.value("unit", std::string()).empty()) p["unit"] = shown["unit"];
        if (shown.value("mappable", false)) p["mappable"] = true;
        p["description"] = text("description", description);
        params.push_back(std::move(p));
    }

    json file = {{"glnode", kNodeFileFormat}, {"id", id}, {"version", version}, {"label", label},
                 {"category", pick("category", "")}, {"description", pick("description", "")}};
    for (const char* key : {"author", "license", "icon"}) {
        const std::string value = pick(key, "");
        if (!value.empty()) file[key] = value;
    }
    if (size && *size > 0.0) file["size"] = *size;
    else if (library && definition.contains("size")) file["size"] = definition["size"];
    file["inputs"] = definition.value("inputs", json::array());
    json output = spec.value("output", json::object());
    file["output"] = {{"name", output.value("name", std::string("out"))}, {"type", output.value("type", std::string("Strands"))}};
    file["params"] = params;
    file["graph"] = inner;
    const std::string text = file.dump(2);
    checkNodeFile(text);
    return text;
}

std::string saveNodeFile(const std::string& text, const std::string& dir) {
    const json file = json::parse(checkNodeFile(text));
    const std::string folder = dir.empty() ? nodeUserDir() : dir;
    if (folder.empty()) throw GraphError("no folder to save the node in (set GROOMLAB_USER_NODES)");
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    const fs::path path = fs::u8path(folder) / fs::u8path(file["id"].get<std::string>() + ".glnode");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw GraphError("cannot write " + path.generic_u8string());
        out << text;
        if (text.empty() || text.back() != '\n') out << '\n';
        if (!out) throw GraphError("cannot write " + path.generic_u8string());
    }
    markChanged();
    return path.generic_u8string();
}

std::string importNodeFile(const std::string& path, const std::string& dir) {
    std::string text = readText(path);
    try {
        checkNodeFile(text);
    } catch (const GraphError& e) {
        throw GraphError(fs::u8path(path).filename().u8string() + ": " + e.what());
    }
    return saveNodeFile(text, dir);
}

std::string withScriptDefinition(const std::string& graphJson, const std::string& node, const std::string& definitionJson) {
    const json definition = json::parse(definitionJson);
    std::string errors;
    const json checked = json::parse(scriptNodeErrorsJson(definition.dump()));  // (held: a temporary's member would dangle)
    for (const json& e : checked["errors"]) {
        if (!errors.empty()) errors += "\n";
        if (e.value("line", 0) > 0) errors += "line " + std::to_string(e["line"].get<int>()) + ", column " + std::to_string(e["column"].get<int>()) + ": ";
        errors += e.value("message", std::string());
    }
    if (!errors.empty()) throw std::invalid_argument(errors);
    auto defaults = [](const json& n) {
        std::map<std::string, json> out;
        for (const json& p : json::parse(nodeSpecJson(n.dump())).value("params", json::array())) out[p.value("name", std::string())] = p.value("default", json());
        return out;
    };
    auto names = [](const json& d) {
        std::set<std::string> out;
        for (const json& p : d.value("params", json::array())) {
            if (p.is_object() && p.value("name", json()).is_string()) out.insert(p["name"].get<std::string>());
        }
        return out;
    };
    json g = json::parse(graphJson);
    bool found = false;
    for (json& item : g["nodes"]) {
        if (item.value("name", std::string()) != node) continue;
        if (item.value("type", std::string()) != "Script") break;
        found = true;
        const std::map<std::string, json> before = defaults(item);
        if (!item.contains("params") || !item["params"].is_object()) item["params"] = json::object();
        json& params = item["params"];
        const std::set<std::string> oldNames = names(params.value("definition", json::object())), newNames = names(definition);
        params["definition"] = definition;
        for (const std::string& gone : oldNames) {
            if (newNames.count(gone)) continue;
            params.erase(gone);
            if (item.contains("paramInputs") && item["paramInputs"].is_array()) {
                json kept = json::array();
                for (const json& p : item["paramInputs"]) {
                    if (p != gone) kept.push_back(p);
                }
                item["paramInputs"] = kept;
            }
            json links = json::array();
            for (const json& c : g.value("connections", json::array())) {
                if (c.value("to", std::string()) != node || c.value("input", std::string()) != gone) links.push_back(c);
            }
            g["connections"] = links;
        }
        // A param still at its old default takes the new one; a new one, its default.
        for (const auto& [name, value] : defaults(item)) {
            if (!newNames.count(name)) continue;
            const auto old = before.find(name);
            if (!params.contains(name) || (old != before.end() && params[name] == old->second)) params[name] = value;
        }
        break;
    }
    if (!found) throw std::invalid_argument(node + " is not a script node");
    return g.dump();
}

}  // namespace groomlab
