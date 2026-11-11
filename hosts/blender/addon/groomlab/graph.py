# SPDX-License-Identifier: GPL-3.0-or-later
"""The groom's node graph in the panel: its nodes as a list (the main chain from the roots to the
output first), the selected node's params as widgets (node_spec: groups, Advanced folded,
showWhen, units), presets, and simple edits (add, remove, move along the chain) through the
helper's edit_graph. A full node editor is the Maya plug-in's and the app's."""

import json

from . import groom, process, props, units
from .helper_client import HelperError

STRAND_TYPES = {"Strands", "Curves"}
_specs = {}
_presets = []


def _call(cmd, **args):
    return process.call(cmd, **args)


def operator_specs():
    if not _specs:
        for spec in _call("operator_specs")["operators"]:
            _specs[spec["type"]] = spec
    return _specs


def presets(refresh=False):
    """[(name, title, description, category)] of the shipped and user presets (cached)."""
    global _presets
    if refresh or not _presets:
        _presets = [(p["name"], p.get("title") or p["name"], p.get("description", ""), p.get("category", ""))
                    for p in _call("list_presets")["presets"]]
    return _presets


_library = {"at": 0.0, "nodes": []}


def library_nodes(refresh=False):
    """The node library's compound nodes (the helper's library_nodes: GROOMLAB_NODE_PATH, the user's
    Documents/GroomLab/nodes that Maya and the app share, the shipped ones) as Add menu entries:
    dicts with type ("node:<id>"), id, version, label, categoryTitle, description. Read again at
    most every two seconds (the Add menu asks as it draws)."""
    import time
    now = time.time()
    if refresh or now - _library["at"] > 2.0:
        try:
            _library["nodes"] = _call("library_nodes")["nodes"]
        except Exception:  # a library that cannot be read never stops the menu
            _library["nodes"] = []
        _library["at"] = now
    return _library["nodes"]


def cached_presets():
    return _presets


# ---- The graph's shape --------------------------------------------------------------------------

def main_chain(graph):
    """The nodes from the strands' source to the output, following each node's strands input; and
    each node's inputs {input: from}."""
    into = {}
    for c in graph.get("connections", []):
        into.setdefault(c["to"], {})[c["input"]] = c["from"]
    chain, seen = [], set()
    node = graph.get("output")
    while node and node not in seen:
        seen.add(node)
        chain.append(node)
        ins = into.get(node, {})
        node = ins.get("strands") or ins.get("a") or ins.get("shafts")
    chain.reverse()
    return chain, into


def _input_from(into, node, source):
    for name, frm in into.get(node, {}).items():
        if frm == source:
            return name
    return "strands"


def _node_error(name, errors):
    for e in errors:
        if e.startswith(name + ":") or ("'%s'" % name) in e:
            return e
    return ""


def refresh(scalp, rt=None):
    """The node list again from the helper's graph (after any change of the graph)."""
    rt = rt or groom.ensure(scalp)
    graph = _call("get_graph", session=rt.session)["graph"]
    g = scalp.groomlab
    g.graph_json = json.dumps(graph)
    types = {n["name"]: n["type"] for n in graph.get("nodes", [])}
    chain, _ = main_chain(graph)
    order = [n for n in chain if n in types] + [n for n in types if n not in chain]
    active = g.nodes[g.node_index].name if 0 <= g.node_index < len(g.nodes) else None
    with props.quiet():
        g.nodes.clear()
        for name in order:
            item = g.nodes.add()
            item.name = name
            item.type = types[name]
            item.in_chain = name in chain
            item.is_output = name == graph.get("output")
            item.unsupported = groom.UNSUPPORTED.get(types[name], "")
        if active in order:
            g.node_index = order.index(active)
        else:
            g.node_index = max(0, len([n for n in chain if n in types]) - 2)
    update_notes(scalp, rt)
    refresh_params(scalp)
    from . import graph_tree
    graph_tree.graph_changed(scalp)  # the Graph Editor's tree follows


