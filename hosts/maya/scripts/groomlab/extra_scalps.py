# SPDX-License-Identifier: Apache-2.0
"""Several scalps on one groom: other meshes the hair grows on (separate ears, a body and a head,
eyebrow patches), joined after the groom's own scalp (inMesh). A Scalp node whose `input` names one
grows on it alone (scalp_pieces.h)."""

import json
import re

from maya import cmds

from groomlab.base import undoable, _original_shape, _shape_of

NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def scalps(groom):
    """The groom's other scalps: [{"name", "mesh" (shape), "index", "rest" (the rest mesh shape, or
    None)}] in the order they join (index order). The groom's own scalp is groomlab.scalp_mesh."""
    out = []
    for index in cmds.getAttr(groom + ".extraScalps", multiIndices=True) or []:
        element = "%s.extraScalps[%d]" % (groom, index)
        sources = cmds.listConnections(element + ".extraScalpMesh", source=True, destination=False, shapes=True) or []
        if not sources:
            continue
        mesh = _shape_of(sources[0], "mesh")
        name = cmds.getAttr(element + ".extraScalpName") or _default_name(mesh)
        rests = cmds.listConnections(element + ".extraScalpRestMesh", source=True, destination=False, shapes=True) or []
        out.append({"name": name, "mesh": mesh, "index": index, "rest": _shape_of(rests[0], "mesh") if rests else None})
    return out


def _default_name(mesh):
    """What the node calls an unnamed scalp: the mesh's transform name, no namespace."""
    parent = cmds.listRelatives(mesh, parent=True) or [mesh]
    return parent[0].split("|")[-1].split(":")[-1]


def valid_name(name):
    """Whether `name` can name another scalp: letters, digits and _ (not starting with a digit), not
    "scalp" (the groom's own)."""
    return bool(name) and name != "scalp" and bool(NAME.match(name))


def _point_counts(groom):
    """The vertex counts of the groom's scalps in the order they join: its own, then the others."""
    from groomlab.groom import scalp_mesh
    own = scalp_mesh(groom)
    counts = [cmds.polyEvaluate(own, vertex=True) if own else 0]
    counts += [cmds.polyEvaluate(s["mesh"], vertex=True) for s in scalps(groom)]
    return counts


def _resize_vertex_maps(groom, edit):
    """The painted vertex maps (scalpMaps) refitted when a scalp joins or leaves: `edit(values,
    channels)` returns each map's new values."""
    text = cmds.getAttr(groom + ".scalpMaps") or ""
    if not text.strip():
        return
    try:
        maps = json.loads(text)
    except ValueError:
        return
    changed = False
    for entry in maps.values() if isinstance(maps, dict) else []:
        if not isinstance(entry, dict) or entry.get("kind") != "vertices" or not isinstance(entry.get("values"), list):
            continue
        values = edit(entry["values"], 4 if entry.get("channels") == 4 else 1)
        if values is not None:
            entry["values"] = values
            changed = True
    if changed:
        cmds.setAttr(groom + ".scalpMaps", json.dumps(maps, separators=(",", ":")), type="string")


