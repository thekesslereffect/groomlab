# SPDX-License-Identifier: Apache-2.0
"""The groom's graph: reading and storing it, one param at a time or as shared text, and the keyable
attributes it reads (controls and keyed params)."""

import json

import groomlab_core

from maya import cmds

from groomlab.base import undo_chunk, undoable


def set_param(groom, node, param, value):
    """Set one parameter of a graph node (a number, string, ramp or map).

    A guide or attribute map on Scatter (which runs before strands exist)
    reads the painted map on the guides, so the guides get connected to it.
    """
    graph = get_graph(groom, expand_default=True)
    for n in graph["nodes"]:
        if n["name"] == node:
            n.setdefault("params", {})[param] = value
            if n["type"] == "Scatter" and isinstance(value, dict) and (value.get("guide") or value.get("attribute")):
                _feed_guides(graph, node)
            break
    else:
        raise ValueError("graph has no node named %r" % node)
    set_graph(groom, graph)


def _feed_guides(graph, node):
    connections = graph.setdefault("connections", [])
    if any(c["to"] == node and c["input"] == "guides" for c in connections):
        return
    source = next((n["name"] for n in graph["nodes"] if n["type"] in ("Guides", "ImportCurves")), None)
    if source:
        connections.append({"from": source, "to": node, "input": "guides"})


# ---------------------------------------------------------------------------
# Graph


def get_graph(groom, expand_default=False):
    """Return the groom's graph as a dict.

    {} means the default graph; pass expand_default=True to get the default
    graph's full JSON instead, ready to edit.
    """
    graph = json.loads(cmds.getAttr(groom + ".graph") or "{}")
    if expand_default and not graph:
        graph = json.loads(cmds.groomLabDefaultGraph())
    return graph


@undoable("graph")
def set_graph(groom, graph):
    """Store a graph dict on the groom (and its Control nodes' attributes): one undo step."""
    sync_controls(groom, graph)
    cmds.setAttr(groom + ".graph", json.dumps(graph, separators=(",", ":")), type="string")


def copy_graph_text(groom):
    """The groom's whole look as text to share (a chat, a forum, a .json file): the preset Save as
    Preset writes (its graph, scalp size, node layout, backdrops) plus its control values. Paste
    it onto another groom with paste_graph_text, or save it as a preset file."""
    from groomlab import presets
    return presets.copy_text(groom)


def paste_graph_text(groom, text, fit=True):
    """Replace the groom's graph with a look copied as text (copy_graph_text, or a preset's JSON):
    checked first (ValueError, nothing changed, when it is not a GroomLab graph), sizes fitted to
    this groom's scalp with `fit`, control values set, one undo step. Returns a report of what
    did not travel (presets.paste_text): painted maps, color sets, files, meshes, colliders, guide
    groups and parts this groom lacks, and a one-line "message"."""
    from groomlab import presets
    return presets.paste_text(groom, text, fit=fit)


# ---------------------------------------------------------------------------
# Controls: keyable attributes on the groom (ctl_<name>) the graph reads, so a look can be dialed
# in the channel box or animated: a Control node's slider, an Expression node's keyable variable,
# and a keyed param (a node's param linked to its own attribute: Set Key in the Graph Editor).

CONTROL_PREFIX = "ctl_"
# A keyed vector's three attributes: a color's r g b (a color attribute), a vector's x y z.
VECTOR_SUFFIXES = {"color": "RGB", "vector": "XYZ"}


def _graph_nodes(graph):
    """Every node of the graph, compounds' inner nodes too (their links read the groom's controls)."""
    for node in graph.get("nodes", []):
        yield node
        definition = node.get("params", {}).get("definition") if node.get("type") == "Compound" else None
        if isinstance(definition, dict):
            for inner in _graph_nodes(definition.get("graph", {})):
                yield inner


