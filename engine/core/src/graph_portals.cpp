// SPDX-License-Identifier: Apache-2.0
// A graph's portals: teleporting links (graph_portals.h).

#include "groomlab/graph_portals.h"

#include <map>
#include <set>
#include <utility>

namespace groomlab {

namespace {

using json = nlohmann::json;

std::string str(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

json& portalsOf(json& graph) {
    if (!graph.contains("portals") || !graph["portals"].is_array()) graph["portals"] = json::array();
    return graph["portals"];
}

const json& portalsView(const json& graph) {
    static const json none = json::array();
    if (!graph.is_object()) return none;
    const auto it = graph.find("portals");
    return it != graph.end() && it->is_array() ? *it : none;
}

json* findPortal(json& graph, const std::string& name) {
    for (json& p : portalsOf(graph)) {
        if (p.is_object() && str(p, "name") == name) return &p;
    }
    return nullptr;
}

json& exitsOf(json& portal) {
    if (!portal.contains("exits") || !portal["exits"].is_array()) portal["exits"] = json::array();
    return portal["exits"];
}

// What feeds to.input as the graph's connections say: (from, output), "" from for nothing.
std::pair<std::string, std::string> sourceOf(const json& graph, const std::string& to, const std::string& input) {
    for (const json& c : graph.value("connections", json::array())) {
        if (str(c, "to") == to && str(c, "input") == input) return {str(c, "from"), str(c, "output")};
    }
    return {};
}

std::string uniqueName(const json& graph, const std::string& base, const std::string& except = {}) {
    std::set<std::string> taken;
    for (const json& p : portalsView(graph)) {
        if (str(p, "name") != except) taken.insert(str(p, "name"));
    }
    const std::string b = base.empty() ? std::string("Portal") : base;
    if (!taken.count(b)) return b;
    int i = 1;
    while (taken.count(b + std::to_string(i))) ++i;
    return b + std::to_string(i);
}

// The link out of every exit (and entry) it is in.
bool dropLink(json& graph, const std::string& to, const std::string& input) {
    bool any = false;
    for (json& p : portalsOf(graph)) {
        if (p.contains("links") && p["links"].is_array()) {
            json kept = json::array();
            for (const json& l : p["links"]) {
                if (str(l, "to") == to && str(l, "input") == input) any = true;
                else kept.push_back(l);
            }
            p["links"] = std::move(kept);
        }
        for (json& e : exitsOf(p)) {
            if (!e.contains("links") || !e["links"].is_array()) continue;
            json kept = json::array();
            for (const json& l : e["links"]) {
                if (str(l, "to") == to && str(l, "input") == input) any = true;
                else kept.push_back(l);
            }
            e["links"] = std::move(kept);
        }
    }
    return any;
}

}  // namespace

void prunePortals(json& graph) {
    if (!graph.is_object() || !graph.contains("portals")) return;
    if (!graph["portals"].is_array()) {
        graph.erase("portals");
        return;
    }
    std::set<std::string> nodes;
    for (const json& n : graph.value("nodes", json::array())) nodes.insert(str(n, "name"));
    json kept = json::array();
    std::set<std::pair<std::string, std::string>> placed;  // links in a dot already
    for (json p : graph["portals"]) {
        if (!p.is_object()) continue;
        const std::string from = str(p, "from"), output = str(p, "output");
        if (from.empty()) {
            // A free dot: nothing comes out of it yet.
            p.erase("links");
            p.erase("output");
            p["exits"] = json::array();
            kept.push_back(std::move(p));
            continue;
        }
        if (!nodes.count(from)) continue;
        auto keep = [&](const json& list) {
            json links = json::array();
            for (const json& l : list.is_array() ? list : json::array()) {
                const std::pair<std::string, std::string> link{str(l, "to"), str(l, "input")};
                if (sourceOf(graph, link.first, link.second) != std::make_pair(from, output)) continue;
                if (!placed.insert(link).second) continue;
                links.push_back({{"to", link.first}, {"input", link.second}});
            }
            return links;
        };
        const json own = keep(p.value("links", json::array()));
        if (own.empty()) p.erase("links");
        else p["links"] = own;
        json exits = json::array();
        for (json e : p.value("exits", json::array())) {
            if (!e.is_object()) continue;
            e["links"] = keep(e.value("links", json::array()));
            exits.push_back(std::move(e));
        }
        p["exits"] = std::move(exits);
        if (output.empty()) p.erase("output");
        kept.push_back(std::move(p));
    }
    if (kept.empty()) graph.erase("portals");
    else graph["portals"] = std::move(kept);
}

std::vector<std::string> portalNames(const json& graph) {
    std::vector<std::string> out;
    for (const json& p : portalsView(graph)) out.push_back(str(p, "name"));
    return out;
}

std::string portalOf(const json& graph, const std::string& from, const std::string& output) {
    for (const json& p : portalsView(graph)) {
        if (str(p, "from") == from && str(p, "output") == output) return str(p, "name");
    }
    return {};
}

std::optional<PortalExitRef> portalOfLink(const json& graph, const std::string& to, const std::string& input) {
    for (const json& p : portalsView(graph)) {
        for (const json& l : p.value("links", json::array())) {
            if (str(l, "to") == to && str(l, "input") == input) return PortalExitRef{str(p, "name"), -1};
        }
        const json exits = p.value("exits", json::array());
        for (std::size_t e = 0; e < exits.size(); ++e) {
            for (const json& l : exits[e].value("links", json::array())) {
                if (str(l, "to") == to && str(l, "input") == input) return PortalExitRef{str(p, "name"), static_cast<int>(e)};
            }
        }
    }
    return std::nullopt;
}

std::string addPortal(json& graph, const std::string& from, const std::string& output, double x, double y, const std::string& name) {
    if (const std::string existing = portalOf(graph, from, output); !existing.empty()) return existing;
    const std::string made = uniqueName(graph, name.empty() ? from + (output.empty() ? std::string() : "_" + output) : name);
    json portal = {{"name", made}, {"from", from}, {"x", x}, {"y", y}, {"exits", json::array()}};
    if (!output.empty()) portal["output"] = output;
    portalsOf(graph).push_back(std::move(portal));
    return made;
}

int addPortalExit(json& graph, const std::string& portal, double x, double y) {
    json* p = findPortal(graph, portal);
    if (!p) return -1;
    exitsOf(*p).push_back({{"x", x}, {"y", y}, {"links", json::array()}});
    return static_cast<int>(exitsOf(*p).size()) - 1;
}

std::optional<PortalExitRef> teleportLink(json& graph, const std::string& to, const std::string& input, double exitX, double exitY,
                                          double entryX, double entryY) {
    const auto [from, output] = sourceOf(graph, to, input);
    if (from.empty()) return std::nullopt;
    const std::string portal = addPortal(graph, from, output, entryX, entryY);
    const int exit = addPortalExit(graph, portal, exitX, exitY);
    addPortalLink(graph, portal, exit, to, input);
    return PortalExitRef{portal, exit};
}

bool addPortalLink(json& graph, const std::string& portal, int exit, const std::string& to, const std::string& input) {
    json* p = findPortal(graph, portal);
    if (!p || exit < -1 || exit >= static_cast<int>(exitsOf(*p).size()) || str(*p, "from").empty()) return false;
    if (sourceOf(graph, to, input) != std::make_pair(str(*p, "from"), str(*p, "output"))) return false;
    dropLink(graph, to, input);
    p = findPortal(graph, portal);
    json& e = exit < 0 ? *p : exitsOf(*p)[exit];
    if (!e.contains("links") || !e["links"].is_array()) e["links"] = json::array();
    e["links"].push_back({{"to", to}, {"input", input}});
    return true;
}

bool removePortalLink(json& graph, const std::string& to, const std::string& input) {
    if (!graph.contains("portals")) return false;
    return dropLink(graph, to, input);
}

bool movePortal(json& graph, const std::string& portal, double x, double y) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p) return false;
    (*p)["x"] = x;
    (*p)["y"] = y;
    return true;
}

