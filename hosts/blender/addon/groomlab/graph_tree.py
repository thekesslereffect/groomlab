# SPDX-License-Identifier: GPL-3.0-or-later
"""The Graph Editor's data: each groom's graph as a Blender node tree ("GroomLab Graph"), kept in
step with the helper's graph both ways, as Maya's GroomLab Graph Editor keeps its scene.

- One node class per operator type, made from the helper's operator_specs (ports typed by port
  type, in Maya's wire colors; the params as the node's own properties with their ranges, units
  and descriptions), registered when the helper first answers. A Compound's params are its own
  (its exposed params), in a list on the node.
- Edits in the node editor (add, delete, link, unlink, rename, mute) go to the helper as
  edit_graph ops when Blender updates the tree (NodeTree.update); a param edited on a node is a
  set_param. The groom then evaluates.
- A change of the helper's graph (a preset, an edit from the 3D view's panel, undo, a .groom
  opened) rebuilds the tree from the graph, node positions kept.
- Positions live in the graph as Maya keeps them: each node's "ui": {"x", "y"} (graph_io.h; Qt's
  y down, so Blender's location is (x, -y)), so a .groom saved in Blender lays out the same in
  Maya's editor and the app's, and the other way round.
- Blender's frames are the graph's backdrops (Maya's: a title, a rectangle, a color), kept with the
  layout; the nodes inside one are its children.
- Blender's reroutes are the graph's pins (Maya's, Blueprints' reroute nodes: {"points", "routes"},
  layout only). A link through reroutes is the plain connection from its source to its input, and
  the reroutes it passes are its route; one reroute feeding several inputs is a bundle (one pin in
  several routes). A reroute that routes no link yet (no input, or no outputs) is the user's: it
  stays as left, the links into it too, and is no pin.
- A node dropped on a link (Blender splices it in) is the graph's insert_on_link."""

import hashlib
import json
import re

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)

from . import graph_ui, groom, process, props, units
from .helper_client import HelperError

TREE = "GroomLabGraphTree"
NODE_PREFIX = "GroomLabNode_"
NODE_WIDTH = 200
LAYOUT_DX, LAYOUT_DY = 240, 140  # the core's column and row for a new node (graph_edit.cpp), as Maya's

# Maya's Graph Editor (hosts/maya/scripts/groomlab/ui/graph_editor.py): port and category colors and
# the plumbing params from the shared UI definitions (engine/ui/graph_ui.json; these defaults when
# the files are missing), the Add menu's order, the spec categories' names.
TYPE_COLORS = graph_ui.port_colors({"Strands": (232, 168, 92), "Guides": (92, 196, 232), "Mesh": (140, 206, 110),
                                    "Field": (214, 122, 214), "Curve": (245, 215, 90), "Curves": (206, 210, 218),
                                    "Value": (120, 214, 190)})
# The Add menu's categories in the core's order (node_catalog.h, each spec's "categoryTitle"): Blender
# registers its node categories before the helper runs, so their titles are listed here; which node
# goes where, its label and its order come from the specs. A test checks this list against them.
CATEGORY_TITLES = ["Sources", "Grow", "Shape", "Deform", "Collide", "Look", "Motion", "Feathers", "Color", "Convert",
                   "Fields & Curves", "Utility", "Games"]
HIDDEN_TYPES = {"Compound", "Input", "Script"}  # made by grouping (Maya), a preset or the node library, not from the Add menu
# When a node runs (its "evaluation"; always when absent): the labels of the other two.
EVALUATION_LABELS = {"viewportOnly": "Viewport only", "renderOnly": "Render only"}
CATEGORY_COLORS = graph_ui.category_colors({
    "Sources": (72, 190, 170), "Grow": (79, 209, 197), "Color": (255, 122, 182), "Shape": (90, 150, 240),
    "Deform": (110, 170, 250), "Collide": (100, 180, 200), "Look": (170, 120, 240), "Fields & Curves": (214, 122, 214),
    "Utility": (140, 148, 158), "Output": (245, 176, 66), "Compound": (110, 200, 120), "Games": (90, 200, 230),
    "Convert": (240, 140, 95), "Motion": (120, 200, 255), "Feathers": (226, 186, 90)})
# Params naming the host's inputs: plumbing, hidden from the panel unless changed (as in Maya).
PLUMBING = graph_ui.plumbing({("Scalp", "input"), ("Guides", "input"), ("Guides", "hierarchy")})
# Nodes whose `path` names a file (graph_ui.json's filePathFilters): Blender's file browser button.
FILE_PATH_FILTERS = graph_ui.get("graph/filePathFilters", {"Reference": "", "ImportCurves": ""})
# A backdrop's color when a frame has none of its own (Maya's Add Backdrop: Blue).
BACKDROP_COLOR = [70, 90, 120]
ADVANCED = "Advanced"
NODE_BG = (43, 45, 50)  # Maya's node body: a node's custom color is it with a wash of its category's

RAMPS = ".GroomLab Ramps"  # a hidden shader node group: one Float Curve node per ramp param (the curve widget)


class _State(object):
    building = 0      # the add-on is writing the tree: its updates are not the user's edits
    in_update = 0     # inside NodeTree.update: rebuilds wait for flush()
    undoing = False


_state = _State()
_pending = set()      # scalp uids whose tree rebuilds at the next flush()
_replaced = set()     # scalp uids whose graph was replaced whole (a preset, a file): positions from it
_node_classes = {}    # operator type -> its node class
_classes_key = [None]
_messages = {}        # scalp uid -> the last edit the helper refused (the editor's status line)


class building(object):
    def __enter__(self):
        _state.building += 1

    def __exit__(self, *exc):
        _state.building -= 1


def _call(cmd, **args):
    return process.call(cmd, **args)


def pretty(name):
    """A param name as a label, as Maya's panel writes it: "guideRadius" reads "Guide radius"."""
    words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).replace("_", " ").strip()
    words = " ".join(w if w.isupper() and len(w) > 1 else w.lower() for w in words.split())
    return words[:1].upper() + words[1:]


def spec_category(spec):
    """The Add menu category a spec names, as the menu titles it (the core's node catalog)."""
    spec = spec or {}
    return spec.get("categoryTitle") or (spec.get("category") or "Utility").title()


def category_of(op_type, specs=None):
    if op_type in ("Compound", "Script"):
        return "Compound"
    specs = specs if specs is not None else _specs()
    return spec_category(specs.get(op_type)) if op_type in specs else "Utility"


def label_of(op_type, specs=None):
    """A node type's label on screen (the core's: "Linked Groom" for Reference); files keep the type."""
    specs = specs if specs is not None else _specs()
    return (specs.get(op_type) or {}).get("label") or op_type


def categories(specs=None):
    """[(category, [types])] for the Add menu: the core's categories and order (node_catalog.h), the
    types the helper has, none of the hidden ones (Compound, Input)."""
    specs = specs if specs is not None else _specs()
    shown = [s for t, s in specs.items() if not s.get("hidden") and t not in HIDDEN_TYPES]
    shown.sort(key=lambda s: (s.get("categoryOrder", 99), s.get("order", 999), s["type"]))
    result = []
    for spec in shown:
        title = spec_category(spec)
        if not result or result[-1][0] != title:
            result.append((title, []))
        result[-1][1].append(spec["type"])
    return result


def param_sections(specs):
    """[(section, [spec])]: the main params (no header), each group in order, then Advanced; no
    group headers with fewer than two groups (Maya's param_sections)."""
    groups = []
    for spec in specs:
        group = spec.get("group") or ""
        if group and group != ADVANCED and not spec.get("advanced") and group not in groups:
            groups.append(group)
    headed = len(groups) >= 2

    def section_of(spec):
        group = spec.get("group") or ""
        if spec.get("advanced") or group == ADVANCED:
            return ADVANCED
        return group if headed else ""

    sections = [(key, [s for s in specs if section_of(s) == key]) for key in [""] + groups + [ADVANCED]]
    return [(key, members) for key, members in sections if members]


def shown_when(spec, value_of, spec_of=None):
    """Whether a param shows (graph.h ParamCondition): its showWhen's param holds one of its values
    (value_of(param)), and that param shows too (spec_of(name): its spec, None: unknown), as Maya's
    Graph Editor does it: Clump's count hides with clumpBy. No condition: always."""
    seen = set()
    while True:
        condition = spec.get("showWhen") or {}
        param = condition.get("param")
        if not param:
            return True
        value = value_of(param)
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, str):
            text = value
        else:
            text = json.dumps(value)
        if text not in (condition.get("in") or []):
            return False
        seen.add(spec.get("name"))
        spec = spec_of(param) if spec_of else None
        if spec is None or spec.get("name") in seen:
            return True


def _specs():
    from . import graph
    return graph._specs


def _rgb(c, a=1.0):
    return (c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, a)