def _param_spec(catalog, node, param):
    """A node's param spec: its operator's, or for a compound's exposed param the inner node's."""
    spec = catalog.param_spec(node.get("type"), param)
    if spec is None and node.get("type") == "Script":  # a script node's: its definition's header
        import groomlab_core
        definition = node.get("params", {}).get("definition") or {}
        try:
            specs = json.loads(groomlab_core.node_spec_json(json.dumps({"type": "Script", "params": {"definition": definition}})))
            spec = next((p for p in specs["params"] if p["name"] == param), None)
        except Exception:  # a definition that cannot be read: no spec
            spec = None
    if spec is None and node.get("type") == "Compound":
        definition = node.get("params", {}).get("definition") or {}
        inner_name, _, inner_param = (definition.get("expose") or {}).get(param, "").partition(".")
        inner = next((n for n in definition.get("graph", {}).get("nodes", []) if n.get("name") == inner_name), None)
        if inner is not None:
            spec = _param_spec(catalog, inner, inner_param)
    return spec


def _link_kind(spec, control):
    """The attribute a keyed param takes: double, long, bool, vector or color."""
    if isinstance(control, (list, tuple)):
        return "color" if "".join(c[-1:] for c in control) == VECTOR_SUFFIXES["color"] else "vector"
    return {"int": "long", "bool": "bool"}.get((spec or {}).get("kind"), "double")


def control_base(control):
    """The attribute name (without ctl_) of a keyed param's control: a vector's parent attribute."""
    return control[0][:-1] if isinstance(control, (list, tuple)) else control


def _controls_in(graph):
    """{name: params} of the graph's controls: its Control nodes, its Expression nodes' keyable
    variables ({"value": v, "control": name}, default v, range 0-1 unless set), and its keyed
    params ({"kind": double|long|bool|vector|color, "default": the param's own value})."""
    out = {}
    catalog = None
    for node in _graph_nodes(graph):
        params = node.get("params", {})
        if node.get("type") == "Control":
            out[params.get("name", "control")] = params
        elif node.get("type") == "Expression":
            try:
                variables = json.loads(params.get("variables") or "{}")
            except ValueError:
                variables = {}
            for value in variables.values() if isinstance(variables, dict) else ():
                if isinstance(value, dict) and value.get("control"):
                    out[value["control"]] = {"default": float(value.get("value", 0.0)),
                                             "min": float(value.get("min", 0.0)), "max": float(value.get("max", 1.0))}
        for param, control in (node.get("paramControls") or {}).items():
            if catalog is None:
                from groomlab import graph as gm
                catalog = gm.OperatorCatalog.from_maya()
            spec = _param_spec(catalog, node, param)
            default = params.get(param, (spec or {}).get("default", 0.0))
            out[control_base(control)] = {"kind": _link_kind(spec, control), "default": default, "link": True}
    return out


def _nice_name(name):
    import re
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).replace("_", " ").strip().title()


def _add_control_attr(groom, attr, name, params):
    kind = params.get("kind", "double")
    default = params.get("default", 0.0)
    nice = _nice_name(name)
    if kind in VECTOR_SUFFIXES:
        color = kind == "color"
        values = list(default) if isinstance(default, (list, tuple)) and len(default) == 3 else [0.0, 0.0, 0.0]
        cmds.addAttr(groom, longName=attr, niceName=nice, attributeType="float3" if color else "double3",
                     usedAsColor=color, keyable=True)
        for suffix, value in zip(VECTOR_SUFFIXES[kind], values):
            cmds.addAttr(groom, longName=attr + suffix, niceName="%s %s" % (nice, suffix), keyable=True,
                         attributeType="float" if color else "double", parent=attr, defaultValue=float(value))
    elif kind == "long":
        cmds.addAttr(groom, longName=attr, niceName=nice, attributeType="long", keyable=True,
                     defaultValue=int(round(float(default))))
    elif kind == "bool":
        cmds.addAttr(groom, longName=attr, niceName=nice, attributeType="bool", keyable=True,
                     defaultValue=bool(default))
    elif params.get("link"):
        cmds.addAttr(groom, longName=attr, niceName=nice, attributeType="double", keyable=True,
                     defaultValue=float(default))
    else:
        lo, hi = float(params.get("min", 0.0)), float(params.get("max", 1.0))
        cmds.addAttr(groom, longName=attr, niceName=nice, attributeType="double", keyable=True,
                     defaultValue=float(params.get("default", 0.0)), softMinValue=min(lo, hi),
                     softMaxValue=max(lo, hi))


