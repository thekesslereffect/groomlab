# SPDX-License-Identifier: Apache-2.0
"""Tubes to Curves' live link: guides that keep following the modeled tubes they were made from.

Converted with "Live link tubes to guides" (utils.tubes_to_curves(..., live_link=True)), each tube
leaves a link on the groom (its glTubeLinks attribute, saved with the scene): the mesh (by UUID),
which shell of it, how it was read, and its guides (shape guides a column each, center guides).
While a scene is open, every linked mesh is watched: a component edit, a change in its history,
or a move of it or of anything above it re-reads the tube and gives its guides their new points,
roots sliding over the scalp as a fresh conversion would put them. An edge run added or removed
(the tube's column count changing) makes its shape guides anew in the same groups, with the same
value in the tube region map. Conversions add links; they never replace the earlier ones.

The guides follow outside the undo queue: they are a function of the mesh, so undoing a mesh edit
makes them follow it back (and after any undo or redo every link is checked again).

    tube_links.links(groom)            # the groom's links (a list of dicts)
    tube_links.sync()                  # bring every linked guide up to date now
    tube_links.unlink(groom, mesh)     # stop a mesh's tubes driving their guides (all: mesh=None)
    tube_links.start() / stop()        # the scene watching (the menu's install / uninstall)
"""

import json

from maya import cmds

ATTR = "glTubeLinks"
VERSION = 1

# What dirties a mesh's shape when its points change: component tweaks, its history, its smoothing,
# and a move of it or anything above it (its world mesh: still caught after a reparent, when the
# world-matrix callback's DAG path may no longer be the mesh's).
_MESH_PLUGS = frozenset(("outMesh", "inMesh", "cachedInMesh", "worldMesh", "worldMatrix",
                         "pnts", "pntx", "pnty", "pntz",
                         "displaySmoothMesh", "smoothLevel"))

_mesh_jobs = {}      # mesh UUID -> [callback ids]
_scene_jobs = []     # scene open/new/import, undo/redo
_pending = set()     # mesh UUIDs changed since the last sync
_scheduled = [False]
_busy = [False]


# ---------------------------------------------------------------------------
# The links, on the groom


def _groom_shape(groom):
    from groomlab.base import GROOM_TYPE, _shape_of
    return _shape_of(groom, GROOM_TYPE)


def links(groom):
    """The groom's tube links (a list of dicts; [] when it has none)."""
    shape = _groom_shape(groom)
    if not cmds.attributeQuery(ATTR, node=shape, exists=True):
        return []
    try:
        data = json.loads(cmds.getAttr(shape + "." + ATTR) or "{}")
    except ValueError:
        return []
    return list(data.get("links", []))


def _store(groom, items):
    shape = _groom_shape(groom)
    if not cmds.attributeQuery(ATTR, node=shape, exists=True):
        cmds.addAttr(shape, longName=ATTR, dataType="string")
    cmds.setAttr(shape + "." + ATTR, json.dumps({"version": VERSION, "links": items}), type="string")


def add_links(groom, items):
    """`items` added to the groom's links (one undo step with the conversion making them), and
    their meshes watched."""
    if not items:
        return
    _store(groom, links(groom) + list(items))
    for item in items:
        _watch_mesh(item["mesh"])


def unlink(groom, mesh=None):
    """The links of `mesh` (a mesh shape or its transform; None: every link) dropped from the
    groom: its guides stay as they are. Returns how many links went."""
    current = links(groom)
    if mesh is None:
        keep = []
    else:
        uuids = set(cmds.ls(_mesh_shapes_of(mesh), uuid=True) or [])
        keep = [l for l in current if l["mesh"] not in uuids]
    if len(keep) != len(current):
        _store(groom, keep)
        _rewatch()
    return len(current) - len(keep)


def _mesh_shapes_of(node):
    if cmds.nodeType(node) == "mesh":
        return [node]
    return cmds.listRelatives(node, allDescendents=True, type="mesh", fullPath=True) or []


def _linked_grooms():
    """Every groom in the scene with links: [(groom shape, links)]."""
    from groomlab.base import GROOM_TYPE
    out = []
    for shape in cmds.ls(type=GROOM_TYPE, long=True) or []:
        found = links(shape)
        if found:
            out.append((shape, found))
    return out


# ---------------------------------------------------------------------------
# Watching the meshes


def _node(uuid):
    found = cmds.ls(uuid, long=True) or []
    return found[0] if found else None


def _watch_mesh(uuid):
    if uuid in _mesh_jobs:
        return
    shape = _node(uuid)
    if not shape:
        return
    try:
        from maya.api import OpenMaya as om
    except ImportError:
        return
    sel = om.MSelectionList()
    sel.add(shape)
    obj, path = sel.getDependNode(0), sel.getDagPath(0)

    def dirty(_node, plug, *_):
        try:
            name = om.MFnAttribute(plug.attribute()).name
        except RuntimeError:
            return
        if name in _MESH_PLUGS:
            _queue(uuid)

    def moved(*_):
        _queue(uuid)
    _mesh_jobs[uuid] = [om.MNodeMessage.addNodeDirtyPlugCallback(obj, dirty),
                        om.MDagMessage.addWorldMatrixModifiedCallback(path, moved)]


