# SPDX-License-Identifier: Apache-2.0
"""Editable model of a groom graph (the Python graph API, PG-7).

No Maya or Qt imports, so it runs anywhere and is what the graph editor edits. The edits every
host's node editor makes (a node added with its name and seed, removed, connected with the type
and cycle checks, a node's ports) are the core's (engine/core/include/groomlab/graph_edit.h,
through groomlab_core), so the standalone app edits a graph exactly as this does. Operator
ports and parameters come from the plugin's `groomLabOperators` command (see
OperatorCatalog.from_maya).

    catalog = OperatorCatalog.from_maya()
    model = GraphModel.from_groom(groom, catalog)
    model.add_node("Curl", name="curl")
    model.insert_between("clump", "length", "curl")
    model.set_param("curl", "radius", 0.02)
    model.save(groom)

The graph dict matches the groom's JSON (see engine/core/include/groomlab/graph_io.h), with the
layout in it as every host keeps it: an optional "ui": {"x": .., "y": ..} per node, the graph's
"backdrops" and its "pins", which evaluation ignores and groom files carry to the standalone app.

Pins are reroute points (Unreal's Blueprint reroute nodes) that links are drawn through, to tidy
the wires: {"points": [{"id", "x", "y"}], "routes": [{"from", "output"?, "to", "input", "via":
[pin ids, source to destination]}]}. A pin carries one signal (its routes share a source), so
several links from one output can run through the same pins as a bundle. A route whose link is
gone, or comes from another source now, is dropped, and a pin no route passes goes with it: the
core's rules (graph_pins.h, through groomlab_core.GraphPins), every host's.

A node can have several outputs (a Map node: luminance, r, g, b, a); a connection
{"from", "to", "input"} reads the source's first output, or the one its "output" names
(stored only when it is not the first):

    model.connect("mask", "clump", "strength", output="r")

A keyed param names the control that sets it on every frame in the node's "paramControls" (a
name, or three for a vector; groomlab.set_param_key makes the link and its attribute):

    model.link_control("clump", "strength", "clump_strength")
"""

import contextlib
import copy
import json

import groomlab_core

LAYOUT_DX = 240  # the core's column for a new node (graph_edit.cpp)
LAYOUT_DY = 140


class GraphError(ValueError):
    """An edit the graph cannot take (unknown node or port, type mismatch, cycle)."""


@contextlib.contextmanager
def _core_errors():
    """The core's GraphError as this module's (a ValueError), with its message."""
    try:
        yield
    except groomlab_core.GraphError as e:
        raise GraphError(str(e))


class OperatorCatalog(object):
    """Operator specs: ports, parameters and their defaults, by type name."""

    def __init__(self, specs):
        self._specs = {s["type"]: s for s in specs}

    @classmethod
    def from_json(cls, text):
        return cls(json.loads(text))

    _cached = (None, None)  # (groomLabOperators text, catalog): parsed once per plugin build

    @classmethod
    def from_maya(cls):
        from maya import cmds  # imported here: the model itself needs no Maya
        text = cmds.groomLabOperators()
        if OperatorCatalog._cached[0] != text:
            OperatorCatalog._cached = (text, cls.from_json(text))
        return OperatorCatalog._cached[1]

    def types(self):
        return sorted(self._specs)

    def set_library(self, entries):
        """The node library's compound nodes (groomlab_core.library_nodes()["nodes"]) as Add Node
        entries of type "node:<id>", in place of the ones set before. Returns a stamp of what they
        are (a menu filled from other entries fills again)."""
        for op_type in [t for t in self._specs if t.startswith("node:")]:
            del self._specs[op_type]
        for entry in entries:
            spec = dict(entry)
            spec.setdefault("params", [])
            spec["hidden"] = False
            spec["outputs"] = [entry.get("output") or {"name": "out", "type": "Strands"}]
            self._specs[entry["type"]] = spec
        return tuple((e["type"], e.get("version"), e.get("label"), e.get("categoryTitle")) for e in entries)

    def spec(self, op_type):
        try:
            return self._specs[op_type]
        except KeyError:
            raise GraphError("unknown operator type %r" % op_type)

    def input_spec(self, op_type, input_name):
        for p in self.spec(op_type)["inputs"]:
            if p["name"] == input_name:
                return p
        raise GraphError("%s has no input %r" % (op_type, input_name))

    def outputs(self, op_type):
        """The operator's output ports [{"name", "type"}], the primary first."""
        spec = self.spec(op_type)
        return spec.get("outputs") or [spec["output"]]

    def output_names(self, op_type):
        return [p["name"] for p in self.outputs(op_type)]

    def output_type(self, op_type, output=None):
        """The type of the operator's output `output` (a name; None: the first)."""
        for port in self.outputs(op_type):
            if output is None or port["name"] == output:
                return port["type"]
        raise GraphError("%s has no output %r" % (op_type, output))

    def param_spec(self, op_type, param):
        for p in self.spec(op_type)["params"]:
            if p["name"] == param:
                return p
        return None


# Nodes whose output port is set by a param: a Value's kind, an Expression's output (a field or a value).
VALUE_SETS = {"Value": ("kind",), "Expression": ("output",)}

VALUE_KINDS = {"any": "Any", "number": "Number", "integer": "Integer", "bool": "Bool", "vec3": "Vec3",
               "choice": "Choice", "text": "Text"}


def types_match(out_type, in_type, out_kind=None, in_kind=None):
    """Whether an output of `out_type` can feed an input of `in_type`: the same type, or Curves
    (strands or guides: the shaping nodes) on either side with Strands or Guides on the other, or a
    Value into a Field (the core's portTypesMatch); for Values, their kinds (`out_kind`, `in_kind`:
    "number", "integer", "bool", "vec3", "choice", "text"; None: any) must fit (valueKindsMatch)."""
    try:
        if out_type != in_type and not groomlab_core.port_types_match(getattr(groomlab_core.PortType, out_type),
                                                                     getattr(groomlab_core.PortType, in_type)):
            return False
        if out_type != "Value":
            return True
        want = (in_kind or "number") if in_type == "Field" else (in_kind or "any")
        return groomlab_core.value_kinds_match(getattr(groomlab_core.ValueKind, VALUE_KINDS[out_kind or "any"]),
                                               getattr(groomlab_core.ValueKind, VALUE_KINDS[want]))
    except (AttributeError, TypeError, KeyError):  # a name no port type (or kind) has
        return out_type == in_type


def kind_label(port_type):
    return {"Strands": "strands", "Guides": "guides", "Curves": "strands or guides"}.get(port_type, port_type)


def port_label(port_type, value_kind=None):
    """A port's type as a tooltip says it: a Value's with its kind ("number Value")."""
    if port_type == "Value" and value_kind and value_kind != "any":
        return "%s Value" % value_kind
    return kind_label(port_type)


def node_fields(node):
    """A node's fields as stored (type, params, paramInputs, bypass, ui and anything else the
    writer put there), without its name; empty paramInputs and a false bypass are dropped.
    Every field is kept so nothing the plugin writes is lost through a load and save."""
    fields = {}
    for key, value in node.items():
        if key == "name":
            continue
        if key in ("paramInputs", "paramControls") and not value:
            continue
        if key == "bypass" and not value:
            continue
        if key == "evaluation" and value == "always":
            continue
        fields[key] = copy.deepcopy(value)
    fields.setdefault("params", {})
    return fields


