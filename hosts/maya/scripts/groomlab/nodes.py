# SPDX-License-Identifier: Apache-2.0
"""Custom nodes: a compound saved as one .glnode file, or a script node (a short script in the
expression language: docs: Custom nodes), listed in the Graph Editor's Add Node menu and Tab search
under its category, and added as one node.

A node added from the library is a Compound (or a Script node) holding a copy of the file, so the
groom opens and renders anywhere without it (another machine, Arnold, RenderMan, the CLI). When the
library has a newer version of a node a groom uses, the Graph Editor marks the node; update_node
takes it.

The library is looked up, in order, in:
  1. every directory on GROOMLAB_NODE_PATH (os.pathsep-separated): a studio's shared nodes,
  2. the user's folder (GROOMLAB_USER_NODES, else Documents/GroomLab/nodes; the standalone app and
     Blender use it too), where save_node and import_node write,
  3. the nodes shipped with GroomLab (<module>/nodes).
A node in an earlier directory hides one with the same id in a later one.

    from groomlab import nodes
    nodes.library_nodes()                         # [{"type": "node:groomlab.soft_frizz", "label": ...}, ...]
    nodes.add_node(groom, "groomlab.soft_frizz")  # a Compound holding the file's copy
    nodes.save_node(groom, "compound1", label="My Frizz", category="Look", author="Me")
    nodes.import_node("C:/downloads/wave.glnode")
    nodes.update_node(groom, "softFrizz")         # the library's newer copy (one undo step)
    name = nodes.add_script_node(groom, nodes.script_template("Lift"), after="width")
    nodes.set_script(groom, name, definition)     # an edited file (ValueError: what is wrong)
    nodes.save_script_node(groom, name)           # to the library, one version up
"""

import json
import os

import groomlab_core

from groomlab.base import undo_chunk
from groomlab.params import get_graph, set_graph

from groomlab.shipped import shipped_dir as _shipped_dir  # noqa: E402
SHIPPED_DIR = _shipped_dir("nodes")
groomlab_core.set_shipped_node_dir(SHIPPED_DIR)


def node_dirs():
    """Where nodes are looked up, in order (GROOMLAB_NODE_PATH's folders, the user's, the shipped)."""
    return list(groomlab_core.node_search_dirs())


def user_dir():
    """The user's node folder (made by the first save)."""
    return groomlab_core.node_user_dir()


def library_nodes(errors=False):
    """The library's nodes as the Add Node menu lists them (read afresh): dicts with type
    ("node:<id>"), id, version, label, category, categoryTitle, description, author, license, path.
    With `errors`, (nodes, errors): the files that cannot be used, each {"path", "id", "error"}."""
    library = groomlab_core.library_nodes()
    return (library["nodes"], library["errors"]) if errors else library["nodes"]


def _groom_size(groom):
    from groomlab import presets
    return presets.scalp_size(groom)


def _edit(groom):
    return groomlab_core.GraphEdit(json.dumps(get_graph(groom, expand_default=True)))


def add_node(groom, node_id, name=None, after=None):
    """Add library node `node_id` to the groom's graph (fitted to its scalp), fed by node `after`
    and feeding what it fed when given. Returns the new node's name. One undo step."""
    edit = _edit(groom)
    made = edit.add_node("node:" + node_id, name or "", "{}", None, None, _groom_size(groom))
    if after:
        graph = json.loads(edit.to_json())
        fed = [c for c in graph.get("connections", []) if c["from"] == after and not c.get("output")]
        edit.connect(after, made, "strands")
        for c in fed:
            edit.connect(made, c["to"], c["input"])
    set_graph(groom, json.loads(edit.to_json()))
    return made


def node_file(groom, compound, label=None, category=None, description=None, node_id=None, version=None,
              author=None, license=None, icon=None, params=None, param_info=None):
    """The .glnode text Save as Node writes for Compound `compound` (nothing saved). What is left
    out comes from the node's library copy when it is one, else from the compound: its name as the
    label, every exposed param, version 1 for a new id (the library's + 1 for one it has)."""
    header = {"id": node_id, "version": version, "label": label, "category": category, "description": description,
              "author": author, "license": license, "icon": icon, "params": params, "paramInfo": param_info}
    header = dict((k, v) for k, v in header.items() if v is not None)
    graph = json.dumps(get_graph(groom, expand_default=True))
    return groomlab_core.node_file_from_compound(graph, compound, json.dumps(header), _groom_size(groom))


def save_node(groom, compound, directory=None, **header):
    """Save Compound `compound` of the groom as a library node (its file in the user's folder,
    or `directory`); the header as node_file takes it (label, category, description, node_id,
    version, author, license, icon, params, param_info). The compound then holds the saved copy
    (one undo step), so it shows no update. Returns the file's path."""
    text = node_file(groom, compound, **header)
    path = groomlab_core.save_node_file(text, directory or "")
    with undo_chunk("save node"):
        update_node(groom, compound, node_id=json.loads(text)["id"])
    return path


