// SPDX-License-Identifier: Apache-2.0
#pragma once

// A graph's portals: teleporting links, so a signal used far away is not drawn as a long wire
// across the graph. A portal is a named dot by an output (its entry); each of its exits is a dot
// somewhere else, from which links run on into inputs. The links are the graph's connections as
// ever (from the portal's source output to those inputs): a portal only says how they are drawn,
// so it is layout, as the pins (graph_pins.h), the nodes' "ui" and the backdrops are; nothing
// evaluates it. Every host's: the app calls these, Maya's GraphModel through
// groomlab_core.GraphPortals. In the graph's JSON:
//
//   "portals": [{"name": "Guides", "from": "guides", "output"?: "", "x": .., "y": ..,
//                "exits": [{"x": .., "y": .., "links": [{"to": "scatter", "input": "guides"}]}]}]
//
// ("output" only when it is not the source's first, as a connection's.) A link belongs to one
// exit at most. The functions keep "portals" pruned: a link no longer fed from the portal's
// source leaves its exit; a portal whose source node is gone goes; no "portals" when none is left.
// An exit with no links stays (placed from the add-node search, before anything is linked to it).

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace groomlab {

// A link drawn through a portal: the portal's name, and the exit it comes out of.
struct PortalExitRef {
    std::string portal;
    int exit = -1;
};

// Drops what no longer holds (above) and the "portals" key when empty.
void prunePortals(nlohmann::json& graph);
// The portals' names, in order.
std::vector<std::string> portalNames(const nlohmann::json& graph);
// The portal of a source output ("" none; output "": the source's first).
std::string portalOf(const nlohmann::json& graph, const std::string& from, const std::string& output);
// The portal exit the link into to.input comes out of, if any.
std::optional<PortalExitRef> portalOfLink(const nlohmann::json& graph, const std::string& to, const std::string& input);

// A new portal on src's `output` ("": its first), its entry at (x, y), named `name` ("": after the
// source), made unique among the portals ("Guides", "Guides1", ...). Returns its name; the
// existing portal's when the output has one already.
std::string addPortal(nlohmann::json& graph, const std::string& from, const std::string& output, double x, double y,
                      const std::string& name = {});
// A new exit of `portal` at (x, y); its index, -1 for no such portal.
int addPortalExit(nlohmann::json& graph, const std::string& portal, double x, double y);
// The link into to.input drawn through a portal (Teleport Link): through its source output's
// portal (made, its entry at (entryX, entryY), when there is none), out of a new exit at
// (exitX, exitY). Returns where it comes out; none when nothing feeds to.input.
std::optional<PortalExitRef> teleportLink(nlohmann::json& graph, const std::string& to, const std::string& input, double exitX,
                                          double exitY, double entryX, double entryY);
// The link into to.input comes out of `portal`'s exit `exit` (it is connected from the portal's
// source first: a host connects, then calls this). False when the link is not fed from the
// portal's source, or no such exit.
bool addPortalLink(nlohmann::json& graph, const std::string& portal, int exit, const std::string& to, const std::string& input);
// The link into to.input drawn as a wire again (out of any exit). Whether it was in one.
bool removePortalLink(nlohmann::json& graph, const std::string& to, const std::string& input);
bool movePortal(nlohmann::json& graph, const std::string& portal, double x, double y);
bool movePortalExit(nlohmann::json& graph, const std::string& portal, int exit, double x, double y);
// A portal gone (its links drawn as wires again), or one exit of it.
bool removePortal(nlohmann::json& graph, const std::string& portal);
bool removePortalExit(nlohmann::json& graph, const std::string& portal, int exit);
// Renames a portal (made unique among the others). Returns the name it took, "" for no such portal.
std::string renamePortal(nlohmann::json& graph, const std::string& portal, const std::string& name);
// A node renamed: the portals and links naming it follow.
void renamePortalNode(nlohmann::json& graph, const std::string& from, const std::string& to);

}  // namespace groomlab