def _linear(c):
    """sRGB (0-255) to Blender's linear color properties."""
    def one(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return tuple(one(v) for v in c)


# A node header's title starts with this much room: the editor's overlay puts the category's dot there
# (the minimal headers of Maya and the app: a dot before the name, no accent bar).
LABEL_LEAD = "    "


def tinted(category):
    """A node's body: Maya's node color with a wash of its category's (Blender draws headers in its
    theme's color: the body carries the category)."""
    accent = CATEGORY_COLORS.get(category, (142, 150, 158))
    mix = [NODE_BG[i] + (accent[i] - NODE_BG[i]) * 0.22 for i in range(3)]
    return _linear(mix)


# ---- Sockets: one class per port type, in Maya's wire colors ------------------------------------

def _socket_class(port_type):
    color = _rgb(TYPE_COLORS.get(port_type, (200, 200, 200)))
    kind = {"Curves": "strands or guides"}.get(port_type, port_type)

    def draw(self, context, layout, node, text):
        layout.label(text=text)

    def draw_color_simple(cls):
        return color

    return type("GroomLabSocket" + port_type, (bpy.types.NodeSocket,), {
        "bl_idname": "GroomLabSocket" + port_type,
        "bl_label": port_type,
        "__doc__": "A GroomLab %s port" % kind,
        "draw": draw,
        "draw_color_simple": classmethod(draw_color_simple),
    })


SOCKET_CLASSES = [_socket_class(t) for t in ("Strands", "Guides", "Mesh", "Field", "Curve", "Curves", "Value", "Any")]


def socket_idname(port_type):
    return "GroomLabSocket" + (port_type if port_type in TYPE_COLORS else "Any")


# ---- The tree ---------------------------------------------------------------------------------

class GroomLabGraphTree(bpy.types.NodeTree):
    """A GroomLab groom's node graph (edits go to the groom, which evaluates as you work)"""
    bl_idname = TREE
    bl_label = "GroomLab Graph"
    bl_icon = "CURVES"

    gl_scalp: PointerProperty(type=bpy.types.Object, name="Groom", description="The groom (scalp mesh) this graph is")

    def update(self):
        if _state.building or _state.undoing or props.is_quiet():
            return
        _state.in_update += 1
        try:
            sync_from_tree(self)
        except HelperError as e:
            _note(self.gl_scalp, e.message)
        except Exception as e:  # noqa: BLE001 - Blender's update must not raise
            _note(self.gl_scalp, str(e))
            print("GroomLab: %r" % e)
        finally:
            _state.in_update -= 1


def _note(scalp, text):
    if scalp is not None:
        _messages[scalp.groomlab.uid] = text


def message(scalp):
    return _messages.get(scalp.groomlab.uid, "") if scalp is not None else ""


def is_graph_tree(tree):
    return tree is not None and getattr(tree, "bl_idname", "") == TREE


def tree_of(scalp):
    """The groom's tree, or None."""
    if scalp is None:
        return None
    for t in bpy.data.node_groups:
        if t.bl_idname == TREE and t.gl_scalp == scalp:
            return t
    return None


def ensure_tree(scalp):
    t = tree_of(scalp)
    if t is None:
        t = bpy.data.node_groups.new("%s Graph" % scalp.name, TREE)
        t.use_fake_user = True  # nothing else uses it: kept in the .blend
        t.gl_scalp = scalp
    return t


def remove_tree(scalp):
    t = tree_of(scalp)
    if t is not None:
        with building():
            bpy.data.node_groups.remove(t)
    _remove_ramps(scalp)


def ours(node):
    return getattr(node, "gl_type", None) is not None and node.bl_idname.startswith(NODE_PREFIX)


# ---- Node classes, made from the operator specs -------------------------------------------------

class GroomLabNodeBase(object):
    """What every GroomLab node class has (the generated classes add the operator's params)."""
    gl_type = ""
    gl_spec = {}

    @classmethod
    def poll(cls, ntree):
        return ntree.bl_idname == TREE

    def init(self, context):
        self.width = NODE_WIDTH
        _make_sockets(self, _class_ports(self.gl_spec))
        self.use_custom_color = True
        self.color = tinted(category_of(self.gl_type))

    def copy(self, node):
        # Shift+D, Ctrl+C/V: a node of its own in the groom, with the copied node's params.
        self.gl_copy_of = node.gl_name
        self.gl_name = ""

    def draw_label(self):
        # Room before the name for the overlay's dot in the category's color (a minimal header).
        return LABEL_LEAD + self.name

    def draw_buttons(self, context, layout):
        linkable = [p["name"] for p in node_param_specs(self) if p.get("port")]
        if linkable:
            row = layout.row(align=True)
            exposed = set(json.loads(self.gl_inputs or "[]"))
            op = row.operator("groomlab.graph_toggle_inputs", text="", icon="LINENUMBERS_ON",
                              depress=all(param in exposed for param in linkable))
            op.node = self.gl_name
        col = layout.column(align=True)
        col.scale_y = 0.8
        kind = "OUTPUT" if self.gl_is_output and self.gl_type == "Output" else label_of(self.gl_type).upper()
        flags = []
        if self.gl_is_output and self.gl_type != "Output":
            flags.append("output")
        if self.gl_viewed:
            flags.append("viewed")
        if self.gl_evaluation in EVALUATION_LABELS:
            flags.append(EVALUATION_LABELS[self.gl_evaluation].lower())
        col.label(text=kind + ("  (%s)" % ", ".join(flags) if flags else ""))
        if self.gl_summary:
            col.label(text=self.gl_summary)


def _base_props():
    return {
        "gl_name": StringProperty(description="The node's name in the groom's graph"),
        "gl_copy_of": StringProperty(),
        "gl_ports": StringProperty(),
        "gl_summary": StringProperty(),
        "gl_outdated": BoolProperty(),
        "gl_version": IntProperty(),
        "gl_is_output": BoolProperty(),
        "gl_viewed": BoolProperty(),
        "gl_evaluation": StringProperty(default="always"),  # when it runs: always, viewportOnly, renderOnly
        "gl_mapped": StringProperty(),     # {param: map value} of params set by a map
        "gl_inputs": StringProperty(),     # [param] exposed as inputs (paramInputs)
        "gl_drivers": StringProperty(),    # {param: driver node} for exposed params linked
        "gl_values": StringProperty(),     # the node's params as the graph has them (JSON)
        "gl_spec_json": StringProperty(),  # node_spec of a Compound, Script or Input (their own ports and params)
        "gl_library": StringProperty(),    # "node:<id>": added from the node library (the Add menu)
        "gl_library_info": StringProperty(),  # node_spec's "library" of a library node (its id, version, latest)
        "gl_init": StringProperty(),       # {"name", "params"} a node added from Tab search starts with (a painted map's Map)
        "gl_controls": StringProperty(),   # {param: control} of keyed params (paramControls)
        "gl_params": CollectionProperty(type=props.GroomLabParam),  # a Compound's params
    }


def _scale():
    scene = getattr(bpy.context, "scene", None)
    return units.cm_per_unit(scene)


def _ranged(spec, conv):
    """min/max/soft_min/soft_max kwargs from a ParamSpec (converted, soft within hard)."""
    kw = {}
    lo, hi = spec.get("min"), spec.get("max")
    if lo is not None:
        kw["min"] = conv(lo)
    if hi is not None:
        kw["max"] = conv(hi)
    slo, shi = spec.get("softMin"), spec.get("softMax")
    if slo is not None:
        kw["soft_min"] = max(conv(slo), kw.get("min", conv(slo)))
    if shi is not None:
        kw["soft_max"] = min(conv(shi), kw.get("max", conv(shi)))
    if "soft_min" in kw and "soft_max" in kw and kw["soft_min"] > kw["soft_max"]:
        kw.pop("soft_min")
        kw.pop("soft_max")
    return kw


def param_property(spec, update, file_path=False):
    """The node property of one param (None: a ramp, drawn with a curve widget; or a kind Blender
    cannot edit here). A unit that holds only in some mode (unitWhen) converts as the node's values
    have it (_unit_of), so the property itself takes none. `file_path`: a string naming a file
    (graph_ui.json's filePathFilters: Blender's file browser button)."""
    kind, unit = spec.get("kind"), "" if spec.get("unitWhen") else (spec.get("unit") or "")
    common = {"name": pretty(spec["name"]), "description": spec.get("description", ""), "update": update}
    if file_path and kind == "string" and not spec.get("choices"):
        return StringProperty(subtype="FILE_PATH", **common)
    scale = _scale()
    if kind == "float":
        if unit in units.SCENE_LENGTH:
            conv = lambda v: float(v) / scale  # noqa: E731
            return FloatProperty(subtype="DISTANCE", unit="LENGTH", precision=4, **_ranged(spec, conv), **common)
        if unit == "perLength":
            conv = lambda v: float(v) * scale  # noqa: E731
            return FloatProperty(precision=3, **_ranged(spec, conv), **common)
        if unit == "fraction":
            kw = _ranged(spec, float)
            kw.setdefault("soft_min", 0.0)
            kw.setdefault("soft_max", 1.0)
            return FloatProperty(subtype="FACTOR", precision=3, **kw, **common)
        # The core's decimals (the spec's), as every host shows the number.
        return FloatProperty(precision=max(0, min(6, int(spec.get("decimals", 3)))), **_ranged(spec, float), **common)
    if kind == "int":
        return IntProperty(**{k: int(round(v)) for k, v in _ranged(spec, float).items()}, **common)
    if kind == "bool":
        return BoolProperty(**common)
    if kind == "vec3":
        if unit == "color":
            return FloatVectorProperty(subtype="COLOR", size=3, min=0.0, soft_max=1.0, **common)
        return FloatVectorProperty(size=3, **common)
    if kind == "string":
        choices = spec.get("choices") or []
        if choices:
            items = [(c, pretty(c) if c else "(none)", "") for c in choices]
            return EnumProperty(items=items, **common)
        return StringProperty(**common)
    return None


def prop_name(param):
    return "p_" + param


def _make_update(param):
    def update(self, context):
        if props.is_quiet() or _state.building:
            return
        push_param(self, param, context)
    return update


def _class_ports(spec):
    ins = [(p["name"], p["type"], False) for p in spec.get("inputs", [])]
    outs = [(p["name"], p["type"]) for p in (spec.get("outputs") or [spec.get("output")]) if p]
    return ins, outs


def make_node_class(spec):
    op_type = spec["type"]
    annotations = _base_props()
    params = {}
    files = op_type in FILE_PATH_FILTERS
    for p in spec.get("params", []):
        params[p["name"]] = p
        prop = param_property(p, _make_update(p["name"]), file_path=files and p["name"] == "path")
        if prop is not None:
            annotations[prop_name(p["name"])] = prop
    attrs = {
        "bl_idname": NODE_PREFIX + op_type,
        "bl_label": spec.get("label") or op_type,  # the core's label ("Linked Groom"); files keep the type
        "bl_description": spec.get("description", ""),
        "bl_icon": "NODE",
        "bl_width_default": NODE_WIDTH,
        "gl_type": op_type,
        "gl_spec": spec,
        "gl_param_specs": params,
        "__annotations__": annotations,
        "__doc__": spec.get("description") or op_type,
    }
    return type(NODE_PREFIX + op_type, (GroomLabNodeBase, bpy.types.Node), attrs)


def ensure_node_classes():
    """One node class per operator the helper has (made again when its operators change: another
    helper)."""
    from . import graph
    specs = graph.operator_specs()
    key = json.dumps([(t, specs[t].get("version"), [p["name"] for p in specs[t].get("params", [])]) for t in sorted(specs)])
    if key == _classes_key[0]:
        return
    with building():  # the trees' nodes are undefined in between: no edit of the user's
        unregister_node_classes()
        for op_type in sorted(specs):
            cls = make_node_class(specs[op_type])
            bpy.utils.register_class(cls)
            _node_classes[op_type] = cls
    _classes_key[0] = key


def unregister_node_classes():
    for cls in _node_classes.values():
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
    _node_classes.clear()
    _classes_key[0] = None


def node_class(op_type):
    return _node_classes.get(op_type)


def reset_for_another_helper():
    """Tests: the helper changed (another executable): its operators are asked again."""
    from . import graph
    graph._specs.clear()
    _classes_key[0] = None


# ---- Sockets of a node ------------------------------------------------------------------------

def _make_sockets(node, ports):
    ins, outs = ports
    signature = json.dumps([ins, outs])
    if node.gl_ports == signature and len(node.inputs) == len(ins) and len(node.outputs) == len(outs):
        return False
    node.inputs.clear()
    node.outputs.clear()
    for name, port_type, is_param in ins:
        s = node.inputs.new(socket_idname(port_type), name, identifier=name)
        s.link_limit = 1
        if is_param:
            s.display_shape = "DIAMOND"  # an exposed param, not a data input (Maya draws a diamond)
    for name, port_type in outs:
        node.outputs.new(socket_idname(port_type), name, identifier=name)
    node.gl_ports = signature
    return True


def _output_type(graph, name, spec_of, seen=None):
    """A node's first output type as connected: Curves gives what feeds its first Curves input."""
    spec = spec_of(name)
    if not spec:
        return "Curves"
    outs = spec.get("outputs") or [spec.get("output")]
    t = outs[0]["type"] if outs and outs[0] else "Curves"
    if t != "Curves":
        return t
    seen = seen or set()
    if name in seen:
        return t
    seen.add(name)
    curves_in = next((p["name"] for p in spec.get("inputs", []) if p["type"] in ("Curves", "Strands", "Guides")), None)
    for c in graph.get("connections", []):
        if c.get("to") == name and c.get("input") == curves_in:
            return _output_type(graph, c["from"], spec_of, seen)
    return t


# ---- Values: graph params into a node's properties and back -------------------------------------

def _unit_of(node, spec):
    """The unit a node's param has now (units.effective, by the node's other properties)."""
    return units.effective(spec, lambda param: getattr(node, prop_name(param), None) if param else None)


def _to_property(node, spec, value, scene):
    """Sets a node property from a graph value (quietly); False when the property cannot show it."""
    name = prop_name(spec["name"])
    if not hasattr(node, name) or isinstance(value, dict):
        return False
    kind, unit = spec.get("kind"), _unit_of(node, spec)
    try:
        if kind == "float" and isinstance(value, (int, float)) and not isinstance(value, bool):
            setattr(node, name, units.to_ui(unit, float(value), scene))
        elif kind == "int" and isinstance(value, int) and not isinstance(value, bool):
            setattr(node, name, value)
        elif kind == "bool" and isinstance(value, bool):
            setattr(node, name, value)
        elif kind == "vec3" and isinstance(value, list) and len(value) == 3:
            setattr(node, name, [units.to_ui(unit, float(v), scene) for v in value])  # a position or move: lengths
        elif kind == "string" and isinstance(value, str):
            setattr(node, name, value)
        else:
            return False
    except (TypeError, ValueError):
        return False
    return True


def property_value(node, spec, scene):
    """A node property as the graph takes it (centimeters)."""
    name = prop_name(spec["name"])
    v = getattr(node, name)
    kind, unit = spec.get("kind"), _unit_of(node, spec)
    if kind == "float":
        return units.from_ui(unit, float(v), scene)
    if kind == "int":
        return int(v)
    if kind == "bool":
        return bool(v)
    if kind == "vec3":
        return [units.from_ui(unit, float(x), scene) for x in v]
    if spec["name"] == "path" and node.gl_type in FILE_PATH_FILTERS and str(v).startswith("//"):
        return bpy.path.abspath(str(v))  # (Blender's file browser gives paths relative to the .blend)
    return str(v)


def node_param_specs(node):
    """The node's params' specs in order: a Compound's (or Input's) own, else its operator's."""
    if node.gl_spec_json:
        return json.loads(node.gl_spec_json).get("params", [])
    return node.gl_spec.get("params", [])


def values(node):
    return json.loads(node.gl_values or "{}")


def summary(specs, params, drivers):
    """A few params that differ from their defaults, as Maya's node cards show them."""
    parts = []
    for p in specs:
        if p["name"] in ("definition",):
            continue
        value = params.get(p["name"], p.get("default"))
        if value == p.get("default"):
            continue
        if isinstance(value, bool):
            text = "on" if value else "off"
        elif isinstance(value, (int, float)):
            text = "%g" % value
        elif isinstance(value, dict):
            source = next((k for k in ("texture", "colorSet", "attribute", "expression", "guide") if k in value), "map")
            text = {"colorSet": "color set", "guide": "map"}.get(source, source)
        elif isinstance(value, str) and len(value) < 14:
            text = value
        else:
            text = "..."
        parts.append("%s %s" % (p["name"], text))
        if len(parts) == 2:
            break
    if drivers and len(parts) < 2:
        parts.append("%s x field" % sorted(drivers)[0])
    return " · ".join(parts)


def set_values(node, gnode, scene, drivers):
    """The graph node's params into the Blender node (quietly), its ramps into their curves."""
    params = gnode.get("params") or {}
    specs = node_param_specs(node)
    mapped = {}
    with props.quiet():
        node.gl_values = json.dumps(params)
        if node.gl_spec_json:  # a Compound's (or Input's) own params: a list of widgets
            from . import graph
            node.gl_params.clear()
            for ps in specs:
                p = node.gl_params.add()
                p.name = ps["name"]
                p.kind = ps.get("kind", "")
                p.unit = units.effective(ps, lambda q: params.get(q, next((s.get("default") for s in specs if s["name"] == q), None)))
                p.group = ps.get("group", "")
                p.advanced = bool(ps.get("advanced"))
                p.description = ps.get("description", "")
                p.choices = json.dumps(ps.get("choices") or [])
                p.show_when = json.dumps(ps.get("showWhen") or {})
                graph._set_widget(p, params.get(ps["name"], ps.get("default")), scene)
                if isinstance(params.get(ps["name"]), dict):
                    mapped[ps["name"]] = params[ps["name"]]
        else:
            for ps in specs:
                value = params.get(ps["name"], ps.get("default"))
                if isinstance(value, dict):
                    mapped[ps["name"]] = value
                elif ps.get("kind") == "ramp":
                    write_ramp(node, ps["name"], value)
                else:
                    _to_property(node, ps, value, scene)
        node.gl_mapped = json.dumps(mapped) if mapped else ""
        node.gl_summary = summary(specs, params, drivers)


def push_param(node, param, context=None):
    """A node property changed: set_param in the helper (centimeters), the groom evaluates soon."""
    tree = node.id_data
    scalp = tree.gl_scalp
    if not groom.is_groom(scalp) or not node.gl_name:
        return
    spec = node.gl_param_specs.get(param)
    scene = context.scene if context is not None and getattr(context, "scene", None) is not None else groom.scene_of(scalp)
    value = property_value(node, spec, scene)
    if not set_param_value(scalp, node, param, value):
        return
    # A param whose unit this one decides (Noise's frequency by its space): shown again in its new unit.
    for other, s in node.gl_param_specs.items():
        if (s.get("unitWhen") or {}).get("param") != param:
            continue
        try:
            stored = _call("get_param", session=groom.ensure(scalp).session, node=node.gl_name, param=other)["value"]
        except HelperError:
            continue
        with props.quiet():
            _to_property(node, s, stored, scene)


def set_param_value(scalp, node, param, value):
    try:
        rt = groom.ensure(scalp)
        _call("set_param", session=rt.session, node=node.gl_name, param=param, value=value)
    except HelperError as e:
        _note(scalp, e.message)
        return False
    params = values(node)
    params[param] = value
    with props.quiet():
        node.gl_values = json.dumps(params)
        node.gl_summary = summary(node_param_specs(node), params, json.loads(node.gl_drivers or "{}"))
    _after_change(scalp, node.gl_name)
    return True


def push_collection_param(p, context):
    """A Compound's param widget (props.GroomLabParam in the node's list) changed."""
    from . import graph
    tree = p.id_data
    at = p.as_pointer()
    node = next((n for n in tree.nodes if ours(n) and any(q.as_pointer() == at for q in n.gl_params)), None)
    if node is None:
        return
    scalp = tree.gl_scalp
    scene = context.scene if context is not None and getattr(context, "scene", None) is not None else groom.scene_of(scalp)
    value = graph.widget_value(p, scene)
    if set_param_value(scalp, node, p.name, value):
        with props.quiet():
            p.raw = json.dumps(value)


def _after_change(scalp, node_name=None):
    from . import graph
    rt = groom.ensure(scalp)
    groom.remember_graph(scalp, rt)
    g = scalp.groomlab
    if node_name and g.params_node == node_name:
        try:
            graph.refresh_params(scalp)
        except HelperError:
            pass
    groom.mark(scalp, "eval")
    _messages.pop(g.uid, None)


def values_changed(scalp, node_name):
    """A param set elsewhere (the 3D view's panel, a script): the node shows it."""
    tree = tree_of(scalp)
    if tree is None:
        return
    node = next((n for n in tree.nodes if ours(n) and n.gl_name == node_name), None)
    if node is None:
        return
    rt = groom.ensure(scalp)
    data = _call("get_graph", session=rt.session)["graph"]
    gnode = next((n for n in data.get("nodes", []) if n.get("name") == node_name), None)
    if gnode is not None:
        with building():
            set_values(node, gnode, groom.scene_of(scalp), json.loads(node.gl_drivers or "{}"))


# ---- Ramps: Blender's curve widget ----------------------------------------------------------------

def _ramp_tree(create=True):
    t = bpy.data.node_groups.get(RAMPS)
    if t is None and create:
        t = bpy.data.node_groups.new(RAMPS, "ShaderNodeTree")
    return t


def _ramp_key(node, param):
    tree = node.id_data
    uid = tree.gl_scalp.groomlab.uid if tree.gl_scalp is not None else tree.name
    return "%s|%s|%s" % (uid, node.gl_name or node.name, param)


def _ramp_name(key):
    return "ramp_" + hashlib.blake2b(key.encode("utf-8"), digest_size=10).hexdigest()


def ramp_node(node, param, create=False):
    """The Float Curve node whose curve is this ramp param (None: not made yet)."""
    t = _ramp_tree(create)
    if t is None:
        return None
    key = _ramp_key(node, param)
    name = _ramp_name(key)
    c = t.nodes.get(name)
    if c is None and create:
        c = t.nodes.new("ShaderNodeFloatCurve")
        c.name = name
        c["gl_key"] = key
    return c


def ramp_keys(value):
    """(keys [(pos, value)], linear) of a ramp param's value: [[pos, value]...] or {"keys", "linear"}."""
    if isinstance(value, dict):
        return [tuple(k) for k in value.get("keys", [])], bool(value.get("linear"))
    if isinstance(value, list):
        return [tuple(k) for k in value if isinstance(k, (list, tuple)) and len(k) == 2], False
    return [], False


def _curve_points(curve):
    c = curve.mapping.curves[0]
    keys = [(round(float(p.location[0]), 5), round(float(p.location[1]), 5)) for p in c.points]
    linear = all(p.handle_type == "VECTOR" for p in c.points)
    return keys, linear


def write_ramp(node, param, value):
    curve = ramp_node(node, param, create=True)
    keys, linear = ramp_keys(value)
    if not keys:
        keys = [(0.0, 1.0)]
    shown = list(keys) if len(keys) >= 2 else [(0.0, keys[0][1]), (1.0, keys[0][1])]
    m = curve.mapping
    c = m.curves[0]
    while len(c.points) > len(shown):
        c.points.remove(c.points[len(c.points) - 1])
    while len(c.points) < len(shown):
        c.points.new(0.5, 0.5)
    for p, (x, y) in zip(c.points, sorted(shown)):
        p.location = (float(x), float(y))
        p.handle_type = "VECTOR" if linear else "AUTO_CLAMPED"
    ys = [k[1] for k in shown]
    m.use_clip = True
    m.clip_min_x, m.clip_max_x = 0.0, 1.0
    m.clip_min_y, m.clip_max_y = min(0.0, min(ys)), max(1.0, max(ys))
    m.update()
    curve["gl_written"] = json.dumps(_curve_points(curve))
    curve["gl_single"] = len(keys) == 1


def ramp_value(curve):
    keys, linear = _curve_points(curve)
    if curve.get("gl_single") and len(keys) == 2 and keys[0][1] == keys[1][1]:
        keys = [(0.0, keys[0][1])]
    keys = [[k[0], k[1]] for k in keys]
    return {"keys": keys, "linear": True} if linear else keys


def ramp_changed(curve):
    return curve.get("gl_written") is not None and json.dumps(_curve_points(curve)) != curve["gl_written"]


def sync_ramps(tree):
    """Curves edited in the panel's widget: their ramps set in the helper. True when any was."""
    t = _ramp_tree(False)
    if t is None or tree.gl_scalp is None:
        return False
    done = False
    for node in tree.nodes:
        if not ours(node) or not node.gl_name:
            continue
        for ps in node_param_specs(node):
            if ps.get("kind") != "ramp":
                continue
            curve = t.nodes.get(_ramp_name(_ramp_key(node, ps["name"])))
            if curve is None or not ramp_changed(curve):
                continue
            value = ramp_value(curve)
            curve["gl_written"] = json.dumps(_curve_points(curve))
            if set_param_value(tree.gl_scalp, node, ps["name"], value):
                done = True
    return done


def _remove_ramps(scalp):
    t = _ramp_tree(False)
    if t is None:
        return
    prefix = scalp.groomlab.uid + "|"
    for n in list(t.nodes):
        if str(n.get("gl_key", "")).startswith(prefix):
            t.nodes.remove(n)


# ---- The helper's graph into the tree -------------------------------------------------------------

def mark_replaced(scalp):
    """The groom's graph is replaced whole (a preset applied, a file opened): the tree takes the
    graph's positions instead of keeping its own."""
    _replaced.add(scalp.groomlab.uid)


def graph_changed(scalp):
    """The helper's graph changed (any edit): the tree follows, now or at the next flush()."""
    if not groom.is_groom(scalp):
        return
    if _state.in_update or _state.building:
        _pending.add(scalp.groomlab.uid)
        _schedule()
        return
    rebuild(scalp)


def _schedule():
    if bpy.app.background:
        return  # tests call flush()
    if not bpy.app.timers.is_registered(flush):
        bpy.app.timers.register(flush, first_interval=0.0)


def flush():
    """Rebuilds the trees waiting for it (the timer's work; tests call it)."""
    pending = set(_pending)
    _pending.clear()
    for uid in pending:
        scalp = groom.find(uid)
        if scalp is None:
            continue
        try:
            rebuild(scalp)
        except HelperError as e:
            _note(scalp, e.message)
    _redraw()
    return None


def _redraw():
    wm = getattr(bpy.context, "window_manager", None)
    if wm is None:
        return
    for window in wm.windows:
        for area in window.screen.areas:
            if area.type in ("NODE_EDITOR", "VIEW_3D"):
                area.tag_redraw()


def location_of(gnode):
    ui = gnode.get("ui")
    if isinstance(ui, dict) and isinstance(ui.get("x"), (int, float)) and isinstance(ui.get("y"), (int, float)):
        return float(ui["x"]), -float(ui["y"])
    return None


def get_location(node):
    """A node's place in the whole editor (a node in a frame has its location relative to it)."""
    loc = node.location_absolute if hasattr(node, "location_absolute") else node.location
    return float(loc[0]), float(loc[1])


def set_location(node, at):
    if hasattr(node, "location_absolute"):
        node.location_absolute = at
    else:
        node.location = at


def ui_of(node):
    """A Blender node's position as the graph keeps it (Maya's: y down)."""
    x, y = get_location(node)
    return [round(x, 2), round(-y, 2)]


# ---- Backdrops: Blender's frames ------------------------------------------------------------------

def frames(tree):
    return [n for n in tree.nodes if n.bl_idname == "NodeFrame"]


def _srgb(color):
    """Blender's linear color to sRGB 0-255 (a backdrop's color)."""
    def one(v):
        v = max(0.0, min(1.0, float(v)))
        v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1.0 / 2.4) - 0.055
        return int(round(v * 255.0))
    return [one(c) for c in color[:3]]


