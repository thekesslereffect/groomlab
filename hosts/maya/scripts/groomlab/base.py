# SPDX-License-Identifier: Apache-2.0
"""The plugin, one-step undo, and the shape lookups every module builds on."""

import contextlib
import functools

from maya import cmds


PLUGIN_NAME = "GroomLab"
GROOM_TYPE = "groomLabShape"


def ensure_plugin():
    """Load the plugin if it is not loaded yet."""
    if not cmds.pluginInfo(PLUGIN_NAME, query=True, loaded=True):
        cmds.loadPlugin(PLUGIN_NAME)


@contextlib.contextmanager
def undo_chunk(name):
    """Everything done to the scene inside `with undo_chunk(name):` is one undo step, named
    "groomLab <name>" (closed however the block ends)."""
    cmds.undoInfo(openChunk=True, chunkName="groomLab " + name)
    try:
        yield
    finally:
        cmds.undoInfo(closeChunk=True)


_no_undo_depth = [0]  # nested no_undo blocks open now: only the outermost one turns the queue off and on


@contextlib.contextmanager
def no_undo():
    """What is done inside `with no_undo():` stays out of Maya's undo queue (bookkeeping: cameras,
    marker sets, a display refresh, stored state). Only the outermost block switches the queue,
    and back to what it found: a block inside another, or opened while the queue is off, never
    turns it on (Maya keeps the queue's state in its prefs, so one left on or off stays so)."""
    was = _no_undo_depth[0] == 0 and cmds.undoInfo(query=True, state=True)
    _no_undo_depth[0] += 1
    if was:
        cmds.undoInfo(stateWithoutFlush=False)
    try:
        yield
    finally:
        _no_undo_depth[0] -= 1
        if was:
            cmds.undoInfo(stateWithoutFlush=True)


def undoable(name):
    """Decorator: everything the function does to the scene is one undo step, named `name`."""
    def wrap(fn):
        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            with undo_chunk(name):
                return fn(*args, **kwargs)
        return wrapper
    return wrap


def _selection(selection):
    """`selection` (a list of nodes or components), or the current selection: never cmds.ls([]),
    which lists every node of the scene."""
    if selection is None:
        return cmds.ls(selection=True, long=True) or []
    return [selection] if isinstance(selection, str) else list(selection)


def selected_meshes(selection=None):
    """The mesh shapes (long names, no intermediate shapes) in `selection` (the current selection
    by default), once each: selected shapes and the meshes under selected transforms or groups,
    then the meshes of selected components (faces, vertices, edges). Every UI action that takes
    "the selected mesh" reads it here, so a face selection counts everywhere."""
    items = _selection(selection)
    if not items:
        return []
    found = cmds.ls(items, dag=True, type="mesh", noIntermediate=True, long=True) or []
    # Components: their mesh (ls dag finds none for "pSphere1.f[0]").
    found += [o for o in cmds.ls(items, objectsOnly=True, long=True) or []
              if cmds.nodeType(o) == "mesh" and not cmds.getAttr(o + ".intermediateObject")]
    return list(dict.fromkeys(found))


def selected_curves(groom=None, selection=None):
    """The nurbsCurve shapes (long names, no intermediate shapes) in `selection` (the current
    selection by default), once each: selected curves and the curves under selected groups. With
    `groom`, its own nodes (its transform and what is under it) are left out, so selecting the
    groom along with the curves is fine."""
    items = _selection(selection)
    if not items:
        return []
    shapes = cmds.ls(items, dag=True, type="nurbsCurve", noIntermediate=True, long=True) or []
    if groom and cmds.objExists(groom):
        own = cmds.listRelatives(groom, parent=True, fullPath=True) if cmds.nodeType(groom) == GROOM_TYPE \
            else cmds.ls(groom, long=True)
        if own:
            shapes = [s for s in shapes if not s.startswith(own[0] + "|")]
    return list(dict.fromkeys(shapes))


def transforms(shapes):
    """The transforms (long names) of `shapes`, in order, once each."""
    out = [(cmds.listRelatives(s, parent=True, fullPath=True) or [s])[0] for s in shapes or []]
    return list(dict.fromkeys(out))


def _shape_of(node, shape_type):
    if cmds.nodeType(node) == shape_type:
        return node
    shapes = cmds.listRelatives(node, shapes=True, type=shape_type, noIntermediate=True, fullPath=True)
    if not shapes:
        raise ValueError("{} has no {} shape".format(node, shape_type))
    return shapes[0]


def _scalp_shape(groom):
    sources = cmds.listConnections(groom + ".inMesh", source=True, destination=False, shapes=True) or []
    return _shape_of(sources[0], "mesh") if sources else None


def _vertex_count(mesh):
    from maya.api import OpenMaya as om
    selection = om.MSelectionList()
    selection.add(mesh)
    return om.MFnMesh(selection.getDagPath(0)).numVertices


def _original_shape(mesh):
    """The undeformed "Orig" shape a deformer inserts under a mesh, if any.

    Only a mesh with a deformer (skin, blend shape, cluster...) in its history has one, and it is
    the same mesh: an intermediate mesh with the same vertex count. Others in the history are not:
    a Combine or Mirror kept with history leaves its pieces behind as intermediate meshes, and a
    Smooth after the deformers changes the count. listHistory lists upstream last, so the last
    match is the original (construction history may still feed it, e.g. polySphere).
    """
    history = cmds.listHistory(mesh) or []
    if not cmds.ls(history, type="geometryFilter"):
        return None
    count = _vertex_count(mesh)
    origs = [m for m in cmds.ls(history, type="mesh") or []
             if m != mesh and cmds.getAttr(m + ".intermediateObject") and _vertex_count(m) == count]
    return origs[-1] if origs else None
