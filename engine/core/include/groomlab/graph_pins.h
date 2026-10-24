// SPDX-License-Identifier: Apache-2.0
#pragma once

// A graph's pins: the reroute points its links are drawn through (Unreal's Blueprint reroute
// nodes), layout only, as the nodes' "ui" and the backdrops are (graph_io.h carries them; nothing
// evaluates them). In the graph's JSON:
//
//   "pins": {"points": [{"id": "pin1", "x": .., "y": ..}],
//            "routes": [{"from", "output"?, "to", "input", "via": ["pin1", ...]}]}
//
// A route lists the pins the link into to.input runs through, source first ("output" only when
// it is not the source's first, as a connection's). A pin carries one signal: every route through
// it comes from the same output, so links from one output can share pins (a bundle). Maya's Graph
// Editor keeps the same rules in Python (hosts/maya/scripts/groomlab/graph.py, GraphModel's pins).
//
// The functions take the graph's JSON (its "connections" say what each link comes from) and keep
// "pins" pruned and in order: a route whose link is gone, or comes from another source now, is
// dropped, so is a pin no route passes; routes sorted by (to, input), points by number; no "pins"
// at all when nothing is left.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace groomlab {

struct PinSource {
    std::string from, output;  // output "": the source's first
    bool operator==(const PinSource& o) const { return from == o.from && output == o.output; }
};
// A link through a pin: its destination, and the pin's place in its route.
struct PinLink {
    std::string to, input;
    std::size_t index = 0;
};

// Drops what no longer holds (see above) and puts "pins" in order.
void prunePins(nlohmann::json& graph);
// The pin ids, by number.
std::vector<std::string> pinIds(const nlohmann::json& graph);
bool pinPosition(const nlohmann::json& graph, const std::string& pin, double* x, double* y);
// The pins the link into to.input runs through, source first ([] for none).
std::vector<std::string> pinRoute(const nlohmann::json& graph, const std::string& to, const std::string& input);
// The signal a pin carries; none for no such pin.
std::optional<PinSource> pinSource(const nlohmann::json& graph, const std::string& pin);
// The links through a pin, by (to, input).
std::vector<PinLink> pinLinks(const nlohmann::json& graph, const std::string& pin);
// The pins from the source up to and including `pin`: what a link drawn out of it runs through.
std::vector<std::string> pinRouteTo(const nlohmann::json& graph, const std::string& pin);

// A new pin at (x, y) on the link into to.input, at `index` in its route (-1: last, by the
// destination). Returns its id ("pin<N>"), "" when nothing feeds to.input.
std::string addPin(nlohmann::json& graph, const std::string& to, const std::string& input, double x, double y, int index = -1);
bool movePin(nlohmann::json& graph, const std::string& pin, double x, double y);
// The pin out of every route through it (those links straighten there).
bool removePin(nlohmann::json& graph, const std::string& pin);
// Routes through `drop` run through `keep` instead (a pin dropped on another): only pins of one
// signal join. Returns whether they did.
bool mergePins(nlohmann::json& graph, const std::string& keep, const std::string& drop);
// The link into to.input runs through `via` (from its source as it is now; [] none). False when
// nothing feeds to.input.
bool setPinRoute(nlohmann::json& graph, const std::string& to, const std::string& input, const std::vector<std::string>& via);
// A node renamed: the routes from and to it follow (before prunePins, which would drop them).
void renamePinNode(nlohmann::json& graph, const std::string& from, const std::string& to);

}  // namespace groomlab
