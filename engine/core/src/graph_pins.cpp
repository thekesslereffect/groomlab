// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph_pins.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace groomlab {

using json = nlohmann::json;

namespace {

struct Route {
    PinSource source;
    std::vector<std::string> via;
};
using Key = std::pair<std::string, std::string>;  // (to, input)

// The graph's pins as maps: points by id, routes by (to, input).
struct Pins {
    std::map<std::string, std::pair<double, double>> points;
    std::map<Key, Route> routes;
};

std::string text(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

Pins read(const json& graph) {
    Pins p;
    if (!graph.contains("pins") || !graph["pins"].is_object()) return p;
    const json& pins = graph["pins"];
    for (const json& point : pins.value("points", json::array())) {
        if (!point.is_object() || text(point, "id").empty()) continue;
        p.points[text(point, "id")] = {point.value("x", 0.0), point.value("y", 0.0)};
    }
    for (const json& r : pins.value("routes", json::array())) {
        if (!r.is_object() || text(r, "to").empty() || text(r, "input").empty() || !r.contains("via") || !r["via"].is_array()) continue;
        Route route{{text(r, "from"), text(r, "output")}, {}};
        for (const json& v : r["via"]) {
            if (v.is_string()) route.via.push_back(v.get<std::string>());
        }
        if (!route.via.empty()) p.routes[{text(r, "to"), text(r, "input")}] = std::move(route);
    }
    return p;
}

// pin12 -> 12 (0 for another name): pins sort and number by it.
long number(const std::string& pin) {
    if (pin.size() <= 3 || pin.compare(0, 3, "pin") != 0) return 0;
    for (std::size_t i = 3; i < pin.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(pin[i]))) return 0;
    }
    return std::stol(pin.substr(3));
}

// What feeds to.input now; none for nothing.
std::optional<PinSource> sourceOf(const json& graph, const std::string& to, const std::string& input) {
    for (const json& c : graph.value("connections", json::array())) {
        if (c.is_object() && text(c, "to") == to && text(c, "input") == input) return PinSource{text(c, "from"), text(c, "output")};
    }
    return std::nullopt;
}

// Drops what no longer holds, then writes "pins" back in order (none when empty).
void write(json& graph, Pins p) {
    std::map<Key, Route> routes;
    for (auto& [key, route] : p.routes) {
        std::vector<std::string> via;
        for (const std::string& v : route.via) {
            if (p.points.count(v)) via.push_back(v);
        }
        const std::optional<PinSource> source = sourceOf(graph, key.first, key.second);
        if (!via.empty() && source && *source == route.source) routes[key] = {route.source, std::move(via)};
    }
    std::set<std::string> used;
    for (const auto& [key, route] : routes) used.insert(route.via.begin(), route.via.end());
    if (routes.empty()) {
        graph.erase("pins");
        return;
    }
    std::vector<std::string> ids;
    for (const auto& [id, at] : p.points) {
        if (used.count(id)) ids.push_back(id);
    }
    std::stable_sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return number(a) < number(b); });
    json points = json::array();
    for (const std::string& id : ids) points.push_back({{"id", id}, {"x", p.points[id].first}, {"y", p.points[id].second}});
    json list = json::array();
    for (const auto& [key, route] : routes) {
        json r = {{"from", route.source.from}, {"to", key.first}, {"input", key.second}, {"via", route.via}};
        if (!route.source.output.empty()) r["output"] = route.source.output;
        list.push_back(std::move(r));
    }
    graph["pins"] = {{"points", std::move(points)}, {"routes", std::move(list)}};
}

// The route into to.input as it holds now (its source the link's, its pins there).
std::vector<std::string> liveRoute(const json& graph, const Pins& p, const std::string& to, const std::string& input) {
    const auto it = p.routes.find({to, input});
    if (it == p.routes.end()) return {};
    const std::optional<PinSource> source = sourceOf(graph, to, input);
    if (!source || !(*source == it->second.source)) return {};
    std::vector<std::string> via;
    for (const std::string& v : it->second.via) {
        if (p.points.count(v)) via.push_back(v);
    }
    return via;
}

}  // namespace

void prunePins(json& graph) { write(graph, read(graph)); }

std::vector<std::string> pinIds(const json& graph) {
    std::vector<std::string> ids;
    for (const auto& [id, at] : read(graph).points) ids.push_back(id);
    std::stable_sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return number(a) < number(b); });
    return ids;
}