def frame_size(frame):
    """(w, h) of a frame in the editor's units: its own size, or what it shrinks to around its nodes
    as last drawn (Ctrl+J's frames)."""
    w, h = float(frame.width), float(frame.height)
    if frame.shrink and frame.dimensions[0] > 0:
        prefs = bpy.context.preferences.system if getattr(bpy.context, "preferences", None) else None
        scale = float(prefs.ui_scale) * float(getattr(prefs, "pixel_size", 1.0)) if prefs is not None else 1.0
        w, h = frame.dimensions[0] / scale, frame.dimensions[1] / scale
    return max(20.0, w), max(20.0, h)


def backdrop_of(frame):
    """A frame as the graph keeps a backdrop (Maya's): {title, x, y, w, h, color}, y down."""
    x, y = get_location(frame)
    w, h = frame_size(frame)
    color = _srgb(frame.color) if frame.use_custom_color else list(BACKDROP_COLOR)
    return {"title": frame.label or frame.name, "x": round(x, 2), "y": round(-y, 2), "w": round(w, 2),
            "h": round(h, 2), "color": color}


def backdrops_of(tree):
    return [backdrop_of(f) for f in frames(tree)] if tree is not None else []


def _same_backdrops(a, b):
    def key(items):
        return [(str(i.get("title", "")), round(float(i.get("x", 0)), 1), round(float(i.get("y", 0)), 1),
                 round(float(i.get("w", 0)), 1), round(float(i.get("h", 0)), 1), list(i.get("color") or BACKDROP_COLOR))
                for i in items or []]
    return key(a) == key(b)


