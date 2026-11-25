// SPDX-License-Identifier: Apache-2.0
#pragma once

// Edits of a graph's JSON (graph_io.h's schema) as every host's node editor makes them: Maya's
// Graph Editor (the Python GraphModel, groomlab/graph.py) and the standalone app's node graph.
// The JSON is edited as it is written: every field a node has (its params, paramInputs,
// paramControls, bypass, "ui" position, anything a host added) is kept, and a graph that does
// not load yet (an input unconnected, no output) can still be edited.
//
// A node's ports come from its operator's spec (operatorSpecsJson), except that a Compound's
// come from its definition (its inputs, its inner output's type, its exposed params), an Input's
// output type from its "type" param, and every param in "paramInputs" adds a Field port (a
// Curve port for a ramp). A connection's types must fit (portTypesMatch on the output's type as
// connected: a Curves output gives what feeds it) and may not make a cycle.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/graph.h"

namespace groomlab {

class GraphEdit {
public:
    // Throws GraphError when the text is not a JSON object.
    explicit GraphEdit(const std::string& graphJson);
    ~GraphEdit();
    GraphEdit(const GraphEdit&);
    GraphEdit& operator=(const GraphEdit&);

    std::string toJson() const;  // compact

    bool hasNode(const std::string& name) const;
    std::string nodeType(const std::string& name) const;  // GraphError for no such node
    std::vector<std::string> nodeNames() const;           // in the graph's order

