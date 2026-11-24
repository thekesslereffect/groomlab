# SPDX-License-Identifier: Apache-2.0
"""Parting lines and flow: where hair does not mix, which way it lies, and the nodes that read them."""

import json

from maya import cmds

from groomlab.base import undoable, _scalp_shape, _shape_of
from groomlab.brushes import brush_stroke, brush_tool


# Parting: lines on the scalp hair does not mix across (Interpolate and Clump
# respect them; the Part node opens the gap)


def add_part(groom, curves, name="part", delete_curves=False):
    """Parts from curves drawn on (or near) the scalp; returns their names.

    Each curve is projected onto the scalp and stored bound to it, so parts
    follow a deforming scalp. The curves are no longer needed afterwards.
    """
    names = cmds.groomLabParts(groom, fromCurves=[_shape_of(c, "nurbsCurve") for c in curves], name=name) or []
    if delete_curves:
        cmds.delete(curves)
    return names


def list_parts(groom):
    return cmds.groomLabParts(groom, list=True) or []


def remove_part(groom, name):
    cmds.groomLabParts(groom, remove=name)


def clear_parts(groom):
    cmds.groomLabParts(groom, clear=True)


def mirror_parts(groom, side="negative", symmetry="x", center=0.0):
    """Mirror to Other Side for parts (issue 105): the parts on `side` ("negative": -X, "positive": +X)
    copied over the other, whose own parts go; a part crossing the middle once is made symmetric.
    `symmetry` "x", "y", "z" (a plane at `center`) or "topology" (across the scalp), as the brushes.
    Returns "<n> mirrored, <n> replaced". One undo step."""
    return cmds.groomLabParts(groom, mirror=side, symmetry=symmetry or "x", center=center)


# ---------------------------------------------------------------------------
# Flow arrows: which way hair lies on the scalp, for the Flow node


def flow_tool(groom=None, radius=60, strength=1.0, symmetry=None):
    """Lay flow arrows by dragging on the scalp (the groom is selected first when given).

    Arrows go down every half a radius along the stroke; ones under the brush
    turn toward it (`strength`); Ctrl+drag erases. The Flow node lays strands
    along the field. One undo step per stroke.
    """
    if groom:
        cmds.select(groom)
    return brush_tool("flow", radius=radius, strength=strength, symmetry=symmetry)  # (makes the Flow node)


def flow_stroke(groom, points, radius=0.25, strength=1.0, erase=False, symmetry=None):
    """Scripted flow stroke: arrows along `points` (world, projected onto the scalp) every radius / 2,
    arrows within `radius` of the path turned toward it; `erase` removes those instead. One undo step.
    A graph without a Flow node gets one, so the arrows lay the strands (issue #112)."""
    if not erase:
        ensure_flow_node(groom)
    return brush_stroke(groom, "flow", points, radius=radius, strength=-abs(strength) if erase else abs(strength),
                        symmetry=symmetry)


FLOW_SINKS = ("whorl", "whorlcw", "converge", "part")


def flow_sink(groom, point, kind="whorl", radius=1.0, erase=False, symmetry=None):
    """Place a flow sink at the scalp point nearest `point`: hair within `radius`
    turns around it. whorl spirals out counter-clockwise (seen from outside),
    whorlcw clockwise, converge gathers in, part points away. `erase` removes
    sinks within `radius` instead; `symmetry` mirrors it. One undo step."""
    if kind not in FLOW_SINKS:
        raise ValueError("kind must be one of %s" % (FLOW_SINKS,))
    cmds.groomLabBrushStroke(groom, mode="flow", flowSink=kind, radius=radius, strength=-1.0 if erase else 1.0,
                             symmetry=symmetry or "none", point=[tuple(point)])


def mirror_flow(groom, side="negative", symmetry="x", center=0.0):
    """Mirror to Other Side for flow (issue 105): the arrows and sinks on `side` ("negative": -X,
    "positive": +X) copied over the other, whose own go (a whorl's mirror turns the other way).
    `symmetry` as mirror_parts'. Returns "<n> mirrored, <n> replaced". One undo step."""
    return cmds.groomLabParts(groom, mirror=side, symmetry=symmetry or "x", center=center, flow=True)