def _control_plugs_of(groom, attr):
    """The attribute's plugs that carry values: a vector's three children, else itself."""
    children = cmds.attributeQuery(attr, node=groom, listChildren=True) or []
    return [groom + "." + a for a in children] or [groom + "." + attr]


def sync_controls(groom, graph=None):
    """Give the groom a keyable attribute per control (ctl_<name>): per Control node (its default
    and slider range), per keyable variable of an Expression node, and per keyed param (at the
    param's own value; a long for a switch, a bool, three channels for a vector, a color attribute
    for a color). Attributes of removed controls go, unless keyed or connected. Returns the
    attribute names."""
    import re
    graph = get_graph(groom) if graph is None else graph
    wanted = _controls_in(graph)
    for name, params in sorted(wanted.items()):
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            cmds.warning("GroomLab: control name %r is not a valid attribute name" % name)
            continue
        attr = CONTROL_PREFIX + name
        if not cmds.attributeQuery(attr, node=groom, exists=True):
            _add_control_attr(groom, attr, name, params)
        elif params.get("kind", "double") == "double" and not params.get("link"):
            lo, hi = float(params.get("min", 0.0)), float(params.get("max", 1.0))
            cmds.addAttr(groom + "." + attr, edit=True, softMinValue=min(lo, hi), softMaxValue=max(lo, hi))
    for attr in cmds.listAttr(groom, userDefined=True) or []:
        if not attr.startswith(CONTROL_PREFIX) or attr[len(CONTROL_PREFIX):] in wanted:
            continue
        if not cmds.attributeQuery(attr, node=groom, exists=True) or \
                cmds.attributeQuery(attr, node=groom, listParent=True):
            continue  # a vector's channel: it goes with its parent
        plugs = [groom + "." + attr] + _control_plugs_of(groom, attr)
        if not cmds.listConnections(plugs, source=True, destination=False):
            cmds.deleteAttr(groom + "." + attr)
    return [CONTROL_PREFIX + n for n in sorted(wanted)]


def list_controls(groom):
    """{name: current value} of the groom's controls (a keyed vector's: [x, y, z])."""
    out = {}
    for n in _controls_in(get_graph(groom)):
        if cmds.attributeQuery(CONTROL_PREFIX + n, node=groom, exists=True):
            values = [cmds.getAttr(p) for p in _control_plugs_of(groom, CONTROL_PREFIX + n)]
            out[n] = values if len(values) == 3 else values[0]
    return out


@undoable("control")
def add_control(groom, node, param, name=None, default=None, min=0.0, max=1.0):
    """Drive a node's mappable param with a keyable slider on the groom.

    Adds a Control node linked to the param, and the attribute ctl_<name>
    (name defaults to the param's). Returns the attribute ("groom.ctl_name").
    """
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    model.position(node)  # (a node the graph lacks: GraphError)
    # Every host's (graph_controls.h addControl): the param's own name numbered until free unless
    # `name` is given, the slider's range widened to take its start in, left of and below the node.
    edit = model._edit()
    with gm._core_errors():
        control = edit.add_control(node, param, name or "", None if default is None else float(default), float(min),
                                   float(max))
    model._take(edit)
    model.save(groom)
    return groom + "." + CONTROL_PREFIX + model.param(control, "name")


# -- Keyed params: Set Key on a param in the Graph Editor ------------------------------------------
#
# The first key links the param to an attribute of its own, ctl_<node>_<param> (the graph's
# "paramControls"), made at the param's value; the key goes on that attribute, so the Channel
# Box, Graph Editor and Dope Sheet show and edit it as any other animation. Evaluation reads the
# attribute on every frame, exports and farm renders too. Remove Animation bakes the value back.