def update_notes(scalp, rt=None):
    rt = rt or groom.runtime_if_any(scalp)
    if rt is None:
        return
    for item in scalp.groomlab.nodes:
        note = rt.notes.get(item.name, "")
        error = _node_error(item.name, rt.errors)
        if item.note != note:
            item.note = note
        if item.error != error:
            item.error = error


def active_node(scalp):
    g = scalp.groomlab
    return g.nodes[g.node_index] if 0 <= g.node_index < len(g.nodes) else None


# ---- Params -----------------------------------------------------------------------------------

def _set_widget(p, value, scene):
    kind, unit = p.kind, p.unit
    p.raw = json.dumps(value)
    p.widget = "fixed"
    p.note = ""
    if isinstance(value, dict):
        p.note = "mapped (%s): edit the map in Maya or the GroomLab app" % (value.get("kind") or value.get("type") or "map")
    elif kind == "bool" and isinstance(value, bool):
        p.widget, p.value_bool = "bool", value
    elif kind == "int" and isinstance(value, int) and not isinstance(value, bool):
        p.widget, p.value_int = "int", value
    elif kind == "float" and isinstance(value, (int, float)) and not isinstance(value, bool):
        if unit in units.SCENE_LENGTH:
            p.widget, p.value_length = "length", units.to_ui(unit, float(value), scene)
        elif unit == "fraction":
            p.widget, p.value_factor = "factor", float(value)
        else:
            p.widget, p.value_float = "float", units.to_ui(unit, float(value), scene)
    elif kind == "vec3" and isinstance(value, list) and len(value) == 3:
        if unit == "color":
            p.widget, p.value_color = "color", [float(v) for v in value]
        else:  # a position or a move (unit length): in scene units
            p.widget, p.value_vector = "vector", [units.to_ui(unit, float(v), scene) for v in value]
    elif kind == "string" and isinstance(value, str):
        choices = json.loads(p.choices or "[]")
        if choices and value in choices:
            p.widget, p.value_enum = "enum", value
        else:
            p.widget, p.value_string = "string", value
    elif kind == "ramp":
        p.note = "a ramp: edit it in Maya or the GroomLab app"
    else:
        p.note = "edit it in Maya or the GroomLab app"


def refresh_params(scalp):
    g = scalp.groomlab
    node = active_node(scalp)
    with props.quiet():
        g.params.clear()
        g.params_node = node.name if node else ""
    if node is None:
        return
    rt = groom.ensure(scalp)
    spec = _call("node_spec", session=rt.session, node=node.name)["spec"]
    graph = json.loads(g.graph_json or "{}")
    values = {}
    for n in graph.get("nodes", []):
        if n.get("name") == node.name:
            values = n.get("params") or {}
    scene = groom.scene_of(scalp)
    defaults = {ps["name"]: ps.get("default") for ps in spec.get("params", [])}
    with props.quiet():
        for ps in spec.get("params", []):
            p = g.params.add()
            p.name = ps["name"]
            p.kind = ps.get("kind", "")
            p.unit = units.effective(ps, lambda q: values.get(q, defaults.get(q)))  # (unitWhen: by the node's values)
            p.group = ps.get("group", "")
            p.advanced = bool(ps.get("advanced"))
            p.description = ps.get("description", "")
            p.choices = json.dumps(ps.get("choices") or [])
            p.show_when = json.dumps(ps.get("showWhen") or {})
            _set_widget(p, values.get(ps["name"], ps.get("default")), scene)
    g.node_type_description = spec.get("description", "")


def widget_value(p, scene):
    w = p.widget
    if w == "bool":
        return bool(p.value_bool)
    if w == "int":
        return int(p.value_int)
    if w == "length":
        return units.from_ui(p.unit, float(p.value_length), scene)
    if w == "factor":
        return float(p.value_factor)
    if w == "float":
        return units.from_ui(p.unit, float(p.value_float), scene)
    if w == "color":
        return [float(v) for v in p.value_color]
    if w == "vector":
        return [units.from_ui(p.unit, float(v), scene) for v in p.value_vector]
    if w == "enum":
        return str(p.value_enum)
    if w == "string":
        return str(p.value_string)
    return json.loads(p.raw or "null")


