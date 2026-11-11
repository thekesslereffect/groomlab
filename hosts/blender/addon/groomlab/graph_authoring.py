# SPDX-License-Identifier: GPL-3.0-or-later
"""The Graph Editor's authoring, after Maya's GroomLab Graph Editor: Group into Compound / Ungroup
(Ctrl+G / Ctrl+Shift+G), a compound's inner params shown on it, params exposed as inputs, sliders
(Control nodes) and keyed params (Blender keys on the groom's ctl_ properties, given to the graph
on each frame), a param's map source (texture, color set, attribute, painted map, expression: the
expression in a text block, its errors shown), backdrops (Blender's frames), the node library (Save
as Node, Save to Library, Update Node, Import Node, Open Nodes Folder), script nodes (New Script
Node, edited in text blocks with live errors, Save to Library), Save as Preset, Copy Graph and
Paste Graph (the whole graph as text). Each edit goes to the helper (graph.py, docs/BLENDER.md
"Authoring"), the node tree follows (graph_tree.py)."""

import json
import os
import tempfile

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, StringProperty

from . import definitions as defs
from . import graph, graph_tree, graph_ui, groom, process, props
from .graph_tree import ours
from .graph_ui import ActionTip, action_prop, bind
from .helper_client import HelperError, sentence

CONTROL_PREFIX = "ctl_"   # a control's value on the groom (its scalp object): a keyable custom property
KEYABLE_KINDS = ("float", "int", "bool", "vec3")
TEXT_PREFIX = "GroomLab "  # the text blocks of expressions and script nodes
SHARED_CATEGORY = "Shared"
_clipboard = [""]          # Copy Graph's text where Blender has no clipboard (background)
_text_state = {}           # text name -> {"applied": text, "errors": [...]} (expressions, scripts)
_sent_controls = {}        # scalp uid -> {control: value} last given to the helper
_control_names = {}        # scalp uid -> (graph text, [control names])


def _ge():
    from . import graph_editor
    return graph_editor


def _fail(op, e):
    op.report({"ERROR"}, sentence(e.message if isinstance(e, HelperError) else str(e)))
    return {"CANCELLED"}


def _node(context, name=""):
    """The node named `name` in the groom's tree (the active one when empty)."""
    ge = _ge()
    if not name:
        return ge.active_node(context)
    tree = ge.context_tree(context)
    return next((n for n in tree.nodes if ours(n) and n.gl_name == name), None) if tree is not None else None


def _select(scalp, names):
    """The tree's selection: these nodes (the first active)."""
    tree = graph_tree.tree_of(scalp)
    if tree is None:
        return
    names = list(names)
    for n in tree.nodes:
        n.select = ours(n) and n.gl_name in names
        if names and ours(n) and n.gl_name == names[0]:
            tree.nodes.active = n


def _settle(scalp):
    """The tree made from the graph now (the pending rebuild)."""
    graph_tree.flush()


class _Op(ActionTip, bpy.types.Operator):
    """A Graph Editor edit of the groom's graph"""
    bl_options = {"REGISTER", "UNDO"}

    action: action_prop()

    @classmethod
    def poll(cls, context):
        return not _ge().foreign_editor(context) and _ge().context_groom(context) is not None


class _NodeOp(_Op):
    node: StringProperty(options={"SKIP_SAVE"}, description="The node (empty: the active one)")

    def target(self, context):
        node = _node(context, self.node)
        return node.gl_name if node is not None else self.node


class _ParamOp(_NodeOp):
    param: StringProperty(options={"SKIP_SAVE"})


# ---- Compounds ----------------------------------------------------------------------------------

class GROOMLAB_OT_graph_group(_Op):
    """Collapse the selected nodes into one Compound node (Ctrl+G)"""
    bl_idname = "groomlab.graph_group"
    bl_label = "Group into Compound"
    action_id = "graph.group"

    names: StringProperty(options={"SKIP_SAVE"}, description="The nodes, comma separated (empty: the selected ones)")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        names = [n for n in self.names.split(",") if n] or [n.gl_name for n in _ge().selected_nodes(context)]
        if not names:
            self.report({"INFO"}, "Select the nodes to group")
            return {"CANCELLED"}
        try:
            made = graph.group(scalp, names)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        _select(scalp, [made])
        self.report({"INFO"}, "Grouped %d nodes into %s (Ctrl+Shift+G ungroups it)" % (len(names), made))
        return {"FINISHED"}


class GROOMLAB_OT_graph_ungroup(_NodeOp):
    """Put the Compound's nodes back in the graph, to see and edit them (Ctrl+Shift+G; Ctrl+Z groups them again)"""
    bl_idname = "groomlab.graph_ungroup"
    bl_label = "Ungroup Compound"
    action_id = "graph.ungroup"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        names = [self.node] if self.node else [n.gl_name for n in _ge().selected_nodes(context) if n.gl_type == "Compound"]
        if not names:
            self.report({"INFO"}, "Select the Compound nodes to ungroup")
            return {"CANCELLED"}
        inner = []
        for name in names:
            try:
                inner += graph.ungroup(scalp, name)
            except HelperError as e:
                return _fail(self, e)
        _settle(scalp)
        _select(scalp, inner)
        return {"FINISHED"}


def inner_params(node):
    """[(inner node, [param spec])] a Compound's inner nodes offer to show on it (Maya's innerParams)."""
    definition = graph_tree.values(node).get("definition") or {}
    specs = graph.operator_specs()
    out = []
    for inner in (definition.get("graph") or {}).get("nodes", []):
        if inner.get("type") == "Input":
            continue
        params = [p for p in (specs.get(inner.get("type")) or {}).get("params", [])
                  if p.get("kind") in ("float", "int", "bool", "vec3", "string") and p["name"] != "definition"
                  and not p.get("channel")]
        if params:
            out.append((inner["name"], params))
    return out


def shown_inner(node):
    definition = graph_tree.values(node).get("definition") or {}
    return set((definition.get("expose") or {}).values())


class GROOMLAB_OT_graph_show_inner(_NodeOp):
    """The inner node's param on the compound (again: no longer)"""
    bl_idname = "groomlab.graph_show_inner"
    bl_label = "Show Inner Param"
    action_id = "compound.showInner"

    target_param: StringProperty(name="Inner Param", description="node.param inside the compound")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None or node.gl_type != "Compound":
            return {"CANCELLED"}
        try:
            if self.target_param in shown_inner(node):
                graph.compound_unexpose(scalp, node.gl_name, self.target_param)
            else:
                inner, _, param = self.target_param.partition(".")
                taken = {p["name"] for p in graph_tree.node_param_specs(node)}
                name = param if param not in taken else inner + param[:1].upper() + param[1:]
                graph.compound_expose(scalp, node.gl_name, name, self.target_param)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        return {"FINISHED"}


# ---- Exposed params, sliders, keys --------------------------------------------------------------

def exposed(node):
    return set(json.loads(node.gl_inputs or "[]"))


def linkable(node):
    """The params Expose Param as Input offers (those with a port)."""
    return [p["name"] for p in graph_tree.node_param_specs(node) if p.get("port")]


class GROOMLAB_OT_graph_expose(_ParamOp):
    """The param as an input port: a field or value node can drive it (again: the port goes, the param keeps its value)"""
    bl_idname = "groomlab.graph_expose"
    bl_label = "Expose Param as Input"
    action_id = "node.expose"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None or not self.param:
            return {"CANCELLED"}
        try:
            if self.param in exposed(node):
                graph.unexpose_param(scalp, node.gl_name, self.param)
            else:
                graph.expose_param(scalp, node.gl_name, self.param)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        return {"FINISHED"}