def _model(groom):
    from groomlab import graph as gm
    return gm.GraphModel.from_groom(groom)


def _control_name(groom, model, node, param):
    """A free control name for node.param: <node>_<param> (the core's control_name, every host's),
    numbered when taken by the graph's controls or the groom's ctl_ attributes."""
    base = groomlab_core.control_name("%s_%s" % (node, param))
    taken = set(_controls_in(model.to_dict()))
    taken |= {a[len(CONTROL_PREFIX):] for a in cmds.listAttr(groom, userDefined=True) or [] if a.startswith(CONTROL_PREFIX)}
    name, i = base, 1
    while name in taken or any(name + s in taken for s in "XYZRGB"):
        name, i = "%s%d" % (base, i), i + 1
    return name


def param_control(groom, node, param):
    """The attribute keying node.param ("groom.ctl_name"), or None when the param is not keyed."""
    control = _model(groom).control_of(node, param)
    return None if control is None else "%s.%s%s" % (groom, CONTROL_PREFIX, control_base(control))


def param_plugs(groom, node, param):
    """The attributes keying node.param ("groom.ctl_...", three for a vector or color), or []
    when the param is not keyed."""
    control = _model(groom).control_of(node, param)
    return _plugs(groom, control) if control is not None else []


def _plugs(groom, control):
    attr = CONTROL_PREFIX + control_base(control)
    if not cmds.attributeQuery(attr, node=groom, exists=True):
        return []
    return _control_plugs_of(groom, attr)


def _as_param(model, node, param, values):
    """Attribute values as the param's value (an int, a bool, [x, y, z] or a float)."""
    kind = model.param_spec(node, param)["kind"]
    if kind == "vec3":
        return [float(v) for v in values]
    if kind == "int":
        return int(round(values[0]))
    if kind == "bool":
        return bool(values[0] >= 0.5)
    return float(values[0])


def param_value(groom, node, param, model=None):
    """A param's value now: its attribute's at the current time when keyed, else the graph's."""
    model = model or _model(groom)
    control = model.control_of(node, param)
    plugs = _plugs(groom, control) if control is not None else []
    if not plugs:
        return model.param(node, param)
    return _as_param(model, node, param, [cmds.getAttr(p) for p in plugs])


def param_key_state(groom, node, param, model=None, time=None):
    """How a param is animated, as the Channel Box colors it: "keyed" (a key at `time`, default
    now), "animated" (keys, none at this time), "driven" (a Control node, or a connection other
    than keys, sets it), "linked" (keyed before, no keys left: its attribute holds the value)
    or None (a plain value)."""
    model = model or _model(groom)
    control = model.control_of(node, param)
    if control is None:
        driver = model.driver_of(node, param)
        return "driven" if driver and model.node_type(driver) == "Control" else None
    plugs = _plugs(groom, control)
    if not plugs:
        return None
    sources = cmds.listConnections(plugs, source=True, destination=False) or []
    curves = [s for s in sources if cmds.nodeType(s).startswith("animCurve")]
    if len(curves) < len(sources):
        return "driven"
    if not curves:
        return "linked"
    t = cmds.currentTime(query=True) if time is None else time
    return "keyed" if any(cmds.keyframe(p, query=True, time=(t, t)) for p in plugs) else "animated"


