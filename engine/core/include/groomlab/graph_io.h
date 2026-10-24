// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

#include "groomlab/graph.h"

namespace groomlab {

// A graph plus the names used in its JSON form and the node whose result is
// the groom.
//
// JSON schema (version 1):
//   {
//     "version": 1,
//     "nodes": [{"name": "scatter", "type": "Scatter", "params": {"density": 100.0}}],
//     "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
//                     {"from": "mask", "to": "clump", "input": "strength", "output": "r"}],
//     "output": "output"
//   }
// Params omitted from JSON take the operator's defaults. A 3-number array is a
// Vec3f; integers stay integers unless the operator declares a float param.
// A connection's "output" names which of the source node's outputs it reads (a Map node has
// luminance, r, g, b, a); absent, the first. It is written only when it is not the first.
// A node's optional "paramControls" keys params in place (Graph::setParamControls): each
// linked param takes the host's control of that name, three names for a vec3:
//   {"name": "length", "type": "Length", "params": {"factor": 1.0},
//    "paramControls": {"factor": "length_factor"}}
// "params" keeps the param's own value (used where the host sets no control).
// The node editors' layout lives in the graph too, so it travels with the groom between hosts
// (Maya's Graph Editor, the standalone app's node graph, groom files): a node's optional
// "ui": {"x": .., "y": ..} (its position) and the graph's optional "backdrops": [{"title", "x",
// "y", "w", "h", "color"}, ...]. Evaluation never reads them; load and save carry them.
struct GraphDocument {
    Graph graph;
    std::map<std::string, NodeId> nodesByName;
    NodeId output = 0;
    struct Position {
        double x = 0.0, y = 0.0;
    };
    std::map<std::string, Position> positions;  // by node name: the "ui" each node has
    std::string backdropsJson;                  // the "backdrops" array as JSON text ("" for none)
    std::string pinsJson;                       // the "pins" object (the links' reroute points) as JSON text ("" for none)

    NodeId node(const std::string& name) const;  // throws GraphError if missing
    std::string nameOf(NodeId id) const;

    // A copy with the same names and output, the graph copied by Graph::clone (cached results
    // included): parse a graph's JSON once and copy the document for each other evaluation of it
    // a host keeps (cards, rachis, rest pose), instead of loadGraphJson again.
    GraphDocument clone() const { return {graph.clone(), nodesByName, output, positions, backdropsJson, pinsJson}; }
};

// Parses a graph. Empty text or "{}" yields the default graph. Throws
// GraphError on malformed JSON, unknown operators or bad connections.
GraphDocument loadGraphJson(const std::string& text);

std::string saveGraphJson(const GraphDocument& doc);

// An operator of `type` (GraphError for an unknown one) with a node's params as a graph's JSON
// holds them (`params`: an object's text; "" none), the rest at their defaults; a value that does
// not parse keeps the default. No graph: what reads a node's settings alone (graphChannels).
std::unique_ptr<Operator> operatorFromJson(const std::string& type, const std::string& params);

// A node's params as a graph's JSON holds them (an object: numbers, colors as [r, g, b], maps as
// {"vertexMap" | ..., "component", "min", "max"}, ramps as keys).
std::string paramsJson(const ParamSet& params);

// A ramp param's value as a graph's JSON holds it ([[position, value], ...], or {"keys": [...],
// "linear": true}), parsed as loadGraphJson parses it (keys sorted). Throws GraphError for
// anything else.
Ramp rampFromJson(const std::string& text);

// Hair out of the box (hair_basics without its clumps): Scalp -> Scatter -> Interpolate (+ Guides,
// Scalp) -> Length (uneven ends) -> Noise -> HairColor -> Width -> Resample (even) -> Output.
const std::string& defaultGraphJson();

// Every registered operator as JSON, for editors and docs:
//   [{"type": "Clump", "version": 1, "category": "look", "fixedChannels": [],
//     "inputs": [{"name": "strands", "type": "Strands", "optional": false}, ...],
//     "output": {"name": "out", "type": "Strands"},          (the first of "outputs")
//     "outputs": [{"name": "out", "type": "Strands"}, ...],
//     "params": [{"name": "strength", "kind": "float", "default": 1.0,
//                 "description": "...", "mappable": true, "unit": "", "choices": [],
//                 "animatable": true, "group": "", "advanced": false, "worldDirection": false,
//                 "channel": "",
//                 "min": -1, "max": 1, "softMin": -1, "softMax": 1,
//                 "showWhen": {"param": "mode", "in": ["scales"]},
//                 "unitWhen": {"param": "space", "in": ["world"]}}, ...]}, ...]
// channel: "writes" or "reads" for a text param naming a strand channel (an attribute) the node
// writes or reads (ParamSpec::channel), else "". fixedChannels: the channels the operator writes
// under fixed names as its defaults set it (Flyaway's flyaway; Operator::channelWrites).
// channelOptional (only where true): a reading param whose default channel may well be missing.
// kind is one of bool, int, float, string, vec3, ramp, map. unit: what the number measures
// (ParamSpec::unit; a vec3's every component); unitWhen (only where set): the unit holds only
// while that param holds one of those values. animatable: false for a param a
// host must not key (ParamSpec::animatable: counts, segments, seeds). worldDirection: a direction
// in world space (a vec3) or a world axis ("x", "y", "z"), turned with the groom's points when it
// changes up axis (ParamSpec::worldDirection, convertUpAxis). min, max: hard limits the
// core clamps to; softMin, softMax: the slider's range (the limits when the spec gives none);
// each only where set, showWhen only where the param has one (ParamSpec; a UI also hides a param
// while the param its showWhen names is hidden: graph.h ParamCondition). version: the
// operator's newest behaviour (Operator::version). A graph's nodes store theirs ("version");
// GraphEdit::nodeSpecJson adds "nodeVersion" and "outdated" (an Upgrade is available).
//
// Saved graphs (saveGraphJson, GraphEdit, groom files) write every param of every node, set or
// not, and each node's version: a later change of a default or of an operator's behaviour
// never changes a saved groom. Loading stays tolerant (a param left out takes its default).
std::string operatorSpecsJson();
// One param's entry of operatorSpecsJson (a node's extra params too: a Script's, GraphEdit::nodeSpecJson).
std::string paramSpecJson(const ParamSpec& spec);

}  // namespace groomlab
