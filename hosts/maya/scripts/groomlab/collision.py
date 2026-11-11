# SPDX-License-Identifier: Apache-2.0
"""Colliders: other meshes the graph reads (ColliderMesh nodes), for Collide nodes."""

from maya import cmds

from groomlab.base import undoable, _original_shape, _shape_of


def colliders(groom):
    """The groom's colliders: [{"name", "mesh" (shape), "index", "rest" (the rest mesh shape, or None),
    "friction" (its ColliderMesh node's, 0-1, for a Simulate node it feeds; -1: the Simulate node's)}] in
    connection order."""
    out = []
    frictions = None
    for index in cmds.getAttr(groom + ".colliderMeshes", multiIndices=True) or []:
        element = "%s.colliderMeshes[%d]" % (groom, index)
        sources = cmds.listConnections(element + ".colliderMesh", source=True, destination=False, shapes=True) or []
        if not sources:
            continue
        if frictions is None:
            frictions = _collider_frictions(groom)
        mesh = _shape_of(sources[0], "mesh")
        name = cmds.getAttr(element + ".colliderName") or _collider_default_name(mesh)
        rests = cmds.listConnections(element + ".colliderRestMesh", source=True, destination=False, shapes=True) or []
        out.append({"name": name, "mesh": mesh, "index": index, "rest": _shape_of(rests[0], "mesh") if rests else None,
                    "friction": frictions.get(name, -1.0)})
    return out


def _collider_frictions(groom):
    """Each collider name's friction, as the first ColliderMesh node reading it has it."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    out = {}
    for n in model.order:
        if model.node_type(n) == "ColliderMesh":
            out.setdefault(model.param(n, "name"), float(model.param(n, "friction")))
    return out


@undoable("collider friction")
def set_collider_friction(groom, name, friction):
    """Set how much simulated hair resting on collider `name` is kept from sliding, 0-1 (-1: the
    Simulate node's `friction`): its ColliderMesh nodes' `friction`. Returns whether it has one."""
    from groomlab import graph as gm
    friction = max(-1.0, min(1.0, float(friction)))
    model = gm.GraphModel.from_groom(groom)
    nodes = [n for n in model.order if model.node_type(n) == "ColliderMesh" and model.param(n, "name") == name]
    for n in nodes:
        model.set_param(n, "friction", friction)
    if nodes:
        model.save(groom)
    return bool(nodes)


def _collider_default_name(mesh):
    """What the node calls an unnamed collider: the mesh's transform name, no namespace."""
    parent = cmds.listRelatives(mesh, parent=True) or [mesh]
    return parent[0].split("|")[-1].split(":")[-1]


@undoable("add collider")
def add_collider(groom, mesh, name=None, wire=True, offset=None, before="output", rest=True, friction=None):
    """Make `mesh` (a transform or mesh shape) a collider of the groom: strands keep off it.

    The mesh's world mesh is connected to the groom (`colliderMeshes`) as the graph's mesh
    input `name` (the mesh's transform name by default), read by a ColliderMesh node of that
    name. With `wire`, a Collide node (`collide_<name>`, `offset` in scene units) goes just
    before `before` (the output), so it acts on the finished look, with the ColliderMesh on
    its `collider` input; move it in the Graph Editor to collide earlier. Any mesh works:
    a body, a collar, another character. It moves with the mesh, in the viewport, in
    renders and in exports. Returns the ColliderMesh node's name (`name`); with an existing
    collider of that name the mesh is swapped and the graph left alone.

    `rest`: the collider's rest pose (`colliderRestMesh`), as the scalp's rest mesh: collisions with
    a deforming collider then ask the distance field of its rest pose, made once, instead of one made
    for every frame. True (the default): a deformed mesh's (skinned, blendshapes) original shape
    under its deformers, in object space (it stays put when the transform animates), when it has
    one; a mesh: that mesh (the same vertices, undeformed); False: none.

    `friction`: how much simulated hair resting on it is kept from sliding, 0-1, in a Simulate node
    it feeds (the ColliderMesh node's `friction`; None leaves it, -1: the Simulate node's own)."""
    from groomlab import graph as gm
    shape = _shape_of(mesh, "mesh")
    name = name or _collider_default_name(shape)
    if name == "scalp" or ":" in name or not name.strip():
        raise ValueError("pick another collider name than %r" % name)
    existing = next((c for c in colliders(groom) if c["name"] == name), None)
    if existing:
        index = existing["index"]
    else:
        used = set(cmds.getAttr(groom + ".colliderMeshes", multiIndices=True) or [])
        index = next(i for i in range(len(used) + 1) if i not in used)
    element = "%s.colliderMeshes[%d]" % (groom, index)
    cmds.connectAttr(shape + ".worldMesh[0]", element + ".colliderMesh", force=True)
    _connect_collider_rest(element, shape, rest)
    cmds.setAttr(element + ".colliderName", name, type="string")
    if existing:
        if friction is not None:
            set_collider_friction(groom, name, friction)
        return name
    model = gm.GraphModel.from_groom(groom)
    if before != "output" and wire:
        raise ValueError("a collider's Collide goes before the output (the Graph Editor moves it)")
    # Every host's graph (graph_inserts.h insertCollider): a ColliderMesh reading it, a Collide
    # before the output, off the scalp too.
    edit = model._edit()
    with gm._core_errors():
        node = edit.insert_collider(name, bool(wire), None if offset is None else float(offset),
                                    None if friction is None else float(friction), model.groom_size)
    model._take(edit)
    model.save(groom)
    return node


def _connect_collider_rest(element, shape, rest):
    """Connects the collider element's rest mesh (add_collider's `rest`), or leaves it unconnected."""
    plug = element + ".colliderRestMesh"
    for source in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
        cmds.disconnectAttr(source, plug)
    if rest is True:
        original = _original_shape(shape)
        if original:
            cmds.connectAttr(original + ".outMesh", plug, force=True)
    elif rest:
        cmds.connectAttr(_shape_of(rest, "mesh") + ".worldMesh[0]", plug, force=True)


@undoable("remove collider")
def remove_collider(groom, name):
    """Drop the collider `name` (its name or mesh): the connection, its ColliderMesh node and the
    Collide nodes it alone fed (their strands pass straight through). Returns whether it was there."""
    from groomlab import graph as gm
    found = [c for c in colliders(groom) if c["name"] == name or c["mesh"] == name
             or _collider_default_name(c["mesh"]) == name.split("|")[-1]]
    if not found:
        return False
    for c in found:
        element = "%s.colliderMeshes[%d]" % (groom, c["index"])
        for source in cmds.listConnections(element + ".colliderMesh", source=True, destination=False, plugs=True) or []:
            cmds.disconnectAttr(source, element + ".colliderMesh")
        cmds.removeMultiInstance(element, b=True)
    model = gm.GraphModel.from_groom(groom)
    names = {c["name"] for c in found}
    readers = [n for n in model.order if model.node_type(n) == "ColliderMesh" and model.param(n, "name") in names]
    if not readers:
        return True
    for reader in readers:
        for consumer, port in list(model.downstream_of(reader)):
            if model.node_type(consumer) != "Collide":
                model.disconnect(consumer, port)  # another reader of the mesh: it reads nothing now
                continue
            # The Collide node made for it (add_collider wired the scalp too): strands pass through.
            upstream = model.source_of(consumer, "strands")
            for to, to_port in list(model.downstream_of(consumer)):
                if upstream:
                    model.connect(upstream, to, to_port)
            model.remove_node(consumer)
        model.remove_node(reader)
    model.save(groom)
    return True