def set_frame(frame, backdrop):
    """A frame showing a backdrop of the graph (quietly: building)."""
    frame.shrink = False
    frame.label = str(backdrop.get("title") or "Backdrop")
    frame.width = max(20.0, float(backdrop.get("w", 300.0)))
    frame.height = max(20.0, float(backdrop.get("h", 200.0)))
    set_location(frame, (float(backdrop.get("x", 0.0)), -float(backdrop.get("y", 0.0))))
    frame.use_custom_color = True
    frame.color = _linear(backdrop.get("color") or BACKDROP_COLOR)
    frame["gl_backdrop"] = True


def frame_at(tree, at):
    """The frame whose rectangle holds the point `at` (Blender's x, y), the smallest; None."""
    best, area = None, None
    for f in frames(tree):
        x, y = get_location(f)
        w, h = frame_size(f)
        if x <= at[0] <= x + w and y - h <= at[1] <= y:
            if area is None or w * h < area:
                best, area = f, w * h
    return best


# ---- Pins: Blender's reroutes ---------------------------------------------------------------------

def reroutes(tree):
    return [n for n in tree.nodes if n.bl_idname == "NodeReroute"]


def pin_of(reroute):
    """The pin a reroute is ("pin<N>"; "" for a reroute that routes nothing yet)."""
    return str(reroute.get("gl_pin") or "")