def _unwatch_mesh(uuid):
    from maya.api import OpenMaya as om
    for job in _mesh_jobs.pop(uuid, []):
        try:
            om.MMessage.removeCallback(job)
        except RuntimeError:
            pass


def _rewatch():
    """The watched meshes made exactly the linked ones in the scene now."""
    wanted = set(l["mesh"] for _, found in _linked_grooms() for l in found)
    for uuid in list(_mesh_jobs):
        if uuid not in wanted or not _node(uuid):
            _unwatch_mesh(uuid)
    for uuid in wanted:
        _watch_mesh(uuid)
    return wanted


def _queue(uuid):
    _pending.add(uuid)
    if _scheduled[0]:
        return
    _scheduled[0] = True
    try:  # between the mouse moves of a drag: Qt's loop runs them, the guides follow as it goes
        from groomlab.ui.qt import QtCore
        QtCore.QTimer.singleShot(0, _flush)
    except Exception:
        cmds.evalDeferred(_flush, lowestPriority=True)


def _flush():
    _scheduled[0] = False
    meshes = set(_pending)
    _pending.clear()
    if meshes:
        try:
            sync(meshes=meshes)
        except Exception as e:  # never into Maya's event loop
            cmds.warning("GroomLab: tube live link: %s" % e)


def _scene_changed(*_):
    for uuid in list(_mesh_jobs):
        _unwatch_mesh(uuid)
    _pending.clear()


def _scene_opened(*_):
    try:
        _rewatch()
    except Exception as e:
        cmds.warning("GroomLab: tube live link: %s" % e)


def _undone(*_):
    """After an undo or redo: links may have come or gone (a conversion undone), and guides may
    have gone back to points from before a mesh edit: every link checked."""
    try:
        for uuid in list(_mesh_jobs):  # watched afresh: an undone reparent leaves stale DAG paths
            _unwatch_mesh(uuid)
        for uuid in _rewatch():
            _queue(uuid)
    except Exception as e:
        cmds.warning("GroomLab: tube live link: %s" % e)


def start():
    """Watch the scene's linked meshes, now and after every scene opened or imported (the menu
    calls it as the plug-in loads)."""
    if not _scene_jobs:
        from maya.api import OpenMaya as om
        scene = om.MSceneMessage
        for when in (scene.kBeforeNew, scene.kBeforeOpen):
            _scene_jobs.append(scene.addCallback(when, _scene_changed))
        for when in (scene.kAfterOpen, scene.kAfterImport, scene.kAfterCreateReference):
            _scene_jobs.append(scene.addCallback(when, _scene_opened))
        for event in ("Undo", "Redo"):
            _scene_jobs.append(om.MEventMessage.addEventCallback(event, _undone))
    _rewatch()


def stop():
    """Every callback gone (the plug-in unloads, the module is replaced)."""
    from maya.api import OpenMaya as om
    for uuid in list(_mesh_jobs):
        _unwatch_mesh(uuid)
    for job in _scene_jobs:
        try:
            om.MMessage.removeCallback(job)
        except RuntimeError:
            pass
    del _scene_jobs[:]
    _pending.clear()


_close_at_exit = stop  # (the hot-swap helper's shutdown hook)


# ---------------------------------------------------------------------------
# Bringing the guides up to date


def link_record(tube, mesh_uuid, shape_ids, center_ids, center_count, groups, ignore_bevels, smooth,
                region_map=None, region_value=None):
    """A tube's link as stored: `tube` as _read_tubes made it (its source and orientation)."""
    return {"mesh": mesh_uuid, "shell": tube.shell, "flipped": bool(tube.flipped),
            "signature": list(tube.signature), "ignoreBevels": bool(ignore_bevels),
            "smooth": smooth, "shape": [str(i) for i in shape_ids], "centers": [str(i) for i in center_ids],
            "columnCount": len(shape_ids), "centerCount": int(center_count), "groups": dict(groups),
            "regionMap": region_map, "regionValue": region_value}


def _read_linked(shape, items):
    """Each link's tube, as its conversion read it: {id(link): Tube}. A shell gone (or no longer a
    tube) is left out."""
    from groomlab import utils, tube_mesh
    by_smooth = {}
    out = {}
    for item in items:
        key = item.get("smooth")
        if key not in by_smooth:
            try:
                by_smooth[key] = utils.read_mesh_tubes(shape, key)
            except tube_mesh.TubeError:
                by_smooth[key] = None
        found = by_smooth[key]
        if not found or item["shell"] >= len(found):
            continue
        # A fresh copy: each link merges its own bevels.
        source = found[item["shell"]]
        tube = tube_mesh.Tube(source.rings, name=source.name, pointed=source.pointed,
                              pointed_root=source.pointed_root, smooth_rings=source.smooth_rings)
        out[id(item)] = tube
    return out