@undoable("add scalp")
def add_scalp(groom, mesh, name=None, rest=True, wire=True):
    """Make `mesh` (a transform or mesh shape) another scalp of the groom: hair grows on it too.

    Its world mesh is connected to the groom (`extraScalps`) under `name` (the mesh's transform name
    by default), joined after the groom's own scalp: guides, parts, flow and painted maps work on it
    as on the scalp, and it moves, deforms and renders with the groom. With `wire`, a Scalp node
    reading it (`scalp_<name>`), a Scatter (the groom's own Scatter's settings) and an Interpolate
    (the groom's guides) grow its hair, merged into the groom's strands right after the groom's own
    Interpolate, so the look that follows applies to both. Returns the name; with an existing scalp
    of that name the mesh is swapped and the graph left alone.

    `rest`: its rest pose (`extraScalpRestMesh`), as the scalp's rest mesh: True (the default) a
    deformed mesh's original shape under its deformers when it has one; a mesh: that mesh; False:
    none."""
    from groomlab import graph as gm
    from groomlab.groom import scalp_mesh
    shape = _shape_of(mesh, "mesh")
    own = scalp_mesh(groom)
    if own and cmds.ls(own, long=True) == cmds.ls(shape, long=True):
        raise ValueError("%s is the groom's scalp already" % shape.split("|")[-1])
    name = name or _default_name(shape)
    if not valid_name(name):
        raise ValueError("pick another scalp name than %r (letters, digits and _, not 'scalp')" % name)
    current = scalps(groom)
    taken = next((s for s in current if cmds.ls(s["mesh"], long=True) == cmds.ls(shape, long=True)), None)
    if taken and taken["name"] != name:
        raise ValueError("%s is a scalp of the groom already (%s)" % (shape.split("|")[-1], taken["name"]))
    existing = next((s for s in current if s["name"] == name), None)
    if existing:
        index = existing["index"]
    else:
        used = set(cmds.getAttr(groom + ".extraScalps", multiIndices=True) or [])
        index = max(used) + 1 if used else 0  # after every other: the order they join in
        # Painted vertex maps: the new scalp's vertices unpainted (0), after the others'.
        added = cmds.polyEvaluate(shape, vertex=True)
        _resize_vertex_maps(groom, lambda values, per: values + ([0.0, 0.0, 0.0, 1.0] * added if per == 4 else [0.0] * added)
                            if len(values) == sum(_point_counts(groom)) * per else None)
    element = "%s.extraScalps[%d]" % (groom, index)
    cmds.connectAttr(shape + ".worldMesh[0]", element + ".extraScalpMesh", force=True)
    _connect_rest(element, shape, rest)
    cmds.setAttr(element + ".extraScalpName", name, type="string")
    if existing or not wire:
        return name
    model = gm.GraphModel.from_groom(groom)
    if any(model.node_type(n) == "Scalp" and model.param(n, "input") == name for n in model.order):
        return name  # a Scalp node reads it already: the graph stays
    own_scalp = next((n for n in model.order if model.node_type(n) == "Scalp" and model.param(n, "input") == "scalp"), None)
    own_scatter = next((c for c, port in model.downstream_of(own_scalp) if model.node_type(c) == "Scatter"), None) \
        if own_scalp else None
    own_interp = next((c for c, port in model.downstream_of(own_scatter) if model.node_type(c) == "Interpolate"), None) \
        if own_scatter else None
    # The guides the hair reads (its Interpolate's): never the first Guides node there is (a braid's).
    guides = model.source_of(own_interp, "guides") if own_interp else None
    scalp_node = model.add_node("Scalp", name=model.unique_name("scalp_" + name), params={
        "input": name, "subdivision": model.param(own_scalp, "subdivision") if own_scalp else "matchRender"})
    scatter_params = dict(model.nodes[own_scatter]["params"]) if own_scatter else {}
    scatter_params.pop("seed", None)  # a seed of its own (add_node picks one)
    scatter = model.add_node("Scatter", name=model.unique_name("scatter_" + name), params=scatter_params)
    model.connect(scalp_node, scatter, "scalp")
    last = scatter
    if own_interp:
        interp = model.add_node("Interpolate", name=model.unique_name("interpolate_" + name),
                                params=dict(model.nodes[own_interp]["params"]))
        model.connect(scatter, interp, "strands")
        if guides:
            model.connect(guides, interp, "guides")
        model.connect(scalp_node, interp, "scalp")
        last = interp
    merge = model.add_node("Merge", name=model.unique_name("merge_" + name))
    anchor = own_interp or own_scatter
    if anchor and model.downstream_of(anchor):
        model.insert_after(anchor, merge)  # anchor -> merge.a -> what anchor fed
    elif model.output and model.source_of(model.output, "strands"):
        model.insert_before(model.output, merge)
    model.connect(last, merge, "b")
    x, y = model.position(merge)
    for i, node in enumerate((scalp_node, scatter, last if last != scatter else None)):
        if node:
            model.set_position(node, x - 660.0 + 220.0 * i, y + 160.0)
    model.save(groom)
    return name