def without_layout(data):
    """A graph's dict without the editors' layout (every node's "ui", the backdrops, the pins, the portals)."""
    out = {k: v for k, v in data.items() if k not in ("backdrops", "pins", "portals")}
    out["nodes"] = [{k: v for k, v in n.items() if k != "ui"} for n in data.get("nodes", [])]
    return out


MAP_SOURCES = ("texture", "colorSet", "attribute", "expression", "guide")
# When a node runs (a node's "evaluation"; none: always): viewportOnly nodes are bypassed in
# renders, exports, the Arnold procedural and the CLI, renderOnly ones in the viewport.
EVALUATION_MODES = ("always", "viewportOnly", "renderOnly")


def map_source(value):
    """Which kind of map a param value is ("texture", "colorSet", "attribute", "expression" or "guide"),
    or None for a plain value."""
    if not isinstance(value, dict):
        return None
    return next((s for s in MAP_SOURCES if s in value), None)


BACKDROPS_KEY = "__backdrops__"


def linkable(spec):
    """Whether a param (its spec) can take an input port (the spec's "port"): every param but the few
    that set the node's own ports. A mappable number or a color field takes a Field port, a ramp a
    Curve port, any other param a Value port (a Value node, an Expression set to give a value)."""
    return bool(spec) and bool(spec.get("port"))


def value_port(spec):
    """Whether a param's input is a Value port (one value for the whole node)."""
    return bool(spec) and (spec.get("port") or {}).get("type") == "Value"

