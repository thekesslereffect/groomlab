// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compound nodes (docs/CUSTOM_NODES.md, phase 1): a compound saved as one `.glnode` file (JSON) and
// listed in every host's Add Node menu as a node of its own. Every host reads the files through
// this: Maya's Graph Editor (groomlab.nodes), the standalone app's node graph, Blender's helper.
//
// The file (format 1):
//   {"glnode": 1, "id": "nick.soft_frizz", "version": 2, "label": "Soft Frizz",
//    "category": "look", "description": "...", "author": "...", "license": "...", "icon": "",
//    "size": 20,                                       (optional: the scalp size it was made on)
//    "inputs": [{"name": "strands", "type": "Strands"}],
//    "output": {"name": "out", "type": "Strands"},
//    "params": [{"name": "amount", "target": "noise.amplitude", "label": "Amount", "default": 0.05,
//                "min": 0, "softMax": 1, "unit": "length", "mappable": true, "description": "..."}],
//    "graph": {<the compound's subgraph: graph_io.h's schema, Input nodes for the inputs>}}
//
// A node added from the library is a Compound whose "definition" is the file's content (checked,
// with its params' "expose" map and their defaults set on the inner nodes): the groom carries its
// own copy, so it opens and renders anywhere without the library (Arnold, RenderMan, the CLI) and
// keeps its look when the library's copy changes. A library copy with a higher "version" makes
// GraphEdit::nodeSpecJson say "updateAvailable"; GraphEdit::updateLibraryNode takes it (never by
// itself). A library node whose copy cannot be read (a node of an unknown type inside, none
// embedded) hands its first input on, with a note saying why: a groom never fails to open by it.
//
// The library: every folder on GROOMLAB_NODE_PATH (the OS path separator between them), the
// user's folder (nodeUserDir: Documents/GroomLab/nodes, shared by every host), then the folder
// shipped with the host (setShippedNodeDir). A file in an earlier folder hides one with the same
// id in a later one.

// Script nodes (phase 2, script.h): the same header with params as real specs, what the script
// writes and the script itself in place of "graph":
//   {"glnode": 1, "id": "nick.wave", "version": 1, "label": "Wave", "category": "shape", ...,
//    "inputs": [{"name": "strands", "type": "Strands"}, {"name": "guides", "type": "Guides",
//                "optional": true}],                     (strands first; guides for guidePoint)
//    "output": {"name": "out", "type": "Strands"},
//    "params": [{"name": "amplitude", "type": "length", "label": "Amplitude", "default": 0.3,
//                "min": 0, "softMax": 2, "mappable": true, "description": "..."}],
//    "writes": ["P"],                                    (P, width, color, channel.<name>)
//    "script": "per point { P += ...; }"}
// A param's "type": number (its "unit" optional: length, width, fraction, angle...), length,
// int, bool, enum (with "choices"; the script reads the choice's index), color (a vector) or
// ramp (read as $name(x); its default [[position, value], ...]). "mappable" (number, length):
// a map, an expression or a field drives it per strand or per point. A node added from the
// library is a Script node (graph op "Script") holding the checked file as its "definition";
// a groom carries it as it carries a compound node's copy.

#include <optional>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/script.h"

