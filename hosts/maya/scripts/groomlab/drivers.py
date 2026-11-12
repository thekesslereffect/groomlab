# SPDX-License-Identifier: Apache-2.0
"""Driver curves: curves an animator rigs that pose the guides attached to them (DriveGuides nodes).

Rig the curves with anything Maya has (joints and a skinCluster, spline IK, wire, blend shapes),
then attach guides to them: select the guides (the GroomLab select tool) and a driver curve, and
attach. The attached guides (and their children) keep their place round their drivers as they
swing, bend and stretch from the bind pose (frame 0 by default: the pose the groom was made in);
every other guide stays as groomed. A Simulate after the DriveGuides node plays on top:
    attach_guides(groom, "hair_ctrl_crv1")                  # the selected guides follow it
    attach_guides(groom, "hair_ctrl_crv2", group="bangs")   # a whole guide group
    detach_guides(groom, guides=[12, 13])                   # those stay as groomed again
    bind_drivers(groom, frame=0)                            # bind again after changing the rig's rest pose

Attaching connects the curve, adds the DriveGuides node right after the Guides node when there is
none, and binds the drivers at the bind frame (the attachments are kept when binding again).
"""

import json

from maya import cmds

from groomlab.base import undoable, _shape_of


def _long(node):
    """A node's full DAG path, so the same curve compares equal however it was named."""
    return (cmds.ls(node, long=True) or [node])[0]


def _bind(groom):
    """The groom's bind as stored (driverBind), as a dict ({} when not bound)."""
    text = cmds.getAttr(groom + ".driverBind") or ""
    try:
        return json.loads(text) if text else {}
    except ValueError:
        return {}


def _store(groom, bind):
    cmds.setAttr(groom + ".driverBind", json.dumps(bind, separators=(",", ":")) if bind else "", type="string")


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
    world space, by the next free index: its ID). Curves connected already are skipped. With
    `wire`, a DriveGuides node goes right after the Guides node (graph_inserts.h insertDriveGuides;
    the one there is kept). Returns the DriveGuides node's name (None without `wire`)."""
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


def _guide_targets(groom, guides, group):
    """The guide IDs an attach or detach works on: `guides`, else `group`'s, else the selected ones."""
    import groomlab
    if guides is None and group:
        guides = groomlab.guide_ids(groom, group)
    if guides is None:
        guides = groomlab.selected_guides(groom)
    return sorted(set(int(g) for g in (guides or [])))


@undoable("attach guides to driver")
def attach_guides(groom, driver, guides=None, group=None, frame=None):
    """Attach guides to the driver curve `driver` (a transform or nurbsCurve shape): `guides` (IDs),
    else every guide of `group`, else the selected guides. They follow it (and their children with
    them); a guide attached to several drivers follows each, the nearest most. The curve is
    connected (add_drivers) and the drivers are bound at `frame` (the bind's frame, else 0). Returns
    the attached guide IDs."""
    ids = _guide_targets(groom, guides, group)
    if not ids:
        raise ValueError("no guides to attach: select some with the GroomLab select tool, or name them")
    add_drivers(groom, [driver])
    shape = _long(_shape_of(driver, "nurbsCurve"))
    index = next(d["index"] for d in driver_curves(groom) if d["curve"] == shape)
    bind = _bind(groom)
    attached = bind.setdefault("attached", {})
    for g in ids:
        drivers = attached.setdefault(str(g), [])
        if index not in drivers:
            drivers.append(index)
    _store(groom, bind)
    bind_drivers(groom, frame=bind.get("frame", 0) if frame is None else frame)
    return ids


@undoable("detach guides")
def detach_guides(groom, guides=None, group=None, driver=None):
    """Detach guides (`guides`, else `group`'s, else the selected ones) from `driver`, or from every
    driver: they stay as groomed again. Returns how many guides were detached."""
    ids = _guide_targets(groom, guides, group)
    bind = _bind(groom)
    attached = bind.get("attached", {})
    index = None
    if driver is not None:
        shape = _long(_shape_of(driver, "nurbsCurve"))
        index = next((d["index"] for d in driver_curves(groom) if d["curve"] == shape), None)
    count = 0
    for g in ids:
        drivers = attached.get(str(g))
        if not drivers:
            continue
        if index is None:
            del attached[str(g)]
        elif index in drivers:
            drivers.remove(index)
            if not drivers:
                del attached[str(g)]
        else:
            continue
        count += 1
    if not attached:
        bind.pop("attached", None)
    _store(groom, bind)
    return count


def attached_guides(groom):
    """{guide ID: [driver IDs]}: the guides attached by hand (their children follow them too)."""
    return {int(g): list(d) for g, d in _bind(groom).get("attached", {}).items()}


@undoable("remove drivers")
def remove_drivers(groom, curves=None):
    """Disconnect the driver curves (`curves`: those; None: every one). The guides attached to them
    stay attached (detach_guides) and the DriveGuides nodes stay (a driver no longer connected stays
    as bound, so its guides stay as groomed). Returns how many were disconnected."""
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
    """Bind the driver curves as they are at `frame` (the scalp then too): the attached guides keep
    their place round their drivers as they were there. The attachments are kept. Stored in the
    groom's driverBind (saved with the scene and in .groom files). Returns how many drivers were
    bound."""
    text = cmds.groomLabDrivers(groom, bind=True, frame=float(frame)) or ""
    bind = json.loads(text) if text else {}
    attached = _bind(groom).get("attached")
    if attached:
        bind["attached"] = attached
        bind.setdefault("frame", int(round(frame)))
    _store(groom, bind)
    return len(bind.get("drivers", []))


@undoable("clear driver bind")
def clear_bind(groom):
    """Forget the bind and the attachments: DriveGuides nodes pass the guides on."""
    cmds.setAttr(groom + ".driverBind", "", type="string")


def bind_frame(groom):
    """The frame the drivers were bound at, or None when they are not bound."""
    bind = _bind(groom)
    return bind.get("frame", 0) if bind.get("drivers") else None