def ramp_preset_keys(name):
    """A curve preset's keys (engine/ui/choices.json's rampPresets, as every host offers them)."""
    from . import definitions
    c = definitions.choice("rampPresets", name)
    return [list(k) for k in c["keys"]] if c else None


def flipped_ramp(value):
    """A ramp value mirrored end to end (each key at 1 - its position), straight or smooth as it was."""
    keys, linear = graph_tree.ramp_keys(value)
    out = [[1.0 - float(p), v] for p, v in reversed(keys)]
    return {"keys": out, "linear": True} if linear else out


class GROOMLAB_OT_graph_ramp_preset(_ParamOp):
    """Replace the curve with a preset, or mirror it end to end (Flip)"""
    bl_idname = "groomlab.graph_ramp_preset"
    bl_label = "Curve Preset"
    action_id = "param.rampPreset"

    preset: StringProperty(options={"SKIP_SAVE"}, description="A preset's name (rampPresets), or flip")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None or not self.param:
            return {"CANCELLED"}
        curve = graph_tree.ramp_node(node, self.param)
        now = graph_tree.ramp_value(curve) if curve is not None else graph_tree.values(node).get(self.param)
        _, linear = graph_tree.ramp_keys(now)
        if self.preset == "flip":
            value = flipped_ramp(now)
        else:
            keys = ramp_preset_keys(self.preset)
            if keys is None:
                return {"CANCELLED"}
            value = {"keys": keys, "linear": True} if linear else keys  # the curve keeps its segments
        if not graph_tree.set_param_value(scalp, node, self.param, value):
            return {"CANCELLED"}
        graph_tree.write_ramp(node, self.param, value)
        _settle(scalp)
        return {"FINISHED"}


class GROOMLAB_OT_graph_toggle_inputs(_NodeOp):
    """Expose all connectable parameter inputs, or collapse them when all are exposed."""
    bl_idname = "groomlab.graph_toggle_inputs"
    bl_label = "Expand Inputs"
    bl_description = "Show or hide all connectable parameter ports"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        params = linkable(node)
        show = not all(param in exposed(node) for param in params)
        connected = {c["input"] for c in json.loads(scalp.groomlab.graph_json or "{}").get("connections", [])
                     if c.get("to") == node.gl_name}
        try:
            for param in params:
                if (param in exposed(node)) != show:
                    if show or param not in connected:
                        (graph.expose_param if show else graph.unexpose_param)(scalp, node.gl_name, param)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        return {"FINISHED"}


def control_names(node, param):
    """The control(s) a keyed param reads ([] when not keyed): one, or three for a vector."""
    value = json.loads(node.gl_controls or "{}").get(param)
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [v for v in value if isinstance(v, str)]
    return []


def control_path(name):
    return '["%s%s"]' % (CONTROL_PREFIX, name)


def _channelbag(scalp):
    ad = scalp.animation_data
    if ad is None or ad.action is None:
        return None
    try:
        from bpy_extras import anim_utils
        return anim_utils.animdata_get_channelbag_for_assigned_slot(ad)
    except (ImportError, AttributeError):
        return ad.action  # (Blender before 4.4: the action has the fcurves)


def fcurve(scalp, name):
    bag = _channelbag(scalp)
    return bag.fcurves.find(control_path(name)) if bag is not None else None


def key_state(scalp, node, param):
    """"keyed" (a key on this frame), "animated" (keys, none here), "linked" (no keys), "driven" (a
    Control node feeds it), or None (graph_ui.json's keyStates)."""
    names = control_names(node, param)
    if not names:
        driver = json.loads(node.gl_drivers or "{}").get(param)
        tree = node.id_data
        src = next((n for n in tree.nodes if ours(n) and n.gl_name == driver), None) if driver else None
        return "driven" if src is not None and src.gl_type == "Control" else None
    frame = groom.scene_of(scalp).frame_current
    curves = [c for c in (fcurve(scalp, n) for n in names) if c is not None]
    if not curves:
        return "linked"
    if any(any(abs(k.co[0] - frame) < 0.5 for k in c.keyframe_points) for c in curves):
        return "keyed"
    return "animated"


def keyable_why(node, param, spec):
    """Why a param cannot be keyed ("" when it can), as Maya's GraphModel.keyable says it."""
    if spec is None or spec.get("kind") not in KEYABLE_KINDS:
        return "only numbers, switches and vectors are keyed"
    if spec.get("animatable") is False:
        return "a count or a seed changes the strands themselves"
    if node.gl_type == "Control":
        return "a Control node is its slider"
    if param in json.loads(node.gl_mapped or "{}"):
        return "it reads a map"
    if param in json.loads(node.gl_drivers or "{}"):
        return "an input drives it"
    return ""


def ensure_control_props(scalp, names, values, spec=None):
    """The groom's ctl_ properties for these controls, with their values when new."""
    for name, value in zip(names, values):
        key = CONTROL_PREFIX + name
        if key not in scalp.keys():
            scalp[key] = float(value) if not isinstance(value, bool) else float(value)
            try:
                ui = scalp.id_properties_ui(key)
                kw = {"description": "GroomLab control %s (the graph's own units: centimeters for lengths)" % name}
                if spec is not None:
                    for src, dst in (("softMin", "soft_min"), ("softMax", "soft_max"), ("min", "min"), ("max", "max")):
                        if isinstance(spec.get(src), (int, float)):
                            kw[dst] = float(spec[src])
                ui.update(**kw)
            except (TypeError, AttributeError):
                pass


def _param_value(scalp, node, param):
    try:
        return process.call("get_param", session=groom.ensure(scalp).session, node=node, param=param)["value"]
    except HelperError:
        return None


def link_param(scalp, node, param):
    """Key node.param in place: linked to a control of its own (link_control), its ctl_ property(ies)
    made with the value now. Returns the control names."""
    tree_node = next((n for n in graph_tree.tree_of(scalp).nodes if ours(n) and n.gl_name == node), None)
    names = control_names(tree_node, param) if tree_node is not None else []
    if names:
        return names
    value = _param_value(scalp, node, param)
    made = graph.link_control(scalp, node, param)
    names = [made] if isinstance(made, str) else list(made)
    values = value if isinstance(value, list) else [value]
    spec = tree_node.gl_param_specs.get(param) if tree_node is not None and not tree_node.gl_spec_json else None
    ensure_control_props(scalp, names, [v if isinstance(v, (int, float)) else 0.0 for v in values], spec)
    _settle(scalp)
    push_controls(scalp, force=True)
    return names


class GROOMLAB_OT_graph_set_key(_ParamOp):
    """Key this param at the current frame: it reads a control of its own, the groom's ctl_ property, keyed in Blender"""
    bl_idname = "groomlab.graph_set_key"
    bl_label = "Set Key"
    action_id = "param.setKey"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        why = keyable_why(node, self.param, (node.gl_param_specs or {}).get(self.param) if not node.gl_spec_json else
                          next((s for s in graph_tree.node_param_specs(node) if s["name"] == self.param), None))
        if why and not control_names(node, self.param):
            self.report({"WARNING"}, "Cannot be keyed: %s" % why)
            return {"CANCELLED"}
        try:
            names = link_param(scalp, node.gl_name, self.param)
        except HelperError as e:
            return _fail(self, e)
        frame = context.scene.frame_current
        for name in names:
            scalp.keyframe_insert(control_path(name), frame=frame, group="GroomLab")
        return {"FINISHED"}