class GraphModel(object):
    """Nodes, connections and the output node of one groom graph."""

    def __init__(self, catalog, data=None):
        self.catalog = catalog
        self.nodes = {}          # name -> {"type": str, "params": dict, "ui": {"x", "y"}}
        self.order = []          # node names in insertion order (stable JSON and layout)
        self.connections = []    # [{"from": src, "to": dst, "input": port[, "output": src's output]}]
        self.output = None
        self.backdrops = []      # [{"title", "x", "y", "w", "h", "color"}]: layout only
        self._portals = None     # the graph's "portals" (graph_portals.h: layout only, kept by the core)
        self.exposed = None      # inside a compound (ScopedModel): its exposed params, {name: "node.param"}
        self._pins = None        # the graph's "pins" ({"points", "routes"}; None: none): layout only (the
                                 # module's notes), kept by the core (graph_pins.h); `pins` and `routes` read it
        self.renamed = {}        # old name -> new name (a path inside a compound: "compound1/sculpt1") since
                                 # the last save (the groom's viewNode and Sculpt edits follow)
        self.copied = {}         # pasted node -> the node it copies, since the last save (Sculpt edits follow)
        self.groom_size = None   # the groom's scalp size (presets.scalp_size): new nodes' defaults fit it
        # Why the groom's graph could not be read (from_groom with tolerant: a node or port this
        # GroomLab does not have, as in a graph a newer GroomLab saved); "" when it was. The model is then empty and
        # never written back (save, save_layout), so the groom keeps its graph.
        self.unreadable = ""
        if data:
            self.load_dict(data)

    # -- Construction ------------------------------------------------------

    @classmethod
    def from_groom(cls, groom, catalog=None, tolerant=False):
        """The groom's graph. A graph this GroomLab cannot read raises GraphError, or with `tolerant`
        (the Graph Editor) gives an empty model saying why (`unreadable`), never written back."""
        from maya import cmds
        catalog = catalog or OperatorCatalog.from_maya()
        text = cmds.getAttr(groom + ".graph") or "{}"
        data = json.loads(text)
        if not data:
            data = json.loads(cmds.groomLabDefaultGraph())
        try:
            model = cls(catalog, data)
        except GraphError as e:
            if not tolerant:
                raise
            model = cls(catalog)
            model.unreadable = ("Can't show this groom's graph (%s): it was most likely saved by a newer GroomLab. "
                                "Update GroomLab to edit it; the groom still evaluates what this version can, and "
                                "its graph is left as it is." % e)
            return model
        from groomlab import presets  # Maya's (scalp_size), as cmds above
        model.groom_size = presets.scalp_size(groom)
        if not any("ui" in model.nodes[n] for n in model.order):
            # A scene from before the layout lived in the graph kept it in graphUi: read it then.
            layout = json.loads(cmds.getAttr(groom + ".graphUi") or "{}")
            backdrops = layout.pop(BACKDROPS_KEY, [])
            if not model.backdrops:
                model.backdrops = [dict(b) for b in backdrops]
            for name, (x, y) in layout.items():
                if name in model.nodes:
                    model.set_position(name, x, y)
        model.ensure_layout()
        return model

    def ensure_layout(self):
        """Lay the graph out when any node has no position yet."""
        if any("ui" not in self.nodes[n] for n in self.order):
            self.auto_layout()

    def load_dict(self, data):
        self.nodes, self.order, self.connections, self.output = {}, [], [], None
        for n in data.get("nodes", []):
            self.catalog.spec(n["type"])  # validates the type
            self.nodes[n["name"]] = node_fields(n)
            self.order.append(n["name"])
        self.output = data.get("output")
        self.backdrops = [dict(b) for b in data.get("backdrops") or [] if isinstance(b, dict)]
        self._pins = None
        # Every connection made again as connect makes it (its checks), in one go in the core.
        self.connections = [dict(c) for c in data.get("connections", [])]
        edit = self._edit()
        with _core_errors():
            edit.check_connections()
        self.connections = json.loads(edit.to_json()).get("connections", [])
        self._load_pins(data.get("pins"))
        self._portals = [dict(p) for p in data.get("portals") or [] if isinstance(p, dict)] or None
        self.exposed = dict(data["exposed"]) if isinstance(data.get("exposed"), dict) else None

    # -- The core's edits (graph_edit.h): what every host's editor does to a graph ----------

    def _edit(self):
        """The graph as the core edits it."""
        return groomlab_core.GraphEdit(json.dumps(self.to_dict()))

    # The last graph the core read for a question, and its text (_query).
    _query_text = None
    _query_edit = None

    def _query(self):
        """The graph as the core reads it, for questions only (never edited through this one): the
        one made for the last question again while the graph's text is the same, so a repaint or a
        run of questions parses the graph once."""
        text = json.dumps(self.to_dict())
        if text != self._query_text:
            self._query_edit = groomlab_core.GraphEdit(text)
            self._query_text = text
        return self._query_edit

    def _take(self, edit):
        """The core's edited graph back into the model (a node that did not change keeps its dict)."""
        data = json.loads(edit.to_json())
        nodes, order = {}, []
        for n in data.get("nodes", []):
            fields = node_fields(n)
            kept = self.nodes.get(n["name"])
            nodes[n["name"]] = kept if kept is not None and node_fields(kept) == fields else fields
            order.append(n["name"])
        self.nodes, self.order = nodes, order
        self.connections = data.get("connections", [])
        self.output = data.get("output")
        if isinstance(data.get("exposed"), dict):
            self.exposed = data["exposed"]  # (a rename inside a compound: its exposed params follow)

    def to_dict(self):
        nodes = []
        for name in self.order:
            entry = {"name": name}
            entry.update(node_fields(self.nodes[name]))
            nodes.append(entry)
        data = {"version": 1, "nodes": nodes, "connections": copy.deepcopy(self.connections)}
        if self.output:
            data["output"] = self.output
        if self.backdrops:
            data["backdrops"] = copy.deepcopy(self.backdrops)
        pins = self._pins_dict()
        if pins:
            data["pins"] = pins
        portals = self._portals_list()
        if portals:
            data["portals"] = portals
        if self.exposed is not None:
            data["exposed"] = copy.deepcopy(self.exposed)
        return data

    def to_json(self, indent=None, layout=True):
        """The graph as the groom stores it, with its layout (every node's "ui", the backdrops);
        layout=False leaves the layout out."""
        data = self.to_dict()
        if not layout:
            data = without_layout(data)
        return json.dumps(data, indent=indent, separators=None if indent else (",", ":"))

    # -- Bypass --------------------------------------------------------------

    def bypassed(self, name):
        return bool(self._node(name).get("bypass"))

    def set_bypass(self, name, on):
        """Pass the node's first input straight through (compare with and without it)."""
        if on:
            self._node(name)["bypass"] = True
        else:
            self._node(name).pop("bypass", None)

    # -- Evaluation (when the node runs) ---------------------------------------

    def evaluation(self, name):
        """"always", "viewportOnly" (bypassed in renders and exports) or "renderOnly"
        (bypassed in the viewport)."""
        return self._node(name).get("evaluation", "always")

    def set_evaluation(self, name, mode):
        if mode not in EVALUATION_MODES:
            raise GraphError("evaluation is one of %s, not %r" % (", ".join(EVALUATION_MODES), mode))
        if mode == "always":
            self._node(name).pop("evaluation", None)
        else:
            self._node(name)["evaluation"] = mode

    # -- Copy and paste ---------------------------------------------------------

    def copy_nodes(self, names):
        """The nodes and the links between them, as JSON text for the clipboard (the core's
        GraphEdit.copy_nodes, the app's too): never the Output node; "" when that leaves none."""
        return self._query().copy_nodes(list(names))

    def paste_nodes(self, text, offset=(40.0, 40.0)):
        """Add copied nodes (new names, moved by `offset`) with their links (the core's
        GraphEdit.paste_nodes, the app's too). Returns the new names. A GraphError (text that holds
        no copied nodes, a link that does not fit) leaves the graph as it was."""
        edit = self._edit()
        with _core_errors():
            made = edit.paste_nodes(text or "", float(offset[0]), float(offset[1]))
        self._take(edit)
        for new, source in made:
            self.copied[new] = source  # its Sculpt edits are copied on save
        return [new for new, _source in made]

    def save(self, groom):
        """Validate, then store the graph and its layout on the groom: one undo step.

        The groom's viewNode (the Focus node) follows a renamed node and is
        cleared when the node it names is gone."""
        if self.unreadable:
            raise GraphError(self.unreadable)
        problems = self.validate()
        if problems:
            raise GraphError("; ".join(problems))
        from maya import cmds
        import groomlab
        self.complete_params()
        text = self.to_json()  # with the layout: it travels with the groom (groom files, the app)
        with groomlab.undo_chunk("graph edit"):
            groomlab.sync_controls(groom, self.to_dict())
            if cmds.getAttr(groom + ".graph") != text:  # unchanged graph: keep the evaluation cache
                cmds.setAttr(groom + ".graph", text, type="string")
            # Sculpt nodes' edits follow renames, copies and deletions, in the same undo step.
            from groomlab import sculpts
            copied, self.copied = self.copied, {}
            sculpts.follow_graph_edit(groom, renamed=self.renamed, copied={k: v for k, v in copied.items() if k in self.nodes},
                                      kept=set(self.nodes))
            self._sync_view_node(groom)

    def complete_params(self):
        """Write every param each node leaves out (its spec default) and each node's version, as
        saved grooms hold them (graph_io.h): a later change of a default never alters this groom."""
        edit = self._edit()
        with _core_errors():
            edit.complete_params()
        self._take(edit)

    def node_version(self, name):
        """(the behaviour version the node keeps, its operator's newest): an Upgrade is on offer
        while the first is lower (graph_edit.h; no version stored: the newest)."""
        node = self._node(name)
        newest = self.catalog.spec(node["type"]).get("version", 1)
        saved = node.get("version")
        return (saved if isinstance(saved, int) and not isinstance(saved, bool) else newest), newest

    def upgrade_node(self, name):
        """Raise an outdated node (its spec's "outdated") to its operator's newest behaviour.
        False when it is the newest already."""
        edit = self._edit()
        with _core_errors():
            done = edit.upgrade_node(name)
        self._take(edit)
        return done

    def library_info(self, name):
        """What a node added from the node library is (graph_edit.h: "id", "version", "latest",
        "updateAvailable", "embedded", "category", "author", "license", "icon"), read now (the
        library may have changed); None for another node."""
        node = self._node(name)
        definition = (node.get("params") or {}).get("definition")
        if node["type"] not in ("Compound", "Script") or not isinstance(definition, dict) or "glnode" not in definition:
            return None
        with _core_errors():
            spec = json.loads(groomlab_core.node_spec_json(json.dumps({"type": node["type"], "params": {"definition": definition}})))
        info = spec.get("library") or {}
        info["label"] = spec.get("label") or info.get("id")
        info["description"] = spec.get("description", "")
        return info

    def update_library_node(self, name, node_id=None):
        """A library node's copy replaced with the library's (graph_edit.h updateLibraryNode), or
        with library node `node_id`'s (a compound just saved as it): the exposed params it still has
        keep their values. False when it had it already."""
        edit = self._edit()
        with _core_errors():
            done = edit.update_library_node(name, self.groom_size, node_id or "")
        self._take(edit)
        return done

    def _sync_view_node(self, groom):
        from maya import cmds
        viewed = cmds.getAttr(groom + ".viewNode") or ""
        renamed, self.renamed = self.renamed, {}
        if not viewed:
            return
        wanted = renamed.get(viewed, viewed)
        if wanted not in self.nodes:
            wanted = ""
        if wanted != viewed:
            cmds.setAttr(groom + ".viewNode", wanted, type="string")

    def save_layout(self, groom):
        """Store only the layout (node positions, backdrops) in the groom's graph as it is stored:
        an edit of the model not saved yet stays out of it (the Graph Editor calls this with undo
        off, so moves make no undo steps). Nothing evaluates again: the graph is the same. Returns
        the graph's text now."""
        from maya import cmds
        text = cmds.getAttr(groom + ".graph") or "{}"
        if self.unreadable:
            return text  # the graph this GroomLab could not read stays as it is
        stored = json.loads(text) if text.strip() else {}
        if not stored:
            stored = json.loads(cmds.groomLabDefaultGraph())  # the default graph, laid out
        bare = without_layout(stored)
        bare["nodes"] = [dict([("name", n.get("name"))] + list(node_fields(n).items())) for n in bare["nodes"]]
        bare.setdefault("version", 1)
        if bare == without_layout(self.to_dict()):
            new = self.to_json()  # nothing else differs: the model's own text (what the editor compares)
        else:
            for n in stored.get("nodes", []):
                if n.get("name") in self.nodes and "ui" in self.nodes[n["name"]]:
                    n["ui"] = copy.deepcopy(self.nodes[n["name"]]["ui"])
            stored.pop("backdrops", None)
            if self.backdrops:
                stored["backdrops"] = copy.deepcopy(self.backdrops)
            stored.pop("pins", None)
            pins = self._pins_dict()
            if pins:
                stored["pins"] = pins
            stored.pop("portals", None)
            portals = self._portals_list()
            if portals:
                stored["portals"] = portals
            new = json.dumps(stored, separators=(",", ":"))
        if new != text:
            cmds.setAttr(groom + ".graph", new, type="string")
        return new

    # -- Nodes -------------------------------------------------------------

    def unique_name(self, base):
        """A new node's name (the core's, every host's): `base` with its first letter lowered,
        else base1, base2, ..."""
        names = groomlab_core.GraphEdit(json.dumps({"nodes": [{"name": n} for n in self.order]}))
        return names.unique_name(base)

    def add_node(self, op_type, name=None, params=None, pos=None):
        """Add a node with default params (overridden by `params`); returns its name.

        A node with a "seed" param that `params` leaves out gets a seed no other
        node of its type has (the default when it is free): two Scatters with one
        seed would make the same strands, two Flyaways pick the same strays. Without
        `pos` it goes in a free column right of the graph (the core's addNode). The defaults
        `params` leaves out that measure something fit `groom_size` (the groom's scalp, as
        presets fit it; the defaults are authored at 20). A type "node:<id>" adds a Compound
        holding the node library's copy of that node (graph_edit.h)."""
        if not op_type.startswith("node:"):
            self.catalog.spec(op_type)
        if name and name in self.nodes:
            raise GraphError("a node named %r exists" % name)
        edit = self._edit()
        with _core_errors():
            made = edit.add_node(op_type, name or "", json.dumps(params or {}),
                                 None if pos is None else float(pos[0]), None if pos is None else float(pos[1]),
                                 self.groom_size)
        self._take(edit)
        return made

    def remove_node(self, name, keep_flow=False):
        """The node gone with its links. keep_flow (the Graph Editor's Delete, issue #80): as Detach
        first, what fed it feeding what it fed (GraphEdit.remove_node_keeping_flow)."""
        self._node(name)
        edit = self._edit()
        with _core_errors():
            if keep_flow:
                edit.remove_node_keeping_flow(name)
            else:
                edit.remove_node(name)
        self._take(edit)
        if keep_flow:
            self.prune_pins()  # the reroute points of links that went
        self.renamed = {k: v for k, v in self.renamed.items() if v != name and not v.startswith(name + "/")}

    def rename_node(self, old, new):
        """Rename a node: its links and the output follow (the core's GraphEdit.rename_node, every
        host's: a name that is empty or taken refused)."""
        self._node(old)
        edit = self._edit()
        with _core_errors():
            edit.rename_node(old, new)
        self._take(edit)
        if self._pins:  # the routes from and to it follow (the core's renamePinNode)
            pins = self._pin_edit()
            pins.rename_node(old, new)
            self._take_pins(pins)
        if self._portals:  # the portals naming it follow (the core's renamePortalNode)
            self._portal_change(lambda p: p.rename_node(old, new))
        self._moved([(old, new)])

    def _moved(self, moves):
        """Fold node moves ((old, new) paths in order: a rename, the core's GraphEdit.moved_nodes for
        Group and Ungroup) into `renamed`; a compound's move takes the paths inside it along."""
        for old, new in moves:
            followed = False
            for k, v in list(self.renamed.items()):
                if v == old or v.startswith(old + "/"):
                    self.renamed[k] = new + v[len(old):]
                    followed = followed or v == old
            if not followed:
                self.renamed.setdefault(old, new)

    def node_type(self, name):
        return self._node(name)["type"]

    def of_type(self, node_type):
        """The nodes of a type, in the graph's order."""
        return [n for n in self.order if self.node_type(n) == node_type]

    def first_of(self, node_type):
        """The first node of a type in the graph's order, None when it has none."""
        return next((n for n in self.order if self.node_type(n) == node_type), None)

    _specs = {}  # node_spec's answers (the core's text) by what they depend on, kept per process

    def node_spec(self, name):
        """Ports and params of a node: its operator's, except that a Compound's
        come from its definition (inputs, output, exposed params) and an
        Input's output type from its params. "outputs" lists every output port
        (the primary, "output", first). The core decides it (graph_edit.h), as
        it does for the app's node graph and for connect's checks."""
        node = self._node(name)
        params = node.get("params") or {}
        exposed = tuple(node.get("paramInputs") or ())
        passes = tuple(node.get("passThrough") or ())  # its pass-through outputs (thru_<input>)
        if node["type"] in ("Compound", "Script"):  # ports and params from its definition
            key = (node["type"], json.dumps(params.get("definition") or {}, sort_keys=True), exposed)
            bare = {"type": node["type"], "params": {"definition": params.get("definition") or {}}}
        elif node["type"] == "Input":
            key = (node["type"], params.get("type", "Strands"), exposed)
            bare = {"type": node["type"], "params": {"type": params.get("type", "Strands")}}
        elif node["type"] in VALUE_SETS:  # its output's kind is its param's (a Value's kind)
            own = {p: params[p] for p in VALUE_SETS[node["type"]] if p in params}
            key = (node["type"], json.dumps(own, sort_keys=True), exposed)
            bare = {"type": node["type"], "params": own}
        else:
            self.catalog.spec(node["type"])  # the catalog's error for an unknown type
            key = (node["type"], None, exposed)
            bare = {"type": node["type"]}
        key = key + (passes,)
        text = GraphModel._specs.get(key)
        if text is None:
            if exposed:
                bare["paramInputs"] = list(exposed)
            if passes:
                bare["passThrough"] = list(passes)
            with _core_errors():
                text = groomlab_core.node_spec_json(json.dumps(bare))
            if len(GraphModel._specs) > 2000:
                GraphModel._specs.clear()
            GraphModel._specs[key] = text
        return json.loads(text)

    # -- Exposed params (PG-7): a param driven by another node's Field output

    def param_inputs(self, name):
        return list(self._node(name).get("paramInputs", []))

    def expose_param(self, name, param):
        """Give a param an input port: a mappable param a Field port (Field, FieldNoise, ... drive it),
        a color param that takes a color field (colorField: a Map's rgb output colors it) one too, a
        ramp param a Curve port (a Curve node drives it), any other a Value port (a Value node, or an
        Expression set to give a value, sets it for the whole node). The core's (graph_controls.h
        exposeParam), every host's."""
        self._node(name)
        edit = self._edit()
        with _core_errors():
            edit.expose_param(name, param)
        self._take(edit)

    def unexpose_param(self, name, param):
        """Remove a param's input port (and what was connected to it); the param keeps its own value."""
        self._node(name)
        edit = self._edit()
        with _core_errors():  # the core's (graph_controls.h unexposeParam), every host's
            if edit.unexpose_param(name, param):
                self._take(edit)

    # -- Keyed params: a param linked to the groom's control attribute(s), set from them per frame

    def control_of(self, name, param):
        """The control(s) keying a param (a name, or three for a vector), or None."""
        return copy.deepcopy(self._node(name).get("paramControls", {}).get(param))

    def keyable(self, name, param):
        """None when the param can be keyed in place, else why not."""
        if self.param_spec(name, param) is None:
            return "%s has no param %r" % (self.node_type(name), param)
        with _core_errors():  # the core's reasons (graph_controls.h notKeyable), every host's
            why = self._query().not_keyable(name, param)
        return why or None

    def link_control(self, name, param, control):
        """Key a param from the groom's control(s): `control` a name, or [x, y, z] names for a vector."""
        why = self.keyable(name, param)
        if why:
            raise GraphError("%s.%s cannot be keyed: %s" % (name, param, why))
        vector = self.param_spec(name, param)["kind"] == "vec3"
        if vector != isinstance(control, (list, tuple)) or (vector and len(control) != 3):
            raise GraphError("%s.%s is keyed by %s" % (name, param, "three controls" if vector else "one control"))
        self._node(name).setdefault("paramControls", {})[param] = list(control) if vector else control

    def unlink_control(self, name, param):
        """The param back to its own value (in "params")."""
        node = self._node(name)
        links = node.get("paramControls", {})
        links.pop(param, None)
        if not links:
            node.pop("paramControls", None)

    def driver_of(self, name, param):
        """The node driving an exposed param, or None."""
        return self.source_of(name, param) if param in self.param_inputs(name) else None

    def input_spec(self, name, input_name):
        for port in self.node_spec(name)["inputs"]:
            if port["name"] == input_name:
                return port
        raise GraphError("%s has no input %r" % (name, input_name))

    def output_names(self, name):
        """The node's output ports by name, the primary first (one for most nodes)."""
        return [p["name"] for p in self.node_spec(name)["outputs"]]

    def output_spec(self, name, output=None):
        """The node's output port `output` (a name; None: the first). GraphError when it has none."""
        outputs = self.node_spec(name)["outputs"]
        if output is None:
            return outputs[0]
        for port in outputs:
            if port["name"] == output:
                return port
        raise GraphError("%s has no output %r (%s)" % (name, output, ", ".join(p["name"] for p in outputs)))

    def output_type(self, name, output=None, _edit=None):
        """A node's output type as connected (its output `output`, None: the first): a Curves node
        (strands or guides) gives what feeds it (the core's, as connect checks it)."""
        port = self.output_spec(name, output)
        out = port["type"]
        if out != "Curves" and not port.get("passThrough"):  # (a pass-through gives what feeds its input)
            return out
        with _core_errors():
            return (_edit or self._query()).output_type(name, output or "")

    def param_spec(self, name, param):
        return next((p for p in self.node_spec(name)["params"] if p["name"] == param), None)

    # -- Pins (layout: links drawn through reroute points) ----------------------

    # The rules are the core's (graph_pins.h, as the app's and Blender's): which routes and pins hold
    # after a graph edit, their order, the next pin's id, joining. The model keeps the graph's
    # "pins" as the core wrote it.

    def _pin_edit(self):
        """The graph's links and pins as the core reads and edits them (groomlab_core.GraphPins)."""
        return groomlab_core.GraphPins(json.dumps({"connections": self.connections, "pins": self._pins or {}}))

    def _take_pins(self, edit):
        text = edit.pins_json()
        self._pins = json.loads(text) if text else None

    def _load_pins(self, data):
        self._pins = None
        if isinstance(data, dict) and data.get("routes"):
            self._pins = data
            self.prune_pins()

    def _source(self, to, input_name):
        """(from, output) of the link into to.input (output None: its first), or None."""
        for c in self.connections:
            if c["to"] == to and c["input"] == input_name:
                return c["from"], c.get("output")
        return None

    @property
    def pins(self):
        """{pin id: [x, y]}."""
        return {p["id"]: [p["x"], p["y"]] for p in (self._pins or {}).get("points", [])}

    @property
    def routes(self):
        """{(to, input): {"from", "output" (None: the source's first), "via": [pin ids, source first]}}."""
        return {(r["to"], r["input"]): {"from": r["from"], "output": r.get("output"), "via": list(r["via"])}
                for r in (self._pins or {}).get("routes", [])}

    def prune_pins(self):
        """Drops what no longer holds: routes whose link is gone or comes from elsewhere now, pins
        they name that are gone, and pins no route passes."""
        if self._pins:
            pins = self._pin_edit()
            pins.prune()
            self._take_pins(pins)

    def _pins_dict(self):
        self.prune_pins()
        return copy.deepcopy(self._pins) if self._pins else None

    def route(self, to, input_name):
        """The pins the link into to.input runs through, source first (ids; [] for none)."""
        return list(self._pin_edit().route(to, input_name)) if self._pins else []

    def pin_source(self, pin):
        """(from, output) of the signal a pin carries (output None: the source's first), or None."""
        source = self._pin_edit().source(pin) if self._pins else None
        return (source[0], source[1] or None) if source else None

    def pin_links(self, pin):
        """The links (to, input) running through a pin, and each one's place of it in its route."""
        return [(tuple(key), index) for key, index in self._pin_edit().links(pin)] if self._pins else []

    def route_to(self, pin):
        """The pins from the source up to and including `pin` (a link drawn out of it runs through
        these): the first of its links' routes, [] for none."""
        return list(self._pin_edit().route_to(pin)) if self._pins else []

    def set_route(self, to, input_name, via):
        """The link into to.input runs through `via` (pin ids), from its source as it is now."""
        pins = self._pin_edit()
        if not pins.set_route(to, input_name, list(via or [])):
            raise GraphError("%s.%s has no link to route" % (to, input_name))
        self._take_pins(pins)

    def add_pin(self, to, input_name, x, y, index=None):
        """A new pin at (x, y) on the link into to.input, at `index` in its route (None: last, by
        the destination). Returns its id."""
        pins = self._pin_edit()
        pin = pins.add(to, input_name, float(x), float(y), -1 if index is None else max(0, int(index)))
        if not pin:
            raise GraphError("%s.%s has no link to pin" % (to, input_name))
        self._take_pins(pins)
        return pin

    def move_pin(self, pin, x, y):
        if self._pins:
            pins = self._pin_edit()
            if pins.move(pin, float(x), float(y)):
                self._take_pins(pins)

    def pin_position(self, pin):
        return tuple(self.pins[pin])

    def remove_pin(self, pin):
        """Takes the pin out of every route through it (those links straighten there)."""
        if self._pins:
            pins = self._pin_edit()
            if pins.remove(pin):
                self._take_pins(pins)

    def merge_pins(self, keep, drop):
        """Routes through `drop` run through `keep` instead (a pin dropped on another): only pins
        carrying the same signal merge. Returns whether they did."""
        if not self._pins:
            return False
        pins = self._pin_edit()
        if not pins.merge(keep, drop):
            return False
        self._take_pins(pins)
        return True

    # -- Portals (layout: links teleported through named dots) -------------------

    # The rules are the core's (graph_portals.h, as the app's): which links stay in a portal after a
    # graph edit, unique names, renames. The model keeps the graph's "portals" as the core wrote it.

    def _portal_edit(self):
        """The graph's links and portals as the core reads and edits them (groomlab_core.GraphPortals)."""
        return groomlab_core.GraphPortals(json.dumps({"nodes": [{"name": n} for n in self.order],
                                                      "connections": self.connections, "portals": self._portals or []}))

    def _take_portals(self, edit):
        text = edit.portals_json()
        self._portals = json.loads(text) if text else None

    def _portals_list(self):
        if self._portals:
            portals = self._portal_edit()
            portals.prune()
            self._take_portals(portals)
        return copy.deepcopy(self._portals) if self._portals else None

    @property
    def portals(self):
        """The portals as the graph keeps them: [{"name", "from", "output"?, "x", "y", "exits": [...]}]."""
        return self._portals_list() or []

    def portal_of_link(self, to, input_name):
        """(portal, exit) the link into to.input comes out of, or None."""
        return self._portal_edit().portal_of_link(to, input_name) if self._portals else None

    def _portal_change(self, fn):
        portals = self._portal_edit()
        result = fn(portals)
        self._take_portals(portals)
        return result

    def teleport(self, to, input_name, exit_at, entry_at):
        """The link into to.input drawn through its source's portal, out of a new exit: (portal, exit)."""
        made = self._portal_change(lambda p: p.teleport(to, input_name, float(exit_at[0]), float(exit_at[1]),
                                                        float(entry_at[0]), float(entry_at[1])))
        if made is None:
            raise GraphError("%s.%s has no link to teleport" % (to, input_name))
        return tuple(made)

    def add_portal_exit(self, portal, x, y):
        exit_index = self._portal_change(lambda p: p.add_exit(portal, float(x), float(y)))
        if exit_index < 0:
            raise GraphError("no portal %r" % portal)
        return exit_index

    def add_portal_link(self, portal, exit_index, to, input_name):
        return self._portal_change(lambda p: p.add_link(portal, int(exit_index), to, input_name))

    def move_portal(self, portal, exit_index, x, y):
        """A portal's entry (exit_index None) or one of its exits moved."""
        if exit_index is None:
            self._portal_change(lambda p: p.move(portal, float(x), float(y)))
        else:
            self._portal_change(lambda p: p.move_exit(portal, int(exit_index), float(x), float(y)))

    def remove_portal(self, portal, exit_index=None):
        """A portal gone (its links drawn as wires again), or one exit of it."""
        if exit_index is None:
            return self._portal_change(lambda p: p.remove(portal))
        return self._portal_change(lambda p: p.remove_exit(portal, int(exit_index)))

    def rename_portal(self, portal, name):
        made = self._portal_change(lambda p: p.rename(portal, name))
        if not made:
            raise GraphError("no portal %r" % portal)
        return made

    # -- Dots (graph_portals.h: every portal is a dot; free ones from the add-node search) ----------

    def add_dot(self, x, y, src=None, output=None):
        """A free dot at (x, y) (fed from src's `output` when given: an exit of that output's dot
        when it has one). Returns its portal's name."""
        name = self._portal_change(lambda p: p.add_dot(float(x), float(y), ""))
        if src:
            portals = self._portal_edit()
            if not portals.set_source(name, src, output or ""):
                into = portals.portal_of(src, output or "")
                if portals.join(name, into) < 0:
                    raise GraphError("%s's output has a dot already" % src)
                name = into
            self._take_portals(portals)
        return name

    def portal_fed(self, portal):
        return bool(self._portal_edit().fed(portal)) if self._portals else False

    def _relink(self, links, src, output):
        """The links (to, input) read src's `output` (the core's checks; nothing changes on a refusal)."""
        edit = self._edit()
        with _core_errors():
            for to, input_name in links:
                edit.connect_by_hand(src, to, input_name, output or "")
        self._take(edit)

    def feed_dot(self, portal, src, output=None):
        """A dot fed from src's `output`: its links (its entry's and exits') read it now."""
        portals = self._portal_edit()
        links = [tuple(l) for l in portals.links(portal)]
        self._relink(links, src, output)
        portals = self._portal_edit()
        if not portals.set_source(portal, src, output or ""):
            raise GraphError("%s's output has a dot already" % src)
        self._take_portals(portals)

    def set_dot_input(self, portal, exit_index, into):
        """A dot's Input Portal: `into` (another dot's name) or None (a free dot). exit_index None:
        the dot's entry. Its links read `into`'s source first."""
        dot = portal
        if exit_index is not None:
            dot = self._portal_change(lambda p: p.leave(portal, int(exit_index)))
            if not dot:
                raise GraphError("no such dot")
        if not into:
            return
        source = next((p for p in self.portals if p["name"] == into), None)
        if source is None or not source.get("from"):
            raise GraphError("%s has nothing coming into it yet" % into)
        self._relink([tuple(l) for l in self._portal_edit().links(dot)], source["from"], source.get("output"))
        if self._portal_change(lambda p: p.join(dot, into)) < 0:
            raise GraphError("cannot take %s's signal" % into)

    def detach_group(self, names):
        """Several nodes out of the flow together (a shake): the links among them kept (the core's)."""
        edit = self._edit()
        with _core_errors():
            done = edit.detach_group(list(names))
        if done["changed"]:
            self._take(edit)
            self.prune_pins()
        return done

    def insert_nodes_on_link(self, src, dst, input_name, nodes, output=None):
        """A chain dropped on a wire as one: src feeds its entry, dst reads its exit (the core's)."""
        edit = self._edit()
        with _core_errors():
            edit.insert_nodes_on_link(src, dst, input_name, list(nodes), output or "")
        self._take(edit)

    # -- Duplicate, output links, pass-through (the core's, every host's) ----------

    def duplicate_nodes(self, names, offset=(40.0, 60.0)):
        """Ctrl+D: copies beside the nodes, each reading what its node reads (the core's
        GraphEdit.duplicate_nodes, the app's too). Returns the new names."""
        edit = self._edit()
        with _core_errors():
            made = edit.duplicate_nodes(list(names), float(offset[0]), float(offset[1]))
        self._take(edit)
        for new, source in made:
            self.copied[new] = source
        return [new for new, _source in made]

    def output_links(self, src, output=None):
        """The links reading src's output (None: its first): [(node, input)]."""
        return [tuple(l) for l in self._query().output_links(src, output or "")]

    def move_output_links(self, src, output, new_src, new_output=None):
        """Every link reading src's output reads new_src's new_output instead where it fits (a
        bundle of links dropped on another output). Returns the (node, input) moved."""
        edit = self._edit()
        with _core_errors():
            moved = edit.move_output_links(src, output or "", new_src, new_output or "")
        self._take(edit)
        return [tuple(m) for m in moved]

    def pass_through(self, name):
        """The inputs the node hands on as outputs (thru_<input>)."""
        return list(self._node(name).get("passThrough") or [])

    def set_pass_through(self, name, input_name, on):
        """An input's pass-through output on or off (off: what read it reads nothing)."""
        edit = self._edit()
        with _core_errors():
            changed = edit.set_pass_through(name, input_name, bool(on))
        if changed:
            self._take(edit)
        return changed

    def set_position(self, name, x, y):
        self._node(name)["ui"] = {"x": float(x), "y": float(y)}

    def position(self, name):
        ui = self._node(name).get("ui", {})
        return ui.get("x", 0.0), ui.get("y", 0.0)

    # -- Params ------------------------------------------------------------

    def param(self, name, param):
        """The node's value for `param`, or the operator default."""
        node = self._node(name)
        if param in node["params"]:
            return node["params"][param]
        spec = self.param_spec(name, param)
        if spec is None:
            raise GraphError("%s has no param %r" % (node["type"], param))
        return copy.deepcopy(spec["default"])

    def set_param(self, name, param, value):
        node = self._node(name)
        if self.param_spec(name, param) is None:
            raise GraphError("%s has no param %r" % (node["type"], param))
        node["params"][param] = copy.deepcopy(value)

    # -- Connections -------------------------------------------------------

    def source_of(self, dst, input_name):
        for c in self.connections:
            if c["to"] == dst and c["input"] == input_name:
                return c["from"]
        return None

    def output_of(self, dst, input_name):
        """Which output of its source feeds dst's input (a name; the first when the link names
        none), or None when the input is not connected."""
        for c in self.connections:
            if c["to"] == dst and c["input"] == input_name:
                return c.get("output") or self.output_names(c["from"])[0]
        return None

    def connect(self, src, dst, input_name, output=None, by_hand=False):
        """Connect src's output `output` (a name; None: its first) to dst's input, replacing what
        fed that input. The link records the output only when it is not the first. The core
        checks it (graph_edit.h, the app's node graph's too): the types must fit, no cycle.
        by_hand: a link the user drew (GraphEdit::connectByHand): the node is set to use it where
        a dropdown says whether it does (a Clump of centersFrom pick fed centers: inputPoints)."""
        edit = self._edit()
        with _core_errors():
            if by_hand:
                if edit.connect_by_hand(src, dst, input_name, output or ""):
                    self._take(edit)
                    return
            else:
                edit.connect(src, dst, input_name, output or "")
        self.connections = json.loads(edit.to_json()).get("connections", [])

    def disconnect(self, dst, input_name):
        """Remove what feeds dst's input (the core's disconnect)."""
        edit = self._edit()
        if edit.disconnect(dst, input_name):
            self.connections = json.loads(edit.to_json()).get("connections", [])

    def clump_color_source(self, name):
        """Clump Colors' source for this Clump: "<channel>@<node>" ("" when it writes no channel)."""
        return self._query().clump_color_source(name) if name in self.nodes else ""

    def clump_colored(self, name, source):
        """Whether a clump color source shows this Clump's clumps (the core's GraphEdit.clump_colored:
        its own source, or the bare channel of grooms from before when it is the last Clump writing it)."""
        return name in self.nodes and self._query().clump_colored(name, source or "")

    def viewable(self, name):
        """Whether View Up to Here takes the node: an output giving strands as connected (the
        core's GraphEdit.viewable, every host's eye button and V key)."""
        return name in self.nodes and self._query().viewable(name)

    def bakeable(self, name):
        """Whether Bake Node to Guides takes the node (the core's GraphEdit.bakeable)."""
        return name in self.nodes and self._query().bakeable(name)

    def value_kind_for_input(self, src, dst, input_name):
        """The kind a Value node no link leaves yet takes when drawn into dst's param input (the
        core's GraphEdit.value_kind_for_input; a link drawn by hand sets it), else None."""
        if src not in self.nodes or dst not in self.nodes:
            return None
        return self._query().value_kind_for_input(src, dst, input_name) or None

    def braid_tubes_offered(self, name):
        """Whether the node has Show Tubes: a Braid (its section tubes), or a node giving guides (the
        tubes they carry; the core's GraphEdit.braid_tubes_offered)."""
        return name in self.nodes and self._query().braid_tubes_offered(name)

    def braid_tubes_shown(self, name, shown):
        """Whether a node's Show Tubes is lit: a node with the button named in `shown` (the groom's
        braidTubes: names, one a line; the core's GraphEdit.braid_tubes_shown)."""
        return name in self.nodes and self._query().braid_tubes_shown(name, shown or "")

    def braid_tubes_toggled(self, name, shown):
        """`shown` after a click on the node's Show Tubes: the node added or taken out (the core's)."""
        return self._query().braid_tubes_toggled(name, shown or "")

    def braid_tubes_kept(self, shown):
        """`shown` without the names of nodes that have no Show Tubes now (a node deleted)."""
        return self._query().braid_tubes_kept(shown or "")

    def upstream(self, name):
        """All nodes feeding `name`, directly or not."""
        if name not in self.nodes:
            return set()
        with _core_errors():
            return set(self._query().upstream(name))

    def channels(self, guide_maps=()):
        """The attributes (strand channels) per node, as the graph is set, without evaluating it
        (groomlab_core.graph_channels): {node: {"incoming": [{"name", "node"}] (reaching it, each
        with the node that wrote it last), "writes": [{"name", "param", "extends"}], "overwrites":
        [{"name", "param", "node"}], "reads": [{"param", "name", "reaches", "node", "optional"}]}}.
        guide_maps: the maps painted on the guides (Interpolate turns them into attributes)."""
        with _core_errors():
            return groomlab_core.graph_channels(json.dumps(self.to_dict()), list(guide_maps))

    def downstream_of(self, src):
        return [(c["to"], c["input"]) for c in self.connections if c["from"] == src]

    # A node into the flow, every host's (graph_inserts.h): the Blender helper's edit_graph ops and
    # the app's node graph make the same edits.

    def _insert(self, how, *args):
        """One of the core's inserts (GraphEdit.insert_*) made on the model; a GraphError leaves it as it was."""
        edit = self._edit()
        with _core_errors():
            getattr(edit, how)(*args)
        self._take(edit)

    def insert_between(self, src, dst, node, node_input=None):
        """Route src -> node -> dst in place of every src -> dst connection (node reading the
        output of src the first of them read, on `node_input`; None: its first input)."""
        self._insert("insert_between", src, dst, node, node_input or "")

    def insert_on_link(self, src, dst, input_name, node, output=None):
        """Route the one link src (its output `output`; None: its first) -> dst.input_name through
        `node`: node reads src on its first input that takes src's output, dst reads node. A
        GraphError (nothing changed) when no input of node takes it or node can't feed dst."""
        self._insert("insert_on_link", src, dst, input_name, node, output or "")

    def insert_after(self, anchor, node, input_name=None):
        """Put `node` right after `anchor` in its chain (anchor -> node -> anchor's first
        consumer), under the anchor; with no consumer, just after it (on `input_name`; None: its
        first input)."""
        self._insert("insert_after", anchor, node, input_name or "")

    def insert_before(self, anchor, node, input_name="strands"):
        """Put `node` between `anchor`'s `input_name` source and `anchor` (e.g. before the output)."""
        self._insert("insert_before", anchor, node, input_name, "")

    def set_output(self, name):
        self._node(name)
        self.output = name

    # -- Compounds (PG-6) ------------------------------------------------

    def collapse(self, names, compound=None):
        """Replace nodes with one Compound node holding them; returns its name (the core's
        GraphEdit.collapse, the app's Group too).

        Connections from outside become the compound's inputs (one per
        source node and output); exactly one of the nodes may feed nodes
        outside, from its first output, and it becomes the compound's output
        (a compound has one output). Inner nodes keep every field (params,
        exposed params, bypass, position) so Ungroup gives them back. A group
        that cannot be made raises and leaves the graph as it was.
        """
        edit = self._edit()
        with _core_errors():
            name = edit.collapse(list(names), compound or "")
        self._take(edit)
        self._moved(edit.moved_nodes())  # each node into the compound: its Sculpt edits follow on save
        return name

    def expose(self, compound, name, target):
        """Show an inner param ("node.param") as the compound param `name` ("": named after the param,
        or node + Param when the compound has that name: the core's graph_controls.h compoundExpose,
        every host's). Returns the name."""
        self._node(compound)
        edit = self._edit()
        with _core_errors():
            made = edit.compound_expose(compound, name or "", target)
        self._take(edit)
        return made

    def unexpose(self, compound, target):
        """Stop showing inner param `target` ("node.param") on the compound (its value inside stays
        what the compound set last; the core's compoundUnexpose, every host's)."""
        self._node(compound)
        edit = self._edit()
        with _core_errors():
            if edit.compound_unexpose(compound, target):
                self._take(edit)

    def detach(self, name, drop=None):
        """Detach (Ctrl+Shift+X; the core's GraphEdit.detach): the node taken out of the flow as if it
        were not there, what fed it feeding what it fed, the node kept, unlinked, `drop` below the
        new link (None: the core's, every host's). Returns the core's {"source", "relinked", "changed"}."""
        self._node(name)
        edit = self._edit()
        with _core_errors():
            done = edit.detach(name) if drop is None else edit.detach(name, float(drop))
        if done["changed"]:
            self._take(edit)
            self.prune_pins()  # the reroute points of links that went
        return done

    def expand(self, compound):
        """Replace a Compound node with the nodes it holds (exposed values applied, every field
        kept); returns their names (the core's GraphEdit.expand, the app's Ungroup too). A
        definition that cannot be expanded (an unknown type, a link that fits nothing) raises and
        leaves the graph as it was."""
        node = self._node(compound)
        if node["type"] != "Compound":
            raise GraphError("%s is not a Compound" % compound)
        edit = self._edit()
        with _core_errors():
            made = edit.expand(compound)
        self._take(edit)
        self._moved(edit.moved_nodes())  # each node out of the compound: its Sculpt edits follow on save
        return list(made)

    # -- Checks and layout -------------------------------------------------

    def feeding_output(self):
        """The nodes the output is made from (the output and everything upstream of it, bypassed
        nodes included): the ones the groom evaluates (the core's GraphEdit.feeding_output)."""
        return set(self._query().feeding_output())

    def validate(self):
        """Problems that would stop the groom from evaluating (empty list: fine): only those of the
        nodes the output is made from. A node off to the side (its output unplugged, say) never
        evaluates, so its problems are warnings (unused_problems), never a reason to stop applying
        the graph. The core's (graph_edit.h GraphEdit::problems), every host's: an input left open,
        no output node, a link reading an output its source lacks or whose types no longer fit, a
        map with no name."""
        with _core_errors():
            return list(self._query().problems(True))

    def unused_problems(self):
        """The problems of nodes the output isn't made from (they evaluate only when viewed)."""
        with _core_errors():
            return list(self._query().problems(False))

    def warnings(self):
        """Things that evaluate but are likely mistakes (the editor shows them; save does not stop).

        Two Scatters with one seed make the same strands (same IDs and roots; a
        Merge renumbers the second's IDs but the roots still coincide); two
        Instances or Flyaways with one seed pick the same strands (the core's)."""
        with _core_errors():
            return list(self._query().seed_warnings())

    def auto_layout(self):
        """Columns by depth from the sources, rows in insertion order (the core's layoutPositions,
        graph_inserts.h: Blender's and the app's layout too)."""
        for name, (x, y) in self._query().layout_positions(LAYOUT_DX, LAYOUT_DY).items():
            self.set_position(name, x, y)

    def _node(self, name):
        try:
            return self.nodes[name]
        except KeyError:
            raise GraphError("no node named %r" % name)