bool pinPosition(const json& graph, const std::string& pin, double* x, double* y) {
    const Pins p = read(graph);
    const auto it = p.points.find(pin);
    if (it == p.points.end()) return false;
    *x = it->second.first;
    *y = it->second.second;
    return true;
}

std::vector<std::string> pinRoute(const json& graph, const std::string& to, const std::string& input) {
    return liveRoute(graph, read(graph), to, input);
}

std::vector<PinLink> pinLinks(const json& graph, const std::string& pin) {
    const Pins p = read(graph);
    std::vector<PinLink> links;
    for (const auto& [key, route] : p.routes) {
        const std::vector<std::string> via = liveRoute(graph, p, key.first, key.second);
        const auto at = std::find(via.begin(), via.end(), pin);
        if (at != via.end()) links.push_back({key.first, key.second, static_cast<std::size_t>(at - via.begin())});
    }
    return links;
}

std::optional<PinSource> pinSource(const json& graph, const std::string& pin) {
    const std::vector<PinLink> links = pinLinks(graph, pin);
    if (links.empty()) return std::nullopt;
    return sourceOf(graph, links.front().to, links.front().input);
}

std::vector<std::string> pinRouteTo(const json& graph, const std::string& pin) {
    const std::vector<PinLink> links = pinLinks(graph, pin);
    if (links.empty()) return {};
    std::vector<std::string> via = pinRoute(graph, links.front().to, links.front().input);
    via.resize(links.front().index + 1);
    return via;
}

std::string addPin(json& graph, const std::string& to, const std::string& input, double x, double y, int index) {
    const std::optional<PinSource> source = sourceOf(graph, to, input);
    if (!source) return {};
    Pins p = read(graph);
    long next = 0;
    for (const auto& [id, at] : p.points) next = std::max(next, number(id));
    const std::string pin = "pin" + std::to_string(next + 1);
    p.points[pin] = {x, y};
    std::vector<std::string> via = liveRoute(graph, p, to, input);
    const std::size_t at = index < 0 ? via.size() : std::min<std::size_t>(static_cast<std::size_t>(index), via.size());
    via.insert(via.begin() + static_cast<std::ptrdiff_t>(at), pin);
    p.routes[{to, input}] = {*source, std::move(via)};
    write(graph, std::move(p));
    return pin;
}

bool movePin(json& graph, const std::string& pin, double x, double y) {
    Pins p = read(graph);
    const auto it = p.points.find(pin);
    if (it == p.points.end()) return false;
    it->second = {x, y};
    write(graph, std::move(p));
    return true;
}

bool removePin(json& graph, const std::string& pin) {
    Pins p = read(graph);
    if (!p.points.erase(pin)) return false;
    for (auto& [key, route] : p.routes) route.via.erase(std::remove(route.via.begin(), route.via.end(), pin), route.via.end());
    write(graph, std::move(p));
    return true;
}

bool mergePins(json& graph, const std::string& keep, const std::string& drop) {
    if (keep == drop) return false;
    Pins p = read(graph);
    if (!p.points.count(keep) || !p.points.count(drop)) return false;
    const std::optional<PinSource> a = pinSource(graph, keep), b = pinSource(graph, drop);
    if (!a || !b || !(*a == *b)) return false;
    for (auto& [key, route] : p.routes) {
        std::vector<std::string> via;
        for (std::string v : route.via) {
            if (v == drop) v = keep;
            if (via.empty() || via.back() != v) via.push_back(v);
        }
        route.via = std::move(via);
    }
    p.points.erase(drop);
    write(graph, std::move(p));
    return true;
}

bool setPinRoute(json& graph, const std::string& to, const std::string& input, const std::vector<std::string>& via) {
    const std::optional<PinSource> source = sourceOf(graph, to, input);
    if (!source) return false;
    Pins p = read(graph);
    if (via.empty()) p.routes.erase({to, input});
    else p.routes[{to, input}] = {*source, via};
    write(graph, std::move(p));
    return true;
}

void renamePinNode(json& graph, const std::string& from, const std::string& to) {
    Pins p = read(graph);
    if (p.routes.empty()) return;
    std::map<Key, Route> routes;
    for (auto& [key, route] : p.routes) {
        if (route.source.from == from) route.source.from = to;
        routes[{key.first == from ? to : key.first, key.second}] = std::move(route);
    }
    p.routes = std::move(routes);
    write(graph, std::move(p));
}

}  // namespace groomlab