bool movePortalExit(json& graph, const std::string& portal, int exit, double x, double y) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p || exit < 0 || exit >= static_cast<int>(exitsOf(*p).size())) return false;
    exitsOf(*p)[exit]["x"] = x;
    exitsOf(*p)[exit]["y"] = y;
    return true;
}

bool removePortal(json& graph, const std::string& portal) {
    if (!graph.contains("portals") || !graph["portals"].is_array()) return false;
    json kept = json::array();
    bool any = false;
    for (const json& p : graph["portals"]) {
        if (str(p, "name") == portal) any = true;
        else kept.push_back(p);
    }
    if (kept.empty()) graph.erase("portals");
    else graph["portals"] = std::move(kept);
    return any;
}

bool removePortalExit(json& graph, const std::string& portal, int exit) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p || exit < 0 || exit >= static_cast<int>(exitsOf(*p).size())) return false;
    exitsOf(*p).erase(exitsOf(*p).begin() + exit);
    return true;
}

std::string renamePortal(json& graph, const std::string& portal, const std::string& name) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p) return {};
    const std::string made = uniqueName(graph, name, portal);
    (*p)["name"] = made;
    return made;
}

void renamePortalNode(json& graph, const std::string& from, const std::string& to) {
    if (!graph.contains("portals") || !graph["portals"].is_array()) return;
    for (json& p : graph["portals"]) {
        if (!p.is_object()) continue;
        if (str(p, "from") == from) p["from"] = to;
        if (p.contains("links") && p["links"].is_array()) {
            for (json& l : p["links"]) {
                if (str(l, "to") == from) l["to"] = to;
            }
        }
        for (json& e : exitsOf(p)) {
            if (!e.contains("links") || !e["links"].is_array()) continue;
            for (json& l : e["links"]) {
                if (str(l, "to") == from) l["to"] = to;
            }
        }
    }
}

