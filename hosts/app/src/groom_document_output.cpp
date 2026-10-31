// SPDX-License-Identifier: Apache-2.0
// GroomDocument: what the GroomLab menu's Add, Simulate, Import and Utilities helpers do to the
// groom (groom_document.h), as Maya's (hosts/maya/scripts/groomlab: motion.py, sim.py,
// collision.py, feathers and braids) and Blender's pipeline.py: the graph edited through the core's
// GraphEdit (graph_edit.h: names, positions, fitted defaults, checked connections), the files
// through the core's writers.

#include "groom_document.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <fstream>
#include <map>
#include <stdexcept>

#include "groomlab/alembic_io.h"
#include "groomlab/graph_edit.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"

namespace groomlab::app {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A new node's place beside another (the core's column and row, Maya's LAYOUT_DX / LAYOUT_DY).
constexpr double kDX = 240.0, kDY = 140.0;

struct Feed {
    std::string from, output;  // "" nothing feeds it
};

// What feeds `to`'s `input`.
Feed feedOf(const json& g, const std::string& to, const std::string& input) {
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("to", std::string()) == to && c.value("input", std::string()) == input) {
            return {c.value("from", std::string()), c.value("output", std::string())};
        }
    }
    return {};
}

// Every (node, input) `from` feeds.
std::vector<std::pair<std::string, std::string>> fedBy(const json& g, const std::string& from) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("from", std::string()) == from) out.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
    }
    return out;
}