class GROOMLAB_OT_graph_delete_key(_ParamOp):
    """Delete this param's key at the current frame"""
    bl_idname = "groomlab.graph_delete_key"
    bl_label = "Delete Key"
    action_id = "param.deleteKey"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        for name in control_names(node, self.param):
            try:
                scalp.keyframe_delete(control_path(name), frame=context.scene.frame_current)
            except RuntimeError:
                pass
        return {"FINISHED"}


class GROOMLAB_OT_graph_remove_animation(_ParamOp):
    """Delete its keys and keep its value now as the param's own"""
    bl_idname = "groomlab.graph_remove_animation"
    bl_label = "Remove Animation"
    action_id = "param.removeAnimation"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        names = control_names(node, self.param)
        if not names:
            return {"CANCELLED"}
        push_controls(scalp, force=True)  # the value now is the one kept
        try:
            graph.unlink_control(scalp, node.gl_name, self.param)
        except HelperError as e:
            return _fail(self, e)
        bag = _channelbag(scalp)
        for name in names:
            curve = bag.fcurves.find(control_path(name)) if bag is not None else None
            if curve is not None:
                bag.fcurves.remove(curve)
            if CONTROL_PREFIX + name in scalp.keys():
                del scalp[CONTROL_PREFIX + name]
        _settle(scalp)
        return {"FINISHED"}


class GROOMLAB_OT_graph_add_control(_ParamOp):
    """A Control node: this param driven by a slider, the groom's ctl_ property (keyable), e.g. to animate a Blend from dry to wet"""
    bl_idname = "groomlab.graph_add_control"
    bl_label = "Drive with Slider"
    action_id = "param.driveWithSlider"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        value = _param_value(scalp, node.gl_name, self.param)
        spec = node.gl_param_specs.get(self.param) if not node.gl_spec_json else None
        lo, hi = 0.0, 1.0
        if spec is not None:
            lo = float(spec.get("softMin", spec.get("min", 0.0)))
            hi = float(spec.get("softMax", spec.get("max", 1.0)))
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            lo, hi = min(lo, float(value)), max(hi, float(value))
        try:
            control = graph.add_control(scalp, node.gl_name, self.param,
                                        default=value if isinstance(value, (int, float)) else None, min=lo, max=hi)
        except HelperError as e:
            return _fail(self, e)
        ensure_control_props(scalp, [control], [value if isinstance(value, (int, float)) else lo],
                             {"min": lo, "max": hi})
        _settle(scalp)
        self.report({"INFO"}, "%s.%s follows the slider %s (the groom's %s%s property: key it with I)"
                    % (node.gl_name, self.param, control, CONTROL_PREFIX, control))
        return {"FINISHED"}


def graph_controls(scalp):
    """The control names the graph reads (controls_list), read again when the graph changed."""
    uid = scalp.groomlab.uid
    text = scalp.groomlab.graph_json
    cached = _control_names.get(uid)
    if cached is None or cached[0] != text:
        try:
            names = [c["name"] for c in graph.controls(scalp)]
        except HelperError:
            names = []
        cached = _control_names[uid] = (text, names)
    return cached[1]


def push_controls(scalp, force=False):
    """The groom's ctl_ properties (as Blender's keys have them on this frame) given to the graph's
    controls (set_controls) when they changed. True when any was."""
    rt = groom.runtime_if_any(scalp)
    if rt is None or rt.session is None or rt.generation != process.generation() or not process.running():
        return False
    keys = [k for k in scalp.keys() if k.startswith(CONTROL_PREFIX)]
    if not keys:
        return False
    wanted = set(graph_controls(scalp))
    values = {}
    for k in keys:
        name = k[len(CONTROL_PREFIX):]
        v = scalp[k]
        if name in wanted and isinstance(v, (int, float)):
            values[name] = float(v)
    uid = scalp.groomlab.uid
    sent = (process.generation(), rt.session, values)  # (a new session or helper is given them again)
    if not force and sent == _sent_controls.get(uid):
        return False
    try:
        graph.set_controls(scalp, values)
    except HelperError as e:
        print("GroomLab: %s" % e)
        return False
    _sent_controls[uid] = sent
    return True


def _push_all_controls(*_args):
    try:
        for scalp in groom.scalps():
            push_controls(scalp)
    except Exception as e:  # noqa: BLE001 - a handler must not raise
        print("GroomLab controls: %r" % e)


# ---- Map sources: texture, color set, attribute, painted map, expression --------------------------

MAP_SOURCES = (("texture", "Texture", "An image at the scalp's UVs (a file path)"),
               ("colorSet", "Color Set", "A color set (Blender: a color attribute) of the scalp"),
               ("attribute", "Attribute", "A strand attribute a node upstream writes"),
               ("guide", "Painted Map", "A map painted in Groom Tools > Paint (on the guides or the scalp)"),
               ("expression", "Expression", "A formula per strand (u, v, t, rand, the params...)"))
MAP_KEYS = tuple(s[0] for s in MAP_SOURCES)
COMPONENTS = (("DEFAULT", "Default", "The map's own value (a texture: its luminance)"), ("r", "R", ""), ("g", "G", ""),
              ("b", "B", ""), ("a", "A", ""), ("luminance", "Luminance", ""))


def map_source(value):
    """(source, name) of a map value; (None, "") for a plain value."""
    if not isinstance(value, dict):
        return None, ""
    source = next((k for k in MAP_KEYS if k in value), None)
    return source, value.get(source, "") if source else ""


def set_map(scalp, node, param, value):
    """A param's value (a map, or a plain value again): set_param (checked by the helper: a bad
    expression raises HelperError "expression" with where), the node and panel follow."""
    graph.set_param(scalp, node, param, value)


class GROOMLAB_OT_graph_map_source(_ParamOp):
    """Feed this param from a map: a texture, a color set, an attribute, a painted map or an expression"""
    bl_idname = "groomlab.graph_map_source"
    bl_label = "Map Source"

    source: EnumProperty(name="Source", items=MAP_SOURCES, default="guide")
    name: StringProperty(name="Name", description="The texture's file, the color set, attribute or painted map's name")
    component: EnumProperty(name="Channel", items=COMPONENTS, default="DEFAULT")
    low: FloatProperty(name="Min", default=0.0, description="The value at the map's black (0)")
    high: FloatProperty(name="Max", default=1.0, description="The value at the map's white (1)")
    clear: BoolProperty(options={"SKIP_SAVE"}, description="Back to a plain value (the param's default)")

    def invoke(self, context, event):
        node = _node(context, self.node)
        value = graph_tree.values(node).get(self.param) if node is not None else None
        source, name = map_source(value)
        if source:
            self.source = source
            self.name = name if isinstance(name, str) else ""
            self.component = value.get("component") if value.get("component") in [c[0] for c in COMPONENTS] else "DEFAULT"
            self.low = float(value.get("min", 0.0))
            self.high = float(value.get("max", 1.0))
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "source")
        if self.source == "expression":
            layout.label(text="Edit the code in its text block (Edit Expression)", icon="TEXT")
        else:
            layout.prop(self, "name")
            layout.prop(self, "component")
        row = layout.row(align=True)
        row.prop(self, "low")
        row.prop(self, "high")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None:
            return {"CANCELLED"}
        try:
            if self.clear:
                spec = next((s for s in graph_tree.node_param_specs(node) if s["name"] == self.param), {})
                set_map(scalp, node.gl_name, self.param, spec.get("default"))
                return {"FINISHED"}
            value = {"min": round(float(self.low), 6), "max": round(float(self.high), 6)}  # (Blender's floats are single)
            if self.source == "expression":
                old = graph_tree.values(node).get(self.param)
                value["expression"] = old["expression"] if isinstance(old, dict) and old.get("expression") else (self.name or "1.0")
            else:
                if not self.name:
                    self.report({"WARNING"}, "Name the %s to read" % dict((s[0], s[1].lower()) for s in MAP_SOURCES)[self.source])
                    return {"CANCELLED"}
                value[self.source] = self.name
                if self.component != "DEFAULT":
                    value["component"] = self.component
            set_map(scalp, node.gl_name, self.param, value)
        except HelperError as e:
            return _fail(self, e)
        if self.source == "expression":
            edit_expression(scalp, node.gl_name, self.param, show=True)
        return {"FINISHED"}