class ScopedModel(GraphModel):
    """The graph inside a compound (the Graph Editor's double-click on a Compound, its path bar and
    U to come back out): edited as a graph of its own, and written back into the graph holding it
    whenever it is saved (the core's with_compound_graph: the compound's inputs follow its Input
    nodes, its exposed params follow renames inside). `outer` is the model of the graph holding the
    compound (the groom's, or another ScopedModel for a compound inside a compound); `name` the
    compound's name there."""

    def __init__(self, outer, name):
        self.outer = outer
        self.name = name
        with _core_errors():
            inner = json.loads(groomlab_core.compound_graph_json(json.dumps(outer.to_dict()), name))
        super(ScopedModel, self).__init__(outer.catalog, inner)
        self.groom_size = outer.groom_size
        if self.exposed is None:
            self.exposed = {}
        self.ensure_layout()

    @property
    def path(self):
        """The compound's path from the groom's graph ("outer/inner")."""
        return (self.outer.path + "/" if isinstance(self.outer, ScopedModel) else "") + self.name

    def whole(self):
        """The graph holding the compound, with this graph in it (a dict)."""
        with _core_errors():
            return json.loads(groomlab_core.with_compound_graph(json.dumps(self.outer.to_dict()), self.name,
                                                                json.dumps(self.to_dict())))

    def write_back(self):
        """This graph into the outer model (and so on up)."""
        self.outer.load_dict(self.whole())
        if isinstance(self.outer, ScopedModel):
            self.outer.write_back()

    def validate(self):
        """Its own problems, then the graph holding it's (an output inside, inputs that fit)."""
        problems = super(ScopedModel, self).validate()
        try:
            whole = self.whole()
        except GraphError as e:
            return problems + [str(e)]
        with _core_errors():
            return problems + [p for p in groomlab_core.GraphEdit(json.dumps(whole)).problems(True) if p not in problems]

    def save(self, groom):
        """Written back into the whole graph, which is stored on the groom (one undo step)."""
        self.write_back()
        top = self.outer
        while isinstance(top, ScopedModel):
            top = top.outer
        top.save(groom)

    def save_layout(self, groom):
        self.write_back()
        top = self.outer
        while isinstance(top, ScopedModel):
            top = top.outer
        return top.save_layout(groom)


def _top(model):
    """The groom's graph model a (scoped) model belongs to, with the scoped graphs written back."""
    if isinstance(model, ScopedModel):
        model.write_back()
        while isinstance(model, ScopedModel):
            model = model.outer
    return model


GraphModel.top = lambda self: _top(self)