def _flow_data(groom):
    """The groom's flowData as a dict ({} when it has no flow)."""
    text = cmds.getAttr(groom + ".flowData") or ""
    return json.loads(text) if text.strip() else {}


def flow_sinks(groom):
    """The flow sinks as [(face, u, v, angle, radius)]."""
    return [(s["face"], s["u"], s["v"], s["angle"], s["radius"]) for s in _flow_data(groom).get("sinks", [])]


def flow_arrows(groom):
    """The flow arrows as [(face, u, v)], for counting and scripts."""
    return [(p["face"], p["u"], p["v"]) for p in _flow_data(groom).get("pins", [])]


def flow_counts(groom):
    """(arrows, sinks): how many flow arrows and sinks the groom keeps (its flowData)."""
    try:
        data = _flow_data(groom)
    except ValueError:  # unreadable: as none
        return 0, 0
    return len(data.get("pins", [])), len(data.get("sinks", []))


@undoable("clear flow")
def clear_flow(groom):
    """Remove every flow arrow and sink from the groom (GroomLab > Utilities > Delete All Flow Arrows,
    the Flow tool's Clear All); one undo step. Returns (arrows, sinks) removed."""
    counts = flow_counts(groom)
    if (cmds.getAttr(groom + ".flowData") or "").strip():
        cmds.setAttr(groom + ".flowData", "", type="string")
    return counts


@undoable("flow node")
def add_flow_node(groom, after="interpolate", lift=12.0, bend=0.6, if_missing=False):
    """Add a Flow node (strands laid along the arrows) after `after`; returns its name.
    With if_missing, an existing Flow node is returned instead, and a graph without one gets it
    after its first Interpolate (guideless: its Scatter) with the spec's defaults (every host's
    Flow tool, graph_inserts.h insertFlow)."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    if if_missing:
        edit = model._edit()
        with gm._core_errors():
            node, made = edit.insert_flow("", after if after in model.nodes else "", model.groom_size)
        if made:
            model._take(edit)
            model.save(groom)
        return node
    node = model.add_node("Flow", params={"lift": lift, "bend": bend})
    model.insert_after(after, node)
    model.save(groom)
    return node


@undoable("part node")
def add_part_node(groom, width=0.1, strength=1.0, after="interpolate", if_missing=False):
    """Add a Part node (opens the parts: strands lean away from them) after `after`; returns its name.
    With if_missing, an existing Part node is returned instead. A graph without
    `after` (guideless: no Interpolate) gets it after its first Flow or Bend, else
    its Scatter."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    if if_missing:
        # Every host's (graph_inserts.h insertPart): the Part node there, or one after `after`.
        edit = model._edit()
        with gm._core_errors():
            node, made = edit.insert_part(json.dumps({"width": width, "strength": strength}),
                                          after if after in model.nodes else "", model.groom_size)
        if made:
            model._take(edit)
            model.save(groom)
        return node
    if after not in model.nodes:
        after = next((n for kind in ("Interpolate", "Flow", "Bend", "Scatter") for n in model.order
                      if model.node_type(n) == kind), None)
        if after is None:
            raise ValueError("the graph has no strands to part (no Scatter)")
    node = model.add_node("Part", params={"width": width, "strength": strength})
    model.insert_after(after, node)
    model.save(groom)
    return node


def ensure_flow_node(groom):
    """The groom's Flow node, made when its graph has none (drawing flow lays the strands, as
    drawing a part opens them: issue #112). None when the graph has no strands to lay (no
    Scatter) or the groom cannot be read; the tool still draws the arrows."""
    try:
        return add_flow_node(groom, if_missing=True)
    except (RuntimeError, ValueError):
        return None


def draw_part_tool(groom, name="part", symmetry=None):
    """Draw parts freehand on the scalp: selects the groom and starts the part tool.

    Press on the scalp and draw; the part is added on release (one undo step),
    named `name` (made unique). The groom gets a Part node if it has none, so
    the part opens.
    """
    if not _scalp_shape(groom):
        raise RuntimeError("%s has no scalp connected" % groom)
    add_part_node(groom, if_missing=True)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    return brush_tool("part", part_name=name, symmetry=symmetry)