    // A new node's name: `base` with its first letter lowered when no node has it, else
    // base1, base2, ... (the first free).
    std::string uniqueName(const std::string& base) const;
    // Adds a node of `type` (GraphError for an unknown one) named `name` ("": uniqueName(type);
    // GraphError when a node has it), its params from `paramsJson` (an object; "" none). A node
    // whose operator has a "seed" param `paramsJson` leaves out takes a seed no other node of its
    // type has: the default when free, else the largest used + 1. `x`, `y` set its position
    // ("ui"). `groomSize`: the groom's scalp size (presetScalpSize): the spec defaults
    // `paramsJson` leaves out that measure something are fitted to it as a preset's params are,
    // the defaults being authored at kDefaultsScalpSize (fitNewNodeParamsJson, presets.h); nullopt:
    // as they are. Returns the name.
    std::string addNode(const std::string& type, const std::string& name = {}, const std::string& paramsJson = {},
                        std::optional<double> x = std::nullopt, std::optional<double> y = std::nullopt,
                        std::optional<double> groomSize = std::nullopt);
    // Removes the node with every connection to or from it; the graph's output is cleared when
    // it was the node. GraphError for no such node.
    void removeNode(const std::string& name);
    // Renames a node: its connections and the graph's output follow. GraphError for no such
    // node, an empty name, or a name another node has.
    void renameNode(const std::string& name, const std::string& newName);
    // The node whose result is the groom ("" none).
    std::string outputNode() const;
    // Makes `name` the graph's output. GraphError for no such node.
    void setOutputNode(const std::string& name);
    // Drops every node's position ("ui"), so an editor lays the graph out again (Tidy Layout).
    void clearPositions();
    // Connects src's output `output` ("": its first) to dst's input, replacing what fed it; the
    // connection records the output only when it is not the first. Throws GraphError for an
    // unknown node, input or output, types that do not fit, or a cycle.
    void connect(const std::string& src, const std::string& dst, const std::string& input, const std::string& output = {});
    // A link the user drew in an editor (every host's): connect, then a node whose dropdown says
    // whether it uses that input set to use it: a Clump of centersFrom pick fed centers takes them
    // (inputPoints; guides, a Guides node's, an ImportCurves', a GuideCache's or a Simulate's, through
    // any curve nodes after them: inputCurves, each clump along its guide), a Cards node of
    // cardsFrom strands fed guides lays its cards along them (guides).
    // A Value node no link leaves yet, drawn into a param's input, takes that param's kind first
    // (valueKindForInput). Disconnecting changes nothing back; connect alone (pastes, compounds,
    // scripts, loading) never changes a param. True when a param changed.
    bool connectByHand(const std::string& src, const std::string& dst, const std::string& input,
                       const std::string& output = {});
    // A Clump the user just wired into a chain after another Clump (issue #115): its strands
    // coming through that Clump (along each node's first input), it is set to clump inside that
    // one's clumps (centersFrom upstreamClump, upstreamChannel that Clump's clumpChannel, its own
    // clumpChannel subClumpId where it would take the same, the nodes after it that read its clumps,
    // a Curl's coherence clump, following that name); after a Braid (the nearer of the
    // two), along each braid's sections (upstreamChannel braidSection). Once: only while its
    // centersFrom is still pick and nothing feeds its centers (a choice the user made stays). connectByHand on a
    // Clump's strands and the inserts (graph_inserts.h, Add Node After) call it; connect alone
    // never does. True when it changed the node.
    bool adoptUpstreamClump(const std::string& node);
    // Removes what feeds dst's input; false when nothing did.
    bool disconnect(const std::string& dst, const std::string& input);
    // Detach (every host's node menu, Ctrl+Shift+X): takes a node out of the flow as if it were
    // not there, the node kept. What fed its first connected input (in its spec's order) feeds
    // each input the node fed instead, where that fits (connect's checks; an input it does not fit
    // is left unconnected), and becomes the graph's output when the node was. Every other link to
    // or from the node goes. The node moves below the new link: midway between that source and
    // the first node it fed, `drop` lower (y grows downward, as every editor draws). GraphError
    // for no such node.
    struct Detached {
        std::string source;                                    // what fed it ("": nothing)
        std::vector<std::pair<std::string, std::string>> relinked;  // (node, input) the source feeds now
        bool changed = false;                                  // any link went or was made
    };
    Detached detach(const std::string& name, double drop = 90.0);
    // Several nodes taken out of the flow together: the links
    // among them stay; every link from outside into them and from them to outside goes, and what
    // fed the group feeds what it fed, where that fits: each link leaving member M is fed from the
    // source outside the group M's first connected inputs lead back to (through the members). The
    // nodes stay where they are. One name: detach without the move. Unknown names are left out.
    Detached detachGroup(const std::vector<std::string>& names);
    // Delete as every host's graph editor does (issue #80): the node detached (what fed it feeds what
    // it fed, the output passes to it) and then removed. GraphError for no such node.
    Detached removeNodeKeepingFlow(const std::string& name);
    // Copy (every host's node editor, Ctrl+C): the nodes `names` (unknown names dropped) with every
    // field, in the graph's order, and the links among them, as JSON text for the clipboard
    // ({"groomlabNodes": 1, "nodes", "connections"}). An Output node is never copied (a graph has
    // one). "" when that leaves nothing.
    std::string copyNodes(const std::vector<std::string>& names) const;
    // Paste (Ctrl+V, Duplicate): the copied nodes added with every field as copied (their seeds and
    // the controls keying their params too), each named afresh when its name is taken (uniqueName)
    // and moved by (dx, dy), the links among them made again (connect's checks). Returns (the new
    // node, the node it copies) per node, in the clipboard's order: what a host keeps by node
    // outside the graph is copied with them (a Sculpt node's edits). GraphError, the graph
    // unchanged, for text that holds no copied nodes, a node type this build lacks, or a link that
    // does not fit.
    std::vector<std::pair<std::string, std::string>> pasteNodes(const std::string& clipboard, double dx = 0.0, double dy = 0.0);
    // Duplicate (every host's Ctrl+D): the nodes copied and pasted moved by (dx, dy) as above, and
    // then every link from outside them into them made again into their copies, so the copies read
    // what the nodes read (what the nodes fed stays theirs alone). Returns (copy, node) per node.
    // GraphError, the graph unchanged, as pasteNodes.
    std::vector<std::pair<std::string, std::string>> duplicateNodes(const std::vector<std::string>& names, double dx = 0.0, double dy = 0.0);
    // Shift+drag from an output (every host's, a bundle of links picked up at once): every link
    // reading src's `output` ("": its first) reads newSrc's `newOutput` instead, where it fits
    // (connect's checks; a link that does not fit, or would make a cycle, stays as it was). Returns
    // the (node, input) links moved, in the graph's order. GraphError for no such node or output.
    std::vector<std::pair<std::string, std::string>> moveOutputLinks(const std::string& src, const std::string& output,
                                                                     const std::string& newSrc, const std::string& newOutput = {});
    // The links reading src's `output` ("": its first), as (node, input), in the graph's order.
    std::vector<std::pair<std::string, std::string>> outputLinks(const std::string& src, const std::string& output = {}) const;