def push_param(p, context):
    """A widget changed: set the param in the helper (centimeters), evaluate soon."""
    scalp = p.id_data
    g = scalp.groomlab
    if not g.params_node:
        return
    scene = context.scene if context is not None and context.scene is not None else groom.scene_of(scalp)
    value = widget_value(p, scene)
    try:
        rt = groom.ensure(scalp, scene)
        _call("set_param", session=rt.session, node=g.params_node, param=p.name, value=value)
        p.raw = json.dumps(value)
        groom.remember_graph(scalp, rt)
        groom.mark(scalp, "eval")
        from . import graph_tree
        graph_tree.values_changed(scalp, g.params_node)
    except HelperError as e:
        groom.runtime(scalp).error = e.message


def set_param(scalp, node, param, value):
    """Scripts and tests: a param by name, in the groom's own units (centimeters)."""
    rt = groom.ensure(scalp)
    _call("set_param", session=rt.session, node=node, param=param, value=value)
    groom.remember_graph(scalp, rt)
    if scalp.groomlab.params_node == node:
        refresh_params(scalp)
    from . import graph_tree
    graph_tree.values_changed(scalp, node)
    groom.mark(scalp, "eval")


def visible(p, by_name):
    """Whether a param shows: its showWhen's param holds one of its values (that param's widget)
    and that param shows too, and so on up (graph.h ParamCondition: Clump's count hides with
    clumpBy)."""
    seen = set()
    while p is not None and p.name not in seen:
        seen.add(p.name)
        when = json.loads(p.show_when or "{}")
        other = by_name.get(when.get("param", "")) if when else None
        if other is None:
            return True
        if not units.condition_holds(when, json.loads(other.raw or "null")):  # (the core's rule: 3.0 reads "3")
            return False
        p = other
    return True


# ---- Edits ------------------------------------------------------------------------------------

def _edit(rt, **args):
    return _call("edit_graph", session=rt.session, **args)


def _ensure(scalp):
    """The groom's runtime, the Graph Editor's node moves sent first (they go with the next edit)."""
    rt = groom.ensure(scalp)
    from . import graph_tree
    graph_tree.push_positions(scalp)
    return rt


def _changed(scalp, rt):
    refresh(scalp, rt)
    groom.mark(scalp, "eval")


def add_node(scalp, type_name, after=None):
    """A node of `type_name`, on the main chain after `after` (else before the output)."""
    rt = _ensure(scalp)
    graph = _call("get_graph", session=rt.session)["graph"]
    chain, into = main_chain(graph)
    made = _edit(rt, op="add_node", type=type_name)["name"]
    spec = operator_specs().get(type_name, {})
    strand_in = next((i["name"] for i in spec.get("inputs", []) if i.get("type") in STRAND_TYPES), None)
    out_type = (spec.get("output") or {}).get("type")
    error = ""
    if strand_in and out_type in STRAND_TYPES and len(chain) >= 2:
        i = chain.index(after) if after in chain and after != chain[-1] else len(chain) - 2
        prev, nxt = chain[i], chain[i + 1]
        nxt_input = _input_from(into, nxt, prev)
        _edit(rt, op="connect", **{"from": prev, "to": made, "input": strand_in})
        error = _edit(rt, op="connect", **{"from": made, "to": nxt, "input": nxt_input})["graphError"]
    _changed(scalp, rt)
    g = scalp.groomlab
    for k, item in enumerate(g.nodes):
        if item.name == made:
            g.node_index = k
    return made, error


def remove_node(scalp, name):
    rt = _ensure(scalp)
    graph = _call("get_graph", session=rt.session)["graph"]
    chain, into = main_chain(graph)
    if name in chain and 0 < chain.index(name) < len(chain) - 1:
        i = chain.index(name)
        _edit(rt, op="connect", **{"from": chain[i - 1], "to": chain[i + 1], "input": _input_from(into, chain[i + 1], name)})
    error = _edit(rt, op="remove_node", name=name)["graphError"]
    _changed(scalp, rt)
    return error