def _pin_number(pin):
    """pin12 -> 12 (0 for another name): pins sort and number by it (Maya's)."""
    digits = pin[3:] if pin.startswith("pin") else ""
    return int(digits) if digits.isdigit() else 0


def connections_of(data):
    """{(to, input): (from, output)} of a graph ("" for the source's first output)."""
    return {(c["to"], c["input"]): (c["from"], c.get("output") or "") for c in data.get("connections", [])}


def prune_pins(pins, connections):
    """The graph's pins ({"points", "routes"}) that hold, as Blender can draw them: routes whose link
    is there, from the same source, through pins that exist, none twice; each pin fed from one place
    (a reroute has one input: a route feeding it from elsewhere is left out, its link drawn straight);
    pins no route passes left out. Returns (points {id: [x, y]}, routes {(to, input): (from, output,
    [via])})."""
    pins = pins if isinstance(pins, dict) else {}
    points = {}
    for p in pins.get("points") or []:
        if isinstance(p, dict) and p.get("id") and isinstance(p.get("x"), (int, float)) and isinstance(p.get("y"), (int, float)):
            points[str(p["id"])] = [float(p["x"]), float(p["y"])]
    routes, fed = {}, {}
    for r in pins.get("routes") or []:
        if not isinstance(r, dict) or not r.get("to") or not r.get("input"):
            continue
        key, source = (r["to"], r["input"]), (r.get("from"), r.get("output") or "")
        via = [str(v) for v in r.get("via") or []]
        if not via or connections.get(key) != source or key in routes or len(set(via)) != len(via):
            continue
        if any(v not in points for v in via):
            continue
        feeds = [("node",) + source] + [("pin", v) for v in via[:-1]]
        if any(fed.get(v, f) != f for v, f in zip(via, feeds)):
            continue
        fed.update(zip(via, feeds))
        routes[key] = (source[0], source[1], via)
    used = {v for r in routes.values() for v in r[2]}
    return {k: v for k, v in points.items() if k in used}, routes


def pins_dict(points, routes):
    """{"points", "routes"} as the graph keeps them (Maya's _pins_dict: routes by (to, input), points
    by number, "output" only when not the first)."""
    out_routes = []
    for (to, input_name), (src, output, via) in sorted(routes.items()):
        entry = {"from": src, "to": to, "input": input_name, "via": list(via)}
        if output:
            entry["output"] = output
        out_routes.append(entry)
    out_points = [{"id": k, "x": v[0], "y": v[1]} for k, v in sorted(points.items(), key=lambda kv: (_pin_number(kv[0]), kv[0]))]
    return {"points": out_points, "routes": out_routes}


def chains(tree):
    """{(to, input): (source node, its output's identifier, [reroutes, source first])}: every link
    into a GroomLab node's input, followed back through the reroutes to the GroomLab node feeding it
    (none when they lead nowhere: a reroute with no input)."""
    into = {l.to_node.as_pointer(): l for l in tree.links if l.to_node.bl_idname == "NodeReroute"}
    out = {}
    for link in tree.links:
        b = link.to_node
        if not (ours(b) and b.gl_name):
            continue
        a, socket, via = link.from_node, link.from_socket, []
        while a is not None and a.bl_idname == "NodeReroute" and a not in via:
            via.insert(0, a)
            feed = into.get(a.as_pointer())
            a, socket = (feed.from_node, feed.from_socket) if feed is not None else (None, None)
        if a is not None and ours(a) and a.gl_name:
            out[(b.gl_name, link.to_socket.identifier)] = (a, socket.identifier, via)
    return out


def _source_of(a, identifier):
    return a.gl_name, "" if identifier == _first_output(a) else identifier


def tree_pins(tree, connections):
    """The tree's reroutes as the graph's pins ({"points", "routes"}): the routes of the links that
    are the graph's connections (`connections`), through reroutes that are pins (each id once)."""
    owner = {}
    for r in reroutes(tree):
        owner.setdefault(pin_of(r), r)
    points, routes = {}, {}
    for key, (a, identifier, via) in chains(tree).items():
        source = _source_of(a, identifier)
        if not via or connections.get(key) != source:
            continue
        if any(not pin_of(r) or owner.get(pin_of(r)) != r for r in via):
            continue
        routes[key] = (source[0], source[1], [pin_of(r) for r in via])
        for r in via:
            points[pin_of(r)] = ui_of(r)
    return pins_dict(points, routes)