def expression_text_name(node, param):
    return "%s%s.%s.expr" % (TEXT_PREFIX, node, param)


def edit_expression(scalp, node, param, show=False):
    """The expression of node.param in a text block of its own (made, or written again from the
    graph); shown in a Text Editor of the window when there is one. Returns the text."""
    tree_node = next((n for n in graph_tree.tree_of(scalp).nodes if ours(n) and n.gl_name == node), None)
    value = graph_tree.values(tree_node).get(param) if tree_node is not None else None
    code = value.get("expression", "") if isinstance(value, dict) else ""
    name = expression_text_name(node, param)
    text = bpy.data.texts.get(name) or bpy.data.texts.new(name)
    if text.as_string() != code:
        text.from_string(code)
    text["gl_kind"] = "expression"
    text["gl_groom"] = scalp.groomlab.uid
    text["gl_node"] = node
    text["gl_param"] = param
    _text_state[name] = {"applied": code, "errors": []}
    if show:
        show_text(text)
    _watch()
    return text


def apply_expression(text):
    """The text block's code as its param's expression (the map's range kept); False and the error
    kept (text_errors) when the helper refuses it."""
    scalp = groom.find(text.get("gl_groom", ""))
    node, param = text.get("gl_node", ""), text.get("gl_param", "")
    code = text.as_string().strip()
    state = _text_state.setdefault(text.name, {"applied": None, "errors": []})
    if scalp is None or not node:
        return False
    tree_node = next((n for n in graph_tree.tree_of(scalp).nodes if ours(n) and n.gl_name == node), None)
    old = graph_tree.values(tree_node).get(param) if tree_node is not None else None
    value = {k: v for k, v in (old or {}).items() if k not in MAP_KEYS} if isinstance(old, dict) else {}
    value["expression"] = code
    try:
        set_map(scalp, node, param, value)
    except HelperError as e:
        state["applied"], state["errors"] = text.as_string(), [{"message": e.message}]
        return False
    state["applied"], state["errors"] = text.as_string(), []
    return True


def show_text(text):
    """The text block in a Text Editor of the window, when one is open (nothing is opened)."""
    wm = getattr(bpy.context, "window_manager", None)
    for window in (wm.windows if wm is not None else []):
        for area in window.screen.areas:
            if area.type == "TEXT_EDITOR":
                area.spaces.active.text = text
                area.tag_redraw()
                return True
    return False


def text_errors(text):
    return (_text_state.get(text.name) or {}).get("errors", [])


# ---- Script nodes ---------------------------------------------------------------------------------

def script_text_names(node):
    return "%s%s.glscript" % (TEXT_PREFIX, node), "%s%s.header.json" % (TEXT_PREFIX, node)


def edit_script(scalp, node, show=False):
    """A script node's file in two text blocks: its script (the code) and its header (the rest, as
    JSON); checked as they change (script_errors) and taken by the node when they check (live, as
    Maya's script editor). Returns (script text, header text)."""
    tree_node = next((n for n in graph_tree.tree_of(scalp).nodes if ours(n) and n.gl_name == node), None)
    definition = dict(graph_tree.values(tree_node).get("definition") or {}) if tree_node is not None else {}
    code = definition.pop("script", "")
    names = script_text_names(node)
    texts = []
    for name, content in zip(names, (code, json.dumps(definition, indent=2))):
        text = bpy.data.texts.get(name) or bpy.data.texts.new(name)
        if text.as_string() != content:
            text.from_string(content)
        text["gl_kind"] = "script"
        text["gl_groom"] = scalp.groomlab.uid
        text["gl_node"] = node
        texts.append(text)
    _text_state[names[0]] = {"applied": script_definition_text(texts[0], texts[1]), "errors": []}
    if show:
        show_text(texts[0])
    _watch()
    return texts[0], texts[1]


def script_definition_text(script, header):
    return header.as_string() + "\n\x00\n" + script.as_string()


def script_definition(script, header):
    """The node file the two text blocks make (ValueError: the header is not JSON)."""
    data = json.loads(header.as_string() or "{}")
    if not isinstance(data, dict):
        raise ValueError("the header is not a JSON object")
    data["script"] = script.as_string()
    return data


def check_script(scalp, node, take=True):
    """The script node's text blocks checked (script_errors); taken by the node when they check
    (`take`). Returns the errors ([] when it checks)."""
    script_name, header_name = script_text_names(node)
    script, header = bpy.data.texts.get(script_name), bpy.data.texts.get(header_name)
    if script is None or header is None:
        return []
    state = _text_state.setdefault(script_name, {"applied": None, "errors": []})
    state["applied"] = script_definition_text(script, header)
    try:
        definition = script_definition(script, header)
    except ValueError as e:
        state["errors"] = [{"part": "header", "message": "the header is not JSON: %s" % e}]
        return state["errors"]
    try:
        errors = graph.script_errors(definition)
        if not errors and take:
            graph.set_script(scalp, node, definition)
            _settle(scalp)
    except HelperError as e:
        errors = [{"part": "script", "message": e.message}]
    state["errors"] = errors
    return errors


def script_errors_of(node):
    return (_text_state.get(script_text_names(node)[0]) or {}).get("errors", [])


def editing_script(node):
    return bpy.data.texts.get(script_text_names(node)[0]) is not None


# ---- Live text blocks: checked as they change -------------------------------------------------------

def check_texts():
    """Expressions and scripts whose text block changed since last checked: applied (their errors
    kept). The watch timer's work; tests call it."""
    for text in list(bpy.data.texts):
        kind = text.get("gl_kind")
        if kind == "expression":
            state = _text_state.get(text.name)
            if state is not None and state.get("applied") != text.as_string():
                apply_expression(text)
        elif kind == "script" and text.name.endswith(".glscript"):
            node = text.get("gl_node", "")
            scalp = groom.find(text.get("gl_groom", ""))
            header = bpy.data.texts.get(script_text_names(node)[1])
            state = _text_state.get(text.name)
            if scalp is not None and header is not None and state is not None and \
                    state.get("applied") != script_definition_text(text, header):
                check_script(scalp, node)


def _watch_tick():
    try:
        check_texts()
    except Exception as e:  # noqa: BLE001 - a timer must not raise
        print("GroomLab: %r" % e)
    if not any(t.get("gl_kind") for t in bpy.data.texts):
        return None
    graph_tree._redraw()
    return 0.6


def _watch():
    if bpy.app.background or bpy.app.timers.is_registered(_watch_tick):
        return
    bpy.app.timers.register(_watch_tick, first_interval=0.6)