def move_node(scalp, name, direction):
    """Moves a node one place along the main chain (-1: toward the roots, +1: toward the output).
    The source (Scatter...) and the output stay where they are. False when it cannot move."""
    rt = _ensure(scalp)
    graph = _call("get_graph", session=rt.session)["graph"]
    chain, into = main_chain(graph)
    if name not in chain:
        return False
    i = chain.index(name)
    j = i - 1 if direction < 0 else i  # swap chain[j] and chain[j + 1]
    if j < 1 or j + 2 > len(chain) - 1:
        return False
    a, b, c, d = chain[j - 1], chain[j], chain[j + 1], chain[j + 2]
    in_b, in_c, in_d = _input_from(into, b, a), _input_from(into, c, b), _input_from(into, d, c)
    _edit(rt, op="connect", **{"from": a, "to": c, "input": in_c})
    _edit(rt, op="connect", **{"from": c, "to": b, "input": in_b})
    _edit(rt, op="connect", **{"from": b, "to": d, "input": in_d})
    _changed(scalp, rt)
    g = scalp.groomlab
    for k, item in enumerate(g.nodes):
        if item.name == name:
            g.node_index = k
    return True


def apply_preset(scalp, name):
    """The preset's graph on the groom (fitted, its control values set), with the painted maps it
    reads made when the groom has none of that name, as Maya and the app make them (its "requires":
    kind, fill value or color), so it evaluates at once: `mapsMade` in the answer; `missingMaps` the
    ones that could not be made. The guide groups it reads too, made empty (`groupsMade`); its
    `drawGroup` the one its Draw tool draws into (a hand-placed look), or None."""
    rt = groom.ensure(scalp)
    r = _call("apply_preset", session=rt.session, name=name)
    r["mapsMade"], r["missingMaps"] = make_required_maps(scalp, r.get("missingMaps") or [])
    r["groupsMade"] = make_required_groups(scalp, r.get("groups") or [])
    r["drawGroup"] = next((g["name"] for g in r.get("groups") or [] if g.get("draw")), None)
    from . import graph_tree
    graph_tree.mark_replaced(scalp)  # the preset's layout, not the tree's
    _changed(scalp, rt)
    return r


def preset_maps_note(r):
    """What an applied preset's answer says of its maps, for the operator's report: (level, text),
    or None when it reads none it lacked."""
    missing = [m["name"] for m in r.get("missingMaps") or []]
    if missing:
        return {"WARNING"}, "The preset reads maps this groom has not painted: %s. Paint them in Groom Tools > Paint" % ", ".join(missing)
    if r.get("mapsMade"):
        return {"INFO"}, "Made the map %s the preset reads: paint it in Groom Tools > Paint" % ", ".join(r["mapsMade"])
    if r.get("drawGroup"):
        return {"INFO"}, "Draw guides from the skin into %s with the Draw tool: each one grows the look" % r["drawGroup"]
    return None


def make_required_maps(scalp, wanted):
    """Make the maps `wanted` ([{name, kind, value, color}], the helper's missingMaps), each one
    undo step on the helper's stack. Returns (the names made, the entries that could not be)."""
    from . import tools
    made, failed = [], []
    for m in wanted:
        args = {"name": m["name"], "kind": m.get("kind") or "guides", "value": float(m.get("value") or 0.0)}
        color = [float(c) for c in m.get("color") or []][:4]
        if len(color) >= 3:  # a color map filled with it (else gray)
            args["color"] = True
            args["fill"] = color
        try:
            r = tools.call(scalp, "map_create", **args)
        except HelperError:  # no scalp to paint on, no guides to carry a layer
            failed.append(m)
            continue
        tools.after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=False)
        made.append(m["name"])
    if made:
        tools.refresh_info(scalp)
    return made, failed


def make_required_groups(scalp, wanted):
    """Make the guide groups `wanted` ([{name, draw}], the helper's groups) the groom lacks, empty,
    each one undo step on the helper's stack (a groom of GroomLab guides; Blender curve guides have
    no groups). Returns the names made."""
    from . import tools
    if not wanted or not tools.is_hierarchy(scalp):
        return []
    have = [g.get("name") for g in (tools.call(scalp, "guides_info").get("groups") or [])]
    made = []
    for g in wanted:
        if g["name"] not in have:
            tools.guides_edit(scalp, "add_group", name=g["name"])
            made.append(g["name"])
    return made


