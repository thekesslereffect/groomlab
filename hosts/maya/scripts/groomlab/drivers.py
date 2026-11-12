# SPDX-License-Identifier: Apache-2.0
"""Driver curves: a few curves an animator rigs that pose the guides (DriveGuides nodes).

Rig the curves with anything Maya has (joints and a skinCluster, spline IK, wire, blend shapes),
connect them, and bind them at the bind frame (frame 0 by default: the pose the groom was made in).
From then on every guide follows its nearest drivers, keeping its place round them as they swing,
bend and stretch; a Simulate after the DriveGuides node plays on top of the driven guides:
    add_drivers(groom, ["hair_ctrl_crv1", "hair_ctrl_crv2"])   # connects them, adds a DriveGuides node
    bind_drivers(groom, frame=0)                                # the bind pose, kept with the scene
    remove_drivers(groom)                                       # every driver; the node stays

Bind again after changing the rig's rest pose or adding drivers (a driver added after the bind
drives nothing until then).
"""

import json

from maya import cmds

from groomlab.base import undoable, _shape_of


def _long(node):
    """A node's full DAG path, so the same curve compares equal however it was named."""
    return (cmds.ls(node, long=True) or [node])[0]


def driver_curves(groom):
    """The groom's driver curves: [{"curve" (shape), "index" (its ID)}] in index order."""
    out = []
    for index in cmds.getAttr(groom + ".driverCurves", multiIndices=True) or []:
        sources = cmds.listConnections("%s.driverCurves[%d]" % (groom, index), source=True, destination=False,
                                       shapes=True) or []
        if sources:
            out.append({"curve": _long(_shape_of(sources[0], "nurbsCurve")), "index": index})
    return out


@undoable("add drivers")
def add_drivers(groom, curves, wire=True):
    """Connect `curves` (transforms or nurbsCurve shapes) as the groom's driver curves (each one's
    world space, by the next free index: its ID in the bind). Curves connected already are skipped.
    With `wire`, a DriveGuides node goes right after the Guides node (graph_inserts.h
    insertDriveGuides; the one there is kept). Bind them (bind_drivers) for them to drive. Returns
    the DriveGuides node's name (None without `wire`)."""
    if isinstance(curves, str):
        curves = [curves]
    have = set(d["curve"] for d in driver_curves(groom))
    used = set(cmds.getAttr(groom + ".driverCurves", multiIndices=True) or [])
    index = 0
    for curve in curves:
        shape = _long(_shape_of(curve, "nurbsCurve"))
        if shape in have:
            continue
        while index in used:
            index += 1
        cmds.connectAttr(shape + ".worldSpace[0]", "%s.driverCurves[%d]" % (groom, index), force=True)
        used.add(index)
        have.add(shape)
    if not wire:
        return None
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    edit = model._edit()
    with gm._core_errors():
        node = edit.insert_drive_guides(model.groom_size)
    model._take(edit)
    model.save(groom)
    return node


@undoable("remove drivers")
def remove_drivers(groom, curves=None):
    """Disconnect the driver curves (`curves`: those; None: every one). The bind stays (clear it with
    clear_bind) and the DriveGuides nodes too (with no driver as bound, they pass the guides on).
    Returns how many were disconnected."""
    wanted = None if curves is None else set(_long(_shape_of(c, "nurbsCurve")) for c in ([curves] if isinstance(curves, str) else curves))
    count = 0
    for d in driver_curves(groom):
        if wanted is not None and d["curve"] not in wanted:
            continue
        plug = "%s.driverCurves[%d]" % (groom, d["index"])
        for source in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
            cmds.disconnectAttr(source, plug)
        count += 1
    return count


@undoable("bind drivers")
def bind_drivers(groom, frame=0):
    """Bind the driver curves as they are at `frame` (the scalp then too): each guide keeps its place
    round its nearest drivers as they were there. Stored in the groom's driverBind (saved with the
    scene and in .groom files). Returns how many drivers were bound."""
    text = cmds.groomLabDrivers(groom, bind=True, frame=float(frame)) or ""
    cmds.setAttr(groom + ".driverBind", text, type="string")
    return len(json.loads(text).get("drivers", [])) if text else 0


@undoable("clear driver bind")
def clear_bind(groom):
    """Forget the bind: DriveGuides nodes pass the guides on until the drivers are bound again."""
    cmds.setAttr(groom + ".driverBind", "", type="string")


def bind_frame(groom):
    """The frame the drivers were bound at, or None when they are not bound."""
    text = cmds.getAttr(groom + ".driverBind") or ""
    if not text:
        return None
    try:
        return json.loads(text).get("frame", 0)
    except ValueError:
        return None
