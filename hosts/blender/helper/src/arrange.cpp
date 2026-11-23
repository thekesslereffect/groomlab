// SPDX-License-Identifier: Apache-2.0
// The Graph Editor arranged by hand (arrange.h): the core's graph_arrange.h, graph_portals.h and
// GraphEdit's pass-through outputs, as Maya's GraphModel and the app's GroomDocument call them.

#include "arrange.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/graph_arrange.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"
#include "groomlab/groom_session.h"

#include "authoring.h"
#include "request.h"
#include "session.h"

namespace groomlab::helper {

namespace {

std::string graphTextOf(const Session& s) { return graphOrDefault(s.groom->data().graphJson); }

std::vector<std::string> stringsOf(const json& args, const char* key) {
    const json& v = need(args, key);
    if (!v.is_array()) fail("bad_request", std::string(key) + " is a list of names");
    std::vector<std::string> out;
    for (const json& n : v) {
        if (!n.is_string()) fail("bad_request", std::string(key) + " is a list of names");
        out.push_back(n.get<std::string>());
    }
    return out;
}

double numberOf(const json& args, const char* key) {
    const json& v = need(args, key);
    if (!v.is_number() || !std::isfinite(v.get<double>())) fail("bad_request", std::string(key) + " is a number");
    return v.get<double>();
}

// [{name, x, y, w, h}]: the cards as the editor draws them (graph units, y down, top-left).
std::vector<LayoutBox> boxesOf(const json& args, const char* key = "boxes") {
    const json& v = need(args, key);
    if (!v.is_array()) fail("bad_request", std::string(key) + " is [{name, x, y, w, h}]");
    std::vector<LayoutBox> out;
    for (const json& b : v) {
        if (!b.is_object()) fail("bad_request", std::string(key) + " is [{name, x, y, w, h}]");
        LayoutBox box;
        box.name = b.value("name", std::string());
        box.x = numberOf(b, "x");
        box.y = numberOf(b, "y");
        box.w = numberOf(b, "w");
        box.h = numberOf(b, "h");
        out.push_back(box);
    }
    return out;
}

json positionsJson(const std::map<std::string, std::pair<double, double>>& placed) {
    json out = json::object();
    for (const auto& [name, at] : placed) out[name] = {at.first, at.second};
    return out;
}

json carriedJson(const CarriedLayout& c) {
    json dots = json::array();
    for (const auto& [portal, exit] : c.dots) dots.push_back({{"portal", portal}, {"exit", exit}});
    return {{"pins", c.pins}, {"dots", dots}};
}

// Each node's "ui" moved to where `placed` puts it (the rest as they were).
void placeNodes(json& graph, const std::map<std::string, std::pair<double, double>>& placed) {
    if (!graph.contains("nodes") || !graph["nodes"].is_array()) return;
    for (json& n : graph["nodes"]) {
        if (!n.is_object()) continue;
        const auto it = placed.find(n.value("name", std::string()));
        if (it == placed.end()) continue;
        json ui = n.contains("ui") && n["ui"].is_object() ? n["ui"] : json::object();
        ui["x"] = it->second.first;
        ui["y"] = it->second.second;
        n["ui"] = ui;
    }
}

// The graph's portal named so (a copy: the graph changes after), or fail.
json portalNamed(const json& graph, const std::string& name) {
    const auto it = graph.find("portals");
    if (it != graph.end() && it->is_array()) {
        for (const json& p : *it) {
            if (p.is_object() && p.value("name", std::string()) == name) return p;
        }
    }
    fail("not_found", "no portal '" + name + "'");
}

// The links (to, input) read from's `output` (connect's checks: GraphEdit::connectByHand), the
// portals kept as they were (a host connects first, then names the dot's source: the app's feedDot).
json relinked(const json& graph, const std::vector<std::pair<std::string, std::string>>& links, const std::string& from,
              const std::string& output) {
    GraphEdit edit(graph.dump());
    for (const auto& [to, input] : links) edit.connectByHand(from, to, input, output);
    json out = json::parse(edit.toJson());
    if (graph.contains("portals")) out["portals"] = graph["portals"];
    else out.erase("portals");
    return out;
}

int exitOf(const json& args) {
    const json& v = args.contains("exit") ? args["exit"] : json();
    if (v.is_null()) return -1;
    if (!v.is_number_integer()) fail("bad_request", "exit is an exit's index (none: the entry)");
    return v.get<int>();
}

}  // namespace

bool runGraphArrange(Session& s, const std::string& op, const json& args, json& result) {
    static const std::set<std::string> kOps = {
        "tidy",          "align",        "place",           "teleport_link",  "add_portal_exit",   "add_portal_link",
        "move_portal",   "remove_portal", "rename_portal",  "add_dot",        "feed_dot",          "set_dot_input",
        "detach_group",  "duplicate_nodes", "insert_nodes_on_link", "move_output_links", "set_pass_through",
        "set_pass_through_name", "move_pass_through", "clear_backdrop_title"};
    if (!kOps.count(op)) return false;
    const std::string before = graphTextOf(s);
    json g = json::parse(before);
    if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
    json r = json::object();
    std::string label = op;
    std::map<std::string, std::string> copied;  // a duplicate's {copy: source}: its Sculpt edits copied
    std::vector<std::pair<std::string, std::string>> moved;
    if (op == "tidy") {
        // Tidy Layout (every host's L): the selected nodes (fewer than two: all) laid out by their
        // links, backdrops as groups, pins and dots carried (graph_arrange.h tidyGraph).
        std::vector<std::string> selected = args.contains("selected") ? stringsOf(args, "selected") : std::vector<std::string>{};
        if (selected.size() < 2) selected.clear();
        g = tidyGraph(g, boxesOf(args), selected, optNumber(args, "gapX", 54.0), optNumber(args, "gapY", 40.0),
                      optNumber(args, "margin", 24.0), optNumber(args, "header", 28.0));
        label = "tidy layout";
    } else if (op == "align") {
        // Align (node menu > Align): the selected cards lined up or spaced (graph_arrange.h alignBoxes).
        const std::string mode = needString(args, "mode");
        const std::vector<LayoutBox> boxes = boxesOf(args);
        std::map<std::string, std::pair<double, double>> placed;
        if (mode == "tidy") {
            const GraphEdit edit(before);
            placed = arrangeSelection(edit, boxes, optNumber(args, "column", 230.0), optNumber(args, "row", 140.0));
        } else {
            placed = alignBoxes(boxes, mode, optNumber(args, "gap", 40.0));
        }
        placeNodes(g, placed);
        r["positions"] = positionsJson(placed);
        label = "align " + mode;
    } else if (op == "place") {
        // Nodes, pins and dots moved together (a drag carrying them, an align gesture): one layout step.
        if (args.contains("positions") && args["positions"].is_object()) {
            std::map<std::string, std::pair<double, double>> placed;
            for (const auto& [name, at] : args["positions"].items()) {
                if (!at.is_array() || at.size() != 2 || !at[0].is_number() || !at[1].is_number()) {
                    fail("bad_request", "positions is {node: [x, y]}");
                }
                placed[name] = {at[0].get<double>(), at[1].get<double>()};
            }
            placeNodes(g, placed);
        }
        if (args.contains("dots") && args["dots"].is_array()) {
            for (const json& d : args["dots"]) {
                const std::string portal = needString(d, "portal");
                const int exit = exitOf(d);
                if (exit < 0) movePortal(g, portal, numberOf(d, "x"), numberOf(d, "y"));
                else movePortalExit(g, portal, exit, numberOf(d, "x"), numberOf(d, "y"));
            }
        }
        label = "move nodes";
    } else if (op == "teleport_link") {
        const auto made = teleportLink(g, needString(args, "to"), needString(args, "input"), numberOf(args, "exitX"),
                                       numberOf(args, "exitY"), numberOf(args, "entryX"), numberOf(args, "entryY"));
        if (!made) fail("bad_request", "nothing feeds " + needString(args, "to") + "." + needString(args, "input"));
        r["portal"] = made->portal;
        r["exit"] = made->exit;
        label = "teleport link";
    } else if (op == "add_portal_exit") {
        const int exit = addPortalExit(g, needString(args, "portal"), numberOf(args, "x"), numberOf(args, "y"));
        if (exit < 0) fail("not_found", "no portal '" + needString(args, "portal") + "'");
        r["exit"] = exit;
        label = "add portal exit";
    } else if (op == "add_portal_link") {
        // A link dragged out of a dot's nub: connected from the portal's source, drawn out of that dot.
        const std::string portal = needString(args, "portal"), to = needString(args, "to"), input = needString(args, "input");
        const json p = portalNamed(g, portal);
        const std::string from = p.value("from", std::string());
        if (from.empty()) fail("bad_request", portal + " has nothing coming into it yet");
        g = relinked(g, {{to, input}}, from, p.value("output", std::string()));
        const int exit = exitOf(args);
        if (exit >= 0) {
            if (!addPortalLink(g, portal, exit, to, input)) fail("bad_request", "cannot link " + to + "." + input + " out of " + portal);
        } else {
            // Out of the entry dot itself (its "links").
            for (json& q : g["portals"]) {
                if (q.value("name", std::string()) != portal) continue;
                json links = q.value("links", json::array());
                links.push_back({{"to", to}, {"input", input}});
                q["links"] = links;
            }
        }
        label = "link from portal";
    } else if (op == "move_portal") {
        const std::string portal = needString(args, "portal");
        const int exit = exitOf(args);
        const bool ok = exit < 0 ? movePortal(g, portal, numberOf(args, "x"), numberOf(args, "y"))
                                 : movePortalExit(g, portal, exit, numberOf(args, "x"), numberOf(args, "y"));
        if (!ok) fail("not_found", "no such dot of '" + portal + "'");
        label = "move dot";
    } else if (op == "remove_portal") {
        const std::string portal = needString(args, "portal");
        const int exit = exitOf(args);
        r["removed"] = exit < 0 ? removePortal(g, portal) : removePortalExit(g, portal, exit);
        label = exit < 0 ? "remove portal" : "remove portal exit";
    } else if (op == "rename_portal") {
        const std::string made = renamePortal(g, needString(args, "portal"), needString(args, "newName"));
        if (made.empty()) fail("not_found", "no portal '" + needString(args, "portal") + "'");
        r["name"] = made;
        label = "rename portal";
    } else if (op == "add_dot") {
        // The Tab search's Dot (fed from an output when a link was dropped there: an exit of that
        // output's dot when it has one), the app's addDot.
        std::string name = addDot(g, numberOf(args, "x"), numberOf(args, "y"));
        const std::string from = optString(args, "from", ""), output = optString(args, "output", "");
        if (!from.empty() && !setPortalSource(g, name, from, output)) {
            const std::string portal = portalOf(g, from, output);
            if (joinPortal(g, name, portal) < 0) fail("bad_request", from + "'s output has a dot already");
            name = portal;
        }
        r["name"] = name;
        label = "add dot";
    } else if (op == "feed_dot") {
        const std::string portal = needString(args, "portal"), from = needString(args, "from"), output = optString(args, "output", "");
        portalNamed(g, portal);
        g = relinked(g, portalLinks(g, portal), from, output);
        if (!setPortalSource(g, portal, from, output)) fail("bad_request", from + "'s output has a dot already");
        label = "feed dot";
    } else if (op == "set_dot_input") {
        // A dot's Input Portal: another dot's signal (its links read that dot's source), or none.
        const std::string portal = needString(args, "portal"), into = optString(args, "into", "");
        std::string dot = portal;
        const int exit = exitOf(args);
        if (exit >= 0) {
            dot = leavePortal(g, portal, exit);
            if (dot.empty()) fail("not_found", "no such dot");
        }
        if (!into.empty()) {
            const json p = portalNamed(g, into);
            const std::string from = p.value("from", std::string());
            if (from.empty()) fail("bad_request", into + " has nothing coming into it yet");
            g = relinked(g, portalLinks(g, dot), from, p.value("output", std::string()));
            const int index = joinPortal(g, dot, into);
            if (index < 0) fail("bad_request", "cannot take " + into + "'s signal");
            r["portal"] = into;
            r["exit"] = index;
        } else {
            r["portal"] = dot;
            r["exit"] = json();
        }
        label = "dot input";
    } else {
        // The edits of the graph's links and nodes (GraphEdit).
        GraphEdit edit(g.dump());
        if (op == "detach_group") {
            // A shake of several nodes: out of the flow together, their own links kept.
            const std::vector<std::string> names = stringsOf(args, "names");
            r["detached"] = names.size() == 1 ? edit.detach(names.front()).changed : edit.detachGroup(names).changed;
            label = "shake loose";
        } else if (op == "duplicate_nodes") {
            // Ctrl+D (every host's): copies beside the nodes reading what they read.
            json made = json::array();
            for (const auto& [copy, source] : edit.duplicateNodes(stringsOf(args, "names"), optNumber(args, "dx", 40.0),
                                                                    optNumber(args, "dy", 60.0))) {
                made.push_back({{"name", copy}, {"source", source}});
                copied[copy] = source;
            }
            r["made"] = made;
            label = "duplicate";
        } else if (op == "insert_nodes_on_link") {
            const std::vector<std::string> nodes = stringsOf(args, "nodes");
            const std::string from = needString(args, "from"), to = needString(args, "to"), input = needString(args, "input");
            if (nodes.size() == 1) insertOnLink(edit, from, to, input, nodes.front(), optString(args, "output", ""));
            else insertNodesOnLink(edit, from, to, input, nodes, optString(args, "output", ""));
            label = "insert nodes";
        } else if (op == "move_output_links") {
            json links = json::array();
            for (const auto& [node, input] : edit.moveOutputLinks(needString(args, "from"), optString(args, "output", ""),
                                                                  needString(args, "to"), optString(args, "toOutput", ""))) {
                links.push_back({{"node", node}, {"input", input}});
            }
            r["moved"] = links;
            label = "move links";
        } else if (op == "set_pass_through") {
            r["changed"] = edit.setPassThrough(needString(args, "name"), needString(args, "input"), optBool(args, "on", true));
            label = "pass through";
        } else if (op == "set_pass_through_name") {
            r["changed"] = edit.setPassThroughName(needString(args, "name"), needString(args, "input"), optString(args, "label", ""));
            label = "rename output";
        } else if (op == "move_pass_through") {
            r["changed"] = edit.movePassThrough(needString(args, "name"), needString(args, "input"),
                                                static_cast<int>(optNumber(args, "by", 1.0)));
            label = "move output";
        } else if (op == "clear_backdrop_title") {
            // A backdrop's title made bigger (its size, a description): its nodes moved clear of it.
            g = clearBackdropTitle(json::parse(edit.toJson()), static_cast<int>(numberOf(args, "index")), boxesOf(args),
                                   optNumber(args, "margin", 16.0));
            edit = GraphEdit(g.dump());
            label = "backdrop title";
        }
        g = json::parse(edit.toJson());
        moved = edit.movedNodes();
    }
    prunePins(g);
    prunePortals(g);
    result = commitGraphText(s, label, before, GraphEdit(g.dump()).toJson(), moved, r, copied);
    return true;
}

bool isArrangeCommand(const std::string& cmd) {
    static const std::set<std::string> kCommands = {"graph_carry", "align_gesture", "wire_route", "shake_test", "backdrop_layout"};
    return kCommands.count(cmd) > 0;
}

json runArrangeCommand(const std::string& cmd, const json& args, const std::function<Session&()>& session) {
    if (cmd == "graph_carry") {
        // What a drag of `names` carries: "up" (Shift: what feeds them), "down" (Ctrl: what they
        // feed) or "" (just them), and the pins and dots that go with those nodes.
        Session& s = session();
        const std::string text = graphTextOf(s);
        const GraphEdit edit(text);
        const std::vector<std::string> names = stringsOf(args, "names");
        const std::string mode = optString(args, "mode", "");
        std::vector<std::string> nodes = names;
        if (mode == "up") nodes = nodesWithUpstream(edit, names);
        else if (mode == "down") nodes = nodesWithDownstream(edit, names);
        json out = carriedJson(carriedWith(json::parse(text), nodes));
        out["nodes"] = nodes;
        return out;
    }
    if (cmd == "backdrop_layout") {
        // A backdrop's area in graph units: the pins and dots lying on it (moved with it, hidden folded),
        // its title bar's height and how far down its nodes start, and how it folds.
        Session& s = session();
        const json backdrop = args.contains("backdrop") && args["backdrop"].is_object() ? args["backdrop"] : json::object();
        const LayoutBox box{"", numberOf(args, "x"), numberOf(args, "y"), numberOf(args, "w"), numberOf(args, "h")};
        json out = carriedJson(layoutOnBackdrop(json::parse(graphTextOf(s)), box));
        const double header = backdropHeader(backdrop);
        out["header"] = header;
        out["contentTop"] = backdropContentTop(backdrop);
        out["titleWidth"] = backdropTitleWidth(backdrop);
        const LayoutBox folded = foldedBackdrop(box, header, std::max(160.0, backdropTitleWidth(backdrop)));
        out["folded"] = {folded.x, folded.y, folded.w, folded.h};
        if (args.contains("nodes")) {
            const LayoutBox grown = grownBackdrop(box, boxesOf(args, "nodes"), optNumber(args, "margin", 24.0), header);
            out["grown"] = {grown.x, grown.y, grown.w, grown.h};
        }
        return out;
    }
    if (cmd == "align_gesture") {
        Session& s = session();
        const GraphEdit edit(graphTextOf(s));
        const double dx = numberOf(args, "dx"), dy = numberOf(args, "dy");
        const auto placed = alignGesture(edit, boxesOf(args), needString(args, "anchor"),
                                         args.contains("selected") ? stringsOf(args, "selected") : std::vector<std::string>{},
                                         needString(args, "mode"), dx, dy, optNumber(args, "gap", 40.0));
        return {{"axis", alignGestureAxis(dx, dy)}, {"positions", positionsJson(placed)}};
    }
    if (cmd == "wire_route") {
        // The core's wire (graph_arrange.h wireRoute): the add-on draws its own port of it, checked
        // against this.
        const double below = args.contains("below") && args["below"].is_number() ? args["below"].get<double>() : std::nan("");
        json steps = json::array();
        for (const WireStep& w : wireRoute(optString(args, "style", "smooth"), numberOf(args, "ax"), numberOf(args, "ay"),
                                           numberOf(args, "bx"), numberOf(args, "by"), below, optBool(args, "through", false))) {
            steps.push_back(w.cubic ? json{w.c1x, w.c1y, w.c2x, w.c2y, w.x, w.y} : json{w.x, w.y});
        }
        return {{"steps", steps}};
    }
    // shake_test: the core's ShakeDetector over [[seconds, x, y]...] (the add-on's port checked
    // against it): the indices where a shake completed.
    ShakeDetector shake;
    json at = json::array();
    const json& moves = need(args, "moves");
    if (!moves.is_array()) fail("bad_request", "moves is [[seconds, x, y]]");
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const json& m = moves[i];
        if (!m.is_array() || m.size() < 3) fail("bad_request", "moves is [[seconds, x, y]]");
        if (shake.add(m[0].get<double>(), m[1].get<double>(), m[2].get<double>())) at.push_back(i);
    }
    return {{"shakes", at}};
}

}  // namespace groomlab::helper