def draw_into(context, scalp, group):
    """The GroomLab Draw tool drawing into guide group `group`, so every guide drawn is one of its
    own (a hand-placed feather). Not in background mode (no viewport). Returns whether it started."""
    import bpy
    from . import tools
    if bpy.app.background or scalp is None or not tools.is_hierarchy(scalp):
        return False
    tools.refresh_info(scalp, context.scene)
    t = context.scene.groomlab_tools
    if not any(g.name == group for g in t.groups):
        return False
    t.group = group
    try:
        return "FINISHED" in bpy.ops.groomlab.tool(tool="draw")
    except RuntimeError:
        return False


def insert_preset(scalp, name, after=None):
    rt = groom.ensure(scalp)
    from . import graph_tree
    graph_tree.push_positions(scalp)  # the look goes after `after`, the nodes right of it move over
    args = {"session": rt.session, "name": name}
    if after:
        args["after"] = after
    r = _call("insert_preset", **args)
    _changed(scalp, rt)
    return r


# ---- Authoring: the Graph Editor's edits (Maya's GraphModel), one undo step each on the helper's stack ----

def _authored(scalp, r):
    """After one of the helper's undoable edits: the kept graph and the node tree follow, the groom
    evaluates, the palette shows the undo labels."""
    from . import tools
    tools.follow_graph(scalp, r)
    tools.after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


def _author(scalp, **args):
    rt = _ensure(scalp)
    return _authored(scalp, _edit(rt, **args))


def group(scalp, names, name=None):
    """Group nodes into one Compound (Ctrl+G); returns its name."""
    return _author(scalp, op="group", names=list(names), name=name or "")["name"]


def ungroup(scalp, name):
    """A Compound's nodes back in the graph (Ctrl+Shift+G); returns their names."""
    return _author(scalp, op="ungroup", name=name)["names"]


def expose_param(scalp, node, param):
    """Give a param an input port (a Field, Curve or Value port: another node drives it)."""
    return _author(scalp, op="expose_param", name=node, param=param)


def unexpose_param(scalp, node, param):
    """Remove a param's input port and what fed it; the param keeps its own value."""
    return _author(scalp, op="unexpose_param", name=node, param=param)


def compound_expose(scalp, compound, name, target):
    """Show a Compound's inner param `target` ("node.param") on its panel as `name`."""
    return _author(scalp, op="compound_expose", name=compound, param=name, target=target)


def compound_unexpose(scalp, compound, target):
    return _author(scalp, op="compound_unexpose", name=compound, target=target)


def insert_on_link(scalp, src, dst, input_name, node, output=None):
    """Route the link src -> dst.input_name through `node` (a node dropped on a wire)."""
    args = {"op": "insert_on_link", "from": src, "to": dst, "input": input_name, "node": node}
    if output:
        args["output"] = output
    return _author(scalp, **args)


def add_control(scalp, node, param, name=None, default=None, min=0.0, max=1.0):  # noqa: A002 (Maya's names)
    """Drive node.param with a slider: a Control node (its control `name`, the param's by default)
    wired to the param. Returns the control's name; set its value with set_controls."""
    args = {"op": "add_control", "node": node, "param": param, "min": float(min), "max": float(max)}
    if name:
        args["name"] = name
    if default is not None:
        args["default"] = float(default)
    return _author(scalp, **args)["control"]


def link_control(scalp, node, param, control=None):
    """Key node.param in place: linked to a control of its own (<node>_<param>; three for a vector)
    that set_controls gives on each frame (from the property's fcurve). Returns the control(s)."""
    args = {"op": "link_control", "node": node, "param": param}
    if control:
        args["control"] = control
    return _author(scalp, **args)["control"]


def unlink_control(scalp, node, param):
    """The keyed param back to its own value (the control's value now)."""
    return _author(scalp, op="unlink_control", node=node, param=param)


def controls(scalp):
    """The controls the graph reads: [{name, kind (control, variable, keyed), node, param?, default,
    min?, max?, value, set}]."""
    rt = groom.ensure(scalp)
    return _call("controls_list", session=rt.session)["controls"]