    // ---- Pass-through outputs (every host's: right-click an input > Pass Through as Output) ----
    // A node can hand any of its inputs on as an extra output, "thru_<input>" (passThroughOutput),
    // listed in its "passThrough": whatever comes into that input comes out of it as it came, so it
    // can feed the next node too (the node itself is not run for it). Its type is what feeds the
    // input (the input's own type while nothing does). Loading a graph links what reads it to what
    // feeds the input (resolvePassThrough); with nothing feeding the input, nothing reads it.
    static std::string passThroughOutput(const std::string& input) { return "thru_" + input; }
    // The input a pass-through output hands on ("" when `output` is not one).
    static std::string passThroughInput(const std::string& output);
    // The inputs the node passes through, in its "passThrough"'s order. GraphError for no such node.
    std::vector<std::string> passThrough(const std::string& name) const;
    // Turns an input's pass-through output on or off; off, the links reading it go. False when
    // nothing changed. GraphError for no such node or input.
    bool setPassThrough(const std::string& name, const std::string& input, bool on);
    // A pass-through output's name as shown (Rename Output): the node's "passThroughNames"[input],
    // its spec's output "label"; "" for none (shown as thru_<input>). Only how it is shown: links
    // still read thru_<input>. Setting "" clears it. False when nothing changed. GraphError for no
    // such node, or an input it does not pass through.
    std::string passThroughName(const std::string& name, const std::string& input) const;
    bool setPassThroughName(const std::string& name, const std::string& input, const std::string& label);
    // Moves a pass-through output `by` places among the node's (-1 up, 1 down; clamped to the
    // ends): the order its outputs are drawn in (Move Output Up / Down). Its links stay. False when
    // it did not move. GraphError for no such node, or an input it does not pass through.
    bool movePassThrough(const std::string& name, const std::string& input, int by);
    // Every connection checked and made again in order (connect's checks), as when a host loads a
    // graph to edit it. Throws GraphError at the first that does not fit.
    void checkConnections();

    // A node's ports and params (operatorSpecsJson's form), as the edits see them: a Compound's
    // and an Input's from their params, the exposed params' ports ({"param": true}) after the
    // operator's inputs, "outputs" always listed, and "nodeVersion" (the behaviour the node keeps)
    // with "outdated" (true: an Upgrade is available, upgradeNode). A Compound's exposed params
    // take their inner params' specs (range, unit, group, advanced...; the default the inner
    // node's value; a showWhen naming an inner param shown by its exposed name, else dropped), in
    // the inner graph's order: its nodes as listed, each one's params as its operator lists them.
    // GraphError for no such node.
    std::string nodeSpecJson(const std::string& name) const;
    // A node's output type as connected (`output` "": its first): Curves gives what feeds its
    // first Curves input.
    std::string outputType(const std::string& name, const std::string& output = {}) const;
    // Each of a node's outputs' types as connected (outputType), in its spec's order.
    std::vector<std::string> outputTypes(const std::string& name) const;
    // View Up to Here (every host's eye on a node's header, and its V key): a node with an output
    // giving strands as connected (outputType: Strands, or Curves with nothing feeding it), not one
    // giving only guides, fields, values or a mesh. False for no such node.
    bool viewable(const std::string& name) const;
    // Bake Node to Guides (every host's node menu, its choices): a node, not the Output node, whose
    // first output gives strands or guides as connected (outputType). False for no such node.
    bool bakeable(const std::string& name) const;
    // A Value node no link leaves yet, drawn into a param's input (an exposed param's port): the
    // kind it takes to fit that param (a float "number", an int "integer", a bool "bool", a vec3
    // "vector", or "color" for a color, a string "text"); "" for anything else. connectByHand sets
    // it; editors offer the link while dragging when this is not "".
    std::string valueKindForInput(const std::string& src, const std::string& dst, const std::string& input) const;
    // Every node feeding `name`, directly or not.
    std::vector<std::string> upstream(const std::string& name) const;
    // Clump Colors (every host's button on a Clump, issue #77). clumpChannel: the strand channel a
    // Clump writes (its clumpChannel, "clumpId" unset; "" for another node, or none). clumpColorSource:
    // the color source showing that node's clumps as it wrote them, "<channel>@<node>" (strandColors;
    // "" when it writes none). clumpColored: whether a clump color `source` shows the node's
    // clumps: its own source, or the bare channel (grooms from before) when the node is the last
    // Clump writing it (every other one feeds it: the clumps the strands show). Missing nodes: "",
    // false.
    std::string clumpChannel(const std::string& node) const;
    std::string clumpColorSource(const std::string& node) const;
    bool clumpColored(const std::string& node, const std::string& source) const;
    // Show Tubes (every host's button on a Braid node): the node's section tubes drawn in the
    // viewport (braid_tubes.h). `shown`: the nodes showing theirs, a display setting
    // (DisplaySettings::braidTubes: names, one a line). braidTubesOffered: whether the node has the
    // button (a Braid, or a node giving guides: their tubes, braid_tubes.h guideTubes). braidTubesShown: whether it is lit (offered, and named in `shown`).
    // braidTubesToggled: `shown` after a click on the node's button (the node added, or taken out;
    // a node without the button changes nothing). braidTubesKept: `shown` without the names of
    // nodes that have no button now (a node deleted, or made another type: what a host stores after
    // an edit). Both keep the other names' order. A rename: renamedBraidTubes.
    bool braidTubesOffered(const std::string& node) const;
    // Show on Scalp (every host's eye on a Layers or ColorLayers node, issue #124): whether the node
    // has it. Lit, the scalp shows the node's maps combined (GroomSession::scalpMapView with
    // kNodeMapPrefix + its name), and stays on it while other maps are picked and painted.
    bool scalpViewOffered(const std::string& node) const;
    bool braidTubesShown(const std::string& node, const std::string& shown) const;
    std::string braidTubesToggled(const std::string& node, const std::string& shown) const;
    std::string braidTubesKept(const std::string& shown) const;