def _connect_rest(element, shape, rest):
    """Connects the scalp element's rest mesh (add_scalp's `rest`), or leaves it unconnected."""
    plug = element + ".extraScalpRestMesh"
    for source in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
        cmds.disconnectAttr(source, plug)
    if rest is True:
        original = _original_shape(shape)
        if original:
            cmds.connectAttr(original + ".outMesh", plug, force=True)
    elif rest:
        cmds.connectAttr(_shape_of(rest, "mesh") + ".worldMesh[0]", plug, force=True)


@undoable("remove scalp")
def remove_scalp(groom, name):
    """Drop the scalp `name` (its name or mesh): the connection, and the Scalp nodes reading it with
    what they alone fed (its Scatter and Interpolate; the Merge joining them passes the rest
    through). Painted vertex maps lose its vertices. Returns whether it was there."""
    from groomlab import graph as gm
    current = scalps(groom)
    found = [s for s in current if s["name"] == name or s["mesh"] == name
             or _default_name(s["mesh"]) == name.split("|")[-1]]
    if not found:
        return False
    # Painted vertex maps: the scalp's vertices taken out (before the connection goes).
    counts = _point_counts(groom)
    order = [s["name"] for s in current]
    for s in found:
        at = order.index(s["name"]) + 1  # after the groom's own

        def cut(values, per, at=at, counts=list(counts)):
            if len(values) != sum(counts) * per:
                return None
            start = sum(counts[:at]) * per
            return values[:start] + values[start + counts[at] * per:]
        _resize_vertex_maps(groom, cut)
        del counts[at]
        order.remove(s["name"])
    for s in found:
        element = "%s.extraScalps[%d]" % (groom, s["index"])
        for plug in (".extraScalpMesh", ".extraScalpRestMesh"):
            for source in cmds.listConnections(element + plug, source=True, destination=False, plugs=True) or []:
                cmds.disconnectAttr(source, element + plug)
        cmds.removeMultiInstance(element, b=True)
    model = gm.GraphModel.from_groom(groom)
    names = {s["name"] for s in found}
    readers = [n for n in model.order if model.node_type(n) == "Scalp" and model.param(n, "input") in names]
    if not readers:
        return True
    doomed = set(readers)
    # What the readers alone feed (a Scatter, its Interpolate): every input of theirs from doomed nodes or none.
    grew = True
    while grew:
        grew = False
        for n in model.order:
            if n in doomed or model.node_type(n) in ("Merge", "Output", "Guides"):
                continue
            feeders = {c["from"] for c in model.connections if c["to"] == n}
            if feeders and feeders <= doomed | {g for g in model.order if model.node_type(g) == "Guides"} \
                    and feeders & doomed:
                doomed.add(n)
                grew = True
    merges = set()
    for n in list(doomed):
        for consumer, port in list(model.downstream_of(n)):
            if consumer in doomed:
                continue
            model.disconnect(consumer, port)
            if model.node_type(consumer) == "Merge" and port == "b":
                merges.add(consumer)
    for n in doomed:
        model.remove_node(n)
    # A Merge left with its a alone (the one add_scalp made): taken out, a feeding what it fed.
    for merge in merges:
        if model.source_of(merge, "b") is not None:
            continue
        upstream = model.source_of(merge, "a")
        for to, port in list(model.downstream_of(merge)):
            if upstream:
                model.connect(upstream, to, port)
        if model.output == merge and upstream:
            model.set_output(upstream)
        model.remove_node(merge)
    model.save(groom)
    return True