def set_controls(scalp, values, replace=False):
    """The controls' values now ({name: number}; None unsets one). Not an undo step: Blender's
    properties and their fcurves hold them, given again on each frame."""
    rt = groom.ensure(scalp)
    r = _call("set_controls", session=rt.session, controls=dict(values), replace=bool(replace))
    if r.get("changed"):
        groom.mark(scalp, "eval")
    return r


def set_backdrops(scalp, backdrops):
    """The Graph Editor's backdrops ([{title, x, y, w, h, color: [r, g, b] 0-255}], all of them),
    kept in the graph with the layout (not an undo step)."""
    rt = _ensure(scalp)
    _edit(rt, op="set_positions", backdrops=list(backdrops))
    groom.remember_graph(scalp, rt)


# ---- The node library (Maya's nodes.py) and Save as Preset ---------------------------------------

def nodes_folder():
    """{userDir, dirs, shippedDir}: where Save as Node and import write, where nodes are looked up."""
    return _call("nodes_folder")


def node_file(scalp, compound, **header):
    """The .glnode Save as Node would write for Compound `compound` (a dict; nothing saved). Header:
    node_id, version, label, category, description, author, license, icon, params, paramInfo."""
    rt = groom.ensure(scalp)
    return _call("node_file", session=rt.session, node=compound, **_header(header))["file"]


def _header(header):
    out = dict((k, v) for k, v in header.items() if v is not None)
    if "node_id" in out:
        out["nodeId"] = out.pop("node_id")
    return out


def save_node(scalp, compound, directory=None, **header):
    """Save Compound `compound` as a library node (the user's folder, or `directory`); the compound
    then holds the saved copy (one undo step). Returns the file's path."""
    rt = _ensure(scalp)
    args = _header(header)
    if directory:
        args["dir"] = directory
    r = _authored(scalp, _call("save_node", session=rt.session, node=compound, **args))
    library_nodes(refresh=True)
    return r["path"]


def import_node(path, directory=None):
    """Copy a .glnode (checked) into the user's node folder (or `directory`). Returns the new path."""
    args = {"path": path}
    if directory:
        args["dir"] = directory
    r = _call("import_node", **args)
    library_nodes(refresh=True)
    return r["path"]


def script_template(label="My Script", author=""):
    """A new script node's file (a dict) whose id the library does not have yet."""
    return _call("script_template", label=label, author=author)["definition"]


def script_errors(definition):
    """[{part, message, line, column}] wrong with a script node's file (a dict or its text); []: it checks."""
    return _call("script_errors", definition=definition)["errors"]


def add_script_node(scalp, definition=None, name=None, after=None):
    """A script node (the template by default) fed by `after` and feeding what it fed. Returns its name."""
    rt = _ensure(scalp)
    args = {}
    if definition is not None:
        args["definition"] = definition
    if name:
        args["name"] = name
    if after:
        args["after"] = after
    return _authored(scalp, _call("new_script_node", session=rt.session, **args))["name"]


def set_script(scalp, node, definition):
    """Script node `node` takes an edited file (HelperError "script" saying what is wrong)."""
    rt = _ensure(scalp)
    return _authored(scalp, _call("set_script", session=rt.session, node=node, definition=definition))


def save_script_node(scalp, node, as_new=False, directory=None):
    """Script node `node` to the library, one version past its copy there (as_new: a new node).
    Returns the file's path."""
    rt = _ensure(scalp)
    args = {"node": node, "asNew": bool(as_new)}
    if directory:
        args["dir"] = directory
    r = _authored(scalp, _call("save_script_node", session=rt.session, **args))
    library_nodes(refresh=True)
    return r["path"]


def save_preset(scalp, name, overwrite=False, about=None):
    """The groom's graph (with its layout) as a user preset, in the folder Maya's presets use.
    Returns the file's path."""
    rt = _ensure(scalp)
    args = {"name": name, "overwrite": bool(overwrite)}
    if about:
        args["about"] = dict(about)
    r = _call("save_preset", session=rt.session, **args)
    presets(refresh=True)
    return r["path"]
