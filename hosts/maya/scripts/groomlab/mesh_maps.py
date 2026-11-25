# SPDX-License-Identifier: Apache-2.0
"""Painted vertex maps kept on the scalp meshes themselves (issue #113): each vertex map of a groom
(its scalpMaps) written as a color set of that name on the scalps it spans, so Maya's Color Set
Editor lists it and Arnold (aiUserDataColor, a color set's name) and other tools read it there.

The groom's scalpMaps stay the maps' source: the color sets are a copy, written again whenever a
groom's maps change (a stroke of any tool that paints them, a map made, removed or renamed, undo:
a change callback on its scalpMaps, run once the change is done), when the scene is saved, and by
maps_to_mesh; an open Color Set Editor is refreshed to show them. A deformed
scalp gets them on its original (Orig) shape, so they come through its deformers. A scalp with
other construction history (a polySphere, a Combine kept live) cannot keep data of its own: it is
skipped with a warning (delete its non-deformer history to keep the maps on it). The sets this
writes are recorded on the mesh (groomLabMapSets), so removing or renaming a map takes its own
set along and never a color set of yours of the same name."""

import json

import groomlab_core
from maya import cmds
from maya.api import OpenMaya as om2

from groomlab.base import _original_shape, no_undo

OWNED_ATTR = "groomLabMapSets"  # on a mesh: the color sets GroomLab wrote there (a JSON list)
_callbacks = []       # the scene callbacks (save, grooms added)
_node_callbacks = {}  # a groom shape's MObjectHandle hash -> its scalpMaps change callback
_pending = {}         # grooms whose maps changed (hash -> MObjectHandle), written when the change is done
_warned = set()


def _target(mesh):
    """The shape a scalp's color sets go on: its Orig shape when deformed (they come through the
    deformers), else itself."""
    return _original_shape(mesh) or mesh


def _has_history(shape):
    return bool(cmds.listConnections(shape + ".inMesh", source=True, destination=False))


def _owned(shape):
    if not cmds.attributeQuery(OWNED_ATTR, node=shape, exists=True):
        return []
    try:
        return list(json.loads(cmds.getAttr(shape + "." + OWNED_ATTR) or "[]"))
    except ValueError:
        return []


def _set_owned(shape, names):
    if not cmds.attributeQuery(OWNED_ATTR, node=shape, exists=True):
        cmds.addAttr(shape, longName=OWNED_ATTR, dataType="string", hidden=True)
    cmds.setAttr(shape + "." + OWNED_ATTR, json.dumps(sorted(set(names))), type="string")


def _scalp_meshes(groom):
    """{scalp name: mesh shape} and [(scalp name, vertex count)] in join order (the groom's own,
    "scalp", first), as the vertex maps span them."""
    from groomlab.extra_scalps import _scalp_counts, scalps
    from groomlab.groom import scalp_mesh
    meshes = {"scalp": scalp_mesh(groom)}
    meshes.update((s["name"], s["mesh"]) for s in scalps(groom))
    return meshes, _scalp_counts(groom)


def _write(shape, name, rgba):
    """The color set `name` on `shape`, one color per vertex (r g b a). False when the counts differ."""
    sel = om2.MSelectionList()
    sel.add(shape)
    fn = om2.MFnMesh(sel.getDagPath(0))
    count = fn.numVertices
    if len(rgba) != 4 * count:
        return False
    if name not in fn.getColorSetNames():
        fn.createColorSet(name, True, rep=om2.MFnMesh.kRGBA)
    colors = om2.MColorArray()
    colors.setLength(count)
    for v in range(count):
        colors[v] = om2.MColor((rgba[4 * v], rgba[4 * v + 1], rgba[4 * v + 2], rgba[4 * v + 3]))
    fn.setColors(colors, name, om2.MFnMesh.kRGBA)
    _counts, vertices = fn.getVertices()
    fn.assignColors(vertices, name)  # every corner its vertex's color
    return True


def maps_to_mesh(groom, names=None):
    """Write the groom's painted vertex maps (all, or `names`) as color sets of its scalp meshes:
    a gray map r = g = b, alpha 1; a color map its r g b a. Returns [(mesh, color set)] written. A copy:
    the groom keeps the maps (no undo step of its own; written again the next time)."""
    text = cmds.getAttr(groom + ".scalpMaps") or ""
    if not text.strip():
        return []
    meshes, counts = _scalp_meshes(groom)
    try:
        sets = groomlab_core.vertex_map_color_sets(text, counts, list(names or []))
    except ValueError:
        return []  # bad scalpMaps text: the shape reports it
    written = []
    owned = {}
    for scalp, name, rgba in sets:
        mesh = meshes.get(scalp)
        if not mesh or not cmds.objExists(mesh):
            continue
        shape = _target(mesh)
        if _has_history(shape):
            if shape not in _warned:
                _warned.add(shape)
                cmds.warning("GroomLab: %s has construction history, so the painted maps cannot be kept on it as "
                             "color sets (delete its non-deformer history to keep them)" % shape.split("|")[-1])
            continue
        if _write(shape, name, rgba):
            owned.setdefault(shape, set(_owned(shape))).add(name)
            written.append((shape, name))
    if names is None:
        # (All written: the sets GroomLab wrote for maps the groom no longer has, a map undone or
        # removed elsewhere, taken off; never a color set of yours.)
        kept = {name for _scalp, name, _rgba in sets}
        for mesh in meshes.values():
            if not mesh or not cmds.objExists(mesh):
                continue
            shape = _target(mesh)
            gone = [n for n in _owned(shape) if n not in kept]
            if not gone:
                continue
            sel = om2.MSelectionList()
            sel.add(shape)
            fn = om2.MFnMesh(sel.getDagPath(0))
            present = set(fn.getColorSetNames())
            for n in gone:
                if n in present:
                    fn.deleteColorSet(n)
            owned[shape] = set(owned.get(shape, set(_owned(shape)))) - set(gone)
    with no_undo():  # (bookkeeping of a copy: no undo step)
        for shape, names_on in owned.items():
            if set(names_on) != set(_owned(shape)):
                _set_owned(shape, names_on)
    return written