def import_node(path, directory=None):
    """Copy a .glnode file into the user's node folder (or `directory`), checked first (ValueError
    saying what is wrong). Returns the new path."""
    try:
        return groomlab_core.import_node_file(path, directory or "")
    except RuntimeError as e:
        raise ValueError(str(e))


def node_info(groom, node):
    """What a node is when it came from the library: {"id", "version", "latest", "updateAvailable",
    "embedded", "category", "author", "license", "icon"}; None for another node."""
    spec = json.loads(_edit(groom).node_spec_json(node))
    return spec.get("library")


# -- Script nodes (docs: Custom nodes, Script nodes) --------------------------------------------


def script_template(label="My Script", author=""):
    """A new script node's file (a dict) to start from: one param, a short per point script, an id
    from the label and author that the library does not have yet."""
    definition = json.loads(groomlab_core.script_node_template(label, author))
    taken = set(n["id"] for n in library_nodes())
    base, i = definition["id"], 2
    while definition["id"] in taken:
        definition["id"] = "%s%d" % (base, i)
        i += 1
    return definition


def node_name(label):
    """The name a node takes from its label: "My Script" -> "myScript"."""
    return groomlab_core.library_node_name(label)


def script_errors(definition):
    """What is wrong with a script node's file (a dict or its JSON text): [{"part": "json" | "header"
    | "script", "message", "line", "column"}] (line and column in the script, 0 for the header); []
    when it checks."""
    text = definition if isinstance(definition, str) else json.dumps(definition)
    return list(groomlab_core.script_node_errors(text))


def next_version(definition):
    """A script node's file one version past the library's copy of it (its own version when the
    library has none)."""
    definition = dict(definition)
    listed = dict((n["id"], n["version"]) for n in library_nodes())
    if definition.get("id") in listed:
        definition["version"] = max(int(definition.get("version", 1)), listed[definition["id"]] + 1)
    return definition


def add_script_node(groom, definition=None, name=None, after=None):
    """Add a script node (its file: a dict, script_template()'s by default), fed by node `after` and
    feeding what it fed when given. Returns its name. One undo step."""
    definition = definition or script_template()
    edit = _edit(groom)
    made = edit.add_node("Script", name or edit.unique_name(node_name(definition.get("label") or "Script")),
                         json.dumps({"definition": definition}), None, None, _groom_size(groom))
    if after:
        graph = json.loads(edit.to_json())
        fed = [c for c in graph.get("connections", []) if c["from"] == after and not c.get("output")]
        edit.connect(after, made, "strands")
        for c in fed:
            edit.connect(made, c["to"], c["input"])
    set_graph(groom, json.loads(edit.to_json()))
    return made


def script_definition(groom, node):
    """A script node's file (a dict)."""
    graph = get_graph(groom, expand_default=True)
    found = next((n for n in graph.get("nodes", []) if n.get("name") == node), None)
    if found is None or found.get("type") != "Script":
        raise ValueError("%s is not a script node" % node)
    return (found.get("params") or {}).get("definition") or {}


def set_script(groom, node, definition):
    """Give script node `node` an edited file (a dict); ValueError listing what is wrong when it does
    not check. Params the header no longer has go (their links too); one at its old default takes
    the new one. One undo step."""
    errors = script_errors(definition)
    if errors:
        raise ValueError("; ".join(("line %d, column %d: " % (e["line"], e["column"]) if e["line"] else "") + e["message"]
                                   for e in errors))
    graph = get_graph(groom, expand_default=True)
    # Every host's (node_library.h withScriptDefinition): params the header no longer has go with
    # their ports and links; a param at its old default takes the new one.
    try:
        text = groomlab_core.with_script_definition(json.dumps(graph), node, json.dumps(definition))
    except ValueError as e:
        raise ValueError(str(e).replace("\n", "; "))
    edit = groomlab_core.GraphEdit(text)
    edit.complete_params()  # the new params at their defaults
    set_graph(groom, json.loads(edit.to_json()))


def save_script_node(groom, node, as_new=False, directory=None):
    """Save script node `node` to the user's node library (or `directory`): one version up when the
    library has its id; as_new: under its id as a new node (ValueError when the library has it).
    The node then holds the saved copy. Returns the file's path."""
    definition = script_definition(groom, node)
    if as_new:
        if definition.get("id") in set(n["id"] for n in library_nodes()):
            raise ValueError("the library has a node %s already: give the new node another id" % definition.get("id"))
        definition = dict(definition, version=1)
    else:
        definition = next_version(definition)
    try:
        path = groomlab_core.save_node_file(json.dumps(definition, indent=2), directory or "")
    except RuntimeError as e:
        raise ValueError(str(e))
    with undo_chunk("save script node"):
        set_script(groom, node, definition)
    return path


def update_node(groom, node, node_id=None):
    """Replace a library node's copy with the library's (the exposed params it still has keep their
    values); `node_id`: take that library node's instead (a compound becomes it). One undo step.
    False when it had the library's copy already."""
    edit = _edit(groom)
    changed = edit.update_library_node(node, _groom_size(groom), node_id or "")
    if changed:
        set_graph(groom, json.loads(edit.to_json()))
    return changed