def _orient_linked(tube, item, distance):
    """Root first as at conversion: the same end while the tube's loops and runs are the ones it
    had; after a topology change, the end nearer the scalp (as converting would) and the link
    remembers it."""
    from groomlab import tube_mesh
    signature = [len(tube.rings), tube.sides]
    if signature == list(item.get("signature", [])):
        if item.get("flipped"):
            tube.reverse()
    else:
        before = tube_mesh.section_center(tube.rings[0])
        tube_mesh.orient(tube, distance)
        item["signature"] = signature
        item["flipped"] = tube_mesh.section_center(tube.rings[0]) != before
        item["_changed"] = True
    if item.get("ignoreBevels", True):
        tube.merge_bevels()
    return tube.use_smooth()


def _existing_ids(groom):
    from groomlab import utils
    return set(utils.all_guide_ids(groom))


def _points(points):
    return [[round(float(c), 7) for c in p] for p in points]


def sync(groom=None, meshes=None):
    """Bring linked guides up to date with their tubes now: `groom`'s (None: every groom's),
    only the links of `meshes` (mesh UUIDs; None: all). Done outside the undo queue. Returns
    {"reshaped": [guide IDs given new points], "remade": [new guide IDs], "removed": [IDs]}."""
    report = {"reshaped": [], "remade": [], "removed": []}
    if _busy[0]:
        return report
    from groomlab.base import GROOM_TYPE
    grooms = [(_groom_shape(groom), links(groom))] if groom else _linked_grooms()
    state = cmds.undoInfo(query=True, state=True)
    _busy[0] = True
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        for shape, items in grooms:
            if not items or not cmds.objExists(shape) or cmds.nodeType(shape) != GROOM_TYPE:
                continue
            _sync_groom(shape, items, meshes, report)
    finally:
        cmds.undoInfo(stateWithoutFlush=state)
        _busy[0] = False
    return report


def _sync_groom(groom, items, meshes, report):
    from groomlab import utils
    wanted = [l for l in items if meshes is None or l["mesh"] in meshes]
    if not wanted:
        return
    existing = _existing_ids(groom)
    distance = utils._scalp_distance(groom)
    closest = utils._scalp_closest(groom)
    by_mesh = {}
    for item in wanted:
        by_mesh.setdefault(item["mesh"], []).append(item)
    points = {}
    changed = False
    for uuid, group in by_mesh.items():
        shape = _node(uuid)
        if not shape:
            continue  # deleted (an undo may bring it back): its guides stay as they are
        tubes = _read_linked(shape, group)
        for item in group:
            tube = tubes.get(id(item))
            if tube is None:
                continue
            tube = _orient_linked(tube, item, distance)
            # Guides deleted since (a brush's erase) stay deleted.
            item["shape"] = [i for i in item["shape"] if i in existing]
            item["centers"] = [i for i in item["centers"] if i in existing]
            if item.pop("_changed", False):
                changed = True
            raw = tube.columns() if item.get("columnCount") else []
            volumes = tube.volume_curves(item.get("centerCount", 1)) if item["centers"] else []
            if closest:  # this tube's roots asked for in one go
                closest.prime([c[0] for c in raw + volumes if c])
            columns = [utils._rooted(c, closest) for c in raw]
            if item.get("columnCount") and len(columns) != item["columnCount"]:
                _remake_shape_guides(groom, item, columns, report)
                changed = True
            elif columns:
                for guide, pts in zip(item["shape"], columns):
                    points[guide] = _points(pts)
            if item["centers"]:
                for guide, pts in zip(item["centers"], volumes):
                    points[guide] = _points(utils._rooted(pts, closest))
    if points:
        report["reshaped"] += list(cmds.groomLabGuides(groom, setPoints=json.dumps(points)) or [])
    kept = [l for l in items if l["shape"] or l["centers"] or not (l in wanted)]
    if changed or len(kept) != len(items):
        _store(groom, kept)


def _remake_shape_guides(groom, item, columns, report):
    """The tube's column count changed (an edge run added or removed): its shape guides made anew."""
    from groomlab import utils
    import groomlab
    old = list(item["shape"])
    value = item.get("regionValue")
    if old:
        report["removed"] += old
        cmds.groomLabGuides(groom, removeGuides=True, guide=old)
    item["shape"], item["columnCount"] = [], len(columns)
    group = (item.get("groups") or {}).get("shape")
    if not columns or not group:
        return
    holder = cmds.group(empty=True, name="glTubeLinkCurves")
    try:
        curves = [utils._curve("glTubeLinkCurve%d" % (i + 1), c, holder) for i, c in enumerate(columns)]
        ids = [str(i) for i in utils._guides_point_per_loop(groom, curves, group)]
    finally:
        if cmds.objExists(holder):
            cmds.delete(holder)
    item["shape"] = ids
    report["remade"] += ids
    region = item.get("regionMap")
    if region and value is not None and ids and region in groomlab.list_maps(groom):
        groomlab.set_guide_attribute(groom, region, value, guides=ids)