def assign_pins(tree, connections):
    """The reroutes on the links that are the graph's connections become pins (a new one: the next
    "pin<N>"); the others route nothing now and are pins no more."""
    taken = {}
    for r in reroutes(tree):
        taken.setdefault(pin_of(r), r)
    used = set()
    for key, (a, identifier, via) in chains(tree).items():
        if via and connections.get(key) == _source_of(a, identifier):
            used.update(r.as_pointer() for r in via)
    number = max([_pin_number(p) for p in taken] or [0])
    with building():
        for r in reroutes(tree):
            pin = pin_of(r)
            if r.as_pointer() not in used:
                if pin:
                    del r["gl_pin"]
            elif not pin or taken.get(pin) != r:  # new, or a copy of another (Shift+D copies the id)
                number += 1
                r["gl_pin"] = "pin%d" % number


def _same_pins(a, b):
    def key(pins):
        pins = pins or {}
        return ([(p.get("id"), round(float(p.get("x", 0)), 1), round(float(p.get("y", 0)), 1)) for p in pins.get("points") or []],
                pins.get("routes") or [])
    return key(a) == key(b)


def auto_layout(data):
    """Columns by depth from the sources, rows in the graph's order (Maya's auto_layout): every
    node's "ui". Returns {node: [x, y]}."""
    depth = {}
    feeds = {}
    for c in data.get("connections", []):
        feeds.setdefault(c["to"], []).append(c["from"])

    def depth_of(name, trail=()):
        if name in depth:
            return depth[name]
        sources = [s for s in feeds.get(name, []) if s not in trail]
        d = 0 if not sources else 1 + max(depth_of(s, trail + (name,)) for s in sources)
        depth[name] = d
        return d

    rows, out = {}, {}
    for n in data.get("nodes", []):
        col = depth_of(n["name"])
        row = rows.get(col, 0)
        rows[col] = row + 1
        out[n["name"]] = [float(col * LAYOUT_DX), float(row * LAYOUT_DY)]
        n["ui"] = {"x": out[n["name"]][0], "y": out[n["name"]][1]}
    return out


def set_positions(scalp, positions, backdrops=None, pins=None):
    """{node: [x, y]} (and the backdrops, all of them, and the pins, {points, routes}, when given)
    into the helper's graph (edit_graph set_positions: a graph that does not load yet takes them too)."""
    if not positions and backdrops is None and pins is None:
        return
    rt = groom.ensure(scalp)
    args = {"positions": positions or {}}
    if backdrops is not None:
        args["backdrops"] = list(backdrops)
    if pins is not None:
        args["pins"] = pins
    _call("edit_graph", session=rt.session, op="set_positions", **args)
    groom.remember_graph(scalp, rt)


def _moved(node):
    """Whether the user moved the node since the tree last had it from the graph (or sent it)."""
    was, at = node.get("gl_at"), ui_of(node)
    return was is None or len(was) != 2 or abs(was[0] - at[0]) > 0.05 or abs(was[1] - at[1]) > 0.05


def push_positions(scalp, tree=None, data=None, pins=True):
    """The positions of the nodes the user moved into the helper's graph (moves are not edits: they
    go with the next edit, a save, a preset). Nodes the user did not move keep the graph's
    position, which an edit of the helper's may have changed (an inserted preset moves the nodes
    after it over). The frames and (with `pins`) the reroutes go too when they changed. Returns the
    graph."""
    tree = tree or tree_of(scalp)
    rt = groom.ensure(scalp)
    if data is None:
        data = _call("get_graph", session=rt.session)["graph"]
    if tree is None:
        return data
    by_name = {n.gl_name: n for n in tree.nodes if ours(n) and n.gl_name}
    moved = {}
    for gnode in data.get("nodes", []):
        node = by_name.get(gnode.get("name"))
        if node is None or node.gl_type != gnode.get("type") or not _moved(node):
            continue
        at = ui_of(node)
        ui = gnode.get("ui") if isinstance(gnode.get("ui"), dict) else {}
        if abs(float(ui.get("x", 1e30)) - at[0]) > 0.05 or abs(float(ui.get("y", 1e30)) - at[1]) > 0.05:
            moved[gnode["name"]] = at
            gnode["ui"] = {"x": at[0], "y": at[1]}
        with building():
            node["gl_at"] = at
    # The frames are the backdrops (Maya's): made, moved, resized, renamed or recolored since the
    # tree last had them from the graph (or sent them), all of them go.
    backdrops = backdrops_of(tree)
    was = tree.get("gl_backdrops")
    changed = was is not None and not _same_backdrops(backdrops, json.loads(was))
    # The reroutes are the pins, the same way: moved, made, rewired or deleted since the tree last
    # had them from the graph (or sent them), all of them go (routes checked against the graph's
    # links: a link the graph no longer has keeps no route).
    routed = tree_pins(tree, connections_of(data)) if pins else None
    was = tree.get("gl_pins")
    rerouted = routed is not None and was is not None and not _same_pins(routed, json.loads(was))
    set_positions(scalp, moved, backdrops if changed else None, routed if rerouted else None)
    if changed:
        with building():
            tree["gl_backdrops"] = json.dumps(backdrops)
        if backdrops:
            data["backdrops"] = backdrops
        else:
            data.pop("backdrops", None)
    if rerouted:
        with building():
            tree["gl_pins"] = json.dumps(routed)
        if routed["routes"]:
            data["pins"] = routed
        else:
            data.pop("pins", None)
    return data


def _drivers(data, node_name, inputs):
    out = {}
    for c in data.get("connections", []):
        if c.get("to") == node_name and c.get("input") in inputs:
            out[c["input"]] = c["from"]
    return out


def rebuild(scalp, replaced=None):
    """The tree again from the helper's graph: nodes, ports, values, links, the output; positions
    the tree's for the nodes it has (pushed to the graph first), the graph's for new ones (all of
    them when the graph was replaced whole)."""
    if not groom.is_groom(scalp):
        return None
    rt = groom.ensure(scalp)
    ensure_node_classes()
    tree = ensure_tree(scalp)
    uid = scalp.groomlab.uid
    replaced = (uid in _replaced) if replaced is None else replaced
    _replaced.discard(uid)
    data = _call("get_graph", session=rt.session)["graph"]
    if not replaced:
        data = push_positions(scalp, tree, data)
    if any(location_of(n) is None for n in data.get("nodes", [])):
        set_positions(scalp, auto_layout(data))  # a graph without a layout: Maya's, laid out the same
    build_tree(scalp, tree, data, rt)
    return tree