def _owning_shapes(groom, name):
    meshes, _counts = _scalp_meshes(groom)
    out = []
    for mesh in meshes.values():
        if mesh and cmds.objExists(mesh):
            shape = _target(mesh)
            if name in _owned(shape) and name in (cmds.polyColorSet(shape, query=True, allColorSets=True) or []):
                out.append(shape)
    return out


def remove_mesh_map(groom, name):
    """The color set a removed map left on the scalp meshes deleted (the ones GroomLab wrote only;
    undoable, in the caller's undo step)."""
    for shape in _owning_shapes(groom, name):
        cmds.polyColorSet(shape, delete=True, colorSet=name)
        _set_owned(shape, [n for n in _owned(shape) if n != name])


def rename_mesh_map(groom, old, new):
    """A renamed map's color set renamed with it on the scalp meshes (GroomLab's own only)."""
    for shape in _owning_shapes(groom, old):
        if new in (cmds.polyColorSet(shape, query=True, allColorSets=True) or []):
            cmds.polyColorSet(shape, delete=True, colorSet=old)  # (yours of that name stays; the map's is rewritten)
        else:
            cmds.polyColorSet(shape, rename=True, colorSet=old, newColorSet=new)
        _set_owned(shape, [n for n in _owned(shape) if n != old] + [new])


def all_maps_to_mesh():
    """Every groom's vertex maps onto its scalp meshes (before a save)."""
    from groomlab.groom import list_grooms
    for groom in list_grooms():
        try:
            maps_to_mesh(groom)
        except Exception as e:  # a save must never fail on this
            cmds.warning("GroomLab: maps of %s not kept on its scalp: %s" % (groom, e))
    refresh_editor()


def refresh_editor():
    """Maya's Color Set Editor, when open, listing the sets as they are now (it does not see sets
    written through the API on its own)."""
    try:
        if cmds.window("colorSetEditor", exists=True):
            from maya import mel
            mel.eval("updateColorSetEditor")
    except Exception:
        pass


def _flush():
    handles = list(_pending.values())
    _pending.clear()
    for handle in handles:
        if not handle.isValid() or not handle.isAlive():
            continue
        try:
            groom = om2.MFnDagNode(handle.object()).partialPathName()
            maps_to_mesh(groom)
        except Exception as e:
            cmds.warning("GroomLab: painted maps not kept on the scalp: %s" % e)
    refresh_editor()


def _maps_changed(msg, plug, _other, handle):
    if not msg & om2.MNodeMessage.kAttributeSet or plug.partialName(useLongNames=True) != "scalpMaps":
        return
    if not _pending:
        cmds.evalDeferred(_flush, lowestPriority=True)  # (once the stroke, undo or edit is done)
    _pending[handle.hashCode()] = handle


def _watch(node):
    """A groom shape's scalpMaps watched: its maps written to its scalps whenever they change."""
    try:
        if om2.MFnDependencyNode(node).typeName != "groomLabShape":
            return
        handle = om2.MObjectHandle(node)
        if handle.hashCode() in _node_callbacks:
            return
        _node_callbacks[handle.hashCode()] = om2.MNodeMessage.addAttributeChangedCallback(node, _maps_changed, handle)
    except RuntimeError:
        pass


def _unwatch_all():
    while _node_callbacks:
        try:
            om2.MMessage.removeCallback(_node_callbacks.popitem()[1])
        except RuntimeError:
            pass


def _watch_scene(*_):
    """Every groom in the scene watched (after a scene is opened or made, and at start)."""
    _unwatch_all()
    shapes = cmds.ls(type="groomLabShape") or []
    if shapes:
        sel = om2.MSelectionList()
        for shape in shapes:
            sel.add(shape)
        for i in range(sel.length()):
            _watch(sel.getDependNode(i))


def start():
    """Write the maps onto the meshes whenever they change and before every save (GroomLab's menu
    installs this)."""
    if _callbacks:
        return
    _callbacks.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kBeforeSave, lambda *_: all_maps_to_mesh()))
    _callbacks.append(om2.MDGMessage.addNodeAddedCallback(lambda node, *_: _watch(node), "groomLabShape"))
    for message in (om2.MSceneMessage.kAfterOpen, om2.MSceneMessage.kAfterNew, om2.MSceneMessage.kAfterImport):
        _callbacks.append(om2.MSceneMessage.addCallback(message, _watch_scene))
    _watch_scene()


def stop():
    while _callbacks:
        try:
            om2.MMessage.removeCallback(_callbacks.pop())
        except RuntimeError:
            pass
    _unwatch_all()
    _pending.clear()