class GROOMLAB_OT_graph_edit_expression(_ParamOp):
    """Edit this param's expression in a text block (a Text Editor shows it): checked as you type, errors in the panel"""
    bl_idname = "groomlab.graph_edit_expression"
    bl_label = "Edit Expression"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        edit_expression(scalp, self.target(context), self.param, show=True)
        self.report({"INFO"}, "The expression is in the text block %s" % expression_text_name(self.target(context), self.param))
        return {"FINISHED"}


class GROOMLAB_OT_graph_apply_text(_Op):
    """Check the text block now and apply it (an expression, a script node)"""
    bl_idname = "groomlab.graph_apply_text"
    bl_label = "Apply"

    text: StringProperty()

    def execute(self, context):
        text = bpy.data.texts.get(self.text)
        if text is None:
            return {"CANCELLED"}
        if text.get("gl_kind") == "expression":
            ok = apply_expression(text)
            errors = text_errors(text)
        else:
            errors = check_script(groom.find(text.get("gl_groom", "")), text.get("gl_node", ""))
            ok = not errors
        if not ok:
            self.report({"WARNING"}, errors[0]["message"] if errors else "not applied")
        return {"FINISHED"}


class GROOMLAB_OT_graph_script_edit(_NodeOp):
    """Edit this script node: its script and header in text blocks, checked and taken live"""
    bl_idname = "groomlab.graph_script_edit"
    bl_label = "Edit Node..."
    action_id = "script.edit"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None or node.gl_type != "Script":
            return {"CANCELLED"}
        script, _header = edit_script(scalp, node.gl_name, show=True)
        self.report({"INFO"}, "The script is in the text block %s (its header beside it)" % script.name)
        return {"FINISHED"}


class GROOMLAB_OT_graph_script_done(_NodeOp):
    """Stop editing the script node (its text blocks removed; the node keeps what it took)"""
    bl_idname = "groomlab.graph_script_done"
    bl_label = "Done"
    bl_options = {"REGISTER"}

    def execute(self, context):
        for name in script_text_names(self.target(context)):
            text = bpy.data.texts.get(name)
            if text is not None:
                bpy.data.texts.remove(text)
            _text_state.pop(name, None)
        return {"FINISHED"}