    // Writes every param each node leaves out with its spec default, and each node's version
    // when it has none (its operator's newest), compounds' inner nodes too: what saved grooms hold,
    // so a later change of a default never alters them (graph_io.h). addNode does it for its node.
    void completeParams();
    // The behaviour version the node evaluates with ("version"; none: its operator's newest).
    int nodeVersion(const std::string& name) const;
    // Raises an outdated node (nodeSpecJson's "outdated") to its operator's newest version (what
    // a host's Upgrade button does: the node then evaluates as new nodes do). False when it is
    // already the newest. GraphError for no such node.
    bool upgradeNode(const std::string& name);

    // The strand channels (attributes) per node as the graph is now: graphChannelsJson
    // (attributes.h), `guideMaps` the maps painted on the guides.
    std::string channelsJson(const std::vector<std::string>& guideMaps = {}) const;

    // The nodes the output is made from (the output node and everything upstream of it, bypassed
    // nodes included): the ones the groom evaluates. In the graph's order; none without an output.
    std::vector<std::string> feedingOutput() const;
    // What would stop the groom from evaluating, as every host's editor says it (no problems: fine).
    // feedingOutput true: the graph's own ("no output node", "output node 'x' does not exist"), then
    // those of the nodes the output is made from; false: those of the other nodes, which evaluate
    // only when viewed (warnings: never a reason not to apply the graph). Per node in the graph's
    // order, per input in its spec's: a required input left open ("length.strands is not
    // connected"), a link reading an output its source does not have ("mask has no output 'q'
    // (clump.strength reads it)": a Compound's definition changed), a link whose types no longer
    // fit ("noise gives guides but frizz.strands takes strands": connect's checks, on a graph
    // edited since); then each param set to a map with no name ("length.keep: the texture has no
    // name").
    std::vector<std::string> problems(bool feedingOutput = true) const;

    // Nodes of one type sharing a seed where that is likely a mistake (two Scatters: their
    // strands coincide; two Instances or Flyaways: they pick the same strands), as warnings.
    std::vector<std::string> seedWarnings() const;

    // ---- Library nodes (node_library.h): a type "node:<id>" given to addNode adds a Compound
    // holding the library's copy of that node (fitted to `groomSize` from the size its file
    // records), named after its label ("softFrizz"); GraphError for an id the library lacks.
    // nodeSpecJson of such a Compound gives its "label" and "description", and "library": {"id",
    // "version", "category", "author", "license", "icon", "latest" (the library's version, 0
    // none), "updateAvailable", "embedded" (false: the groom holds no copy)}; its exposed params
    // take the header's "label", description and ranges.
    // Replaces a library node's copy with the library's (one edit: hosts make it one undo step):
    // the exposed params it still has keep their values, links into inputs and params it no
    // longer has go. `id`: the library node to take instead of the one it holds (a compound just
    // saved as that node takes its copy). False when the copy was the library's already.
    // GraphError for a node that is not a library node (nor given an id) or an id the library lacks.
    bool updateLibraryNode(const std::string& name, std::optional<double> groomSize = std::nullopt, const std::string& id = {});