std::string addDot(json& graph, double x, double y, const std::string& name) {
    const std::string made = uniqueName(graph, name.empty() ? std::string("Dot") : name);
    portalsOf(graph).push_back({{"name", made}, {"from", ""}, {"x", x}, {"y", y}, {"exits", json::array()}});
    return made;
}

bool portalFed(const json& graph, const std::string& portal) {
    for (const json& p : portalsView(graph)) {
        if (str(p, "name") == portal) return !str(p, "from").empty();
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> portalLinks(const json& graph, const std::string& portal) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& p : portalsView(graph)) {
        if (str(p, "name") != portal) continue;
        for (const json& l : p.value("links", json::array())) out.emplace_back(str(l, "to"), str(l, "input"));
        for (const json& e : p.value("exits", json::array())) {
            for (const json& l : e.value("links", json::array())) out.emplace_back(str(l, "to"), str(l, "input"));
        }
    }
    return out;
}

bool setPortalSource(json& graph, const std::string& portal, const std::string& from, const std::string& output) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p || from.empty()) return false;
    const std::string other = portalOf(graph, from, output);
    if (!other.empty() && other != portal) return false;
    (*p)["from"] = from;
    if (output.empty()) p->erase("output");
    else (*p)["output"] = output;
    return true;
}

int joinPortal(json& graph, const std::string& dot, const std::string& into) {
    if (dot == into || !graph.contains("portals")) return -1;
    json* d = findPortal(graph, dot);
    json* target = findPortal(graph, into);
    if (!d || !target || str(*target, "from").empty()) return -1;
    json exit = {{"x", d->value("x", 0.0)}, {"y", d->value("y", 0.0)}, {"links", json::array()}};
    for (const auto& [to, input] : portalLinks(graph, dot)) exit["links"].push_back({{"to", to}, {"input", input}});
    exitsOf(*target).push_back(std::move(exit));
    const int index = static_cast<int>(exitsOf(*target).size()) - 1;
    removePortal(graph, dot);
    return index;
}

std::string leavePortal(json& graph, const std::string& portal, int exit) {
    json* p = graph.contains("portals") ? findPortal(graph, portal) : nullptr;
    if (!p || exit < 0 || exit >= static_cast<int>(exitsOf(*p).size())) return {};
    const json e = exitsOf(*p)[exit];
    exitsOf(*p).erase(exitsOf(*p).begin() + exit);
    return addDot(graph, e.value("x", 0.0), e.value("y", 0.0), portal);
}

}  // namespace groomlab
