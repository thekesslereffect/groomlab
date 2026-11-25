# SPDX-License-Identifier: Apache-2.0
"""Whether a groom's data changed since a panel last read it, without reading it: a count per
attribute of each groom shape, raised by a Maya change callback whenever that attribute is set (an
edit, a brush stroke, undo and redo alike). A panel polled on a timer keeps the counts it read at
and reads the groom again only when they moved, so a poll with nothing new costs nothing: reading
the guides (parsing them, laying them on the scalp) on every tick was a hitch every few hundred
milliseconds while the Tube tool was up (tumbling the camera, moving the cursor, scrolling the
panel)."""

from maya import cmds
from maya.api import OpenMaya as om2

_watched = {}  # a groom shape's MObjectHandle hash -> (its handle, its callback id, {attribute: count})


def _node(groom):
    try:
        sel = om2.MSelectionList()
        sel.add(groom)
        node = sel.getDependNode(0)
    except (RuntimeError, ValueError):
        return None
    if node.hasFn(om2.MFn.kTransform):
        shapes = cmds.listRelatives(groom, shapes=True, type="groomLabShape", fullPath=True) or []
        return _node(shapes[0]) if shapes else None
    return node


def _changed(msg, plug, _other, counts):
    if msg & (om2.MNodeMessage.kAttributeSet | om2.MNodeMessage.kConnectionMade | om2.MNodeMessage.kConnectionBroken):
        name = plug.partialName(useLongNames=True)
        counts[name] = counts.get(name, 0) + 1


def stamp(groom, attributes):
    """Where `groom`'s `attributes` stand: a value that changes whenever any of them is set, and
    for another groom (or one deleted and made again). None when there is no such groom (read
    every time, as before)."""
    if not groom:
        return None
    node = _node(groom)
    if node is None:
        return None
    handle = om2.MObjectHandle(node)
    key = handle.hashCode()
    entry = _watched.get(key)
    if entry is not None and not (entry[0].isValid() and entry[0].isAlive() and entry[0].object() == node):
        try:
            om2.MMessage.removeCallback(entry[1])
        except RuntimeError:
            pass
        entry = None
    if entry is None:
        counts = {}
        try:
            callback = om2.MNodeMessage.addAttributeChangedCallback(node, _changed, counts)
        except RuntimeError:
            return None
        entry = (handle, callback, counts)
        _watched[key] = entry
    counts = entry[2]
    return (key, id(counts)) + tuple(counts.get(a, 0) for a in attributes)


def stop():
    """Every callback removed (the plug-in unloading)."""
    while _watched:
        _key, (_handle, callback, _counts) = _watched.popitem()
        try:
            om2.MMessage.removeCallback(callback)
        except RuntimeError:
            pass