class GROOMLAB_OT_graph_script_save(_NodeOp):
    """Write this script node to your node library (its file, one version up when the library has it)"""
    bl_idname = "groomlab.graph_script_save"
    bl_label = "Save to Library"
    action_id = "script.saveToLibrary"

    as_new: BoolProperty(name="As a New Node", options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = _ge().context_groom(context)
        name = self.target(context)
        if editing_script(name) and check_script(scalp, name):
            self.report({"WARNING"}, "Not saved: the script does not check (see the panel)")
            return {"CANCELLED"}
        try:
            path = graph.save_script_node(scalp, name, as_new=self.as_new)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        self.report({"INFO"}, "Saved %s to %s" % (name, path))
        return {"FINISHED"}


class GROOMLAB_OT_graph_new_script(_Op):
    """A node of your own, written as a short script (per strand, per point), after the selected node: edited in text blocks, live"""
    bl_idname = "groomlab.graph_new_script"
    bl_label = "New Script Node"
    action_id = "node.newScript"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _ge().active_node(context)
        after = node.gl_name if node is not None and node.select and not node.gl_is_output and _ge()._strands_output(node) else None
        try:
            name = graph.add_script_node(scalp, after=after)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        _select(scalp, [name])
        edit_script(scalp, name, show=True)
        return {"FINISHED"}


# ---- The node library -------------------------------------------------------------------------------

def library_info(node):
    return json.loads(node.gl_library_info or "{}") if node is not None else {}


class GROOMLAB_OT_graph_save_node(_NodeOp):
    """Save this compound to your node library: it shows in the Add menu and Tab search, in every groom, in Maya and the standalone app"""
    bl_idname = "groomlab.graph_save_node"
    bl_label = "Save as Node..."
    action_id = "compound.saveAsNode"

    label: StringProperty(name="Label", description="The node's name in the Add menu")
    category: StringProperty(name="Category", default="Compound", description="The Add menu's category it shows in")
    about: StringProperty(name="Description", description="What it does (its tooltip)")
    author: StringProperty(name="Author")
    node_id: StringProperty(name="Id", description="Its id in the library (empty: made from the label)")

    def invoke(self, context, event):
        node = _node(context, self.node)
        if node is not None and not self.label:
            info = library_info(node)
            self.label = json.loads(node.gl_spec_json or "{}").get("label") or node.gl_name
            self.category = info.get("category") or self.category
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        if node is None or node.gl_type != "Compound":
            return {"CANCELLED"}
        header = {"label": self.label or node.gl_name, "category": self.category or None,
                  "description": self.about or None, "author": self.author or None, "node_id": self.node_id or None}
        try:
            path = graph.save_node(scalp, node.gl_name, **header)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        self.report({"INFO"}, "Saved %s to %s" % (header["label"], path))
        return {"FINISHED"}


class GROOMLAB_OT_graph_save_to_library(_NodeOp):
    """Write this node as it is now (its values as the new defaults) over its library file, one version up"""
    bl_idname = "groomlab.graph_save_to_library"
    bl_label = "Save to Library"
    action_id = "compound.saveToLibrary"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        node = _node(context, self.node)
        info = library_info(node)
        if node is None or not info:
            return {"CANCELLED"}
        version = max(int(info.get("version", 0)), int(info.get("latest", 0))) + 1
        try:
            path = graph.save_node(scalp, node.gl_name, node_id=info.get("id"), version=version)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        self.report({"INFO"}, "Saved %s version %d to %s" % (info.get("id"), version, path))
        return {"FINISHED"}


class GROOMLAB_OT_graph_update_node(_NodeOp):
    """Take the library's newer copy of this node (its values kept where it still has them)"""
    bl_idname = "groomlab.graph_update_node"
    bl_label = "Update Node"
    action_id = "node.update"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        name = self.target(context)
        rt = groom.ensure(scalp)
        try:
            graph_tree.push_positions(scalp)
            done = process.call("edit_graph", session=rt.session, op="update_library_node", name=name).get("updated")
        except HelperError as e:
            return _fail(self, e)
        graph_tree._edited(scalp, rt)
        self.report({"INFO"}, "%s updated to the library's version" % name if done else "%s is the library's version" % name)
        return {"FINISHED"}


class GROOMLAB_OT_graph_import_node(_Op):
    """Copy a .glnode file (a node someone shared) into your node library and add it to the graph"""
    bl_idname = "groomlab.graph_import_node"
    bl_label = "Import Node..."
    action_id = "node.import"

    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.glnode", options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        scalp = _ge().context_groom(context)
        try:
            r = process.call("import_node", path=bpy.path.abspath(self.filepath))
            graph.library_nodes(refresh=True)
            node = _ge().active_node(context)
            made, _ = graph.add_node(scalp, "node:" + r["id"], after=node.gl_name if node is not None else None)
        except HelperError as e:
            return _fail(self, e)
        _settle(scalp)
        _select(scalp, [made])
        self.report({"INFO"}, "Imported %s (%s) into %s" % (r.get("label") or r["id"], made, os.path.dirname(r["path"])))
        return {"FINISHED"}


class GROOMLAB_OT_graph_open_nodes_folder(bpy.types.Operator):
    """Your node library's folder (Documents/GroomLab/nodes): Maya and the standalone app read it too"""
    bl_idname = "groomlab.graph_open_nodes_folder"
    bl_label = "Open Nodes Folder"

    action: action_prop()

    def execute(self, context):
        try:
            folder = graph.nodes_folder()["userDir"]
        except HelperError as e:
            return _fail(self, e)
        os.makedirs(folder, exist_ok=True)
        if not bpy.app.background:
            bpy.ops.wm.path_open(filepath=folder)
        self.report({"INFO"}, folder)
        return {"FINISHED"}


class GROOMLAB_OT_graph_save_preset(_Op):
    """Save this groom's graph (its layout and backdrops too) as a preset of your own, in the folder Maya's presets use"""
    bl_idname = "groomlab.graph_save_preset"
    bl_label = "Save as Preset..."
    bl_options = {"REGISTER"}
    action_id = "preset.save"

    name: StringProperty(name="Name", description="The preset's file name (letters, digits, _ and -)")
    title: StringProperty(name="Title")
    about: StringProperty(name="Description")
    category: StringProperty(name="Category", description="The Presets menu's category (empty: your own, last)")
    overwrite: BoolProperty(name="Replace", description="Replace a preset of the same name")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = _ge().context_groom(context)
        if not self.name:
            self.report({"WARNING"}, "Name the preset")
            return {"CANCELLED"}
        about = {k: v for k, v in (("title", self.title), ("description", self.about), ("category", self.category)) if v}
        try:
            path = graph.save_preset(scalp, self.name, overwrite=self.overwrite, about=about or None)
            _ge().preset_catalog(refresh=True)
        except HelperError as e:
            if e.code == "exists":
                self.report({"WARNING"}, "A preset named %s exists: tick Replace to replace it" % self.name)
                return {"CANCELLED"}
            return _fail(self, e)
        self.report({"INFO"}, "Saved preset %s (%s)" % (self.name, path))
        return {"FINISHED"}


# ---- Copy Graph / Paste Graph --------------------------------------------------------------------

def scalp_size(scalp):
    """The scalp's size as presets record it (its largest extent in centimeters, groom space)."""
    rt = groom.ensure(scalp)
    got = process.call("get_scalp", session=rt.session, space="groom")
    if not got.get("scalp"):
        return None
    pts = process.client().array(got["arrays"]["points"])
    if not len(pts):
        return None
    size = float((pts.max(axis=0) - pts.min(axis=0)).max())
    return size if size > 0.0 else None


def copy_text(scalp):
    """The groom's look as text (Maya's copy_graph_text): the preset Save as Preset writes (the
    graph, the scalp's size, the layout, the backdrops), titled after the groom, with its control
    values. Pasted in Maya, the app or Blender, or saved as a .json preset."""
    graph_tree.push_positions(scalp)
    rt = groom.ensure(scalp)
    g = dict(process.call("get_graph", session=rt.session)["graph"])
    data = {"version": g.pop("version")} if "version" in g else {}
    size = scalp_size(scalp)
    if size:
        data["size"] = size
    data["about"] = {"title": scalp.name, "category": SHARED_CATEGORY,
                     "description": "Copied from %s (GroomLab for Blender)" % scalp.name}
    backdrops = g.pop("backdrops", [])
    data.update(g)
    if backdrops:
        data["backdrops"] = backdrops
    values = {c["name"]: c["value"] for c in graph.controls(scalp) if c.get("set") and isinstance(c.get("value"), (int, float))}
    if values:
        data["controls"] = values
    return json.dumps(data, indent=2)


def _unfenced(text):
    text = text.strip()
    if text.startswith("```"):
        first, _, rest = text.partition("\n")
        if rest.rstrip().endswith("```") and len(first.strip("`").strip()) <= 10:
            return rest.rstrip()[:-3].strip()
    return text


def parse_text(text):
    """The preset a pasted text holds (JSON only, never run): ValueError saying why otherwise."""
    if not isinstance(text, str) or not text.strip():
        raise ValueError("cannot paste a graph: the clipboard is empty")
    if len(text) > 4 * 1024 * 1024:  # the core's limit (presets.h kMaxPresetText; the helper checks the rest)
        raise ValueError("cannot paste a graph: the text is more than any groom's graph (at most 4 MB)")
    try:
        data = json.loads(_unfenced(text))
    except ValueError as e:
        raise ValueError("cannot paste a graph: the text is not a GroomLab graph (%s)" % e)
    if isinstance(data, dict) and isinstance(data.get("graph"), dict) and "nodes" not in data:
        data = data["graph"]  # a .groom's graph
    if not isinstance(data, dict) or not isinstance(data.get("nodes"), list):
        raise ValueError("cannot paste a graph: the text is not a GroomLab graph (no nodes)")
    return data


def paste_text(scalp, text):
    """Replace the groom's graph with a look copied as text (copy_text, or a preset's JSON): fitted
    to this scalp (from its "size"), its layout, backdrops and control values: the helper applies it
    as a preset. Returns the helper's answer (nodes, fitFactor, missingMaps)."""
    data = parse_text(text)
    rt = groom.ensure(scalp)
    fd, path = tempfile.mkstemp(prefix="groomlab_paste_", suffix=".json")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
        try:
            r = process.call("apply_preset", session=rt.session, path=path)
        except HelperError as e:
            raise ValueError("cannot paste the graph: %s" % e.message)
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
    graph_tree.mark_replaced(scalp)
    graph.refresh(scalp, rt)
    if data.get("backdrops") and not process.call("get_graph", session=rt.session)["graph"].get("backdrops"):
        graph.set_backdrops(scalp, data["backdrops"])
        graph_tree.rebuild(scalp, replaced=True)
    groom.mark(scalp, "eval")
    return r


class GROOMLAB_OT_graph_copy(_Op):
    """Put this groom's whole graph on the clipboard as text (a preset with the control values), to share or save as a .json preset"""
    bl_idname = "groomlab.graph_copy"
    bl_label = "Copy Graph"
    bl_options = {"REGISTER"}
    action_id = "graph.copy"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        try:
            text = copy_text(scalp)
        except HelperError as e:
            return _fail(self, e)
        _clipboard[0] = text
        context.window_manager.clipboard = text
        nodes = len(json.loads(text).get("nodes", []))
        self.report({"INFO"}, "Copied %s's graph as text (%d nodes): Paste Graph on any groom, or save it as a .json preset"
                    % (scalp.name, nodes))
        return {"FINISHED"}


class GROOMLAB_OT_graph_paste(_Op):
    """Replace this groom's graph with one copied as text (Copy Graph, or a preset's JSON), sizes fitted to this scalp"""
    bl_idname = "groomlab.graph_paste"
    bl_label = "Paste Graph"
    action_id = "graph.paste"

    text: StringProperty(options={"SKIP_SAVE", "HIDDEN"}, description="The text (empty: the clipboard's)")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        text = self.text or context.window_manager.clipboard or _clipboard[0]
        try:
            r = paste_text(scalp, text)
        except ValueError as e:
            return _fail(self, e)
        except HelperError as e:
            return _fail(self, e)
        missing = [m["name"] for m in r.get("missingMaps", [])]
        note = " It reads maps this groom has not painted: %s." % ", ".join(missing) if missing else ""
        self.report({"INFO"}, "Pasted a graph of %d nodes (Ctrl+Z undoes it).%s" % (r.get("nodes", 0), note))
        if _ge().editor_tree(context) is not None:
            bpy.ops.node.view_all()
        return {"FINISHED"}


# ---- Backdrops: Blender's frames -------------------------------------------------------------------

def _frame(context, name):
    tree = _ge().context_tree(context)
    if tree is None:
        return None
    node = tree.nodes.get(name) if name else tree.nodes.active
    return node if node is not None and node.bl_idname == "NodeFrame" else None


def node_rect(node):
    """(left, top, right, bottom) of a node in Blender's coordinates (its drawn size, or an estimate
    when it was never drawn)."""
    x, y = graph_tree.get_location(node)
    w, h = node.dimensions
    prefs = getattr(bpy.context, "preferences", None)
    scale = float(prefs.system.ui_scale) * float(getattr(prefs.system, "pixel_size", 1.0)) if prefs else 1.0
    if w > 0:
        w, h = w / scale, h / scale
    else:
        w, h = float(node.width), 60.0 + 22.0 * max(len(node.inputs), len(node.outputs))
    return x, y, x + w, y - h


def add_backdrop(scalp, names=None, title="Backdrop", at=None):
    """A labelled frame around the nodes (Maya's Add Backdrop Around Selection: 30 around, 50 above),
    else a 400 x 240 one at `at`; its nodes inside it. Kept in the graph's backdrops. Returns the frame."""
    tree = graph_tree.tree_of(scalp)
    nodes = [n for n in tree.nodes if ours(n) and n.gl_name in (names or [])]
    if nodes:
        rects = [node_rect(n) for n in nodes]
        left, top = min(r[0] for r in rects) - 30.0, max(r[1] for r in rects) + 50.0
        right, bottom = max(r[2] for r in rects) + 30.0, min(r[3] for r in rects) - 30.0
    else:
        cx, cy = at or (0.0, 0.0)
        left, top, right, bottom = cx - 200.0, cy + 120.0, cx + 200.0, cy - 120.0
    with graph_tree.building():
        frame = tree.nodes.new("NodeFrame")
        graph_tree.set_frame(frame, {"title": title, "x": left, "y": -top, "w": right - left, "h": top - bottom,
                                     "color": list(graph_tree.BACKDROP_COLOR)})
        for n in nodes:
            n.parent = frame
        tree["gl_sig"] = graph_tree.signature(tree)
    graph_tree.push_positions(scalp, tree)
    return frame


def backdrops_changed(scalp):
    """A frame edited by the add-on (renamed, recolored, removed): the graph's backdrops follow."""
    graph_tree.push_positions(scalp)


class GROOMLAB_OT_graph_backdrop_add(_Op):
    """A labelled frame behind the selected nodes (a backdrop: kept in the graph, Maya shows it too)"""
    bl_idname = "groomlab.graph_backdrop_add"
    bl_label = "Add Backdrop Around Selection"
    action_id = "backdrop.add"

    title: StringProperty(name="Title", default="Backdrop")

    def execute(self, context):
        scalp = _ge().context_groom(context)
        names = [n.gl_name for n in _ge().selected_nodes(context)]
        at = None
        region = getattr(context, "region", None)
        if not names and region is not None and getattr(region, "view2d", None) is not None:
            at = region.view2d.region_to_view(region.width / 2, region.height / 2)
        try:
            frame = add_backdrop(scalp, names, self.title, at)
        except HelperError as e:
            return _fail(self, e)
        tree = frame.id_data
        for n in tree.nodes:
            n.select = n == frame
        tree.nodes.active = frame
        return {"FINISHED"}


class GROOMLAB_OT_graph_backdrop_rename(_Op):
    """The backdrop's title"""
    bl_idname = "groomlab.graph_backdrop_rename"
    bl_label = "Rename Backdrop"
    action_id = "backdrop.rename"

    frame: StringProperty(options={"SKIP_SAVE"})
    title: StringProperty(name="Title")

    def invoke(self, context, event):
        frame = _frame(context, self.frame)
        if frame is not None:
            self.title = frame.label or frame.name
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        frame = _frame(context, self.frame)
        if frame is None:
            return {"CANCELLED"}
        frame.label = self.title
        try:
            backdrops_changed(_ge().context_groom(context))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_backdrop_color(_Op):
    """The backdrop's color"""
    bl_idname = "groomlab.graph_backdrop_color"
    bl_label = "Backdrop Color"
    action_id = "backdrop.color"

    frame: StringProperty(options={"SKIP_SAVE"})
    color: StringProperty(description="[r, g, b] 0-255")

    def execute(self, context):
        frame = _frame(context, self.frame)
        if frame is None:
            return {"CANCELLED"}
        frame.use_custom_color = True
        frame.color = graph_tree._linear(json.loads(self.color))
        try:
            backdrops_changed(_ge().context_groom(context))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_backdrop_delete(_Op):
    """Delete the backdrop (its nodes stay)"""
    bl_idname = "groomlab.graph_backdrop_delete"
    bl_label = "Delete Backdrop"
    action_id = "backdrop.delete"

    frame: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        frame = _frame(context, self.frame)
        if frame is None:
            return {"CANCELLED"}
        tree = frame.id_data
        with graph_tree.building():
            tree.nodes.remove(frame)
            tree["gl_sig"] = graph_tree.signature(tree)
        try:
            backdrops_changed(_ge().context_groom(context))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Node edits the node menu has besides --------------------------------------------------------------

class GROOMLAB_OT_graph_rename(_NodeOp):
    """Rename the node"""
    bl_idname = "groomlab.graph_rename"
    bl_label = "Rename Node"
    action_id = "node.rename"

    new_name: StringProperty(name="Name")

    def invoke(self, context, event):
        self.new_name = self.target(context)
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = _ge().context_groom(context)
        old = self.target(context)
        if not self.new_name or self.new_name == old:
            return {"CANCELLED"}
        rt = groom.ensure(scalp)
        try:
            graph_tree.push_positions(scalp)
            process.call("edit_graph", session=rt.session, op="rename_node", name=old, newName=self.new_name)
        except HelperError as e:
            return _fail(self, e)
        if scalp.groomlab.view_node == old:
            with props.quiet():
                scalp.groomlab.view_node = self.new_name
        graph_tree._edited(scalp, rt)
        return {"FINISHED"}


class GROOMLAB_OT_graph_delete(_NodeOp):
    """Delete the node (empty: the selected ones)"""
    bl_idname = "groomlab.graph_delete"
    bl_label = "Delete"
    action_id = "node.delete"

    def execute(self, context):
        scalp = _ge().context_groom(context)
        names = [self.node] if self.node else [n.gl_name for n in _ge().selected_nodes(context)]
        if not names:
            return {"CANCELLED"}
        rt = groom.ensure(scalp)
        try:
            graph_tree.push_positions(scalp)
            for name in names:
                process.call("edit_graph", session=rt.session, op="remove_node", name=name)
        except HelperError as e:
            return _fail(self, e)
        if scalp.groomlab.view_node in names:
            with props.quiet():
                scalp.groomlab.view_node = ""
        graph_tree._edited(scalp, rt)
        return {"FINISHED"}


def sculpt_target(scalp):
    """The Sculpt node brushes edit now ("" for the base groom or a corrective)."""
    try:
        from . import tools
        target = tools.sculpt_target(scalp).get("target") or {}
    except (HelperError, ImportError):
        return ""
    return target.get("name", "") if target.get("kind") == "node" else ""


class GROOMLAB_OT_graph_brush_sculpt(_NodeOp):
    """Brushes shape the guides this node hands on (again: the base groom)"""
    bl_idname = "groomlab.graph_brush_sculpt"
    bl_label = "Brush This Node's Guides"
    action_id = "node.brushSculpt"

    def execute(self, context):
        from . import tools
        scalp = _ge().context_groom(context)
        name = self.target(context)
        try:
            if sculpt_target(scalp) == name:
                tools.sculpt_target(scalp, base=True)
            else:
                tools.sculpt_target(scalp, node=name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_sculpt_clear(_NodeOp):
    """Drop every stroke kept in this node: its guides as it is fed them"""
    bl_idname = "groomlab.graph_sculpt_clear"
    bl_label = "Clear Sculpt Edits"
    action_id = "sculpt.clearEdits"

    def execute(self, context):
        from . import tools
        scalp = _ge().context_groom(context)
        try:
            tools.sculpt_clear(scalp, self.target(context))
            tools.after_edit(scalp, context.scene)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_paint_param(_ParamOp):
    """Paint this param: a map feeding it (white = its value now, black = 0), painted in Groom Tools > Paint"""
    bl_idname = "groomlab.graph_paint_param"
    bl_label = "Paint"

    kind: EnumProperty(name=defs.get("choices/mapKind")["label"], items=defs.enum_items("mapKind"), default="vertices")

    def execute(self, context):
        from . import tools
        scalp = _ge().context_groom(context)
        try:
            # A map made with faces of the scalp selected in Edit Mode: white on them only.
            tools.map_for_param(scalp, self.target(context), self.param, kind=self.kind, faces=groom.selected_faces(scalp))
            tools.pick(context, "paint")
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Bindings: the UI definitions' action ids -------------------------------------------------------

def _n(ctx, _item):
    return {"node": ctx.node or ""}


def _np(ctx, _item):
    return {"node": ctx.node or "", "param": ctx.param or ""}


def _node_of(ctx):
    tree = ctx.tree
    return next((n for n in tree.nodes if ours(n) and n.gl_name == ctx.node), None) if tree is not None and ctx.node else None


def _bind_all():
    bind("compound.groupSelection", "groomlab.graph_group")
    bind("graph.group", "groomlab.graph_group")
    bind("graph.ungroup", "groomlab.graph_ungroup")
    bind("compound.ungroup", "groomlab.graph_ungroup", _n)
    bind("compound.showInner", "groomlab.graph_show_inner",
         lambda ctx, item: {"node": ctx.node or "", "target_param": item["value"]},
         lambda ctx, item: item["value"] in shown_inner(_node_of(ctx)) if _node_of(ctx) else False)
    bind("compound.saveAsNode", "groomlab.graph_save_node", _n)
    bind("compound.saveToLibrary", "groomlab.graph_save_to_library", _n)
    bind("node.update", "groomlab.graph_update_node", _n)
    bind("node.expose", "groomlab.graph_expose", lambda ctx, item: {"node": ctx.node or "", "param": item["value"]},
         lambda ctx, item: item["value"] in exposed(_node_of(ctx)) if _node_of(ctx) else False)
    bind("param.expose", "groomlab.graph_expose", _np,
         lambda ctx, item: ctx.param in exposed(_node_of(ctx)) if _node_of(ctx) else False)
    bind("param.setKey", "groomlab.graph_set_key", _np)
    bind("param.deleteKey", "groomlab.graph_delete_key", _np)
    bind("param.removeAnimation", "groomlab.graph_remove_animation", _np)
    bind("param.driveWithSlider", "groomlab.graph_add_control", _np)
    bind("param.rampPreset", "groomlab.graph_ramp_preset", lambda ctx, item: dict(_np(ctx, item), preset=item["value"]))
    bind("param.rampFlip", "groomlab.graph_ramp_preset", lambda ctx, item: dict(_np(ctx, item), preset="flip"))
    bind("param.paintGuides", "groomlab.graph_paint_param", lambda ctx, item: dict(_np(ctx, item), kind="guides"))
    bind("param.paintVertices", "groomlab.graph_paint_param", lambda ctx, item: dict(_np(ctx, item), kind="vertices"))
    bind("param.paintTexture", "groomlab.graph_paint_param", lambda ctx, item: dict(_np(ctx, item), kind="texture"))
    bind("script.edit", "groomlab.graph_script_edit", _n)
    bind("script.saveToLibrary", "groomlab.graph_script_save", _n)
    bind("node.newScript", "groomlab.graph_new_script")
    bind("node.import", "groomlab.graph_import_node")
    bind("node.openFolder", "groomlab.graph_open_nodes_folder")
    bind("preset.save", "groomlab.graph_save_preset")
    bind("graph.copy", "groomlab.graph_copy")
    bind("graph.paste", "groomlab.graph_paste")
    bind("backdrop.add", "groomlab.graph_backdrop_add")
    bind("backdrop.rename", "groomlab.graph_backdrop_rename", lambda ctx, item: {"frame": ctx.frame or ""})
    bind("backdrop.color", "groomlab.graph_backdrop_color",
         lambda ctx, item: {"frame": ctx.frame or "", "color": json.dumps(item["value"])})
    bind("backdrop.delete", "groomlab.graph_backdrop_delete", lambda ctx, item: {"frame": ctx.frame or ""})
    bind("node.rename", "groomlab.graph_rename", _n)
    bind("node.delete", "groomlab.graph_delete", _n)
    bind("node.bake", "groomlab.bake_to_guides", _n)
    bind("node.brushSculpt", "groomlab.graph_brush_sculpt", _n,
         lambda ctx, item: bool(ctx.scalp) and sculpt_target(ctx.scalp) == ctx.node)
    bind("sculpt.clearEdits", "groomlab.graph_sculpt_clear", _n)


classes = (GROOMLAB_OT_graph_group, GROOMLAB_OT_graph_ungroup, GROOMLAB_OT_graph_show_inner, GROOMLAB_OT_graph_expose,
           GROOMLAB_OT_graph_ramp_preset, GROOMLAB_OT_graph_toggle_inputs,
           GROOMLAB_OT_graph_set_key, GROOMLAB_OT_graph_delete_key, GROOMLAB_OT_graph_remove_animation,
           GROOMLAB_OT_graph_add_control, GROOMLAB_OT_graph_map_source, GROOMLAB_OT_graph_edit_expression,
           GROOMLAB_OT_graph_apply_text, GROOMLAB_OT_graph_script_edit, GROOMLAB_OT_graph_script_done,
           GROOMLAB_OT_graph_script_save, GROOMLAB_OT_graph_new_script, GROOMLAB_OT_graph_save_node,
           GROOMLAB_OT_graph_save_to_library, GROOMLAB_OT_graph_update_node, GROOMLAB_OT_graph_import_node,
           GROOMLAB_OT_graph_open_nodes_folder, GROOMLAB_OT_graph_save_preset, GROOMLAB_OT_graph_copy,
           GROOMLAB_OT_graph_paste, GROOMLAB_OT_graph_backdrop_add, GROOMLAB_OT_graph_backdrop_rename,
           GROOMLAB_OT_graph_backdrop_color, GROOMLAB_OT_graph_backdrop_delete, GROOMLAB_OT_graph_rename,
           GROOMLAB_OT_graph_delete, GROOMLAB_OT_graph_brush_sculpt, GROOMLAB_OT_graph_sculpt_clear,
           GROOMLAB_OT_graph_paint_param)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    _bind_all()
    bpy.app.handlers.frame_change_post.append(_push_all_controls)
    bpy.app.handlers.depsgraph_update_post.append(_push_all_controls)


def unregister():
    for name in ("frame_change_post", "depsgraph_update_post"):
        lst = getattr(bpy.app.handlers, name)
        if _push_all_controls in lst:
            lst.remove(_push_all_controls)
    if bpy.app.timers.is_registered(_watch_tick):
        bpy.app.timers.unregister(_watch_tick)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    _text_state.clear()
    _sent_controls.clear()
    _control_names.clear()