def build_tree(scalp, tree, data, rt=None):
    rt = rt or groom.ensure(scalp)
    scene = groom.scene_of(scalp)
    specs = _specs()
    gnodes = data.get("nodes", [])
    names = [n["name"] for n in gnodes]
    own_spec = {}
    for n in gnodes:  # Compounds and Inputs have ports and params of their own; exposed params add ports
        if n.get("type") in ("Compound", "Script", "Input") or n.get("paramInputs"):
            own_spec[n["name"]] = _call("node_spec", session=rt.session, node=n["name"])["spec"]

    def spec_of(name):
        if name in own_spec:
            return own_spec[name]
        t = next((n.get("type") for n in gnodes if n["name"] == name), None)
        return specs.get(t)

    view = scalp.groomlab.view_node
    with building():
        existing = {}
        kept_frames = []
        kept_reroutes = []
        for node in list(tree.nodes):
            if node.bl_idname == "NodeFrame":
                kept_frames.append(node)
                continue
            if node.bl_idname == "NodeReroute":
                kept_reroutes.append((node, get_location(node)))
                continue
            gname = node.gl_name if ours(node) else None
            gtype = next((n.get("type") for n in gnodes if n["name"] == gname), None) if gname else None
            if gname and gtype == node.gl_type and gname not in existing:
                existing[gname] = node
            else:
                tree.nodes.remove(node)  # gone from the graph, of another type now, or not ours
        # The backdrops as frames (in order: the first frame the first backdrop), placed first so
        # the nodes inside each can be its children (moving it moves them, as in Maya).
        for node in existing.values():
            node.parent = None
        for node, _ in kept_reroutes:
            node.parent = None
        backdrops =[b for b in data.get("backdrops") or [] if isinstance(b, dict)]
        for i, backdrop in enumerate(backdrops):
            frame = kept_frames[i] if i < len(kept_frames) else tree.nodes.new("NodeFrame")
            frame.parent = None
            set_frame(frame, backdrop)
        for frame in kept_frames[len(backdrops):]:
            tree.nodes.remove(frame)
        for i, node in enumerate(existing.values()):
            node.name = "__groomlab_%d" % i  # free every name before giving the graph's
        made = {}
        for gnode in gnodes:
            name, op_type = gnode["name"], gnode.get("type")
            cls = node_class(op_type)
            if cls is None:
                continue
            node = existing.get(name)
            fresh = node is None
            if fresh:
                node = tree.nodes.new(cls.bl_idname)
            node.name = name
            node.gl_name = name
            node.gl_copy_of = ""
            node.gl_init = ""
            at = location_of(gnode)
            if at is not None:
                set_location(node, at)
            inside = frame_at(tree, get_location(node)) if backdrops else None
            if inside is not None:
                node.parent = inside
            node["gl_at"] = ui_of(node)  # where the graph has it: a move after this is the user's
            spec = spec_of(name) or {}
            node.gl_spec_json = json.dumps(own_spec[name]) if name in own_spec and op_type in ("Compound", "Script", "Input") else ""
            library = (own_spec.get(name) or {}).get("library")
            node.gl_library_info = json.dumps(library) if isinstance(library, dict) else ""
            controls = gnode.get("paramControls")
            node.gl_controls = json.dumps(controls) if isinstance(controls, dict) and controls else ""
            ins = [(p["name"], p["type"], bool(p.get("param"))) for p in spec.get("inputs", [])]
            outs = [(p["name"], p["type"]) for p in (spec.get("outputs") or [spec.get("output")]) if p]
            if outs:
                outs[0] = (outs[0][0], _output_type(data, name, spec_of))
            _make_sockets(node, (ins, outs))
            inputs = list(gnode.get("paramInputs") or [])
            drivers = _drivers(data, name, inputs)
            node.gl_inputs = json.dumps(inputs) if inputs else ""
            node.gl_drivers = json.dumps(drivers) if drivers else ""
            version = gnode.get("version")
            newest = (specs.get(op_type) or {}).get("version", 1)
            node.gl_version = int(version) if isinstance(version, int) else newest
            node.gl_outdated = node.gl_version < newest
            node.gl_is_output = data.get("output") == name
            node.gl_viewed = view == name
            node.gl_evaluation = gnode.get("evaluation") or "always"
            node.mute = bool(gnode.get("bypass"))
            node.use_custom_color = True
            node.color = tinted("Output" if node.gl_is_output else category_of(op_type, specs))
            set_values(node, gnode, scene, drivers)
            made[name] = node
        # The pins as reroutes: the tree's own for the pins it has (Shift+D's copy of one, the
        # second, is the user's), new ones for the rest; a reroute that was a pin the graph no
        # longer has goes; one that never was (the user's, routing nothing yet) stays where it is.
        connections = {k: v for k, v in connections_of(data).items() if k[0] in made and v[0] in made}
        points, routes = prune_pins(data.get("pins"), connections)
        pin_nodes, loose = {}, []
        for node, at in kept_reroutes:
            pin = pin_of(node)
            if pin and pin in points and pin not in pin_nodes:
                pin_nodes[pin] = node
            elif pin and pin not in points:
                tree.nodes.remove(node)
            else:
                if pin:
                    del node["gl_pin"]
                loose.append((node, at))
        users = {node.as_pointer() for node, _ in loose}
        placed = list(loose)
        for pin in sorted(points, key=lambda p: (_pin_number(p), p)):
            node = pin_nodes.get(pin)
            if node is None:
                node = pin_nodes[pin] = tree.nodes.new("NodeReroute")
                node["gl_pin"] = pin
            placed.append((node, (points[pin][0], -points[pin][1])))
        for node, at in placed:
            set_location(node, at)
            inside = frame_at(tree, at) if backdrops else None
            if inside is not None:
                node.parent = inside
        # The links: only those that differ are removed or made (a node being dragged keeps its own).
        # A routed link runs source -> its reroutes -> input; links into a reroute that is no pin
        # are the user's.
        wanted = {}

        def want(out, inp):
            wanted[(out.node.name, out.identifier, inp.node.name, inp.identifier)] = (out, inp)

        for c in data.get("connections", []):
            src, dst = made.get(c.get("from")), made.get(c.get("to"))
            if src is None or dst is None:
                continue
            out = src.outputs.get(c.get("output") or "") if c.get("output") else (src.outputs[0] if len(src.outputs) else None)
            inp = dst.inputs.get(c.get("input", ""))
            if out is None or inp is None:
                continue
            for pin in routes.get((c["to"], c["input"]), (None, None, []))[2]:
                want(out, pin_nodes[pin].inputs[0])
                out = pin_nodes[pin].outputs[0]
            want(out, inp)
        for link in list(tree.links):
            key = (link.from_node.name, link.from_socket.identifier, link.to_node.name, link.to_socket.identifier)
            if wanted.pop(key, None) is None and link.to_node.as_pointer() not in users:
                tree.links.remove(link)
        for out, inp in wanted.values():
            tree.links.new(out, inp)
        if view and view not in names:
            with props.quiet():
                scalp.groomlab.view_node = ""
        tree["gl_sig"] = signature(tree)
        tree["gl_backdrops"] = json.dumps(backdrops_of(tree))
        tree["gl_pins"] = json.dumps(tree_pins(tree, connections))
        tree["gl_generation"] = process.generation()
    return tree


# ---- The tree's edits into the helper -----------------------------------------------------------

def signature(tree):
    """What the tree's structure is (nodes, names, mutes, links): an update that leaves it alone
    (a move, a selection) sends nothing."""
    nodes = sorted((n.name, n.gl_name, n.bl_idname, bool(n.mute)) for n in tree.nodes if ours(n))
    # Every link, valid or not: Blender validates a new link after the tree's update callback.
    links = sorted((l.from_node.name, l.from_socket.identifier, l.to_node.name, l.to_socket.identifier)
                   for l in tree.links)
    others = len([n for n in tree.nodes if not ours(n)])
    return json.dumps([nodes, links, others])


def _first_output(node):
    return node.outputs[0].identifier if len(node.outputs) else ""


def sync_from_tree(tree):
    """The user's edits of the tree (nodes added, duplicated, deleted, renamed, muted; links made
    or removed, through reroutes or not) as edit_graph ops in the helper; then the groom evaluates
    and the tree is rebuilt from the graph (a link the graph refused goes away). The reroutes on its
    links are the graph's pins, sent with the layout: reroutes alone are no edit. True when anything
    was sent."""
    scalp = tree.gl_scalp
    if not groom.is_groom(scalp):
        return False
    if any(n.bl_idname == "NodeUndefined" for n in tree.nodes):
        return False  # node types not registered (yet): the tree is made again from the graph, never sent
    sig = signature(tree)
    if tree.get("gl_sig") == sig:
        return False
    rt = groom.ensure(scalp)
    session = rt.session
    data = push_positions(scalp, tree, pins=False)  # (the reroutes go once the links are the graph's)
    gnodes = {n["name"]: n for n in data.get("nodes", [])}
    refused = []
    sent = []

    def edit(**args):
        sent.append(args["op"])
        try:
            return _call("edit_graph", session=session, **args)
        except HelperError as e:
            refused.append(e.message)
            return None

    nodes = [n for n in tree.nodes if ours(n)]
    # Renames (F2, the sidebar's Name field).
    for node in nodes:
        if node.gl_name and node.gl_name in gnodes and node.name != node.gl_name:
            if edit(op="rename_node", name=node.gl_name, newName=node.name) is not None:
                if scalp.groomlab.view_node == node.gl_name:
                    with props.quiet():
                        scalp.groomlab.view_node = node.name
                gnodes[node.name] = gnodes.pop(node.gl_name)
                with building():
                    node.gl_name = node.name
    # Deleted nodes.
    present = {n.gl_name for n in nodes if n.gl_name}
    for name in list(gnodes):
        if name not in present:
            edit(op="remove_node", name=name)
            gnodes.pop(name)
            if scalp.groomlab.view_node == name:
                with props.quiet():
                    scalp.groomlab.view_node = ""
    # New nodes (the Add menu, a duplicate or a paste: the copied node's params).
    for node in nodes:
        if node.gl_name:
            continue
        # (A Compound from the Add menu's Compound Nodes: the library node it stands for.)
        args = {"op": "add_node", "type": node.gl_library or node.gl_type, "x": ui_of(node)[0], "y": ui_of(node)[1]}
        source = data_node(data, node.gl_copy_of) if node.gl_copy_of else None
        if source is not None and source.get("type") == node.gl_type:
            args["params"] = source.get("params") or {}
        init = json.loads(node.gl_init) if node.gl_init else {}
        if init.get("params"):  # (Tab search's painted map: a Map node reading it, named after it)
            args["params"] = init["params"]
        if init.get("name"):
            args["name"] = unique_name(init["name"], gnodes)
        r = edit(**args)
        with building():
            if r is None:
                tree.nodes.remove(node)
                continue
            node.name = r["name"]
            node.gl_name = r["name"]
            node.gl_copy_of = ""
        gnodes[r["name"]] = {"name": r["name"], "type": node.gl_type}
        if source is not None and source.get("bypass"):
            edit(op="set_bypass", name=r["name"], bypass=True)
    # Links: what the tree has against what the graph has.
    current = _call("get_graph", session=session)["graph"]
    have = {}
    for c in current.get("connections", []):
        have[(c["to"], c["input"])] = (c["from"], c.get("output") or "")
    # (A link through reroutes is the link from where they lead back to: plugging another output
    # into a reroute feeds every input after it from that output.)
    want = {key: _source_of(a, identifier) for key, (a, identifier, _) in chains(tree).items()}
    # A node dropped on a link (Blender splices it in): the graph's insert_on_link, one step (Maya's
    # insert_on_wire), which picks the node's input as Maya does; the graph's links are then the truth.
    for (dst, inp, src, output, mid) in insertions(have, want, gnodes):
        args = {"op": "insert_on_link", "from": src, "to": dst, "input": inp, "node": mid}
        if output:
            args["output"] = output
        if edit(**args) is None:
            continue
        want = {k: v for k, v in want.items() if not ((k[0] == mid and v[0] == src) or (k == (dst, inp)))}
        current = _call("get_graph", session=session)["graph"]
        have = {(c["to"], c["input"]): (c["from"], c.get("output") or "") for c in current.get("connections", [])}
        for key in [k for k in have if k[0] == mid or k == (dst, inp)]:
            want[key] = have[key]
    for key, frm in have.items():
        if key not in want and key[0] in gnodes and frm[0] in gnodes:
            edit(op="disconnect", to=key[0], input=key[1])
    for key, frm in want.items():
        if have.get(key) != frm:
            args = {"op": "connect", "from": frm[0], "to": key[0], "input": key[1]}
            if frm[1]:
                args["output"] = frm[1]
            edit(**args)
    # Muted nodes: Bypass in the graph.
    bypassed = {n["name"] for n in current.get("nodes", []) if n.get("bypass")}
    for node in nodes:
        if node.gl_name and node.gl_name in gnodes and bool(node.mute) != (node.gl_name in bypassed):
            edit(op="set_bypass", name=node.gl_name, bypass=bool(node.mute))
    # The reroutes on the graph's links now are its pins (layout: with the positions). After a
    # refusal the tree is made again from the graph, pins as they were (a reroute fed from an output
    # its inputs cannot take keeps its own).
    pins = tree.get("gl_pins")
    if refused:
        _note(scalp, refused[-1])
        with building():
            tree.pop("gl_pins", None)
    else:
        _messages.pop(scalp.groomlab.uid, None)
        final = _call("get_graph", session=session)["graph"] if sent else current
        assign_pins(tree, connections_of(final))
        push_positions(scalp, tree, final)
    from . import graph
    tree["gl_sig"] = signature(tree)
    if not sent:  # only reroutes made, rewired or deleted on the same links: layout, nothing evaluates
        return tree.get("gl_pins") != pins
    groom.remember_graph(scalp, rt)
    graph.refresh(scalp, rt)  # the 3D view's list; the tree is rebuilt from the graph (pending)
    groom.mark(scalp, "eval")
    return True