    // ---- Compounds (graph_compound.cpp): every host's Group (Ctrl+G) and Ungroup (Ctrl+Shift+G)
    // Replaces the nodes `names` (unknown names dropped) with one Compound node holding them,
    // named `compound` ("": uniqueName("compound")) and placed at their mean position. Each
    // outside source (a node and its output) feeding them becomes one input of the compound (named
    // after the first input it fed, else <node>_<output>) and an Input node inside ("in_<input>");
    // Value and Field ports (exposed params) alike. Exactly one of the nodes may feed the rest of
    // the graph, from its first output: it is the compound's output (none feeding out: the
    // graph's output when it is one of them, else their one sink). The inner nodes keep every
    // field (params, paramInputs, paramControls, bypass, position), so expand gives them back; the
    // graph's output follows. Returns the compound's name. GraphError, the graph unchanged, when
    // it cannot (nothing to group, two nodes feeding out, a node feeding out from another output).
    std::string collapse(const std::vector<std::string>& names, const std::string& compound = {});
    // Replaces a Compound node with the nodes it holds, the values of its exposed params applied
    // to them and every field kept, each named afresh when its name is taken (uniqueName); they go
    // where they were when grouped, moved with the compound (no position: fanned out from it).
    // What fed the compound feeds them, what it fed their output; the graph's output follows.
    // Returns their names (the definition's order). GraphError, the graph unchanged, for a node
    // that is not a Compound or a definition that cannot be expanded (an unknown type, no output
    // node inside, a link that fits nothing).
    std::vector<std::string> expand(const std::string& compound);
    // Where a flattened compound's parts went (flattenCompounds): its output node's path, and each
    // exposed param's inner node and param, by the compound's path (a compound inside another
    // by its path too: "outer/inner").
    struct FlatCompounds {
        std::map<std::string, std::string> output;                              // compound -> its output node
        std::map<std::string, std::map<std::string, std::pair<std::string, std::string>>> exposed;  // compound -> param -> (node, param)
        // The node and param a compound's exposed param sets (followed into compounds inside),
        // else the node and param given.
        std::pair<std::string, std::string> param(const std::string& node, const std::string& name) const;
        // The node whose result a compound gives, else the node given.
        std::string result(const std::string& node) const;
    };
    // Every compound evaluated as the nodes it holds, inline (GroomSession: the CPU, the GPU's
    // preview and the caches see one graph, as Unreal's collapsed graphs and Blender's node groups
    // run): each Compound replaced by its nodes as expand does, each named by its path
    // ("compound/inner", the path compounds give their nodes: Cards, Sculpt and Simulate key by
    // it), compounds inside them too. Left as they are: a compound running its nodes more than
    // once (repeat) or reading its pass (an Input named "iteration"), one bypassed (it hands its
    // input on, as its nodes would not), one with a
    // control keyed on a param, and one that cannot be expanded (a library node's copy that cannot
    // run). Returns where the flattened compounds' parts went.
    FlatCompounds flattenCompounds();

    // The node paths this edit's renames, groups and ungroups moved, in order (old -> new; a node
    // inside a compound by its path, "compound/inner"): a rename moves the node, Group moves each
    // grouped node in ("sculpt1" -> "compound1/sculpt1"), Ungroup moves them out (to their new
    // names when theirs were taken). A compound's move takes the paths inside it along. What a
    // host keeps by node path outside the graph follows them in the same step (a Sculpt node's
    // edits: moveSculptNodes, sculpt.h).
    const std::vector<std::pair<std::string, std::string>>& movedNodes() const;

private:
    void recordMove(const std::string& from, const std::string& to);  // one more movedNodes entry

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The graph with every param and node version written (GraphEdit::completeParams).
std::string completeGraphJson(const std::string& graphJson);

// Every link reading a pass-through output ("thru_<input>" of a node passing that input) made to
// read what feeds that input instead (followed through pass-throughs feeding pass-throughs); a link
// whose input nothing feeds goes. What loading a graph does (graph_io.h), and flattening compounds:
// the result evaluates as the graph drawn.
std::string resolvePassThroughJson(const std::string& graphJson);

// ---- Inside a compound (every host's: double-click a Compound to go into it, the bar's path or
// Backspace to come back out). `path` names the compound, one inside another by its path
// ("outer/inner"). compoundGraphJson: the graph a Compound holds as a graph of its own (its Input
// nodes, inner nodes, connections and output, the backdrops and pins drawn inside, and its exposed
// params as "exposed": {name: "node.param"}, which a rename inside follows). withCompoundGraph: the
// whole graph with the compound at `path` holding `innerJson` instead: the compound's inputs follow
// its Input nodes (their names and types, in order; a link into an input it no longer has goes),
// its exposed params naming a node it no longer holds go. GraphError for a path that is not a
// Compound holding a graph, or an inner graph with no output node.
std::string compoundGraphJson(const std::string& graphJson, const std::string& path);
std::string withCompoundGraph(const std::string& graphJson, const std::string& path, const std::string& innerJson);

// A node's spec as the edits see it (GraphEdit::nodeSpecJson), from the node's own JSON alone
// (`nodeJson`: {"type", "params", "paramInputs"...}).
std::string nodeSpecJson(const std::string& nodeJson);

}  // namespace groomlab