const json* nodeIn(const json& g, const std::string& name) {
    const auto nodes = g.find("nodes");  // (a pointer into g itself, not into a copy)
    if (nodes == g.end() || !nodes->is_array()) return nullptr;
    for (const json& n : *nodes) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

std::string typeOf(const json& g, const std::string& name) {
    const json* n = nodeIn(g, name);
    return n ? n->value("type", std::string()) : std::string();
}

json paramOf(const json& g, const std::string& node, const std::string& param) {
    const json* n = nodeIn(g, node);
    if (!n || !n->contains("params") || !(*n)["params"].is_object()) return nullptr;
    const auto it = (*n)["params"].find(param);
    return it == (*n)["params"].end() ? json() : *it;
}

std::vector<std::string> ofType(const json& g, const std::string& type) {
    std::vector<std::string> out;
    for (const json& n : g.value("nodes", json::array())) {
        if (n.value("type", std::string()) == type) out.push_back(n.value("name", std::string()));
    }
    return out;
}

// Where a node is drawn (nullopt: the view lays it out).
std::optional<std::pair<double, double>> positionOf(const json& g, const std::string& name) {
    const json* n = nodeIn(g, name);
    if (!n || !n->contains("ui") || !(*n)["ui"].is_object()) return std::nullopt;
    const json& ui = (*n)["ui"];
    if (!ui.contains("x") || !ui.contains("y") || !ui["x"].is_number() || !ui["y"].is_number()) return std::nullopt;
    return std::make_pair(ui["x"].get<double>(), ui["y"].get<double>());
}

// The graph edited as GraphEdit sees it, its JSON read back for the lookups between the edits.
struct GraphEdits {
    GraphEdit e;
    std::optional<double> size;
    explicit GraphEdits(const json& g, std::optional<double> scalpSize) : e(g.dump()), size(scalpSize) {}
    json now() const { return json::parse(e.toJson()); }
    std::string add(const std::string& type, const std::string& base, const json& params,
                    std::optional<std::pair<double, double>> at = std::nullopt) {
        const std::string name = e.uniqueName(base.empty() ? type : base);
        return e.addNode(type, name, params.is_null() ? std::string() : params.dump(), at ? std::optional<double>(at->first) : std::nullopt,
                         at ? std::optional<double>(at->second) : std::nullopt, size);
    }
    // src -> node -> dst in place of every src -> dst link (Blender's insert_between).
    void insertBetween(const std::string& src, const std::string& dst, const std::string& node, const std::string& nodeInput) {
        const json g = now();
        std::vector<std::pair<std::string, std::string>> links;  // (input, output)
        for (const json& c : g.value("connections", json::array())) {
            if (c.value("from", std::string()) == src && c.value("to", std::string()) == dst) {
                links.emplace_back(c.value("input", std::string()), c.value("output", std::string()));
            }
        }
        if (links.empty()) throw std::runtime_error(src + " is not connected to " + dst);
        e.connect(src, node, nodeInput, links.front().second);
        for (const auto& [input, output] : links) e.connect(node, dst, input);
    }
    // `node` on what feeds `anchor`'s `input`, drawn a row under the anchor.
    void insertBefore(const std::string& anchor, const std::string& node, const std::string& input = "strands",
                      const std::string& nodeInput = "strands") {
        const json g = now();
        const Feed feed = feedOf(g, anchor, input);
        if (feed.from.empty()) throw std::runtime_error(anchor + " has no " + input + " input to insert before");
        insertBetween(feed.from, anchor, node, nodeInput);
        place(node, anchor, 0.0, kDY);
    }
    void place(const std::string& node, const std::string& beside, double dx, double dy) {
        if (const auto at = positionOf(now(), beside)) {
            json positions = json::object();
            positions[node] = {at->first + dx, at->second + dy};
            setPositions(positions);
        }
    }
    void setPositions(const json& positions) {
        json g = now();
        for (json& n : g["nodes"]) {
            const std::string name = n.value("name", std::string());
            if (positions.contains(name)) n["ui"] = {{"x", positions[name][0]}, {"y", positions[name][1]}};
        }
        e = GraphEdit(g.dump());
    }
    void setParam(const std::string& node, const std::string& param, const json& value) {
        json g = now();
        for (json& n : g["nodes"]) {
            if (n.value("name", std::string()) == node) n["params"][param] = value;
        }
        e = GraphEdit(g.dump());
    }
};

// Where a motion node goes (Maya's motion_anchor): just before the output, but before the Collide
// and Cards nodes ending its chain, so the strands collide where they moved to and the cards take
// the motion.
std::string motionAnchor(const json& g, const std::string& output) {
    std::string anchor = output;
    std::set<std::string> seen;
    while (seen.insert(anchor).second) {
        const Feed feed = feedOf(g, anchor, "strands");
        const std::string type = feed.from.empty() ? std::string() : typeOf(g, feed.from);
        if (type != "Collide" && type != "Cards") break;
        anchor = feed.from;
    }
    return anchor;
}

// Braid paths and feather shafts are not hair: Guides nodes reading every guide leave `group` out.
void excludeFromHairGuides(GraphEdits& edit, const std::string& group) {
    const json g = edit.now();
    for (const std::string& n : ofType(g, "Guides")) {
        const json own = paramOf(g, n, "group");
        if (own.is_string() && !own.get<std::string>().empty()) continue;
        const json ex = paramOf(g, n, "exclude");
        std::string text = ex.is_string() ? ex.get<std::string>() : std::string();
        std::replace(text.begin(), text.end(), ',', ' ');
        std::vector<std::string> words;
        std::string word;
        for (const char c : text + " ") {
            if (c == ' ') {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += c;
            }
        }
        if (std::find(words.begin(), words.end(), group) != words.end()) continue;
        words.push_back(group);
        std::string joined;
        for (const std::string& w : words) joined += (joined.empty() ? "" : " ") + w;
        edit.setParam(n, "exclude", joined);
    }
}

// The params of the Propagate nodes inside a Compound's params (and its compounds'), to edit in
// place (Blender's _propagates_in).
void propagatesIn(json& params, std::vector<json*>* out) {
    if (!params.is_object() || !params.contains("definition") || !params["definition"].is_object()) return;
    json& definition = params["definition"];
    if (!definition.contains("graph") || !definition["graph"].is_object() || !definition["graph"].contains("nodes")) return;
    for (json& node : definition["graph"]["nodes"]) {
        const std::string type = node.value("type", std::string());
        if (type == "Propagate") {
            if (!node.contains("params") || !node["params"].is_object()) node["params"] = json::object();
            out->push_back(&node["params"]);
        } else if (type == "Compound") {
            if (!node.contains("params") || !node["params"].is_object()) node["params"] = json::object();
            propagatesIn(node["params"], out);
        }
    }
}

// {group name: groupId} of the feather designs already in the graph (their Propagates').
std::map<std::string, int> featherGroups(const json& g) {
    std::map<std::string, int> groups;
    for (const json& n : g.value("nodes", json::array())) {
        json params = n.value("params", json::object());
        std::vector<json*> found;
        if (n.value("type", std::string()) == "Propagate") found.push_back(&params);
        else if (n.value("type", std::string()) == "Compound") propagatesIn(params, &found);
        for (const json* p : found) {
            const int id = p->contains("groupId") && (*p)["groupId"].is_number() ? (*p)["groupId"].get<int>() : 0;
            if (id <= 0) continue;
            const std::string name = p->contains("groupName") && (*p)["groupName"].is_string() ? (*p)["groupName"].get<std::string>()
                                                                                                 : "group" + std::to_string(id);
            groups.emplace(name, id);
        }
    }
    return groups;
}

// ---- Files ----------------------------------------------------------------------------------------

const char* const kUdimTokens[] = {"<UDIM>", "<udim>", "%(UDIM)d"};

const char* udimToken(const std::string& path) {
    for (const char* t : kUdimTokens) {
        if (path.find(t) != std::string::npos) return t;
    }
    return nullptr;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Whether a file is there; a UDIM tile set when any tile is.
bool fileThere(const std::string& path) {
    std::error_code ec;
    if (path.empty()) return false;
    if (const char* token = udimToken(path)) {
        const fs::path p = fs::u8path(path);
        const std::string base = p.filename().u8string();
        const std::size_t at = base.find(token);
        const std::string head = base.substr(0, at), tail = base.substr(at + std::string(token).size());
        const fs::path folder = p.parent_path().empty() ? fs::path(".") : p.parent_path();
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            if (name.size() == head.size() + 4 + tail.size() && name.compare(0, head.size(), head) == 0 &&
                name.compare(name.size() - tail.size(), tail.size(), tail) == 0 &&
                std::all_of(name.begin() + static_cast<std::ptrdiff_t>(head.size()), name.begin() + static_cast<std::ptrdiff_t>(head.size() + 4),
                            [](unsigned char c) { return std::isdigit(c); })) {
                return true;
            }
        }
        return false;
    }
    return fs::is_regular_file(fs::u8path(path), ec);
}

bool isImagePath(const std::string& s) {
    static const char* const kExtensions[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic",
                                             ".pnm", ".ppm", ".pgm", ".exr", ".tif", ".tiff"};
    const std::string l = lower(s);
    if (s.find("<UDIM>") != std::string::npos) return true;
    for (const char* e : kExtensions) {
        const std::string ext = e;
        if (l.size() >= ext.size() && l.compare(l.size() - ext.size(), ext.size(), ext) == 0) return true;
    }
    return false;
}

// Every graph param naming a file (Maya's presets._file_params): ImportCurves' path, FeatherColor's
// textures, a {"texture": ...} value, HairColor's map and Interpolate's regionMap when an image's
// path; inside compounds too (compound/inner.param). `visit(label, the param's value)`.
void fileParams(json& graph, const std::string& where, const std::function<void(const std::string&, json&)>& visit) {
    if (!graph.is_object() || !graph.contains("nodes")) return;
    for (json& node : graph["nodes"]) {
        if (!node.contains("params") || !node["params"].is_object()) continue;
        const std::string name = node.value("name", std::string()), type = node.value("type", std::string());
        for (auto it = node["params"].begin(); it != node["params"].end(); ++it) {
            const std::string param = it.key();
            json& value = it.value();
            const bool named = param == "path" || param == "texture" || param == "textureRight";
            const bool image = ((param == "map" && type == "HairColor") || (param == "regionMap" && type == "Interpolate")) &&
                               value.is_string() && isImagePath(value.get<std::string>());
            if (value.is_object() && value.contains("texture") && value["texture"].is_string()) {
                visit(where + name + "." + param, value["texture"]);
            } else if ((named || image) && value.is_string()) {
                visit(where + name + "." + param, value);
            }
        }
        if (type == "Compound" && node["params"].contains("definition") && node["params"]["definition"].is_object()) {
            json& definition = node["params"]["definition"];
            if (definition.contains("graph")) fileParams(definition["graph"], where + name + "/", visit);
        }
    }
}

std::string trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The file of `index` (lower-cased name -> paths) that `stored` names (Maya's painting._match_file):
// one fits (the path), none, or several (the candidates), several of a name told apart by the ends of
// their folders.
std::string matchFile(const std::map<std::string, std::vector<std::string>>& index, const std::string& stored,
                      std::vector<std::string>* candidatesOut) {
    std::vector<std::string> parts;
    {
        std::string part;
        for (const char c : stored + "/") {
            if (c == '/' || c == '\\') {
                if (!part.empty() && part != ".") parts.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
    }
    if (parts.empty()) return {};
    std::string base = parts.back();
    const char* token = udimToken(base);
    std::string key = base;
    if (token) key.replace(key.find(token), std::string(token).size(), "1001");
    const auto it = index.find(lower(key));
    if (it == index.end()) return {};
    std::vector<std::string> candidates = it->second;
    if (token) {
        for (std::string& c : candidates) c = fs::u8path(c).parent_path().generic_u8string() + "/" + base;
    }
    if (candidates.size() == 1) return candidates.front();
    auto tail = [&](const std::string& path) {
        std::vector<std::string> theirs;
        std::string part;
        for (const char c : path + "/") {
            if (c == '/') {
                if (!part.empty()) theirs.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        std::size_t n = 0;
        while (n < parts.size() && n < theirs.size() && lower(parts[parts.size() - 1 - n]) == lower(theirs[theirs.size() - 1 - n])) ++n;
        return n;
    };
    std::vector<std::pair<std::size_t, std::string>> scores;
    for (const std::string& c : candidates) scores.emplace_back(tail(c), c);
    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    if (scores[0].first > 1 && scores[0].first > scores[1].first) return scores[0].second;
    std::sort(candidates.begin(), candidates.end());
    if (candidatesOut) *candidatesOut = candidates;
    return {};
}

}  // namespace

// ---- Add and Simulate -------------------------------------------------------------------------------

std::string GroomDocument::addMotionNode(const std::string& type, const json& params, std::string* made) {
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add " + type, [&](json& g) {
        GraphEdits edit(g, scalp ? presetScalpSize(*scalp) : std::nullopt);
        const std::string output = edit.e.outputNode();
        if (output.empty()) throw std::runtime_error("the graph has no output");
        name = edit.add(type, type, params);
        edit.insertBefore(motionAnchor(edit.now(), output), name);
        g = edit.now();
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::addSimulateNode(std::string* made) {
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add Simulate", [&](json& g) {
        GraphEdits edit(g, scalp ? presetScalpSize(*scalp) : std::nullopt);
        const json now = edit.now();
        auto interpolates = [&](const std::string& source) {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& [to, input] : fedBy(now, source)) {
                if (typeOf(now, to) == "Interpolate") out.emplace_back(to, input);
            }
            return out;
        };
        std::string source;
        for (const std::string& n : ofType(now, "Guides")) {
            if (!interpolates(n).empty()) {
                source = n;
                break;
            }
        }
        if (source.empty()) throw std::runtime_error("the groom's graph has no Guides node feeding an Interpolate");
        const auto consumers = interpolates(source);
        const auto at = positionOf(now, source);
        name = edit.add("Simulate", "Simulate", nullptr,
                        at ? std::optional<std::pair<double, double>>({at->first + kDX / 2.0, at->second + kDY}) : std::nullopt);
        for (const auto& [to, input] : consumers) edit.e.connect(name, to, input);
        edit.e.connect(source, name, "guides");
        g = edit.now();
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::addCollider(const std::string& meshName, std::string* made) {
    if (meshName.empty() || meshName == "scalp") return "pick another collider name than '" + meshName + "'";
    for (const std::string& n : ofType(graph_, "ColliderMesh")) {
        const json own = paramOf(graph_, n, "name");
        if (own.is_string() && own.get<std::string>() == meshName) {
            if (made) *made = n;  // that mesh collides already
            return {};
        }
    }
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add collider " + meshName, [&](json& g) {
        GraphEdits edit(g, scalp ? presetScalpSize(*scalp) : std::nullopt);
        const std::string output = edit.e.outputNode();
        if (output.empty()) throw std::runtime_error("the graph has no output");
        // Node names of letters, digits and _ (a prop's name may be a file's: "collar.obj").
        std::string base;
        for (const char c : meshName) base += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
        if (base.empty() || std::isdigit(static_cast<unsigned char>(base[0]))) base = "collider" + base;
        name = edit.add("ColliderMesh", base, json{{"name", meshName}});
        const std::string collide = edit.add("Collide", "collide_" + base, nullptr);
        edit.insertBefore(output, collide);
        edit.e.connect(name, collide, "collider");
        const json now = edit.now();
        if (feedOf(now, collide, "scalp").from.empty()) {
            for (const std::string& s : ofType(now, "Scalp")) {
                const json input = paramOf(now, s, "input");
                if (input.is_null() || (input.is_string() && input.get<std::string>() == "scalp")) {
                    edit.e.connect(s, collide, "scalp");  // off the scalp too, as Collide is meant
                    break;
                }
            }
        }
        edit.place(name, collide, -220.0, -60.0);
        g = edit.now();
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::ensureGuideGroup(const std::string& name) {
    if (name.empty()) return "a guide group needs a name";
    const HierarchyPtr h = session().authoredHierarchy();
    if (h && h->findGroup(name) != kNoGroup) return {};
    return editGuides("add group " + name, [&](GuideHierarchy& hierarchy) { hierarchy.addGroup(name); });
}

std::vector<std::string> GroomDocument::featherGuideGroups() {
    // The groups of the Guides nodes (their `group`) that feed a feather design: a Propagate, or a
    // Compound with one (Maya's feather_guide_groups).
    std::set<std::string> designs;
    for (const json& n : graph_.value("nodes", json::array())) {
        json params = n.value("params", json::object());
        std::vector<json*> found;
        if (n.value("type", std::string()) == "Propagate") found.push_back(&params);
        else if (n.value("type", std::string()) == "Compound") propagatesIn(params, &found);
        if (!found.empty()) designs.insert(n.value("name", std::string()));
    }
    std::vector<std::string> groups;
    for (const std::string& guides : ofType(graph_, "Guides")) {
        const json own = paramOf(graph_, guides, "group");
        std::string text = own.is_string() ? own.get<std::string>() : std::string();
        std::replace(text.begin(), text.end(), ',', ' ');
        std::vector<std::string> names;
        std::string word;
        for (const char c : text + " ") {
            if (c != ' ') {
                word += c;
            } else if (!word.empty()) {
                names.push_back(word);
                word.clear();
            }
        }
        if (names.empty()) continue;
        std::set<std::string> seen;
        std::vector<std::string> todo{guides};
        bool feeds = false;
        while (!todo.empty() && !feeds) {
            const std::string n = todo.back();
            todo.pop_back();
            if (!seen.insert(n).second) continue;
            feeds = designs.count(n) > 0;
            for (const auto& [to, input] : fedBy(graph_, n)) todo.push_back(to);
        }
        for (const std::string& name : names) {
            if (feeds && std::find(groups.begin(), groups.end(), name) == groups.end()) groups.push_back(name);
        }
    }
    const HierarchyPtr h = session().authoredHierarchy();
    if (std::find(groups.begin(), groups.end(), "feathers") == groups.end() && h && h->findGroup("feathers") != kNoGroup) {
        groups.push_back("feathers");
    }
    return groups;
}

std::string GroomDocument::addFeathers(const std::vector<std::pair<std::string, std::string>>& designs, const std::vector<double>& weights,
                                       const std::string& source, const std::string& group, std::string* made) {
    if (designs.empty() || designs.size() > 4) return "one to four feather designs (a Split has four outputs)";
    std::vector<double> shares = weights.empty() ? std::vector<double>(designs.size(), 1.0) : weights;
    if (shares.size() != designs.size()) return "one weight per design";
    if (source != "scatter" && source != "guides") return "the shafts come from scatter or guides, not '" + source + "'";
    const MeshPtr& scalp = file().data.scalp;
    const std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    // Each design: its preset's feather Compound, fitted to this scalp, and its shafts' Scatter params.
    struct Part {
        std::string design;
        json compound;
        json scatter = json::object();
    };
    std::vector<Part> parts;
    for (const auto& [design, text] : designs) {
        json preset;
        try {
            preset = json::parse(presetGraphJson(text, size));
        } catch (const std::exception& e) {
            return "cannot read the feather design " + design + ": " + e.what();
        }
        Part part;
        part.design = design;
        for (const json& n : preset.value("nodes", json::array())) {
            const std::string type = n.value("type", std::string());
            if (type == "Compound" && part.compound.is_null()) part.compound = n;
            if (type == "Scatter" && part.scatter.empty()) {
                const json p = n.value("params", json::object());
                for (const char* k : {"density", "length"}) {
                    if (p.contains(k)) part.scatter[k] = p[k];
                }
            }
        }
        if (part.compound.is_null()) return "the feather design " + design + " has no feather (a Compound node)";
        parts.push_back(std::move(part));
    }
    if (source == "guides") {
        if (const std::string error = ensureGuideGroup(group); !error.empty()) return error;
    }
    std::string first;
    const std::string error = editGraph("add feathers", [&](json& g) {
        GraphEdits edit(g, size);
        const std::string out = edit.e.outputNode();
        if (out.empty()) throw std::runtime_error("the graph has no output");
        std::vector<std::string> made_;
        std::string shafts;
        if (source == "scatter") {
            const json now = edit.now();
            std::string scalpNode = ofType(now, "Scalp").empty() ? std::string() : ofType(now, "Scalp").front();
            if (scalpNode.empty()) {
                scalpNode = edit.add("Scalp", "scalp", nullptr);
                made_.push_back(scalpNode);
            }
            std::string guides;
            for (const std::string& n : ofType(now, "Guides")) {
                const json own = paramOf(now, n, "group");
                if (!own.is_string() || own.get<std::string>().empty()) {
                    guides = n;
                    break;
                }
            }
            if (guides.empty()) {
                guides = edit.add("Guides", "guides", nullptr);
                made_.push_back(guides);
            }
            const std::string scatter = edit.add("Scatter", "featherShafts", parts.front().scatter);
            const std::string shapes = edit.add("Interpolate", "featherShapes", nullptr);
            edit.e.connect(scalpNode, scatter, "scalp");
            edit.e.connect(scatter, shapes, "strands");
            edit.e.connect(guides, shapes, "guides");
            edit.e.connect(scalpNode, shapes, "scalp");
            shafts = shapes;
            made_.push_back(scatter);
            made_.push_back(shapes);
        } else {
            excludeFromHairGuides(edit, group);
            const json now = edit.now();
            for (const std::string& n : ofType(now, "Guides")) {
                const json own = paramOf(now, n, "group");
                if (own.is_string() && own.get<std::string>() == group) shafts = n;
            }
            if (shafts.empty()) {
                shafts = edit.add("Guides", "featherGuides", json{{"group", group}});
                made_.push_back(shafts);
            }
        }
        std::vector<std::pair<std::string, std::string>> feeds{{shafts, ""}};
        if (parts.size() > 1) {
            json params = json::object();
            for (int i = 0; i < 4; ++i) params[std::string("weight") + "ABCD"[i]] = i < static_cast<int>(shares.size()) ? shares[i] : 0.0;
            const std::string split = edit.add("Split", "featherSplit", params);
            edit.e.connect(shafts, split, "strands");
            feeds.clear();
            for (std::size_t i = 0; i < parts.size(); ++i) feeds.emplace_back(split, std::string(1, "abcd"[i]));
            made_.push_back(split);
        }
        // Each design a groom group of its own (groupId, groupName on its Propagate): Unreal gives it
        // its own material, LODs and physics; the same design again joins its group.
        std::map<std::string, int> groups = featherGroups(edit.now());
        std::vector<std::string> names;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            json params = parts[i].compound.value("params", json::object());
            if (!groups.count(parts[i].design)) {
                int most = 0;
                for (const auto& [n, id] : groups) most = std::max(most, id);
                groups[parts[i].design] = most + 1;
            }
            std::vector<json*> propagates;
            propagatesIn(params, &propagates);
            for (json* p : propagates) {
                (*p)["groupId"] = groups[parts[i].design];
                (*p)["groupName"] = parts[i].design;
            }
            const std::string name = edit.add("Compound", parts[i].compound.value("name", std::string("feathers")), params);
            edit.e.connect(feeds[i].first, name, "shafts", feeds[i].second);
            names.push_back(name);
        }
        std::string feathers = names.front();
        std::vector<std::string> merges;
        for (std::size_t i = 1; i < names.size(); ++i) {
            const std::string merge = edit.add("Merge", "featherMerge", nullptr);
            edit.e.connect(feathers, merge, "a");
            edit.e.connect(names[i], merge, "b");
            feathers = merge;
            merges.push_back(merge);
        }
        // A row under the output: the shafts, the split, the designs, then their merges.
        if (const auto at = positionOf(edit.now(), out)) {
            const double y = at->second + 2 * kDY;
            const double x = at->first - static_cast<double>(made_.size() + 2) * kDX;
            json positions = json::object();
            for (std::size_t i = 0; i < made_.size(); ++i) positions[made_[i]] = {x + static_cast<double>(i) * kDX, y};
            for (std::size_t i = 0; i < names.size(); ++i) positions[names[i]] = {x + static_cast<double>(made_.size()) * kDX, y + static_cast<double>(i) * kDY};
            for (std::size_t i = 0; i < merges.size(); ++i) positions[merges[i]] = {x + static_cast<double>(made_.size() + 1) * kDX, y + static_cast<double>(i) * kDY};
            edit.setPositions(positions);
        }
        if (!feedOf(edit.now(), out, "strands").from.empty()) {
            const std::string final_ = edit.add("Merge", "feathersMerge", nullptr);
            edit.insertBefore(out, final_, "strands", "a");
            edit.e.connect(feathers, final_, "b");
        } else {
            edit.e.connect(feathers, out, "strands");
        }
        first = names.front();
        g = edit.now();
    });
    if (error.empty() && made) *made = first;
    return error;
}

std::string GroomDocument::addBraid(const std::string& style, const std::string& group, std::string* made) {
    static const char* const kStyles[] = {"three", "fishtail", "rope", "cornrow"};
    if (std::none_of(std::begin(kStyles), std::end(kStyles), [&](const char* s) { return style == s; })) {
        return "the braid's style is three, fishtail, rope or cornrow, not '" + style + "'";
    }
    // Checked first: the group is not made for a graph that cannot take a braid.
    std::string after;
    {
        if (typeOf(graph_, "interpolate") == "Interpolate") after = "interpolate";
        for (const std::string& n : ofType(graph_, "Interpolate")) {
            if (after.empty() && !fedBy(graph_, n).empty()) after = n;
        }
        if (after.empty()) return "the groom's graph has no Interpolate node to braid after";
    }
    if (const std::string error = ensureGuideGroup(group); !error.empty()) return error;
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add braid", [&](json& g) {
        GraphEdits edit(g, scalp ? presetScalpSize(*scalp) : std::nullopt);
        excludeFromHairGuides(edit, group);
        std::string paths;
        for (const std::string& n : ofType(edit.now(), "Guides")) {
            const json own = paramOf(edit.now(), n, "group");
            if (own.is_string() && own.get<std::string>() == group) paths = n;
        }
        if (paths.empty()) paths = edit.add("Guides", "braidPaths", json{{"group", group}});
        name = edit.add("Braid", "braid", json{{"style", style}, {"feed", style == "cornrow"}});
        const auto downstream = fedBy(edit.now(), after);
        if (!downstream.empty()) edit.insertBetween(after, downstream.front().first, name, "strands");
        else edit.e.connect(after, name, "strands");
        edit.place(name, after, kDX / 2.0, kDY);
        edit.e.connect(paths, name, "braids");
        edit.place(paths, after, 0.0, 2 * kDY);
        g = edit.now();
    });
    if (error.empty() && made) *made = name;
    return error;
}

// ---- Import and Utilities ---------------------------------------------------------------------------

std::string GroomDocument::importAlembicCurves(const std::string& path, const std::string& object, int samples, std::size_t* curves) {
    if (samples < 0 || samples > 1024) return "samples is 0 (as read) to 1024";
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec)) return "no file at " + path;
    std::size_t count = 0;
    try {
        count = readAlembicCurvesAt(path, time_, object).strandCount();
    } catch (const std::exception& e) {
        return std::string("cannot read the Alembic curves: ") + e.what();
    }
    if (count == 0) return object.empty() ? "the file has no curves" : "the file has no curves under " + object;
    const std::string error = editGraph("import Alembic curves", [&](json& g) {
        json nodes = json::array();
        bool hasScalp = false;
        json drawn;
        for (const json& n : g.value("nodes", json::array())) {
            const std::string name = n.value("name", std::string());
            if (name == "guides") {
                if (n.contains("ui")) drawn = n["ui"];  // where the Guides node was drawn
                continue;
            }
            hasScalp = hasScalp || name == "scalp";
            nodes.push_back(n);
        }
        json params = {{"path", path}, {"samples", samples}};
        if (!object.empty()) params["object"] = object;
        json made = {{"name", "guides"}, {"type", "ImportCurves"}, {"params", params}};
        if (!drawn.is_null()) made["ui"] = drawn;
        nodes.push_back(made);
        g["nodes"] = nodes;
        json connections = json::array();
        for (const json& c : g.value("connections", json::array())) {
            if (c.value("to", std::string()) != "guides") connections.push_back(c);
        }
        if (hasScalp) connections.push_back({{"from", "scalp"}, {"to", "guides"}, {"input", "scalp"}});
        g["connections"] = connections;
        GraphEdit check(g.dump());  // (the links checked: an Interpolate takes curves on its guides input)
        check.checkConnections();
    });
    if (error.empty() && curves) *curves = count;
    return error;
}

std::string GroomDocument::writeStrandMesh(const std::string& objPath, const StrandMeshOptions& options, std::size_t maxStrands,
                                           std::size_t* strandsWritten) {
    if (options.sides < 3) return "a tube has 3 sides or more";
    if (maxStrands < 1) return "at most 1 strand or more";
    try {
        MeshPtr scalp;
        const bool cards = options.make == StrandMeshOptions::Make::Auto || options.make == StrandMeshOptions::Make::Cards;
        const StrandsPtr all = exportFrame(time_, 1.0f, &scalp, cards);
        // Scales, feathers, cards and instances whole (their plates and rachis are the strand set's); tubes thinned.
        const bool whole = resolveMeshMake(*all, options) != StrandMeshOptions::Make::Strands;
        const std::vector<std::size_t> keep = thinStrandsById(*all, whole ? all->strandCount() : maxStrands);
        StrandSet kept;
        const StrandSet* strands = all.get();
        if (keep.size() < all->strandCount()) {
            for (const std::size_t i : keep) {
                const Vec3f* p = all->strandPoints(i);
                const std::size_t n = all->strandPointCount(i);
                std::vector<float> w;
                if (all->widths().size() == all->pointCount()) {
                    w.assign(all->widths().begin() + static_cast<std::ptrdiff_t>(all->strandBegin(i)),
                             all->widths().begin() + static_cast<std::ptrdiff_t>(all->strandBegin(i) + n));
                }
                kept.addStrand(all->ids()[i], std::vector<Vec3f>(p, p + n), w, i < all->roots().size() ? all->roots()[i] : StrandRoot{});
            }
            strands = &kept;
        }
        const GroomFile& groomFile = file();
        const PrototypeLookup prototypes = [&groomFile](const std::string& n) -> const InstancePrototype* {
            const auto it = groomFile.prototypes.find(n);
            return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
        };
        const StrandMesh mesh = buildStrandMesh(*strands, scalp.get(), options, prototypes);
        if (mesh.mesh.faceCount() == 0) return "nothing to make a mesh of (no faces)";
        const bool second = options.strandUVs && mesh.u2.size() == mesh.mesh.u.size();
        const std::vector<float>& us = second ? mesh.u2 : mesh.mesh.u;
        const std::vector<float>& vs = second ? mesh.v2 : mesh.mesh.v;
        std::ofstream out(fs::u8path(objPath), std::ios::binary);
        if (!out) return "cannot write " + objPath;
        out << "# GroomLab: " << strands->strandCount() << " strands as " << (options.style == StrandMeshOptions::Style::Tube ? "tubes" : "ribbons")
            << "\n";
        out << "o " << fs::u8path(objPath).stem().u8string() << "\n";
        char line[96];
        for (const Vec3f& p : mesh.mesh.points) {
            std::snprintf(line, sizeof(line), "v %.6g %.6g %.6g\n", p.x, p.y, p.z);
            out << line;
        }
        const bool uvs = !us.empty() && mesh.mesh.faceUVs.size() == mesh.mesh.faceVertices.size();
        if (uvs) {
            for (std::size_t i = 0; i < us.size(); ++i) {
                std::snprintf(line, sizeof(line), "vt %.6g %.6g\n", us[i], i < vs.size() ? vs[i] : 0.0f);
                out << line;
            }
        }
        std::size_t k = 0;
        for (const int count : mesh.mesh.faceCounts) {
            out << "f";
            for (int c = 0; c < count; ++c, ++k) {
                out << ' ' << mesh.mesh.faceVertices[k] + 1;
                if (uvs) out << '/' << mesh.mesh.faceUVs[k] + 1;
            }
            out << "\n";
        }
        if (!out) return "cannot write " + objPath;
        if (strandsWritten) *strandsWritten = strands->strandCount();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string GroomDocument::bakeScaleMaps(const std::string& directory, const std::string& name, int resolution,
                                         std::vector<std::string>* files) {
    if (resolution < 16 || resolution > 8192) return "the resolution is 16 to 8192";
    try {
        MeshPtr scalp;
        const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp);
        const GroomFile& groomFile = file();
        const PrototypeLookup prototypes = [&groomFile](const std::string& n) -> const InstancePrototype* {
            const auto it = groomFile.prototypes.find(n);
            return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
        };
        ScaleMeshBuilder builder;
        const auto mesh = builder.build(*strands, scalp, prototypes);
        if (!mesh || mesh->triangles.empty()) return "the groom has no scales (a Scales node)";
        if (!scalp || !scalp->hasUVs()) return "the scalp has no uvs to bake the scales onto";
        std::error_code ec;
        fs::create_directories(fs::u8path(directory), ec);
        ScaleMapOptions options;
        options.resolution = resolution;
        const ScaleMaps maps = groomlab::bakeScaleMaps(*scalp, *mesh, options);
        const std::vector<std::string> written = writeScaleMaps(maps, (fs::u8path(directory) / fs::u8path(name + "_")).generic_u8string());
        if (files) *files = written;
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::vector<GroomDocument::MissingFile> GroomDocument::missingFiles() const {
    std::vector<MissingFile> out;
    for (const PaintedMap& m : maps()) {
        if (m.texture.empty()) continue;
        if (!missingMapFile(m.name).empty()) out.push_back({"maps", m.name, m.texture});
    }
    json graph = graph_;
    fileParams(graph, "", [&](const std::string& label, json& value) {
        const std::string stored = trimmed(value.get<std::string>());
        if (stored.empty()) return;
        if (!fileThere(resolveMapPath(stored))) out.push_back({"params", label, stored});
    });
    return out;
}

std::string GroomDocument::findMissingFiles(const std::string& folder, bool recursive, RelinkReport* report) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(fs::u8path(folder), ec)) return "no folder " + folder;
    std::map<std::string, std::vector<std::string>> index;  // lower-cased file name -> paths
    const fs::path root = fs::absolute(fs::u8path(folder), ec);
    auto note = [&](const fs::directory_entry& e) {
        if (e.is_regular_file(ec)) index[lower(e.path().filename().u8string())].push_back(e.path().generic_u8string());
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) note(*it);
    } else {
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) note(*it);
    }
    RelinkReport r;
    const std::vector<MissingFile> missing = missingFiles();
    std::map<std::string, std::string> params;  // label -> file found
    for (const MissingFile& m : missing) {
        std::vector<std::string> candidates;
        const std::string found = matchFile(index, m.stored, &candidates);
        if (found.empty()) {
            if (candidates.empty()) r.stillMissing.push_back(m);
            else r.ambiguous.emplace_back(m, candidates);
            continue;
        }
        if (m.kind == "maps") {
            const std::string error = relinkMap(m.key, found);
            if (!error.empty()) {
                r.stillMissing.push_back(m);
                continue;
            }
        } else {
            params[m.key] = found;
        }
        r.relinked.emplace_back(m, found);
    }
    if (!params.empty()) {
        const std::string error = editGraph("relink missing files", [&](json& g) {
            fileParams(g, "", [&](const std::string& label, json& value) {
                const auto it = params.find(label);
                if (it != params.end()) value = it->second;
            });
        });
        if (!error.empty()) return error;
    }
    if (report) *report = std::move(r);
    return {};
}

void GroomDocument::recoveredAs(const std::string& path) {
    const bool moved = path != path_;
    path_ = path;
    dirty_ = true;
    if (moved && !path_.empty()) {
        // Relative paths (texture maps) resolve against the file's folder first, then the copy's
        // and its search paths (the autosave's: the file's folder among them).
        std::vector<std::string> dirs{fs::u8path(path_).parent_path().u8string()};
        if (!scene_->file().directory.empty()) dirs.push_back(scene_->file().directory);
        dirs.insert(dirs.end(), scene_->file().searchPaths.begin(), scene_->file().searchPaths.end());
        session().setPathResolver(searchPathResolver(std::move(dirs)));
        paints_.clear();
    }
}

}  // namespace groomlab::app
