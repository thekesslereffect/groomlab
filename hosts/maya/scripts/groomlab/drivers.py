# SPDX-License-Identifier: Apache-2.0
"""Driver curves: curves an animator rigs that pose the guides attached to them (DriveGuides nodes).

Rig the curves with anything Maya has (joints and a skinCluster, spline IK, wire, blend shapes),
then attach guides to them: select the guides (the GroomLab select tool) and a driver curve, and
attach; or make the curves from the selected guides (create_drivers: one down the middle of them,
or one per guide on it exactly), attached and bound already. The attached guides (and their children) keep their place round their drivers as they
swing, bend and stretch from the bind pose (frame 0 by default: the pose the groom was made in);
every other guide stays as groomed. A Simulate after the DriveGuides node plays on top:
    attach_guides(groom, "hair_ctrl_crv1")                  # the selected guides follow it
    attach_guides(groom, "hair_ctrl_crv2", group="bangs")   # a whole guide group
    create_drivers(groom)                                   # a curve down the selected guides, attached
    create_drivers(groom, group="bangs", per_guide=True)    # a curve on each guide
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
    _attach(groom, {driver: ids}, frame)
    return ids


def _attach(groom, guides_by_driver, frame):
    """Connect the drivers ({driver curve: guide IDs}), record the attachments and bind once."""
    add_drivers(groom, list(guides_by_driver))
    index_of = dict((d["curve"], d["index"]) for d in driver_curves(groom))
    bind = _bind(groom)
    attached = bind.setdefault("attached", {})
    for driver, ids in guides_by_driver.items():
        index = index_of[_long(_shape_of(driver, "nurbsCurve"))]
        for g in ids:
            drivers = attached.setdefault(str(g), [])
            if index not in drivers:
                drivers.append(index)
    _store(groom, bind)
    bind_drivers(groom, frame=bind.get("frame", 0) if frame is None else frame)


def _resampled(points, count):
    """`points` (a polyline) as `count` points evenly spaced along its length."""
    if len(points) < 2 or count < 2:
        return [tuple(points[0])] * max(count, 1)
    lengths = [0.0]
    for a, b in zip(points, points[1:]):
        lengths.append(lengths[-1] + sum((b[i] - a[i]) ** 2 for i in range(3)) ** 0.5)
    total = lengths[-1] or 1.0
    out, seg = [], 0
    for k in range(count):
        s = total * k / (count - 1)
        while seg < len(points) - 2 and lengths[seg + 1] < s:
            seg += 1
        span = lengths[seg + 1] - lengths[seg]
        t = (s - lengths[seg]) / span if span > 0 else 0.0
        t = min(max(t, 0.0), 1.0)
        a, b = points[seg], points[seg + 1]
        out.append(tuple(a[i] + (b[i] - a[i]) * t for i in range(3)))
    return out


def _guide_points(groom, guide_id):
    """A guide's points as groomed (not posed), on the scalp as it is now, in world space."""
    xyz = cmds.groomLabGuides(groom, guidePoints=str(guide_id)) or []
    return [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]


def _curve_through(name, points, parent):
    """A curve through `points` (a cubic with them as its edit points; fewer than 4: a line), its
    pivot at the first (the root), so rotating it swings it from there."""
    pts = [tuple(p) for p in points]
    curve = cmds.curve(name=name, editPoint=pts, degree=3) if len(pts) >= 4 else \
        cmds.curve(name=name, point=pts, degree=1)
    curve = cmds.ls(cmds.parent(curve, parent)[0], long=True)[0]
    cmds.xform(curve, worldSpace=True, pivots=pts[0])
    return curve


@undoable("create driver curves")
def create_drivers(groom, guides=None, group=None, per_guide=False, points=None, frame=None, name=None):
    """Driver curves made from guides, the guides attached to them and bound: `guides` (IDs), else
    every guide of `group`, else the selected guides. One curve down the middle of them all (the
    mean of their shapes, root to tip), or with `per_guide` a curve per guide on it exactly (the
    guide then follows its curve as if bound to it directly). Rig and animate the curves.

    The curves are made from the guides as groomed at the bind frame (`frame`: the bind's, else 0;
    the timeline goes there and back while they are made), with `points` edit points (default: as
    many as the guides have) and the pivot at the root, under one group transform per groom ("<groom>_drivers"). Returns the
    curve transforms."""
    ids = _guide_targets(groom, guides, group)
    if not ids:
        raise ValueError("no guides to make drivers from: select some with the GroomLab select tool, or name them")
    if frame is None:
        frame = _bind(groom).get("frame", 0)
    now = cmds.currentTime(query=True)
    if now != frame:
        cmds.currentTime(frame, update=True)
    try:
        shapes = {g: _guide_points(groom, g) for g in ids}
    finally:
        if now != frame:
            cmds.currentTime(now, update=True)
    shapes = {g: p for g, p in shapes.items() if len(p) >= 2}
    if not shapes:
        raise ValueError("the guides %s have no points" % ids)

    transform = cmds.listRelatives(groom, parent=True)[0].split("|")[-1].split(":")[-1]
    holder = "%s_drivers" % transform
    if not cmds.objExists(holder):
        holder = cmds.group(empty=True, name=holder)
    base = name or "%s_driver" % transform
    count = points or max(len(p) for p in shapes.values())
    made = {}
    if per_guide:
        for g, p in sorted(shapes.items()):
            curve = _curve_through("%s_%d" % (base, g), p if points is None else _resampled(p, count), holder)
            made[curve] = [g]
    else:
        if len(shapes) == 1 and points is None:
            mean = list(shapes.values())[0]
        else:
            lines = [_resampled(p, count) for p in shapes.values()]
            mean = [tuple(sum(line[k][i] for line in lines) / len(lines) for i in range(3)) for k in range(count)]
        made[_curve_through(base + "1", mean, holder)] = sorted(shapes)
    _attach(groom, made, frame)
    return list(made)


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