namespace groomlab {

// The newest file format this build reads and writes ("glnode").
constexpr int kNodeFileFormat = 1;

// A node type's name in the Add Node menus ("node:<id>"): GraphEdit::addNode takes it.
std::string libraryNodeType(const std::string& id);
// The id a "node:<id>" type names; "" for another type.
std::string libraryNodeId(const std::string& type);
// The name a node added from the library takes from its label: "Soft Frizz" -> "softFrizz".
std::string libraryNodeName(const std::string& label);

// One node file of the library.
struct LibraryNode {
    std::string id;
    int version = 0;
    std::string label, category, description, author, license, icon;
    std::string path;
    std::string definition;  // the checked file as a Compound's or a Script's definition (JSON); "" when error
    std::string error;       // why the file cannot be used ("" none)
    std::string kind;        // "compound" or "script" ("" when unread)
};

// ---- Script nodes ----
// Whether a node file's text (or a definition) is a script node's (it has "script").
bool isScriptNodeFile(const std::string& text);
// A script node's header as the node runs and shows it: its inputs, params (each a ParamSpec: its
// default, range, unit, choices, "(mappable)" in its description; and its label), what it writes
// and its script. Throws GraphError saying what is wrong and where ("params[1]: ...").
struct ScriptNodeHeader {
    std::vector<PortSpec> inputs;
    std::vector<ParamSpec> params;
    std::vector<std::string> labels;  // each param's ("" none)
    std::vector<ScriptParam> scriptParams;
    std::vector<std::string> writes;
    std::string script;
};
ScriptNodeHeader scriptNodeHeader(const std::string& definitionJson);
// What is wrong with a script node file (or its definition), for an editor:
//   {"errors": [{"part": "json" | "header" | "script", "message": "...", "line": 3, "column": 9}]}
// (line and column of the script's text, 0 for a header's error); no errors when it checks.
std::string scriptNodeErrorsJson(const std::string& text);
// The graph with script node `node` given an edited `definitionJson` (every host's Edit Script):
// the params its header no longer has go, with their input ports and the links into them; a param
// still at its old default takes the new default; the new ones are written at theirs. Throws
// std::invalid_argument for a definition that does not check (its errors, a line each, as
// "line L, column C: message") or a node that is no Script node.
std::string withScriptDefinition(const std::string& graphJson, const std::string& node, const std::string& definitionJson);
// A new script node file (JSON text) to start from: a param and a short per point script, the id
// from the label and author (suggestNodeId).
std::string scriptNodeTemplate(const std::string& label = "My Script", const std::string& author = {});

// A node file's text checked and made a Compound's definition (JSON text): the params' targets as
// "expose", their defaults set on the inner nodes. Throws GraphError saying what is wrong (and
// where: "params[2]: ..."): not JSON, a format newer than kNodeFileFormat, a bad id, a missing
// label, a graph that does not load, an input or param that fits nothing. A script node's file:
// checked as it is (its header, and its script compiled: "script: ... (line 3, column 9)").
std::string checkNodeFile(const std::string& text);
// The file at `path` read and checked (never throws: LibraryNode::error says why not).
LibraryNode readNodeFile(const std::string& path);
// Whether an id can be a node's: two or more parts of a-z, 0-9, _ and - joined by dots
// ("studio.hair.soft_frizz"). "" or why not.
std::string nodeIdError(const std::string& id);
// An id for a new node from its label and author: "nick_rutlinh.soft_frizz" ("my." without author).
std::string suggestNodeId(const std::string& label, const std::string& author);

// The user's node folder: $GROOMLAB_USER_NODES when set, else Documents/GroomLab/nodes (Windows:
// the profile's; elsewhere $HOME's). "" when no home is known. Made by the first save.
std::string nodeUserDir();
// The folder of nodes shipped with this host ("" none: the CLI, the procedurals).
void setShippedNodeDir(const std::string& dir);
std::string shippedNodeDir();
// GROOMLAB_NODE_PATH's folders, nodeUserDir(), shippedNodeDir(): where nodes are looked up.
std::vector<std::string> nodeSearchDirs();
// Every node file in `dirs` (*.glnode), an id in an earlier folder hiding a later one's; sorted
// by label. Files that cannot be used come too, with their error (and the id when it was read).
std::vector<LibraryNode> listLibraryNodes(const std::vector<std::string>& dirs);
// listLibraryNodes(nodeSearchDirs()), cached: read again when the folders or their files change
// (looked at once a second at most) and after refreshNodeLibrary (what an Add Node menu opening
// calls) or a save.
std::vector<LibraryNode> libraryNodes();
void refreshNodeLibrary();
// The usable library node of that id, or nullopt.
std::optional<LibraryNode> findLibraryNode(const std::string& id);
// What the Add Node menus list (as operatorSpecsJson's entries: type "node:<id>", label,
// category, categoryTitle, categoryOrder, order, description, keywords, inputs, output; with id,
// version, author, license, icon, path), plus the files that cannot be used:
//   {"nodes": [...], "errors": [{"path", "id", "error"}], "dirs": [...], "userDir": "..."}
// A category naming a built-in one (its id or title) lists under it; another under its own
// title, after the built-in ones; none under "My Nodes".
std::string libraryNodesJson();  // (the folders read afresh: a menu opening)

// The node file (JSON text, fields in the documented order) made from the Compound named
// `compound` in `graphJson`. `headerJson` (an object; every field optional): "id", "version",
// "label", "category", "description", "author", "license", "icon", "params" (the exposed params
// to include, by name, in order; absent: all of them) and "paramInfo" ({name: {"label",
// "description", "min", "max", "softMin", "softMax"}}). What it leaves out comes from the
// compound's own copy when it is a library node, else from the compound (label: its name in
// words; version 1). Exposed params left out keep the compound's value inside. `size`: the scalp
// size the groom has (recorded as "size", so the node fits the scalp it is added to). Throws
// GraphError when the node is not a Compound or the file would not check.
std::string nodeFileFromCompound(const std::string& graphJson, const std::string& compound, const std::string& headerJson,
                                 std::optional<double> size = std::nullopt);
// Writes a node file (checked) to `dir` ("": nodeUserDir(), made when missing) as <id>.glnode and
// returns its path. Throws GraphError when it does not check or cannot be written.
std::string saveNodeFile(const std::string& text, const std::string& dir = {});
// Copies the node file at `path` (checked) into `dir` ("": nodeUserDir()); returns the new path.
std::string importNodeFile(const std::string& path, const std::string& dir = {});

// The params of a Compound made from a library node: {"definition": its copy, fitted from the
// size the file records to `size` (each param's default and range with it)}. GraphError for a
// node whose file cannot be used.
std::string libraryNodeParamsJson(const LibraryNode& node, std::optional<double> size);

}  // namespace groomlab