@undoable("set key")
def set_param_key(groom, node, param, value=None, time=None):
    """Key a graph param at `time` (default now), at `value` (default its value now): one undo step.

    The first key links the param to a keyable attribute of its own on the groom (ctl_<node>_<param>;
    three channels for a vector, a color attribute for a color) made at the param's value. A switch
    (an int) or a toggle gets stepped keys. Params that set counts, segments or seeds cannot be
    keyed (ValueError). Returns the attribute."""
    model = _model(groom)
    control = model.control_of(node, param)
    if control is None:
        why = model.keyable(node, param)
        if why:
            raise ValueError("%s.%s cannot be keyed: %s" % (node, param, why))
        spec = model.param_spec(node, param)
        name = _control_name(groom, model, node, param)
        if spec["kind"] == "vec3":
            control = [name + s for s in VECTOR_SUFFIXES["color" if spec.get("unit") == "color" else "vector"]]
        else:
            control = name
        if param not in model.nodes[node]["params"]:  # stored, so its kind is known (a compound's exposed param)
            model.set_param(node, param, model.param(node, param))
        model.link_control(node, param, control)
        model.save(groom)  # the attribute, at the param's value
    plugs = _plugs(groom, control)
    t = cmds.currentTime(query=True) if time is None else time
    kind = model.param_spec(node, param)["kind"]
    values = [None] * len(plugs)
    if value is not None:
        values = list(value) if len(plugs) == 3 else [value]
    for plug, v in zip(plugs, values):
        flags = {"time": t}
        if v is not None:
            flags["value"] = float(v)
        if kind in ("int", "bool"):
            flags["outTangentType"] = "step"  # a switch jumps from key to key
        cmds.setKeyframe(plug, **flags)
        if v is not None and t == cmds.currentTime(query=True):
            cmds.setAttr(plug, float(v))  # a key replaced on this frame: the value now too
    return "%s.%s%s" % (groom, CONTROL_PREFIX, control_base(control))


@undoable("delete key")
def delete_param_key(groom, node, param, time=None):
    """Delete a keyed param's key at `time` (default now). The last key gone, the param is plain
    again, at its value then (remove_param_animation)."""
    model = _model(groom)
    control = model.control_of(node, param)
    if control is None:
        raise ValueError("%s.%s is not keyed" % (node, param))
    plugs = _plugs(groom, control)
    t = cmds.currentTime(query=True) if time is None else time
    for plug in plugs:
        if cmds.keyframe(plug, query=True, time=(t, t)):
            cmds.cutKey(plug, time=(t, t), clear=True)
    if not any(cmds.keyframe(p, query=True, keyframeCount=True) for p in plugs):
        remove_param_animation(groom, node, param)


@undoable("remove animation")
def remove_param_animation(groom, node, param):
    """Unlink a keyed param: its value now goes back into the graph, its keys go, and its
    attribute too unless something else is connected to it."""
    model = _model(groom)
    control = model.control_of(node, param)
    if control is None:
        return
    value = param_value(groom, node, param, model)
    plugs = _plugs(groom, control)
    if plugs:
        cmds.cutKey(plugs, clear=True)
    model.set_param(node, param, value)
    model.unlink_control(node, param)
    model.save(groom)  # sync_controls drops the attribute when nothing drives it


def set_param_value(groom, node, param, value):
    """Set a param as the Graph Editor does: a keyed param's attribute (a key with Auto Key on and
    keys there, as Maya does; else the value holds until the time changes), else the graph."""
    model = _model(groom)
    control = model.control_of(node, param)
    plugs = _plugs(groom, control) if control is not None else []
    if not plugs:
        set_param(groom, node, param, value)
        return
    state = param_key_state(groom, node, param, model)
    if state in ("keyed", "animated") and cmds.autoKeyframe(query=True, state=True):
        set_param_key(groom, node, param, value=value)
        return
    values = list(value) if len(plugs) == 3 else [value]
    with undo_chunk("set param"):
        for plug, v in zip(plugs, values):
            cmds.setAttr(plug, float(v))


def select_param_attribute(groom, node, param):
    """Select the groom with a keyed param's attribute picked in the Channel Box (Maya's Graph
    Editor then shows its curve). Returns the attribute, or None when the param is not keyed."""
    attr = param_control(groom, node, param)
    if attr is None:
        return None
    cmds.select(groom, replace=True)
    try:
        cmds.channelBox("mainChannelBox", edit=True, select=_control_plugs_of(groom, attr.split(".", 1)[1]))
    except (RuntimeError, TypeError):  # no channel box (batch)
        pass
    return attr