def data_node(data, name):
    return next((n for n in data.get("nodes", []) if n.get("name") == name), None)


def insertions(have, want, known):
    """[(to, input, from, output, node)]: links the graph has (`have` {(to, input): (from, output)})
    that the tree now routes through another node (`want`): the link from -> to.input gone, `node`
    feeding to.input and taking from `from` on an input that did not before (a node dropped on it)."""
    found = []
    for (dst, inp), frm in have.items():
        now = want.get((dst, inp))
        if now is None or now == frm or dst not in known or frm[0] not in known:
            continue
        mid = now[0]
        if mid == frm[0] or mid == dst or mid not in known:
            continue
        if any(k[0] == mid and v[0] == frm[0] and have.get(k) != v for k, v in want.items()):
            found.append((dst, inp, frm[0], frm[1], mid))
    return found


def unique_name(base, taken):
    """`base` as a node name the graph does not have yet: letters, digits and _, a number added."""
    base = re.sub(r"\W", "_", base or "node") or "node"
    if base[0].isdigit():
        base = "n" + base
    name, k = base, 1
    while name in taken:
        name = "%s%d" % (base, k)
        k += 1
    return name


# ---- Edits the editor's operators make ----------------------------------------------------------

def set_output(scalp, name):
    rt = groom.ensure(scalp)
    push_positions(scalp)
    _call("edit_graph", session=rt.session, op="set_output", name=name)
    _edited(scalp, rt)


def upgrade_node(scalp, name):
    rt = groom.ensure(scalp)
    push_positions(scalp)
    upgraded = _call("edit_graph", session=rt.session, op="upgrade_node", name=name).get("upgraded", False)
    _edited(scalp, rt)
    return upgraded


def set_bypass(scalp, names, on):
    rt = groom.ensure(scalp)
    push_positions(scalp)
    for name in names:
        _call("edit_graph", session=rt.session, op="set_bypass", name=name, bypass=bool(on))
    _edited(scalp, rt)


def set_evaluation(scalp, names, mode):
    """When the nodes run: "always", "viewportOnly" (renders skip them) or "renderOnly" (the
    preview skips them)."""
    rt = groom.ensure(scalp)
    push_positions(scalp)
    for name in names:
        _call("edit_graph", session=rt.session, op="set_evaluation", name=name, mode=mode)
    _edited(scalp, rt)


def view_node(scalp, name):
    """The viewport shows `name`'s strands instead of the output's (renders keep the output); None
    or "" back to the output."""
    with props.quiet():
        scalp.groomlab.view_node = name or ""
    tree = tree_of(scalp)
    if tree is not None:
        with building():
            for n in tree.nodes:
                if ours(n):
                    n.gl_viewed = bool(name) and n.gl_name == name
    groom.mark(scalp, "eval")


def tidy(scalp):
    rt = groom.ensure(scalp)
    data = _call("get_graph", session=rt.session)["graph"]
    set_positions(scalp, auto_layout(data))
    rebuild(scalp, replaced=True)


def _edited(scalp, rt):
    from . import graph
    groom.remember_graph(scalp, rt)
    graph.refresh(scalp, rt)  # the tree follows (graph_changed)
    groom.mark(scalp, "eval")


# ---- Undo, files --------------------------------------------------------------------------------

def before_undo(*args):
    _state.undoing = True


def after_undo(*args):
    """Blender's undo restored the tree and the groom's graph text: the helper takes the graph back
    (with the tree's positions) and the tree is made again from it."""
    _state.undoing = False
    for scalp in groom.scalps():
        rt = groom.runtime_if_any(scalp)
        tree = tree_of(scalp)
        if rt is None or rt.session is None or rt.generation != process.generation() or not process.running():
            continue
        try:
            text = scalp.groomlab.graph_json
            if text:
                data = json.loads(text)
                if tree is not None:
                    by_name = {n.gl_name: n for n in tree.nodes if ours(n) and n.gl_name}
                    for gnode in data.get("nodes", []):
                        node = by_name.get(gnode.get("name"))
                        if node is not None and node.gl_type == gnode.get("type"):
                            at = ui_of(node)
                            gnode["ui"] = {"x": at[0], "y": at[1]}
                    by_pin = {pin_of(r): r for r in reroutes(tree) if pin_of(r)}
                    for point in (data.get("pins") or {}).get("points") or []:
                        if isinstance(point, dict) and point.get("id") in by_pin:
                            point["x"], point["y"] = ui_of(by_pin[point["id"]])
                current = _call("get_graph", session=rt.session)["graph"]
                if json.dumps(current, sort_keys=True) != json.dumps(data, sort_keys=True):
                    try:
                        _call("set_graph", session=rt.session, graph=data)
                    except HelperError as e:
                        _note(scalp, "undo: %s" % e.message)
            from . import graph
            mark_replaced(scalp)  # the positions as undone (the tree's, put in the graph above)
            graph.refresh(scalp, rt)  # the 3D view's list, and the tree (graph_changed)
            groom.mark(scalp, "eval")
        except HelperError as e:
            _note(scalp, e.message)


def before_save():
    """Positions into the graph before it is written (a .groom, the .blend's copy)."""
    for scalp in groom.scalps():
        rt = groom.runtime_if_any(scalp)
        if rt is None or rt.session is None or rt.generation != process.generation() or not process.running():
            continue
        if tree_of(scalp) is None:
            continue
        try:
            push_positions(scalp)
        except HelperError as e:
            print("GroomLab: %s" % e)


classes = tuple(SOCKET_CLASSES) + (GroomLabGraphTree,)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.app.handlers.undo_pre.append(before_undo)
    bpy.app.handlers.redo_pre.append(before_undo)
    bpy.app.handlers.undo_post.append(after_undo)
    bpy.app.handlers.redo_post.append(after_undo)
    groom.before_save.append(before_save)


def unregister():
    if before_save in groom.before_save:
        groom.before_save.remove(before_save)
    for name, fn in (("undo_pre", before_undo), ("redo_pre", before_undo), ("undo_post", after_undo),
                     ("redo_post", after_undo)):
        lst = getattr(bpy.app.handlers, name)
        if fn in lst:
            lst.remove(fn)
    if bpy.app.timers.is_registered(flush):
        bpy.app.timers.unregister(flush)
    unregister_node_classes()
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    _pending.clear()
    _replaced.clear()
